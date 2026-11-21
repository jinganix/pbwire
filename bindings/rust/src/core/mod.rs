//! The pure-Rust wire core: a `FileDescriptorSet` reader, a value tree, wire
//! primitives and a binary encoder/decoder, with no C library, no `unsafe` and
//! no generated code.
//!
//! It is **not** a replacement for the crate's default codec, which links the C
//! core statically (see the crate root). It is the `core` arm of the pbench
//! three-arm contract and the mode a build with no C toolchain uses:
//!
//! ```no_run
//! # #[cfg(feature = "core")] {
//! let schema = pbwire::core::Schema::from_file("tests/data/matrix3.desc")?;
//! let ty = schema.message("structtest.Scalars3")?;
//! let mut msg = ty.new_message();
//! msg.set_str("str", "hello")?;
//! let wire = ty.encode(&msg)?;
//! let back = ty.decode(&wire)?;
//! assert_eq!(back.get_str("str")?.as_deref(), Some("hello"));
//! # }
//! # Ok::<(), pbwire::core::CoreError>(())
//! ```
//!
//! The decode side keeps unknown fields (in input order, tag and payload
//! verbatim), validates groups to their matching end tag, accepts packed runs
//! for every numeric kind, collapses oneofs to the last member, follows each
//! field's own presence rule and enforces the nesting cap.
//!
//! This mirrors `bindings/python/pure/`, `bindings/node/lib/core/`,
//! `bindings/dart/lib/core/` and `bindings/julia/runtime/src/core/`: one
//! host-language port of the C core per binding, held to the same official
//! binary conformance suite.

#![forbid(unsafe_code)]

mod decode;
mod descriptor;
mod encode;
mod tree;
mod wire;

use std::path::Path;
use std::rc::Rc;

pub use decode::{decode, decode_slice};
pub use descriptor::{
    field_by_name, field_count, load_schema, CoreSchema, EnumDef, Field, Kind, Limits, MsgDef,
    Presence,
};
pub use encode::{encode, encode_to};
pub use tree::{Message, Value};
pub use wire::{
    CoreError, Reader, Result, Tag, MAX_DEPTH, WT_END_GROUP, WT_FIXED32, WT_FIXED64, WT_LEN,
    WT_START_GROUP, WT_VARINT,
};

/// A loaded `FileDescriptorSet`: the runtime schema every type comes from.
#[derive(Clone)]
pub struct Schema {
    core: Rc<CoreSchema>,
    path: Option<String>,
}

impl Schema {
    /// Loads a descriptor set written by `protoc --descriptor_set_out`.
    pub fn from_file(path: impl AsRef<Path>) -> Result<Schema> {
        let path = path.as_ref();
        let bytes = std::fs::read(path)
            .map_err(|e| CoreError::new(format!("{}: {e}", path.display())))?;
        let mut schema = Self::from_bytes(&bytes, &Limits::default())?;
        schema.path = Some(path.display().to_string());
        Ok(schema)
    }

    /// Loads descriptor-set bytes; use `--include_imports` so every type
    /// resolves.
    pub fn from_bytes(bytes: &[u8], limits: &Limits) -> Result<Schema> {
        let core = load_schema(bytes, *limits)?;
        Ok(Schema { core: Rc::new(core), path: None })
    }

    /// Looks up a message by its fully qualified name.
    pub fn message(&self, full_name: &str) -> Result<MessageType> {
        let def = self.core.message_index(full_name)?;
        Ok(MessageType { core: Rc::clone(&self.core), def })
    }

    /// True when the set declares that message.
    pub fn has_message(&self, full_name: &str) -> bool {
        self.core.message_by_name.contains_key(full_name)
    }

    /// The compiled schema, for the tests and the pbench arm.
    pub fn core(&self) -> &Rc<CoreSchema> {
        &self.core
    }

    /// The file the set came from, when it came from a file.
    pub fn path(&self) -> Option<&str> {
        self.path.as_deref()
    }
}

impl std::fmt::Debug for Schema {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "<Schema {} messages>", self.core.messages.len())
    }
}

/// One message type: `new_message()` plus binary decode/encode.
#[derive(Clone)]
pub struct MessageType {
    core: Rc<CoreSchema>,
    def: usize,
}

impl MessageType {
    /// A handle for a message already resolved in `schema` (what the
    /// conformance testee needs after a name lookup on a borrowed schema).
    pub fn from_parts(schema: Rc<CoreSchema>, def: usize) -> MessageType {
        MessageType { core: schema, def }
    }

    /// Fully qualified name, e.g. `structtest.Scalars3`.
    pub fn name(&self) -> &str {
        &self.core.messages[self.def].full_name
    }

    pub fn def(&self) -> &MsgDef {
        &self.core.messages[self.def]
    }

    /// Index of this type in the compiled schema (the `decode`/`decode_slice`
    /// entry points take it).
    pub fn def_index(&self) -> usize {
        self.def
    }

    /// The compiled schema this type came from (the tests and the pbench arm
    /// resolve a field's message/enum through it).
    pub fn schema(&self) -> &Rc<CoreSchema> {
        &self.core
    }

    pub fn field(&self, name: &str) -> Result<&Field> {
        match self.def().by_name.get(name) {
            Some(slot) => Ok(&self.def().fields[*slot]),
            None => Err(CoreError::new(format!("{} has no field {name}", self.name()))),
        }
    }

    pub fn fields(&self) -> &[Field] {
        &self.def().fields
    }

    /// An empty, mutable message of this type.
    pub fn new_message(&self) -> Message {
        Message::new(Rc::clone(&self.core), self.def)
    }

    /// Decodes wire bytes; unknown fields are kept and re-encoded. The bytes
    /// are the authority on presence, so a field absent on the wire stays
    /// absent.
    pub fn decode(&self, data: &[u8]) -> Result<Message> {
        decode(&self.core, self.def, data, &Limits::default())
    }

    /// [`MessageType::decode`] with explicit limits.
    pub fn decode_with(&self, data: &[u8], limits: &Limits) -> Result<Message> {
        decode(&self.core, self.def, data, limits)
    }

    /// Decodes one body slice (offset + length) of a larger buffer.
    pub fn decode_slice(
        &self,
        data: &[u8],
        off: usize,
        length: usize,
        limits: &Limits,
    ) -> Result<Message> {
        decode_slice(&self.core, self.def, data, off, length, limits)
    }

    /// Encodes a tree; a tree that came from `decode()` re-emits its bytes.
    pub fn encode(&self, m: &Message) -> Result<Vec<u8>> {
        encode(m)
    }

    /// Encodes into a reused buffer: a hot loop allocates nothing.
    pub fn encode_to(&self, m: &Message, w: &mut Vec<u8>) -> Result<()> {
        encode_to(m, w)
    }
}

impl std::fmt::Debug for MessageType {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "<MessageType {}>", self.name())
    }
}

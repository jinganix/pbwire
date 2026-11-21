//! `FileDescriptorSet` reader and schema builder for the pure-Rust core: the
//! only reader of `descriptor.proto` the pure path needs. It parses just enough
//! for a binary codec (names, numbers, labels, types, type names, oneofs,
//! extensions, packedness, proto3 optional, editions features, syntax) and
//! skips everything else by wire type, so unknown `descriptor.proto` additions
//! never break loading.
//!
//! The wire layout knowledge lives in [`super::wire`]; `encode`/`decode` know
//! the message wire layout and `mod.rs` is the public facade. This mirrors
//! `bindings/python/pure/descriptor.py` (itself the twin of
//! `bindings/node/lib/core/descriptor.ts` and `bindings/java/runtime/Proto.java`).

use std::collections::HashMap;

use super::wire::{
    fail, fail_status, tag_bytes, CoreError, Reader, Result, Tag, MAX_DEPTH, WT_LEN, WT_VARINT,
};

// ---------------------------------------------------------------- wire kinds

/// Wire kinds of the table engine (its own numbering, not protobuf's): every
/// packable kind sorts before `Double`.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Kind {
    Int32 = 0,
    Uint32 = 1,
    Sint32 = 2,
    Int64 = 3,
    Uint64 = 4,
    Sint64 = 5,
    Bool = 6,
    Enum = 7,
    Fixed32 = 8,
    Sfixed32 = 9,
    Float = 10,
    Fixed64 = 11,
    Sfixed64 = 12,
    Double = 13,
    String = 14,
    Bytes = 15,
    Message = 16,
    Group = 17,
}

impl Kind {
    /// The packable numeric kinds: packed runs cross every one of them.
    pub fn is_packable(self) -> bool {
        (self as u8) <= (Kind::Double as u8)
    }

    pub fn is_ref(self) -> bool {
        matches!(self, Kind::String | Kind::Bytes | Kind::Message | Kind::Group)
    }
}

/// Presence kinds. `Implicit` is a proto3 plain scalar (no has-bit).
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Presence {
    Implicit = 0,
    Explicit = 1,
    Required = 2,
    MessagePresence = 3,
}

// ----------------------------------------------------------- compiled schema

#[derive(Debug, Default)]
pub struct EnumDef {
    pub full_name: String,
    pub by_name: HashMap<String, i32>,
    pub by_number: HashMap<i32, String>,
    pub closed: bool,
}

#[derive(Debug, Clone)]
pub struct Field {
    pub number: u32,
    pub name: String,
    pub kind: Kind,
    /// Wire type of the declared form: LEN for packed numerics.
    pub wt: u8,
    /// Wire type of a single element (what an unpacked occurrence uses).
    pub elem_wt: u8,
    pub repeated: bool,
    pub packed: bool,
    /// Oneof index, -1 when the field is not in a oneof.
    pub oneof: i32,
    pub presence: Presence,
    pub msg_type: Option<usize>,
    pub enum_type: Option<usize>,
    pub enum_closed: bool,
    /// A decoded string must be valid UTF-8 (proto3/editions VERIFY).
    pub utf8: bool,
    /// Extensions only: the global name `[pkg.name]`.
    pub full_name: Option<String>,
    /// Precomputed tag bytes (the start-group tag for a group).
    pub tag: Tag,
    /// Groups only: the end-group tag.
    pub tag_end: Tag,
}

#[derive(Debug, Default)]
pub struct MsgDef {
    pub full_name: String,
    pub fields: Vec<Field>,
    /// Dense number-to-slot map, `None` when field numbers are sparse.
    pub slot_by_fnum: Option<Vec<i32>>,
    pub by_name: HashMap<String, usize>,
    pub oneof_by_name: HashMap<String, usize>,
    pub oneof_count: usize,
    pub has_required: bool,
    /// Some transitively reachable message has a REQUIRED field.
    pub deep_required: bool,
    /// protoc's synthesized map entry (field 1 key, field 2 value).
    pub map_entry: bool,
    pub reserved_names: Vec<String>,
}

#[derive(Debug, Default)]
pub struct CoreSchema {
    pub messages: Vec<MsgDef>,
    pub message_by_name: HashMap<String, usize>,
    pub enums: Vec<EnumDef>,
    pub enum_by_name: HashMap<String, usize>,
}

impl CoreSchema {
    pub fn message_index(&self, full_name: &str) -> Result<usize> {
        match self.message_by_name.get(full_name) {
            Some(i) => Ok(*i),
            None => fail(format!("no such message type: {full_name}")),
        }
    }

    pub fn message(&self, full_name: &str) -> Result<&MsgDef> {
        Ok(&self.messages[self.message_index(full_name)?])
    }

    pub fn enum_(&self, full_name: &str) -> Option<&EnumDef> {
        self.enum_by_name.get(full_name).map(|i| &self.enums[*i])
    }

    /// Slot of a field number, -1 when the message has no such field.
    pub fn slot_of(&self, def: usize, fnum: u32) -> i32 {
        let d = &self.messages[def];
        match &d.slot_by_fnum {
            Some(slots) => {
                if (fnum as usize) < slots.len() {
                    slots[fnum as usize]
                } else {
                    -1
                }
            }
            None => d
                .fields
                .iter()
                .position(|f| f.number == fnum)
                .map_or(-1, |i| i as i32),
        }
    }
}

/// Bounds for untrusted input; zero fields mean the defaults.
#[derive(Clone, Copy, Debug, Default)]
pub struct Limits {
    pub max_bytes: usize,
    pub max_depth: u32,
}

impl Limits {
    pub fn new(max_bytes: usize, max_depth: u32) -> Limits {
        Limits { max_bytes, max_depth }
    }

    pub(crate) fn depth(&self) -> u32 {
        if self.max_depth == 0 || self.max_depth > MAX_DEPTH {
            MAX_DEPTH
        } else {
            self.max_depth
        }
    }

    pub(crate) fn check_bytes(&self, n: usize, what: &str) -> Result<()> {
        if self.max_bytes > 0 && n > self.max_bytes {
            return fail_status(format!("{what} exceeds the {}-byte limit", self.max_bytes), 5);
        }
        Ok(())
    }
}

// ------------------------------------------------- descriptor.proto model

const T_DOUBLE: i32 = 1;
const T_FLOAT: i32 = 2;
const T_INT64: i32 = 3;
const T_UINT64: i32 = 4;
const T_INT32: i32 = 5;
const T_FIXED64: i32 = 6;
const T_FIXED32: i32 = 7;
const T_BOOL: i32 = 8;
const T_STRING: i32 = 9;
const T_GROUP: i32 = 10;
const T_MESSAGE: i32 = 11;
const T_BYTES: i32 = 12;
const T_UINT32: i32 = 13;
const T_ENUM: i32 = 14;
const T_SFIXED32: i32 = 15;
const T_SFIXED64: i32 = 16;
const T_SINT32: i32 = 17;
const T_SINT64: i32 = 18;

const L_OPTIONAL: i32 = 1;
const L_REQUIRED: i32 = 2;
const L_REPEATED: i32 = 3;

#[derive(Default)]
struct POneof {
    name: String,
}

#[derive(Default)]
struct PField {
    name: Option<String>,
    extendee: Option<String>,
    number: i32,
    label: i32,
    type_: i32,
    type_name: Option<String>,
    oneof_index: i32,
    proto3_optional: bool,
    packed: bool,
    has_packed_option: bool,
    /// Editions `FeatureSet.enum_type`: 0 unset, 1 OPEN, 2 CLOSED.
    enum_open: i32,
    /// Editions `FeatureSet.utf8_validation`: 0 unset, 2 VERIFY, 3 NONE.
    utf8: i32,
    /// Linked: the parsed message/enum index.
    msg_type: Option<usize>,
    enum_type: Option<usize>,
    enum_closed: bool,
    /// Linked: the declared oneof index.
    oneof: Option<usize>,
    has_presence: bool,
    full_name: Option<String>,
}

#[derive(Default)]
struct PEnumValue {
    name: Option<String>,
    number: i32,
}

#[derive(Default)]
struct PEnum {
    name: Option<String>,
    full_name: String,
    idx: usize,
    values: Vec<PEnumValue>,
    enum_open: i32,
    closed: bool,
}

#[derive(Default)]
struct PMessage {
    name: Option<String>,
    full_name: String,
    idx: usize,
    map_entry: bool,
    fields: Vec<PField>,
    nested: Vec<PMessage>,
    extensions: Vec<PField>,
    oneofs: Vec<POneof>,
    enums: Vec<PEnum>,
    reserved_names: Vec<String>,
}

#[derive(Default)]
struct PFile {
    name: Option<String>,
    pkg: String,
    syntax: String,
    /// Field 14: an editions file (behaves as proto3 here).
    edition: bool,
    enum_open: i32,
    utf8: i32,
    messages: Vec<PMessage>,
    extensions: Vec<PField>,
    enums: Vec<PEnum>,
}

#[derive(Default)]
struct Linker {
    /// full name -> message index
    message_by_name: HashMap<String, usize>,
    /// full name -> enum index
    enum_by_name: HashMap<String, usize>,
    /// per enum index: closed?
    enum_closed: Vec<bool>,
    next_message: usize,
    next_enum: usize,
}

// -------------------------------------------------------------- wire parsing

fn str_of(b: &[u8], off: usize, n: usize) -> String {
    String::from_utf8_lossy(&b[off..off + n]).into_owned()
}

fn as_int32(v: u64) -> i32 {
    v as u32 as i32
}

fn parse_set(bytes: &[u8]) -> Result<Vec<PFile>> {
    let mut r = Reader::new(bytes);
    let mut files = Vec::new();
    while r.pos < r.end {
        let tag = r.r_var32()?;
        if tag >> 3 == 1 && (tag & 7) as u8 == WT_LEN {
            let n = r.r_len()?;
            files.push(parse_file(&r.b, r.pos, n)?);
            r.pos += n;
        } else {
            r.skip((tag & 7) as u8)?;
        }
    }
    Ok(files)
}

fn parse_file(b: &[u8], off: usize, len: usize) -> Result<PFile> {
    let mut r = Reader::slice(b, off, off + len)?;
    let mut f = PFile::default();
    while r.pos < r.end {
        let tag = r.r_var32()?;
        let num = tag >> 3;
        let wt = (tag & 7) as u8;
        if wt == WT_VARINT {
            // Field 14 is the edition marker; its value is irrelevant here.
            if num == 14 {
                r.r_var32()?;
                f.edition = true;
            } else {
                r.skip(wt)?;
            }
            continue;
        }
        if wt == WT_LEN {
            let n = r.r_len()?;
            match num {
                4 => f.messages.push(parse_message(b, r.pos, n, 1)?),
                5 => f.enums.push(parse_enum(b, r.pos, n)?),
                7 => f.extensions.push(parse_field(b, r.pos, n)?),
                8 => {
                    f.enum_open = parse_features(b, r.pos, r.pos + n, 50)?;
                    f.utf8 = parse_features_utf8(b, r.pos, r.pos + n, 50)?;
                }
                1 => f.name = Some(str_of(b, r.pos, n)),
                2 => f.pkg = str_of(b, r.pos, n),
                12 => f.syntax = str_of(b, r.pos, n),
                _ => {}
            }
            r.pos += n;
        } else {
            r.skip(wt)?;
        }
    }
    Ok(f)
}

fn parse_message(b: &[u8], off: usize, len: usize, depth: u32) -> Result<PMessage> {
    // Descriptor nesting is peer data too: a FileDescriptorSet a few tens of KB
    // can nest thousands of DescriptorProtos, so cap it instead of letting the
    // recursion overflow the stack.
    if depth > MAX_DEPTH {
        return fail(format!("descriptor nesting exceeds {MAX_DEPTH}"));
    }
    let mut r = Reader::slice(b, off, off + len)?;
    let mut m = PMessage::default();
    while r.pos < r.end {
        let tag = r.r_var32()?;
        let num = tag >> 3;
        if (tag & 7) as u8 != WT_LEN {
            r.skip((tag & 7) as u8)?;
            continue;
        }
        let n = r.r_len()?;
        match num {
            1 => m.name = Some(str_of(b, r.pos, n)),
            2 => m.fields.push(parse_field(b, r.pos, n)?),
            3 => m.nested.push(parse_message(b, r.pos, n, depth + 1)?),
            4 => m.enums.push(parse_enum(b, r.pos, n)?),
            6 => m.extensions.push(parse_field(b, r.pos, n)?),
            7 => m.map_entry = parse_map_entry(b, r.pos, n)?,
            8 => m.oneofs.push(parse_oneof(b, r.pos, n)?),
            10 => m.reserved_names.push(str_of(b, r.pos, n)),
            _ => {}
        }
        r.pos += n;
    }
    if m.name.is_none() {
        return fail("message without a name");
    }
    Ok(m)
}

fn parse_oneof(b: &[u8], off: usize, len: usize) -> Result<POneof> {
    let mut r = Reader::slice(b, off, off + len)?;
    let mut o = POneof::default();
    while r.pos < r.end {
        let tag = r.r_var32()?;
        if tag >> 3 == 1 && (tag & 7) as u8 == WT_LEN {
            let n = r.r_len()?;
            o.name = str_of(b, r.pos, n);
            r.pos += n;
        } else {
            r.skip((tag & 7) as u8)?;
        }
    }
    Ok(o)
}

fn parse_enum(b: &[u8], off: usize, len: usize) -> Result<PEnum> {
    let mut r = Reader::slice(b, off, off + len)?;
    let mut e = PEnum::default();
    while r.pos < r.end {
        let tag = r.r_var32()?;
        let num = tag >> 3;
        if (tag & 7) as u8 != WT_LEN {
            r.skip((tag & 7) as u8)?;
            continue;
        }
        let n = r.r_len()?;
        match num {
            1 => e.name = Some(str_of(b, r.pos, n)),
            2 => e.values.push(parse_enum_value(b, r.pos, n)?),
            3 => e.enum_open = parse_features(b, r.pos, r.pos + n, 7)?,
            _ => {}
        }
        r.pos += n;
    }
    Ok(e)
}

fn parse_enum_value(b: &[u8], off: usize, len: usize) -> Result<PEnumValue> {
    let mut r = Reader::slice(b, off, off + len)?;
    let mut v = PEnumValue::default();
    let mut has_number = false;
    while r.pos < r.end {
        let tag = r.r_var32()?;
        let num = tag >> 3;
        let wt = (tag & 7) as u8;
        if wt == WT_VARINT {
            if num == 2 {
                v.number = as_int32(r.r_var32()? as u64);
                has_number = true;
            } else {
                r.skip(wt)?;
            }
        } else if wt == WT_LEN {
            let n = r.r_len()?;
            if num == 1 {
                v.name = Some(str_of(b, r.pos, n));
            }
            r.pos += n;
        } else {
            r.skip(wt)?;
        }
    }
    if v.name.is_none() || !has_number {
        return fail("enum value without name/number");
    }
    Ok(v)
}

/// `MessageOptions.map_entry` is field 7.
fn parse_map_entry(b: &[u8], off: usize, len: usize) -> Result<bool> {
    let mut r = Reader::slice(b, off, off + len)?;
    let mut map_entry = false;
    while r.pos < r.end {
        let tag = r.r_var32()?;
        if (tag & 7) as u8 != WT_VARINT {
            r.skip((tag & 7) as u8)?;
            continue;
        }
        if tag >> 3 == 7 {
            map_entry = r.r_var32()? != 0;
        } else {
            r.skip(WT_VARINT)?;
        }
    }
    Ok(map_entry)
}

/// Reads one Options message and returns `FeatureSet.enum_type` (1 OPEN,
/// 2 CLOSED) when it carries one; 0 means unset.
fn parse_features(b: &[u8], off: usize, end: usize, option_number: u32) -> Result<i32> {
    let mut r = Reader::slice(b, off, end)?;
    let mut open = 0;
    while r.pos < r.end {
        let tag = r.r_var32()?;
        let num = tag >> 3;
        if (tag & 7) as u8 != WT_LEN {
            r.skip((tag & 7) as u8)?;
            continue;
        }
        let n = r.r_len()?;
        if num == option_number {
            open = parse_feature_set_enum_type(b, r.pos, r.pos + n)?;
        }
        r.pos += n;
    }
    Ok(open)
}

/// `FileOptions.features` (50) -> `FeatureSet.utf8_validation`.
fn parse_features_utf8(b: &[u8], off: usize, end: usize, option_number: u32) -> Result<i32> {
    let mut r = Reader::slice(b, off, end)?;
    let mut utf8 = 0;
    while r.pos < r.end {
        let tag = r.r_var32()?;
        let num = tag >> 3;
        if (tag & 7) as u8 != WT_LEN {
            r.skip((tag & 7) as u8)?;
            continue;
        }
        let n = r.r_len()?;
        if num == option_number {
            utf8 = parse_feature_set_utf8(b, r.pos, r.pos + n)?;
        }
        r.pos += n;
    }
    Ok(utf8)
}

/// `FeatureSet.enum_type` = 2: 1 OPEN or 2 CLOSED.
fn parse_feature_set_enum_type(b: &[u8], off: usize, end: usize) -> Result<i32> {
    let mut r = Reader::slice(b, off, end)?;
    let mut open = 0;
    while r.pos < r.end {
        let tag = r.r_var32()?;
        if tag >> 3 == 2 && (tag & 7) as u8 == WT_VARINT {
            let v = as_int32(r.r_var32()? as u64);
            if v == 1 || v == 2 {
                open = v;
            }
        } else {
            r.skip((tag & 7) as u8)?;
        }
    }
    Ok(open)
}

/// `FeatureSet.utf8_validation` = 4: 2 VERIFY or 3 NONE.
fn parse_feature_set_utf8(b: &[u8], off: usize, end: usize) -> Result<i32> {
    let mut r = Reader::slice(b, off, end)?;
    let mut utf8 = 0;
    while r.pos < r.end {
        let tag = r.r_var32()?;
        if tag >> 3 == 4 && (tag & 7) as u8 == WT_VARINT {
            let v = as_int32(r.r_var32()? as u64);
            if v == 2 || v == 3 {
                utf8 = v;
            }
        } else {
            r.skip((tag & 7) as u8)?;
        }
    }
    Ok(utf8)
}

fn parse_field(b: &[u8], off: usize, len: usize) -> Result<PField> {
    let mut r = Reader::slice(b, off, off + len)?;
    let mut f = PField { label: L_OPTIONAL, oneof_index: -1, ..PField::default() };
    while r.pos < r.end {
        let tag = r.r_var32()?;
        let num = tag >> 3;
        let wt = (tag & 7) as u8;
        if wt == WT_VARINT {
            let v = r.r_var32()? as u64;
            match num {
                3 => f.number = as_int32(v),
                4 => f.label = as_int32(v),
                5 => f.type_ = as_int32(v),
                9 => f.oneof_index = as_int32(v),
                17 => f.proto3_optional = v != 0,
                _ => {}
            }
        } else if wt == WT_LEN {
            let n = r.r_len()?;
            match num {
                1 => f.name = Some(str_of(b, r.pos, n)),
                2 => f.extendee = Some(str_of(b, r.pos, n)),
                6 => f.type_name = Some(str_of(b, r.pos, n)),
                8 => parse_field_options(b, r.pos, r.pos + n, &mut f)?,
                _ => {}
            }
            r.pos += n;
        } else {
            r.skip(wt)?;
        }
    }
    if f.name.is_none() || f.type_ == 0 || f.number == 0 {
        return fail("field without name/type/number");
    }
    if !(T_DOUBLE..=T_SINT64).contains(&f.type_) {
        return fail(format!("bad field type {}", f.type_));
    }
    if !(L_OPTIONAL..=L_REPEATED).contains(&f.label) {
        return fail(format!("bad label {}", f.label));
    }
    Ok(f)
}

/// `FieldOptions`: packed (2) and the editions FeatureSet (21).
fn parse_field_options(b: &[u8], off: usize, end: usize, f: &mut PField) -> Result<()> {
    let mut r = Reader::slice(b, off, end)?;
    while r.pos < r.end {
        let tag = r.r_var32()?;
        let num = tag >> 3;
        if num == 2 && (tag & 7) as u8 == WT_VARINT {
            f.packed = r.r_var32()? != 0;
            f.has_packed_option = true;
        } else if num == 21 && (tag & 7) as u8 == WT_LEN {
            let n = r.r_len()?;
            f.enum_open = parse_feature_set_enum_type(b, r.pos, r.pos + n)?;
            f.utf8 = parse_feature_set_utf8(b, r.pos, r.pos + n)?;
            r.pos += n;
        } else {
            r.skip((tag & 7) as u8)?;
        }
    }
    Ok(())
}

// ---------------------------------------------------------------- linking

impl Linker {
    fn index_message(&mut self, m: &mut PMessage, scope: &str) -> Result<()> {
        let name = m.name.clone().unwrap_or_default();
        m.full_name = if scope.is_empty() { name } else { format!("{scope}.{name}") };
        if self.message_by_name.contains_key(&m.full_name) {
            return fail(format!("duplicate message {}", m.full_name));
        }
        m.idx = self.next_message;
        self.next_message += 1;
        self.message_by_name.insert(m.full_name.clone(), m.idx);
        let full = m.full_name.clone();
        for e in &mut m.enums {
            self.index_enum(e, &full)?;
        }
        for n in &mut m.nested {
            self.index_message(n, &full)?;
        }
        Ok(())
    }

    fn index_enum(&mut self, e: &mut PEnum, scope: &str) -> Result<()> {
        let name = e.name.clone().unwrap_or_default();
        e.full_name = if scope.is_empty() { name } else { format!("{scope}.{name}") };
        if self.enum_by_name.contains_key(&e.full_name) {
            return fail(format!("duplicate enum {}", e.full_name));
        }
        e.idx = self.next_enum;
        self.next_enum += 1;
        self.enum_closed.push(false);
        self.enum_by_name.insert(e.full_name.clone(), e.idx);
        Ok(())
    }

    /// An enum is closed for proto2 files (and editions files asking for it).
    fn link_enum(&mut self, e: &mut PEnum, proto3: bool, file_enum_open: i32) -> Result<()> {
        if e.name.is_none() {
            return fail("enum without a name");
        }
        e.closed = if e.enum_open != 0 {
            e.enum_open == 2
        } else if file_enum_open != 0 {
            file_enum_open == 2
        } else {
            !proto3
        };
        self.enum_closed[e.idx] = e.closed;
        Ok(())
    }

    fn link_enums(&mut self, m: &mut PMessage, proto3: bool, file_enum_open: i32) -> Result<()> {
        if m.full_name.is_empty() {
            return fail("message without a name");
        }
        for e in &mut m.enums {
            self.link_enum(e, proto3, file_enum_open)?;
        }
        for n in &mut m.nested {
            self.link_enums(n, proto3, file_enum_open)?;
        }
        Ok(())
    }

    fn link_message(
        &mut self,
        m: &mut PMessage,
        scope: &str,
        proto3: bool,
        file_utf8: i32,
    ) -> Result<()> {
        let full = m.full_name.clone();
        let oneofs: Vec<String> = m.oneofs.iter().map(|o| o.name.clone()).collect();
        for f in &mut m.fields {
            self.link_field(f, scope, proto3, file_utf8, Some(&oneofs))?;
        }
        for n in &mut m.nested {
            self.link_message(n, &full, proto3, file_utf8)?;
        }
        for e in &mut m.extensions {
            self.link_field(e, scope, proto3, file_utf8, None)?;
            e.full_name = e.name.as_ref().map(|n| {
                if scope.is_empty() {
                    n.clone()
                } else {
                    format!("{scope}.{n}")
                }
            });
        }
        Ok(())
    }

    fn link_field(
        &mut self,
        f: &mut PField,
        scope: &str,
        proto3: bool,
        file_utf8: i32,
        parent_oneofs: Option<&[String]>,
    ) -> Result<()> {
        // Extensions are never in a oneof (the core ignores the index there).
        if f.oneof_index >= 0 {
            if let Some(oneofs) = parent_oneofs {
                if f.oneof_index as usize >= oneofs.len() {
                    return fail(format!(
                        "field {}: oneof_index out of range",
                        f.name.clone().unwrap_or_default()
                    ));
                }
                f.oneof = Some(f.oneof_index as usize);
            }
        }
        if (f.type_ == T_MESSAGE || f.type_ == T_GROUP) && f.type_name.is_some() {
            let name = f.type_name.clone().unwrap_or_default();
            let idx = resolve_message(&self.message_by_name, scope, &name);
            match idx {
                Some(i) => f.msg_type = Some(i),
                None => {
                    return fail(format!(
                        "field {}: no such type {name}",
                        f.name.clone().unwrap_or_default()
                    ))
                }
            }
        }
        if f.type_ == T_ENUM && f.type_name.is_some() {
            let name = f.type_name.clone().unwrap_or_default();
            let idx = resolve_enum(&self.enum_by_name, scope, &name);
            match idx {
                Some(i) => {
                    f.enum_type = Some(i);
                    // Editions may close one field in an open file; that wins
                    // over the enum type, like the core's per-field enum_open.
                    f.enum_closed = if f.enum_open != 0 {
                        f.enum_open == 2
                    } else {
                        self.enum_closed[i]
                    };
                }
                None => {
                    return fail(format!(
                        "field {}: no such enum {name}",
                        f.name.clone().unwrap_or_default()
                    ))
                }
            }
        }
        if f.label == L_REPEATED && is_packable_type(f.type_) && !f.has_packed_option {
            f.packed = proto3; // the syntax default
        }
        // utf8_validation: field FeatureSet, then the file's, then the dialect
        // default (VERIFY for proto3/editions, NONE for proto2). Only `string`
        // fields carry it, and only they validate on parse.
        if f.type_ == T_STRING {
            let utf8 = if f.utf8 != 0 {
                f.utf8
            } else if file_utf8 != 0 {
                file_utf8
            } else if proto3 {
                2
            } else {
                3
            };
            f.utf8 = if utf8 == 2 { 2 } else { 3 };
        } else {
            f.utf8 = 3;
        }
        f.has_presence = f.label != L_REPEATED
            && (f.type_ == T_MESSAGE
                || f.type_ == T_GROUP
                || f.label == L_REQUIRED
                || !proto3
                || f.proto3_optional);
        Ok(())
    }

    fn link_files(&mut self, files: &mut [PFile]) -> Result<()> {
        for f in files.iter_mut() {
            let pkg = f.pkg.clone();
            for m in &mut f.messages {
                self.index_message(m, &pkg)?;
            }
            for e in &mut f.enums {
                self.index_enum(e, &pkg)?;
            }
        }
        for f in files.iter_mut() {
            // Editions files carry syntax "editions" plus field 14: the core
            // treats them as proto3, so packed defaults and presence match.
            let proto3 = f.syntax == "proto3" || f.edition;
            let pkg = f.pkg.clone();
            let file_enum_open = f.enum_open;
            let file_utf8 = f.utf8;
            for e in &mut f.enums {
                self.link_enum(e, proto3, file_enum_open)?;
            }
            // Enums close/open before fields link: a field copies its enum's
            // closed flag, so the enum must be linked first.
            for m in &mut f.messages {
                self.link_enums(m, proto3, file_enum_open)?;
            }
            for m in &mut f.messages {
                self.link_message(m, &pkg, proto3, file_utf8)?;
            }
            for e in &mut f.extensions {
                self.link_field(e, &pkg, proto3, file_utf8, None)?;
                e.full_name = e.name.as_ref().map(|n| {
                    if pkg.is_empty() {
                        n.clone()
                    } else {
                        format!("{pkg}.{n}")
                    }
                });
            }
        }
        Ok(())
    }
}

fn is_packable_type(t: i32) -> bool {
    !(t == T_STRING || t == T_GROUP || t == T_MESSAGE || t == T_BYTES)
}

/// Leading-dot names resolve from the root; otherwise search the scope up.
fn resolve_message(by_name: &HashMap<String, usize>, scope: &str, name: &str) -> Option<usize> {
    if let Some(rest) = name.strip_prefix('.') {
        return by_name.get(rest).copied();
    }
    let mut s = scope.to_string();
    loop {
        let key = if s.is_empty() { name.to_string() } else { format!("{s}.{name}") };
        if let Some(i) = by_name.get(&key) {
            return Some(*i);
        }
        // Nested scopes qualify from the inside out: C++ scoping.
        if let Some(dot) = name.find('.') {
            let head = &name[..dot];
            let rest = &name[dot + 1..];
            let prefix = if s.is_empty() { head.to_string() } else { format!("{s}.{head}") };
            if by_name.contains_key(&prefix) {
                return resolve_message(by_name, &prefix, rest);
            }
        }
        if s.is_empty() {
            return None;
        }
        match s.rfind('.') {
            Some(cut) => s.truncate(cut),
            None => s.clear(),
        }
    }
}

/// The enum counterpart of [`resolve_message`]: same C++ scoping rules.
fn resolve_enum(by_name: &HashMap<String, usize>, scope: &str, name: &str) -> Option<usize> {
    if let Some(rest) = name.strip_prefix('.') {
        return by_name.get(rest).copied();
    }
    let mut s = scope.to_string();
    loop {
        let key = if s.is_empty() { name.to_string() } else { format!("{s}.{name}") };
        if let Some(i) = by_name.get(&key) {
            return Some(*i);
        }
        if let Some(dot) = name.find('.') {
            let head = &name[..dot];
            let rest = &name[dot + 1..];
            let prefix = if s.is_empty() { head.to_string() } else { format!("{s}.{head}") };
            if by_name.contains_key(&prefix) {
                return resolve_enum(by_name, &prefix, rest);
            }
        }
        if s.is_empty() {
            return None;
        }
        match s.rfind('.') {
            Some(cut) => s.truncate(cut),
            None => s.clear(),
        }
    }
}

// --------------------------------------------- compiled schema building

fn kind_of(t: i32) -> Kind {
    match t {
        T_INT32 => Kind::Int32,
        T_UINT32 => Kind::Uint32,
        T_SINT32 => Kind::Sint32,
        T_INT64 => Kind::Int64,
        T_UINT64 => Kind::Uint64,
        T_SINT64 => Kind::Sint64,
        T_BOOL => Kind::Bool,
        T_ENUM => Kind::Enum,
        T_FIXED32 => Kind::Fixed32,
        T_SFIXED32 => Kind::Sfixed32,
        T_FLOAT => Kind::Float,
        T_FIXED64 => Kind::Fixed64,
        T_SFIXED64 => Kind::Sfixed64,
        T_DOUBLE => Kind::Double,
        T_STRING => Kind::String,
        T_BYTES => Kind::Bytes,
        T_MESSAGE => Kind::Message,
        _ => Kind::Group,
    }
}

fn wire_type(t: i32, packed: bool) -> u8 {
    if packed {
        return WT_LEN;
    }
    match t {
        T_DOUBLE | T_FIXED64 | T_SFIXED64 => 1,
        T_STRING | T_BYTES | T_MESSAGE => WT_LEN,
        T_GROUP => 3,
        T_FIXED32 | T_SFIXED32 | T_FLOAT => 5,
        _ => WT_VARINT,
    }
}

fn build_enum(schema: &mut CoreSchema, e: &PEnum) -> Result<()> {
    let mut by_name = HashMap::new();
    let mut by_number = HashMap::new();
    for v in &e.values {
        let name = v.name.clone().unwrap_or_default();
        if by_name.contains_key(&name) {
            return fail(format!("duplicate enum value {}.{name}", e.full_name));
        }
        by_name.insert(name.clone(), v.number);
        // Aliases repeat a number: the first declared name is printed.
        by_number.entry(v.number).or_insert(name);
    }
    schema.enums[e.idx] = EnumDef {
        full_name: e.full_name.clone(),
        by_name,
        by_number,
        closed: e.closed,
    };
    Ok(())
}

fn build_enums(schema: &mut CoreSchema, m: &PMessage, depth: u32) -> Result<()> {
    if depth > MAX_DEPTH {
        return fail(format!("descriptor nesting exceeds {MAX_DEPTH}"));
    }
    for e in &m.enums {
        build_enum(schema, e)?;
    }
    for n in &m.nested {
        build_enums(schema, n, depth + 1)?;
    }
    Ok(())
}

fn build_field(f: &PField, oneofs: &mut HashMap<usize, usize>) -> Result<Field> {
    let kind = kind_of(f.type_);
    let repeated = f.label == L_REPEATED;
    let mut oneof: i32 = -1;
    if let Some(declared) = f.oneof {
        let next = oneofs.len();
        let slot = *oneofs.entry(declared).or_insert(next);
        oneof = slot as i32;
    }
    let presence = if oneof >= 0 {
        Presence::Explicit // reads/writes go through the oneof case
    } else if f.label == L_REQUIRED {
        Presence::Required
    } else if kind == Kind::Message || kind == Kind::Group {
        Presence::MessagePresence
    } else if f.has_presence {
        Presence::Explicit
    } else {
        Presence::Implicit
    };
    let packed = repeated && f.packed;
    let wt = wire_type(f.type_, packed);
    let number = f.number as u32;
    Ok(Field {
        number,
        name: f.name.clone().unwrap_or_default(),
        kind,
        wt,
        elem_wt: wire_type(f.type_, false),
        repeated,
        packed,
        oneof,
        presence,
        msg_type: f.msg_type,
        enum_type: f.enum_type,
        enum_closed: f.enum_closed,
        utf8: f.type_ == T_STRING && f.utf8 == 2,
        full_name: f.full_name.clone(),
        tag: tag_bytes(number, wt),
        tag_end: if kind == Kind::Group {
            tag_bytes(number, 4)
        } else {
            Tag::default()
        },
    })
}

fn finish_def(d: &mut MsgDef, mut fields: Vec<Field>, oneofs: &HashMap<usize, usize>,
              oneof_names: &[String]) {
    // Stable sort by field number: the slots decode into are field-number
    // ordered, like the C core's table.
    fields.sort_by_key(|f| f.number);
    let mut max_number = 0u32;
    let mut has_required = false;
    for f in &fields {
        if f.number > max_number {
            max_number = f.number;
        }
        if f.presence == Presence::Required {
            has_required = true;
        }
    }
    d.has_required = has_required;
    // Dense only when bounded, like the C core's fnum map: one field numbered
    // 536870911 must not allocate a 2 GiB table.
    d.slot_by_fnum = if max_number <= 512 && max_number as usize <= fields.len() * 8 + 8 {
        let mut slots = vec![-1i32; max_number as usize + 1];
        for (i, f) in fields.iter().enumerate() {
            slots[f.number as usize] = i as i32;
        }
        Some(slots)
    } else {
        None
    };
    d.by_name = fields.iter().enumerate().map(|(i, f)| (f.name.clone(), i)).collect();
    let mut oneof_by_name = HashMap::new();
    for (declared, slot) in oneofs {
        if let Some(name) = oneof_names.get(*declared) {
            oneof_by_name.insert(name.clone(), *slot);
        }
    }
    d.oneof_by_name = oneof_by_name;
    d.oneof_count = d.oneof_count.max(oneofs.len());
    d.fields = fields;
}

fn build_def(schema: &mut CoreSchema, m: &PMessage, depth: u32) -> Result<()> {
    if depth > MAX_DEPTH {
        return fail(format!("descriptor nesting exceeds {MAX_DEPTH}"));
    }
    let mut oneofs = HashMap::new();
    let mut fields = Vec::with_capacity(m.fields.len());
    for f in &m.fields {
        fields.push(build_field(f, &mut oneofs)?);
    }
    let names: Vec<String> = m.oneofs.iter().map(|o| o.name.clone()).collect();
    {
        let d = &mut schema.messages[m.idx];
        d.full_name = m.full_name.clone();
        d.reserved_names = m.reserved_names.clone();
        d.map_entry = m.map_entry;
        finish_def(d, fields, &oneofs, &names);
    }
    for n in &m.nested {
        build_def(schema, n, depth + 1)?;
    }
    Ok(())
}

fn attach_extension(schema: &mut CoreSchema, e: &PField) -> Result<()> {
    let mut extendee = e.extendee.clone().ok_or_else(|| CoreError::new("extension without extendee"))?;
    if let Some(rest) = extendee.strip_prefix('.') {
        extendee = rest.to_string();
    }
    let idx = match schema.message_by_name.get(&extendee) {
        Some(i) => *i,
        None => return fail(format!("extendee missing: {}", e.extendee.clone().unwrap_or_default())),
    };
    let mut s = build_field(e, &mut HashMap::new())?;
    s.full_name = e.full_name.clone();
    let mut fields = std::mem::take(&mut schema.messages[idx].fields);
    fields.push(s);
    let oneof_by_name = std::mem::take(&mut schema.messages[idx].oneof_by_name);
    let count = schema.messages[idx].oneof_count;
    finish_def(&mut schema.messages[idx], fields, &HashMap::new(), &[]);
    schema.messages[idx].oneof_by_name = oneof_by_name; // extensions are never in a oneof
    schema.messages[idx].oneof_count = count;
    Ok(())
}

fn attach_message_extensions(schema: &mut CoreSchema, m: &PMessage) -> Result<()> {
    for e in &m.extensions {
        attach_extension(schema, e)?;
    }
    for n in &m.nested {
        attach_message_extensions(schema, n)?;
    }
    Ok(())
}

fn finish_transitive(schema: &mut CoreSchema) {
    loop {
        let mut changed = false;
        for idx in 0..schema.messages.len() {
            if schema.messages[idx].deep_required {
                continue;
            }
            let needs = {
                let d = &schema.messages[idx];
                d.fields.iter().any(|f| {
                    f.presence == Presence::Required
                        || ((f.kind == Kind::Message || f.kind == Kind::Group)
                            && f.msg_type
                                .map_or(false, |i| schema.messages[i].deep_required))
                })
            };
            if needs {
                schema.messages[idx].deep_required = true;
                changed = true;
            }
        }
        if !changed {
            return;
        }
    }
}

/// Parses and links a `FileDescriptorSet` into a compiled schema.
pub fn load_schema(bytes: &[u8], limits: Limits) -> Result<CoreSchema> {
    limits.check_bytes(bytes.len(), "descriptor set")?;
    let mut files = parse_set(bytes).map_err(|e| CoreError::with_status(format!("descriptor set: {e}"), 1))?;
    let mut linker = Linker::default();
    linker
        .link_files(&mut files)
        .map_err(|e| CoreError::with_status(format!("descriptor set: {e}"), 1))?;

    let mut schema = CoreSchema::default();
    for _ in 0..linker.next_message {
        schema.messages.push(MsgDef::default());
    }
    for _ in 0..linker.next_enum {
        schema.enums.push(EnumDef::default());
    }
    // Name -> index before anything is built: extensions attach by extendee
    // name, so the map has to exist before the build walk.
    for (name, idx) in &linker.message_by_name {
        schema.message_by_name.insert(name.clone(), *idx);
    }
    for (name, idx) in &linker.enum_by_name {
        schema.enum_by_name.insert(name.clone(), *idx);
    }
    // Enums first: message fields link their element type while the defs are
    // built.
    for f in &files {
        for e in &f.enums {
            build_enum(&mut schema, e)?;
        }
        for m in &f.messages {
            build_enums(&mut schema, m, 1)?;
        }
    }
    for f in &files {
        for m in &f.messages {
            build_def(&mut schema, m, 1)?;
        }
    }
    // Extensions attach to their extendee after all defs exist.
    for f in &files {
        for e in &f.extensions {
            attach_extension(&mut schema, e)?;
        }
        for m in &f.messages {
            attach_message_extensions(&mut schema, m)?;
        }
    }
    finish_transitive(&mut schema);
    Ok(schema)
}

/// Number of declared fields in a message (descriptor test surface).
pub fn field_count(d: &MsgDef) -> usize {
    d.fields.len()
}

/// The field named `name`, or `None`.
pub fn field_by_name<'a>(d: &'a MsgDef, name: &str) -> Option<&'a Field> {
    d.by_name.get(name).map(|slot| &d.fields[*slot])
}

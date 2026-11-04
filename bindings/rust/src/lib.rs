//! Rust binding over the pbwire C ABI: a dynamic protobuf message tree with no
//! generated code. A `FileDescriptorSet` (`protoc --descriptor_set_out`) is
//! loaded at run time and drives field lookup, wire types and presence; the
//! wire format itself lives in the C core, which this crate links statically.
//!
//! ```no_run
//! let schema = pbwire::Schema::from_file("tests/data/matrix3.desc")?;
//! let ty = schema.message("structtest.Inner3")?;
//! let name = ty.field("name")?;
//!
//! let mut msg = ty.new_message();
//! msg.set_str(name, "kid");
//! let wire = msg.encode()?;
//! let decoded = ty.decode(&wire)?;
//! assert_eq!(decoded.get_str(name), Some("kid"));
//! # Ok::<(), pbwire::Error>(())
//! ```
//!
//! A `Field` is resolved once and then reused: `set_*`/`get_*` take the handle,
//! so a hot loop makes one native call per operation instead of a name lookup.
//!
//! Ownership: a `Schema` frees itself on drop, and so does a root `Message`
//! (from `new_message`/`decode`). A nested message is a view of its parent's
//! tree (`set_message`/`get_message`): it borrows the parent, so it cannot
//! outlive it, and it never frees anything on its own.
//!
//! Not thread safe, like the C core: one message belongs to one thread. A
//! schema is immutable once loaded, so it can be shared (`&Schema`).

mod ffi;

use std::ffi::{c_char, c_int, CStr, CString};
use std::fmt;
use std::fs;
use std::marker::PhantomData;
use std::path::Path;
use std::ptr::{self, NonNull};

pub use ffi::{
    PBW_LABEL_REPEATED, PBW_TYPE_GROUP, PBW_TYPE_MESSAGE, PBW_VALUE_BYTES, PBW_VALUE_MESSAGE,
    PBW_VALUE_UINT,
};

/// Anything the C core refused, plus I/O failures from `Schema::from_file`.
#[derive(Debug, Clone)]
pub struct Error {
    status: i32,
    message: String,
}

impl Error {
    /// The `pbw_status` value, or -1 for an I/O failure.
    pub fn status(&self) -> i32 {
        self.status
    }

    pub fn message(&self) -> &str {
        &self.message
    }
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.message)
    }
}

impl std::error::Error for Error {}

pub type Result<T> = std::result::Result<T, Error>;

fn cstr(ptr: *const c_char) -> String {
    if ptr.is_null() {
        String::new()
    } else {
        unsafe { CStr::from_ptr(ptr) }.to_string_lossy().into_owned()
    }
}

fn status_error(status: c_int) -> Error {
    Error {
        status,
        message: cstr(unsafe { ffi::pbw_strerror(status) }),
    }
}

/// A loaded `FileDescriptorSet`: the schema every [`Type`] comes from.
pub struct Schema {
    ptr: NonNull<ffi::pbw_schema>,
}

impl Schema {
    /// Loads descriptor-set bytes; use `--include_imports` so every type resolves.
    pub fn from_bytes(bytes: &[u8]) -> Result<Schema> {
        let mut out: *mut ffi::pbw_schema = ptr::null_mut();
        let status =
            unsafe { ffi::pbw_descriptor_set_load(bytes.as_ptr().cast(), bytes.len(), &mut out) };
        if status != ffi::PBW_OK {
            let error = if out.is_null() {
                status_error(status)
            } else {
                let message = cstr(unsafe { ffi::pbw_schema_error(out) });
                unsafe { ffi::pbw_schema_free(out) };
                Error {
                    status,
                    message: if message.is_empty() {
                        status_error(status).message
                    } else {
                        message
                    },
                }
            };
            return Err(error);
        }
        match NonNull::new(out) {
            Some(ptr) => Ok(Schema { ptr }),
            None => Err(status_error(-1)),
        }
    }

    /// Loads a descriptor set written by `protoc --descriptor_set_out`.
    pub fn from_file(path: impl AsRef<Path>) -> Result<Schema> {
        let path = path.as_ref();
        let bytes = fs::read(path).map_err(|e| Error {
            status: -1,
            message: format!("{}: {e}", path.display()),
        })?;
        Schema::from_bytes(&bytes)
    }

    /// Looks up a message by fully qualified name, e.g. `"structtest.Inner3"`.
    pub fn message<'s>(&'s self, full_name: &str) -> Result<Type<'s>> {
        let name = CString::new(full_name).map_err(|_| Error {
            status: -1,
            message: format!("message name contains a NUL byte: {full_name:?}"),
        })?;
        let desc = unsafe { ffi::pbw_schema_find(self.ptr.as_ptr(), name.as_ptr()) };
        match NonNull::new(desc) {
            Some(desc) => Ok(Type {
                desc,
                _schema: PhantomData,
            }),
            None => Err(Error {
                status: -1,
                message: format!("no such message type: {full_name}"),
            }),
        }
    }
}

impl Drop for Schema {
    fn drop(&mut self) {
        unsafe { ffi::pbw_schema_free(self.ptr.as_ptr()) }
    }
}

/// A message type from a [`Schema`].
pub struct Type<'s> {
    desc: NonNull<ffi::pbw_descriptor>,
    _schema: PhantomData<&'s Schema>,
}

impl<'s> Type<'s> {
    pub fn name(&self) -> &str {
        unsafe { CStr::from_ptr(ffi::pbw_descriptor_name(self.desc.as_ptr())) }
            .to_str()
            .unwrap_or("")
    }

    /// Resolves a field once; reuse the handle in hot loops.
    pub fn field(&self, name: &str) -> Result<Field> {
        let name = CString::new(name).map_err(|_| Error {
            status: -1,
            message: "field name contains a NUL byte".into(),
        })?;
        match NonNull::new(unsafe {
            ffi::pbw_descriptor_find_field_name(self.desc.as_ptr(), name.as_ptr())
        } as *mut ffi::pbw_field) {
            Some(ptr) => Ok(Field { ptr }),
            None => Err(Error {
                status: -1,
                message: format!("{}: no such field", name.to_string_lossy()),
            }),
        }
    }

    pub fn field_by_number(&self, number: u32) -> Result<Field> {
        match NonNull::new(unsafe { ffi::pbw_descriptor_find_field(self.desc.as_ptr(), number) }
            as *mut ffi::pbw_field)
        {
            Some(ptr) => Ok(Field { ptr }),
            None => Err(Error {
                status: -1,
                message: format!("no field {number}"),
            }),
        }
    }

    /// A new empty message, freed on drop.
    pub fn new_message(&self) -> Message<'_> {
        let ptr = unsafe { ffi::pbw_message_new(self.desc.as_ptr()) };
        Message {
            ptr: NonNull::new(ptr).expect("pbw_message_new: out of memory"),
            owned: true,
            _parent: PhantomData,
        }
    }

    /// Decodes wire bytes; unknown fields are preserved and re-encoded.
    pub fn decode(&self, wire: &[u8]) -> Result<Message<'_>> {
        let mut out: *mut ffi::pbw_message = ptr::null_mut();
        let status =
            unsafe { ffi::pbw_decode(self.desc.as_ptr(), wire.as_ptr().cast(), wire.len(), &mut out) };
        if status != ffi::PBW_OK {
            return Err(status_error(status));
        }
        Ok(Message {
            ptr: NonNull::new(out).ok_or_else(|| status_error(-1))?,
            owned: true,
            _parent: PhantomData,
        })
    }
}

/// A resolved field handle: the number, wire type and storage the schema says.
#[derive(Copy, Clone)]
pub struct Field {
    ptr: NonNull<ffi::pbw_field>,
}

impl Field {
    /* The C ABI hands out const field pointers; the handle is only ever read. */
    fn as_const(&self) -> *const ffi::pbw_field {
        self.ptr.as_ptr() as *const ffi::pbw_field
    }

    pub fn number(&self) -> u32 {
        unsafe { ffi::pbw_field_number(self.as_const()) }
    }

    pub fn name(&self) -> &str {
        unsafe { CStr::from_ptr(ffi::pbw_field_name(self.as_const())) }
            .to_str()
            .unwrap_or("")
    }

    /// `pbw_type`: `PBW_TYPE_INT32`, `PBW_TYPE_MESSAGE`, ...
    pub fn wire_type(&self) -> i32 {
        unsafe { ffi::pbw_field_type(self.as_const()) }
    }

    /// `pbw_label`: 3 for repeated.
    pub fn label(&self) -> i32 {
        unsafe { ffi::pbw_field_label(self.as_const()) }
    }
}

/// One dynamic message. A root owns its tree; a nested message borrows it.
pub struct Message<'a> {
    ptr: NonNull<ffi::pbw_message>,
    owned: bool,
    _parent: PhantomData<&'a ()>,
}

impl Message<'_> {
    /* ------------------------------------------------------------ setting */

    /// Any integer or enum field; the value is stored as raw 64-bit bits.
    pub fn set_i64(&mut self, field: Field, value: i64) -> &mut Self {
        self.store(field, unsafe { ffi::pbw_value_new_int(value) })
    }

    pub fn set_bool(&mut self, field: Field, value: bool) -> &mut Self {
        self.store(field, unsafe { ffi::pbw_value_new_bool(value as c_int) })
    }

    pub fn set_f32(&mut self, field: Field, value: f32) -> &mut Self {
        self.store(field, unsafe { ffi::pbw_value_new_float(value) })
    }

    pub fn set_f64(&mut self, field: Field, value: f64) -> &mut Self {
        self.store(field, unsafe { ffi::pbw_value_new_double(value) })
    }

    pub fn set_str(&mut self, field: Field, value: &str) -> &mut Self {
        let text = CString::new(value).expect("set_str: string contains a NUL byte (use set_bytes)");
        self.store(field, unsafe { ffi::pbw_value_new_string(text.as_ptr()) })
    }

    pub fn set_bytes(&mut self, field: Field, value: &[u8]) -> &mut Self {
        self.store(field, unsafe {
            ffi::pbw_value_new_bytes(value.as_ptr().cast(), value.len())
        })
    }

    /// Nested message for a MESSAGE field, created in place (or returned as it is).
    pub fn set_message(&mut self, field: Field) -> Message<'_> {
        let ptr = unsafe { ffi::pbw_message_set_message(self.ptr.as_ptr(), field.number()) };
        let ptr = NonNull::new(ptr)
            .unwrap_or_else(|| panic!("set {}: not a message field", field.name()));
        Message {
            ptr,
            owned: false,
            _parent: PhantomData,
        }
    }

    /// Appends to a repeated field or replaces a singular one.
    fn store(&mut self, field: Field, value: *mut ffi::pbw_value) -> &mut Self {
        if value.is_null() {
            panic!("pbw_value_new_*: out of memory");
        }
        let status =
            unsafe { ffi::pbw_message_set_field(self.ptr.as_ptr(), field.as_const(), value) };
        if status != ffi::PBW_OK {
            /* Rejected values stay owned by the caller. */
            unsafe { ffi::pbw_value_free(value) };
            panic!("set {}: {}", field.name(), status_error(status));
        }
        self
    }

    pub fn clear(&mut self, field: Field) {
        unsafe { ffi::pbw_message_clear(self.ptr.as_ptr(), field.number()) }
    }

    /* ------------------------------------------------------------ reading */

    pub fn value(&self, field: Field) -> Option<Value<'_>> {
        let ptr = unsafe { ffi::pbw_message_get_field(self.ptr.as_ptr(), field.as_const()) };
        NonNull::new(ptr).map(|ptr| Value {
            ptr,
            _parent: PhantomData,
        })
    }

    pub fn has(&self, field: Field) -> bool {
        self.value(field).is_some()
    }

    pub fn get_i64(&self, field: Field) -> i64 {
        self.value(field).map_or(0, |v| unsafe {
            ffi::pbw_value_get_int(v.ptr.as_ptr())
        })
    }

    pub fn get_bool(&self, field: Field) -> bool {
        self.value(field).is_some_and(|v| unsafe {
            ffi::pbw_value_get_bool(v.ptr.as_ptr()) != 0
        })
    }

    pub fn get_f32(&self, field: Field) -> f32 {
        self.value(field).map_or(0.0, |v| unsafe {
            ffi::pbw_value_get_float(v.ptr.as_ptr())
        })
    }

    pub fn get_f64(&self, field: Field) -> f64 {
        self.value(field).map_or(0.0, |v| unsafe {
            ffi::pbw_value_get_double(v.ptr.as_ptr())
        })
    }

    pub fn get_str(&self, field: Field) -> Option<&str> {
        let value = self.value(field)?;
        let ptr = unsafe { ffi::pbw_value_get_string(value.ptr.as_ptr()) };
        if ptr.is_null() {
            return None;
        }
        unsafe { CStr::from_ptr(ptr) }.to_str().ok()
    }

    pub fn get_bytes(&self, field: Field) -> Option<&[u8]> {
        let value = self.value(field)?;
        let mut len = 0usize;
        let ptr = unsafe { ffi::pbw_value_get_bytes(value.ptr.as_ptr(), &mut len) };
        if ptr.is_null() {
            return None;
        }
        Some(unsafe { std::slice::from_raw_parts(ptr.cast::<u8>(), len) })
    }

    pub fn get_message(&self, field: Field) -> Option<Message<'_>> {
        let value = self.value(field)?;
        let ptr = unsafe { ffi::pbw_value_get_message(value.ptr.as_ptr()) };
        NonNull::new(ptr).map(|ptr| Message {
            ptr,
            owned: false,
            _parent: PhantomData,
        })
    }

    /* -------------------------------------------------------------- wire */

    /// Size the decoder last read into this tree (0 for a tree built by hand):
    /// a hint for [`Message::encode_to`], not a size calculator.
    pub fn wire_size(&self) -> usize {
        unsafe { ffi::pbw_message_wire_size(self.ptr.as_ptr()) }
    }

    /// True when every proto2 `required` field is present.
    pub fn is_initialized(&self) -> bool {
        unsafe { ffi::pbw_message_is_initialized(self.ptr.as_ptr()) != 0 }
    }

    /// Encodes into a fresh `Vec` (one allocation by the core, one copy).
    pub fn encode(&self) -> Result<Vec<u8>> {
        let mut out: *mut u8 = ptr::null_mut();
        let mut len = 0usize;
        let status = unsafe { ffi::pbw_encode(self.ptr.as_ptr(), &mut out, &mut len) };
        if status != ffi::PBW_OK {
            return Err(status_error(status));
        }
        /* An empty message encodes to 0 bytes; slice::from_raw_parts wants a
         * non-null pointer even then, and the core's empty-case buffer need not
         * stay non-null forever. */
        if len == 0 {
            unsafe { ffi::pbw_free(out.cast()) };
            return Ok(Vec::new());
        }
        let bytes = unsafe { std::slice::from_raw_parts(out, len) }.to_vec();
        unsafe { ffi::pbw_free(out.cast()) };
        Ok(bytes)
    }

    /// Encodes into `buf`'s spare capacity: the buffer is grown once and reused,
    /// so a hot loop allocates nothing. Doubling on `PBW_ERR_LIMIT` keeps the
    /// first call simple (the core has no "size this message" call).
    pub fn encode_to<'b>(&self, buf: &'b mut Vec<u8>) -> Result<&'b [u8]> {
        let mut want = self.wire_size() + 32;
        loop {
            if buf.capacity() < want {
                buf.reserve(want - buf.len());
            }
            let mut len = 0usize;
            let status = unsafe {
                ffi::pbw_encode_to(
                    self.ptr.as_ptr(),
                    buf.as_mut_ptr().cast(),
                    buf.capacity(),
                    &mut len,
                )
            };
            match status {
                ffi::PBW_OK => {
                    unsafe { buf.set_len(len) };
                    return Ok(buf.as_slice());
                }
                ffi::PBW_ERR_LIMIT => want = buf.capacity().saturating_mul(2) + 16,
                _ => return Err(status_error(status)),
            }
        }
    }
}

impl Drop for Message<'_> {
    fn drop(&mut self) {
        if self.owned {
            unsafe { ffi::pbw_message_free(self.ptr.as_ptr()) }
        }
    }
}

/// A field value: what `Message::value` hands back, typed by `kind`.
pub struct Value<'a> {
    ptr: NonNull<ffi::pbw_value>,
    _parent: PhantomData<&'a ()>,
}

impl<'a> Value<'a> {
    /// `pbw_value_kind`: 0 numeric bits, 1 bytes/string, 2 message, 3 list.
    pub fn kind(&self) -> i32 {
        unsafe { ffi::pbw_value_get_kind(self.ptr.as_ptr()) }
    }

    pub fn as_i64(&self) -> i64 {
        unsafe { ffi::pbw_value_get_int(self.ptr.as_ptr()) }
    }

    pub fn as_bool(&self) -> bool {
        unsafe { ffi::pbw_value_get_bool(self.ptr.as_ptr()) != 0 }
    }

    pub fn as_f32(&self) -> f32 {
        unsafe { ffi::pbw_value_get_float(self.ptr.as_ptr()) }
    }

    pub fn as_f64(&self) -> f64 {
        unsafe { ffi::pbw_value_get_double(self.ptr.as_ptr()) }
    }

    pub fn as_str(&self) -> Option<&'a str> {
        let ptr = unsafe { ffi::pbw_value_get_string(self.ptr.as_ptr()) };
        if ptr.is_null() {
            return None;
        }
        unsafe { CStr::from_ptr(ptr) }.to_str().ok()
    }

    pub fn as_bytes(&self) -> Option<&'a [u8]> {
        let mut len = 0usize;
        let ptr = unsafe { ffi::pbw_value_get_bytes(self.ptr.as_ptr(), &mut len) };
        if ptr.is_null() {
            return None;
        }
        Some(unsafe { std::slice::from_raw_parts(ptr.cast::<u8>(), len) })
    }

    pub fn as_message(&self) -> Option<Message<'a>> {
        let ptr = unsafe { ffi::pbw_value_get_message(self.ptr.as_ptr()) };
        NonNull::new(ptr).map(|ptr| Message {
            ptr,
            owned: false,
            _parent: PhantomData,
        })
    }

    /// Items in a repeated field (1 for a singular value).
    pub fn len(&self) -> usize {
        unsafe { ffi::pbw_value_count(self.ptr.as_ptr()) }
    }

    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }

    pub fn at(&self, index: usize) -> Option<Value<'a>> {
        let ptr = unsafe { ffi::pbw_value_at(self.ptr.as_ptr(), index) };
        NonNull::new(ptr).map(|ptr| Value {
            ptr,
            _parent: PhantomData,
        })
    }
}

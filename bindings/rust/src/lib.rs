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

use std::ffi::{c_char, c_int, c_void, CStr, CString};
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

/// What a generated message's `decode` returns on bad wire bytes: the
/// prost `DecodeError` shape (`failed to decode Protobuf message: ...`)
/// over this crate's dynamic errors, so business code treats both alike.
#[derive(Debug, Clone)]
pub struct DecodeError {
    message: String,
}

impl DecodeError {
    pub fn new(message: impl Into<String>) -> Self {
        DecodeError {
            message: message.into(),
        }
    }
}

impl From<Error> for DecodeError {
    fn from(err: Error) -> Self {
        DecodeError::new(err.message)
    }
}

impl fmt::Display for DecodeError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "failed to decode Protobuf message: {}", self.message)
    }
}

impl std::error::Error for DecodeError {}

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

impl Schema {
    /// Services declared in the loaded files, for routing. The core does no
    /// RPC; a transport resolves method names to input/output types.
    pub fn service_count(&self) -> usize {
        unsafe { ffi::pbw_schema_service_count(self.ptr.as_ptr()) }
    }

    /// One service by index.
    pub fn service_at(&self, index: usize) -> Option<Service<'_>> {
        NonNull::new(unsafe { ffi::pbw_schema_service_at(self.ptr.as_ptr(), index) }
            as *mut ffi::pbw_service)
        .map(|ptr| Service {
            ptr,
            _schema: PhantomData,
        })
    }

    /// One service by fully qualified name (`"pkg.Service"`).
    pub fn service(&self, name: &str) -> Result<Service<'_>> {
        let name = CString::new(name).map_err(|_| Error {
            status: -1,
            message: "service name contains a NUL byte".into(),
        })?;
        match NonNull::new(unsafe {
            ffi::pbw_schema_service_find(self.ptr.as_ptr(), name.as_ptr())
                as *mut ffi::pbw_service
        }) {
            Some(ptr) => Ok(Service {
                ptr,
                _schema: PhantomData,
            }),
            None => Err(Error {
                status: -1,
                message: format!("no such service: {}", name.to_string_lossy()),
            }),
        }
    }

    /// Resolves `service`/`method` to its input/output message types: decode
    /// with the first, encode with the second.
    pub fn resolve_method(&self, service: &str, method: &str) -> Result<(Type<'_>, Type<'_>)> {
        let svc = CString::new(service).map_err(|_| Error {
            status: -1,
            message: "service name contains a NUL byte".into(),
        })?;
        let meth = CString::new(method).map_err(|_| Error {
            status: -1,
            message: "method name contains a NUL byte".into(),
        })?;
        let mut in_desc: *const ffi::pbw_descriptor = ptr::null();
        let mut out_desc: *const ffi::pbw_descriptor = ptr::null();
        let status = unsafe {
            ffi::pbw_service_resolve(
                self.ptr.as_ptr(),
                svc.as_ptr(),
                meth.as_ptr(),
                &mut in_desc,
                &mut out_desc,
            )
        };
        if status != ffi::PBW_OK {
            return Err(status_error(status));
        }
        Ok((
            Type {
                desc: NonNull::new(in_desc as *mut ffi::pbw_descriptor)
                    .ok_or_else(|| status_error(-1))?,
                _schema: PhantomData,
            },
            Type {
                desc: NonNull::new(out_desc as *mut ffi::pbw_descriptor)
                    .ok_or_else(|| status_error(-1))?,
                _schema: PhantomData,
            },
        ))
    }
}

/// One scanned field: raw wire content, no allocation, no value tree.
/// `field` is `None` when the number is unknown to the schema.
pub struct VisitField<'a> {
    pub field: Option<Field<'a>>,
    pub number: u32,
    pub wire_type: u32,
    /// VARINT/32BIT/64BIT raw wire bits (no zigzag or sign extension).
    /// Unused for the length-delimited shapes below.
    pub raw: u64,
    /// LEN payload bytes (nested message, packed run, string), or the whole
    /// raw field for groups. `None` for the scalar shapes above.
    pub data: Option<&'a [u8]>,
}

/// A service from a [`Schema`].
pub struct Service<'s> {
    ptr: NonNull<ffi::pbw_service>,
    _schema: PhantomData<&'s Schema>,
}

impl Service<'_> {
    pub fn name(&self) -> &str {
        unsafe { CStr::from_ptr(ffi::pbw_service_name(self.ptr.as_ptr())) }
            .to_str()
            .unwrap_or("")
    }

    pub fn method_count(&self) -> usize {
        unsafe { ffi::pbw_service_method_count(self.ptr.as_ptr()) }
    }

    pub fn method_at(&self, index: usize) -> Option<Method<'_>> {
        NonNull::new(unsafe { ffi::pbw_service_method_at(self.ptr.as_ptr(), index) }
            as *mut ffi::pbw_method)
        .map(|ptr| Method {
            ptr,
            _schema: PhantomData,
        })
    }

    pub fn method(&self, name: &str) -> Result<Method<'_>> {
        let name = CString::new(name).map_err(|_| Error {
            status: -1,
            message: "method name contains a NUL byte".into(),
        })?;
        match NonNull::new(unsafe {
            ffi::pbw_service_method_find(self.ptr.as_ptr(), name.as_ptr())
                as *mut ffi::pbw_method
        }) {
            Some(ptr) => Ok(Method {
                ptr,
                _schema: PhantomData,
            }),
            None => Err(Error {
                status: -1,
                message: format!("no such method: {}", name.to_string_lossy()),
            }),
        }
    }
}

/// One service method: fully qualified input/output type names for routing.
pub struct Method<'s> {
    ptr: NonNull<ffi::pbw_method>,
    _schema: PhantomData<&'s Schema>,
}

impl Method<'_> {
    pub fn name(&self) -> &str {
        unsafe { CStr::from_ptr(ffi::pbw_method_name(self.ptr.as_ptr())) }
            .to_str()
            .unwrap_or("")
    }

    /// Fully qualified input message name, without the leading dot.
    pub fn input_type(&self) -> &str {
        unsafe { CStr::from_ptr(ffi::pbw_method_input_type(self.ptr.as_ptr())) }
            .to_str()
            .unwrap_or("")
    }

    /// Fully qualified output message name, without the leading dot.
    pub fn output_type(&self) -> &str {
        unsafe { CStr::from_ptr(ffi::pbw_method_output_type(self.ptr.as_ptr())) }
            .to_str()
            .unwrap_or("")
    }

    pub fn client_streaming(&self) -> bool {
        unsafe { ffi::pbw_method_client_streaming(self.ptr.as_ptr()) != 0 }
    }

    pub fn server_streaming(&self) -> bool {
        unsafe { ffi::pbw_method_server_streaming(self.ptr.as_ptr()) != 0 }
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
    pub fn field(&self, name: &str) -> Result<Field<'_>> {
        let name = CString::new(name).map_err(|_| Error {
            status: -1,
            message: "field name contains a NUL byte".into(),
        })?;
        match NonNull::new(unsafe {
            ffi::pbw_descriptor_find_field_name(self.desc.as_ptr(), name.as_ptr())
        } as *mut ffi::pbw_field) {
            Some(ptr) => Ok(Field {
                ptr,
                _schema: PhantomData,
            }),
            None => Err(Error {
                status: -1,
                message: format!("{}: no such field", name.to_string_lossy()),
            }),
        }
    }

    pub fn field_by_number(&self, number: u32) -> Result<Field<'_>> {
        match NonNull::new(unsafe { ffi::pbw_descriptor_find_field(self.desc.as_ptr(), number) }
            as *mut ffi::pbw_field)
        {
            Some(ptr) => Ok(Field {
                ptr,
                _schema: PhantomData,
            }),
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

    /// Decodes with explicit bounds: inputs larger than `max_bytes` fail,
    /// and nesting past `max_depth` fails. Zero means the defaults (no byte
    /// cap, `PBW_MAX_DEPTH` nesting).
    pub fn decode_limits(&self, wire: &[u8], max_bytes: usize, max_depth: u32) -> Result<Message<'_>> {
        let limits = ffi::pbw_limits {
            max_bytes,
            max_depth,
        };
        let mut out: *mut ffi::pbw_message = ptr::null_mut();
        let status = unsafe {
            ffi::pbw_decode_limits(
                self.desc.as_ptr(),
                wire.as_ptr().cast(),
                wire.len(),
                &limits,
                &mut out,
            )
        };
        if status != ffi::PBW_OK {
            return Err(status_error(status));
        }
        Ok(Message {
            ptr: NonNull::new(out).ok_or_else(|| status_error(-1))?,
            owned: true,
            _parent: PhantomData,
        })
    }

    /// Decodes one length-delimited frame from the front of `wire`, returning
    /// the message and how many bytes the frame occupied.
    pub fn decode_delimited(&self, wire: &[u8]) -> Result<(Message<'_>, usize)> {
        let mut out: *mut ffi::pbw_message = ptr::null_mut();
        let mut consumed = 0usize;
        let status = unsafe {
            ffi::pbw_decode_delimited(
                self.desc.as_ptr(),
                wire.as_ptr().cast(),
                wire.len(),
                &mut out,
                &mut consumed,
            )
        };
        if status != ffi::PBW_OK {
            return Err(status_error(status));
        }
        Ok((
            Message {
                ptr: NonNull::new(out).ok_or_else(|| status_error(-1))?,
                owned: true,
                _parent: PhantomData,
            },
            consumed,
        ))
    }

    /// Parses JSON mapping text into a new tree. Unknown fields fail unless
    /// `ignore_unknown` is set.
    pub fn parse_json(&self, text: &str, ignore_unknown: bool) -> Result<Message<'_>> {
        self.parse_json_limits(text, ignore_unknown, 0, 0)
    }

    /// [`parse_json`](Self::parse_json) with explicit bounds: text longer than
    /// `max_bytes` fails, nesting past `max_depth` fails; 0 means the defaults.
    pub fn parse_json_limits(
        &self,
        text: &str,
        ignore_unknown: bool,
        max_bytes: usize,
        max_depth: u32,
    ) -> Result<Message<'_>> {
        let opts = ffi::pbw_json_parse_options {
            ignore_unknown_fields: c_int::from(ignore_unknown),
        };
        let limits = ffi::pbw_limits {
            max_bytes,
            max_depth,
        };
        let mut out: *mut ffi::pbw_message = ptr::null_mut();
        let status = unsafe {
            ffi::pbw_json_parse_limits(
                self.desc.as_ptr(),
                text.as_ptr().cast(),
                text.len(),
                &limits,
                &opts,
                &mut out,
            )
        };
        if status != ffi::PBW_OK {
            return Err(status_error(status));
        }
        Ok(Message {
            ptr: NonNull::new(out).ok_or_else(|| status_error(-1))?,
            owned: true,
            _parent: PhantomData,
        })
    }

    /// Parses text format into a new tree.
    pub fn parse_text(&self, text: &str) -> Result<Message<'_>> {
        self.parse_text_limits(text, 0, 0)
    }

    /// [`parse_text`](Self::parse_text) with explicit bounds.
    pub fn parse_text_limits(
        &self,
        text: &str,
        max_bytes: usize,
        max_depth: u32,
    ) -> Result<Message<'_>> {
        let limits = ffi::pbw_limits {
            max_bytes,
            max_depth,
        };
        let mut out: *mut ffi::pbw_message = ptr::null_mut();
        let status = unsafe {
            ffi::pbw_text_parse_limits(
                self.desc.as_ptr(),
                text.as_ptr().cast(),
                text.len(),
                &limits,
                &mut out,
            )
        };
        if status != ffi::PBW_OK {
            return Err(status_error(status));
        }
        Ok(Message {
            ptr: NonNull::new(out).ok_or_else(|| status_error(-1))?,
            owned: true,
            _parent: PhantomData,
        })
    }

    /// Packs `payload` (already encoded bytes of the message named by
    /// `type_url`) into a new `google.protobuf.Any` tree. `self` is the Any
    /// descriptor.
    pub fn any_pack(&self, type_url: &str, payload: &[u8]) -> Result<Message<'_>> {
        let url = CString::new(type_url).map_err(|_| Error {
            status: -1,
            message: "type url contains a NUL byte".into(),
        })?;
        let mut out: *mut ffi::pbw_message = ptr::null_mut();
        let status = unsafe {
            ffi::pbw_any_pack(
                self.desc.as_ptr(),
                url.as_ptr(),
                payload.as_ptr().cast(),
                payload.len(),
                &mut out,
            )
        };
        if status != ffi::PBW_OK {
            return Err(status_error(status));
        }
        Ok(Message {
            ptr: NonNull::new(out).ok_or_else(|| status_error(-1))?,
            owned: true,
            _parent: PhantomData,
        })
    }

    /// A new `google.protobuf.Timestamp` tree holding `ms`. `self` is the
    /// Timestamp descriptor.
    pub fn timestamp_from_millis(&self, ms: i64) -> Result<Message<'_>> {
        let mut out: *mut ffi::pbw_message = ptr::null_mut();
        let status =
            unsafe { ffi::pbw_timestamp_from_millis(self.desc.as_ptr(), ms, &mut out) };
        if status != ffi::PBW_OK {
            return Err(status_error(status));
        }
        Ok(Message {
            ptr: NonNull::new(out).ok_or_else(|| status_error(-1))?,
            owned: true,
            _parent: PhantomData,
        })
    }

    /// A new `google.protobuf.Duration` tree holding `ms`. `self` is the
    /// Duration descriptor.
    pub fn duration_from_millis(&self, ms: i64) -> Result<Message<'_>> {
        let mut out: *mut ffi::pbw_message = ptr::null_mut();
        let status =
            unsafe { ffi::pbw_duration_from_millis(self.desc.as_ptr(), ms, &mut out) };
        if status != ffi::PBW_OK {
            return Err(status_error(status));
        }
        Ok(Message {
            ptr: NonNull::new(out).ok_or_else(|| status_error(-1))?,
            owned: true,
            _parent: PhantomData,
        })
    }

    /// Single-level scan of `wire` without building a message tree: one
    /// callback per field, no allocation. Nested messages and packed runs
    /// arrive as byte payloads for the caller to recurse into. Returning an
    /// error aborts the scan with that error's status.
    pub fn visit<F>(&self, wire: &[u8], cb: &mut F) -> Result<()>
    where
        F: FnMut(VisitField<'_>) -> Result<()>,
    {
        unsafe extern "C" fn tramp<F>(
            ud: *mut c_void,
            field: *const ffi::pbw_field,
            number: u32,
            wire_type: u32,
            value: *const ffi::pbw_visit_value,
        ) -> c_int
        where
            F: FnMut(VisitField<'_>) -> Result<()>,
        {
            let cb = unsafe { &mut *(ud as *mut F) };
            let field = NonNull::new(field as *mut ffi::pbw_field).map(|ptr| Field {
                ptr,
                _schema: PhantomData,
            });
            let raw = unsafe { &*value };
            let data = if raw.data.is_null() {
                None
            } else {
                Some(unsafe {
                    std::slice::from_raw_parts(raw.data.cast::<u8>(), raw.len)
                })
            };
            // A panicking callback must not unwind through the C frame (that
            // aborts the process): report it as a failed scan instead.
            let outcome = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
                cb(VisitField {
                    field,
                    number,
                    wire_type,
                    raw: raw.u64,
                    data,
                })
            }));
            match outcome {
                Ok(Ok(())) => ffi::PBW_OK,
                Ok(Err(e)) => e.status,
                Err(_) => ffi::PBW_ERR_INVALID,
            }
        }
        let mut fail_off = 0usize;
        let status = unsafe {
            ffi::pbw_visit(
                self.desc.as_ptr(),
                wire.as_ptr().cast(),
                wire.len(),
                Some(tramp::<F>),
                cb as *mut F as *mut c_void,
                &mut fail_off,
            )
        };
        if status != ffi::PBW_OK {
            return Err(status_error(status));
        }
        Ok(())
    }
}

/// A resolved field handle: the number, wire type and storage the schema says.
/// Tied to the [`Type`] (and so the [`Schema`]) it came from: a handle cannot
/// outlive the descriptor it points into.
#[derive(Copy, Clone)]
pub struct Field<'s> {
    ptr: NonNull<ffi::pbw_field>,
    _schema: PhantomData<&'s ()>,
}

impl Field<'_> {
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
    pub fn set_i64(&mut self, field: Field<'_>, value: i64) -> &mut Self {
        self.store(field, unsafe { ffi::pbw_value_new_int(value) })
    }

    pub fn set_bool(&mut self, field: Field<'_>, value: bool) -> &mut Self {
        self.store(field, unsafe { ffi::pbw_value_new_bool(value as c_int) })
    }

    pub fn set_f32(&mut self, field: Field<'_>, value: f32) -> &mut Self {
        self.store(field, unsafe { ffi::pbw_value_new_float(value) })
    }

    pub fn set_f64(&mut self, field: Field<'_>, value: f64) -> &mut Self {
        self.store(field, unsafe { ffi::pbw_value_new_double(value) })
    }

    /// Sets a string field.
    ///
    /// # Panics
    ///
    /// Panics when `value` contains a NUL byte: the C ABI takes a
    /// NUL-terminated `const char *` for strings. protobuf strings may legally
    /// contain one, so use [`Message::set_bytes`] for those (a string field
    /// accepts a bytes value).
    pub fn set_str(&mut self, field: Field<'_>, value: &str) -> &mut Self {
        let text = CString::new(value).expect("set_str: string contains a NUL byte (use set_bytes)");
        self.store(field, unsafe { ffi::pbw_value_new_string(text.as_ptr()) })
    }

    pub fn set_bytes(&mut self, field: Field<'_>, value: &[u8]) -> &mut Self {
        self.store(field, unsafe {
            ffi::pbw_value_new_bytes(value.as_ptr().cast(), value.len())
        })
    }

    /// Nested message for a MESSAGE field, created in place (or returned as it is).
    pub fn set_message(&mut self, field: Field<'_>) -> Message<'_> {
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
    fn store(&mut self, field: Field<'_>, value: *mut ffi::pbw_value) -> &mut Self {
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

    pub fn clear(&mut self, field: Field<'_>) {
        unsafe { ffi::pbw_message_clear(self.ptr.as_ptr(), field.number()) }
    }

    /* ------------------------------------------------------------ reading */

    pub fn value(&self, field: Field<'_>) -> Option<Value<'_>> {
        let ptr = unsafe { ffi::pbw_message_get_field(self.ptr.as_ptr(), field.as_const()) };
        NonNull::new(ptr).map(|ptr| Value {
            ptr,
            _parent: PhantomData,
        })
    }

    pub fn has(&self, field: Field<'_>) -> bool {
        self.value(field).is_some()
    }

    pub fn get_i64(&self, field: Field<'_>) -> i64 {
        self.value(field).map_or(0, |v| unsafe {
            ffi::pbw_value_get_int(v.ptr.as_ptr())
        })
    }

    pub fn get_bool(&self, field: Field<'_>) -> bool {
        self.value(field).is_some_and(|v| unsafe {
            ffi::pbw_value_get_bool(v.ptr.as_ptr()) != 0
        })
    }

    pub fn get_f32(&self, field: Field<'_>) -> f32 {
        self.value(field).map_or(0.0, |v| unsafe {
            ffi::pbw_value_get_float(v.ptr.as_ptr())
        })
    }

    pub fn get_f64(&self, field: Field<'_>) -> f64 {
        self.value(field).map_or(0.0, |v| unsafe {
            ffi::pbw_value_get_double(v.ptr.as_ptr())
        })
    }

    pub fn get_str(&self, field: Field<'_>) -> Option<&str> {
        let value = self.value(field)?;
        let ptr = unsafe { ffi::pbw_value_get_string(value.ptr.as_ptr()) };
        if ptr.is_null() {
            return None;
        }
        unsafe { CStr::from_ptr(ptr) }.to_str().ok()
    }

    pub fn get_bytes(&self, field: Field<'_>) -> Option<&[u8]> {
        let value = self.value(field)?;
        let mut len = 0usize;
        let ptr = unsafe { ffi::pbw_value_get_bytes(value.ptr.as_ptr(), &mut len) };
        if ptr.is_null() {
            return None;
        }
        Some(unsafe { std::slice::from_raw_parts(ptr.cast::<u8>(), len) })
    }

    pub fn get_message(&self, field: Field<'_>) -> Option<Message<'_>> {
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

    /// Exact bytes [`Message::encode`] would write, without writing them.
    pub fn encoded_size(&self) -> Result<usize> {
        let mut size = 0usize;
        let status = unsafe { ffi::pbw_message_encoded_size(self.ptr.as_ptr(), &mut size) };
        if status != ffi::PBW_OK {
            return Err(status_error(status));
        }
        Ok(size)
    }

    /// True when every proto2 `required` field is present.
    pub fn is_initialized(&self) -> bool {
        unsafe { ffi::pbw_message_is_initialized(self.ptr.as_ptr()) != 0 }
    }

    /// Encodes with map entries sorted by key, so equal messages encode to
    /// equal bytes regardless of insertion order.
    pub fn encode_deterministic(&self) -> Result<Vec<u8>> {
        let mut out: *mut u8 = ptr::null_mut();
        let mut len = 0usize;
        let status =
            unsafe { ffi::pbw_encode_deterministic(self.ptr.as_ptr(), &mut out, &mut len) };
        if status != ffi::PBW_OK {
            return Err(status_error(status));
        }
        if len == 0 {
            unsafe { ffi::pbw_free(out.cast()) };
            return Ok(Vec::new());
        }
        let bytes = unsafe { std::slice::from_raw_parts(out, len) }.to_vec();
        unsafe { ffi::pbw_free(out.cast()) };
        Ok(bytes)
    }

    /// Encodes one length-delimited frame (varint length + message).
    pub fn encode_delimited(&self) -> Result<Vec<u8>> {
        let mut out: *mut u8 = ptr::null_mut();
        let mut len = 0usize;
        let status = unsafe { ffi::pbw_encode_delimited(self.ptr.as_ptr(), &mut out, &mut len) };
        if status != ffi::PBW_OK {
            return Err(status_error(status));
        }
        if len == 0 {
            unsafe { ffi::pbw_free(out.cast()) };
            return Ok(Vec::new());
        }
        let bytes = unsafe { std::slice::from_raw_parts(out, len) }.to_vec();
        unsafe { ffi::pbw_free(out.cast()) };
        Ok(bytes)
    }

    /// Prints this message in protobuf JSON mapping. Unknown fields are not
    /// printed, matching the reference printer.
    pub fn print_json(&self) -> Result<String> {
        let mut out: *mut c_char = ptr::null_mut();
        let mut len = 0usize;
        let status = unsafe { ffi::pbw_json_print(self.ptr.as_ptr(), &mut out, &mut len) };
        if status != ffi::PBW_OK {
            return Err(status_error(status));
        }
        if len == 0 || out.is_null() {
            unsafe { ffi::pbw_free(out.cast()) };
            return Ok(String::new());
        }
        let bytes = unsafe { std::slice::from_raw_parts(out.cast::<u8>(), len) }.to_vec();
        unsafe { ffi::pbw_free(out.cast()) };
        String::from_utf8(bytes).map_err(|_| status_error(-1))
    }

    /// Prints this message in protobuf text format. Unknown fields print as
    /// numeric lines when `print_unknown` is set, like the reference printer.
    pub fn print_text(&self, print_unknown: bool) -> Result<String> {
        let mut out: *mut c_char = ptr::null_mut();
        let mut len = 0usize;
        // pbw_text_print prints unknown fields; the opts entry is the one
        // that honours the flag either way.
        let status = unsafe {
            ffi::pbw_text_print_opts(
                self.ptr.as_ptr(),
                c_int::from(print_unknown),
                &mut out,
                &mut len,
            )
        };
        if status != ffi::PBW_OK {
            return Err(status_error(status));
        }
        if len == 0 || out.is_null() {
            unsafe { ffi::pbw_free(out.cast()) };
            return Ok(String::new());
        }
        let bytes = unsafe { std::slice::from_raw_parts(out.cast::<u8>(), len) }.to_vec();
        unsafe { ffi::pbw_free(out.cast()) };
        String::from_utf8(bytes).map_err(|_| status_error(-1))
    }

    /// Field 1 of a `google.protobuf.Any` tree, or `None` when absent.
    pub fn any_type_url(&self) -> Option<&str> {
        let ptr = unsafe { ffi::pbw_any_type_url(self.ptr.as_ptr()) };
        if ptr.is_null() {
            return None;
        }
        unsafe { CStr::from_ptr(ptr) }.to_str().ok()
    }

    /// Field 2 of a `google.protobuf.Any` tree, or `None` when absent.
    pub fn any_payload(&self) -> Option<&[u8]> {
        let mut len = 0usize;
        let ptr = unsafe { ffi::pbw_any_payload(self.ptr.as_ptr(), &mut len) };
        if ptr.is_null() {
            return None;
        }
        Some(unsafe { std::slice::from_raw_parts(ptr.cast::<u8>(), len) })
    }

    /// A `google.protobuf.Timestamp` tree as milliseconds.
    pub fn timestamp_millis(&self) -> Result<i64> {
        let mut ms = 0i64;
        let status = unsafe { ffi::pbw_timestamp_to_millis(self.ptr.as_ptr(), &mut ms) };
        if status != ffi::PBW_OK {
            return Err(status_error(status));
        }
        Ok(ms)
    }

    /// A `google.protobuf.Duration` tree as milliseconds.
    pub fn duration_millis(&self) -> Result<i64> {
        let mut ms = 0i64;
        let status = unsafe { ffi::pbw_duration_to_millis(self.ptr.as_ptr(), &mut ms) };
        if status != ffi::PBW_OK {
            return Err(status_error(status));
        }
        Ok(ms)
    }

    /// How many paths a `google.protobuf.FieldMask` tree holds.
    pub fn fieldmask_path_count(&self) -> usize {
        unsafe { ffi::pbw_fieldmask_path_count(self.ptr.as_ptr()) }
    }

    /// One `google.protobuf.FieldMask` path (`None` past the end).
    pub fn fieldmask_path_at(&self, index: usize) -> Option<&str> {
        let ptr = unsafe { ffi::pbw_fieldmask_path_at(self.ptr.as_ptr(), index) };
        if ptr.is_null() {
            return None;
        }
        unsafe { CStr::from_ptr(ptr) }.to_str().ok()
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
    /// so a hot loop allocates nothing. The caller's buffer is tried first, so a
    /// steady-state loop walks the tree exactly once; only a buffer that does
    /// not fit triggers a `pbw_message_encoded_size` walk (which also reports a
    /// depth-overflow tree as LIMIT instead of growing the buffer forever), and
    /// the core's writer over-reserves up to 12 bytes per write, hence the
    /// slack.
    pub fn encode_to<'b>(&self, buf: &'b mut Vec<u8>) -> Result<&'b [u8]> {
        /* Fast path: one walk. A buffer that is already big enough (the steady
         * state after the first call) never pays the sizing walk, so a hot loop
         * encodes once per call instead of twice. */
        if buf.capacity() > 0 {
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
                /* The buffer did not fit: size it below and try once more. */
                ffi::PBW_ERR_LIMIT => {}
                _ => return Err(status_error(status)),
            }
        }
        /* Slow path: the exact size comes from `pbw_message_encoded_size`
         * (a depth-overflow tree reports LIMIT there and returns, instead of
         * growing the buffer forever); the core's writer over-reserves up to
         * 12 bytes per write, hence the slack. */
        let mut size = 0usize;
        let status = unsafe { ffi::pbw_message_encoded_size(self.ptr.as_ptr(), &mut size) };
        if status != ffi::PBW_OK {
            return Err(status_error(status));
        }
        let want = size.saturating_add(12);
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
        if status != ffi::PBW_OK {
            return Err(status_error(status));
        }
        unsafe { buf.set_len(len) };
        Ok(buf.as_slice())
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

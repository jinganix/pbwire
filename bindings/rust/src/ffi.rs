//! The C ABI, hand-declared: `include/pbwire.h` is the whole contract, so no
//! bindgen step, no build-time dependency, and nothing to keep in sync beyond
//! the signatures used below.

use std::ffi::{c_char, c_int, c_void};

macro_rules! opaque {
    ($($name:ident),* $(,)?) => {
        $(
            #[repr(C)]
            pub struct $name {
                _private: [u8; 0],
            }
        )*
    };
}

opaque!(pbw_schema, pbw_descriptor, pbw_field, pbw_message, pbw_value);

extern "C" {
    /* schema */
    pub fn pbw_schema_free(schema: *mut pbw_schema);
    pub fn pbw_schema_error(schema: *const pbw_schema) -> *const c_char;
    pub fn pbw_schema_find(schema: *const pbw_schema, name: *const c_char) -> *mut pbw_descriptor;
    pub fn pbw_descriptor_set_load(
        data: *const c_void,
        len: usize,
        out: *mut *mut pbw_schema,
    ) -> c_int;
    pub fn pbw_descriptor_name(desc: *const pbw_descriptor) -> *const c_char;
    pub fn pbw_descriptor_find_field_name(
        desc: *const pbw_descriptor,
        name: *const c_char,
    ) -> *const pbw_field;
    pub fn pbw_descriptor_find_field(desc: *const pbw_descriptor, number: u32) -> *const pbw_field;
    pub fn pbw_field_name(field: *const pbw_field) -> *const c_char;
    pub fn pbw_field_number(field: *const pbw_field) -> u32;
    pub fn pbw_field_type(field: *const pbw_field) -> c_int;
    pub fn pbw_field_label(field: *const pbw_field) -> c_int;

    /* message */
    pub fn pbw_message_new(desc: *const pbw_descriptor) -> *mut pbw_message;
    pub fn pbw_message_free(msg: *mut pbw_message);
    pub fn pbw_message_get_field(msg: *const pbw_message, field: *const pbw_field)
        -> *mut pbw_value;
    pub fn pbw_message_set_field(
        msg: *mut pbw_message,
        field: *const pbw_field,
        value: *mut pbw_value,
    ) -> c_int;
    pub fn pbw_message_set_message(msg: *mut pbw_message, number: u32) -> *mut pbw_message;
    pub fn pbw_message_clear(msg: *mut pbw_message, number: u32);
    pub fn pbw_message_wire_size(msg: *const pbw_message) -> usize;
    pub fn pbw_message_encoded_size(msg: *const pbw_message, out: *mut usize) -> c_int;
    pub fn pbw_message_is_initialized(msg: *const pbw_message) -> c_int;

    /* value */
    pub fn pbw_value_new_int(value: i64) -> *mut pbw_value;
    pub fn pbw_value_new_bool(value: c_int) -> *mut pbw_value;
    pub fn pbw_value_new_float(value: f32) -> *mut pbw_value;
    pub fn pbw_value_new_double(value: f64) -> *mut pbw_value;
    pub fn pbw_value_new_bytes(data: *const c_void, len: usize) -> *mut pbw_value;
    pub fn pbw_value_new_string(string: *const c_char) -> *mut pbw_value;
    pub fn pbw_value_free(value: *mut pbw_value);
    pub fn pbw_value_get_kind(value: *const pbw_value) -> c_int;
    pub fn pbw_value_get_int(value: *const pbw_value) -> i64;
    pub fn pbw_value_get_bool(value: *const pbw_value) -> c_int;
    pub fn pbw_value_get_float(value: *const pbw_value) -> f32;
    pub fn pbw_value_get_double(value: *const pbw_value) -> f64;
    pub fn pbw_value_get_bytes(value: *const pbw_value, len: *mut usize) -> *const c_void;
    pub fn pbw_value_get_string(value: *const pbw_value) -> *const c_char;
    pub fn pbw_value_get_message(value: *const pbw_value) -> *mut pbw_message;
    pub fn pbw_value_count(value: *const pbw_value) -> usize;
    pub fn pbw_value_at(value: *const pbw_value, index: usize) -> *mut pbw_value;

    /* wire */
    pub fn pbw_encode(msg: *const pbw_message, out: *mut *mut u8, len: *mut usize) -> c_int;
    pub fn pbw_encode_to(
        msg: *const pbw_message,
        out: *mut c_void,
        cap: usize,
        len: *mut usize,
    ) -> c_int;
    pub fn pbw_decode(
        desc: *const pbw_descriptor,
        buf: *const c_void,
        len: usize,
        out: *mut *mut pbw_message,
    ) -> c_int;
    pub fn pbw_strerror(status: c_int) -> *const c_char;
    pub fn pbw_free(ptr: *mut c_void);
}

/* pbw_status / pbw_type / pbw_label / pbw_value_kind */
pub const PBW_OK: c_int = 0;
pub const PBW_ERR_LIMIT: c_int = 5;
pub const PBW_TYPE_GROUP: c_int = 10;
pub const PBW_TYPE_MESSAGE: c_int = 11;
pub const PBW_LABEL_REPEATED: c_int = 3;
pub const PBW_VALUE_UINT: c_int = 0;
pub const PBW_VALUE_BYTES: c_int = 1;
pub const PBW_VALUE_MESSAGE: c_int = 2;

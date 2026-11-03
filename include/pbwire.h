/*
 * pbwire - a dependency-free Protobuf runtime in C with dynamic schema support.
 *
 * Schemas are built at runtime (see pbw_schema_*); messages are dynamic value
 * trees (see pbw_message_*); wire encoding/decoding follows the standard
 * Protobuf wire format (see pbw_encode / pbw_decode).
 *
 * Conventions:
 *   - Every pointer returned by this library is owned by the library unless
 * documented otherwise; free with pbw_free() / pbw_message_free() /
 * pbw_value_free().
 *   - Numeric values are stored as raw bit patterns: PBW_TYPE_FLOAT and
 *     PBW_TYPE_FIXED32/PBW_TYPE_SFIXED32 live in the low 32 bits,
 * PBW_TYPE_DOUBLE and the 64-bit fixed types use all 64 bits. Signed types are
 * sign extended.
 *   - Byte payloads are always NUL terminated so pbw_value_get_string() is safe
 * on decoded string fields (the extra byte is not part of the field length).
 *   - No defaults are filled in: a field that was not present on the wire is
 * simply absent (pbw_message_get() returns NULL). Explicit presence per field.
 */
#ifndef PBWIRE_H
#define PBWIRE_H

#include <stddef.h>
#include <stdint.h>

/* ABI version: bumped on every breaking change to the C ABI (removed or
 * reordered exports, changed struct layouts visible to callers). Additive
 * changes keep the number. The shared library SONAME carries the same
 * number, so `libpbwire.so.1` and `PBW_ABI_VERSION == 1` always agree. */
#define PBW_ABI_VERSION 1

#ifdef __cplusplus
extern "C" {
#endif

/* Runtime view of PBW_ABI_VERSION, for bindings that dlopen the library. */
int pbw_abi_version(void);

typedef enum {
    PBW_OK = 0,
    PBW_ERR_INVALID = 1,   /* malformed wire data or invalid argument */
    PBW_ERR_NOMEM = 2,     /* allocation failure */
    PBW_ERR_NOT_FOUND = 3, /* unknown field number / name */
    PBW_ERR_TYPE = 4,      /* value does not match the field type */
    PBW_ERR_LIMIT = 5,     /* nesting or size limit exceeded */
    PBW_ERR_REQUIRED = 6   /* a proto2 required field has no value */
} pbw_status;

/* Values match protobuf FieldDescriptorProto.Type numbering. */
typedef enum {
    PBW_TYPE_DOUBLE = 1,
    PBW_TYPE_FLOAT = 2,
    PBW_TYPE_INT64 = 3,
    PBW_TYPE_UINT64 = 4,
    PBW_TYPE_INT32 = 5,
    PBW_TYPE_FIXED64 = 6,
    PBW_TYPE_FIXED32 = 7,
    PBW_TYPE_BOOL = 8,
    PBW_TYPE_STRING = 9,
    PBW_TYPE_GROUP = 10,
    PBW_TYPE_MESSAGE = 11,
    PBW_TYPE_BYTES = 12,
    PBW_TYPE_UINT32 = 13,
    PBW_TYPE_ENUM = 14,
    PBW_TYPE_SFIXED32 = 15,
    PBW_TYPE_SFIXED64 = 16,
    PBW_TYPE_SINT32 = 17,
    PBW_TYPE_SINT64 = 18
} pbw_type;

/* Values match protobuf FieldDescriptorProto.Label numbering. */
typedef enum {
    PBW_LABEL_OPTIONAL = 1,
    PBW_LABEL_REQUIRED = 2,
    PBW_LABEL_REPEATED = 3
} pbw_label;

typedef enum {
    PBW_VALUE_UINT = 0,  /* any numeric type, raw bit pattern */
    PBW_VALUE_BYTES = 1, /* string or bytes */
    PBW_VALUE_MESSAGE = 2,
    PBW_VALUE_LIST = 3 /* repeated field storage */
} pbw_value_kind;

typedef struct pbw_schema pbw_schema;
typedef struct pbw_ctx pbw_ctx;
typedef struct pbw_descriptor pbw_descriptor;
typedef struct pbw_field pbw_field;
typedef struct pbw_value pbw_value;
typedef struct pbw_message pbw_message;

/* ---------------------------------------------------------------- schema */

pbw_schema *pbw_schema_new(void);
void pbw_schema_free(pbw_schema *schema);

/* Create a message or enum descriptor. Names must be unique within a schema. */
pbw_descriptor *pbw_schema_add_message(pbw_schema *schema, const char *name);
pbw_descriptor *pbw_schema_add_enum(pbw_schema *schema, const char *name);
pbw_descriptor *pbw_schema_find(const pbw_schema *schema, const char *name);

/* Dialect for the descriptors added afterwards: 1 = proto3/editions semantics
 * (open enums, no required checks; the default), 0 = proto2 semantics (closed
 * enums, `required` fields validated when decoding). A descriptor set sets this
 * per file on its own, so this is only needed for hand-built schemas. */
void pbw_schema_set_proto3(pbw_schema *schema, int proto3);

/* Append an enum value. Enum descriptors are only usable after
 * pbw_schema_link(). */
void pbw_enum_add_value(pbw_descriptor *enum_desc, const char *name,
                        int32_t number);
int pbw_enum_find_name(const pbw_descriptor *enum_desc, int32_t number,
                       const char **name);
int pbw_enum_find_value(const pbw_descriptor *enum_desc, const char *name,
                        int32_t *number);

/* Append a field. Field numbers must be unique within the message. */
pbw_field *pbw_message_add_field(pbw_descriptor *msg, const char *name,
                                 uint32_t number, pbw_type type,
                                 pbw_label label);
/* Append an extension field to the message it extends. Only the number must be
 * unique: extensions from different scopes may share a name, so they are found
 * by number (pbw_descriptor_find_field), not by name. */
pbw_field *pbw_message_add_extension(pbw_descriptor *msg, const char *name,
                                     uint32_t number, pbw_type type,
                                     pbw_label label);

/* Point a MESSAGE/ENUM field at another descriptor by name; resolved by
 * pbw_schema_link(). */
void pbw_field_set_type_name(pbw_field *field, const char *type_name);
/* Dotted full name of an extension field ("pkg.Ext"), used inside brackets
 * by the text format. The loader sets this from the declaring scope; call it
 * for hand-built extensions that must parse `[pkg.Ext]` text. */
void pbw_field_set_full_name(pbw_field *field, const char *full_name);
/* Emit repeated numeric fields packed (proto3 default). */
void pbw_field_set_packed(pbw_field *field, int packed);
/* Mark a message as a map entry (MessageOptions.map_entry): JSON prints it
 * as an object instead of an array of entries. The loader sets this from
 * the descriptor; call it for hand-built map entries. */
void pbw_descriptor_set_map_entry(pbw_descriptor *msg);
/* 1-based oneof group index; 0 means the field is not in a oneof. */
void pbw_field_set_oneof(pbw_field *field, uint32_t oneof_index);

/* Resolve type names, sort fields by number and build lookup tables.
 * Idempotent. */
pbw_status pbw_schema_link(pbw_schema *schema);
/* Last link failure reason (empty when the last link succeeded). */
const char *pbw_schema_error(const pbw_schema *schema);

/* Load a standard FileDescriptorSet (protoc --descriptor_set_out, buf, ...)
 * into a new schema, so a schema can come from a .proto without generated code.
 * Every referenced type must be present in the set (use --include_imports).
 * Descriptors are named fully qualified without the leading dot:
 * "pkg.Outer.Inner". Field type names resolve the same way.
 * *out is always written; on failure it holds the partially built schema so
 * pbw_schema_error() explains why, and the caller frees it as usual. */
pbw_status pbw_descriptor_set_load(const void *data, size_t len,
                                   pbw_schema **out);

const char *pbw_descriptor_name(const pbw_descriptor *desc);
int pbw_descriptor_is_enum(const pbw_descriptor *desc);
size_t pbw_descriptor_field_count(const pbw_descriptor *desc);
const pbw_field *pbw_descriptor_field_at(const pbw_descriptor *desc,
                                         size_t index);
const pbw_field *pbw_descriptor_find_field(const pbw_descriptor *desc,
                                           uint32_t number);
const pbw_field *pbw_descriptor_find_field_name(const pbw_descriptor *desc,
                                                const char *name);

const char *pbw_field_name(const pbw_field *field);
/* proto2 default as written in the .proto, or NULL when the field has none. */
const char *pbw_field_default_value(const pbw_field *field);
/* 1 when the field was declared as an extension of its message. */
int pbw_field_is_extension(const pbw_field *field);
uint32_t pbw_field_number(const pbw_field *field);
pbw_type pbw_field_type(const pbw_field *field);
pbw_label pbw_field_label(const pbw_field *field);
int pbw_field_is_packed(const pbw_field *field);
uint32_t pbw_field_oneof_index(const pbw_field *field);
const pbw_descriptor *pbw_field_message_type(const pbw_field *field);

/* --------------------------------------------------------------- message */

/* A message keeps pointing at its descriptor: the schema must outlive it. */
pbw_message *pbw_message_new(const pbw_descriptor *desc);
void pbw_message_free(pbw_message *msg);
const pbw_descriptor *pbw_message_descriptor(const pbw_message *msg);

/* Present value of a field, or NULL. Repeated fields expose a PBW_VALUE_LIST.
 */
pbw_value *pbw_message_get(const pbw_message *msg, uint32_t number);
/* Number of fields with a value. */
size_t pbw_message_field_count(const pbw_message *msg);
/* Reflection over present fields; returns NULL past the end. */
const pbw_field *pbw_message_field_at(const pbw_message *msg, size_t index,
                                      pbw_value **value);
/* 0 when a proto2 `required` field (recursively, through submessages) has no
 * value. pbw_decode() already rejects such data the way the reference parser
 * does; this is for a tree you built yourself, which pbw_encode() writes as it
 * stands (the reference serializer does the same). Always 1 for proto3
 * messages that contain no proto2 submessage. */
int pbw_message_is_initialized(const pbw_message *msg);
/* Drop a field value (and, for a oneof, the group case). */
void pbw_message_clear(pbw_message *msg, uint32_t number);

/* Store a value: appends for repeated fields, replaces otherwise. Takes
 * ownership of value on success only. Values owned by another message (see
 * pbw_message_get()) are rejected with PBW_ERR_INVALID; build a fresh value
 * instead. */
pbw_status pbw_message_set(pbw_message *msg, uint32_t number, pbw_value *value);
/* Create (or return the existing) nested message value for a MESSAGE field. */
pbw_message *pbw_message_set_message(pbw_message *msg, uint32_t number);

/* ----------------------------------------------------------------- value */

pbw_value *pbw_value_new_uint(uint64_t bits);
pbw_value *pbw_value_new_int(int64_t value);
pbw_value *pbw_value_new_bool(int value);
pbw_value *pbw_value_new_float(float value);
pbw_value *pbw_value_new_double(double value);
pbw_value *pbw_value_new_bytes(const void *data, size_t len);
pbw_value *pbw_value_new_string(const char *str);
pbw_value *pbw_value_new_message(pbw_message *msg); /* takes ownership of msg */
/* Frees a value created by pbw_value_new*(). Values owned by a message (from
 * pbw_message_get(), pbw_value_at(), ...) are released with it: this call is a
 * no-op on them. */
void pbw_value_free(pbw_value *value);

pbw_value_kind pbw_value_get_kind(const pbw_value *value);
uint64_t pbw_value_get_uint(const pbw_value *value);
int64_t pbw_value_get_int(const pbw_value *value);
int32_t pbw_value_get_int32(const pbw_value *value);
uint32_t pbw_value_get_uint32(const pbw_value *value);
int pbw_value_get_bool(const pbw_value *value);
float pbw_value_get_float(const pbw_value *value);
double pbw_value_get_double(const pbw_value *value);
const void *pbw_value_get_bytes(const pbw_value *value, size_t *len);
const char *pbw_value_get_string(const pbw_value *value);
pbw_message *pbw_value_get_message(const pbw_value *value);
size_t pbw_value_count(const pbw_value *value);
pbw_value *pbw_value_at(const pbw_value *value, size_t index);

/* -------------------------------------------------------------- workspace */

/*
 * A context owns a bump allocator and a reusable encode buffer, so a program
 * that decodes or encodes in a loop stops calling malloc after the first few
 * messages. One context per thread/goroutine; never share one concurrently.
 *
 * Messages decoded with a context live in its allocator:
 *   - pbw_message_free() on them is a no-op (they die with pbw_ctx_reset)
 *   - they are read-only: pbw_message_set/set_message fail, clear does nothing
 *   - pbw_ctx_reset() invalidates every message decoded with it so far
 * Plain pbw_decode()/pbw_encode() allocate one block per call instead (see
 * below), so they need no workspace at all.
 */
pbw_ctx *pbw_ctx_new(void);
void pbw_ctx_free(pbw_ctx *ctx);
/* Release every allocation made since the last reset, keeping the buffers. */
void pbw_ctx_reset(pbw_ctx *ctx);
/* Max bytes the context keeps resident across resets (decode arena head +
 * encode buffer), default 1 MiB. Single allocations above it bypass the
 * reusable chunk and are freed on reset, so one outlier never pins the
 * context. 0 keeps nothing: every reset frees everything, like the plain
 * functions but through the context API. Applies lazily at the next reset. */
void pbw_ctx_set_retain_cap(pbw_ctx *ctx, size_t cap);

typedef struct pbw_pool pbw_pool;

/* Bounded freelist of contexts: acquire instead of pbw_ctx_new, release
 * instead of pbw_ctx_reset/pbw_ctx_free. A released context is already
 * reset, so the next acquire can use it directly; releases past max_idle
 * are freed. Unsynchronized: one pool per worker thread, never shared
 * concurrently (this keeps the core dependency-free). Allocation is lazy;
 * steady use then stops calling malloc.
 *
 * retain_cap is stamped on contexts the pool creates (0 = the 1 MiB
 * default, like the zero-means-default pbw_limits fields); max_idle 0
 * keeps nothing and the pool degenerates to new/free. Releasing a context
 * whose retain cap the caller changed keeps that cap. */
pbw_pool *pbw_pool_new(size_t max_idle, size_t retain_cap);
void pbw_pool_free(pbw_pool *pool);
/* Exclusive use until released; NULL when pool is NULL or out of memory. */
pbw_ctx *pbw_pool_acquire(pbw_pool *pool);
void pbw_pool_release(pbw_pool *pool, pbw_ctx *ctx);
/* Idle contexts held now (for sizing/debugging, the printf side). */
size_t pbw_pool_idle(const pbw_pool *pool);
/* Bytes currently held in the context (for sizing/debugging). */
size_t pbw_ctx_used(const pbw_ctx *ctx);

/* Decode into the context instead of one allocation per value. */
pbw_status pbw_decode_ctx(pbw_ctx *ctx, const pbw_descriptor *desc,
                          const void *buf, size_t len, pbw_message **out);
/* Encode into the context's buffer. *out stays valid until the next
 * pbw_encode_ctx()/pbw_ctx_reset()/pbw_ctx_free() on the same context. */
pbw_status pbw_encode_ctx(pbw_ctx *ctx, const pbw_message *msg,
                          const uint8_t **out, size_t *len);

/* ------------------------------------------------------------------ wire */

/* Encode msg; *out is replaced with a malloc'd buffer owned by the caller
 * (pbw_free). */
/* Print msg in protobuf text format. *out is malloc'd, NUL-terminated and owned
 * by the caller (pbw_free); *len excludes the NUL. Unknown fields are shown
 * as numeric `field: value` lines, like the reference printer. */
pbw_status pbw_text_print(const pbw_message *msg, char **out, size_t *len);
/* Same, with explicit control over unknown fields (the conformance suite asks
 * for both). */
pbw_status pbw_text_print_opts(const pbw_message *msg, int print_unknown_fields,
                               char **out, size_t *len);
/* Parse text format into a new message tree. Reserved field names are skipped
 * silently; anything else unknown fails. A missing proto2 `required` field
 * fails with PBW_ERR_REQUIRED, like the binary decoder. */
pbw_status pbw_text_parse(const pbw_descriptor *desc, const char *text,
                          size_t len, pbw_message **out);

/* Print msg in protobuf JSON mapping. *out is malloc'd, NUL-terminated and
 * owned by the caller (pbw_free); *len excludes the NUL. Matches the
 * reference printer semantically: 64-bit integers print quoted, bytes print
 * as standard base64, enums print by name, well-known types use their JSON
 * form (`Any`, `Timestamp`, `Duration`, `Struct`, `FieldMask`, wrappers).
 * Unknown fields are not printed. A message that cannot be represented
 * (invalid UTF-8, non-finite `Value`, out-of-range time) fails. */
pbw_status pbw_json_print(const pbw_message *msg, char **out, size_t *len);
/* Parse JSON mapping into a new message tree. Unknown fields fail unless
 * `ignore_unknown_fields` is set (the conformance JSON_IGNORE_UNKNOWN
 * category). A missing proto2 `required` field fails with PBW_ERR_REQUIRED. */
typedef struct pbw_json_parse_options {
    /* Skip unknown field names and unknown `Any` types instead of failing. */
    int ignore_unknown_fields;
} pbw_json_parse_options;
pbw_status pbw_json_parse_opts(const pbw_descriptor *desc, const char *text,
                               size_t len,
                               const pbw_json_parse_options *options,
                               pbw_message **out);
/* Same with default options (unknown fields fail). */
pbw_status pbw_json_parse(const pbw_descriptor *desc, const char *text,
                          size_t len, pbw_message **out);

pbw_status pbw_encode(const pbw_message *msg, uint8_t **out, size_t *len);
/* Decode buf into a new message of the given type. Unknown fields are kept.
 *
 * The returned tree is one allocation: pbw_message_free() on the root releases
 * every submessage, repeated item and string in it, so values and submessages
 * obtained from it must not be freed with pbw_value_free()/pbw_message_free()
 * (those become no-ops on them). The tree is mutable: pbw_message_set()/
 * clear()/set_message() work on it and their payloads are released normally. */
pbw_status pbw_decode(const pbw_descriptor *desc, const void *buf, size_t len,
                      pbw_message **out);

/* ------------------------------------------------------------------ limits */

/* Bounds for untrusted input. Zero fields mean the defaults: no byte cap and
 * PBW_MAX_DEPTH nesting. A max_depth above PBW_MAX_DEPTH is clamped to it. */
typedef struct pbw_limits {
    size_t max_bytes;   /* reject inputs larger than this */
    uint32_t max_depth; /* nesting cap, 0 = PBW_MAX_DEPTH */
} pbw_limits;

/* Decode with explicit bounds; identical to pbw_decode() with NULL limits. */
pbw_status pbw_decode_limits(const pbw_descriptor *desc, const void *buf,
                             size_t len, const pbw_limits *limits,
                             pbw_message **out);
pbw_status pbw_decode_ctx_limits(pbw_ctx *ctx, const pbw_descriptor *desc,
                                 const void *buf, size_t len,
                                 const pbw_limits *limits, pbw_message **out);

/* Encode with map entries sorted by key, so equal messages encode to equal
 * bytes regardless of insertion order. Non-map fields encode exactly as
 * pbw_encode() writes them. */
pbw_status pbw_encode_deterministic(const pbw_message *msg, uint8_t **out,
                                    size_t *len);

/* --------------------------------------------------------------- streaming */

/* Sink for streaming encodes: return PBW_OK to continue, anything else aborts
 * the encode with that status. */
typedef pbw_status (*pbw_sink_fn)(const uint8_t *data, size_t len,
                                  void *opaque);

/* Encode msg and hand the bytes to sink, in 8 KiB chunks. */
pbw_status pbw_encode_sink(const pbw_message *msg, pbw_sink_fn sink,
                           void *opaque);
/* Length-delimited framing (varint length + message), the standard framing
 * for a stream of messages. */
pbw_status pbw_encode_delimited(const pbw_message *msg, uint8_t **out,
                                size_t *len);

typedef struct pbw_istream pbw_istream;

/* Incremental delimited-message reader: feed() appends wire bytes as they
 * arrive, next() decodes one message per call. next() writes NULL to *out
 * and returns PBW_OK when the buffer holds no complete message yet. */
pbw_istream *pbw_istream_new(const pbw_limits *limits); /* NULL = defaults */
void pbw_istream_free(pbw_istream *s);
/* Drop buffered bytes (complete and partial frames alike). */
void pbw_istream_reset(pbw_istream *s);
pbw_status pbw_istream_feed(pbw_istream *s, const void *data, size_t len);
pbw_status pbw_istream_next(pbw_istream *s, const pbw_descriptor *desc,
                            pbw_message **out);
void pbw_free(void *ptr);
const char *pbw_strerror(pbw_status status);

#ifdef __cplusplus
}
#endif

#endif /* PBWIRE_H */

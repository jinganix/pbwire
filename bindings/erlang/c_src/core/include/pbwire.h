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
 *     PBW_TYPE_DOUBLE and the 64-bit fixed types use all 64 bits. Signed
 *     types are sign extended.
 *   - Byte payloads are always NUL terminated so pbw_value_get_string() is safe
 * on decoded string fields (the extra byte is not part of the field length).
 *   - No defaults are filled in: a field that was not present on the wire is
 * simply absent (pbw_message_get() returns NULL). Explicit presence per field.
 */
#ifndef PBWIRE_H
#define PBWIRE_H

#include <stddef.h>
#include <stdint.h>

/* The library is built with -fvisibility=hidden: everything declared here is
 * exported, and the internal helpers (src/internal.h) stay out of the dynamic
 * symbol table. An embedder can therefore not link against them by accident,
 * and their names are free to change without an ABI break. */
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC visibility push(default)
#endif

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
typedef struct pbw_service pbw_service;
typedef struct pbw_method pbw_method;

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
 * pbw_schema_link(). Returns PBW_OK, PBW_ERR_INVALID for a bad descriptor
 * or a frozen schema, PBW_ERR_NOMEM when the value cannot be stored. */
pbw_status pbw_enum_add_value(pbw_descriptor *enum_desc, const char *name,
                              int32_t number);
int pbw_enum_find_name(const pbw_descriptor *enum_desc, int32_t number,
                       const char **name);
int pbw_enum_find_value(const pbw_descriptor *enum_desc, const char *name,
                        int32_t *number);

/* Append a field. Field numbers must be unique within the message, and every
 * field must be added before the first successful pbw_schema_link(): the layout
 * (and with it the value slots of every existing message) freezes then, so a
 * later add fails and returns NULL. */
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

/* Resolve type names, sort fields by number and build lookup tables. Idempotent
 * while no field is added; the first success freezes the field layout, so
 * create messages and set values only after it returns PBW_OK. */
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
/* Nonzero when the descriptor is a map entry (MessageOptions.map_entry). */
int pbw_descriptor_is_map_entry(const pbw_descriptor *desc);
/* 1 for proto3/editions semantics (open enums, no `required` fields), 0 for
 * proto2 syntax. Decides whether an unknown enum number stays in the field or
 * moves to the unknown fields, so a decoder that materializes values itself
 * needs it to match the reference parsers. */
int pbw_descriptor_is_proto3(const pbw_descriptor *desc);
size_t pbw_descriptor_field_count(const pbw_descriptor *desc);
const pbw_field *pbw_descriptor_field_at(const pbw_descriptor *desc,
                                         size_t index);
const pbw_field *pbw_descriptor_find_field(const pbw_descriptor *desc,
                                           uint32_t number);
const pbw_field *pbw_descriptor_find_field_name(const pbw_descriptor *desc,
                                                const char *name);

const char *pbw_field_name(const pbw_field *field);
/* Services: loaded from a FileDescriptorSet, queryable for routing. The
 * core does no RPC; a transport (gRPC, Twirp, plain HTTP) uses these to
 * resolve method names to input/output types. Services live in the schema
 * and die with it. Hand-built schemas carry no services. */
size_t pbw_schema_service_count(const pbw_schema *schema);
const pbw_service *pbw_schema_service_at(const pbw_schema *schema,
                                         size_t index);
const pbw_service *pbw_schema_service_find(const pbw_schema *schema,
                                           const char *name);
/* Fully qualified without the leading dot ("pkg.Service"), like descriptors. */
const char *pbw_service_name(const pbw_service *svc);
size_t pbw_service_method_count(const pbw_service *svc);
const pbw_method *pbw_service_method_at(const pbw_service *svc, size_t index);
const pbw_method *pbw_service_method_find(const pbw_service *svc,
                                          const char *name);
const char *pbw_method_name(const pbw_method *method);
/* Fully qualified input/output message names, without the leading dot. */
const char *pbw_method_input_type(const pbw_method *method);
const char *pbw_method_output_type(const pbw_method *method);
/* Resolve a method to its input/output descriptors for routing: finds
 * service `service`, then method `method`, then both message types by the
 * method's input/output names. A transport (gRPC, Twirp, plain HTTP) decodes
 * with *in_desc and encodes with *out_desc; the core still does no RPC.
 * NULL for any missing piece, PBW_ERR_INVALID when anything is unknown. */
pbw_status pbw_service_resolve(const pbw_schema *schema, const char *service,
                               const char *method,
                               const pbw_descriptor **in_desc,
                               const pbw_descriptor **out_desc);
/* Well-known-type helpers over the dynamic API. The caller loads the
 * descriptors (google/protobuf/any.proto etc. via a FileDescriptorSet);
 * these only read/write fields 1 and 2, so a hand-built mirror with the
 * same numbers works too. The core never special-cases WKTs elsewhere. */
/* Pack payload (already encoded bytes of the message named by type_url, e.g.
 * "type.googleapis.com/pkg.Msg") into a new google.protobuf.Any tree. */
pbw_status pbw_any_pack(const pbw_descriptor *any_desc, const char *type_url,
                        const void *payload, size_t len, pbw_message **out);
/* type_url (field 1) of an Any tree, or NULL when absent. Owned by the tree. */
const char *pbw_any_type_url(const pbw_message *any_msg);
/* value bytes (field 2) of an Any tree; *len is 0 and NULL returns when
 * absent. Owned by the tree. */
const void *pbw_any_payload(const pbw_message *any_msg, size_t *len);
/* google.protobuf.Timestamp / Duration as milliseconds: field 1 (seconds,
 * int64) and field 2 (nanos, int32); nanos must be within +/-999,999,999.
 * PBW_ERR_INVALID on out-of-range nanos or a missing seconds field,
 * PBW_ERR_LIMIT when seconds overflow int64 milliseconds. */
pbw_status pbw_timestamp_to_millis(const pbw_message *ts, int64_t *out);
pbw_status pbw_duration_to_millis(const pbw_message *dur, int64_t *out);
/* The pack direction: a new Timestamp/Duration tree holding ms. Nanos keep
 * the millisecond remainder, so the value round-trips through *_to_millis. */
pbw_status pbw_timestamp_from_millis(const pbw_descriptor *ts_desc, int64_t ms,
                                     pbw_message **out);
pbw_status pbw_duration_from_millis(const pbw_descriptor *dur_desc, int64_t ms,
                                    pbw_message **out);
/* google.protobuf.FieldMask paths (field 1, repeated string) without
 * materializing the list: count and indexed access, NULL past the end. */
size_t pbw_fieldmask_path_count(const pbw_message *mask);
const char *pbw_fieldmask_path_at(const pbw_message *mask, size_t index);
/* Streaming flags from MethodDescriptorProto; both 0 is unary. */
int pbw_method_client_streaming(const pbw_method *method);
int pbw_method_server_streaming(const pbw_method *method);
/* proto2 default as written in the .proto, or NULL when the field has none. */
const char *pbw_field_default_value(const pbw_field *field);
/* 1 when the field was declared as an extension of its message. */
int pbw_field_is_extension(const pbw_field *field);
uint32_t pbw_field_number(const pbw_field *field);
pbw_type pbw_field_type(const pbw_field *field);
pbw_label pbw_field_label(const pbw_field *field);
int pbw_field_is_packed(const pbw_field *field);
/* 1 when a decoded value of this field must be valid UTF-8, i.e. the reference
 * implementation's rule: a `string` field whose utf8_validation resolves to
 * VERIFY (proto3 and edition 2023+ files by default, proto2 never unless a
 * FeatureSet says otherwise). pbw_decode() rejects such a value with
 * PBW_ERR_INVALID instead of storing bytes the reference would refuse; `bytes`
 * fields and proto2 strings stay unchecked, exactly like the reference. */
int pbw_field_requires_utf8_validation(const pbw_field *field);
/* Structural UTF-8 check: 1 when data is valid UTF-8, 0 for overlong forms,
 * surrogates and values past U+10FFFF. The same check decode() applies to a
 * field whose utf8_validation resolves to VERIFY, and that the text/JSON
 * parsers apply to every string they accept; bindings that validate on their
 * own (a host runtime that decodes strings itself) should use this one so the
 * rule stays single-sourced. */
int pbw_utf8_valid(const uint8_t *data, size_t len);
uint32_t pbw_field_oneof_index(const pbw_field *field);
const pbw_descriptor *pbw_field_message_type(const pbw_field *field);

/* --------------------------------------------------------------- message */

/* A message keeps pointing at its descriptor: the schema must outlive it. The
 * descriptor must be linked (pbw_schema_link() returned PBW_OK), because field
 * lookup relies on the number-sorted field array the link builds. */
pbw_message *pbw_message_new(const pbw_descriptor *desc);
void pbw_message_free(pbw_message *msg);
const pbw_descriptor *pbw_message_descriptor(const pbw_message *msg);
/* Wire length this tree was decoded from, 0 when it was built by hand. A
 * buffer-size hint that needs no invalidation: mutation makes it an
 * underestimate, which the encoder handles by growing. */
size_t pbw_message_wire_size(const pbw_message *msg);
/* Exact number of bytes pbw_encode() writes for this tree: one walk over the
 * present values, no allocation, no bytes written. For callers that must know
 * the length before encoding (a length-prefixed frame, a pre-sized buffer)
 * this costs less than encoding twice. Deterministic encoding produces the
 * same length. PBW_ERR_LIMIT when nesting exceeds PBW_MAX_DEPTH, PBW_ERR_INVALID
 * on a NULL submessage. */
pbw_status pbw_message_encoded_size(const pbw_message *msg, size_t *len);
/* Slack pbw_encode_to() needs past pbw_message_encoded_size(): the writer
 * reserves tag/varint room upfront, so `size + PBW_ENCODE_SLACK` is the
 * smallest cap guaranteed to fit, while `size` alone may be rejected. */
#define PBW_ENCODE_SLACK 12

/* Present value of a field, or NULL. Repeated fields expose a
 * PBW_VALUE_LIST. */
pbw_value *pbw_message_get(const pbw_message *msg, uint32_t number);
/* Same as pbw_message_get() for an already-resolved field of the message's
 * descriptor (NULL when it belongs elsewhere); skips the number lookup for
 * hot reflection loops that cached the pointer. */
pbw_value *pbw_message_get_field(const pbw_message *msg,
                                 const pbw_field *field);
/* Number of fields with a value. */
size_t pbw_message_field_count(const pbw_message *msg);
/* Present fields in field-index order: fills up to cap (fnum, value) pairs
 * and returns how many were written. Lets hot reflection loops visit only
 * what the wire carried instead of probing every schema field. */
size_t pbw_message_present_list(const pbw_message *msg, uint32_t *fnums,
                                const pbw_value **values, size_t cap);
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

/* Unknown fields kept from decoding, as raw wire bytes (each item is its
 * tag followed by its payload), in wire order. Empty when nothing unknown
 * was seen. The bytes stay owned by the message. */
size_t pbw_message_unknown_len(const pbw_message *msg);
const uint8_t *pbw_message_unknown_bytes(const pbw_message *msg);
/* Append raw unknown-field bytes (tag + payload per item, as produced by
 * pbw_message_unknown_bytes). Fails on context-owned read-only messages. */
pbw_status pbw_message_add_unknown_bytes(pbw_message *msg, const void *data,
                                         size_t len);

/* Store a value: appends for repeated fields, replaces otherwise. Takes
 * ownership of value on success only. Values owned by another message (see
 * pbw_message_get()) are rejected with PBW_ERR_INVALID; build a fresh value
 * instead. A message built with pbw_message_new_ctx() only accepts values
 * built with the matching pbw_value_new_*_ctx() constructor (also
 * PBW_ERR_INVALID): the context arena frees by reset, so it cannot own a heap
 * payload. A plain-decoded tree accepts both. */
pbw_status pbw_message_set(pbw_message *msg, uint32_t number, pbw_value *value);
/* Same as pbw_message_set() for an already-resolved field of the message's
 * descriptor (PBW_ERR_INVALID when it belongs elsewhere); skips the number
 * lookup for hot build loops that cached the pointer. */
pbw_status pbw_message_set_field(pbw_message *msg, const pbw_field *field,
                                 pbw_value *value);
/* Create (or return the existing) nested message value for a MESSAGE field. */
pbw_message *pbw_message_set_message(pbw_message *msg, uint32_t number);
/* Build a message tree in a context's arena for pbw_encode_ctx(): steady use
 * stops calling malloc after the first few messages. The tree is mutable
 * until the next pbw_decode_ctx() on the same context seals it; reset the
 * context before each build or decode. Values are built with the
 * pbw_value_new_*_ctx() constructors below so every payload lives in the
 * same arena and dies with the reset; a heap value (pbw_value_new_string(),
 * ...) is rejected with PBW_ERR_INVALID because the reset could not free it. */
pbw_message *pbw_message_new_ctx(pbw_ctx *ctx, const pbw_descriptor *desc);

/* ----------------------------------------------------------------- value */

pbw_value *pbw_value_new_uint(uint64_t bits);
pbw_value *pbw_value_new_int(int64_t value);
pbw_value *pbw_value_new_bool(int value);
pbw_value *pbw_value_new_float(float value);
pbw_value *pbw_value_new_double(double value);
pbw_value *pbw_value_new_bytes(const void *data, size_t len);
pbw_value *pbw_value_new_string(const char *str);
pbw_value *pbw_value_new_message(pbw_message *msg); /* takes ownership of msg */
/* Arena twins of the constructors above, for trees built with
 * pbw_message_new_ctx(): payload bytes are copied into the context so the
 * tree never points at caller memory. NULL data zero-fills the buffer for
 * the caller to fill, read back via pbw_value_get_bytes() (a plain
 * pbw_value_new_bytes(NULL, len) does the same). All scalars share the uint
 * constructor (float/double/int/bool pass their bit pattern, like the plain
 * pbw_value_new_int/bool/float/double do). */
pbw_value *pbw_value_new_uint_ctx(pbw_ctx *ctx, uint64_t bits);
pbw_value *pbw_value_new_bytes_ctx(pbw_ctx *ctx, const void *data, size_t len);
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
/* Same context on caller memory: the handle, its arena and the arena's one
 * chunk all live in mem, so the context never allocates and closing it never
 * frees (nothing is owned). mem must be 8-byte aligned and len must cover the
 * handle plus a chunk header plus room for one allocation; NULL when it is not. An allocation that does not fit returns PBW_ERR_NOMEM from decode and
 * PBW_ERR_LIMIT from pbw_encode_to: the block is the hard cap, so a steady
 * workload never calls malloc. pbw_encode_ctx() needs a growable buffer and
 * reports PBW_ERR_NOMEM here; use pbw_encode_to() instead. */
pbw_ctx *pbw_ctx_new_mem(void *mem, size_t len);
void pbw_ctx_free(pbw_ctx *ctx);
/* Opaque binding slot, e.g. the language binding's per-thread state.
 * Untouched by the core (reads, writes, reset and pooling all preserve
 * it); freed with the context. */
void pbw_ctx_set_user(pbw_ctx *ctx, void *user);
void *pbw_ctx_user(const pbw_ctx *ctx);
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

/* Bounds for untrusted input. Zero fields mean the defaults: no byte cap and
 * PBW_MAX_DEPTH nesting. A max_depth above PBW_MAX_DEPTH is clamped to it. */
typedef struct pbw_limits {
    size_t max_bytes;   /* reject inputs larger than this */
    uint32_t max_depth; /* nesting cap, 0 = PBW_MAX_DEPTH */
} pbw_limits;

/* ------------------------------------------------------------------ wire */

/* The text and JSON mappings define '.' as the decimal separator, but strtod
 * and snprintf follow LC_NUMERIC, which the embedding host may have set to a
 * comma-decimal locale. The printers and parsers therefore translate the
 * locale's separator when it is not '.', so the output stays valid under both.
 * That check reads localeconv(), which the C standard does not make safe
 * against a concurrent setlocale(): set the locale once at startup, before
 * another thread calls into this library. */

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
/* Same with explicit bounds. Like the binary decoder, a max_depth above
 * PBW_MAX_DEPTH clamps to it and a max_bytes of 0 means no byte cap; both are
 * checked before any recursion, so a host on a small thread stack can lower the
 * nesting cap instead of trusting PBW_MAX_DEPTH. */
pbw_status pbw_text_parse_limits(const pbw_descriptor *desc, const char *text,
                                 size_t len, const pbw_limits *limits,
                                 pbw_message **out);

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
 * category). A missing proto2 `required` field fails with PBW_ERR_REQUIRED.
 * The well-known types are recognized by full message name and then require
 * their real shape (`Any` 1=string `type_url`, 2=bytes `value`; Timestamp
 * and Duration 1=int64 `seconds`, 2=int32 `nanos`); a schema that carries
 * only the name is PBW_ERR_INVALID rather than a crash. */
typedef struct pbw_json_parse_options {
    /* Skip unknown field names instead of failing. An `Any` whose `@type`
     * does not resolve stays an error either way, like the reference parser. */
    int ignore_unknown_fields;
} pbw_json_parse_options;
pbw_status pbw_json_parse_opts(const pbw_descriptor *desc, const char *text,
                               size_t len,
                               const pbw_json_parse_options *options,
                               pbw_message **out);
/* Same with default options (unknown fields fail). */
pbw_status pbw_json_parse(const pbw_descriptor *desc, const char *text,
                          size_t len, pbw_message **out);
/* Same with explicit bounds and options (either pointer may be NULL for the
 * defaults). The nesting and byte caps behave like pbw_text_parse_limits();
 * note that an array costs a level too, so a repeated message chain spends two
 * levels per message (the default cap then rejects at 128, not 256). */
pbw_status pbw_json_parse_limits(const pbw_descriptor *desc, const char *text,
                                 size_t len, const pbw_limits *limits,
                                 const pbw_json_parse_options *options,
                                 pbw_message **out);

/* Encode msg; *out is replaced with a malloc'd buffer owned by the caller
 * (pbw_free). *out and *len are 0 on failure. */
pbw_status pbw_encode(const pbw_message *msg, uint8_t **out, size_t *len);
/* Encode into caller memory of `cap` bytes, so a binding can write straight
 * into its own buffer. No allocation happens and nothing is freed; `cap` is
 * the hard limit and the writer over-reserves up to 10 bytes per write, so
 * `pbw_message_encoded_size(msg) + PBW_ENCODE_SLACK` may be needed even when
 * the exact size is known. *len is the bytes written, 0 after any failure.
 * PBW_ERR_LIMIT when the message does not fit. */
pbw_status pbw_encode_to(const pbw_message *msg, void *out, size_t cap,
                         size_t *len);
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

/* Nesting cap the decoder enforces (100 in the reference implementations; 256
 * here for the Erlang binding's 200-deep round-trips, which still rejects
 * conformance test 20000). A decoder that walks wire bytes itself must use the
 * same bound. The cap bounds recursion, not stack: one nesting level costs
 * about 1.4 KB of C stack in a release build and up to 3.8 KB in a debug
 * build (~1 MB under ASan/UBSan), so a thread that parses input up to the cap
 * needs roughly 1 MB free (test_threads pins it to 2 MB; macOS worker threads
 * default to 512 KB). Callers that cannot promise that stack use
 * pbw_limits.max_depth to lower the cap for the binary decoder. */
#define PBW_MAX_DEPTH 256

/* Decode with explicit bounds; identical to pbw_decode() with NULL limits. */
pbw_status pbw_decode_limits(const pbw_descriptor *desc, const void *buf,
                             size_t len, const pbw_limits *limits,
                             pbw_message **out);
pbw_status pbw_decode_ctx_limits(pbw_ctx *ctx, const pbw_descriptor *desc,
                                 const void *buf, size_t len,
                                 const pbw_limits *limits, pbw_message **out);
/* Bounded descriptor load: like pbw_descriptor_set_load(), but rejects
 * inputs larger than limits->max_bytes with PBW_ERR_LIMIT and caps message
 * nesting at limits->max_depth (0 = PBW_MAX_DEPTH, above clamps to it).
 * Descriptors stay trusted input; the bounds only cap resource use from
 * oversized or hostile sets. NULL limits behaves exactly like
 * pbw_descriptor_set_load(). */
pbw_status pbw_descriptor_set_load_limits(const void *data, size_t len,
                                          const pbw_limits *limits,
                                          pbw_schema **out);

/* Encode with map entries sorted by key, so equal messages encode to equal
 * bytes regardless of insertion order. Non-map fields encode exactly as
 * pbw_encode() writes them. */
pbw_status pbw_encode_deterministic(const pbw_message *msg, uint8_t **out,
                                    size_t *len);
/* The caller-buffer twin of pbw_encode_deterministic(), with the same
 * contract as pbw_encode_to(): no allocation, `cap` is the hard limit,
 * PBW_ERR_LIMIT when the message does not fit. */
pbw_status pbw_encode_deterministic_to(const pbw_message *msg, void *out,
                                       size_t cap, size_t *len);

/* ------------------------------------------------------------ visit scan */

/* One scanned field: raw wire content, no allocation, no value tree. */
typedef struct pbw_visit_value {
    /* VARINT/32BIT/64BIT: raw wire bits (varints unfolded: no zigzag or sign
     * extension applied). Unused for the length-delimited shapes below. */
    uint64_t u64;
    /* LEN: the payload bytes (a nested message, a packed run, a string); for
     * START_GROUP the whole field (tag through the matching END_GROUP).
     * Points into buf; NULL for the scalar shapes above. */
    const uint8_t *data;
    /* Payload/raw length; 0 for the scalar shapes. */
    size_t len;
} pbw_visit_value;

/* Scan callback: field is NULL when the number is unknown to the schema.
 * number/wire_type are what the wire carries: a known field may carry another
 * wire type (a packed LEN for a repeated scalar, or a mismatch the reference
 * decoders would keep as an unknown field). LEN data points into buf. Return
 * PBW_OK to continue, anything else aborts the scan with that status. */
typedef pbw_status (*pbw_visit_fn)(void *ud, const pbw_field *field,
                                   uint32_t number, uint32_t wire_type,
                                   const pbw_visit_value *value);

/* Single-level scan of buf without building a message tree: one validating tag
 * pass, one callback per field, no allocation. Nested messages and packed runs
 * arrive as LEN payloads for the caller to recurse into (field->sub plus
 * another pbw_visit); groups arrive as raw bytes. No required/enum checks:
 * values are raw wire bits. fail_off (nullable) is always written: len on
 * success, the failing/aborting field's byte offset otherwise. */
pbw_status pbw_visit(const pbw_descriptor *desc, const void *buf, size_t len,
                     pbw_visit_fn cb, void *ud, size_t *fail_off);
/* Same with explicit bounds; identical to pbw_visit() with NULL limits. Only
 * max_bytes and the group-nesting cap apply: the scan itself never recurses. */
pbw_status pbw_visit_limits(const pbw_descriptor *desc, const void *buf,
                            size_t len, const pbw_limits *limits,
                            pbw_visit_fn cb, void *ud, size_t *fail_off);

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
/* Same framing into caller memory: no allocation, `cap` is the hard limit.
 * The writer over-reserves like pbw_encode_to(), so the guaranteed-fit cap is
 * pbw_message_encoded_size(msg) + its length prefix + PBW_ENCODE_SLACK (the
 * prefix is one byte under 128 bytes, one more per 7 bits above). *len is the
 * whole frame, 0 after any failure. */
pbw_status pbw_encode_delimited_to(const pbw_message *msg, void *out,
                                   size_t cap, size_t *len);

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
/* Decode one length-delimited frame (varint length + message) from buf: the
 * one-shot twin of pbw_encode_delimited(). *consumed is the prefix + frame
 * bytes, so a caller holding concatenated frames advances by it and decodes
 * again. Trailing bytes are left for the caller; a truncated frame fails
 * with PBW_ERR_INVALID (there is no "need more data" one-shot: use
 * pbw_istream for that). */
pbw_status pbw_decode_delimited(const pbw_descriptor *desc, const void *buf,
                                size_t len, pbw_message **out,
                                size_t *consumed);
/* Same with explicit bounds; identical to pbw_decode_delimited() with NULL
 * limits. */
pbw_status pbw_decode_delimited_limits(const pbw_descriptor *desc,
                                       const void *buf, size_t len,
                                       const pbw_limits *limits,
                                       pbw_message **out, size_t *consumed);
/* The pbw_ctx twin: the tree lands in the context's arena (read-only, like
 * pbw_decode_ctx_limits()) instead of one malloc block, so a framed stream
 * decoded in a loop stops allocating. The context is not reset here -- reset
 * it first, as with pbw_decode_ctx(). A declared frame length past
 * limits->max_bytes is PBW_ERR_LIMIT before the body is decoded. */
pbw_status pbw_decode_delimited_ctx_limits(pbw_ctx *ctx,
                                           const pbw_descriptor *desc,
                                           const void *buf, size_t len,
                                           const pbw_limits *limits,
                                           pbw_message **out,
                                           size_t *consumed);
/* Same with default bounds; identical to pbw_decode_delimited_ctx_limits()
 * with NULL limits. */
pbw_status pbw_decode_delimited_ctx(pbw_ctx *ctx, const pbw_descriptor *desc,
                                    const void *buf, size_t len,
                                    pbw_message **out, size_t *consumed);
void pbw_free(void *ptr);
const char *pbw_strerror(pbw_status status);

#ifdef __cplusplus
}
#endif

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC visibility pop
#endif

#endif /* PBWIRE_H */

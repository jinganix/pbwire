/* N-API addon: the C half of the Node.js binding.
 *
 * A message has two representations, and the JS layer picks which one it holds:
 *
 *   tree - a pbw_message, what newMessage()/set() build. Values stay in C, so
 *          encoding never crosses the boundary again and the core writer runs
 *          at core speed.
 *
 *   wire - the bytes decode() was given plus one 12-byte slice per wire field.
 *          The payload is validated once while those slices are recorded, a
 *          value materializes only when it is read, and encode() hands the
 *          retained bytes back, so a parsed message round-trips byte-exactly
 *          (unknown fields, field order and non-canonical encodings included).
 *
 * The wire form deliberately keeps no native state: the bytes are a JS Buffer
 * and the slices a JS Uint32Array, both passed back on every call. Attaching a
 * finalizer to native memory costs N-API ~250 ns per message, which would be
 * more than the whole decode, so only the tree form (owned by a finalizer,
 * because it is mutable and has no JS-visible storage) pays for one.
 *
 * Everything the addon does per field comes from a desc_table: one cached,
 * per-descriptor record of what the schema says (number, type, wire type,
 * label, oneof, closed enum, sub-message) plus a name hash. Building it costs
 * one pass over the descriptor; the hot paths then touch no pbw_* accessor.
 *
 * Failures are thrown JS Errors; no pbw_* status escapes as a crash.
 */
#define NAPI_VERSION 8
#include <node_api.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pbwire.h"

#define NOT_FOUND 0xffffffffu /* field index that no field can have */

/* ------------------------------------------------------------ wire reader */

/* These mirror src/wire.c exactly: a varint is at most 10 bytes (bits above
 * bit 63 of the tenth are dropped), a tag is a varint32 whose fifth byte may
 * overflow, a length is a strict varint32. The lazy decoder accepts what the
 * core accepts and nothing else, which is what keeps it from accepting bytes
 * the tree decoder would reject. src/wire.c is the definition; keep the two in
 * step. */

static int
rb_varint(const uint8_t **pp, const uint8_t *end, uint64_t *value)
{
    const uint8_t *p = *pp;
    uint64_t result;
    unsigned i;

    if (p >= end) {
        return 0;
    }
    result = p[0];
    if (result < 0x80) {
        *pp = p + 1;
        *value = result;
        return 1;
    }
    result = 0;
    for (i = 0; i < 10 && p + i < end; i++) {
        uint8_t byte = p[i];

        result |= (uint64_t)(byte & 0x7f) << (7 * i);
        if ((byte & 0x80) == 0) {
            *pp = p + i + 1;
            *value = result;
            return 1;
        }
    }
    return 0;
}

static int
rb_tag(const uint8_t **pp, const uint8_t *end, uint32_t *tag)
{
    const uint8_t *p = *pp;
    uint32_t result;
    unsigned i;

    if (p >= end) {
        return 0;
    }
    result = p[0];
    if (result < 0x80) {
        *pp = p + 1;
        *tag = result;
        return 1;
    }
    result &= 0x7f;
    for (i = 1; i < 5 && p + i < end; i++) {
        uint32_t byte = p[i];

        result |= (byte & 0x7f) << (7 * i);
        if (byte < 0x80) {
            *pp = p + i + 1;
            *tag = result;
            return 1;
        }
    }
    return 0;
}

static int
rb_len(const uint8_t **pp, const uint8_t *end, size_t *len)
{
    const uint8_t *p = *pp;
    uint32_t result;
    unsigned i;

    if (p >= end) {
        return 0;
    }
    result = p[0];
    if (result < 0x80) {
        *pp = p + 1;
        *len = result;
        return 1;
    }
    result &= 0x7f;
    for (i = 1; i < 5 && p + i < end; i++) {
        uint32_t byte = p[i];

        if (i == 4 && (byte & 0x7f) > 0x0f) {
            return 0;
        }
        result |= (byte & 0x7f) << (7 * i);
        if (byte < 0x80) {
            *pp = p + i + 1;
            *len = (size_t)result;
            return 1;
        }
    }
    return 0;
}

static uint32_t
rb_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint64_t
rb_le64(const uint8_t *p)
{
    return (uint64_t)rb_le32(p) | ((uint64_t)rb_le32(p + 4) << 32);
}

static uint64_t
rb_zigzag(uint64_t value, unsigned width)
{
    uint64_t mask = width == 32 ? 0xffffffffull : ~0ull;
    uint64_t u = value & mask;

    return ((u >> 1) ^ (0ull - (u & 1))) & mask;
}

/* Wire type a field of this type is encoded with (pbw_expected_wire_type is
 * internal). */
static uint32_t
wire_type_of(pbw_type type)
{
    switch (type) {
    case PBW_TYPE_DOUBLE:
    case PBW_TYPE_FIXED64:
    case PBW_TYPE_SFIXED64:
        return 1;
    case PBW_TYPE_FLOAT:
    case PBW_TYPE_FIXED32:
    case PBW_TYPE_SFIXED32:
        return 5;
    case PBW_TYPE_STRING:
    case PBW_TYPE_BYTES:
    case PBW_TYPE_MESSAGE:
        return 2;
    case PBW_TYPE_GROUP:
        return 3;
    default:
        return 0;
    }
}

static int
packable(pbw_type type)
{
    switch (type) {
    case PBW_TYPE_STRING:
    case PBW_TYPE_BYTES:
    case PBW_TYPE_MESSAGE:
    case PBW_TYPE_GROUP:
        return 0;
    default:
        return 1;
    }
}

/* --------------------------------------------------------- descriptor data */

typedef struct desc_table desc_table;
typedef struct schema_handle schema_handle;

typedef struct {
    const pbw_field *field; /* resolved once, so hot loops hand it straight to */
    const char *name;       /* pbw_message_{get,set}_field()                  */
    desc_table *sub;        /* resolved on first use, MESSAGE/GROUP only */
    uint32_t number;
    uint32_t oneof; /* 1-based oneof group, 0 = not in a oneof */
    uint8_t type;
    uint8_t label;
    uint8_t wire;   /* wire type this field is encoded with */
    uint8_t closed; /* proto2 closed enum: unknown numbers leave the field */
    uint8_t utf8;   /* 1: a `string` payload must be valid UTF-8 */
} field_entry;

struct desc_table {
    schema_handle *schema;
    const pbw_descriptor *desc;
    field_entry *fields; /* descriptor field index order */
    uint32_t *by_number; /* field indices, ascending by field number */
    uint32_t *dense;     /* field index by number when the numbers are small */
    uint32_t dense_max;
    uint32_t nfields;
    uint32_t oneof_count; /* oneof groups in this message */
    uint32_t mask;        /* name hash: slot count - 1 */
    uint32_t *slots;      /* name hash: field index + 1, 0 = empty */
    int8_t eager;         /* 1: a required field is reachable; -2: resolving */
    uint8_t leaf;         /* 1: every field is a singular scalar/string/bytes */
};

struct schema_handle {
    pbw_schema *schema;
    desc_table **tables;
    size_t count;
    size_t cap;
};

typedef struct {
    schema_handle *schema;
    desc_table *table;
    pbw_ctx *enc; /* reusable encode buffer for this type */
    const uint8_t *enc_data;
    size_t enc_len;
    int enc_valid; /* the scratch holds enc_len bytes of this type's encoding */
    napi_ref schema_ref;
} desc_handle;

/* A recorded wire field, laid out exactly as three Uint32Array words so the
 * slice list a decode produces can be handed to JS as is. `off` is an absolute
 * offset into the retained bytes: the value itself for a scalar, the payload
 * for a delimited field (string, bytes, nested message, packed run) and the
 * body for a group. An empty packed run is recorded as nothing, so `len == 0`
 * on a packable field always means "a scalar value at off". */
typedef struct {
    uint32_t index; /* field index */
    uint32_t off;
    uint32_t len; /* payload length; 0 for an undelimited value */
} slice_t;

typedef struct {
    slice_t *items;
    uint32_t count;
    uint32_t cap;
    /* Small messages record ~20 slices (matrix3 small tier: 18); keeping
     * them on the stack avoids a malloc/realloc pair per decode. */
    slice_t inline_[32];
} slice_list;

typedef struct {
    desc_handle *desc;
    napi_ref desc_ref; /* keeps the descriptor's schema alive */
    pbw_message *msg;
    int freed;
} message_handle;

/* Every napi_* call is checked: a failure becomes a thrown error, never a
 * return into JS with a stale value. */
#define TRY(call)                                                             \
    do {                                                                      \
        if ((call) != napi_ok) {                                              \
            napi_throw_error(env, NULL, "pbwire: internal napi call failed"); \
            return NULL;                                                      \
        }                                                                     \
    } while (0)

static napi_value
fail_status(napi_env env, const char *what, pbw_status status)
{
    char message[512];

    snprintf(message, sizeof(message), "pbwire: %s: %s", what,
             pbw_strerror(status));
    napi_throw_error(env, NULL, message);
    return NULL;
}

static napi_value
fail_argument(napi_env env, const char *what)
{
    char message[256];

    snprintf(message, sizeof(message), "pbwire: %s", what);
    napi_throw_type_error(env, NULL, message);
    return NULL;
}

static napi_value
fail_memory(napi_env env)
{
    napi_throw_error(env, NULL, "pbwire: out of memory");
    return NULL;
}

static int
get_args(napi_env env, napi_callback_info info, napi_value *argv, size_t count)
{
    size_t argc = count;

    if (napi_get_cb_info(env, info, &argc, argv, NULL, NULL) != napi_ok ||
        argc < count) {
        napi_throw_type_error(env, NULL, "pbwire: wrong number of arguments");
        return 0;
    }
    return 1;
}

static int
unwrap(napi_env env, napi_value value, void **out, const char *what)
{
    if (napi_unwrap(env, value, out) != napi_ok || *out == NULL) {
        fail_argument(env, what);
        return 0;
    }
    return 1;
}

/* Copies a JS string into `buf` when it fits and returns buf, otherwise
 * returns a malloc'd copy with *heap set. Field names take the first branch:
 * they are short, and this keeps the per-call name path allocation-free. */
typedef struct {
    char stack[64];
    size_t len;
    int heap;
} string_buf;

static const char *
string_bytes(napi_env env, napi_value value, string_buf *out)
{
    size_t got = 0;
    char *copy;

    out->heap = 0;
    if (napi_get_value_string_utf8(env, value, out->stack, sizeof(out->stack),
                                   &got) != napi_ok) {
        fail_argument(env, "expected a string");
        return NULL;
    }
    if (got < sizeof(out->stack) - 1) {
        out->len = got;
        return out->stack;
    }
    {
        size_t need = 0;

        if (napi_get_value_string_utf8(env, value, NULL, 0, &need) != napi_ok) {
            fail_argument(env, "expected a string");
            return NULL;
        }
        copy = malloc(need + 1);
        if (copy == NULL) {
            fail_memory(env);
            return NULL;
        }
        if (napi_get_value_string_utf8(env, value, copy, need + 1, &got) !=
            napi_ok) {
            free(copy);
            fail_argument(env, "expected a string");
            return NULL;
        }
        out->len = got;
        out->heap = 1;
        return copy;
    }
}

static void
string_free(string_buf *buf, const char *data)
{
    if (buf->heap) {
        free((void *)data);
    }
}

/* ------------------------------------------------------- descriptor tables */

static desc_table *table_for(schema_handle *schema, const pbw_descriptor *desc);

static uint32_t
name_hash(const char *name)
{
    uint32_t hash = 2166136261u;

    while (*name != '\0') {
        hash ^= (uint8_t)*name++;
        hash *= 16777619u;
    }
    return hash;
}

static uint32_t
table_by_name(const desc_table *table, const char *name)
{
    uint32_t slot;

    if (table->slots == NULL) {
        return NOT_FOUND;
    }
    slot = name_hash(name) & table->mask;
    for (;;) {
        uint32_t entry = table->slots[slot];

        if (entry == 0) {
            return NOT_FOUND;
        }
        if (strcmp(table->fields[entry - 1].name, name) == 0) {
            return entry - 1;
        }
        slot = (slot + 1) & table->mask;
    }
}

/* Field index for a field number, or NOT_FOUND. A schema whose numbers fit a
 * small direct table (the usual case) answers in one load; a sparse one falls
 * back to the binary search over the sorted numbers. */
#define DENSE_LIMIT 4096u

static uint32_t
table_by_number(const desc_table *table, uint32_t number)
{
    uint32_t lo = 0;
    uint32_t hi = table->nfields;

    if (table->dense != NULL) {
        return number <= table->dense_max ? table->dense[number] : NOT_FOUND;
    }

    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        uint32_t index = table->by_number[mid];
        uint32_t found = table->fields[index].number;

        if (found < number) {
            lo = mid + 1;
        } else if (found > number) {
            hi = mid;
        } else {
            return index;
        }
    }
    return NOT_FOUND;
}

/* The sub-message table of a MESSAGE/GROUP field, built on first use. A
 * recursive schema terminates because the table is cached before it descends. */
static desc_table *
table_sub(desc_table *table, field_entry *entry)
{
    if (entry->sub == NULL) {
        const pbw_descriptor *sub = pbw_field_message_type(entry->field);

        if (sub != NULL) {
            entry->sub = table_for(table->schema, sub);
        }
    }
    return entry->sub;
}

static int
by_number_compare(const void *a, const void *b)
{
    uint64_t left = *(const uint64_t *)a;
    uint64_t right = *(const uint64_t *)b;

    return left < right ? -1 : (left > right ? 1 : 0);
}

static desc_table *
table_build(schema_handle *schema, const pbw_descriptor *desc)
{
    desc_table *table = calloc(1, sizeof(*table));
    uint32_t nfields;
    uint64_t *order = NULL;
    uint32_t i;
    uint32_t slot_bits = 3; /* 8 slots minimum */

    if (table == NULL) {
        return NULL;
    }
    table->schema = schema;
    table->desc = desc;
    table->eager = -1;
    nfields = (uint32_t)pbw_descriptor_field_count(desc);
    table->nfields = nfields;
    if (nfields > 0) {
        table->fields = calloc(nfields, sizeof(field_entry));
        table->by_number = calloc(nfields, sizeof(uint32_t));
        order = calloc(nfields, sizeof(uint64_t));
        if (table->fields == NULL || table->by_number == NULL ||
            order == NULL) {
            free(order);
            free(table->dense);
            free(table->by_number);
            free(table->fields);
            free(table);
            return NULL;
        }
    }
    for (i = 0; i < nfields; i++) {
        const pbw_field *field = pbw_descriptor_field_at(desc, i);
        field_entry *entry = &table->fields[i];

        if (field == NULL) {
            free(order);
            free(table->dense);
            free(table->by_number);
            free(table->fields);
            free(table);
            return NULL;
        }
        entry->field = field;
        entry->name = pbw_field_name(field);
        entry->number = pbw_field_number(field);
        entry->oneof = pbw_field_oneof_index(field);
        if (entry->oneof > table->oneof_count) {
            table->oneof_count = entry->oneof;
        }
        entry->type = (uint8_t)pbw_field_type(field);
        entry->label = (uint8_t)pbw_field_label(field);
        entry->wire = (uint8_t)wire_type_of((pbw_type)entry->type);
        entry->utf8 = (uint8_t)pbw_field_requires_utf8_validation(field);
        if (entry->type == PBW_TYPE_ENUM) {
            const pbw_descriptor *enum_desc = pbw_field_message_type(field);

            entry->closed =
                enum_desc != NULL && !pbw_descriptor_is_proto3(enum_desc);
        }
        order[i] = ((uint64_t)entry->number << 32) | i;
    }
    if (nfields > 0) {
        qsort(order, nfields, sizeof(*order), by_number_compare);
        for (i = 0; i < nfields; i++) {
            table->by_number[i] = (uint32_t)(order[i] & 0xffffffffu);
        }
        free(order);
        table->dense_max = table->fields[table->by_number[nfields - 1]].number;
        if (table->dense_max < DENSE_LIMIT) {
            table->dense = malloc(((size_t)table->dense_max + 1) *
                                  sizeof(*table->dense));
            if (table->dense == NULL) {
                free(table->by_number);
                free(table->fields);
                free(table);
                return NULL;
            }
            for (i = 0; i <= table->dense_max; i++) {
                table->dense[i] = NOT_FOUND;
            }
            for (i = 0; i < nfields; i++) {
                table->dense[table->fields[i].number] = i;
            }
        }
    }
    while (slot_bits < 6 || (1u << slot_bits) < nfields * 2) {
        slot_bits++;
    }
    table->mask = (1u << slot_bits) - 1;
    table->slots = calloc((size_t)table->mask + 1, sizeof(uint32_t));
    if (table->slots == NULL) {
        free(table->dense);
        free(table->by_number);
        free(table->fields);
        free(table);
        return NULL;
    }
    for (i = 0; i < nfields; i++) {
        uint32_t slot = name_hash(table->fields[i].name) & table->mask;

        while (table->slots[slot] != 0) {
            slot = (slot + 1) & table->mask;
        }
        table->slots[slot] = i + 1;
    }
    /* A leaf table holds only singular scalars, strings and bytes: no
     * repeated field, no oneof member, no sub-message. Such a message
     * materializes in one tag scan with no slice list and no per-level
     * scratch (see wire_leaf_to_js). Eager tables never reach the wire
     * path, so `required` cannot appear here. */
    table->leaf = nfields > 0;
    for (i = 0; i < nfields; i++) {
        field_entry *entry = &table->fields[i];

        if (entry->label == PBW_LABEL_REPEATED || entry->oneof != 0 ||
            entry->type == PBW_TYPE_MESSAGE ||
            entry->type == PBW_TYPE_GROUP) {
            table->leaf = 0;
            break;
        }
    }
    return table;
}

static desc_table *
table_for(schema_handle *schema, const pbw_descriptor *desc)
{
    size_t i;
    desc_table *table;

    for (i = 0; i < schema->count; i++) {
        if (schema->tables[i]->desc == desc) {
            return schema->tables[i];
        }
    }
    table = table_build(schema, desc);
    if (table == NULL) {
        return NULL;
    }
    if (schema->count == schema->cap) {
        size_t cap = schema->cap == 0 ? 8 : schema->cap * 2;
        desc_table **grown =
            realloc(schema->tables, cap * sizeof(desc_table *));

        if (grown == NULL) {
            free(table->slots);
            free(table->dense);
            free(table->by_number);
            free(table->fields);
            free(table);
            return NULL;
        }
        schema->tables = grown;
        schema->cap = cap;
    }
    schema->tables[schema->count++] = table;
    return table;
}

/* 1 when this descriptor or any message reachable from it declares a proto2
 * `required` field. Such a schema decodes eagerly: PBW_ERR_REQUIRED is a
 * property of the merged message (a field can arrive split across repeated
 * occurrences of one submessage), which is exactly what the tree decoder
 * checks after parsing. A cycle contributes nothing new, hence the
 * "resolving" state instead of a cycle error. */
static int
table_is_eager(desc_table *table)
{
    uint32_t i;

    if (table->eager >= 0) {
        return table->eager;
    }
    if (table->eager == -2) {
        return 0;
    }
    table->eager = -2;
    for (i = 0; i < table->nfields; i++) {
        field_entry *entry = &table->fields[i];

        if (entry->label == PBW_LABEL_REQUIRED) {
            table->eager = 1;
            return 1;
        }
        if (entry->type == PBW_TYPE_MESSAGE || entry->type == PBW_TYPE_GROUP) {
            desc_table *sub = table_sub(table, entry);

            if (sub != NULL && table_is_eager(sub)) {
                table->eager = 1;
                return 1;
            }
        }
    }
    table->eager = 0;
    return 0;
}

/* ------------------------------------------------------------ pbw -> JS */

static napi_value message_to_js(napi_env env, const pbw_message *msg);

/* The canonical bit pattern of one wire scalar: the form pbw_value stores, so
 * that both representations hand JS the same number. */
static uint64_t
canonical_bits(pbw_type type, uint64_t raw)
{
    switch (type) {
    case PBW_TYPE_INT32:
    case PBW_TYPE_ENUM:
        return (uint64_t)(int64_t)(int32_t)raw;
    case PBW_TYPE_UINT32:
        return (uint32_t)raw;
    case PBW_TYPE_SINT32:
        return (uint64_t)(int64_t)(int32_t)rb_zigzag(raw, 32);
    case PBW_TYPE_SINT64:
        return rb_zigzag(raw, 64);
    case PBW_TYPE_BOOL:
        return raw != 0 ? 1u : 0u;
    default:
        return raw;
    }
}

static napi_value
scalar_to_js(napi_env env, pbw_type type, uint64_t bits)
{
    napi_value out = NULL;
    double number;
    float single;
    uint32_t low;

    switch (type) {
    case PBW_TYPE_DOUBLE:
        memcpy(&number, &bits, sizeof(number));
        napi_create_double(env, number, &out);
        break;
    case PBW_TYPE_FLOAT:
        low = (uint32_t)bits;
        memcpy(&single, &low, sizeof(single));
        napi_create_double(env, (double)single, &out);
        break;
    case PBW_TYPE_BOOL:
        napi_get_boolean(env, bits != 0, &out);
        break;
    case PBW_TYPE_INT32:
    case PBW_TYPE_SINT32:
    case PBW_TYPE_SFIXED32:
    case PBW_TYPE_ENUM:
        napi_create_int32(env, (int32_t)bits, &out);
        break;
    case PBW_TYPE_UINT32:
    case PBW_TYPE_FIXED32:
        napi_create_uint32(env, (uint32_t)bits, &out);
        break;
    case PBW_TYPE_INT64:
    case PBW_TYPE_SINT64:
    case PBW_TYPE_SFIXED64:
        /* Exact: a JS number cannot hold every 64-bit integer. */
        napi_create_bigint_int64(env, (int64_t)bits, &out);
        break;
    default: /* PBW_TYPE_UINT64, PBW_TYPE_FIXED64 */
        napi_create_bigint_uint64(env, bits, &out);
        break;
    }
    return out;
}

static napi_value
bytes_to_js(napi_env env, pbw_type type, const void *data, size_t len)
{
    napi_value out = NULL;

    if (type == PBW_TYPE_STRING) {
        napi_create_string_utf8(env, (const char *)data, len, &out);
    } else {
        napi_create_buffer_copy(env, len, data, NULL, &out);
    }
    return out;
}

static napi_value
value_to_js(napi_env env, pbw_type type, const pbw_value *value)
{
    napi_value out = NULL;

    switch (pbw_value_get_kind(value)) {
    case PBW_VALUE_LIST: {
        size_t count = pbw_value_count(value);
        size_t i;

        TRY(napi_create_array_with_length(env, count, &out));
        for (i = 0; i < count; i++) {
            napi_value item = value_to_js(env, type, pbw_value_at(value, i));

            if (item == NULL) {
                return NULL;
            }
            TRY(napi_set_element(env, out, (uint32_t)i, item));
        }
        return out;
    }
    case PBW_VALUE_MESSAGE:
        return message_to_js(env, pbw_value_get_message(value));
    case PBW_VALUE_BYTES: {
        size_t len = 0;
        const void *bytes = pbw_value_get_bytes(value, &len);

        return bytes_to_js(env, type, bytes, len);
    }
    case PBW_VALUE_UINT:
    default:
        return scalar_to_js(env, type, pbw_value_get_uint(value));
    }
}

static napi_value
message_to_js(napi_env env, const pbw_message *msg)
{
    napi_value out;
    size_t index;

    TRY(napi_create_object(env, &out));
    for (index = 0;; index++) {
        pbw_value *value = NULL;
        const pbw_field *field = pbw_message_field_at(msg, index, &value);
        napi_value item;

        if (field == NULL) {
            return out;
        }
        item = value_to_js(env, pbw_field_type(field), value);
        if (item == NULL) {
            return NULL;
        }
        TRY(napi_set_named_property(env, out, pbw_field_name(field), item));
    }
}

/* ------------------------------------------------------------ JS -> pbw */

/* One error shape for a bad field value: the field name comes first, so a
 * caller can tell which of a message's fields was refused. */
static void
fail_field(napi_env env, const char *field, const char *what)
{
    char message[256];

    snprintf(message, sizeof(message), "pbwire: %s: %s", field, what);
    napi_throw_type_error(env, NULL, message);
}

static int js_to_message(napi_env env, pbw_message *msg, desc_table *table,
                         napi_value object, uint32_t *seen, pbw_ctx *ctx);

/* A 64-bit field's value: BigInt or a decimal string, both exact. Anything
 * else (a fractional number, an out-of-range or non-decimal string, a
 * negative value for an unsigned field) is refused, never truncated. */
static int
js_int64(napi_env env, napi_value value, int is_unsigned, uint64_t *out)
{
    napi_valuetype kind;

    if (napi_typeof(env, value, &kind) != napi_ok) {
        return 0;
    }
    if (kind == napi_bigint) {
        bool lossless = true;

        if (is_unsigned) {
            if (napi_get_value_bigint_uint64(env, value, out, &lossless) !=
                    napi_ok ||
                !lossless) {
                return 0;
            }
        } else {
            int64_t bits = 0;

            if (napi_get_value_bigint_int64(env, value, &bits, &lossless) !=
                    napi_ok ||
                !lossless) {
                return 0;
            }
            *out = (uint64_t)bits; /* two's complement: the raw bit pattern */
        }
        return 1;
    }
    if (kind == napi_number) {
        double number;

        /* A JS number is exact only through +-2^53; outside it (or when it is
         * not an integer) converting would silently change the value. */
        if (napi_get_value_double(env, value, &number) != napi_ok ||
            !(number >= -9007199254740992.0 && number <= 9007199254740992.0) ||
            (double)(int64_t)number != number) {
            return 0;
        }
        if (is_unsigned && number < 0) {
            return 0;
        }
        *out = (uint64_t)(int64_t)number;
        return 1;
    }
    if (kind == napi_string) {
        string_buf buf;
        const char *text = string_bytes(env, value, &buf);
        uint64_t limit;
        uint64_t mag = 0;
        size_t i = 0;
        int neg = 0;
        int ok = 1;

        if (text == NULL) {
            return 0;
        }
        if (i < buf.len && text[i] == '-') {
            neg = 1;
            i++;
        }
        if (neg && is_unsigned) {
            ok = 0;
        }
        if (i == buf.len) {
            ok = 0; /* "" and "-" are not numbers */
        }
        limit = is_unsigned
                    ? UINT64_MAX
                    : (neg ? 9223372036854775808ull : 9223372036854775807ull);
        for (; ok && i < buf.len; i++) {
            unsigned digit = (unsigned)(text[i] - '0');

            if (text[i] < '0' || text[i] > '9' ||
                mag > (limit - digit) / 10) {
                ok = 0;
                break;
            }
            mag = mag * 10 + digit;
        }
        if (ok) {
            *out = neg ? (uint64_t)0 - mag : mag;
        }
        string_free(&buf, text);
        return ok;
    }
    return 0;
}

/* A value in the caller's arena when one is given, on the heap otherwise.
 * The arena path is what makes a whole object walk one allocation: the tree
 * and every scalar die with the next pbw_ctx_reset(). */
static pbw_value *
value_new_uint(pbw_ctx *ctx, uint64_t bits)
{
    return ctx != NULL ? pbw_value_new_uint_ctx(ctx, bits)
                       : pbw_value_new_uint(bits);
}

static pbw_value *
value_new_bytes(pbw_ctx *ctx, const void *data, size_t len)
{
    return ctx != NULL ? pbw_value_new_bytes_ctx(ctx, data, len)
                       : pbw_value_new_bytes(data, len);
}

static pbw_value *
value_from_js(napi_env env, pbw_type type, napi_value value,
              const char *field, pbw_ctx *ctx)
{
    pbw_value *out = NULL;
    bool flag = false;

    switch (type) {
    case PBW_TYPE_DOUBLE: {
        double number;

        if (napi_get_value_double(env, value, &number) == napi_ok) {
            uint64_t bits;

            memcpy(&bits, &number, sizeof(bits));
            out = value_new_uint(ctx, bits);
        }
        break;
    }
    case PBW_TYPE_FLOAT: {
        double number;

        if (napi_get_value_double(env, value, &number) == napi_ok) {
            float single = (float)number;
            uint32_t low;

            memcpy(&low, &single, sizeof(low));
            out = value_new_uint(ctx, low);
        }
        break;
    }
    case PBW_TYPE_BOOL:
        if (napi_get_value_bool(env, value, &flag) == napi_ok) {
            out = value_new_uint(ctx, flag ? 1 : 0);
        }
        break;
    case PBW_TYPE_INT32:
    case PBW_TYPE_SINT32:
    case PBW_TYPE_SFIXED32:
    case PBW_TYPE_ENUM: {
        int32_t number;

        if (napi_get_value_int32(env, value, &number) == napi_ok) {
            out = value_new_uint(ctx, (uint64_t)(int64_t)number);
        }
        break;
    }
    case PBW_TYPE_UINT32:
    case PBW_TYPE_FIXED32: {
        uint32_t number;

        if (napi_get_value_uint32(env, value, &number) == napi_ok) {
            out = value_new_uint(ctx, number);
        }
        break;
    }
    case PBW_TYPE_STRING: {
        string_buf buf;
        napi_valuetype kind;
        const char *data;

        if (napi_typeof(env, value, &kind) != napi_ok ||
            kind != napi_string) {
            break; /* "value does not match the field type" below */
        }
        data = string_bytes(env, value, &buf);
        if (data == NULL) {
            return NULL;
        }
        out = value_new_bytes(ctx, data, buf.len);
        string_free(&buf, data);
        break;
    }
    case PBW_TYPE_BYTES: {
        void *data = NULL;
        size_t len = 0;

        if (napi_get_buffer_info(env, value, &data, &len) == napi_ok) {
            out = value_new_bytes(ctx, data, len);
        }
        break;
    }
    default: { /* 64-bit integers: BigInt or decimal string, exact */
        uint64_t bits;
        int is_unsigned =
            (type == PBW_TYPE_UINT64 || type == PBW_TYPE_FIXED64);

        if (js_int64(env, value, is_unsigned, &bits)) {
            out = value_new_uint(ctx, bits);
        }
        break;
    }
    }
    if (out == NULL) {
        fail_field(env, field, "value does not match the field type");
    }
    return out;
}

/* One value for one field: nested messages and repeated elements both land
 * here. */
static int
set_element(napi_env env, pbw_message *msg, desc_table *table,
            field_entry *entry, napi_value value, pbw_ctx *ctx)
{
    if (entry->type == PBW_TYPE_MESSAGE || entry->type == PBW_TYPE_GROUP) {
        pbw_message *child;

        if (entry->label != PBW_LABEL_REPEATED) {
            /* set() replaces a field: no merge into the previous submessage. */
            pbw_message_clear(msg, entry->number);
        }
        child = pbw_message_set_message(msg, entry->number);
        if (child == NULL) {
            fail_argument(env, "cannot set the nested message field");
            return 0;
        }
        return js_to_message(env, child, table_sub(table, entry), value, NULL,
                             ctx);
    }
    {
        pbw_value *item = value_from_js(env, (pbw_type)entry->type, value,
                                        entry->name, ctx);
        pbw_status status;

        if (item == NULL) {
            return 0;
        }
        status = pbw_message_set_field(msg, entry->field, item);
        if (status != PBW_OK) {
            pbw_value_free(item); /* ownership transfers only on success */
            fail_status(env, entry->name, status);
            return 0;
        }
    }
    return 1;
}

static int
set_field(napi_env env, pbw_message *msg, desc_table *table,
          field_entry *entry, napi_value value, pbw_ctx *ctx)
{
    napi_valuetype kind;
    bool is_array = false;
    uint32_t count = 0;
    uint32_t i;

    if (napi_typeof(env, value, &kind) != napi_ok) {
        return 0;
    }
    if (kind == napi_undefined || kind == napi_null) {
        pbw_message_clear(msg, entry->number);
        return 1;
    }
    if (entry->label != PBW_LABEL_REPEATED) {
        return set_element(env, msg, table, entry, value, ctx);
    }
    if (napi_is_array(env, value, &is_array) != napi_ok || !is_array) {
        fail_field(env, entry->name, "a repeated field takes an array");
        return 0;
    }
    if (napi_get_array_length(env, value, &count) != napi_ok) {
        return 0;
    }
    /* set() replaces: an array write does not append to what was there. */
    pbw_message_clear(msg, entry->number);
    for (i = 0; i < count; i++) {
        napi_value item;

        if (napi_get_element(env, value, i, &item) != napi_ok) {
            return 0;
        }
        if (!set_element(env, msg, table, entry, item, ctx)) {
            return 0;
        }
    }
    return 1;
}

/* Rejects a property the schema does not have. Only called when the count of
 * object properties and the count of fields that landed differ, so the walk
 * above stays one napi_get_named_property per schema field and never pays for
 * enumerating an object it understood completely. */
static int
js_reject_unknown(napi_env env, napi_value object, desc_table *table)
{
    napi_value names;
    uint32_t count = 0;
    uint32_t i;

    if (napi_get_all_property_names(
            env, object, napi_key_own_only,
            (napi_key_filter)(napi_key_enumerable | napi_key_skip_symbols),
            napi_key_numbers_to_strings, &names) != napi_ok ||
        napi_get_array_length(env, names, &count) != napi_ok) {
        return 0;
    }
    for (i = 0; i < count; i++) {
        string_buf buf;
        napi_value key;
        const char *name;

        if (napi_get_element(env, names, i, &key) != napi_ok) {
            return 0;
        }
        name = string_bytes(env, key, &buf);
        if (name == NULL) {
            return 0;
        }
        if (table_by_name(table, name) == NOT_FOUND) {
            char message[512];

            snprintf(message, sizeof(message), "pbwire: %s has no field %s",
                     pbw_descriptor_name(table->desc), name);
            napi_throw_error(env, NULL, message);
            string_free(&buf, name);
            return 0;
        }
        string_free(&buf, name);
    }
    return 1;
}

/* Walks the schema's field table in order, one napi_get_named_property per
 * field: the addon reads the host's object directly, with no JS round trip and
 * no name hashing on the common path. An `undefined` property is absent (so a
 * proto3 optional field is set only when its property is present); nested
 * messages and repeated elements recurse here, inside C. `*seen`, when given,
 * counts the properties that landed, so the caller can compare it with the
 * object's own property count. */
static int
js_to_message(napi_env env, pbw_message *msg, desc_table *table,
              napi_value object, uint32_t *seen, pbw_ctx *ctx)
{
    napi_valuetype kind;
    bool is_array = false;
    napi_value names;
    uint32_t count = 0;
    uint32_t i;
    uint32_t found = 0;

    if (napi_typeof(env, object, &kind) != napi_ok) {
        return 0;
    }
    if (napi_is_array(env, object, &is_array) != napi_ok ||
        kind != napi_object || is_array) {
        fail_argument(env, "a nested message takes an object");
        return 0;
    }
    for (i = 0; i < table->nfields; i++) {
        field_entry *entry = &table->fields[i];
        napi_value value;
        napi_valuetype vkind;

        if (napi_get_named_property(env, object, entry->name, &value) !=
            napi_ok) {
            return 0; /* pending exception from a getter */
        }
        if (napi_typeof(env, value, &vkind) != napi_ok) {
            return 0;
        }
        if (vkind == napi_undefined) {
            continue;
        }
        if (!set_field(env, msg, table, entry, value, ctx)) {
            return 0;
        }
        found++;
    }
    if (napi_get_all_property_names(
            env, object, napi_key_own_only,
            (napi_key_filter)(napi_key_enumerable | napi_key_skip_symbols),
            napi_key_numbers_to_strings, &names) != napi_ok) {
        return 0;
    }
    if (napi_get_array_length(env, names, &count) != napi_ok) {
        return 0;
    }
    if (count != found && !js_reject_unknown(env, object, table)) {
        return 0;
    }
    if (seen != NULL) {
        *seen = found;
    }
    return 1;
}

/* -------------------------------------------------------- wire -> slices */

typedef enum {
    SCAN_OK = 0,
    SCAN_INVALID, /* malformed wire data */
    SCAN_LIMIT,   /* nesting past PBW_MAX_DEPTH */
    SCAN_NOMEM    /* slice list could not grow */
} scan_status;

static int
slices_record(slice_list *list, uint32_t index, uint32_t off, uint32_t len)
{
    if (list->count == list->cap) {
        uint32_t cap;
        slice_t *grown;

        if (list->cap > UINT32_MAX / 2) {
            /* Doubling would wrap the count to zero and the next store would
             * run past a zero-sized block. */
            return 0;
        }
        cap = list->cap == 0 ? 32 : list->cap * 2;

        if (list->items == list->inline_) {
            grown = malloc((size_t)cap * sizeof(*grown));
            if (grown != NULL) {
                memcpy(grown, list->inline_,
                       (size_t)list->count * sizeof(*grown));
            }
        } else {
            grown = realloc(list->items, (size_t)cap * sizeof(*grown));
        }
        if (grown == NULL) {
            return 0;
        }
        list->items = grown;
        list->cap = cap;
    }
    list->items[list->count].index = index;
    list->items[list->count].off = off;
    list->items[list->count].len = len;
    list->count++;
    return 1;
}

static void
slices_release(slice_list *list)
{
    if (list->items != NULL && list->items != list->inline_) {
        free(list->items);
    }
    list->items = NULL;
    list->count = 0;
    list->cap = 0;
}

static scan_status skip_group(const uint8_t **pp, const uint8_t *end,
                              uint32_t number, uint32_t depth);

static scan_status
skip_value(const uint8_t **pp, const uint8_t *end, uint32_t number,
           uint32_t wire_type, uint32_t depth)
{
    uint64_t scratch;
    size_t len;

    switch (wire_type) {
    case 0:
        return rb_varint(pp, end, &scratch) ? SCAN_OK : SCAN_INVALID;
    case 1:
        if ((size_t)(end - *pp) < 8) {
            return SCAN_INVALID;
        }
        *pp += 8;
        return SCAN_OK;
    case 2:
        if (!rb_len(pp, end, &len) || len > (size_t)(end - *pp)) {
            return SCAN_INVALID;
        }
        *pp += len;
        return SCAN_OK;
    case 3:
        return skip_group(pp, end, number, depth);
    case 5:
        if ((size_t)(end - *pp) < 4) {
            return SCAN_INVALID;
        }
        *pp += 4;
        return SCAN_OK;
    default:
        return SCAN_INVALID;
    }
}

/* Skips a legacy group up to and including its matching END_GROUP tag. */
static scan_status
skip_group(const uint8_t **pp, const uint8_t *end, uint32_t number,
           uint32_t depth)
{
    if (depth >= PBW_MAX_DEPTH) {
        return SCAN_LIMIT;
    }
    for (;;) {
        uint32_t tag;
        uint32_t wire_type;

        if (!rb_tag(pp, end, &tag)) {
            return SCAN_INVALID;
        }
        wire_type = tag & 7;
        if ((tag >> 3) == 0) {
            return SCAN_INVALID;
        }
        if (wire_type == 4) {
            return (tag >> 3) == number ? SCAN_OK : SCAN_INVALID;
        }
        {
            scan_status status =
                skip_value(pp, end, tag >> 3, wire_type, depth + 1);

            if (status != SCAN_OK) {
                return status;
            }
        }
    }
}

/* A packed run must hold a whole number of complete values. */
static int
packed_run_ok(pbw_type type, const uint8_t *data, size_t len)
{
    const uint8_t *p = data;
    const uint8_t *end = data + len;
    uint64_t scratch;

    switch (type) {
    case PBW_TYPE_FLOAT:
    case PBW_TYPE_FIXED32:
    case PBW_TYPE_SFIXED32:
        return len % 4 == 0;
    case PBW_TYPE_DOUBLE:
    case PBW_TYPE_FIXED64:
    case PBW_TYPE_SFIXED64:
        return len % 8 == 0;
    default:
        while (p < end) {
            if (!rb_varint(&p, end, &scratch)) {
                return 0;
            }
        }
        return 1;
    }
}

/* One tag scan of one message range; with `out` set it also records a slice
 * per known wire field. A non-zero `group_number` makes the range a legacy
 * group body, which ends at the matching END_GROUP tag instead of the buffer
 * end, and reports through `consumed` how much of the range that took (the
 * closing tag included). */
static scan_status
scan_range(desc_table *table, const uint8_t *base, uint32_t off, uint32_t len,
           uint32_t depth, uint32_t group_number, slice_list *out,
           size_t *consumed)
{
    const uint8_t *p = base + off;
    const uint8_t *end = p + len;

    while (p < end) {
        uint32_t tag;
        uint32_t number;
        uint32_t wire_type;
        uint32_t index;
        field_entry *entry;
        scan_status status;
        const uint8_t *tag_at = p;

        if (!rb_tag(&p, end, &tag)) {
            return SCAN_INVALID;
        }
        number = tag >> 3;
        wire_type = tag & 7;
        if (number == 0 || wire_type > 5) {
            return SCAN_INVALID;
        }
        if (wire_type == 4) {
            /* END_GROUP closes only the group that started it. `consumed`
             * stops before the tag itself: the caller slices the body, and the
             * tag may be written as an overlong varint. */
            if (group_number != 0 && number == group_number) {
                if (consumed != NULL) {
                    *consumed = (size_t)(tag_at - (base + off));
                }
                return SCAN_OK;
            }
            return SCAN_INVALID;
        }
        index = table_by_number(table, number);
        if (index == NOT_FOUND) {
            status = skip_value(&p, end, number, wire_type, depth);
            if (status != SCAN_OK) {
                return status;
            }
            continue;
        }
        entry = &table->fields[index];
        if (wire_type == entry->wire) {
            if (entry->type == PBW_TYPE_MESSAGE) {
                uint32_t payload;
                size_t payload_len;

                if (depth >= PBW_MAX_DEPTH) {
                    return SCAN_LIMIT;
                }
                if (!rb_len(&p, end, &payload_len) ||
                    payload_len > (size_t)(end - p)) {
                    return SCAN_INVALID;
                }
                payload = (uint32_t)(p - base);
                status = scan_range(table_sub(table, entry), base, payload,
                                    (uint32_t)payload_len, depth + 1, 0, NULL,
                                    NULL);
                if (status != SCAN_OK) {
                    return status;
                }
                p += payload_len;
                if (out != NULL &&
                    !slices_record(out, index, payload, (uint32_t)payload_len)) {
                    return SCAN_NOMEM;
                }
                continue;
            }
            if (entry->type == PBW_TYPE_GROUP) {
                uint32_t body = (uint32_t)(p - base);
                size_t body_len = 0;

                if (depth >= PBW_MAX_DEPTH) {
                    return SCAN_LIMIT;
                }
                status = scan_range(table_sub(table, entry), base, body,
                                    (uint32_t)(end - p), depth + 1, number,
                                    NULL, &body_len);
                if (status != SCAN_OK) {
                    return status;
                }
                {
                    /* The slice is the group body (the END_GROUP tag is not
                     * part of it), but the scan must resume after the tag --
                     * which the writer may have spelled with an overlong
                     * varint, so its width is read from the bytes. */
                    const uint8_t *tagp = base + body + body_len;
                    size_t ctag = 1;

                    while ((tagp[ctag - 1] & 0x80) != 0 && tagp + ctag < end) {
                        ctag++;
                    }
                    p = tagp + ctag;
                }
                if (out != NULL &&
                    !slices_record(out, index, body, (uint32_t)body_len)) {
                    return SCAN_NOMEM;
                }
                continue;
            }
            if (entry->type == PBW_TYPE_STRING ||
                entry->type == PBW_TYPE_BYTES) {
                uint32_t payload;
                size_t payload_len;

                if (!rb_len(&p, end, &payload_len) ||
                    payload_len > (size_t)(end - p)) {
                    return SCAN_INVALID;
                }
                /* A proto3/editions `string` must be valid UTF-8, exactly as
                 * the tree decoder requires: the lazy view and the tree agree
                 * on which messages exist. */
                if (entry->utf8 &&
                    !pbw_utf8_valid(p, (size_t)payload_len)) {
                    return SCAN_INVALID;
                }
                payload = (uint32_t)(p - base);
                p += payload_len;
                if (out != NULL &&
                    !slices_record(out, index, payload,
                                   (uint32_t)payload_len)) {
                    return SCAN_NOMEM;
                }
                continue;
            }
            {
                uint32_t value_off = (uint32_t)(p - base);

                status = skip_value(&p, end, number, wire_type, depth);
                if (status != SCAN_OK) {
                    return status;
                }
                if (out != NULL && !slices_record(out, index, value_off, 0)) {
                    return SCAN_NOMEM;
                }
                continue;
            }
        }
        if (wire_type == 2 && entry->label == PBW_LABEL_REPEATED &&
            packable((pbw_type)entry->type)) {
            uint32_t payload;
            size_t payload_len;

            if (!rb_len(&p, end, &payload_len) ||
                payload_len > (size_t)(end - p)) {
                return SCAN_INVALID;
            }
            payload = (uint32_t)(p - base);
            if (!packed_run_ok((pbw_type)entry->type, base + payload,
                               payload_len)) {
                return SCAN_INVALID;
            }
            p += payload_len;
            /* An empty run carries no value: the field stays absent, exactly
             * as the tree decoder leaves it. Not recording it also keeps
             * `len == 0` meaning "scalar" for a packable field. */
            if (out != NULL && payload_len > 0 &&
                !slices_record(out, index, payload, (uint32_t)payload_len)) {
                return SCAN_NOMEM;
            }
            continue;
        }
        /* A known field with an unexpected wire type is unknown data: the
         * reference parsers keep it as raw bytes, so the scan skips it. */
        status = skip_value(&p, end, number, wire_type, depth);
        if (status != SCAN_OK) {
            return status;
        }
    }
    if (group_number != 0) {
        return SCAN_INVALID; /* END_GROUP never came */
    }
    if (consumed != NULL) {
        *consumed = (size_t)(p - (base + off));
    }
    return SCAN_OK;
}

/* ------------------------------------------------------------ wire -> JS */

/* A packable field's slice is a packed run when it carries a payload. */
static int
slice_is_packed(const field_entry *entry, const slice_t *slice)
{
    return entry->label == PBW_LABEL_REPEATED &&
           packable((pbw_type)entry->type) && slice->len != 0;
}

/* Reads one scalar value at a validated offset. */
static int
read_value(const uint8_t **pp, const uint8_t *end, pbw_type type,
           uint64_t *out)
{
    uint64_t raw;

    switch (type) {
    case PBW_TYPE_DOUBLE:
    case PBW_TYPE_FIXED64:
    case PBW_TYPE_SFIXED64:
        if ((size_t)(end - *pp) < 8) {
            return 0;
        }
        *out = rb_le64(*pp);
        *pp += 8;
        return 1;
    case PBW_TYPE_FLOAT:
    case PBW_TYPE_FIXED32:
    case PBW_TYPE_SFIXED32:
        if ((size_t)(end - *pp) < 4) {
            return 0;
        }
        *out = rb_le32(*pp);
        *pp += 4;
        return 1;
    default:
        if (!rb_varint(pp, end, &raw)) {
            return 0;
        }
        *out = canonical_bits(type, raw);
        return 1;
    }
}

/* A proto2 closed enum keeps only the numbers its declaration lists; the
 * reference parsers move the others to the unknown fields, which for a
 * value-materializing reader means the field does not take that value. */
static int
enum_known(field_entry *entry, uint64_t bits)
{
    const pbw_descriptor *enum_desc;

    if (!entry->closed) {
        return 1;
    }
    enum_desc = pbw_field_message_type(entry->field);
    return enum_desc != NULL &&
           pbw_enum_find_name(enum_desc, (int32_t)bits, NULL);
}

static int
slice_known(field_entry *entry, const uint8_t *base, uint32_t wire_len,
            const slice_t *slice)
{
    const uint8_t *p;
    uint64_t bits;

    if (!entry->closed || slice->len != 0) {
        return 1; /* not a closed enum, or a packed run (filtered per item) */
    }
    p = base + slice->off;
    if (!read_value(&p, base + wire_len, (pbw_type)entry->type, &bits)) {
        return 0;
    }
    return enum_known(entry, bits);
}

static napi_value wire_object_to_js(napi_env env, desc_table *table,
                                    const uint8_t *base, uint32_t off,
                                    uint32_t len, uint32_t depth);

/* One recorded slice as a JS value. `depth` bounds the recursion a hand-built
 * slice array could otherwise make unbounded (a slice that points inside its
 * own message would loop forever). */
static napi_value
slice_to_js(napi_env env, desc_table *table, field_entry *entry,
            const uint8_t *base, uint32_t wire_len, const slice_t *slice,
            uint32_t depth)
{
    uint64_t bits;
    const uint8_t *p;

    if (entry->type == PBW_TYPE_MESSAGE || entry->type == PBW_TYPE_GROUP) {
        desc_table *sub = table_sub(table, entry);

        if (sub == NULL) {
            return fail_argument(env, "unresolved message type");
        }
        return wire_object_to_js(env, sub, base, slice->off, slice->len,
                                 depth + 1);
    }
    if (entry->type == PBW_TYPE_STRING || entry->type == PBW_TYPE_BYTES) {
        return bytes_to_js(env, (pbw_type)entry->type, base + slice->off,
                           slice->len);
    }
    p = base + slice->off;
    if (!read_value(&p, base + wire_len, (pbw_type)entry->type, &bits)) {
        return fail_argument(env, "truncated field");
    }
    return scalar_to_js(env, (pbw_type)entry->type, bits);
}

/* Appends the elements of one packed run. */
static int
push_packed(napi_env env, napi_value array, uint32_t *pos, field_entry *entry,
            const uint8_t *base, const slice_t *slice)
{
    const uint8_t *p = base + slice->off;
    const uint8_t *end = p + slice->len;

    while (p < end) {
        uint64_t bits;
        napi_value item;

        if (!read_value(&p, end, (pbw_type)entry->type, &bits)) {
            return 0;
        }
        if (!enum_known(entry, bits)) {
            continue;
        }
        item = scalar_to_js(env, (pbw_type)entry->type, bits);
        if (item == NULL) {
            return 0;
        }
        if (napi_set_element(env, array, (*pos)++, item) != napi_ok) {
            return 0;
        }
    }
    return 1;
}

/* The live slice of a singular field, or -1 when it is absent: the last
 * occurrence on the wire, except that a later member of the same oneof group
 * clears it and a closed enum drops a number the schema does not declare. */
static int32_t
singular_slice(desc_table *table, field_entry *entry, const uint8_t *base,
               uint32_t wire_len, const slice_t *slices, uint32_t count,
               uint32_t index)
{
    uint32_t i = count;

    while (i-- > 0) {
        const slice_t *slice = &slices[i];
        field_entry *found = &table->fields[slice->index];

        if (!slice_known(found, base, wire_len, slice)) {
            continue; /* a closed-enum number the schema does not declare */
        }
        if (entry->oneof != 0) {
            if (found->oneof != entry->oneof) {
                continue; /* some field outside the group */
            }
            if (slice->index != index) {
                return -1; /* another member of the group supersedes it */
            }
        } else if (slice->index != index) {
            continue;
        }
        return (int32_t)i;
    }
    return -1;
}

/* Materializes a message field from every occurrence it had on the wire:
 * parsing the concatenation as one message is what merging means. */
static napi_value
joined_to_js(napi_env env, desc_table *table, field_entry *entry,
             const uint8_t *base, const slice_t *slices, uint32_t count,
             uint32_t index, uint32_t depth)
{
    desc_table *sub = table_sub(table, entry);
    size_t total = 0;
    uint8_t *joined;
    uint8_t *dst;
    napi_value out;
    uint32_t i;

    if (sub == NULL) {
        return fail_argument(env, "unresolved message type");
    }
    for (i = 0; i < count; i++) {
        if (slices[i].index == index) {
            total += slices[i].len;
        }
    }
    joined = malloc(total == 0 ? 1 : total);
    if (joined == NULL) {
        return fail_memory(env);
    }
    dst = joined;
    for (i = 0; i < count; i++) {
        if (slices[i].index != index) {
            continue;
        }
        memcpy(dst, base + slices[i].off, slices[i].len);
        dst += slices[i].len;
    }
    out = wire_object_to_js(env, sub, joined, 0, (uint32_t)total, depth + 1);
    free(joined);
    return out;
}

/* The value of one field of a message whose slices are known. */
static napi_value
field_to_js(napi_env env, desc_table *table, const uint8_t *base,
            uint32_t wire_len, const slice_t *slices, uint32_t count,
            uint32_t index, uint32_t depth)
{
    field_entry *entry = &table->fields[index];

    if (entry->label == PBW_LABEL_REPEATED) {
        napi_value out = NULL;
        uint32_t pos = 0;
        uint32_t i;
        int present = 0;

        for (i = 0; i < count; i++) {
            present |= slices[i].index == index;
        }
        if (!present) {
            /* No value on the wire, including an empty packed run: the field
             * reads as absent, exactly like the tree decoder. */
            TRY(napi_get_undefined(env, &out));
            return out;
        }
        TRY(napi_create_array(env, &out));
        for (i = 0; i < count; i++) {
            const slice_t *slice = &slices[i];
            napi_value item;

            if (slice->index != index) {
                continue;
            }
            if (slice_is_packed(entry, slice)) {
                if (!push_packed(env, out, &pos, entry, base, slice)) {
                    return NULL;
                }
                continue;
            }
            if (!slice_known(entry, base, wire_len, slice)) {
                continue; /* a closed-enum number the schema does not know */
            }
            item = slice_to_js(env, table, entry, base, wire_len, slice,
                               depth);
            if (item == NULL) {
                return NULL;
            }
            TRY(napi_set_element(env, out, pos++, item));
        }
        return out;
    }
    {
        int32_t slot = singular_slice(table, entry, base, wire_len, slices,
                                      count, index);

        if (slot < 0) {
            napi_value out = NULL;

            TRY(napi_get_undefined(env, &out));
            return out;
        }
        if (entry->type == PBW_TYPE_MESSAGE ||
            entry->type == PBW_TYPE_GROUP) {
            return joined_to_js(env, table, entry, base, slices, count, index,
                                depth);
        }
        return slice_to_js(env, table, entry, base, wire_len, &slices[slot],
                           depth);
    }
}

/* A slice list as a plain JS object: one JS object and one JS array per
 * repeated field the wire carried. */
static napi_value
slices_to_js(napi_env env, desc_table *table, const uint8_t *base,
             uint32_t wire_len, const slice_t *slices, uint32_t count,
             uint32_t depth)
{
    napi_value out = NULL;
    napi_value *arrays;
    uint32_t *positions;
    uint32_t *pending;
    uint32_t *oneof_live;
    uint32_t npending = 0;
    uint32_t i;
    const uint8_t *last_bytes = NULL;
    uint32_t last_len = 0;
    napi_value last_str = NULL;

    arrays = calloc(table->nfields == 0 ? 1 : table->nfields,
                    sizeof(napi_value));
    positions = calloc(table->nfields == 0 ? 1 : table->nfields,
                       sizeof(uint32_t));
    pending = calloc(table->nfields == 0 ? 1 : table->nfields,
                     sizeof(uint32_t));
    oneof_live = calloc(table->oneof_count + 1, sizeof(uint32_t));
    if (arrays == NULL || positions == NULL || pending == NULL ||
        oneof_live == NULL) {
        free(arrays);
        free(positions);
        free(pending);
        free(oneof_live);
        return fail_memory(env);
    }
    if (napi_create_object(env, &out) != napi_ok) {
        out = NULL;
    }
    for (i = 0; out != NULL && i < count; i++) {
        const slice_t *slice = &slices[i];
        field_entry *entry = &table->fields[slice->index];
        napi_value item;

        if (entry->label == PBW_LABEL_REPEATED) {
            if (arrays[slice->index] == NULL) {
                napi_value array;

                if (napi_create_array(env, &array) != napi_ok ||
                    napi_set_named_property(env, out, entry->name, array) !=
                        napi_ok) {
                    out = NULL;
                    break;
                }
                arrays[slice->index] = array;
            }
            if (slice_is_packed(entry, slice)) {
                if (!push_packed(env, arrays[slice->index],
                                 &positions[slice->index], entry, base,
                                 slice)) {
                    out = NULL;
                    break;
                }
                continue;
            }
            if (!slice_known(entry, base, wire_len, slice)) {
                continue;
            }
            if (entry->type == PBW_TYPE_STRING && last_str != NULL &&
                slice->len == last_len &&
                (slice->len == 0 ||
                 memcmp(base + slice->off, last_bytes, slice->len) == 0)) {
                item = last_str;
            } else {
                item = slice_to_js(env, table, entry, base, wire_len, slice,
                                   depth);
                if (item == NULL) {
                    out = NULL;
                    break;
                }
                if (entry->type == PBW_TYPE_STRING) {
                    last_bytes = base + slice->off;
                    last_len = slice->len;
                    last_str = item;
                }
            }
            if (napi_set_element(env, arrays[slice->index],
                                 positions[slice->index]++, item) != napi_ok) {
                out = NULL;
            }
            continue;
        }
        if (entry->oneof != 0) {
            /* Which member of a group survives is only known at the end: note
             * the live one and materialize it below. */
            if (slice_known(entry, base, wire_len, slice)) {
                oneof_live[entry->oneof] = slice->index + 1;
            }
            continue;
        }
        if (entry->type == PBW_TYPE_MESSAGE || entry->type == PBW_TYPE_GROUP) {
            /* A singular message field merges across occurrences: materialize
             * it once, after the loop, from all of its payloads. The wire may
             * repeat it more times than the table has fields, so the list is
             * per field (deduped), never per slice. */
            uint32_t k;
            int seen = 0;

            for (k = 0; k < npending; k++) {
                seen |= pending[k] == slice->index;
            }
            if (!seen) {
                pending[npending++] = slice->index;
            }
            continue;
        }
        if (!slice_known(entry, base, wire_len, slice)) {
            continue;
        }
        item = slice_to_js(env, table, entry, base, wire_len, slice, depth);
        if (item == NULL) {
            out = NULL;
            break;
        }
        if (napi_set_named_property(env, out, entry->name, item) != napi_ok) {
            out = NULL;
        }
    }
    for (i = 0; out != NULL && i < npending; i++) {
        uint32_t index = pending[i];
        uint32_t j;
        int seen = 0;
        napi_value item;

        for (j = 0; j < i; j++) {
            seen |= pending[j] == index;
        }
        if (seen) {
            continue;
        }
        item = joined_to_js(env, table, &table->fields[index], base, slices,
                            count, index, depth);
        if (item == NULL ||
            napi_set_named_property(env, out, table->fields[index].name,
                                    item) != napi_ok) {
            out = NULL;
        }
    }
    for (i = 0; out != NULL && i <= table->oneof_count; i++) {
        uint32_t index;
        napi_value item;

        if (oneof_live[i] == 0) {
            continue;
        }
        index = oneof_live[i] - 1;
        item = field_to_js(env, table, base, wire_len, slices, count, index,
                           depth);
        if (item == NULL ||
            napi_set_named_property(env, out, table->fields[index].name,
                                    item) != napi_ok) {
            out = NULL;
        }
    }
    free(arrays);
    free(positions);
    free(pending);
    free(oneof_live);
    return out;
}

/* A leaf message range as a plain JS object: one tag scan that validates and
 * materializes in a single pass, with no slice list and none of the four
 * per-level scratch arrays slices_to_js allocates. A repeated unknown number
 * is skipped exactly as scan_range would skip it; a known field with an
 * unexpected wire type is unknown data too. Later occurrences overwrite
 * earlier ones, which is last-wins for a table with no oneof and no merge.
 * The cursor is an offset, recomputed into a pointer after every N-API call,
 * so no raw pointer into the caller's Buffer outlives an allocation. */
static napi_value
wire_leaf_to_js(napi_env env, desc_table *table, const uint8_t *base,
                uint32_t off, uint32_t len, uint32_t depth)
{
    napi_value out = NULL;
    uint32_t pos = off;
    uint32_t end = off + len;

    if (depth >= PBW_MAX_DEPTH) {
        return fail_argument(env, "nesting limit exceeded");
    }
    if (napi_create_object(env, &out) != napi_ok) {
        return NULL;
    }
    while (pos < end) {
        const uint8_t *p = base + pos;
        const uint8_t *pend = base + end;
        uint32_t tag;
        uint32_t number;
        uint32_t wire_type;
        uint32_t index;
        field_entry *entry;
        napi_value item;
        scan_status skip;

        if (!rb_tag(&p, pend, &tag)) {
            return fail_argument(env, "invalid wire data");
        }
        number = tag >> 3;
        wire_type = tag & 7;
        if (number == 0 || wire_type > 5 || wire_type == 4) {
            return fail_argument(env, "invalid wire data");
        }
        pos = (uint32_t)(p - base);
        index = table_by_number(table, number);
        if (index == NOT_FOUND ||
            table->fields[index].wire != wire_type) {
            /* Unknown, or a known field with an unexpected wire type:
             * unknown data, skipped after validation. */
            p = base + pos;
            skip = skip_value(&p, pend, number, wire_type, depth);
            if (skip != SCAN_OK) {
                return fail_argument(env, skip == SCAN_LIMIT
                                              ? "nesting limit exceeded"
                                              : "invalid wire data");
            }
            pos = (uint32_t)(p - base);
            continue;
        }
        entry = &table->fields[index];
        if (entry->type == PBW_TYPE_STRING ||
            entry->type == PBW_TYPE_BYTES) {
            size_t payload_len;
            uint32_t payload;

            p = base + pos;
            if (!rb_len(&p, pend, &payload_len) ||
                payload_len > (size_t)(pend - p)) {
                return fail_argument(env, "invalid wire data");
            }
            if (entry->utf8 && !pbw_utf8_valid(p, payload_len)) {
                return fail_argument(env, "invalid wire data");
            }
            payload = (uint32_t)(p - base);
            pos = (uint32_t)(p - base + payload_len);
            item = bytes_to_js(env, (pbw_type)entry->type, base + payload,
                               payload_len);
            if (item == NULL) {
                return NULL;
            }
            if (napi_set_named_property(env, out, entry->name, item) !=
                napi_ok) {
                return NULL;
            }
            continue;
        }
        {
            uint64_t bits;
            const uint8_t *after;

            p = base + pos;
            after = p;
            if (!read_value(&after, pend, (pbw_type)entry->type, &bits)) {
                return fail_argument(env, "invalid wire data");
            }
            pos = (uint32_t)(after - base);
            if (!enum_known(entry, bits)) {
                continue; /* a closed-enum number the schema ignores */
            }
            item = scalar_to_js(env, (pbw_type)entry->type, bits);
            if (item == NULL) {
                return NULL;
            }
            if (napi_set_named_property(env, out, entry->name, item) !=
                napi_ok) {
                return NULL;
            }
        }
    }
    return out;
}

/* A message range as a plain JS object: one tag scan into a stack slice list,
 * then the object. */
static napi_value
wire_object_to_js(napi_env env, desc_table *table, const uint8_t *base,
                  uint32_t off, uint32_t len, uint32_t depth)
{
    slice_list list;
    napi_value out;
    scan_status status;

    memset(&list, 0, sizeof(list));
    list.items = list.inline_;
    list.cap = 32;
    if (depth >= PBW_MAX_DEPTH) {
        return fail_argument(env, "nesting limit exceeded");
    }
    if (table->leaf) {
        return wire_leaf_to_js(env, table, base, off, len, depth);
    }
    status = scan_range(table, base, off, len, depth, 0, &list, NULL);
    if (status != SCAN_OK) {
        slices_release(&list);
        return fail_argument(env, "invalid wire data");
    }
    out = slices_to_js(env, table, base, off + len, list.items, list.count,
                       depth);
    slices_release(&list);
    return out;
}

/* ------------------------------------------------------------- lifecycle */

static void
finalize_schema(napi_env env, void *data, void *hint)
{
    schema_handle *handle = data;
    size_t i;

    (void)env;
    (void)hint;
    for (i = 0; i < handle->count; i++) {
        desc_table *table = handle->tables[i];

        free(table->slots);
        free(table->dense);
        free(table->by_number);
        free(table->fields);
        free(table);
    }
    free(handle->tables);
    pbw_schema_free(handle->schema);
    free(handle);
}

static void
free_reference(napi_env env, napi_ref *ref)
{
    if (*ref != NULL) {
        napi_delete_reference(env, *ref);
        *ref = NULL;
    }
}

static void
finalize_desc(napi_env env, void *data, void *hint)
{
    desc_handle *handle = data;

    (void)hint;
    pbw_ctx_free(handle->enc);
    free_reference(env, &handle->schema_ref);
    free(handle);
}

static void
finalize_message(napi_env env, void *data, void *hint)
{
    message_handle *handle = data;

    (void)hint;
    pbw_message_free(handle->msg);
    free_reference(env, &handle->desc_ref);
    free(handle);
}

/* Every handle is a plain wrapped object; the JS layer attaches its own
 * prototype, so no addon-side shape is promised. */
static napi_value
new_handle(napi_env env, void *handle, napi_finalize finalize)
{
    napi_value object;

    /* A failed wrap leaves the finalizer unattached: the caller owns handle
     * and runs finalize itself. */
    if (napi_create_object(env, &object) != napi_ok ||
        napi_wrap(env, object, handle, finalize, NULL, NULL) != napi_ok) {
        napi_throw_error(env, NULL, "pbwire: out of memory");
        return NULL;
    }
    return object;
}

/* Wraps a message handle that already holds its tree, taking a strong
 * reference to the descriptor object so the schema outlives it. */
static napi_value
new_message_value(napi_env env, desc_handle *desc, napi_value desc_object,
                  message_handle *handle)
{
    napi_value out;

    handle->desc = desc;
    if (napi_create_reference(env, desc_object, 1, &handle->desc_ref) !=
        napi_ok) {
        pbw_message_free(handle->msg);
        free(handle);
        return fail_memory(env);
    }
    out = new_handle(env, handle, finalize_message);
    if (out == NULL) {
        pbw_message_free(handle->msg);
        free_reference(env, &handle->desc_ref);
        free(handle);
    }
    return out;
}

/* Optional {maxBytes, maxDepth} limits argument: absent, null or undefined
 * means the core defaults (no byte cap, PBW_MAX_DEPTH nesting). */
/* Whether a non-negative integral double fits the target's size_t. */
static int
fits_size_t(double value)
{
    return value <= (double)SIZE_MAX;
}

static int
parse_limits(napi_env env, napi_value *argv, size_t argc, size_t index,
             pbw_limits *storage, const pbw_limits **out)
{
    napi_valuetype kind;
    napi_value prop;
    double number;
    uint32_t depth;

    *out = NULL;
    if (index >= argc) {
        return 1;
    }
    if (napi_typeof(env, argv[index], &kind) != napi_ok) {
        return 0;
    }
    if (kind == napi_undefined || kind == napi_null) {
        return 1;
    }
    if (kind != napi_object) {
        fail_argument(env, "limits must be an object");
        return 0;
    }
    memset(storage, 0, sizeof(*storage));
    if (napi_get_named_property(env, argv[index], "maxBytes", &prop) !=
            napi_ok ||
        napi_typeof(env, prop, &kind) != napi_ok) {
        return 0;
    }
    if (kind != napi_undefined && kind != napi_null) {
        /* A JS number is a double: reject NaN, Infinity, a fraction and
         * anything a size_t cannot hold, so a bad cap fails loudly instead of
         * truncating to 0 ("no cap") or wrapping to a huge one. */
        if (napi_get_value_double(env, prop, &number) != napi_ok ||
            !(number >= 0) || number != number ||
            number != floor(number) ||
            number > (double)9007199254740991.0 ||
            !fits_size_t(number)) {
            fail_argument(env, "maxBytes must be a non-negative integer");
            return 0;
        }
        storage->max_bytes = (size_t)number;
    }
    if (napi_get_named_property(env, argv[index], "maxDepth", &prop) !=
            napi_ok ||
        napi_typeof(env, prop, &kind) != napi_ok) {
        return 0;
    }
    if (kind != napi_undefined && kind != napi_null) {
        /* napi_get_value_uint32 converts a negative double instead of
         * failing (-1 becomes 4294967295), so validate the double first. */
        double depth_value;

        if (napi_get_value_double(env, prop, &depth_value) != napi_ok ||
            !(depth_value >= 1) || depth_value != depth_value ||
            depth_value != floor(depth_value) ||
            depth_value > (double)UINT32_MAX) {
            fail_argument(env, "maxDepth must be a positive integer");
            return 0;
        }
        depth = (uint32_t)depth_value;
        storage->max_depth = depth;
    }
    *out = storage;
    return 1;
}

/* Wraps one message descriptor: a scratch encode context plus the cached
 * field table, holding a strong reference to the schema object. */
static napi_value
new_desc_value(napi_env env, napi_value schema_value, schema_handle *schema,
               const pbw_descriptor *desc)
{
    napi_value out;
    desc_handle *handle = calloc(1, sizeof(*handle));

    if (handle == NULL) {
        return fail_memory(env);
    }
    handle->schema = schema;
    handle->enc = pbw_ctx_new();
    handle->table = table_for(schema, desc);
    if (handle->enc == NULL || handle->table == NULL) {
        pbw_ctx_free(handle->enc);
        free(handle);
        return fail_memory(env);
    }
    if (napi_create_reference(env, schema_value, 1, &handle->schema_ref) !=
        napi_ok) {
        finalize_desc(env, handle, NULL);
        return fail_memory(env);
    }
    out = new_handle(env, handle, finalize_desc);
    if (out == NULL) {
        finalize_desc(env, handle, NULL);
    }
    return out;
}

/* Wraps a fresh tree in a message handle holding the descriptor. */
static napi_value
new_tree_value(napi_env env, desc_handle *desc, napi_value desc_object,
               pbw_message *msg)
{
    message_handle *handle = calloc(1, sizeof(*handle));

    if (handle == NULL) {
        pbw_message_free(msg);
        return fail_memory(env);
    }
    handle->msg = msg;
    return new_message_value(env, desc, desc_object, handle);
}

/* ---------------------------------------------------------- entry points */

static napi_value
abi_version(napi_env env, napi_callback_info info)
{
    napi_value out = NULL;

    (void)info;
    if (napi_create_int32(env, pbw_abi_version(), &out) != napi_ok) {
        return NULL;
    }
    return out;
}

static napi_value
load_descriptor_set(napi_env env, napi_callback_info info)
{
    napi_value argv[2];
    size_t argc = 2;
    napi_value out;
    schema_handle *handle;
    pbw_schema *schema = NULL;
    pbw_status status;
    pbw_limits storage;
    const pbw_limits *limits;
    void *data = NULL;
    size_t len = 0;

    if (napi_get_cb_info(env, info, &argc, argv, NULL, NULL) != napi_ok ||
        argc < 1) {
        napi_throw_type_error(env, NULL,
                              "pbwire: expected (bytes, limits?)");
        return NULL;
    }
    if (!parse_limits(env, argv, argc, 1, &storage, &limits)) {
        return NULL;
    }
    if (napi_get_buffer_info(env, argv[0], &data, &len) != napi_ok) {
        return fail_argument(env, "loadDescriptorSet expects a Buffer");
    }
    status = pbw_descriptor_set_load_limits(data, len, limits, &schema);
    if (status != PBW_OK) {
        char message[512];

        snprintf(message, sizeof(message), "pbwire: %s",
                 pbw_schema_error(schema));
        napi_throw_error(env, NULL, message);
        pbw_schema_free(schema);
        return NULL;
    }
    handle = calloc(1, sizeof(*handle));
    if (handle == NULL) {
        pbw_schema_free(schema);
        return fail_memory(env);
    }
    handle->schema = schema;
    out = new_handle(env, handle, finalize_schema);
    if (out == NULL) {
        finalize_schema(env, handle, NULL);
    }
    return out;
}

static napi_value
descriptor(napi_env env, napi_callback_info info)
{
    napi_value argv[2];
    napi_value out;
    schema_handle *schema;
    const pbw_descriptor *desc;
    string_buf buf;
    const char *name;

    if (!get_args(env, info, argv, 2)) {
        return NULL;
    }
    if (!unwrap(env, argv[0], (void **)&schema, "schema handle")) {
        return NULL;
    }
    name = string_bytes(env, argv[1], &buf);
    if (name == NULL) {
        return NULL;
    }
    desc = pbw_schema_find(schema->schema, name);
    string_free(&buf, name);
    if (desc == NULL || pbw_descriptor_is_enum(desc)) {
        TRY(napi_get_null(env, &out));
        return out;
    }
    return new_desc_value(env, argv[0], schema, desc);
}

static napi_value
message_new(napi_env env, napi_callback_info info)
{
    napi_value argv[1];
    desc_handle *desc;
    message_handle *handle;
    pbw_message *msg;

    if (!get_args(env, info, argv, 1)) {
        return NULL;
    }
    if (!unwrap(env, argv[0], (void **)&desc, "message descriptor handle")) {
        return NULL;
    }
    msg = pbw_message_new(desc->table->desc);
    if (msg == NULL) {
        return fail_memory(env);
    }
    handle = calloc(1, sizeof(*handle));
    if (handle == NULL) {
        pbw_message_free(msg);
        return fail_memory(env);
    }
    handle->msg = msg;
    return new_message_value(env, desc, argv[0], handle);
}

/* 1 when this type (or a message reachable from it) declares a proto2
 * `required` field: the JS layer then decodes through decodeEager(). */
static napi_value
is_eager(napi_env env, napi_callback_info info)
{
    napi_value argv[1];
    napi_value out = NULL;
    desc_handle *desc;

    if (!get_args(env, info, argv, 1)) {
        return NULL;
    }
    if (!unwrap(env, argv[0], (void **)&desc, "message descriptor handle")) {
        return NULL;
    }
    if (napi_get_boolean(env, table_is_eager(desc->table) != 0, &out) !=
        napi_ok) {
        return NULL;
    }
    return out;
}

/* Validates a wire range and returns one (field index, offset, length) triple
 * per wire field as a Uint32Array. The bytes stay owned by the caller (a JS
 * Buffer), so a decoded message needs no native state at all. */
static napi_value
decode(napi_env env, napi_callback_info info)
{
    napi_value argv[3];
    size_t argc = 3;
    napi_value out = NULL;
    napi_value array_buffer = NULL;
    desc_handle *desc;
    void *data = NULL;
    size_t len = 0;
    slice_list list;
    scan_status status;
    pbw_limits storage;
    const pbw_limits *limits;
    void *words = NULL;
    uint32_t i;

    if (napi_get_cb_info(env, info, &argc, argv, NULL, NULL) != napi_ok ||
        argc < 2) {
        napi_throw_type_error(env, NULL,
                              "pbwire: expected (desc, bytes, limits?)");
        return NULL;
    }
    if (!unwrap(env, argv[0], (void **)&desc, "message descriptor handle")) {
        return NULL;
    }
    if (!parse_limits(env, argv, argc, 2, &storage, &limits)) {
        return NULL;
    }
    if (napi_get_buffer_info(env, argv[1], &data, &len) != napi_ok) {
        return fail_argument(env, "decode expects a Buffer");
    }
    if ((uint64_t)len > 0xffffffffull) {
        return fail_argument(env, "decode expects a message below 4 GiB");
    }
    if (table_is_eager(desc->table)) {
        return fail_argument(env, "decode on a schema that needs the tree");
    }
    if (limits != NULL) {
        /* The view itself stays lazy: validate eagerly, then hand back
         * the usual slices. A non-eager schema has no required fields, so
         * the check only enforces the requested bounds. */
        pbw_message *checked = NULL;
        pbw_status checked_status = pbw_decode_limits(
            desc->table->desc, data, len, limits, &checked);

        if (checked_status != PBW_OK) {
            return fail_status(env, "decode", checked_status);
        }
        pbw_message_free(checked);
    }
    memset(&list, 0, sizeof(list));
    list.items = list.inline_;
    list.cap = 32;
    status = scan_range(desc->table, (const uint8_t *)data, 0, (uint32_t)len,
                        0, 0, &list, NULL);
    if (status != SCAN_OK) {
        slices_release(&list);
        if (status == SCAN_NOMEM) {
            return fail_memory(env);
        }
        return fail_argument(env, status == SCAN_LIMIT
                                      ? "decode: nesting limit exceeded"
                                      : "decode: invalid wire data");
    }
    if (napi_create_arraybuffer(env, (size_t)list.count * sizeof(slice_t),
                                &words, &array_buffer) != napi_ok ||
        napi_create_typedarray(env, napi_uint32_array,
                               (size_t)list.count * 3, array_buffer, 0,
                               &out) != napi_ok) {
        slices_release(&list);
        return fail_memory(env);
    }
    for (i = 0; i < list.count; i++) {
        uint32_t *triple = (uint32_t *)words + (size_t)i * 3;

        triple[0] = list.items[i].index;
        triple[1] = list.items[i].off;
        triple[2] = list.items[i].len;
    }
    slices_release(&list);
    return out;
}

/* Decodes into a tree for schemas whose proto2 `required` fields have to hold
 * in the merged message: the tree decoder is the code that checks that. */
static napi_value
decode_eager(napi_env env, napi_callback_info info)
{
    napi_value argv[3];
    size_t argc = 3;
    desc_handle *desc;
    message_handle *handle;
    pbw_message *msg = NULL;
    void *data = NULL;
    size_t len = 0;
    pbw_limits storage;
    const pbw_limits *limits;
    pbw_status status;

    if (napi_get_cb_info(env, info, &argc, argv, NULL, NULL) != napi_ok ||
        argc < 2) {
        napi_throw_type_error(env, NULL,
                              "pbwire: expected (desc, bytes, limits?)");
        return NULL;
    }
    if (!unwrap(env, argv[0], (void **)&desc, "message descriptor handle")) {
        return NULL;
    }
    if (!parse_limits(env, argv, argc, 2, &storage, &limits)) {
        return NULL;
    }
    if (napi_get_buffer_info(env, argv[1], &data, &len) != napi_ok) {
        return fail_argument(env, "decodeEager expects a Buffer");
    }
    status =
        pbw_decode_limits(desc->table->desc, data, len, limits, &msg);
    if (status != PBW_OK) {
        return fail_status(env, "decode", status);
    }
    handle = calloc(1, sizeof(*handle));
    if (handle == NULL) {
        pbw_message_free(msg);
        return fail_memory(env);
    }
    handle->msg = msg;
    return new_message_value(env, desc, argv[0], handle);
}

/* One validating scan straight into a plain object: no slice array, no
 * retained bytes, one N-API call however many fields are read. Schemas with
 * proto2 `required` fields go through the tree (the only decoder that
 * checks them) and convert from there. */
static napi_value
decode_object(napi_env env, napi_callback_info info)
{
    napi_value argv[3];
    size_t argc = 3;
    desc_handle *desc;
    void *data = NULL;
    size_t len = 0;
    pbw_limits storage;
    const pbw_limits *limits;

    if (napi_get_cb_info(env, info, &argc, argv, NULL, NULL) != napi_ok ||
        argc < 2) {
        napi_throw_type_error(env, NULL,
                              "pbwire: expected (desc, bytes, limits?)");
        return NULL;
    }
    if (!unwrap(env, argv[0], (void **)&desc, "message descriptor handle")) {
        return NULL;
    }
    if (!parse_limits(env, argv, argc, 2, &storage, &limits)) {
        return NULL;
    }
    if (napi_get_buffer_info(env, argv[1], &data, &len) != napi_ok) {
        return fail_argument(env, "decodeObject expects a Buffer");
    }
    if ((uint64_t)len > 0xffffffffull) {
        return fail_argument(env, "decodeObject expects a message below 4 GiB");
    }
    if (table_is_eager(desc->table)) {
        pbw_message *msg = NULL;
        napi_value out;
        pbw_status status = pbw_decode_limits(desc->table->desc, data, len,
                                              limits, &msg);

        if (status != PBW_OK) {
            return fail_status(env, "decodeObject", status);
        }
        out = message_to_js(env, msg);
        pbw_message_free(msg);
        return out;
    }
    if (limits != NULL) {
        /* Same eager validation as decode(): the wire view accepts what
         * the tree decoder accepts, so this only enforces the bounds. */
        pbw_message *checked = NULL;
        pbw_status checked_status = pbw_decode_limits(
            desc->table->desc, data, len, limits, &checked);

        if (checked_status != PBW_OK) {
            return fail_status(env, "decodeObject", checked_status);
        }
        pbw_message_free(checked);
    }
    return wire_object_to_js(env, desc->table, (const uint8_t *)data, 0,
                             (uint32_t)len, 0);
}

/* The value of one field of a decoded message. */
static napi_value
wire_get(napi_env env, napi_callback_info info)
{
    napi_value argv[4];
    desc_handle *desc;
    void *wire = NULL;
    size_t wire_len = 0;
    napi_typedarray_type array_type;
    size_t count = 0;
    void *words = NULL;
    string_buf buf;
    const char *name;
    uint32_t index;

    if (!get_args(env, info, argv, 4)) {
        return NULL;
    }
    if (!unwrap(env, argv[0], (void **)&desc, "message descriptor handle")) {
        return NULL;
    }
    if (napi_get_buffer_info(env, argv[1], &wire, &wire_len) != napi_ok) {
        return fail_argument(env, "wireGet expects a Buffer");
    }
    /* Same bound as decode(): the slices carry uint32 offsets, and a longer
     * buffer would let a triple point past the truncated end used below. */
    if ((uint64_t)wire_len > 0xffffffffull) {
        return fail_argument(env, "wireGet expects a message below 4 GiB");
    }
    if (napi_get_typedarray_info(env, argv[2], &array_type, &count, &words,
                                 NULL, NULL) != napi_ok ||
        array_type != napi_uint32_array || count % 3 != 0) {
        return fail_argument(env, "wireGet expects a slice array");
    }
    /* The slice array is caller-supplied and mutable: bound every triple
     * before anything indexes the field table or reads the bytes with it. */
    {
        const uint32_t *triples = words;
        size_t i;

        for (i = 0; i < count; i += 3) {
            uint64_t off = triples[i + 1];
            uint64_t len = triples[i + 2];

            if (triples[i] >= desc->table->nfields ||
                off > (uint64_t)wire_len || len > (uint64_t)wire_len - off) {
                return fail_argument(env, "wireGet: slice out of range");
            }
        }
    }
    name = string_bytes(env, argv[3], &buf);
    if (name == NULL) {
        return NULL;
    }
    index = table_by_name(desc->table, name);
    if (index == NOT_FOUND) {
        char message[512];

        snprintf(message, sizeof(message), "pbwire: %s has no field %s",
                 pbw_descriptor_name(desc->table->desc), name);
        napi_throw_error(env, NULL, message);
        string_free(&buf, name);
        return NULL;
    }
    string_free(&buf, name);
    return field_to_js(env, desc->table, (const uint8_t *)wire,
                       (uint32_t)wire_len, (const slice_t *)words,
                       (uint32_t)(count / 3), index, 0);
}

/* A decoded message that is written to becomes a tree. */
static napi_value
promote(napi_env env, napi_callback_info info)
{
    napi_value argv[2];
    desc_handle *desc;
    message_handle *handle;
    pbw_message *msg = NULL;
    void *data = NULL;
    size_t len = 0;
    pbw_status status;

    if (!get_args(env, info, argv, 2)) {
        return NULL;
    }
    if (!unwrap(env, argv[0], (void **)&desc, "message descriptor handle")) {
        return NULL;
    }
    if (napi_get_buffer_info(env, argv[1], &data, &len) != napi_ok) {
        return fail_argument(env, "promote expects a Buffer");
    }
    status = pbw_decode(desc->table->desc, data, len, &msg);
    if (status != PBW_OK) {
        return fail_status(env, "message", status);
    }
    handle = calloc(1, sizeof(*handle));
    if (handle == NULL) {
        pbw_message_free(msg);
        return fail_memory(env);
    }
    handle->msg = msg;
    return new_message_value(env, desc, argv[0], handle);
}

/* Shared by every entry point that takes a tree message: a freed handle throws
 * instead of dereferencing a released tree. */
static int
unwrap_message(napi_env env, napi_value value, message_handle **out)
{
    message_handle *handle;

    if (!unwrap(env, value, (void **)&handle, "message handle")) {
        return 0;
    }
    if (handle->freed) {
        fail_argument(env, "message was freed");
        return 0;
    }
    *out = handle;
    return 1;
}

/* Resolves a JS field name against the message's descriptor. */
static field_entry *
field_by_name(napi_env env, message_handle *handle, napi_value name_value)
{
    string_buf buf;
    const char *name = string_bytes(env, name_value, &buf);
    desc_table *table = handle->desc->table;
    uint32_t index;

    if (name == NULL) {
        return NULL;
    }
    index = table_by_name(table, name);
    if (index == NOT_FOUND) {
        char message[512];

        snprintf(message, sizeof(message), "pbwire: %s has no field %s",
                 pbw_descriptor_name(table->desc), name);
        napi_throw_error(env, NULL, message);
        string_free(&buf, name);
        return NULL;
    }
    string_free(&buf, name);
    return &table->fields[index];
}

static napi_value
message_get(napi_env env, napi_callback_info info)
{
    napi_value argv[2];
    message_handle *handle;
    field_entry *entry;

    if (!get_args(env, info, argv, 2)) {
        return NULL;
    }
    if (!unwrap_message(env, argv[0], &handle)) {
        return NULL;
    }
    entry = field_by_name(env, handle, argv[1]);
    if (entry == NULL) {
        return NULL;
    }
    {
        const pbw_value *value =
            pbw_message_get_field(handle->msg, entry->field);

        if (value == NULL) {
            napi_value out = NULL;

            TRY(napi_get_undefined(env, &out));
            return out;
        }
        return value_to_js(env, (pbw_type)entry->type, value);
    }
}

static napi_value
message_set(napi_env env, napi_callback_info info)
{
    napi_value argv[3];
    napi_value out;
    message_handle *handle;
    field_entry *entry;

    if (!get_args(env, info, argv, 3)) {
        return NULL;
    }
    if (!unwrap_message(env, argv[0], &handle)) {
        return NULL;
    }
    entry = field_by_name(env, handle, argv[1]);
    if (entry == NULL) {
        return NULL;
    }
    if (!set_field(env, handle->msg, handle->desc->table, entry, argv[2],
                   NULL)) {
        return NULL;
    }
    TRY(napi_get_undefined(env, &out));
    return out;
}

/* Encodes a tree into the type's scratch buffer and returns its length. The
 * JS layer then allocates a pooled Buffer of exactly that size and calls
 * copyEncoded(): N-API's own Buffer creation costs ~10x a pooled allocation. */
static napi_value
encode(napi_env env, napi_callback_info info)
{
    napi_value argv[1];
    napi_value out = NULL;
    message_handle *handle;
    pbw_status status;

    if (!get_args(env, info, argv, 1)) {
        return NULL;
    }
    if (!unwrap_message(env, argv[0], &handle)) {
        return NULL;
    }
    status = pbw_encode_ctx(handle->desc->enc, handle->msg,
                            &handle->desc->enc_data, &handle->desc->enc_len);
    if (status != PBW_OK) {
        handle->desc->enc_valid = 0;
        return fail_status(env, "encode", status);
    }
    if (napi_create_double(env, (double)handle->desc->enc_len, &out) !=
        napi_ok) {
        return NULL;
    }
    handle->desc->enc_valid = 1;
    return out;
}

/* Builds a tree from a plain object, encodes it into the type's scratch and
 * returns its length; the tree never outlives this call. */
static napi_value
encode_object(napi_env env, napi_callback_info info)
{
    napi_value argv[2];
    napi_value out = NULL;
    desc_handle *desc;
    pbw_message *msg;
    pbw_status status;
    int ok;

    if (!get_args(env, info, argv, 2)) {
        return NULL;
    }
    if (!unwrap(env, argv[0], (void **)&desc, "message descriptor handle")) {
        return NULL;
    }
    /* One allocation for the whole walk: the tree and every value live in the
     * type's encode arena and die with the next reset, so a 3000-element
     * payload costs one arena chunk, not one malloc per field. */
    pbw_ctx_reset(desc->enc);
    msg = pbw_message_new_ctx(desc->enc, desc->table->desc);
    if (msg == NULL) {
        return fail_memory(env);
    }
    ok = js_to_message(env, msg, desc->table, argv[1], NULL, desc->enc);
    if (ok) {
        status = pbw_encode_ctx(desc->enc, msg, &desc->enc_data, &desc->enc_len);
        if (status != PBW_OK) {
            desc->enc_valid = 0;
            fail_status(env, "encode", status);
            ok = 0;
        }
    }
    if (!ok) {
        return NULL;
    }
    if (napi_create_double(env, (double)desc->enc_len, &out) != napi_ok) {
        return NULL;
    }
    desc->enc_valid = 1;
    return out;
}

/* Copies the scratch an encode()/encodeObject() call just filled into a
 * caller-provided Buffer, which lets JS use a pooled allocation. */
static napi_value
copy_encoded(napi_env env, napi_callback_info info)
{
    napi_value argv[2];
    napi_value out;
    desc_handle *desc;
    void *data = NULL;
    size_t len = 0;

    if (!get_args(env, info, argv, 2)) {
        return NULL;
    }
    if (!unwrap(env, argv[0], (void **)&desc, "message descriptor handle")) {
        return NULL;
    }
    if (napi_get_buffer_info(env, argv[1], &data, &len) != napi_ok) {
        return fail_argument(env, "copyEncoded expects a Buffer");
    }
    if (!desc->enc_valid) {
        return fail_argument(env, "copyEncoded without a preceding encode");
    }
    if (len < desc->enc_len) {
        return fail_argument(env, "copyEncoded buffer is too small");
    }
    if (desc->enc_len > 0) {
        memcpy(data, desc->enc_data, desc->enc_len);
    }
    desc->enc_valid = 0;
    TRY(napi_get_undefined(env, &out));
    return out;
}

/* ------------------------- flat-image matrix3 spike ------------------------ */

/* Hand-written flat layout for structtest.Scalars3, mirroring
 * pbench/layout-matrix3.js (same field order as payload.js nativeObject).
 * encodeImage reads the host ArrayBuffer in place through the offsets below:
 * one N-API crossing per op, no per-field property reads. String payloads
 * are copied into the type's encode arena exactly like encodeObject's values
 * (value_new_*_ctx), so nothing aliases the host buffer. decodeImage runs
 * the tree decoder and fills a fresh ArrayBuffer the same way.
 *
 * Fixed header (83 bytes): i32@0 i64@4 u32@12 u64@16 s32@24 s64@28 fx32@36
 * fx64@40 sfx32@48 sfx64@52 f32@60 f64@64 flag@72 color@73 opt_i32 present@77
 * opt_i32@78 opt_str present@82. Variable tail from 83, in field order:
 * str, inner, nums, tags, inners, opt_str (when present), flags; each length
 * or count a little-endian u32, each scalar little-endian at its width.
 *
 * Spike limits (documented in layout-matrix3.js): every singular field
 * except the two proto3 optionals is assumed present; an absent singular
 * encodes as zero/empty rather than failing. */

#define IMG_HEAD_LEN 83
#define IMG_OFF_I32 0
#define IMG_OFF_I64 4
#define IMG_OFF_U32 12
#define IMG_OFF_U64 16
#define IMG_OFF_S32 24
#define IMG_OFF_S64 28
#define IMG_OFF_FX32 36
#define IMG_OFF_FX64 40
#define IMG_OFF_SFX32 48
#define IMG_OFF_SFX64 52
#define IMG_OFF_F32 60
#define IMG_OFF_F64 64
#define IMG_OFF_FLAG 72
#define IMG_OFF_COLOR 73
#define IMG_OFF_OPT_I32_PRESENT 77
#define IMG_OFF_OPT_I32 78
#define IMG_OFF_OPT_STR_PRESENT 82

/* Every resolved field of Scalars3: the singular entries plus the inner
 * child's name/value entries from its sub-table. */
typedef struct {
    field_entry *i32;
    field_entry *i64;
    field_entry *u32;
    field_entry *u64;
    field_entry *s32;
    field_entry *s64;
    field_entry *fx32;
    field_entry *fx64;
    field_entry *sfx32;
    field_entry *sfx64;
    field_entry *f32;
    field_entry *f64;
    field_entry *flag;
    field_entry *str;
    field_entry *color;
    field_entry *inner;
    field_entry *inner_name;
    field_entry *inner_value;
    field_entry *nums;
    field_entry *tags;
    field_entry *inners;
    field_entry *opt_i32;
    field_entry *opt_str;
    field_entry *flags;
} img_fields;

/* A cursor over the host image; reads check first, so a short or hostile
 * buffer fails loudly instead of over-reading. */
typedef struct {
    size_t len;
    size_t pos;
} img_cursor;

static int
img_avail(const img_cursor *r, size_t n)
{
    return n <= r->len && r->pos <= r->len - n;
}

static uint32_t
img_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t
img_u64(const uint8_t *p)
{
    return (uint64_t)img_u32(p) | ((uint64_t)img_u32(p + 4) << 32);
}

static void
img_put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void
img_put_u64(uint8_t *p, uint64_t v)
{
    img_put_u32(p, (uint32_t)v);
    img_put_u32(p + 4, (uint32_t)(v >> 32));
}

static int
img_resolve(napi_env env, desc_table *table, img_fields *f)
{
#define IMG_FIELD(name)                                                   \
    do {                                                                  \
        uint32_t index = table_by_name(table, #name);                     \
                                                                          \
        if (index == NOT_FOUND) {                                         \
            fail_argument(env, "flat image supports structtest.Scalars3 " \
                               "only");                                   \
            return 0;                                                     \
        }                                                                 \
        f->name = &table->fields[index];                                  \
    } while (0)
    IMG_FIELD(i32);
    IMG_FIELD(i64);
    IMG_FIELD(u32);
    IMG_FIELD(u64);
    IMG_FIELD(s32);
    IMG_FIELD(s64);
    IMG_FIELD(fx32);
    IMG_FIELD(fx64);
    IMG_FIELD(sfx32);
    IMG_FIELD(sfx64);
    IMG_FIELD(f32);
    IMG_FIELD(f64);
    IMG_FIELD(flag);
    IMG_FIELD(str);
    IMG_FIELD(color);
    IMG_FIELD(inner);
    IMG_FIELD(nums);
    IMG_FIELD(tags);
    IMG_FIELD(inners);
    IMG_FIELD(opt_i32);
    IMG_FIELD(opt_str);
    IMG_FIELD(flags);
#undef IMG_FIELD
    {
        desc_table *sub = table_sub(table, f->inner);
        uint32_t index;

        if (sub == NULL) {
            fail_argument(env, "flat image supports structtest.Scalars3 only");
            return 0;
        }
        index = table_by_name(sub, "name");
        if (index == NOT_FOUND) {
            fail_argument(env, "flat image supports structtest.Scalars3 only");
            return 0;
        }
        f->inner_name = &sub->fields[index];
        index = table_by_name(sub, "value");
        if (index == NOT_FOUND) {
            fail_argument(env, "flat image supports structtest.Scalars3 only");
            return 0;
        }
        f->inner_value = &sub->fields[index];
    }
    return 1;
}

static int
img_set_uint(napi_env env, pbw_message *msg, field_entry *entry,
             uint64_t bits, pbw_ctx *ctx)
{
    pbw_value *value = value_new_uint(ctx, bits);
    pbw_status status;

    if (value == NULL) {
        fail_memory(env);
        return 0;
    }
    status = pbw_message_set_field(msg, entry->field, value);
    if (status != PBW_OK) {
        pbw_value_free(value);
        fail_status(env, entry->name, status);
        return 0;
    }
    return 1;
}

static int
img_set_bytes(napi_env env, pbw_message *msg, field_entry *entry,
              const void *data, size_t len, pbw_ctx *ctx)
{
    pbw_value *value = value_new_bytes(ctx, data, len);
    pbw_status status;

    if (value == NULL) {
        fail_memory(env);
        return 0;
    }
    status = pbw_message_set_field(msg, entry->field, value);
    if (status != PBW_OK) {
        pbw_value_free(value);
        fail_status(env, entry->name, status);
        return 0;
    }
    return 1;
}

/* Reads the flat image into a tree in the type's encode arena, then encodes
 * it; the returned Buffer is the only thing that crosses back. */
static napi_value
encode_image(napi_env env, napi_callback_info info)
{
    napi_value argv[2];
    napi_value out = NULL;
    desc_handle *desc;
    void *image = NULL;
    size_t image_len = 0;
    img_fields f;
    img_cursor r;
    const uint8_t *base;
    pbw_message *msg;
    pbw_status status;
    uint32_t count;
    uint32_t i;
#define IMG_OK(x)        \
    do {                 \
        if (!(x)) {      \
            return NULL; \
        }                \
    } while (0)

    if (!get_args(env, info, argv, 2)) {
        return NULL;
    }
    if (!unwrap(env, argv[0], (void **)&desc, "message descriptor handle")) {
        return NULL;
    }
    if (strcmp(pbw_descriptor_name(desc->table->desc),
               "structtest.Scalars3") != 0) {
        return fail_argument(env,
                             "encodeImage supports structtest.Scalars3 only");
    }
    if (napi_get_arraybuffer_info(env, argv[1], &image, &image_len) !=
        napi_ok) {
        return fail_argument(env, "encodeImage expects an ArrayBuffer");
    }
    if (!img_resolve(env, desc->table, &f)) {
        return NULL;
    }
    base = image;
    r.len = image_len;
    r.pos = 0;
    if (!img_avail(&r, IMG_HEAD_LEN)) {
        return fail_argument(env, "encodeImage: image too small");
    }
    /* One allocation for the whole walk: the tree and every value live in the
     * type's encode arena and die with the next reset, exactly like
     * encodeObject. */
    pbw_ctx_reset(desc->enc);
    msg = pbw_message_new_ctx(desc->enc, desc->table->desc);
    if (msg == NULL) {
        return fail_memory(env);
    }
    IMG_OK(img_set_uint(env, msg, f.i32,
                        (uint64_t)(int64_t)(int32_t)img_u32(base +
                                                            IMG_OFF_I32),
                        desc->enc));
    IMG_OK(img_set_uint(env, msg, f.i64, img_u64(base + IMG_OFF_I64),
                        desc->enc));
    IMG_OK(img_set_uint(env, msg, f.u32, img_u32(base + IMG_OFF_U32),
                        desc->enc));
    IMG_OK(img_set_uint(env, msg, f.u64, img_u64(base + IMG_OFF_U64),
                        desc->enc));
    IMG_OK(img_set_uint(env, msg, f.s32,
                        (uint64_t)(int64_t)(int32_t)img_u32(base +
                                                            IMG_OFF_S32),
                        desc->enc));
    IMG_OK(img_set_uint(env, msg, f.s64, img_u64(base + IMG_OFF_S64),
                        desc->enc));
    IMG_OK(img_set_uint(env, msg, f.fx32, img_u32(base + IMG_OFF_FX32),
                        desc->enc));
    IMG_OK(img_set_uint(env, msg, f.fx64, img_u64(base + IMG_OFF_FX64),
                        desc->enc));
    IMG_OK(img_set_uint(env, msg, f.sfx32,
                        (uint64_t)(int64_t)(int32_t)img_u32(base +
                                                            IMG_OFF_SFX32),
                        desc->enc));
    IMG_OK(img_set_uint(env, msg, f.sfx64, img_u64(base + IMG_OFF_SFX64),
                        desc->enc));
    IMG_OK(img_set_uint(env, msg, f.f32, img_u32(base + IMG_OFF_F32),
                        desc->enc));
    IMG_OK(img_set_uint(env, msg, f.f64, img_u64(base + IMG_OFF_F64),
                        desc->enc));
    IMG_OK(img_set_uint(env, msg, f.flag, base[IMG_OFF_FLAG] != 0,
                        desc->enc));
    IMG_OK(img_set_uint(env, msg, f.color,
                        (uint64_t)(int64_t)(int32_t)img_u32(base +
                                                            IMG_OFF_COLOR),
                        desc->enc));
    if (base[IMG_OFF_OPT_I32_PRESENT] != 0) {
        IMG_OK(img_set_uint(
            env, msg, f.opt_i32,
            (uint64_t)(int64_t)(int32_t)img_u32(base + IMG_OFF_OPT_I32),
            desc->enc));
    }
    r.pos = IMG_HEAD_LEN;
    {
        /* str */
        uint32_t len;

        if (!img_avail(&r, 4)) {
            return fail_argument(env, "encodeImage: image too small");
        }
        len = img_u32(base + r.pos);
        r.pos += 4;
        if (!img_avail(&r, len)) {
            return fail_argument(env, "encodeImage: image too small");
        }
        IMG_OK(img_set_bytes(env, msg, f.str, base + r.pos, len, desc->enc));
        r.pos += len;
    }
    {
        /* inner */
        uint32_t len;
        int32_t value;
        pbw_message *child;

        if (!img_avail(&r, 4)) {
            return fail_argument(env, "encodeImage: image too small");
        }
        len = img_u32(base + r.pos);
        r.pos += 4;
        if (!img_avail(&r, (size_t)len + 4)) {
            return fail_argument(env, "encodeImage: image too small");
        }
        child = pbw_message_set_message(msg, f.inner->number);
        if (child == NULL) {
            return fail_argument(env, "cannot set the nested message field");
        }
        IMG_OK(img_set_bytes(env, child, f.inner_name, base + r.pos, len,
                             desc->enc));
        r.pos += len;
        value = (int32_t)img_u32(base + r.pos);
        r.pos += 4;
        IMG_OK(img_set_uint(env, child, f.inner_value,
                            (uint64_t)(int64_t)value, desc->enc));
    }
    {
        /* nums */
        if (!img_avail(&r, 4)) {
            return fail_argument(env, "encodeImage: image too small");
        }
        count = img_u32(base + r.pos);
        r.pos += 4;
        if (count > (r.len - r.pos) / 4) {
            return fail_argument(env, "encodeImage: image too small");
        }
        for (i = 0; i < count; i++) {
            int32_t value = (int32_t)img_u32(base + r.pos);

            r.pos += 4;
            IMG_OK(img_set_uint(env, msg, f.nums, (uint64_t)(int64_t)value,
                                desc->enc));
        }
    }
    {
        /* tags */
        if (!img_avail(&r, 4)) {
            return fail_argument(env, "encodeImage: image too small");
        }
        count = img_u32(base + r.pos);
        r.pos += 4;
        for (i = 0; i < count; i++) {
            uint32_t len;

            if (!img_avail(&r, 4)) {
                return fail_argument(env, "encodeImage: image too small");
            }
            len = img_u32(base + r.pos);
            r.pos += 4;
            if (!img_avail(&r, len)) {
                return fail_argument(env, "encodeImage: image too small");
            }
            IMG_OK(img_set_bytes(env, msg, f.tags, base + r.pos, len,
                                 desc->enc));
            r.pos += len;
        }
    }
    {
        /* inners */
        if (!img_avail(&r, 4)) {
            return fail_argument(env, "encodeImage: image too small");
        }
        count = img_u32(base + r.pos);
        r.pos += 4;
        for (i = 0; i < count; i++) {
            uint32_t len;
            int32_t value;
            pbw_message *child;

            if (!img_avail(&r, 4)) {
                return fail_argument(env, "encodeImage: image too small");
            }
            len = img_u32(base + r.pos);
            r.pos += 4;
            if (!img_avail(&r, (size_t)len + 4)) {
                return fail_argument(env, "encodeImage: image too small");
            }
            child = pbw_message_set_message(msg, f.inners->number);
            if (child == NULL) {
                return fail_argument(env,
                                     "cannot set the nested message field");
            }
            IMG_OK(img_set_bytes(env, child, f.inner_name, base + r.pos, len,
                                 desc->enc));
            r.pos += len;
            value = (int32_t)img_u32(base + r.pos);
            r.pos += 4;
            IMG_OK(img_set_uint(env, child, f.inner_value,
                                (uint64_t)(int64_t)value, desc->enc));
        }
    }
    if (base[IMG_OFF_OPT_STR_PRESENT] != 0) {
        /* opt_str */
        uint32_t len;

        if (!img_avail(&r, 4)) {
            return fail_argument(env, "encodeImage: image too small");
        }
        len = img_u32(base + r.pos);
        r.pos += 4;
        if (!img_avail(&r, len)) {
            return fail_argument(env, "encodeImage: image too small");
        }
        IMG_OK(img_set_bytes(env, msg, f.opt_str, base + r.pos, len,
                             desc->enc));
        r.pos += len;
    }
    {
        /* flags */
        if (!img_avail(&r, 4)) {
            return fail_argument(env, "encodeImage: image too small");
        }
        count = img_u32(base + r.pos);
        r.pos += 4;
        if (!img_avail(&r, count)) {
            return fail_argument(env, "encodeImage: image too small");
        }
        for (i = 0; i < count; i++) {
            IMG_OK(img_set_uint(env, msg, f.flags, base[r.pos] != 0,
                                desc->enc));
            r.pos += 1;
        }
    }
    if (r.pos != r.len) {
        return fail_argument(env, "encodeImage: image size mismatch");
    }
    status = pbw_encode_ctx(desc->enc, msg, &desc->enc_data, &desc->enc_len);
    if (status != PBW_OK) {
        desc->enc_valid = 0;
        return fail_status(env, "encode", status);
    }
    /* One crossing: the bytes are copied out here, not through the
     * scratch/length/copyEncoded two-step encodeObject uses. */
    if (napi_create_buffer_copy(env, desc->enc_len, desc->enc_data, NULL,
                                &out) != napi_ok) {
        return NULL;
    }
    return out;
#undef IMG_OK
}

/* Present scalar bits of a tree field, 0 when the field is absent (the spike
 * rule for singulars the payload always carries: see the layout comment). */
static uint64_t
img_tree_uint(const pbw_message *msg, field_entry *entry)
{
    const pbw_value *value = pbw_message_get_field(msg, entry->field);

    return value == NULL ? 0 : pbw_value_get_uint(value);
}

static int
img_size_add(napi_env env, size_t *total, size_t n)
{
    if (n > SIZE_MAX - *total) {
        fail_argument(env, "decodeImage: message too large");
        return 0;
    }
    *total += n;
    return 1;
}

/* The ArrayBuffer size a decoded tree fills: the fixed header plus every
 * variable payload the layout's tail carries. */
static int
img_measure(napi_env env, pbw_message *msg, const img_fields *f, size_t *total)
{
    const pbw_value *str = pbw_message_get_field(msg, f->str->field);
    const pbw_value *inner = pbw_message_get_field(msg, f->inner->field);
    const pbw_value *nums = pbw_message_get_field(msg, f->nums->field);
    const pbw_value *tags = pbw_message_get_field(msg, f->tags->field);
    const pbw_value *inners = pbw_message_get_field(msg, f->inners->field);
    const pbw_value *opt_str = pbw_message_get_field(msg, f->opt_str->field);
    const pbw_value *flags = pbw_message_get_field(msg, f->flags->field);
    size_t n = 0;
    size_t i;
    size_t count;
    uint32_t u32;
#define IMG_ADD(x)                            \
    do {                                      \
        if (!img_size_add(env, total, (x))) { \
            return 0;                         \
        }                                     \
    } while (0)
    if (str != NULL) {
        (void)pbw_value_get_bytes(str, &n);
    }
    if (n > 0xffffffffu) {
        fail_argument(env, "decodeImage: field too large");
        return 0;
    }
    IMG_ADD(4 + n);
    n = 0;
    if (inner != NULL && pbw_value_get_kind(inner) == PBW_VALUE_MESSAGE) {
        pbw_message *child = pbw_value_get_message(inner);
        const pbw_value *name =
            pbw_message_get_field(child, f->inner_name->field);

        if (name != NULL) {
            (void)pbw_value_get_bytes(name, &n);
        }
    }
    if (n > 0xffffffffu) {
        fail_argument(env, "decodeImage: field too large");
        return 0;
    }
    IMG_ADD(4 + n + 4);
    count = nums == NULL ? 0 : pbw_value_count(nums);
    if (count > 0xffffffffu) {
        fail_argument(env, "decodeImage: field too large");
        return 0;
    }
    u32 = (uint32_t)count;
    IMG_ADD(4 + ((size_t)u32) * 4);
    count = tags == NULL ? 0 : pbw_value_count(tags);
    if (count > 0xffffffffu) {
        fail_argument(env, "decodeImage: field too large");
        return 0;
    }
    IMG_ADD(4);
    for (i = 0; i < count; i++) {
        const pbw_value *item = pbw_value_at(tags, i);

        n = 0;
        if (item != NULL) {
            (void)pbw_value_get_bytes(item, &n);
        }
        if (n > 0xffffffffu) {
            fail_argument(env, "decodeImage: field too large");
            return 0;
        }
        IMG_ADD(4 + n);
    }
    count = inners == NULL ? 0 : pbw_value_count(inners);
    if (count > 0xffffffffu) {
        fail_argument(env, "decodeImage: field too large");
        return 0;
    }
    IMG_ADD(4);
    for (i = 0; i < count; i++) {
        const pbw_value *item = pbw_value_at(inners, i);

        n = 0;
        if (item != NULL &&
            pbw_value_get_kind(item) == PBW_VALUE_MESSAGE) {
            pbw_message *child = pbw_value_get_message(item);
            const pbw_value *name =
                pbw_message_get_field(child, f->inner_name->field);

            if (name != NULL) {
                (void)pbw_value_get_bytes(name, &n);
            }
        }
        if (n > 0xffffffffu) {
            fail_argument(env, "decodeImage: field too large");
            return 0;
        }
        IMG_ADD(4 + n + 4);
    }
    if (opt_str != NULL) {
        n = 0;
        (void)pbw_value_get_bytes(opt_str, &n);
        if (n > 0xffffffffu) {
            fail_argument(env, "decodeImage: field too large");
            return 0;
        }
        IMG_ADD(4 + n);
    }
    count = flags == NULL ? 0 : pbw_value_count(flags);
    if (count > 0xffffffffu) {
        fail_argument(env, "decodeImage: field too large");
        return 0;
    }
    IMG_ADD(4);
    IMG_ADD(count);
    return 1;
#undef IMG_ADD
}

/* Fills a pre-sized image from a decoded tree; sizes come from img_measure,
 * so no check can fail here. */
static void
img_fill(pbw_message *msg, const img_fields *f, uint8_t *base)
{
    const pbw_value *str = pbw_message_get_field(msg, f->str->field);
    const pbw_value *inner = pbw_message_get_field(msg, f->inner->field);
    const pbw_value *nums = pbw_message_get_field(msg, f->nums->field);
    const pbw_value *tags = pbw_message_get_field(msg, f->tags->field);
    const pbw_value *inners = pbw_message_get_field(msg, f->inners->field);
    const pbw_value *opt_i32 = pbw_message_get_field(msg, f->opt_i32->field);
    const pbw_value *opt_str = pbw_message_get_field(msg, f->opt_str->field);
    const pbw_value *flags = pbw_message_get_field(msg, f->flags->field);
    size_t pos = IMG_HEAD_LEN;
    uint32_t u32;
    size_t i;
    size_t count;
#define IMG_TAKE_BYTES(value, dst, len)                         \
    do {                                                        \
        size_t img_need = 0;                                    \
        const void *img_from = NULL;                            \
                                                                \
        if ((value) != NULL) {                                  \
            img_from = pbw_value_get_bytes((value), &img_need); \
        }                                                       \
        (dst) = img_from;                                       \
        (len) = img_need;                                       \
    } while (0)
    img_put_u32(base + IMG_OFF_I32, (uint32_t)img_tree_uint(msg, f->i32));
    img_put_u64(base + IMG_OFF_I64, img_tree_uint(msg, f->i64));
    img_put_u32(base + IMG_OFF_U32, (uint32_t)img_tree_uint(msg, f->u32));
    img_put_u64(base + IMG_OFF_U64, img_tree_uint(msg, f->u64));
    img_put_u32(base + IMG_OFF_S32, (uint32_t)img_tree_uint(msg, f->s32));
    img_put_u64(base + IMG_OFF_S64, img_tree_uint(msg, f->s64));
    img_put_u32(base + IMG_OFF_FX32, (uint32_t)img_tree_uint(msg, f->fx32));
    img_put_u64(base + IMG_OFF_FX64, img_tree_uint(msg, f->fx64));
    img_put_u32(base + IMG_OFF_SFX32, (uint32_t)img_tree_uint(msg, f->sfx32));
    img_put_u64(base + IMG_OFF_SFX64, img_tree_uint(msg, f->sfx64));
    img_put_u32(base + IMG_OFF_F32, (uint32_t)img_tree_uint(msg, f->f32));
    img_put_u64(base + IMG_OFF_F64, img_tree_uint(msg, f->f64));
    base[IMG_OFF_FLAG] = img_tree_uint(msg, f->flag) != 0;
    img_put_u32(base + IMG_OFF_COLOR, (uint32_t)img_tree_uint(msg, f->color));
    base[IMG_OFF_OPT_I32_PRESENT] = opt_i32 != NULL;
    img_put_u32(base + IMG_OFF_OPT_I32,
                opt_i32 == NULL ? 0 : (uint32_t)pbw_value_get_uint(opt_i32));
    base[IMG_OFF_OPT_STR_PRESENT] = opt_str != NULL;
    {
        const void *from;
        size_t need;

        IMG_TAKE_BYTES(str, from, need);
        u32 = (uint32_t)need;
        img_put_u32(base + pos, u32);
        if (need > 0) {
            memcpy(base + pos + 4, from, need);
        }
        pos += 4 + need;
    }
    {
        const void *from = NULL;
        size_t need = 0;
        uint64_t bits = 0;

        if (inner != NULL && pbw_value_get_kind(inner) == PBW_VALUE_MESSAGE) {
            pbw_message *child = pbw_value_get_message(inner);
            const pbw_value *name =
                pbw_message_get_field(child, f->inner_name->field);
            const pbw_value *value =
                pbw_message_get_field(child, f->inner_value->field);

            if (name != NULL) {
                from = pbw_value_get_bytes(name, &need);
            }
            if (value != NULL) {
                bits = pbw_value_get_uint(value);
            }
        }
        u32 = (uint32_t)need;
        img_put_u32(base + pos, u32);
        if (need > 0) {
            memcpy(base + pos + 4, from, need);
        }
        pos += 4 + need;
        img_put_u32(base + pos, (uint32_t)bits);
        pos += 4;
    }
    count = nums == NULL ? 0 : pbw_value_count(nums);
    u32 = (uint32_t)count;
    img_put_u32(base + pos, u32);
    pos += 4;
    for (i = 0; i < count; i++) {
        const pbw_value *item = pbw_value_at(nums, i);

        img_put_u32(base + pos, item == NULL
                                    ? 0
                                    : (uint32_t)pbw_value_get_uint(item));
        pos += 4;
    }
    count = tags == NULL ? 0 : pbw_value_count(tags);
    u32 = (uint32_t)count;
    img_put_u32(base + pos, u32);
    pos += 4;
    for (i = 0; i < count; i++) {
        const pbw_value *item = pbw_value_at(tags, i);
        const void *from;
        size_t need;

        IMG_TAKE_BYTES(item, from, need);
        u32 = (uint32_t)need;
        img_put_u32(base + pos, u32);
        if (need > 0) {
            memcpy(base + pos + 4, from, need);
        }
        pos += 4 + need;
    }
    count = inners == NULL ? 0 : pbw_value_count(inners);
    u32 = (uint32_t)count;
    img_put_u32(base + pos, u32);
    pos += 4;
    for (i = 0; i < count; i++) {
        const pbw_value *item = pbw_value_at(inners, i);
        const void *from = NULL;
        size_t need = 0;
        uint64_t bits = 0;

        if (item != NULL && pbw_value_get_kind(item) == PBW_VALUE_MESSAGE) {
            pbw_message *child = pbw_value_get_message(item);
            const pbw_value *name =
                pbw_message_get_field(child, f->inner_name->field);
            const pbw_value *value =
                pbw_message_get_field(child, f->inner_value->field);

            if (name != NULL) {
                from = pbw_value_get_bytes(name, &need);
            }
            if (value != NULL) {
                bits = pbw_value_get_uint(value);
            }
        }
        u32 = (uint32_t)need;
        img_put_u32(base + pos, u32);
        if (need > 0) {
            memcpy(base + pos + 4, from, need);
        }
        pos += 4 + need;
        img_put_u32(base + pos, (uint32_t)bits);
        pos += 4;
    }
    if (opt_str != NULL) {
        const void *from;
        size_t need;

        IMG_TAKE_BYTES(opt_str, from, need);
        u32 = (uint32_t)need;
        img_put_u32(base + pos, u32);
        if (need > 0) {
            memcpy(base + pos + 4, from, need);
        }
        pos += 4 + need;
    }
    count = flags == NULL ? 0 : pbw_value_count(flags);
    u32 = (uint32_t)count;
    img_put_u32(base + pos, u32);
    pos += 4;
    for (i = 0; i < count; i++) {
        const pbw_value *item = pbw_value_at(flags, i);

        base[pos] = item != NULL && pbw_value_get_uint(item) != 0;
        pos += 1;
    }
#undef IMG_TAKE_BYTES
}

/* Validates the wire, decodes it into a tree and fills a fresh ArrayBuffer:
 * one N-API crossing, no per-field object writes. */
static napi_value
decode_image(napi_env env, napi_callback_info info)
{
    napi_value argv[3];
    size_t argc = 3;
    napi_value out = NULL;
    desc_handle *desc;
    void *wire = NULL;
    size_t wire_len = 0;
    pbw_limits storage;
    const pbw_limits *limits;
    pbw_message *msg = NULL;
    pbw_status status;
    img_fields f;
    size_t total = IMG_HEAD_LEN;
    void *image = NULL;

    if (napi_get_cb_info(env, info, &argc, argv, NULL, NULL) != napi_ok ||
        argc < 2) {
        napi_throw_type_error(env, NULL,
                              "pbwire: expected (desc, bytes, limits?)");
        return NULL;
    }
    if (!unwrap(env, argv[0], (void **)&desc, "message descriptor handle")) {
        return NULL;
    }
    if (strcmp(pbw_descriptor_name(desc->table->desc),
               "structtest.Scalars3") != 0) {
        return fail_argument(env,
                             "decodeImage supports structtest.Scalars3 only");
    }
    if (!parse_limits(env, argv, argc, 2, &storage, &limits)) {
        return NULL;
    }
    if (!img_resolve(env, desc->table, &f)) {
        return NULL;
    }
    if (napi_get_buffer_info(env, argv[1], &wire, &wire_len) != napi_ok) {
        return fail_argument(env, "decodeImage expects a Buffer");
    }
    if (limits != NULL) {
        status = pbw_decode_limits(desc->table->desc, wire, wire_len, limits,
                                   &msg);
    } else {
        status = pbw_decode(desc->table->desc, wire, wire_len, &msg);
    }
    if (status != PBW_OK) {
        return fail_status(env, "decode", status);
    }
    if (!img_measure(env, msg, &f, &total)) {
        pbw_message_free(msg);
        return NULL;
    }
    if (napi_create_arraybuffer(env, total, &image, &out) != napi_ok) {
        pbw_message_free(msg);
        return fail_memory(env);
    }
    img_fill(msg, &f, image);
    pbw_message_free(msg);
    return out;
}

static napi_value
message_free(napi_env env, napi_callback_info info)
{
    napi_value argv[1];
    napi_value out;
    message_handle *handle;

    if (!get_args(env, info, argv, 1)) {
        return NULL;
    }
    if (!unwrap(env, argv[0], (void **)&handle, "message handle")) {
        return NULL;
    }
    pbw_message_free(handle->msg);
    handle->msg = NULL;
    handle->freed = 1;
    free_reference(env, &handle->desc_ref);
    /* The descriptor reference is gone: drop the pointer so nothing can
     * reach a collectable descriptor through a freed handle. */
    handle->desc = NULL;
    TRY(napi_get_undefined(env, &out));
    return out;
}

/* JSON text of a tree (protobuf JSON mapping). */
static napi_value
message_to_json(napi_env env, napi_callback_info info)
{
    napi_value argv[1];
    napi_value out = NULL;
    message_handle *handle;
    char *text = NULL;
    size_t len = 0;
    pbw_status status;

    if (!get_args(env, info, argv, 1)) {
        return NULL;
    }
    if (!unwrap_message(env, argv[0], &handle)) {
        return NULL;
    }
    status = pbw_json_print(handle->msg, &text, &len);
    if (status != PBW_OK) {
        return fail_status(env, "messageToJson", status);
    }
    if (napi_create_string_utf8(env, text, len, &out) != napi_ok) {
        pbw_free(text);
        return NULL;
    }
    pbw_free(text);
    return out;
}

/* Parses JSON mapping text into a new tree; an optional third boolean
 * argument skips unknown field names instead of failing. */
static napi_value
json_to_message(napi_env env, napi_callback_info info)
{
    napi_value argv[4];
    size_t argc = 4;
    desc_handle *desc;
    message_handle *handle;
    pbw_message *msg = NULL;
    string_buf buf;
    const char *text;
    pbw_status status;
    pbw_json_parse_options opts;
    pbw_limits storage;
    const pbw_limits *limits;
    bool ignore = false;

    if (napi_get_cb_info(env, info, &argc, argv, NULL, NULL) != napi_ok ||
        argc < 2) {
        napi_throw_type_error(env, NULL, "pbwire: expected (desc, json)");
        return NULL;
    }
    if (!unwrap(env, argv[0], (void **)&desc, "message descriptor handle")) {
        return NULL;
    }
    if (argc > 2) {
        napi_valuetype want = napi_undefined;

        if (napi_typeof(env, argv[2], &want) != napi_ok ||
            want != napi_boolean ||
            napi_get_value_bool(env, argv[2], &ignore) != napi_ok) {
            napi_throw_type_error(env, NULL,
                                  "pbwire: ignore flag must be a boolean");
            return NULL;
        }
    }
    if (!parse_limits(env, argv, argc, 3, &storage, &limits)) {
        return NULL;
    }
    text = string_bytes(env, argv[1], &buf);
    if (text == NULL) {
        return NULL;
    }
    opts.ignore_unknown_fields = ignore;
    if (limits != NULL) {
        status = pbw_json_parse_limits(desc->table->desc, text, buf.len,
                                       limits, &opts, &msg);
    } else {
        status = pbw_json_parse_opts(desc->table->desc, text, buf.len, &opts,
                                     &msg);
    }
    string_free(&buf, text);
    if (status != PBW_OK) {
        return fail_status(env, "jsonToMessage", status);
    }
    handle = calloc(1, sizeof(*handle));
    if (handle == NULL) {
        pbw_message_free(msg);
        return fail_memory(env);
    }
    handle->msg = msg;
    return new_message_value(env, desc, argv[0], handle);
}

/* Text format of a tree. */
static napi_value
message_to_text(napi_env env, napi_callback_info info)
{
    napi_value argv[2];
    size_t argc = 2;
    napi_value out = NULL;
    message_handle *handle;
    char *text = NULL;
    size_t len = 0;
    pbw_status status;
    /* Absent means print (the binding's standing behavior); the testee
     * passes the runner's print_unknown_fields flag explicitly. */
    bool print_unknown = true;

    if (napi_get_cb_info(env, info, &argc, argv, NULL, NULL) != napi_ok ||
        argc < 1) {
        napi_throw_type_error(env, NULL, "pbwire: expected (message)");
        return NULL;
    }
    if (!unwrap_message(env, argv[0], &handle)) {
        return NULL;
    }
    if (argc > 1) {
        napi_valuetype want = napi_undefined;

        if (napi_typeof(env, argv[1], &want) != napi_ok ||
            want != napi_boolean ||
            napi_get_value_bool(env, argv[1], &print_unknown) != napi_ok) {
            napi_throw_type_error(env, NULL,
                                  "pbwire: print flag must be a boolean");
            return NULL;
        }
    }
    status = pbw_text_print_opts(handle->msg, print_unknown, &text, &len);
    if (status != PBW_OK) {
        return fail_status(env, "messageToText", status);
    }
    if (napi_create_string_utf8(env, text, len, &out) != napi_ok) {
        pbw_free(text);
        return NULL;
    }
    pbw_free(text);
    return out;
}

/* Parses text format into a new tree. */
static napi_value
text_to_message(napi_env env, napi_callback_info info)
{
    napi_value argv[3];
    size_t argc = 3;
    desc_handle *desc;
    message_handle *handle;
    pbw_message *msg = NULL;
    string_buf buf;
    const char *text;
    pbw_status status;
    pbw_limits storage;
    const pbw_limits *limits;

    if (napi_get_cb_info(env, info, &argc, argv, NULL, NULL) != napi_ok ||
        argc < 2) {
        napi_throw_type_error(env, NULL,
                              "pbwire: expected (desc, text, limits?)");
        return NULL;
    }
    if (!unwrap(env, argv[0], (void **)&desc, "message descriptor handle")) {
        return NULL;
    }
    if (!parse_limits(env, argv, argc, 2, &storage, &limits)) {
        return NULL;
    }
    text = string_bytes(env, argv[1], &buf);
    if (text == NULL) {
        return NULL;
    }
    if (limits != NULL) {
        status = pbw_text_parse_limits(desc->table->desc, text, buf.len,
                                       limits, &msg);
    } else {
        status = pbw_text_parse(desc->table->desc, text, buf.len, &msg);
    }
    string_free(&buf, text);
    if (status != PBW_OK) {
        return fail_status(env, "textToMessage", status);
    }
    handle = calloc(1, sizeof(*handle));
    if (handle == NULL) {
        pbw_message_free(msg);
        return fail_memory(env);
    }
    handle->msg = msg;
    return new_message_value(env, desc, argv[0], handle);
}

/* Presence of one field by name: set (even to a zero) reads true. */
static napi_value
message_has(napi_env env, napi_callback_info info)
{
    napi_value argv[2];
    napi_value out = NULL;
    message_handle *handle;
    field_entry *entry;
    int present;

    if (!get_args(env, info, argv, 2)) {
        return NULL;
    }
    if (!unwrap_message(env, argv[0], &handle)) {
        return NULL;
    }
    entry = field_by_name(env, handle, argv[1]);
    if (entry == NULL) {
        return NULL;
    }
    present = pbw_message_get_field(handle->msg, entry->field) != NULL;
    if (napi_get_boolean(env, present, &out) != napi_ok) {
        return NULL;
    }
    return out;
}

/* One length-delimited frame (varint length + message) as a Buffer. */
static napi_value
encode_delimited(napi_env env, napi_callback_info info)
{
    napi_value argv[1];
    napi_value out = NULL;
    message_handle *handle;
    uint8_t *wire = NULL;
    size_t wire_len = 0;
    pbw_status status;

    if (!get_args(env, info, argv, 1)) {
        return NULL;
    }
    if (!unwrap_message(env, argv[0], &handle)) {
        return NULL;
    }
    status = pbw_encode_delimited(handle->msg, &wire, &wire_len);
    if (status != PBW_OK) {
        return fail_status(env, "encodeDelimited", status);
    }
    if (napi_create_buffer_copy(env, wire_len, wire, NULL, &out) != napi_ok) {
        pbw_free(wire);
        return NULL;
    }
    pbw_free(wire);
    return out;
}

/* One frame from a Buffer: {message, read}, where read counts the length
 * prefix plus the frame, so concatenated frames decode in a loop. */
static napi_value
decode_delimited(napi_env env, napi_callback_info info)
{
    napi_value argv[3];
    size_t argc = 3;
    napi_value out = NULL;
    napi_value message_value = NULL;
    napi_value read_value = NULL;
    desc_handle *desc;
    message_handle *handle;
    pbw_message *msg = NULL;
    void *data = NULL;
    size_t len = 0;
    size_t consumed = 0;
    pbw_limits storage;
    const pbw_limits *limits;
    pbw_status status;

    if (napi_get_cb_info(env, info, &argc, argv, NULL, NULL) != napi_ok ||
        argc < 2) {
        napi_throw_type_error(env, NULL,
                              "pbwire: expected (desc, bytes, limits?)");
        return NULL;
    }
    if (!unwrap(env, argv[0], (void **)&desc, "message descriptor handle")) {
        return NULL;
    }
    if (!parse_limits(env, argv, argc, 2, &storage, &limits)) {
        return NULL;
    }
    if (napi_get_buffer_info(env, argv[1], &data, &len) != napi_ok) {
        return fail_argument(env, "decodeDelimited expects a Buffer");
    }
    /* `read` is a JS uint32: past 4 GiB it would wrap and the caller's
     * frame loop would stop making progress. */
    if ((uint64_t)len > 0xffffffffull) {
        return fail_argument(env, "decodeDelimited expects a message below 4 GiB");
    }
    status = pbw_decode_delimited_limits(desc->table->desc, data, len,
                                         limits, &msg, &consumed);
    if (status != PBW_OK) {
        return fail_status(env, "decodeDelimited", status);
    }
    handle = calloc(1, sizeof(*handle));
    if (handle == NULL) {
        pbw_message_free(msg);
        return fail_memory(env);
    }
    handle->msg = msg;
    message_value = new_message_value(env, desc, argv[0], handle);
    if (message_value == NULL) {
        return NULL;
    }
    if (napi_create_object(env, &out) != napi_ok ||
        napi_set_named_property(env, out, "message", message_value) !=
            napi_ok ||
        napi_create_uint32(env, (uint32_t)consumed, &read_value) != napi_ok ||
        napi_set_named_property(env, out, "read", read_value) != napi_ok) {
        return NULL;
    }
    return out;
}

/* Deterministic bytes of a tree: map entries sorted by key, so equal
 * messages encode to equal bytes regardless of insertion order. */
static napi_value
encode_deterministic(napi_env env, napi_callback_info info)
{
    napi_value argv[1];
    napi_value out = NULL;
    message_handle *handle;
    uint8_t *wire = NULL;
    size_t wire_len = 0;
    pbw_status status;

    if (!get_args(env, info, argv, 1)) {
        return NULL;
    }
    if (!unwrap_message(env, argv[0], &handle)) {
        return NULL;
    }
    status = pbw_encode_deterministic(handle->msg, &wire, &wire_len);
    if (status != PBW_OK) {
        return fail_status(env, "encodeDeterministic", status);
    }
    if (napi_create_buffer_copy(env, wire_len, wire, NULL, &out) != napi_ok) {
        pbw_free(wire);
        return NULL;
    }
    pbw_free(wire);
    return out;
}

/* Exact byte count pbw_encode() would write for this tree. */
static napi_value
encoded_size(napi_env env, napi_callback_info info)
{
    napi_value argv[1];
    napi_value out = NULL;
    message_handle *handle;
    size_t len = 0;
    pbw_status status;

    if (!get_args(env, info, argv, 1)) {
        return NULL;
    }
    if (!unwrap_message(env, argv[0], &handle)) {
        return NULL;
    }
    status = pbw_message_encoded_size(handle->msg, &len);
    if (status != PBW_OK) {
        return fail_status(env, "encodedSize", status);
    }
    if (napi_create_double(env, (double)len, &out) != napi_ok) {
        return NULL;
    }
    return out;
}

/* Encode into a caller-provided Buffer: the cap is the hard limit, so a
 * short Buffer fails instead of growing. Returns the bytes written. */
static napi_value
encode_to(napi_env env, napi_callback_info info)
{
    napi_value argv[2];
    napi_value out = NULL;
    message_handle *handle;
    void *data = NULL;
    size_t cap = 0;
    size_t len = 0;
    pbw_status status;

    if (!get_args(env, info, argv, 2)) {
        return NULL;
    }
    if (!unwrap_message(env, argv[0], &handle)) {
        return NULL;
    }
    if (napi_get_buffer_info(env, argv[1], &data, &cap) != napi_ok) {
        return fail_argument(env, "encodeTo expects a Buffer");
    }
    status = pbw_encode_to(handle->msg, data, cap, &len);
    if (status != PBW_OK) {
        return fail_status(env, "encodeTo", status);
    }
    if (napi_create_double(env, (double)len, &out) != napi_ok) {
        return NULL;
    }
    return out;
}

/* One delimited frame into a caller-provided Buffer. */
static napi_value
encode_delimited_to(napi_env env, napi_callback_info info)
{
    napi_value argv[2];
    napi_value out = NULL;
    message_handle *handle;
    void *data = NULL;
    size_t cap = 0;
    size_t len = 0;
    pbw_status status;

    if (!get_args(env, info, argv, 2)) {
        return NULL;
    }
    if (!unwrap_message(env, argv[0], &handle)) {
        return NULL;
    }
    if (napi_get_buffer_info(env, argv[1], &data, &cap) != napi_ok) {
        return fail_argument(env, "encodeDelimitedTo expects a Buffer");
    }
    status = pbw_encode_delimited_to(handle->msg, data, cap, &len);
    if (status != PBW_OK) {
        return fail_status(env, "encodeDelimitedTo", status);
    }
    if (napi_create_double(env, (double)len, &out) != napi_ok) {
        return NULL;
    }
    return out;
}

typedef struct {
    napi_env env;
    napi_value callback;
    napi_value undefined;
    int abort;  /* the callback returned false */
    int failed; /* the N-API call failed; an exception is pending */
} sink_state;

/* Hands each 8 KiB chunk to the JS callback as a Buffer; a false return
 * aborts the encode. */
static pbw_status
sink_trampoline(const uint8_t *data, size_t len, void *opaque)
{
    sink_state *state = opaque;
    napi_env env = state->env;
    napi_value chunk = NULL;
    napi_value result = NULL;
    bool proceed = false;

    if (napi_create_buffer_copy(env, len, data, NULL, &chunk) != napi_ok ||
        napi_call_function(env, state->undefined, state->callback, 1,
                           &chunk, &result) != napi_ok) {
        state->failed = 1;
        return PBW_ERR_NOMEM;
    }
    if (napi_get_value_bool(env, result, &proceed) != napi_ok) {
        state->failed = 1;
        return PBW_ERR_NOMEM;
    }
    if (!proceed) {
        state->abort = 1;
        return PBW_ERR_INVALID;
    }
    return PBW_OK;
}

/* Streams the encoding through the JS callback in chunks. */
static napi_value
encode_sink(napi_env env, napi_callback_info info)
{
    napi_value argv[2];
    napi_value out;
    message_handle *handle;
    napi_valuetype kind;
    sink_state state;
    pbw_status status;

    if (!get_args(env, info, argv, 2)) {
        return NULL;
    }
    if (!unwrap_message(env, argv[0], &handle)) {
        return NULL;
    }
    if (napi_typeof(env, argv[1], &kind) != napi_ok ||
        kind != napi_function) {
        return fail_argument(env, "encodeSink expects a function");
    }
    memset(&state, 0, sizeof(state));
    state.env = env;
    state.callback = argv[1];
    if (napi_get_undefined(env, &state.undefined) != napi_ok) {
        return NULL;
    }
    status = pbw_encode_sink(handle->msg, sink_trampoline, &state);
    if (state.failed) {
        return NULL;
    }
    if (state.abort) {
        return fail_argument(env, "encodeSink aborted by the callback");
    }
    if (status != PBW_OK) {
        return fail_status(env, "encodeSink", status);
    }
    TRY(napi_get_undefined(env, &out));
    return out;
}

typedef struct {
    uint32_t number;
    uint32_t wire_type;
    const pbw_field *field;
    uint64_t u64;        /* VARINT/32BIT/64BIT raw bits */
    const uint8_t *data; /* LEN payload or whole group; into the input */
    size_t len;
} visit_event;

typedef struct {
    visit_event *items;
    size_t count;
    size_t cap;
} visit_state;

/* One event per wire field, recorded without building a tree. The payload
 * still points into the caller's Buffer; it is copied when the JS array
 * is built, so no pinning outlives this call. */
static pbw_status
visit_collect(void *ud, const pbw_field *field, uint32_t number,
              uint32_t wire_type, const pbw_visit_value *value)
{
    visit_state *state = ud;
    visit_event *event;

    if (state->count == state->cap) {
        size_t cap = state->cap != 0 ? state->cap * 2 : 32;
        visit_event *items = realloc(state->items, cap * sizeof(*items));

        if (items == NULL) {
            return PBW_ERR_NOMEM;
        }
        state->items = items;
        state->cap = cap;
    }
    event = &state->items[state->count++];
    event->number = number;
    event->wire_type = wire_type;
    event->field = field;
    event->u64 = value->u64;
    event->data = value->data;
    event->len = value->len;
    return PBW_OK;
}

/* Single-level scan of wire bytes: an array of
 * {number, wireType, name, value}, where value is a BigInt for scalar
 * shapes and a Buffer for LEN payloads and groups. */
static napi_value
visit(napi_env env, napi_callback_info info)
{
    napi_value argv[3];
    size_t argc = 3;
    napi_value out = NULL;
    desc_handle *desc;
    void *data = NULL;
    size_t len = 0;
    pbw_limits storage;
    const pbw_limits *limits;
    visit_state state;
    size_t fail_off = 0;
    pbw_status status;
    size_t i;

    memset(&state, 0, sizeof(state));
    if (napi_get_cb_info(env, info, &argc, argv, NULL, NULL) != napi_ok ||
        argc < 2) {
        napi_throw_type_error(env, NULL,
                              "pbwire: expected (desc, bytes, limits?)");
        return NULL;
    }
    if (!unwrap(env, argv[0], (void **)&desc, "message descriptor handle")) {
        return NULL;
    }
    if (!parse_limits(env, argv, argc, 2, &storage, &limits)) {
        return NULL;
    }
    if (napi_get_buffer_info(env, argv[1], &data, &len) != napi_ok) {
        return fail_argument(env, "visit expects a Buffer");
    }
    status = pbw_visit_limits(desc->table->desc, data, len, limits,
                              visit_collect, &state, &fail_off);
    if (status != PBW_OK) {
        free(state.items);
        return fail_status(env, "visit", status);
    }
    if (napi_create_array_with_length(env, state.count, &out) != napi_ok) {
        free(state.items);
        return NULL;
    }
    for (i = 0; i < state.count; i++) {
        visit_event *event = &state.items[i];
        napi_value item = NULL;
        napi_value value = NULL;
        napi_value name = NULL;
        napi_value number = NULL;
        napi_value wire_type = NULL;
        uint32_t index = table_by_number(desc->table, event->number);

        if (event->wire_type == 0 || event->wire_type == 1 ||
            event->wire_type == 5) {
            if (napi_create_bigint_uint64(env, event->u64, &value) !=
                napi_ok) {
                goto fail;
            }
        } else if (napi_create_buffer_copy(env, event->len, event->data,
                                           NULL, &value) != napi_ok) {
            goto fail;
        }
        if (index != NOT_FOUND) {
            const char *field_name = desc->table->fields[index].name;

            if (napi_create_string_utf8(env, field_name, NAPI_AUTO_LENGTH,
                                        &name) != napi_ok) {
                goto fail;
            }
        } else if (napi_get_null(env, &name) != napi_ok) {
            goto fail;
        }
        if (napi_create_object(env, &item) != napi_ok ||
            napi_create_uint32(env, event->number, &number) != napi_ok ||
            napi_create_uint32(env, event->wire_type, &wire_type) !=
                napi_ok ||
            napi_set_named_property(env, item, "number", number) !=
                napi_ok ||
            napi_set_named_property(env, item, "wireType", wire_type) !=
                napi_ok ||
            napi_set_named_property(env, item, "name", name) != napi_ok ||
            napi_set_named_property(env, item, "value", value) != napi_ok ||
            napi_set_element(env, out, (uint32_t)i, item) != napi_ok) {
            goto fail;
        }
        continue;
    fail:
        free(state.items);
        return NULL;
    }
    free(state.items);
    return out;
}

/* Service names of a schema, for routing. */
static napi_value
services(napi_env env, napi_callback_info info)
{
    napi_value argv[1];
    napi_value out = NULL;
    schema_handle *schema;
    size_t count;
    size_t i;

    if (!get_args(env, info, argv, 1)) {
        return NULL;
    }
    if (!unwrap(env, argv[0], (void **)&schema, "schema handle")) {
        return NULL;
    }
    count = pbw_schema_service_count(schema->schema);
    if (napi_create_array_with_length(env, count, &out) != napi_ok) {
        return NULL;
    }
    for (i = 0; i < count; i++) {
        const pbw_service *svc = pbw_schema_service_at(schema->schema, i);
        napi_value name = NULL;

        if (svc == NULL ||
            napi_create_string_utf8(env, pbw_service_name(svc),
                                    NAPI_AUTO_LENGTH, &name) != napi_ok ||
            napi_set_element(env, out, (uint32_t)i, name) != napi_ok) {
            return NULL;
        }
    }
    return out;
}

/* One service with its methods and streaming flags. */
static napi_value
service(napi_env env, napi_callback_info info)
{
    napi_value argv[2];
    napi_value out = NULL;
    napi_value methods = NULL;
    napi_value name_value = NULL;
    schema_handle *schema;
    string_buf buf;
    const char *name;
    const pbw_service *svc;
    size_t count;
    size_t i;

    if (!get_args(env, info, argv, 2)) {
        return NULL;
    }
    if (!unwrap(env, argv[0], (void **)&schema, "schema handle")) {
        return NULL;
    }
    name = string_bytes(env, argv[1], &buf);
    if (name == NULL) {
        return NULL;
    }
    svc = pbw_schema_service_find(schema->schema, name);
    string_free(&buf, name);
    if (svc == NULL) {
        return fail_argument(env, "unknown service");
    }
    count = pbw_service_method_count(svc);
    if (napi_create_array_with_length(env, count, &methods) != napi_ok) {
        return NULL;
    }
    for (i = 0; i < count; i++) {
        const pbw_method *method = pbw_service_method_at(svc, i);
        napi_value item = NULL;
        napi_value prop = NULL;
        bool flag;

        if (method == NULL || napi_create_object(env, &item) != napi_ok) {
            return NULL;
        }
#define SET_STRING_PROP(key, text)                                        \
    do {                                                                  \
        if (napi_create_string_utf8(env, (text), NAPI_AUTO_LENGTH,        \
                                    &prop) != napi_ok ||                  \
            napi_set_named_property(env, item, (key), prop) != napi_ok) { \
            return NULL;                                                  \
        }                                                                 \
    } while (0)
        SET_STRING_PROP("name", pbw_method_name(method));
        SET_STRING_PROP("inputType", pbw_method_input_type(method));
        SET_STRING_PROP("outputType", pbw_method_output_type(method));
#undef SET_STRING_PROP
        flag = pbw_method_client_streaming(method) != 0;
        if (napi_get_boolean(env, flag, &prop) != napi_ok ||
            napi_set_named_property(env, item, "clientStreaming", prop) !=
                napi_ok) {
            return NULL;
        }
        flag = pbw_method_server_streaming(method) != 0;
        if (napi_get_boolean(env, flag, &prop) != napi_ok ||
            napi_set_named_property(env, item, "serverStreaming", prop) !=
                napi_ok) {
            return NULL;
        }
        if (napi_set_element(env, methods, (uint32_t)i, item) != napi_ok) {
            return NULL;
        }
    }
    if (napi_create_object(env, &out) != napi_ok ||
        napi_create_string_utf8(env, pbw_service_name(svc), NAPI_AUTO_LENGTH,
                                &name_value) != napi_ok ||
        napi_set_named_property(env, out, "name", name_value) != napi_ok ||
        napi_set_named_property(env, out, "methods", methods) != napi_ok) {
        return NULL;
    }
    return out;
}

/* Resolves a method to its input/output descriptors for routing. */
static napi_value
resolve(napi_env env, napi_callback_info info)
{
    napi_value argv[3];
    napi_value out = NULL;
    napi_value input_value = NULL;
    napi_value output_value = NULL;
    schema_handle *schema;
    string_buf svc_buf;
    string_buf method_buf;
    const char *svc_name;
    const char *method_name;
    const pbw_descriptor *in_desc = NULL;
    const pbw_descriptor *out_desc = NULL;
    pbw_status status;

    if (!get_args(env, info, argv, 3)) {
        return NULL;
    }
    if (!unwrap(env, argv[0], (void **)&schema, "schema handle")) {
        return NULL;
    }
    svc_name = string_bytes(env, argv[1], &svc_buf);
    if (svc_name == NULL) {
        return NULL;
    }
    method_name = string_bytes(env, argv[2], &method_buf);
    if (method_name == NULL) {
        string_free(&svc_buf, svc_name);
        return NULL;
    }
    status = pbw_service_resolve(schema->schema, svc_name, method_name,
                                 &in_desc, &out_desc);
    string_free(&svc_buf, svc_name);
    string_free(&method_buf, method_name);
    if (status != PBW_OK) {
        return fail_status(env, "resolve", status);
    }
    input_value = new_desc_value(env, argv[0], schema, in_desc);
    if (input_value == NULL) {
        return NULL;
    }
    output_value = new_desc_value(env, argv[0], schema, out_desc);
    if (output_value == NULL) {
        return NULL;
    }
    if (napi_create_object(env, &out) != napi_ok ||
        napi_set_named_property(env, out, "input", input_value) != napi_ok ||
        napi_set_named_property(env, out, "output", output_value) !=
            napi_ok) {
        return NULL;
    }
    return out;
}

/* Packs already-encoded bytes into a new google.protobuf.Any tree. */
static napi_value
any_pack(napi_env env, napi_callback_info info)
{
    napi_value argv[3];
    desc_handle *desc;
    pbw_message *msg = NULL;
    string_buf buf;
    const char *type_url;
    void *payload = NULL;
    size_t payload_len = 0;
    pbw_status status;

    if (!get_args(env, info, argv, 3)) {
        return NULL;
    }
    if (!unwrap(env, argv[0], (void **)&desc, "message descriptor handle")) {
        return NULL;
    }
    type_url = string_bytes(env, argv[1], &buf);
    if (type_url == NULL) {
        return NULL;
    }
    if (napi_get_buffer_info(env, argv[2], &payload, &payload_len) !=
        napi_ok) {
        string_free(&buf, type_url);
        return fail_argument(env, "anyPack expects a Buffer payload");
    }
    status = pbw_any_pack(desc->table->desc, type_url, payload, payload_len,
                          &msg);
    string_free(&buf, type_url);
    if (status != PBW_OK) {
        return fail_status(env, "anyPack", status);
    }
    return new_tree_value(env, desc, argv[0], msg);
}

/* type_url (field 1) of an Any tree, or null when absent. */
static napi_value
any_type_url(napi_env env, napi_callback_info info)
{
    napi_value argv[1];
    napi_value out = NULL;
    message_handle *handle;
    const char *url;

    if (!get_args(env, info, argv, 1)) {
        return NULL;
    }
    if (!unwrap_message(env, argv[0], &handle)) {
        return NULL;
    }
    url = pbw_any_type_url(handle->msg);
    if (url == NULL) {
        TRY(napi_get_null(env, &out));
        return out;
    }
    if (napi_create_string_utf8(env, url, NAPI_AUTO_LENGTH, &out) != napi_ok) {
        return NULL;
    }
    return out;
}

/* value bytes (field 2) of an Any tree, or null when absent. */
static napi_value
any_payload(napi_env env, napi_callback_info info)
{
    napi_value argv[1];
    napi_value out = NULL;
    message_handle *handle;
    const void *payload;
    size_t len = 0;

    if (!get_args(env, info, argv, 1)) {
        return NULL;
    }
    if (!unwrap_message(env, argv[0], &handle)) {
        return NULL;
    }
    payload = pbw_any_payload(handle->msg, &len);
    if (payload == NULL) {
        TRY(napi_get_null(env, &out));
        return out;
    }
    if (napi_create_buffer_copy(env, len, payload, NULL, &out) != napi_ok) {
        return NULL;
    }
    return out;
}

/* int64 milliseconds from a BigInt or a number. */
static int
millis_from_js(napi_env env, napi_value value, int64_t *out)
{
    uint64_t bits;

    if (!js_int64(env, value, 0, &bits)) {
        fail_argument(env, "millis must be a BigInt or a number");
        return 0;
    }
    *out = (int64_t)bits;
    return 1;
}

/* A new Timestamp/Duration tree holding ms. */
static napi_value
millis_to_message(napi_env env, napi_callback_info info, int duration)
{
    napi_value argv[2];
    desc_handle *desc;
    pbw_message *msg = NULL;
    int64_t ms;
    pbw_status status;

    if (!get_args(env, info, argv, 2)) {
        return NULL;
    }
    if (!unwrap(env, argv[0], (void **)&desc, "message descriptor handle")) {
        return NULL;
    }
    if (!millis_from_js(env, argv[1], &ms)) {
        return NULL;
    }
    if (duration) {
        status =
            pbw_duration_from_millis(desc->table->desc, ms, &msg);
    } else {
        status =
            pbw_timestamp_from_millis(desc->table->desc, ms, &msg);
    }
    if (status != PBW_OK) {
        return fail_status(env,
                           duration ? "durationFromMillis"
                                    : "timestampFromMillis",
                           status);
    }
    return new_tree_value(env, desc, argv[0], msg);
}

static napi_value
timestamp_from_millis(napi_env env, napi_callback_info info)
{
    return millis_to_message(env, info, 0);
}

static napi_value
duration_from_millis(napi_env env, napi_callback_info info)
{
    return millis_to_message(env, info, 1);
}

/* Milliseconds of a Timestamp/Duration tree, exact as a BigInt. */
static napi_value
millis_from_message(napi_env env, napi_callback_info info, int duration)
{
    napi_value argv[1];
    napi_value out = NULL;
    message_handle *handle;
    int64_t ms = 0;
    pbw_status status;

    if (!get_args(env, info, argv, 1)) {
        return NULL;
    }
    if (!unwrap_message(env, argv[0], &handle)) {
        return NULL;
    }
    if (duration) {
        status = pbw_duration_to_millis(handle->msg, &ms);
    } else {
        status = pbw_timestamp_to_millis(handle->msg, &ms);
    }
    if (status != PBW_OK) {
        return fail_status(env,
                           duration ? "durationMillis" : "timestampMillis",
                           status);
    }
    if (napi_create_bigint_int64(env, ms, &out) != napi_ok) {
        return NULL;
    }
    return out;
}

static napi_value
timestamp_millis(napi_env env, napi_callback_info info)
{
    return millis_from_message(env, info, 0);
}

static napi_value
duration_millis(napi_env env, napi_callback_info info)
{
    return millis_from_message(env, info, 1);
}

/* FieldMask paths as an array of strings. */
static napi_value
fieldmask_paths(napi_env env, napi_callback_info info)
{
    napi_value argv[1];
    napi_value out = NULL;
    message_handle *handle;
    size_t count;
    size_t i;

    if (!get_args(env, info, argv, 1)) {
        return NULL;
    }
    if (!unwrap_message(env, argv[0], &handle)) {
        return NULL;
    }
    count = pbw_fieldmask_path_count(handle->msg);
    if (napi_create_array_with_length(env, count, &out) != napi_ok) {
        return NULL;
    }
    for (i = 0; i < count; i++) {
        const char *path = pbw_fieldmask_path_at(handle->msg, i);
        napi_value item = NULL;

        if (path == NULL ||
            napi_create_string_utf8(env, path, NAPI_AUTO_LENGTH, &item) !=
                napi_ok ||
            napi_set_element(env, out, (uint32_t)i, item) != napi_ok) {
            return NULL;
        }
    }
    return out;
}

NAPI_MODULE_INIT()
{
    napi_property_descriptor properties[] = {
        {"abiVersion", NULL, abi_version, NULL, NULL, NULL, napi_enumerable,
         NULL},
        {"loadDescriptorSet", NULL, load_descriptor_set, NULL, NULL, NULL,
         napi_enumerable, NULL},
        {"descriptor", NULL, descriptor, NULL, NULL, NULL, napi_enumerable,
         NULL},
        {"messageNew", NULL, message_new, NULL, NULL, NULL, napi_enumerable,
         NULL},
        {"isEager", NULL, is_eager, NULL, NULL, NULL, napi_enumerable,
         NULL},
        {"decode", NULL, decode, NULL, NULL, NULL, napi_enumerable, NULL},
        {"decodeEager", NULL, decode_eager, NULL, NULL, NULL,
         napi_enumerable, NULL},
        {"decodeObject", NULL, decode_object, NULL, NULL, NULL,
         napi_enumerable, NULL},
        {"decodeImage", NULL, decode_image, NULL, NULL, NULL,
         napi_enumerable, NULL},
        {"wireGet", NULL, wire_get, NULL, NULL, NULL, napi_enumerable, NULL},
        {"promote", NULL, promote, NULL, NULL, NULL, napi_enumerable, NULL},
        {"get", NULL, message_get, NULL, NULL, NULL, napi_enumerable, NULL},
        {"set", NULL, message_set, NULL, NULL, NULL, napi_enumerable, NULL},
        {"encode", NULL, encode, NULL, NULL, NULL, napi_enumerable, NULL},
        {"encodeObject", NULL, encode_object, NULL, NULL, NULL,
         napi_enumerable, NULL},
        {"encodeImage", NULL, encode_image, NULL, NULL, NULL,
         napi_enumerable, NULL},
        {"copyEncoded", NULL, copy_encoded, NULL, NULL, NULL, napi_enumerable,
         NULL},
        {"free", NULL, message_free, NULL, NULL, NULL, napi_enumerable, NULL},
        {"messageToJson", NULL, message_to_json, NULL, NULL, NULL,
         napi_enumerable, NULL},
        {"jsonToMessage", NULL, json_to_message, NULL, NULL, NULL,
         napi_enumerable, NULL},
        {"messageToText", NULL, message_to_text, NULL, NULL, NULL,
         napi_enumerable, NULL},
        {"textToMessage", NULL, text_to_message, NULL, NULL, NULL,
         napi_enumerable, NULL},
        {"has", NULL, message_has, NULL, NULL, NULL, napi_enumerable, NULL},
        {"encodeDelimited", NULL, encode_delimited, NULL, NULL, NULL,
         napi_enumerable, NULL},
        {"decodeDelimited", NULL, decode_delimited, NULL, NULL, NULL,
         napi_enumerable, NULL},
        {"encodeDeterministic", NULL, encode_deterministic, NULL, NULL,
         NULL, napi_enumerable, NULL},
        {"encodedSize", NULL, encoded_size, NULL, NULL, NULL,
         napi_enumerable, NULL},
        {"encodeTo", NULL, encode_to, NULL, NULL, NULL, napi_enumerable,
         NULL},
        {"encodeDelimitedTo", NULL, encode_delimited_to, NULL, NULL, NULL,
         napi_enumerable, NULL},
        {"encodeSink", NULL, encode_sink, NULL, NULL, NULL, napi_enumerable,
         NULL},
        {"visit", NULL, visit, NULL, NULL, NULL, napi_enumerable, NULL},
        {"services", NULL, services, NULL, NULL, NULL, napi_enumerable,
         NULL},
        {"service", NULL, service, NULL, NULL, NULL, napi_enumerable,
         NULL},
        {"resolve", NULL, resolve, NULL, NULL, NULL, napi_enumerable,
         NULL},
        {"anyPack", NULL, any_pack, NULL, NULL, NULL, napi_enumerable,
         NULL},
        {"anyTypeUrl", NULL, any_type_url, NULL, NULL, NULL,
         napi_enumerable, NULL},
        {"anyPayload", NULL, any_payload, NULL, NULL, NULL, napi_enumerable,
         NULL},
        {"timestampFromMillis", NULL, timestamp_from_millis, NULL, NULL,
         NULL, napi_enumerable, NULL},
        {"timestampMillis", NULL, timestamp_millis, NULL, NULL, NULL,
         napi_enumerable, NULL},
        {"durationFromMillis", NULL, duration_from_millis, NULL, NULL,
         NULL, napi_enumerable, NULL},
        {"durationMillis", NULL, duration_millis, NULL, NULL, NULL,
         napi_enumerable, NULL},
        {"fieldmaskPaths", NULL, fieldmask_paths, NULL, NULL, NULL,
         napi_enumerable, NULL},
    };

    if (napi_define_properties(env, exports,
                               sizeof(properties) / sizeof(properties[0]),
                               properties) != napi_ok) {
        napi_throw_error(env, NULL, "pbwire: cannot export the addon");
        return NULL;
    }
    return exports;
}

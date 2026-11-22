/* The Julia binding's native layout reader (pbench `shim` arm).
 *
 * One FFI call per op: the generated `mutable struct <Message>Layout` is the
 * native object, and this file walks that host memory in place with the C
 * binding's layout codec (bindings/c/runtime/pbw_struct.c), so no field ever
 * crosses the boundary. Encode borrows the image; decode fills a zeroed image
 * and bump-allocates strings/bytes/sub-messages from the caller's arena block.
 *
 * Layout rows come from codegen (`codegen/internal/gen/julia.go`): the offsets
 * are `fieldoffset` calls emitted there, so the Julia struct field order and
 * this row table cannot disagree. A MESSAGE row names the child's proto full
 * name; the child type must be registered first (its rows and layout size
 * arrive with it).
 *
 * Memory contract:
 *  - the struct image (and every string/bytes/child pointer it holds) is the
 *    caller's memory; this file never allocates a tree;
 *  - an encode buffer is owned by the type and reused (the returned bytes are
 *    valid until the next encode of that type);
 *  - decode writes into `base` (zeroed here) and bump-allocates from
 *    `arena_mem`, which must stay alive while the decoded image is read.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "pbwire.h"
#include "pbw_struct.h"

/* One row as Julia passes it: all 64-bit so the Julia mirror needs no padding
 * rules, plus the child's proto full name for a MESSAGE row. */
typedef struct pbw_julia_row {
    uint64_t number;
    uint64_t kind; /* pbw_struct_kind_t */
    uint64_t repeated;
    uint64_t offset;
    uint64_t has;
    uint64_t count;
    uint64_t type; /* pbw_type, 0 = let the codec ask the schema */
    uint64_t wire;
    uint64_t packed;
    uint64_t required;
    uint64_t oneof;
    uint64_t cs;
    const char *child; /* MESSAGE: proto full name, else NULL */
} pbw_julia_row;

typedef struct pbw_julia_type pbw_julia_type;
typedef struct pbw_julia_schema pbw_julia_schema;

struct pbw_julia_type {
    char *name;
    pbw_struct_field_t *rows;
    size_t nrows;
    pbw_struct_codec *codec;
    pbw_struct_buf buf;
    size_t size; /* Julia layout size (sizeof the generated struct data) */
};

struct pbw_julia_schema {
    const pbw_schema *schema; /* owned by the core wrapper, not here */
    pbw_julia_type **types;
    size_t ntypes;
    size_t cap;
};

static pbw_julia_type *
julia_find_type(const pbw_julia_schema *schema, const char *name)
{
    size_t i;

    for (i = 0; i < schema->ntypes; i++) {
        if (strcmp(schema->types[i]->name, name) == 0) {
            return schema->types[i];
        }
    }
    return NULL;
}

/* Wraps a core schema (created by libpbwire and kept alive by Julia) in the
 * registry this library uses to resolve child layouts. */
pbw_status
pbw_julia_schema_new(const void *raw, pbw_julia_schema **out)
{
    pbw_julia_schema *wrapper;

    if (out == NULL) {
        return PBW_ERR_INVALID;
    }
    *out = NULL;
    if (raw == NULL) {
        return PBW_ERR_INVALID;
    }
    wrapper = (pbw_julia_schema *)calloc(1, sizeof(*wrapper));
    if (wrapper == NULL) {
        return PBW_ERR_NOMEM;
    }
    wrapper->schema = (const pbw_schema *)raw;
    *out = wrapper;
    return PBW_OK;
}

/* Frees every registered type (codecs before the rows they reference); the
 * core schema belongs to the caller and is left untouched. Safe on NULL. */
void
pbw_julia_schema_free(pbw_julia_schema *schema)
{
    size_t i;

    if (schema == NULL) {
        return;
    }
    for (i = 0; i < schema->ntypes; i++) {
        pbw_julia_type *type = schema->types[i];

        pbw_struct_codec_free(type->codec);
        type->codec = NULL;
    }
    for (i = 0; i < schema->ntypes; i++) {
        pbw_julia_type *type = schema->types[i];

        pbw_struct_buf_free(&type->buf);
        free(type->rows);
        free(type->name);
        free(type);
    }
    free(schema->types);
    free(schema);
}

/* Proto2 enum default (a nonzero default is the only one the codec seeds);
 * absent or non-numeric defaults stay 0, exactly like the schema says. */
static int32_t
julia_row_default(const pbw_descriptor *desc, const pbw_struct_field_t *row)
{
    const pbw_field *field;
    const char *text;
    char *end = NULL;
    long value;

    if (row->kind != PBW_SF_ENUM || row->repeated != 0) {
        return 0;
    }
    field = pbw_descriptor_find_field(desc, row->fnum);
    if (field == NULL) {
        return 0;
    }
    text = pbw_field_default_value(field);
    if (text == NULL || text[0] == '\0') {
        return 0;
    }
    value = strtol(text, &end, 10);
    if (end == NULL || *end != '\0' || value < INT32_MIN || value > INT32_MAX) {
        return 0;
    }
    return (int32_t)value;
}

/* Registers one message layout. `rows` is the Julia row table, `size` the
 * generated struct's data size; children must be registered before their
 * parent. *out is NULL on failure. */
pbw_status
pbw_julia_type_new(pbw_julia_schema *schema, const char *message,
                   const pbw_julia_row *rows, size_t nrows, size_t size,
                   pbw_julia_type **out)
{
    const pbw_descriptor *desc;
    pbw_julia_type *type;
    size_t i;
    pbw_status status;

    if (out == NULL) {
        return PBW_ERR_INVALID;
    }
    *out = NULL;
    if (schema == NULL || message == NULL || rows == NULL || size == 0) {
        return PBW_ERR_INVALID;
    }
    desc = pbw_schema_find(schema->schema, message);
    if (desc == NULL) {
        return PBW_ERR_INVALID;
    }
    if (julia_find_type(schema, message) != NULL) {
        return PBW_ERR_INVALID; /* one type per message, registered once */
    }
    type = (pbw_julia_type *)calloc(1, sizeof(*type));
    if (type == NULL) {
        return PBW_ERR_NOMEM;
    }
    type->name = (char *)malloc(strlen(message) + 1);
    type->rows = (pbw_struct_field_t *)calloc(nrows, sizeof(*type->rows));
    if (type->name == NULL || type->rows == NULL) {
        free(type->name);
        free(type->rows);
        free(type);
        return PBW_ERR_NOMEM;
    }
    strcpy(type->name, message);
    type->nrows = nrows;
    type->size = size;
    for (i = 0; i < nrows; i++) {
        const pbw_julia_row *r = &rows[i];
        pbw_struct_field_t *row = &type->rows[i];
        pbw_julia_type *child = NULL;

        row->fnum = (uint32_t)r->number;
        row->kind = (pbw_struct_kind_t)r->kind;
        row->repeated = (int)r->repeated;
        row->offset = (size_t)r->offset;
        row->has = (size_t)r->has;
        row->count = (size_t)r->count;
        row->type = (uint8_t)r->type;
        row->wire = (uint8_t)r->wire;
        row->packed = (uint8_t)r->packed;
        row->required = (uint8_t)r->required;
        row->oneof = (uint8_t)r->oneof;
        row->cs = (size_t)r->cs;
        if (r->child != NULL && r->child[0] != '\0') {
            child = julia_find_type(schema, r->child);
            if (child == NULL) {
                status = PBW_ERR_INVALID; /* child not registered yet */
                goto fail;
            }
            row->fields = child->rows;
            row->nfields = child->nrows;
            row->elem = child->size;
        }
        row->dflt = julia_row_default(desc, row);
    }
    status = pbw_struct_codec_init(&type->codec, schema->schema, message,
                                   type->rows, nrows);
    if (status != PBW_OK) {
        goto fail;
    }
    if (schema->ntypes == schema->cap) {
        size_t cap = schema->cap == 0 ? 8 : schema->cap * 2;
        pbw_julia_type **grown = (pbw_julia_type **)realloc(
            schema->types, cap * sizeof(*schema->types));

        if (grown == NULL) {
            status = PBW_ERR_NOMEM;
            goto fail;
        }
        schema->types = grown;
        schema->cap = cap;
    }
    schema->types[schema->ntypes++] = type;
    *out = type;
    return PBW_OK;
fail:
    pbw_struct_codec_free(type->codec);
    free(type->rows);
    free(type->name);
    free(type);
    return status;
}

/* Struct image -> wire bytes, into the type's reusable buffer. The bytes stay
 * valid until the next encode of the same type. */
pbw_status
pbw_julia_encode(pbw_julia_type *type, const void *base, uint8_t **out,
                 size_t *out_len)
{
    pbw_status status;

    if (type == NULL || base == NULL || out == NULL || out_len == NULL) {
        return PBW_ERR_INVALID;
    }
    status = pbw_struct_codec_encode(type->codec, base, &type->buf);
    if (status != PBW_OK) {
        *out = NULL;
        *out_len = 0;
        return status;
    }
    *out = type->buf.data;
    *out_len = type->buf.len;
    return PBW_OK;
}

/* Wire bytes -> zeroed struct image. Everything non-blittable is bump-
 * allocated from the caller's arena block, so the image is a view of that
 * memory; PBW_ERR_NOMEM means the block is too small (call again with more). */
pbw_status
pbw_julia_decode(pbw_julia_type *type, const uint8_t *wire, size_t wire_len,
                 void *base, void *arena_mem, size_t arena_len)
{
    pbw_struct_arena *arena;
    pbw_status status;

    if (type == NULL || wire == NULL || base == NULL || arena_mem == NULL) {
        return PBW_ERR_INVALID;
    }
    memset(base, 0, type->size);
    arena = pbw_struct_arena_new_mem(arena_mem, arena_len);
    if (arena == NULL) {
        return PBW_ERR_NOMEM; /* unaligned or too small for the header */
    }
    status = pbw_struct_codec_decode(type->codec, wire, wire_len, &arena, base);
    return status;
}

void
pbw_julia_type_free(pbw_julia_type *type)
{
    if (type == NULL) {
        return;
    }
    pbw_struct_codec_free(type->codec);
    pbw_struct_buf_free(&type->buf);
    free(type->rows);
    free(type->name);
    free(type);
}

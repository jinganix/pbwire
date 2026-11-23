/* The Swift binding's native layout reader (pbench `shim` arm).
 *
 * One downcall per op: the generated `@frozen <Message>Repr` is the native
 * object, and this file walks that host memory in place with the C binding's
 * layout codec (bindings/c/runtime/pbw_struct.c, compiled into this target),
 * so no field ever crosses the boundary. Encode borrows the image; decode
 * fills a zeroed image and bump-allocates strings/bytes/sub-messages from the
 * type's arena. The mirror holds no references, so the walk pays no ARC.
 *
 * Layout rows come from codegen (`codegen/internal/gen/swift.go`): every
 * offset is a `MemoryLayout<Repr>.offset(of: \.slot)!` emitted there, so the
 * Swift slot order and this row table cannot disagree. A MESSAGE row resolves
 * its child layout through the schema's field descriptor, which is why the
 * child must be registered first (no name crosses the boundary).
 *
 * Memory contract:
 *  - the mirror image (and every string/bytes/child pointer it holds) is the
 *    caller's memory; this file never allocates a tree;
 *  - the encode buffer is owned by the type and reused (the returned bytes
 *    are valid until the next encode of that type);
 *  - decode writes into `base` (zeroed here) and bump-allocates from the
 *    type's arena, which must stay alive while the decoded image is read.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "CPbwire.h"

struct pbw_swift_type {
    char *name;
    pbw_struct_field_t *rows;
    size_t nrows;
    pbw_struct_codec *codec;
    pbw_struct_buf buf;
    pbw_struct_arena *arena; /* decode arena: reused, so a decode loop allocates nothing */
    size_t size;             /* mirror size in bytes (MemoryLayout<R>.size) */
};

struct pbw_swift_schema {
    const pbw_schema *schema; /* owned by the Swift wrapper, not here */
    pbw_swift_type **types;
    size_t ntypes;
    size_t cap;
};

static pbw_swift_type *
swift_find_type(const pbw_swift_schema *schema, const char *name)
{
    size_t i;

    for (i = 0; i < schema->ntypes; i++) {
        if (strcmp(schema->types[i]->name, name) == 0) {
            return schema->types[i];
        }
    }
    return NULL;
}

pbw_status
pbw_swift_schema_new(const void *raw, pbw_swift_schema **out)
{
    pbw_swift_schema *wrapper;

    if (out == NULL) {
        return PBW_ERR_INVALID;
    }
    *out = NULL;
    if (raw == NULL) {
        return PBW_ERR_INVALID;
    }
    wrapper = (pbw_swift_schema *)calloc(1, sizeof(*wrapper));
    if (wrapper == NULL) {
        return PBW_ERR_NOMEM;
    }
    wrapper->schema = (const pbw_schema *)raw;
    *out = wrapper;
    return PBW_OK;
}

void
pbw_swift_schema_free(pbw_swift_schema *schema)
{
    size_t i;

    if (schema == NULL) {
        return;
    }
    for (i = 0; i < schema->ntypes; i++) {
        pbw_swift_type_free(schema->types[i]);
    }
    free(schema->types);
    free(schema);
}

/* Proto2 enum default (only a nonzero default matters; absent or non-numeric
 * defaults stay 0, exactly like the schema says). */
static int32_t
swift_row_default(const pbw_descriptor *desc, const pbw_struct_field_t *row)
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

/* Child layout of a MESSAGE row: the schema names it, the registry holds it. */
static pbw_swift_type *
swift_row_child(const pbw_swift_schema *schema, const pbw_descriptor *desc,
                uint32_t number)
{
    const pbw_field *field;
    const pbw_descriptor *child;
    const char *name;

    field = pbw_descriptor_find_field(desc, number);
    if (field == NULL) {
        return NULL;
    }
    child = pbw_field_message_type(field);
    if (child == NULL) {
        return NULL;
    }
    name = pbw_descriptor_name(child);
    if (name == NULL) {
        return NULL;
    }
    return swift_find_type(schema, name);
}

pbw_status
pbw_swift_type_new(pbw_swift_schema *schema, const char *message,
                   const pbw_swift_row *rows, size_t nrows, size_t size,
                   pbw_swift_type **out)
{
    const pbw_descriptor *desc;
    pbw_swift_type *type;
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
    if (swift_find_type(schema, message) != NULL) {
        return PBW_ERR_INVALID; /* one type per message, registered once */
    }
    type = (pbw_swift_type *)calloc(1, sizeof(*type));
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
        const pbw_swift_row *r = &rows[i];
        pbw_struct_field_t *row = &type->rows[i];

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
        row->elem = (size_t)r->elem;
        if (row->kind == PBW_SF_MESSAGE) {
            pbw_swift_type *child = swift_row_child(schema, desc, row->fnum);

            if (child == NULL) {
                status = PBW_ERR_INVALID; /* child not registered yet */
                goto fail;
            }
            row->fields = child->rows;
            row->nfields = child->nrows;
            if (row->elem == 0) {
                row->elem = child->size;
            }
        }
        row->dflt = swift_row_default(desc, row);
    }
    status = pbw_struct_codec_init(&type->codec, schema->schema, message,
                                   type->rows, nrows);
    if (status != PBW_OK) {
        goto fail;
    }
    if (schema->ntypes == schema->cap) {
        size_t cap = schema->cap == 0 ? 8 : schema->cap * 2;
        pbw_swift_type **grown =
            (pbw_swift_type **)realloc(schema->types, cap * sizeof(*schema->types));

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
    type->codec = NULL;
    free(type->rows);
    free(type->name);
    free(type);
    return status;
}

pbw_status
pbw_swift_encode(pbw_swift_type *type, const void *base, uint8_t **out,
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

pbw_status
pbw_swift_decode(pbw_swift_type *type, const uint8_t *wire, size_t wire_len,
                 void *base)
{
    if (type == NULL || base == NULL) {
        return PBW_ERR_INVALID;
    }
    if (wire == NULL && wire_len != 0) {
        return PBW_ERR_INVALID;
    }
    memset(base, 0, type->size);
    return pbw_struct_codec_decode(type->codec, wire, wire_len, &type->arena,
                                   base);
}

void
pbw_swift_type_free(pbw_swift_type *type)
{
    if (type == NULL) {
        return;
    }
    pbw_struct_codec_free(type->codec);
    pbw_struct_buf_free(&type->buf);
    pbw_struct_arena_free(type->arena);
    free(type->rows);
    free(type->name);
    free(type);
}

/* Compile check that the re-exported headers parse as C (the ABI itself lives
 * in libpbwire, which the final link picks up from ../../build). */
const char *
cpbwire_header_check(void)
{
    return "ok";
}

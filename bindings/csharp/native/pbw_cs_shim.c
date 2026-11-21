/* The C# binding's native layout reader (pbench `shim` arm).
 *
 * One P/Invoke per op: the managed side writes the native object into a
 * contiguous unmanaged image once (a generated `WriteImage` that stores
 * scalars at their emitted offsets, pointers to strings/bytes and child images
 * in a call-owned bump arena), then this file walks that image in place with
 * the C binding's layout codec (bindings/c/runtime/pbw_struct.c). No field
 * ever crosses the boundary. Decode fills a zeroed image and bump-allocates
 * strings/bytes/sub-messages from the caller's arena block.
 *
 * Why the image and not the object: a C# class with `string`/`List<T>` members
 * cannot be pinned (`GCHandle.Alloc(obj, Pinned)` throws for a non-blittable
 * type), and the GC may move those members at any time. The image is the
 * pinned representation; filling it is the arm's honest cost.
 *
 * Layout rows come from codegen (`codegen/internal/gen/csharp.go`): the offsets
 * are computed there, so the generated `WriteImage`/`ReadImage` and this row
 * table cannot disagree. A MESSAGE row names the child's proto full name; the
 * child type must be registered first (its rows and its image size arrive with
 * it).
 *
 * Memory contract:
 *  - the image (and every string/bytes/child pointer it holds) is the caller's
 *    memory; this file never allocates a tree;
 *  - the encode buffer is owned by the type and reused (the returned bytes are
 *    valid until the next encode of that type);
 *  - decode writes into `base` (zeroed here) and bump-allocates from
 *    `arena_mem`, which must stay alive while the decoded image is read.
 *
 * The shim loads its own descriptor set (`pbw_cs_shim_schema_load`) instead of
 * borrowing the binding's `pbw_schema`: this library links its own copy of the
 * core, so a pointer made by libpbwire would cross two copies of the same
 * struct. Same reason the Dart reader does it.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "pbwire.h"
#include "pbw_struct.h"

/* One row as the managed side passes it: all 64-bit so the C# mirror needs no
 * padding rules, plus the child's proto full name for a MESSAGE row. */
typedef struct pbw_cs_row {
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
} pbw_cs_row;

typedef struct pbw_cs_type pbw_cs_type;
typedef struct pbw_cs_schema pbw_cs_schema;

struct pbw_cs_type {
    char *name;
    pbw_struct_field_t *rows;
    size_t nrows;
    pbw_struct_codec *codec;
    pbw_struct_buf buf;
    size_t size; /* image size in bytes (the generated WriteImage's stride) */
};

struct pbw_cs_schema {
    pbw_schema *schema;
    pbw_cs_type **types;
    size_t ntypes;
    size_t cap;
};

static pbw_cs_type *
cs_find_type(const pbw_cs_schema *schema, const char *name)
{
    size_t i;

    for (i = 0; i < schema->ntypes; i++) {
        if (strcmp(schema->types[i]->name, name) == 0) {
            return schema->types[i];
        }
    }
    return NULL;
}

static char *
cs_strdup(const char *text)
{
    size_t n = strlen(text) + 1;
    char *out = (char *)malloc(n);

    if (out != NULL) {
        memcpy(out, text, n);
    }
    return out;
}

/* Wire a FileDescriptorSet into a schema this library owns. */
pbw_status
pbw_cs_shim_schema_load(const void *data, size_t len, void **out)
{
    pbw_schema *schema = NULL;
    pbw_status status;
    pbw_cs_schema *wrapper;

    if (out == NULL) {
        return PBW_ERR_INVALID;
    }
    *out = NULL;
    status = pbw_descriptor_set_load(data, len, &schema);
    if (status != PBW_OK) {
        if (schema != NULL) {
            pbw_schema_free(schema);
        }
        return status;
    }
    wrapper = (pbw_cs_schema *)calloc(1, sizeof(*wrapper));
    if (wrapper == NULL) {
        pbw_schema_free(schema);
        return PBW_ERR_NOMEM;
    }
    wrapper->schema = schema;
    *out = wrapper;
    return PBW_OK;
}

void
pbw_cs_shim_schema_free(void *handle)
{
    pbw_cs_schema *schema = (pbw_cs_schema *)handle;
    size_t i;

    if (schema == NULL) {
        return;
    }
    for (i = 0; i < schema->ntypes; i++) {
        pbw_cs_type *type = schema->types[i];

        pbw_struct_codec_free(type->codec);
        pbw_struct_buf_free(&type->buf);
        free(type->rows);
        free(type->name);
        free(type);
    }
    free(schema->types);
    pbw_schema_free(schema->schema);
    free(schema);
}

/* Proto2 enum default (a nonzero default is the only one the codec seeds);
 * absent or non-numeric defaults stay 0, exactly like the schema says. */
static int32_t
cs_row_default(const pbw_descriptor *desc, const pbw_struct_field_t *row)
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

/* Compile one message layout. `rows` is the generated row table, `size` the
 * generated image size; children must be compiled before their parent. *out is
 * NULL on failure. */
pbw_status
pbw_cs_shim_type_new(void *handle, const char *message, const pbw_cs_row *rows,
                     size_t nrows, size_t size, void **out)
{
    pbw_cs_schema *schema = (pbw_cs_schema *)handle;
    const pbw_descriptor *desc;
    pbw_cs_type *type;
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
    if (cs_find_type(schema, message) != NULL) {
        return PBW_ERR_INVALID; /* one type per message, compiled once */
    }
    type = (pbw_cs_type *)calloc(1, sizeof(*type));
    if (type == NULL) {
        return PBW_ERR_NOMEM;
    }
    type->name = cs_strdup(message);
    type->rows = (pbw_struct_field_t *)calloc(nrows == 0 ? 1 : nrows,
                                              sizeof(*type->rows));
    if (type->name == NULL || type->rows == NULL) {
        free(type->name);
        free(type->rows);
        free(type);
        return PBW_ERR_NOMEM;
    }
    type->nrows = nrows;
    type->size = size;
    for (i = 0; i < nrows; i++) {
        const pbw_cs_row *r = &rows[i];
        pbw_struct_field_t *row = &type->rows[i];
        pbw_cs_type *child = NULL;

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
            child = cs_find_type(schema, r->child);
            if (child == NULL) {
                status = PBW_ERR_INVALID; /* child not compiled yet */
                goto fail;
            }
            row->fields = child->rows;
            row->nfields = child->nrows;
            row->elem = child->size;
        }
        row->dflt = cs_row_default(desc, row);
    }
    status = pbw_struct_codec_init(&type->codec, schema->schema, message,
                                   type->rows, nrows);
    if (status != PBW_OK) {
        goto fail;
    }
    if (schema->ntypes == schema->cap) {
        size_t cap = schema->cap == 0 ? 8 : schema->cap * 2;
        pbw_cs_type **grown = (pbw_cs_type **)realloc(
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

/* Image -> wire bytes, into the type's reusable buffer. The bytes stay valid
 * until the next encode of the same type. */
pbw_status
pbw_cs_shim_encode(void *handle, const void *image, const uint8_t **out,
                   size_t *out_len)
{
    pbw_cs_type *type = (pbw_cs_type *)handle;
    pbw_status status;

    if (type == NULL || image == NULL || out == NULL || out_len == NULL) {
        return PBW_ERR_INVALID;
    }
    status = pbw_struct_codec_encode(type->codec, image, &type->buf);
    if (status != PBW_OK) {
        *out = NULL;
        *out_len = 0;
        return status;
    }
    *out = type->buf.data;
    *out_len = type->buf.len;
    return PBW_OK;
}

/* Wire bytes -> zeroed image. Everything non-blittable is bump-allocated from
 * the caller's arena block, so the image is a view of that memory;
 * PBW_ERR_NOMEM means the block is too small (call again with more). */
pbw_status
pbw_cs_shim_decode(void *handle, const uint8_t *wire, size_t wire_len,
                   void *image, void *arena_mem, size_t arena_len)
{
    pbw_cs_type *type = (pbw_cs_type *)handle;
    pbw_struct_arena *arena;
    pbw_status status;

    if (type == NULL || wire == NULL || image == NULL || arena_mem == NULL) {
        return PBW_ERR_INVALID;
    }
    memset(image, 0, type->size);
    arena = pbw_struct_arena_new_mem(arena_mem, arena_len);
    if (arena == NULL) {
        return PBW_ERR_NOMEM; /* unaligned or too small for the header */
    }
    status = pbw_struct_codec_decode(type->codec, wire, wire_len, &arena, image);
    return status;
}

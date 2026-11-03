/* Generic struct codec over the pbw dynamic core. See pbw_struct.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pbw_struct.h"

static const pbw_struct_field_t *
find_field(const pbw_struct_field_t *fields, size_t nfields, uint32_t fnum)
{
    size_t i;

    for (i = 0; i < nfields; i++) {
        if (fields[i].fnum == fnum) {
            return &fields[i];
        }
    }
    return NULL;
}

/* The message owns the value on success; on failure the value (including a
 * wrapped submessage) is released and the caller owns nothing. */
static pbw_status
set_value(pbw_message *msg, uint32_t number, pbw_value *value)
{
    if (value == NULL) {
        return PBW_ERR_NOMEM;
    }
    if (pbw_message_set(msg, number, value) != PBW_OK) {
        pbw_value_free(value);
        return PBW_ERR_INVALID;
    }
    return PBW_OK;
}

static size_t
scalar_size(pbw_struct_kind_t kind)
{
    switch (kind) {
    case PBW_SF_INT32:
    case PBW_SF_UINT32:
    case PBW_SF_FLOAT:
    case PBW_SF_ENUM:
        return 4;
    case PBW_SF_INT64:
    case PBW_SF_UINT64:
    case PBW_SF_DOUBLE:
        return 8;
    case PBW_SF_BOOL:
        return 1;
    default:
        return 0;
    }
}

/* Singular scalar struct value -> fresh pbw_value. */
static pbw_value *
new_scalar(pbw_struct_kind_t kind, const void *p)
{
    int32_t i32;
    int64_t i64;

    switch (kind) {
    case PBW_SF_INT32:
    case PBW_SF_ENUM:
        memcpy(&i32, p, 4);
        return pbw_value_new_int(i32);
    case PBW_SF_UINT32: {
        uint32_t u32;
        memcpy(&u32, p, 4);
        return pbw_value_new_uint(u32);
    }
    case PBW_SF_BOOL:
        return pbw_value_new_bool(*(const unsigned char *)p != 0);
    case PBW_SF_INT64:
        memcpy(&i64, p, 8);
        return pbw_value_new_int(i64);
    case PBW_SF_UINT64: {
        uint64_t u64;
        memcpy(&u64, p, 8);
        return pbw_value_new_uint(u64);
    }
    case PBW_SF_FLOAT: {
        float f;
        memcpy(&f, p, 4);
        return pbw_value_new_float(f);
    }
    case PBW_SF_DOUBLE: {
        double d;
        memcpy(&d, p, 8);
        return pbw_value_new_double(d);
    }
    default:
        return NULL;
    }
}

/* Schema field type accepted for a layout kind. */
static int
scalar_accepts(pbw_type type, pbw_struct_kind_t kind)
{
    switch (kind) {
    case PBW_SF_INT32:
        return type == PBW_TYPE_INT32 || type == PBW_TYPE_SINT32 ||
               type == PBW_TYPE_SFIXED32;
    case PBW_SF_UINT32:
        return type == PBW_TYPE_UINT32 || type == PBW_TYPE_FIXED32;
    case PBW_SF_INT64:
        return type == PBW_TYPE_INT64 || type == PBW_TYPE_SINT64 ||
               type == PBW_TYPE_SFIXED64;
    case PBW_SF_UINT64:
        return type == PBW_TYPE_UINT64 || type == PBW_TYPE_FIXED64;
    case PBW_SF_BOOL:
        return type == PBW_TYPE_BOOL;
    case PBW_SF_FLOAT:
        return type == PBW_TYPE_FLOAT;
    case PBW_SF_DOUBLE:
        return type == PBW_TYPE_DOUBLE;
    case PBW_SF_ENUM:
        return type == PBW_TYPE_ENUM;
    default:
        return 0;
    }
}

/* Struct -> message tree. The schema drives: every schema field needs a
 * layout row, so a wrong field number fails instead of dropping data. */
static pbw_status
build_tree(const pbw_schema *schema, const pbw_descriptor *desc,
           const pbw_struct_field_t *fields, size_t nfields, const void *base,
           pbw_message *msg)
{
    size_t count = pbw_descriptor_field_count(desc);
    size_t i, j;

    (void)schema;
    for (i = 0; i < count; i++) {
        const pbw_field *field = pbw_descriptor_field_at(desc, i);
        const pbw_struct_field_t *row;
        const char *p;
        uint32_t fnum;
        pbw_status status = PBW_OK;

        if (field == NULL) {
            return PBW_ERR_INVALID;
        }
        fnum = pbw_field_number(field);
        row = find_field(fields, nfields, fnum);
        if (row == NULL) {
            return PBW_ERR_INVALID;
        }
        if ((pbw_field_label(field) == PBW_LABEL_REPEATED) !=
            (row->repeated != 0)) {
            return PBW_ERR_INVALID;
        }
        p = (const char *)base + row->offset;
        if (row->kind == PBW_SF_MESSAGE) {
            const pbw_descriptor *sub = pbw_field_message_type(field);
            if (pbw_field_type(field) != PBW_TYPE_MESSAGE || sub == NULL ||
                row->fields == NULL) {
                return PBW_ERR_INVALID;
            }
            if (!row->repeated) {
                const void *item = *(void *const *)p;
                pbw_message *sub_msg;
                pbw_value *value;
                if (item == NULL) {
                    continue; /* NULL means absent */
                }
                sub_msg = pbw_message_new(sub);
                if (sub_msg == NULL) {
                    return PBW_ERR_NOMEM;
                }
                status = build_tree(schema, sub, row->fields, row->nfields,
                                    item, sub_msg);
                if (status == PBW_OK) {
                    value = pbw_value_new_message(sub_msg);
                    status = set_value(msg, fnum, value);
                }
                if (status != PBW_OK) {
                    pbw_message_free(sub_msg);
                    return status;
                }
            } else {
                void *items = *(void **)p;
                size_t n = *(const size_t *)((const char *)base + row->count);
                if (row->elem == 0) {
                    return PBW_ERR_INVALID;
                }
                for (j = 0; j < n && status == PBW_OK; j++) {
                    const void *item = (const char *)items + j * row->elem;
                    pbw_message *sub_msg = pbw_message_new(sub);
                    pbw_value *value;
                    if (sub_msg == NULL) {
                        return PBW_ERR_NOMEM;
                    }
                    status = build_tree(schema, sub, row->fields,
                                        row->nfields, item, sub_msg);
                    if (status == PBW_OK) {
                        value = pbw_value_new_message(sub_msg);
                        status = set_value(msg, fnum, value);
                    }
                    if (status != PBW_OK) {
                        pbw_message_free(sub_msg);
                        return status;
                    }
                }
            }
            continue;
        }
        if (pbw_field_type(field) == PBW_TYPE_STRING) {
            if (row->kind != PBW_SF_STRING) {
                return PBW_ERR_INVALID;
            }
            if (!row->repeated) {
                const char *s = *(char *const *)p;
                if (s == NULL) {
                    continue;
                }
                status = set_value(msg, fnum, pbw_value_new_string(s));
            } else {
                char *const *items = *(char *const **)p;
                size_t n = *(const size_t *)((const char *)base + row->count);
                for (j = 0; j < n && status == PBW_OK; j++) {
                    status =
                        set_value(msg, fnum, pbw_value_new_string(items[j]));
                }
            }
            if (status != PBW_OK) {
                return status;
            }
            continue;
        }
        if (!scalar_accepts(pbw_field_type(field), row->kind)) {
            return PBW_ERR_INVALID;
        }
        if (!row->repeated) {
            if (row->has != 0 &&
                *(const int *)((const char *)base + row->has) == 0) {
                continue;
            }
            status = set_value(msg, fnum, new_scalar(row->kind, p));
        } else {
            /* offset points at the pointer member; the array lives
             * behind it, unlike the message/string branches above
             * this one reads through. */
            const char *items = *(const char *const *)p;
            size_t n = *(const size_t *)((const char *)base + row->count);
            size_t width = scalar_size(row->kind);
            if (width == 0) {
                return PBW_ERR_INVALID;
            }
            for (j = 0; j < n && status == PBW_OK; j++) {
                status = set_value(msg, fnum,
                                   new_scalar(row->kind, items + j * width));
            }
        }
        if (status != PBW_OK) {
            return status;
        }
    }
    return PBW_OK;
}

pbw_status
pbw_struct_encode(const pbw_schema *schema, const char *message,
                  const pbw_struct_field_t *fields, size_t nfields,
                  const void *base, uint8_t **wire, size_t *wire_len)
{
    const pbw_descriptor *desc = pbw_schema_find(schema, message);
    pbw_message *msg;
    pbw_status status;

    if (desc == NULL || base == NULL || wire == NULL || wire_len == NULL) {
        return PBW_ERR_INVALID;
    }
    msg = pbw_message_new(desc);
    if (msg == NULL) {
        return PBW_ERR_NOMEM;
    }
    status = build_tree(schema, desc, fields, nfields, base, msg);
    if (status == PBW_OK && !pbw_message_is_initialized(msg)) {
        status = PBW_ERR_REQUIRED; /* a proto2 required field has no value */
    }
    if (status == PBW_OK) {
        status = pbw_encode(msg, wire, wire_len);
    }
    pbw_message_free(msg);
    return status;
}

static char *
xstrdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *dup = malloc(n);

    if (dup != NULL) {
        memcpy(dup, s, n);
    }
    return dup;
}

/* Fresh scalar from a wire value. */
static void
store_scalar(pbw_struct_kind_t kind, void *p, const pbw_value *value)
{
    int32_t i32;
    uint32_t u32;
    int64_t i64;
    uint64_t u64;
    float f;
    double d;

    switch (kind) {
    case PBW_SF_INT32:
    case PBW_SF_ENUM:
        i32 = pbw_value_get_int32(value);
        memcpy(p, &i32, 4);
        break;
    case PBW_SF_UINT32:
        u32 = pbw_value_get_uint32(value);
        memcpy(p, &u32, 4);
        break;
    case PBW_SF_BOOL: {
        unsigned char b = pbw_value_get_bool(value) ? 1 : 0;
        memcpy(p, &b, 1);
        break;
    }
    case PBW_SF_INT64:
        i64 = pbw_value_get_int(value);
        memcpy(p, &i64, 8);
        break;
    case PBW_SF_UINT64:
        u64 = pbw_value_get_uint(value);
        memcpy(p, &u64, 8);
        break;
    case PBW_SF_FLOAT:
        f = pbw_value_get_float(value);
        memcpy(p, &f, 4);
        break;
    case PBW_SF_DOUBLE:
        d = pbw_value_get_double(value);
        memcpy(p, &d, 8);
        break;
    default:
        break;
    }
}

/* Message tree -> struct. base must be zeroed; arrays and strings are
 * calloc'd so pbw_struct_free() stays safe halfway through. Schema-driven
 * like the encoder, so absent enums pick up their default. */
static pbw_status
fill_tree(const pbw_descriptor *desc, const pbw_struct_field_t *fields,
          size_t nfields, const pbw_message *msg, void *base)
{
    size_t count = pbw_descriptor_field_count(desc);
    size_t i, j;

    for (i = 0; i < count; i++) {
        const pbw_field *field = pbw_descriptor_field_at(desc, i);
        const pbw_struct_field_t *row;
        pbw_value *value;
        uint32_t fnum;
        char *p;
        pbw_status status = PBW_OK;

        if (field == NULL) {
            return PBW_ERR_INVALID;
        }
        fnum = pbw_field_number(field);
        row = find_field(fields, nfields, fnum);
        if (row == NULL) {
            return PBW_ERR_INVALID;
        }
        p = (char *)base + row->offset;
        value = pbw_message_get(msg, fnum);
        if (value == NULL) {
            /* Absent: enums fall back to their default, else zero. */
            if (row->kind == PBW_SF_ENUM && !row->repeated) {
                memcpy(p, &row->dflt, 4);
            }
            continue;
        }
        if (row->kind == PBW_SF_MESSAGE) {
            if (pbw_field_type(field) != PBW_TYPE_MESSAGE) {
                return PBW_ERR_INVALID;
            }
            if (!row->repeated) {
                void *item = calloc(1, row->elem);
                if (item == NULL) {
                    return PBW_ERR_NOMEM;
                }
                status = fill_tree(pbw_field_message_type(field),
                                   row->fields, row->nfields,
                                   pbw_value_get_message(value), item);
                if (status != PBW_OK) {
                    pbw_struct_free(row->fields, row->nfields, item);
                    free(item);
                    return status;
                }
                *(void **)p = item;
            } else {
                size_t n = pbw_value_count(value);
                void *items;
                if (row->elem == 0) {
                    return PBW_ERR_INVALID;
                }
                items = calloc(n > 0 ? n : 1, row->elem);
                if (items == NULL) {
                    return PBW_ERR_NOMEM;
                }
                for (j = 0; j < n && status == PBW_OK; j++) {
                    void *item = (char *)items + j * row->elem;
                    status = fill_tree(pbw_field_message_type(field),
                                       row->fields, row->nfields,
                                       pbw_value_get_message(
                                           pbw_value_at(value, j)),
                                       item);
                }
                if (status != PBW_OK) {
                    for (j = 0; j < n; j++) {
                        pbw_struct_free(row->fields, row->nfields,
                                        (char *)items + j * row->elem);
                    }
                    free(items);
                    return status;
                }
                *(void **)p = items;
                *(size_t *)((char *)base + row->count) = n;
            }
            continue;
        }
        if (pbw_field_type(field) == PBW_TYPE_STRING) {
            if (row->kind != PBW_SF_STRING) {
                return PBW_ERR_INVALID;
            }
            if (!row->repeated) {
                char *dup = xstrdup(pbw_value_get_string(value));
                if (dup == NULL) {
                    return PBW_ERR_NOMEM;
                }
                *(char **)p = dup;
            } else {
                size_t n = pbw_value_count(value);
                char **items = calloc(n > 0 ? n : 1, sizeof(char *));
                if (items == NULL) {
                    return PBW_ERR_NOMEM;
                }
                for (j = 0; j < n && status == PBW_OK; j++) {
                    items[j] = xstrdup(pbw_value_get_string(
                        pbw_value_at(value, j)));
                    if (items[j] == NULL) {
                        status = PBW_ERR_NOMEM;
                    }
                }
                if (status != PBW_OK) {
                    for (j = 0; j < n && items[j] != NULL; j++) {
                        free(items[j]);
                    }
                    free(items);
                    return status;
                }
                *(char ***)p = items;
                *(size_t *)((char *)base + row->count) = n;
            }
            continue;
        }
        if (!scalar_accepts(pbw_field_type(field), row->kind)) {
            return PBW_ERR_INVALID;
        }
        if (!row->repeated) {
            store_scalar(row->kind, p, value);
            if (row->has != 0) {
                *(int *)((char *)base + row->has) = 1;
            }
        } else {
            size_t n = pbw_value_count(value);
            size_t width = scalar_size(row->kind);
            void *items;
            if (width == 0) {
                return PBW_ERR_INVALID;
            }
            items = calloc(n > 0 ? n : 1, width);
            if (items == NULL) {
                return PBW_ERR_NOMEM;
            }
            for (j = 0; j < n; j++) {
                store_scalar(row->kind, (char *)items + j * width,
                             pbw_value_at(value, j));
            }
            *(void **)p = items;
            *(size_t *)((char *)base + row->count) = n;
        }
    }
    return PBW_OK;
}

pbw_status
pbw_struct_decode(const pbw_schema *schema, const char *message,
                  const pbw_struct_field_t *fields, size_t nfields,
                  const uint8_t *wire, size_t wire_len, void *base)
{
    const pbw_descriptor *desc = pbw_schema_find(schema, message);
    pbw_message *msg = NULL;
    pbw_status status;

    if (desc == NULL || base == NULL) {
        return PBW_ERR_INVALID;
    }
    status = pbw_decode(desc, wire, wire_len, &msg);
    if (status != PBW_OK) {
        return status;
    }
    status = fill_tree(desc, fields, nfields, msg, base);
    if (status != PBW_OK) {
        pbw_struct_free(fields, nfields, base);
    }
    pbw_message_free(msg);
    return status;
}

void
pbw_struct_free(const pbw_struct_field_t *fields, size_t nfields, void *base)
{
    size_t i, j;

    if (base == NULL) {
        return;
    }
    for (i = 0; i < nfields; i++) {
        const pbw_struct_field_t *row = &fields[i];
        char *p = (char *)base + row->offset;

        if (row->kind == PBW_SF_MESSAGE) {
            if (!row->repeated) {
                void *item = *(void **)p;
                if (item != NULL) {
                    pbw_struct_free(row->fields, row->nfields, item);
                    free(item);
                    *(void **)p = NULL;
                }
            } else {
                void *items = *(void **)p;
                size_t n = *(size_t *)((char *)base + row->count);
                for (j = 0; j < n; j++) {
                    pbw_struct_free(row->fields, row->nfields,
                                    (char *)items + j * row->elem);
                }
                free(items);
                *(void **)p = NULL;
            }
        } else if (row->kind == PBW_SF_STRING) {
            if (!row->repeated) {
                free(*(void **)p);
                *(void **)p = NULL;
            } else {
                char **items = *(char ***)p;
                size_t n = *(size_t *)((char *)base + row->count);
                for (j = 0; j < n; j++) {
                    free(items[j]);
                }
                free(items);
                *(void **)p = NULL;
            }
        } else if (row->repeated) {
            free(*(void **)p);
            *(void **)p = NULL;
        }
    }
}

/* FileDescriptorSet bytes are copied into schema-owned memory by
 * pbw_descriptor_set_load, so the file buffer is released here. */
pbw_status
pbw_struct_schema_load(const char *path, pbw_schema **out)
{
    FILE *f;
    long size;
    size_t len;
    uint8_t *desc;
    pbw_status status;

    if (out == NULL) {
        return PBW_ERR_INVALID;
    }
    *out = NULL;
    if (path == NULL) {
        return PBW_ERR_INVALID;
    }
    f = fopen(path, "rb");
    if (f == NULL) {
        return PBW_ERR_INVALID;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return PBW_ERR_INVALID;
    }
    size = ftell(f);
    if (size <= 0) {
        fclose(f);
        return PBW_ERR_INVALID;
    }
    rewind(f);
    desc = malloc((size_t)size);
    if (desc == NULL) {
        fclose(f);
        return PBW_ERR_NOMEM;
    }
    len = fread(desc, 1, (size_t)size, f);
    fclose(f);
    if (len != (size_t)size) {
        free(desc);
        return PBW_ERR_INVALID;
    }
    status = pbw_descriptor_set_load(desc, len, out);
    free(desc);
    return status;
}

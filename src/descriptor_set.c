/* FileDescriptorSet loader: a standard descriptor set produced by protoc / buf
 * becomes a pbw_schema, so callers get a runtime schema without generating code
 * and without pbwire depending on protobuf.
 *
 * Only the fields that carry runtime semantics are read: names, numbers, types,
 * labels, oneofs, the `packed` option and the file syntax. Everything else
 * (services, extensions, reserved ranges, source code info, other options) is
 * skipped like any unknown field.
 *
 * Names are stored fully qualified without the leading dot: a message `Inner`
 * nested in `Outer` in package `a.b` is "a.b.Outer.Inner". Field type names are
 * normalized the same way.
 */

#include "internal.h"

#include <stdarg.h>
#include <stdio.h>

/* buffer size for a fully qualified name; longer names are rejected */
#define DS_MAX_NAME 512

typedef struct {
    uint32_t field;
    uint32_t wire;
    uint64_t varint; /* PBW_WIRE_VARINT */
    const uint8_t *data;
    size_t len;
} ds_field;

static void
ds_error(pbw_schema *schema, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(schema->error, sizeof(schema->error), fmt, ap);
    va_end(ap);
}

/* Reads the next field. 1 = field, 0 = end, -1 = malformed. Legacy groups never
 * appear in descriptor.proto, so their wire types are rejected. */
static int
ds_next(const uint8_t **pp, const uint8_t *end, ds_field *f)
{
    const uint8_t *p = *pp;
    uint64_t value;
    size_t used;
    uint32_t tag;

    if (p >= end) {
        return 0;
    }
    if (!pbw_tag_read(p, end, &tag, &used)) {
        return -1;
    }
    p += used;
    f->field = tag >> 3;
    f->wire = tag & 7u;
    f->varint = 0;
    f->data = NULL;
    f->len = 0;
    if (f->field == 0) {
        return -1;
    }
    switch (f->wire) {
    case PBW_WIRE_VARINT:
        if (!pbw_varint_read(p, end, &value, &used)) {
            return -1;
        }
        *pp = p + used;
        f->varint = value;
        return 1;
    case PBW_WIRE_LEN:
        if (!pbw_varint_read(p, end, &value, &used)) {
            return -1;
        }
        p += used;
        if (value > (uint64_t)(end - p)) {
            return -1;
        }
        f->data = p;
        f->len = (size_t)value;
        *pp = p + f->len;
        return 1;
    case PBW_WIRE_64BIT:
    case PBW_WIRE_32BIT: {
        size_t size = f->wire == PBW_WIRE_64BIT ? 8u : 4u;
        if ((size_t)(end - p) < size) {
            return -1;
        }
        f->data = p;
        f->len = size;
        *pp = p + size;
        return 1;
    }
    default:
        return -1;
    }
}

/* Copies a length-delimited string out of the wire. An empty string is not an
 * error: `package` and `type_name` are legitimately absent. */
static pbw_status
ds_str(char *buf, size_t cap, const ds_field *f)
{
    if (f->wire != PBW_WIRE_LEN) {
        return PBW_ERR_INVALID;
    }
    if (f->len == 0) {
        buf[0] = '\0';
        return PBW_OK;
    }
    if (f->len >= cap) {
        return PBW_ERR_LIMIT;
    }
    memcpy(buf, f->data, f->len);
    buf[f->len] = '\0';
    return PBW_OK;
}

/* Field 1 of a DescriptorProto / EnumDescriptorProto. */
static pbw_status
ds_local_name(char *buf, size_t cap, const uint8_t *p, const uint8_t *end)
{
    ds_field f;

    for (;;) {
        int rc = ds_next(&p, end, &f);
        if (rc < 0) {
            return PBW_ERR_INVALID;
        }
        if (rc == 0) {
            return PBW_ERR_INVALID; /* no name */
        }
        if (f.field == 1) {
            return ds_str(buf, cap, &f);
        }
    }
}

static pbw_status
ds_fqname(pbw_schema *schema, char *out, size_t cap, const char *parent,
          const uint8_t *p, const uint8_t *end)
{
    char local[DS_MAX_NAME];
    pbw_status status;
    int written;

    if ((status = ds_local_name(local, sizeof(local), p, end)) != PBW_OK) {
        ds_error(schema, "descriptor without a name");
        return status;
    }
    written = parent[0] == '\0' ? snprintf(out, cap, "%s", local)
                                : snprintf(out, cap, "%s.%s", parent, local);
    if (written < 0 || (size_t)written >= cap) {
        ds_error(schema, "name too long: %.64s...", local);
        return PBW_ERR_LIMIT;
    }
    return PBW_OK;
}

/* FieldOptions.packed, when the schema spells it out. */
static pbw_status
ds_field_options(const uint8_t *p, const uint8_t *end, int *packed)
{
    ds_field f;

    for (;;) {
        int rc = ds_next(&p, end, &f);
        if (rc < 0) {
            return PBW_ERR_INVALID;
        }
        if (rc == 0) {
            return PBW_OK;
        }
        if (f.field == 2 && f.wire == PBW_WIRE_VARINT) {
            *packed = f.varint != 0;
        }
    }
}

static pbw_status
ds_field_proto(pbw_schema *schema, pbw_descriptor *msg, int proto3,
               const uint8_t *p, const uint8_t *end)
{
    char name[DS_MAX_NAME] = "";
    char type_name[DS_MAX_NAME] = "";
    pbw_field *field;
    pbw_status status;
    ds_field f;
    uint32_t number = 0;
    uint32_t type = 0;
    uint32_t label = PBW_LABEL_OPTIONAL;
    uint32_t oneof = 0;
    int has_number = 0;
    int has_type = 0;
    int has_oneof = 0;
    int packed = -1;

    for (;;) {
        int rc = ds_next(&p, end, &f);
        if (rc < 0) {
            return PBW_ERR_INVALID;
        }
        if (rc == 0) {
            break;
        }
        switch (f.field) {
        case 1: /* name */
            if ((status = ds_str(name, sizeof(name), &f)) != PBW_OK) {
                return status;
            }
            break;
        case 3: /* number */
            if (f.wire != PBW_WIRE_VARINT) {
                return PBW_ERR_INVALID;
            }
            number = (uint32_t)f.varint;
            has_number = 1;
            break;
        case 4: /* label */
            if (f.wire != PBW_WIRE_VARINT) {
                return PBW_ERR_INVALID;
            }
            label = (uint32_t)f.varint;
            break;
        case 5: /* type */
            if (f.wire != PBW_WIRE_VARINT) {
                return PBW_ERR_INVALID;
            }
            type = (uint32_t)f.varint;
            has_type = 1;
            break;
        case 6: /* type_name, ".pkg.Type" */
            if ((status = ds_str(type_name, sizeof(type_name), &f)) != PBW_OK) {
                return status;
            }
            break;
        case 8: /* options */
            if ((status = ds_field_options(f.data, f.data + f.len, &packed)) !=
                PBW_OK) {
                return status;
            }
            break;
        case 9: /* oneof_index is 0-based on the wire, 1-based here */
            if (f.wire != PBW_WIRE_VARINT) {
                return PBW_ERR_INVALID;
            }
            oneof = (uint32_t)f.varint + 1u;
            has_oneof = 1;
            break;
        default:
            break;
        }
    }

    if (name[0] == '\0' || !has_number || !has_type ||
        number < 1 || number > PBW_MAX_FIELD_NUMBER ||
        label < PBW_LABEL_OPTIONAL || label > PBW_LABEL_REPEATED ||
        type < PBW_TYPE_DOUBLE || type > PBW_TYPE_SINT64) {
        ds_error(schema, "invalid field %s.%s", pbw_descriptor_name(msg), name);
        return PBW_ERR_INVALID;
    }
    /* Groups are legacy proto2 syntax. pbwire skips them on decode and cannot
     * encode them, so a group field is left out of the schema: its field number
     * then behaves exactly like any other unknown field. Failing the whole load
     * instead would make every legacy proto2 descriptor set unusable. */
    if (type == PBW_TYPE_GROUP) {
        return PBW_OK;
    }
    /* proto3 packs repeated primitives unless the schema says otherwise */
    if (packed < 0) {
        packed = proto3 && label == PBW_LABEL_REPEATED &&
                 pbw_is_packable((pbw_type)type);
    }
    field = pbw_message_add_field(msg, name, number, (pbw_type)type,
                                  (pbw_label)label);
    if (field == NULL) {
        /* pbw_message_add_field rejects duplicates and out-of-range numbers */
        ds_error(schema, "duplicate or invalid field %s.%s (%u)",
                 pbw_descriptor_name(msg), name, number);
        return PBW_ERR_INVALID;
    }
    if (type == PBW_TYPE_MESSAGE || type == PBW_TYPE_ENUM) {
        pbw_field_set_type_name(field,
                                type_name[0] == '.' ? type_name + 1 : type_name);
    }
    if (packed) {
        pbw_field_set_packed(field, 1);
    }
    if (has_oneof) {
        pbw_field_set_oneof(field, oneof);
    }
    return PBW_OK;
}

static pbw_status
ds_enum_proto(pbw_schema *schema, const char *parent, const uint8_t *p,
              const uint8_t *end)
{
    char fqname[DS_MAX_NAME];
    char value_name[DS_MAX_NAME] = "";
    pbw_descriptor *desc;
    pbw_status status;
    ds_field f;

    if ((status = ds_fqname(schema, fqname, sizeof(fqname), parent, p, end)) !=
        PBW_OK) {
        return status;
    }
    desc = pbw_schema_add_enum(schema, fqname);
    if (desc == NULL) {
        ds_error(schema, "duplicate or invalid enum %s", fqname);
        return PBW_ERR_INVALID;
    }
    for (;;) {
        const uint8_t *vp;
        const uint8_t *vend;
        int32_t number = 0;
        int rc = ds_next(&p, end, &f);

        if (rc < 0) {
            return PBW_ERR_INVALID;
        }
        if (rc == 0) {
            return PBW_OK;
        }
        if (f.field != 2 || f.wire != PBW_WIRE_LEN) { /* value */
            continue;
        }
        vp = f.data;
        vend = f.data + f.len;
        value_name[0] = '\0';
        for (;;) {
            int vrc = ds_next(&vp, vend, &f);
            if (vrc < 0) {
                return PBW_ERR_INVALID;
            }
            if (vrc == 0) {
                break;
            }
            if (f.field == 1) {
                if ((status = ds_str(value_name, sizeof(value_name), &f)) !=
                    PBW_OK) {
                    return status;
                }
            } else if (f.field == 2 && f.wire == PBW_WIRE_VARINT) {
                number = (int32_t)(uint32_t)f.varint;
            }
        }
        if (value_name[0] == '\0') {
            ds_error(schema, "enum value without a name in %s", fqname);
            return PBW_ERR_INVALID;
        }
        /* pbw_enum_add_value cannot report OOM; a lost value would fail link
         * only if something references it, which is also an allocation path. */
        pbw_enum_add_value(desc, value_name, number);
    }
}

static pbw_status
ds_message_proto(pbw_schema *schema, int proto3, const char *parent,
                 const uint8_t *p, const uint8_t *end, uint32_t depth)
{
    char fqname[DS_MAX_NAME];
    pbw_descriptor *msg;
    pbw_status status;
    ds_field f;

    if (depth >= PBW_MAX_DEPTH) {
        return PBW_ERR_LIMIT;
    }
    if ((status = ds_fqname(schema, fqname, sizeof(fqname), parent, p, end)) !=
        PBW_OK) {
        return status;
    }
    msg = pbw_schema_add_message(schema, fqname);
    if (msg == NULL) {
        ds_error(schema, "duplicate or invalid message %s", fqname);
        return PBW_ERR_INVALID;
    }
    for (;;) {
        int rc = ds_next(&p, end, &f);

        if (rc < 0) {
            return PBW_ERR_INVALID;
        }
        if (rc == 0) {
            return PBW_OK;
        }
        if (f.wire != PBW_WIRE_LEN) {
            continue;
        }
        switch (f.field) {
        case 2: /* field */
            status = ds_field_proto(schema, msg, proto3, f.data, f.data + f.len);
            break;
        case 3: /* nested_type */
            status = ds_message_proto(schema, proto3, fqname, f.data,
                                      f.data + f.len, depth + 1);
            break;
        case 4: /* enum_type */
            status = ds_enum_proto(schema, fqname, f.data, f.data + f.len);
            break;
        default:
            status = PBW_OK;
            break;
        }
        if (status != PBW_OK) {
            return status;
        }
    }
}

static pbw_status
ds_file_proto(pbw_schema *schema, const uint8_t *p, const uint8_t *end)
{
    char package[DS_MAX_NAME] = "";
    char syntax[DS_MAX_NAME] = "";
    pbw_status status;
    ds_field f;
    int proto3 = 0;
    int has_edition = 0;
    const uint8_t *walk;

    /* pass 1: syntax and package may sit after message_type on the wire */
    walk = p;
    for (;;) {
        int rc = ds_next(&walk, end, &f);
        if (rc < 0) {
            return PBW_ERR_INVALID;
        }
        if (rc == 0) {
            break;
        }
        if (f.field == 2) {
            if ((status = ds_str(package, sizeof(package), &f)) != PBW_OK) {
                return status;
            }
        } else if (f.field == 12) {
            if ((status = ds_str(syntax, sizeof(syntax), &f)) != PBW_OK) {
                return status;
            }
        } else if (f.field == 14 && f.wire == PBW_WIRE_VARINT) {
            has_edition = 1;
        }
    }
    proto3 = has_edition || strcmp(syntax, "proto3") == 0;

    /* pass 2: declarations */
    walk = p;
    for (;;) {
        int rc = ds_next(&walk, end, &f);
        if (rc < 0) {
            return PBW_ERR_INVALID;
        }
        if (rc == 0) {
            return PBW_OK;
        }
        if (f.wire != PBW_WIRE_LEN) {
            continue;
        }
        if (f.field == 4) { /* message_type */
            status = ds_message_proto(schema, proto3, package, f.data,
                                      f.data + f.len, 0);
        } else if (f.field == 5) { /* enum_type */
            status = ds_enum_proto(schema, package, f.data, f.data + f.len);
        } else {
            status = PBW_OK;
        }
        if (status != PBW_OK) {
            return status;
        }
    }
}

pbw_status
pbw_descriptor_set_load(const void *data, size_t len, pbw_schema **out)
{
    const uint8_t *p = (const uint8_t *)data;
    const uint8_t *end;
    pbw_schema *schema;
    pbw_status status = PBW_OK;
    ds_field f;

    if (out == NULL) {
        return PBW_ERR_INVALID;
    }
    *out = NULL;
    if (data == NULL) {
        return PBW_ERR_INVALID;
    }
    schema = pbw_schema_new();
    if (schema == NULL) {
        return PBW_ERR_NOMEM;
    }
    end = p + len;
    for (;;) {
        int rc = ds_next(&p, end, &f);
        if (rc < 0) {
            ds_error(schema, "malformed FileDescriptorSet");
            status = PBW_ERR_INVALID;
            break;
        }
        if (rc == 0) {
            break;
        }
        if (f.field == 1 && f.wire == PBW_WIRE_LEN) { /* file */
            status = ds_file_proto(schema, f.data, f.data + f.len);
            if (status != PBW_OK) {
                break;
            }
        }
    }
    if (status == PBW_OK) {
        status = pbw_schema_link(schema);
    }
    *out = schema; /* on failure the reason is in pbw_schema_error() */
    return status;
}

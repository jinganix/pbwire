/* FileDescriptorSet loader: a standard descriptor set produced by protoc / buf
 * becomes a pbw_schema, so callers get a runtime schema without generating code
 * and without pbwire depending on protobuf.
 *
 * Only the fields that carry runtime semantics are read: names, numbers, types,
 * labels, oneofs, the `packed` option, the file syntax and services (names
 * plus method input/output types, for routing; the core does no RPC).
 * Everything else (extensions, reserved ranges, source code info, other
 * options) is skipped like any unknown field.
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
/* proto2 defaults are short literals; longer ones are rejected */
#define DS_MAX_DEFAULT 4096

/* Editions FeatureSet resolution: file > message/enum > field, explicit
 * FieldOptions.packed still wins over every repeated_field_encoding.
 * Values are the FeatureSet enum numbers (0 = unset/unknown):
 * presence 1 EXPLICIT, 2 IMPLICIT, 3 LEGACY_REQUIRED; enum_open 1 OPEN,
 * 2 CLOSED; repack 1 PACKED, 2 EXPANDED; msg_enc 1 LENGTH_PREFIXED,
 * 2 DELIMITED. Loader-internal: presence/repack/msg_enc resolve to
 * single values at load, enum_open stays per-field (inherit = 0). */
typedef struct {
    int presence;
    int enum_open;
    int repack;
    int msg_enc;
} ds_features;

/* One parsed FieldDescriptorProto: regular fields and extensions share the
 * wire shape, only the presence of `extendee` tells them apart. */
typedef struct {
    char name[DS_MAX_NAME];
    char extendee[DS_MAX_NAME];
    char type_name[DS_MAX_NAME];
    char default_value[DS_MAX_DEFAULT];
    uint32_t number;
    uint32_t type;
    uint32_t label;
    uint32_t oneof;
    int has_number;
    int has_type;
    int has_oneof;
    int has_default;
    int packed;       /* -1 when the options do not say */
    ds_features feat; /* per-field FeatureSet override, zeros when absent */
} ds_field_info;

/* An extension waiting for its extendee: attach after every file is read, so
 * order between files does not matter. `scope` is the declaring scope
 * (containing message, or the file package) for the extension's full name. */
typedef struct {
    char extendee[DS_MAX_NAME];
    char scope[DS_MAX_NAME];
    ds_field_info info;
    /* file/message FeatureSet defaults of the declaring file, resolved at
     * queue time (the extendee may live in another file). */
    ds_features file;
} ds_pending_ext;

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
    const uint8_t *p;
    uint64_t value;
    size_t used;
    uint32_t tag;

    if (pp == NULL || end == NULL || f == NULL || *pp == NULL) {
        return -1;
    }
    p = *pp;
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
    case PBW_WIRE_LEN: {
        size_t len;

        if (!pbw_len_read(p, end, &len, &used)) {
            return -1;
        }
        p += used;
        if (len > pbw_remaining(end, p)) {
            return -1;
        }
        f->data = p;
        f->len = len;
        *pp = p + len;
        return 1;
    }
    case PBW_WIRE_64BIT:
    case PBW_WIRE_32BIT: {
        size_t size = f->wire == PBW_WIRE_64BIT ? 8u : 4u;
        if (pbw_remaining(end, p) < size) {
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
    if (buf == NULL || cap == 0 || f == NULL) {
        return PBW_ERR_INVALID;
    }
    if (f->wire != PBW_WIRE_LEN) {
        return PBW_ERR_INVALID;
    }
    if (f->data == NULL && f->len > 0) {
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

/* Parses a FeatureSet message (fields 1,2,3,5); utf8_validation and
 * json_format carry no codec semantics here. */
static pbw_status
ds_parse_featureset(const uint8_t *p, const uint8_t *end, ds_features *out)
{
    ds_field f;

    for (;;) {
        int rc = ds_next(&p, end, &f);
        uint64_t v;

        if (rc < 0) {
            return PBW_ERR_INVALID;
        }
        if (rc == 0) {
            return PBW_OK;
        }
        if (f.wire != PBW_WIRE_VARINT) {
            continue;
        }
        v = f.varint;
        switch (f.field) {
        case 1: /* field_presence */
            if (v >= 1 && v <= 3) {
                out->presence = (int)v;
            }
            break;
        case 2: /* enum_type */
            if (v >= 1 && v <= 2) {
                out->enum_open = (int)v;
            }
            break;
        case 3: /* repeated_field_encoding */
            if (v >= 1 && v <= 2) {
                out->repack = (int)v;
            }
            break;
        case 5: /* message_encoding */
            if (v >= 1 && v <= 2) {
                out->msg_enc = (int)v;
            }
            break;
        default:
            break;
        }
    }
}

/* FieldOptions.packed (field 2), when the schema spells it out, plus the
 * per-field FeatureSet override (field 21). */
static pbw_status
ds_field_options(const uint8_t *p, const uint8_t *end, int *packed,
                 ds_features *feat)
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
        } else if (f.field == 21 && f.wire == PBW_WIRE_LEN) {
            pbw_status status =
                ds_parse_featureset(f.data, f.data + f.len, feat);
            if (status != PBW_OK) {
                return status;
            }
        }
    }
}

/* Parses one FieldDescriptorProto. Field 2 (`extendee`) is only set on
 * extensions; field 7 is the proto2 default; field 10 (`json_name`) carries no
 * runtime semantics yet. */
static pbw_status
ds_parse_field(ds_field_info *out, const uint8_t *p, const uint8_t *end)
{
    ds_field f;
    pbw_status status;

    memset(out, 0, sizeof(*out));
    out->label = PBW_LABEL_OPTIONAL;
    out->packed = -1;
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
            if ((status = ds_str(out->name, sizeof(out->name), &f)) != PBW_OK) {
                return status;
            }
            break;
        case 2: /* extendee, ".pkg.Type" (extensions only) */
            if ((status = ds_str(out->extendee, sizeof(out->extendee), &f)) !=
                PBW_OK) {
                return status;
            }
            break;
        case 3: /* number */
            if (f.wire != PBW_WIRE_VARINT) {
                return PBW_ERR_INVALID;
            }
            out->number = (uint32_t)f.varint;
            out->has_number = 1;
            break;
        case 4: /* label */
            if (f.wire != PBW_WIRE_VARINT) {
                return PBW_ERR_INVALID;
            }
            out->label = (uint32_t)f.varint;
            break;
        case 5: /* type */
            if (f.wire != PBW_WIRE_VARINT) {
                return PBW_ERR_INVALID;
            }
            out->type = (uint32_t)f.varint;
            out->has_type = 1;
            break;
        case 6: /* type_name, ".pkg.Type" */
            if ((status = ds_str(out->type_name, sizeof(out->type_name), &f)) !=
                PBW_OK) {
                return status;
            }
            break;
        case 7: /* default_value, the raw .proto literal */
            if ((status = ds_str(out->default_value,
                                 sizeof(out->default_value), &f)) != PBW_OK) {
                return status;
            }
            out->has_default = 1;
            break;
        case 8: /* options */
            if ((status = ds_field_options(f.data, f.data + f.len,
                                           &out->packed, &out->feat)) !=
                PBW_OK) {
                return status;
            }
            break;
        case 9: /* oneof_index is 0-based on the wire, 1-based here */
            if (f.wire != PBW_WIRE_VARINT) {
                return PBW_ERR_INVALID;
            }
            out->oneof = (uint32_t)f.varint + 1u;
            out->has_oneof = 1;
            break;
        default:
            break;
        }
    }
    return PBW_OK;
}

/* Stores a parsed default literal on the field; OOM is the only failure. */
static pbw_status
ds_set_default(pbw_schema *schema, pbw_field *field, const char *def)
{
    size_t len;
    char *copy;

    if (def == NULL) {
        return PBW_OK;
    }
    len = strlen(def) + 1;
    copy = (char *)malloc(len);
    if (copy == NULL) {
        ds_error(schema, "out of memory");
        return PBW_ERR_NOMEM;
    }
    memcpy(copy, def, len);
    field->default_value = copy;
    return PBW_OK;
}

static pbw_status
ds_add_field(pbw_schema *schema, pbw_descriptor *msg,
             const ds_field_info *info, int is_extension,
             const ds_features *file)
{
    pbw_field *field;
    pbw_status status;
    int repack = file->repack;

    if (info->name[0] == '\0' || !info->has_number || !info->has_type ||
        info->number < 1 || info->number > PBW_MAX_FIELD_NUMBER ||
        info->label < PBW_LABEL_OPTIONAL || info->label > PBW_LABEL_REPEATED ||
        info->type < PBW_TYPE_DOUBLE || info->type > PBW_TYPE_SINT64) {
        ds_error(schema, "invalid field %s.%s", pbw_descriptor_name(msg),
                 info->name);
        return PBW_ERR_INVALID;
    }
    if (is_extension && info->has_oneof) {
        ds_error(schema, "extension %s.%s cannot be in a oneof",
                 pbw_descriptor_name(msg), info->name);
        return PBW_ERR_INVALID;
    }
    if (info->has_oneof && info->oneof == 0) {
        /* Wire oneof_index is 0-based and stored 1-based, so 0 only comes
         * from a 2^32-1 wrap. */
        ds_error(schema, "invalid oneof index for %s.%s",
                 pbw_descriptor_name(msg), info->name);
        return PBW_ERR_INVALID;
    }
    /* Packing default: explicit `packed` wins, then the field FeatureSet,
     * then the file/message default (PACKED for proto3/editions files,
     * EXPANDED for proto2). Unchanged when no editions features exist. */
    if (info->feat.repack != 0) {
        repack = info->feat.repack == 1;
    }
    if (info->packed >= 0) {
        repack = info->packed;
    }
    if (is_extension) {
        field = pbw_message_add_extension(msg, info->name, info->number,
                                          (pbw_type)info->type,
                                          (pbw_label)info->label);
    } else {
        field = pbw_message_add_field(msg, info->name, info->number,
                                      (pbw_type)info->type,
                                      (pbw_label)info->label);
    }
    if (field == NULL) {
        /* add rejects duplicates and out-of-range numbers */
        ds_error(schema, "duplicate or invalid field %s.%s (%u)",
                 pbw_descriptor_name(msg), info->name, info->number);
        return PBW_ERR_INVALID;
    }
    if (info->type == PBW_TYPE_MESSAGE || info->type == PBW_TYPE_ENUM ||
        info->type == PBW_TYPE_GROUP) {
        pbw_field_set_type_name(field, info->type_name[0] == '.'
                                           ? info->type_name + 1
                                           : info->type_name);
    }
    if (info->has_default) {
        if ((status = ds_set_default(schema, field, info->default_value)) !=
            PBW_OK) {
            return status;
        }
    }
    if (repack && info->label == PBW_LABEL_REPEATED &&
        pbw_is_packable((pbw_type)info->type)) {
        pbw_field_set_packed(field, 1);
    }
    /* Editions resolution: field FeatureSet wins, file/message default
     * fills the rest. Non-editions files resolve to zeros (dialect-wide
     * behavior, as before). */
    field->presence =
        (uint8_t)(info->feat.presence ? info->feat.presence : file->presence);
    field->enum_open = (uint8_t)info->feat.enum_open;
    field->msg_enc =
        (uint8_t)(info->feat.msg_enc ? info->feat.msg_enc : file->msg_enc);
    if (info->has_oneof) {
        pbw_field_set_oneof(field, info->oneof);
    }
    return PBW_OK;
}

/* Queues an extension for attachment after every file is read. */
static pbw_status
ds_queue_extension(pbw_schema *schema, ds_pending_ext **exts, size_t *count,
                   size_t *cap, const ds_field_info *info, const char *scope,
                   const ds_features *file)
{
    ds_pending_ext *grown;

    if (*count == *cap) {
        size_t ncap = *cap ? *cap * 2 : 8;
        /* Extension counts are bounded by the input length, which reaches
         * the 32-bit wrap with tens of MB of hostile input. */
        if (ncap < *cap || ncap > SIZE_MAX / sizeof(*grown)) {
            ds_error(schema, "out of memory");
            return PBW_ERR_NOMEM;
        }
        grown = (ds_pending_ext *)realloc(*exts, ncap * sizeof(*grown));
        if (grown == NULL) {
            ds_error(schema, "out of memory");
            return PBW_ERR_NOMEM;
        }
        *exts = grown;
        *cap = ncap;
    }
    if (info->extendee[0] == '\0') {
        ds_error(schema, "extension %s has no extendee", info->name);
        return PBW_ERR_INVALID;
    }
    (*exts)[*count].info = *info;
    (*exts)[*count].file = *file;
    {
        const char *src =
            info->extendee[0] == '.' ? info->extendee + 1 : info->extendee;
        size_t len = strlen(src);
        if (len >= sizeof((*exts)[*count].extendee)) {
            ds_error(schema, "extendee name too long: %.64s...", src);
            return PBW_ERR_LIMIT;
        }
        memcpy((*exts)[*count].extendee, src, len + 1);
    }
    {
        size_t len = strlen(scope);
        if (len >= sizeof((*exts)[*count].scope)) {
            ds_error(schema, "scope name too long: %.64s...", scope);
            return PBW_ERR_LIMIT;
        }
        memcpy((*exts)[*count].scope, scope, len + 1);
    }
    (*count)++;
    return PBW_OK;
}

/* Attaches every queued extension to the message it extends. The dialect
 * follows the extendee: an extension of a proto2 message is proto2 itself. */
static pbw_status
ds_attach_extensions(pbw_schema *schema, ds_pending_ext *exts, size_t count)
{
    size_t i;

    for (i = 0; i < count; i++) {
        pbw_descriptor *target = pbw_schema_find(schema, exts[i].extendee);
        pbw_status status;
        char full[DS_MAX_NAME * 2];
        const pbw_field *field;
        char *copy;
        int written;

        if (target == NULL || target->is_enum) {
            ds_error(schema, "unknown extendee %s for extension %s",
                     exts[i].extendee, exts[i].info.name);
            return PBW_ERR_NOT_FOUND;
        }
        status = ds_add_field(schema, target, &exts[i].info, 1,
                              &exts[i].file);
        if (status != PBW_OK) {
            return status;
        }
        /* The full name is scope + short name, like the reference. */
        written = exts[i].scope[0] == '\0'
                      ? snprintf(full, sizeof(full), "%s", exts[i].info.name)
                      : snprintf(full, sizeof(full), "%s.%s", exts[i].scope,
                                 exts[i].info.name);
        if (written < 0 || (size_t)written >= sizeof(full)) {
            ds_error(schema, "extension name too long: %.64s...",
                     exts[i].info.name);
            return PBW_ERR_LIMIT;
        }
        copy = (char *)malloc((size_t)written + 1);
        if (copy == NULL) {
            ds_error(schema, "out of memory");
            return PBW_ERR_NOMEM;
        }
        memcpy(copy, full, (size_t)written + 1);
        /* the schema is not linked yet, so scan the unsorted fields */
        field = NULL;
        {
            size_t k;
            for (k = 0; k < target->field_count; k++) {
                if (target->fields[k].number == exts[i].info.number) {
                    field = &target->fields[k];
                    break;
                }
            }
        }
        if (field == NULL) {
            free(copy);
            ds_error(schema, "extension %s vanished", exts[i].info.name);
            return PBW_ERR_INVALID;
        }
        ((pbw_field *)field)->full_name = copy;
    }
    return PBW_OK;
}

static pbw_status
ds_enum_proto(pbw_schema *schema, const char *parent, const uint8_t *p,
              const uint8_t *end, int file_closed, int file_proto3)
{
    char fqname[DS_MAX_NAME];
    char value_name[DS_MAX_NAME] = "";
    pbw_descriptor *desc;
    pbw_status status;
    /* Editions enum_type: explicit EnumOptions.features wins, the file
     * default fills the rest. Options may follow the values on the wire,
     * so resolve first: openness only matters at link time. */
    int closed = file_closed;
    ds_field f;

    if ((status = ds_fqname(schema, fqname, sizeof(fqname), parent, p, end)) !=
        PBW_OK) {
        return status;
    }
    {
        const uint8_t *o = p;
        ds_field of;

        for (;;) {
            int orc = ds_next(&o, end, &of);

            if (orc < 0) {
                return PBW_ERR_INVALID;
            }
            if (orc == 0) {
                break;
            }
            if (of.field != 3 || of.wire != PBW_WIRE_LEN) { /* options */
                continue;
            }
            {
                const uint8_t *q = of.data;
                const uint8_t *qend = of.data + of.len;
                ds_field qf;

                for (;;) {
                    int qrc = ds_next(&q, qend, &qf);
                    ds_features feat;

                    if (qrc < 0) {
                        return PBW_ERR_INVALID;
                    }
                    if (qrc == 0) {
                        break;
                    }
                    if (qf.field != 7 || qf.wire != PBW_WIRE_LEN) {
                        continue;
                    }
                    memset(&feat, 0, sizeof(feat));
                    status = ds_parse_featureset(qf.data, qf.data + qf.len,
                                                 &feat);
                    if (status != PBW_OK) {
                        return status;
                    }
                    if (feat.enum_open != 0) {
                        closed = feat.enum_open == 2;
                    }
                }
            }
        }
    }
    desc = pbw_schema_add_enum(schema, fqname);
    if (desc == NULL) {
        ds_error(schema, "duplicate or invalid enum %s", fqname);
        return PBW_ERR_INVALID;
    }
    /* Openness rides the dialect flag: a closed enum rejects unknown
     * numbers exactly like a proto2 one. */
    desc->proto3 = closed ? 0 : file_proto3;
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
        status = pbw_enum_add_value(desc, value_name, number);
        if (status != PBW_OK) {
            ds_error(schema, "out of memory");
            return status;
        }
    }
}

static pbw_status
ds_message_proto(pbw_schema *schema, const ds_features *file,
                 const char *parent, const uint8_t *p, const uint8_t *end,
                 uint32_t depth, ds_pending_ext **exts, size_t *ext_count,
                 size_t *ext_cap)
{
    char fqname[DS_MAX_NAME];
    pbw_descriptor *msg;
    pbw_status status;
    /* Message-level FeatureSet overrides the file default for the fields
     * declared here; nested messages resolve against the file again. */
    ds_features feat = *file;
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
    /* Pre-scan for options: fields may precede them on the wire, but the
     * message-level FeatureSet must apply to every field declared here. */
    {
        const uint8_t *o = p;
        ds_field of;

        for (;;) {
            int orc = ds_next(&o, end, &of);

            if (orc < 0) {
                return PBW_ERR_INVALID;
            }
            if (orc == 0) {
                break;
            }
            if (of.field != 7 || of.wire != PBW_WIRE_LEN) {
                continue;
            }
            {
                const uint8_t *q = of.data;
                const uint8_t *qend = of.data + of.len;
                ds_field qf;

                for (;;) {
                    int qrc = ds_next(&q, qend, &qf);

                    if (qrc < 0) {
                        return PBW_ERR_INVALID;
                    }
                    if (qrc == 0) {
                        break;
                    }
                    if (qf.field == 12 && qf.wire == PBW_WIRE_LEN) {
                        status = ds_parse_featureset(qf.data,
                                                     qf.data + qf.len,
                                                     &feat);
                        if (status != PBW_OK) {
                            return status;
                        }
                    }
                }
            }
        }
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
        case 6: /* extension */
        {
            /* Heap, not stack: the struct carries a 4 KiB default buffer
             * and this function recurses per nesting level, so a stack
             * slot would multiply a hostile descriptor's depth into a
             * stack overflow. Parsing is load-time only, one malloc per
             * field is immaterial. */
            ds_field_info *info = (ds_field_info *)malloc(sizeof(*info));

            if (info == NULL) {
                ds_error(schema, "out of memory");
                return PBW_ERR_NOMEM;
            }
            status = ds_parse_field(info, f.data, f.data + f.len);
            if (status == PBW_OK) {
                if (f.field == 6 || info->extendee[0] != '\0') {
                    status = ds_queue_extension(schema, exts, ext_count,
                                                ext_cap, info, fqname,
                                                &feat);
                } else {
                    status =
                        ds_add_field(schema, msg, info, 0, &feat);
                }
            }
            free(info);
            if (status != PBW_OK) {
                return status;
            }
            break;
        }
        case 10: /* reserved_name */
        {
            char name[DS_MAX_NAME];
            char **grown;

            status = ds_str(name, sizeof(name), &f);
            if (status != PBW_OK) {
                return status;
            }
            /* reserved names are skipped silently by the text parser */
            if (msg->reserved_count == SIZE_MAX ||
                (msg->reserved_count + 1) > SIZE_MAX / sizeof(*grown)) {
                ds_error(schema, "out of memory");
                return PBW_ERR_NOMEM;
            }
            grown = (char **)realloc(msg->reserved, (msg->reserved_count + 1) *
                                                        sizeof(*grown));
            if (grown == NULL) {
                ds_error(schema, "out of memory");
                return PBW_ERR_NOMEM;
            }
            msg->reserved = grown;
            grown[msg->reserved_count] = NULL;
            {
                size_t len = strlen(name);
                char *copy;
                if (len == SIZE_MAX) {
                    ds_error(schema, "out of memory");
                    return PBW_ERR_NOMEM;
                }
                copy = (char *)malloc(len + 1);
                if (copy == NULL) {
                    ds_error(schema, "out of memory");
                    return PBW_ERR_NOMEM;
                }
                memcpy(copy, name, len);
                copy[len] = '\0';
                grown[msg->reserved_count] = copy;
            }
            msg->reserved_count++;
            status = PBW_OK;
            break;
        }
        case 7: /* options: map_entry plus the message-level FeatureSet */
        {
            const uint8_t *o = f.data;
            const uint8_t *oend = f.data + f.len;
            ds_field of;

            for (;;) {
                int orc = ds_next(&o, oend, &of);

                if (orc < 0) {
                    return PBW_ERR_INVALID;
                }
                if (orc == 0) {
                    break;
                }
                if (of.field == 7 && of.wire == PBW_WIRE_VARINT &&
                    of.varint != 0) {
                    msg->map_entry = 1;
                } else if (of.field == 12 && of.wire == PBW_WIRE_LEN) {
                    status = ds_parse_featureset(of.data, of.data + of.len,
                                                 &feat);
                    if (status != PBW_OK) {
                        return status;
                    }
                } else if (of.field == 1 && of.wire == PBW_WIRE_VARINT &&
                           of.varint != 0) {
                    /* MessageOptions.message_set_wire_format */
                    msg->is_messageset = 1;
                }
            }
            status = PBW_OK;
            break;
        }
        case 3: /* nested_type: fresh scope, back to the file default */
            status = ds_message_proto(schema, file, fqname, f.data,
                                      f.data + f.len, depth + 1, exts,
                                      ext_count, ext_cap);
            break;
        case 4: /* enum_type */
            status = ds_enum_proto(schema, fqname, f.data, f.data + f.len,
                                   file->enum_open == 2, schema->proto3);
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

/* One parsed MethodDescriptorProto: names only, no transport. */
typedef struct {
    char name[DS_MAX_NAME];
    char input[DS_MAX_NAME];
    char output[DS_MAX_NAME];
    int client_streaming;
    int server_streaming;
} ds_method_info;

static char *
ds_dup(const char *s)
{
    size_t len;
    char *copy;

    if (s == NULL) {
        return NULL;
    }
    len = strlen(s) + 1;
    copy = (char *)malloc(len);
    if (copy == NULL) {
        return NULL;
    }
    memcpy(copy, s, len);
    return copy;
}

/* Without the leading dot: ".pkg.Msg" -> "pkg.Msg", like field types. */
static const char *
ds_unroot(const char *s)
{
    return s[0] == '.' ? s + 1 : s;
}

static pbw_status
ds_method_proto(ds_method_info *out, const uint8_t *p, const uint8_t *end)
{
    ds_field f;

    memset(out, 0, sizeof(*out));
    for (;;) {
        int rc = ds_next(&p, end, &f);
        pbw_status status;

        if (rc < 0) {
            return PBW_ERR_INVALID;
        }
        if (rc == 0) {
            break;
        }
        switch (f.field) {
        case 1: /* name */
            if ((status = ds_str(out->name, sizeof(out->name), &f)) !=
                PBW_OK) {
                return status;
            }
            break;
        case 2: /* input_type */
        case 3: /* output_type */
        {
            char tmp[DS_MAX_NAME];
            const char *bare;
            size_t len;

            if ((status = ds_str(tmp, sizeof(tmp), &f)) != PBW_OK) {
                return status;
            }
            bare = ds_unroot(tmp);
            len = strlen(bare) + 1;
            if (len > sizeof(out->input)) {
                return PBW_ERR_LIMIT;
            }
            memcpy(f.field == 2 ? out->input : out->output, bare, len);
            break;
        }
        case 5: /* client_streaming */
            if (f.wire != PBW_WIRE_VARINT) {
                return PBW_ERR_INVALID;
            }
            out->client_streaming = f.varint != 0;
            break;
        case 6: /* server_streaming */
            if (f.wire != PBW_WIRE_VARINT) {
                return PBW_ERR_INVALID;
            }
            out->server_streaming = f.varint != 0;
            break;
        default:
            break;
        }
    }
    return PBW_OK;
}

/* Releases a service the loader built but cannot keep (load failure). */
static void
ds_free_service(pbw_service *svc)
{
    size_t m;

    if (svc == NULL) {
        return;
    }
    /* method_count only counts stored methods, so this frees exactly
     * what the parse loop committed. */
    for (m = 0; m < svc->method_count; m++) {
        free(svc->methods[m].name);
        free(svc->methods[m].input_type);
        free(svc->methods[m].output_type);
    }
    free(svc->methods);
    free(svc->name);
    free(svc);
}

/* One ServiceDescriptorProto: the service joins the schema, its methods join
 * the service in wire order. Unknown method entries fail the load: a
 * routing table that silently drops an rpc is worse than no table. */
static pbw_status
ds_service_proto(pbw_schema *schema, const char *package, const uint8_t *p,
                 const uint8_t *end)
{
    char fqname[DS_MAX_NAME];
    pbw_service *svc;
    pbw_status status;
    ds_field f;

    if ((status = ds_fqname(schema, fqname, sizeof(fqname), package, p,
                            end)) != PBW_OK) {
        return status;
    }
    svc = (pbw_service *)calloc(1, sizeof(*svc));
    if (svc == NULL) {
        ds_error(schema, "out of memory");
        return PBW_ERR_NOMEM;
    }
    svc->name = ds_dup(fqname);
    if (svc->name == NULL) {
        free(svc);
        ds_error(schema, "out of memory");
        return PBW_ERR_NOMEM;
    }
    svc->schema = schema;
    for (;;) {
        int rc = ds_next(&p, end, &f);

        if (rc < 0) {
            status = PBW_ERR_INVALID;
            break;
        }
        if (rc == 0) {
            status = PBW_OK;
            break;
        }
        if (f.field != 2 || f.wire != PBW_WIRE_LEN) { /* method */
            continue;
        }
        {
            ds_method_info info;
            pbw_method *grown;
            pbw_method *m;

            status =
                ds_method_proto(&info, f.data, f.data + f.len);
            if (status != PBW_OK) {
                break;
            }
            if (info.name[0] == '\0' || info.input[0] == '\0' ||
                info.output[0] == '\0') {
                status = PBW_ERR_INVALID;
                break;
            }
            if (svc->method_count == SIZE_MAX ||
                (svc->method_count + 1) > SIZE_MAX / sizeof(*grown)) {
                ds_error(schema, "out of memory");
                status = PBW_ERR_NOMEM;
                break;
            }
            grown = (pbw_method *)realloc(svc->methods,
                                          (svc->method_count + 1) *
                                              sizeof(*grown));
            if (grown == NULL) {
                ds_error(schema, "out of memory");
                status = PBW_ERR_NOMEM;
                break;
            }
            svc->methods = grown;
            m = &grown[svc->method_count];
            memset(m, 0, sizeof(*m));
            m->name = ds_dup(info.name);
            m->input_type = ds_dup(info.input);
            m->output_type = ds_dup(info.output);
            if (m->name == NULL || m->input_type == NULL ||
                m->output_type == NULL) {
                free(m->name);
                free(m->input_type);
                free(m->output_type);
                ds_error(schema, "out of memory");
                status = PBW_ERR_NOMEM;
                break;
            }
            m->client_streaming = info.client_streaming;
            m->server_streaming = info.server_streaming;
            svc->method_count++;
        }
    }
    if (status == PBW_OK) {
        pbw_service **grown;

        if (schema->service_count == SIZE_MAX ||
            (schema->service_count + 1) > SIZE_MAX / sizeof(*grown)) {
            ds_error(schema, "out of memory");
            status = PBW_ERR_NOMEM;
        } else {
            grown = (pbw_service **)realloc(schema->services,
                                            (schema->service_count + 1) *
                                                sizeof(*grown));
            if (grown == NULL) {
                ds_error(schema, "out of memory");
                status = PBW_ERR_NOMEM;
            } else {
                schema->services = grown;
                grown[schema->service_count++] = svc;
                return PBW_OK;
            }
        }
    }
    ds_free_service(svc);
    return status;
}

static pbw_status
ds_file_proto(pbw_schema *schema, const uint8_t *p, const uint8_t *end,
              ds_pending_ext **exts, size_t *ext_count, size_t *ext_cap)
{
    char package[DS_MAX_NAME] = "";
    char syntax[DS_MAX_NAME] = "";
    pbw_status status;
    ds_field f;
    ds_features feat;
    int proto3 = 0;
    int edition = 0;
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
            edition = (int)(f.varint > INT32_MAX ? INT32_MAX : f.varint);
        }
    }
    /* An editions file with the proto2 edition keeps proto2 semantics;
     * every other edition behaves like proto3 unless told otherwise by
     * its FeatureSet. */
    proto3 = has_edition ? edition != 998 : strcmp(syntax, "proto3") == 0;
    /* Descriptors created below inherit the dialect of the file they came
     * from, so one set may hold proto2 and proto3 files at once. */
    schema->proto3 = proto3;
    schema->edition = has_edition ? edition : 0;
    /* File FeatureSet over the syntax defaults. FileOptions.features is
     * field 50; options may sit anywhere, so re-walk for them. */
    memset(&feat, 0, sizeof(feat));
    feat.presence = has_edition ? 1 : 0;            /* EXPLICIT unless overridden */
    feat.enum_open = has_edition || proto3 ? 1 : 0; /* OPEN unless proto2 */
    feat.repack = proto3 || has_edition;            /* PACKED for proto3/editions */
    feat.msg_enc = has_edition ? 1 : 0;             /* LENGTH_PREFIXED for editions */
    walk = p;
    for (;;) {
        int rc = ds_next(&walk, end, &f);
        if (rc < 0) {
            return PBW_ERR_INVALID;
        }
        if (rc == 0) {
            break;
        }
        if (f.field != 8 || f.wire != PBW_WIRE_LEN) { /* options */
            continue;
        }
        {
            const uint8_t *o = f.data;
            const uint8_t *oend = f.data + f.len;
            ds_field of;

            for (;;) {
                int orc = ds_next(&o, oend, &of);

                if (orc < 0) {
                    return PBW_ERR_INVALID;
                }
                if (orc == 0) {
                    break;
                }
                if (of.field != 50 || of.wire != PBW_WIRE_LEN) {
                    continue;
                }
                status = ds_parse_featureset(of.data, of.data + of.len,
                                             &feat);
                if (status != PBW_OK) {
                    return status;
                }
            }
        }
    }

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
            status = ds_message_proto(schema, &feat, package, f.data,
                                      f.data + f.len, 0, exts, ext_count,
                                      ext_cap);
        } else if (f.field == 5) { /* enum_type */
            status = ds_enum_proto(schema, package, f.data, f.data + f.len,
                                   feat.enum_open == 2, proto3);
        } else if (f.field == 6) { /* service */
            status = ds_service_proto(schema, package, f.data,
                                      f.data + f.len);
        } else if (f.field == 7) { /* extension */
            /* Heap, not stack: see the field/extension case above. */
            ds_field_info *info = (ds_field_info *)malloc(sizeof(*info));

            if (info == NULL) {
                ds_error(schema, "out of memory");
                return PBW_ERR_NOMEM;
            }
            status = ds_parse_field(info, f.data, f.data + f.len);
            if (status == PBW_OK) {
                status = ds_queue_extension(schema, exts, ext_count, ext_cap,
                                            info, package, &feat);
            }
            free(info);
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
    const uint8_t *p;
    const uint8_t *end;
    pbw_schema *schema;
    pbw_status status = PBW_OK;
    ds_pending_ext *exts = NULL;
    size_t ext_count = 0;
    size_t ext_cap = 0;
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
    p = pbw_normalize_input(data, len);
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
            status = ds_file_proto(schema, f.data, f.data + f.len, &exts,
                                   &ext_count, &ext_cap);
            if (status != PBW_OK) {
                break;
            }
        }
    }
    if (status == PBW_OK) {
        status = ds_attach_extensions(schema, exts, ext_count);
    }
    free(exts);
    if (status == PBW_OK) {
        status = pbw_schema_link(schema);
    } else if (schema->error[0] == '\0') {
        /* Every failure must be explainable through pbw_schema_error(), also
         * the ones deep in the per-file parsers. */
        ds_error(schema, "malformed FileDescriptorSet");
    }
    *out = schema; /* on failure the reason is in pbw_schema_error() */
    return status;
}

/* Descriptor model: schema registry, field metadata, name resolution, lookup
 * tables. */

#include "internal.h"

#include <stdio.h>

static char *
dup_str(const char *s)
{
    size_t len;
    char *copy;

    if (s == NULL) {
        return NULL;
    }
    len = strlen(s) + 1;
    copy = (char *)malloc(len);
    if (copy != NULL) {
        memcpy(copy, s, len);
    }
    return copy;
}

void
pbw_descriptor_free(pbw_descriptor *desc)
{
    size_t i;

    if (desc == NULL) {
        return;
    }
    for (i = 0; i < desc->field_count; i++) {
        free(desc->fields[i].name);
        free(desc->fields[i].full_name);
        free(desc->fields[i].type_name);
        free(desc->fields[i].default_value);
    }
    free(desc->fields);
    for (i = 0; i < desc->enum_count; i++) {
        free(desc->enum_values[i].name);
    }
    free(desc->enum_values);
    free(desc->fnum_map);
    free(desc->tag_map);
    free(desc->tag_action);
    for (i = 0; i < desc->reserved_count; i++) {
        free(desc->reserved[i]);
    }
    free(desc->reserved);
    free(desc->name);
    free(desc);
}

pbw_schema *
pbw_schema_new(void)
{
    pbw_schema *schema = (pbw_schema *)calloc(1, sizeof(pbw_schema));

    /* Hand-built schemas default to proto3 semantics, which is what this API
     * did before a dialect could be chosen. */
    if (schema != NULL) {
        schema->proto3 = 1;
    }
    return schema;
}

void
pbw_schema_set_proto3(pbw_schema *schema, int proto3)
{
    if (schema != NULL) {
        schema->proto3 = proto3 != 0;
    }
}

void
pbw_schema_free(pbw_schema *schema)
{
    size_t i;

    if (schema == NULL) {
        return;
    }
    for (i = 0; i < schema->count; i++) {
        pbw_descriptor_free(schema->descs[i]);
    }
    free(schema->descs);
    free(schema);
}

static pbw_descriptor *
add_descriptor(pbw_schema *schema, const char *name,
               int is_enum)
{
    pbw_descriptor *desc;
    pbw_descriptor **descs;

    if (schema == NULL || name == NULL || pbw_schema_find(schema, name) != NULL) {
        return NULL;
    }
    if (schema->count == schema->cap) {
        size_t cap = schema->cap ? schema->cap * 2 : 8;
        descs = (pbw_descriptor **)realloc(schema->descs, cap * sizeof(*descs));
        if (descs == NULL) {
            return NULL;
        }
        schema->descs = descs;
        schema->cap = cap;
    }
    desc = (pbw_descriptor *)calloc(1, sizeof(*desc));
    if (desc == NULL) {
        return NULL;
    }
    desc->name = dup_str(name);
    if (desc->name == NULL) {
        free(desc);
        return NULL;
    }
    desc->is_enum = is_enum;
    desc->proto3 = schema->proto3;
    desc->schema = schema;
    schema->descs[schema->count++] = desc;
    schema->linked = 0;
    return desc;
}

pbw_descriptor *
pbw_schema_add_message(pbw_schema *schema, const char *name)
{
    return add_descriptor(schema, name, 0);
}

pbw_descriptor *
pbw_schema_add_enum(pbw_schema *schema, const char *name)
{
    return add_descriptor(schema, name, 1);
}

pbw_descriptor *
pbw_schema_find(const pbw_schema *schema, const char *name)
{
    size_t i;

    if (schema == NULL || name == NULL) {
        return NULL;
    }
    for (i = 0; i < schema->count; i++) {
        if (strcmp(schema->descs[i]->name, name) == 0) {
            return schema->descs[i];
        }
    }
    return NULL;
}

void
pbw_enum_add_value(pbw_descriptor *enum_desc, const char *name,
                   int32_t number)
{
    struct pbw_enum_value *values;
    char *copy;

    if (enum_desc == NULL || !enum_desc->is_enum || name == NULL) {
        return;
    }
    copy = dup_str(name);
    if (copy == NULL) {
        return;
    }
    values = (struct pbw_enum_value *)realloc(
        enum_desc->enum_values, (enum_desc->enum_count + 1) * sizeof(*values));
    if (values == NULL) {
        free(copy);
        return;
    }
    enum_desc->enum_values = values;
    values[enum_desc->enum_count].name = copy;
    values[enum_desc->enum_count].number = number;
    enum_desc->enum_count++;
}

int
pbw_enum_find_name(const pbw_descriptor *enum_desc, int32_t number,
                   const char **name)
{
    size_t i;

    if (enum_desc == NULL || !enum_desc->is_enum) {
        return 0;
    }
    for (i = 0; i < enum_desc->enum_count; i++) {
        if (enum_desc->enum_values[i].number == number) {
            if (name != NULL) {
                *name = enum_desc->enum_values[i].name;
            }
            return 1;
        }
    }
    return 0;
}

int
pbw_enum_find_value(const pbw_descriptor *enum_desc, const char *name,
                    int32_t *number)
{
    size_t i;

    if (enum_desc == NULL || !enum_desc->is_enum || name == NULL) {
        return 0;
    }
    for (i = 0; i < enum_desc->enum_count; i++) {
        if (strcmp(enum_desc->enum_values[i].name, name) == 0) {
            if (number != NULL) {
                *number = enum_desc->enum_values[i].number;
            }
            return 1;
        }
    }
    return 0;
}

static pbw_field *
add_field(pbw_descriptor *msg, const char *name, uint32_t number, pbw_type type,
          pbw_label label, int is_extension)
{
    pbw_field *fields;
    pbw_field *field;
    size_t i;

    if (msg == NULL || msg->is_enum || name == NULL) {
        return NULL;
    }
    if (msg->schema != NULL && msg->schema->frozen) {
        /* Growing field_count after a message exists would put that message's
         * value slots out of sync with the descriptor, so the layout freezes at
         * the first successful link. */
        snprintf(msg->schema->error, sizeof(msg->schema->error),
                 "add field %s.%s before pbw_schema_link()", msg->name, name);
        return NULL;
    }
    if (number == 0 || number > PBW_MAX_FIELD_NUMBER) {
        return NULL;
    }
    for (i = 0; i < msg->field_count; i++) {
        /* Extensions from different scopes may share a name; only the number
         * identifies one. Regular fields keep the name check too. */
        if (msg->fields[i].number == number ||
            (!is_extension && strcmp(msg->fields[i].name, name) == 0)) {
            return NULL;
        }
    }
    fields = (pbw_field *)realloc(msg->fields,
                                  (msg->field_count + 1) * sizeof(*fields));
    if (fields == NULL) {
        return NULL;
    }
    msg->fields = fields;
    field = &fields[msg->field_count];
    memset(field, 0, sizeof(*field));
    field->name = dup_str(name);
    if (field->name == NULL) {
        return NULL;
    }
    field->schema = msg->schema;
    field->number = number;
    field->type = type;
    field->label = label;
    field->is_extension = is_extension ? 1u : 0u;
    msg->field_count++;
    msg->schema->linked = 0;
    return field;
}

pbw_field *
pbw_message_add_field(pbw_descriptor *msg, const char *name,
                      uint32_t number, pbw_type type,
                      pbw_label label)
{
    return add_field(msg, name, number, type, label, 0);
}

pbw_field *
pbw_message_add_extension(pbw_descriptor *msg, const char *name,
                          uint32_t number, pbw_type type, pbw_label label)
{
    return add_field(msg, name, number, type, label, 1);
}

void
pbw_field_set_type_name(pbw_field *field, const char *type_name)
{
    char *copy;

    if (field == NULL) {
        return;
    }
    copy = dup_str(type_name);
    if (type_name != NULL && copy == NULL) {
        return;
    }
    free(field->type_name);
    field->type_name = copy;
    field->sub = NULL;
    if (field->schema != NULL) {
        field->schema->linked = 0;
    }
}

void
pbw_descriptor_set_map_entry(pbw_descriptor *msg)
{
    if (msg == NULL || msg->is_enum) {
        return;
    }
    msg->map_entry = 1;
    if (msg->schema != NULL) {
        msg->schema->linked = 0;
    }
}

void
pbw_field_set_full_name(pbw_field *field, const char *full_name)
{
    char *copy;

    if (field == NULL) {
        return;
    }
    copy = dup_str(full_name);
    if (full_name != NULL && copy == NULL) {
        return;
    }
    free(field->full_name);
    field->full_name = copy;
    if (field->schema != NULL) {
        field->schema->linked = 0;
    }
}

void
pbw_field_set_packed(pbw_field *field, int packed)
{
    if (field != NULL) {
        field->packed = packed ? 1u : 0u;
        if (field->schema != NULL) {
            field->schema->linked = 0;
        }
    }
}

void
pbw_field_set_oneof(pbw_field *field, uint32_t oneof_index)
{
    if (field != NULL) {
        field->oneof_index = oneof_index;
        if (field->schema != NULL) {
            field->schema->linked = 0;
        }
    }
}

static int
compare_fields(const void *a, const void *b)
{
    const pbw_field *fa = (const pbw_field *)a;
    const pbw_field *fb = (const pbw_field *)b;

    if (fa->number < fb->number) {
        return -1;
    }
    return fa->number > fb->number ? 1 : 0;
}

/* Precomputes the wire bytes of a field's tag, so neither the encoder nor the
 * decoder has to format or compare one again. */
static void
link_field_tags(pbw_field *field)
{
    field->wire_type = (uint8_t)pbw_expected_wire_type(field->type);
    field->tag_len =
        (uint8_t)pbw_varint_write(field->tag, ((uint64_t)field->number << 3) |
                                                  field->wire_type);
    field->len_tag_len = (uint8_t)pbw_varint_write(
        field->len_tag, ((uint64_t)field->number << 3) | PBW_WIRE_LEN);
    field->tag_word = pbw_read_le32(field->tag);
    field->len_tag_word = pbw_read_le32(field->len_tag);
}

static pbw_status
link_message(pbw_schema *schema, pbw_descriptor *msg)
{
    size_t i;
    uint32_t max_fnum = 0;

    for (i = 0; i < msg->field_count; i++) {
        pbw_field *field = &msg->fields[i];

        if (field->type == PBW_TYPE_MESSAGE || field->type == PBW_TYPE_ENUM ||
            field->type == PBW_TYPE_GROUP) {
            pbw_descriptor *sub;

            if (field->type_name == NULL) {
                snprintf(schema->error, sizeof(schema->error),
                         "field %s.%s needs a type name", msg->name, field->name);
                return PBW_ERR_INVALID;
            }
            sub = pbw_schema_find(schema, field->type_name);
            if (sub == NULL) {
                snprintf(schema->error, sizeof(schema->error),
                         "unknown type %s for field %s.%s", field->type_name, msg->name,
                         field->name);
                return PBW_ERR_NOT_FOUND;
            }
            if (sub->is_enum != (field->type == PBW_TYPE_ENUM)) {
                snprintf(schema->error, sizeof(schema->error),
                         "type %s does not match field %s.%s", field->type_name,
                         msg->name, field->name);
                return PBW_ERR_TYPE;
            }
            field->sub = sub;
        }
        if (field->packed &&
            (field->label != PBW_LABEL_REPEATED || !pbw_is_packable(field->type))) {
            snprintf(schema->error, sizeof(schema->error),
                     "field %s.%s cannot be packed", msg->name, field->name);
            return PBW_ERR_INVALID;
        }
        link_field_tags(field);
        if (!msg->proto3 && field->label == PBW_LABEL_REQUIRED) {
            schema->has_required = 1;
        }
        if (field->oneof_index > msg->oneof_count) {
            msg->oneof_count = field->oneof_index;
        }
        if (field->number > max_fnum) {
            max_fnum = field->number;
        }
    }

    msg->has_closed_enum = 0;
    for (i = 0; i < msg->field_count; i++) {
        const pbw_field *field = &msg->fields[i];

        if (field->type == PBW_TYPE_ENUM && field->sub != NULL &&
            !field->sub->proto3) {
            msg->has_closed_enum = 1;
            break;
        }
    }

    qsort(msg->fields, msg->field_count, sizeof(*msg->fields), compare_fields);
    for (i = 1; i < msg->field_count; i++) {
        if (msg->fields[i - 1].number == msg->fields[i].number) {
            snprintf(schema->error, sizeof(schema->error),
                     "duplicate field number %u in %s", msg->fields[i].number,
                     msg->name);
            return PBW_ERR_INVALID;
        }
    }

    /* Dense field-number lookup table for the decoder; sparse messages use
     * bsearch. */
    free(msg->fnum_map);
    msg->fnum_map = NULL;
    msg->fnum_map_size = 0;
    if (max_fnum <= PBW_FNUM_MAP_LIMIT &&
        max_fnum <= (uint32_t)(msg->field_count * 8 + 8)) {
        pbw_field **map =
            (pbw_field **)calloc(max_fnum + 1, sizeof(*map));
        if (map == NULL) {
            return PBW_ERR_NOMEM;
        }
        for (i = 0; i < msg->field_count; i++) {
            map[msg->fields[i].number] = &msg->fields[i];
        }
        msg->fnum_map = map;
        msg->fnum_map_size = max_fnum + 1;
    }
    /* One-byte tag dispatch for the decoder hot loop: a tag byte below 0x80
     * indexes this table directly, skipping the field-number lookup. A tag
     * byte is unique per field (number and wire type are both fixed), so no
     * entry is ever shared. */
    free(msg->tag_map);
    msg->tag_map = NULL;
    free(msg->tag_action);
    msg->tag_action = NULL;
    for (i = 0; i < msg->field_count; i++) {
        if (msg->fields[i].tag_len == 1) {
            break;
        }
    }
    if (i < msg->field_count) {
        pbw_field **map = (pbw_field **)calloc(128, sizeof(*map));
        uint8_t *act;
        if (map == NULL) {
            return PBW_ERR_NOMEM;
        }
        act = (uint8_t *)calloc(128, sizeof(*act));
        if (act == NULL) {
            free(map);
            return PBW_ERR_NOMEM;
        }
        for (i = 0; i < msg->field_count; i++) {
            pbw_field *field = &msg->fields[i];
            int closed = field->type == PBW_TYPE_ENUM && field->sub != NULL &&
                         !field->sub->proto3;
            uint8_t action = PBW_ACT_GENERIC;
            /* Oneof members, groups and closed enums keep the generic logic:
             * only shapes with no side branches get a fast action. */
            if (field->oneof_index == 0 && field->type != PBW_TYPE_GROUP &&
                !closed) {
                if (field->label != PBW_LABEL_REPEATED) {
                    switch (field->type) {
                    case PBW_TYPE_INT32:
                    case PBW_TYPE_ENUM:
                        action = PBW_ACT_I32;
                        break;
                    case PBW_TYPE_INT64:
                    case PBW_TYPE_UINT64:
                        action = PBW_ACT_I64;
                        break;
                    case PBW_TYPE_UINT32:
                        action = PBW_ACT_U32;
                        break;
                    case PBW_TYPE_SINT32:
                        action = PBW_ACT_S32;
                        break;
                    case PBW_TYPE_SINT64:
                        action = PBW_ACT_S64;
                        break;
                    case PBW_TYPE_BOOL:
                        action = PBW_ACT_BOOL;
                        break;
                    case PBW_TYPE_FIXED32:
                    case PBW_TYPE_SFIXED32:
                    case PBW_TYPE_FLOAT:
                        action = PBW_ACT_F32;
                        break;
                    case PBW_TYPE_FIXED64:
                    case PBW_TYPE_SFIXED64:
                    case PBW_TYPE_DOUBLE:
                        action = PBW_ACT_F64;
                        break;
                    case PBW_TYPE_STRING:
                    case PBW_TYPE_BYTES:
                        action = PBW_ACT_STR;
                        break;
                    case PBW_TYPE_MESSAGE:
                        action = PBW_ACT_MSG;
                        break;
                    default:
                        break;
                    }
                } else if (pbw_is_packable(field->type)) {
                    /* The packed LEN form gets its own entry below; the
                     * unpacked form stays generic. */
                    action = PBW_ACT_GENERIC;
                }
            }
            if (field->tag_len == 1) {
                map[field->tag[0]] = field;
                act[field->tag[0]] = action;
            }
            /* A repeated packable field also accepts the packed (LEN)
             * form; its tag byte can never belong to another field. */
            if (field->len_tag_len == 1 &&
                field->label == PBW_LABEL_REPEATED &&
                pbw_is_packable(field->type) && !closed &&
                field->oneof_index == 0) {
                map[field->len_tag[0]] = field;
                act[field->len_tag[0]] = PBW_ACT_PACKED;
            }
        }
        msg->tag_map = map;
        msg->tag_action = act;
    }
    return PBW_OK;
}

pbw_status
pbw_schema_link(pbw_schema *schema)
{
    size_t i;
    pbw_status status;

    if (schema == NULL) {
        return PBW_ERR_INVALID;
    }
    if (schema->linked) {
        return PBW_OK;
    }
    schema->error[0] = '\0';
    for (i = 0; i < schema->count; i++) {
        if (schema->descs[i]->is_enum) {
            continue;
        }
        status = link_message(schema, schema->descs[i]);
        if (status != PBW_OK) {
            return status;
        }
    }
    schema->linked = 1;
    schema->frozen = 1;
    return PBW_OK;
}

const char *
pbw_schema_error(const pbw_schema *schema)
{
    return schema == NULL ? "" : schema->error;
}

const char *
pbw_descriptor_name(const pbw_descriptor *desc)
{
    return desc == NULL ? NULL : desc->name;
}

int
pbw_descriptor_is_enum(const pbw_descriptor *desc)
{
    return desc != NULL && desc->is_enum;
}

int
pbw_descriptor_is_proto3(const pbw_descriptor *desc)
{
    return desc != NULL && desc->proto3;
}

size_t
pbw_descriptor_field_count(const pbw_descriptor *desc)
{
    return desc == NULL ? 0 : desc->field_count;
}

const pbw_field *
pbw_descriptor_field_at(const pbw_descriptor *desc,
                        size_t index)
{
    if (desc == NULL || index >= desc->field_count) {
        return NULL;
    }
    return &desc->fields[index];
}

const pbw_field *
pbw_descriptor_find_field(const pbw_descriptor *desc, uint32_t number)
{
    if (desc == NULL || desc->is_enum ||
        (desc->schema != NULL && !desc->schema->linked)) {
        return NULL;
    }
    return pbw_find_field(desc, number);
}

const pbw_field *
pbw_descriptor_find_field_name(const pbw_descriptor *desc,
                               const char *name)
{
    size_t i;

    if (desc == NULL || desc->is_enum || name == NULL) {
        return NULL;
    }
    for (i = 0; i < desc->field_count; i++) {
        if (strcmp(desc->fields[i].name, name) == 0) {
            return &desc->fields[i];
        }
    }
    return NULL;
}

const char *
pbw_field_name(const pbw_field *field)
{
    return field == NULL ? NULL : field->name;
}

const char *
pbw_field_default_value(const pbw_field *field)
{
    return field == NULL ? NULL : field->default_value;
}

int
pbw_field_is_extension(const pbw_field *field)
{
    return field != NULL && field->is_extension != 0;
}

uint32_t
pbw_field_number(const pbw_field *field)
{
    return field == NULL ? 0 : field->number;
}

pbw_type
pbw_field_type(const pbw_field *field)
{
    return field == NULL ? PBW_TYPE_INT32 : field->type;
}

pbw_label
pbw_field_label(const pbw_field *field)
{
    return field == NULL ? PBW_LABEL_OPTIONAL : field->label;
}

int
pbw_field_is_packed(const pbw_field *field)
{
    return field != NULL && field->packed != 0;
}

uint32_t
pbw_field_oneof_index(const pbw_field *field)
{
    return field == NULL ? 0 : field->oneof_index;
}

const pbw_descriptor *
pbw_field_message_type(const pbw_field *field)
{
    return field == NULL ? NULL : field->sub;
}

int
pbw_descriptor_is_reserved(const pbw_descriptor *desc, const char *name)
{
    size_t i;

    if (desc == NULL || name == NULL) {
        return 0;
    }
    for (i = 0; i < desc->reserved_count; i++) {
        if (strcmp(desc->reserved[i], name) == 0) {
            return 1;
        }
    }
    return 0;
}

const pbw_field *
pbw_schema_find_extension(const pbw_schema *schema, const char *name)
{
    size_t d;
    size_t i;

    if (schema == NULL || name == NULL) {
        return NULL;
    }
    for (d = 0; d < schema->count; d++) {
        const pbw_descriptor *desc = schema->descs[d];

        if (desc->is_enum) {
            continue;
        }
        for (i = 0; i < desc->field_count; i++) {
            const pbw_field *field = &desc->fields[i];

            if (!field->is_extension) {
                continue;
            }
            if (field->full_name != NULL) {
                if (strcmp(field->full_name, name) == 0) {
                    return field;
                }
            } else if (strcmp(field->name, name) == 0) {
                return field;
            }
        }
    }
    return NULL;
}

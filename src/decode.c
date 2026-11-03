/* Wire decoder: Protobuf binary -> dynamic message tree. Unknown fields are
 * skipped. */

#include "internal.h"

typedef struct {
    const uint8_t *p;
    const uint8_t *end;
} reader;

static uint64_t
zigzag_decode(uint64_t value, unsigned width)
{
    uint64_t mask = width == 32 ? 0xffffffffull : ~0ull;
    uint64_t u = value & mask;

    return ((u >> 1) ^ (0ull - (u & 1))) & mask;
}

/* Reads a varint. One- and two-byte values (most tags and small numbers) are
 * handled here; longer ones go through the general reader. */
PBW_INLINE int
read_varint(reader *r, uint64_t *out)
{
    size_t used;

    if (r->end - r->p >= 2) {
        uint32_t first = r->p[0];

        if (first < 0x80) {
            *out = first;
            r->p += 1;
            return 1;
        }
        if (r->p[1] < 0x80) {
            *out = (uint64_t)(first & 0x7f) | ((uint64_t)r->p[1] << 7);
            r->p += 2;
            return 1;
        }
    } else if (r->p < r->end && r->p[0] < 0x80) {
        *out = r->p[0];
        r->p += 1;
        return 1;
    }
    if (!pbw_varint_read(r->p, r->end, out, &used)) {
        return 0;
    }
    r->p += used;
    return 1;
}

static pbw_status skip_group(reader *r, uint32_t number, uint32_t depth, uint32_t dlim);
static pbw_status skip_field(reader *r, pbw_message *msg, uint32_t number,
                             uint32_t wire_type, uint32_t depth, uint32_t dlim,
                             const uint8_t *start);

static pbw_status
skip_value(reader *r, uint32_t number, uint32_t wire_type,
           uint32_t depth, uint32_t dlim)
{
    uint64_t scratch;
    size_t len;
    size_t used;

    switch (wire_type) {
    case PBW_WIRE_VARINT:
        return read_varint(r, &scratch) ? PBW_OK : PBW_ERR_INVALID;
    case PBW_WIRE_64BIT:
        if ((size_t)(r->end - r->p) < 8) {
            return PBW_ERR_INVALID;
        }
        r->p += 8;
        return PBW_OK;
    case PBW_WIRE_32BIT:
        if ((size_t)(r->end - r->p) < 4) {
            return PBW_ERR_INVALID;
        }
        r->p += 4;
        return PBW_OK;
    case PBW_WIRE_LEN:
        if (!pbw_len_read(r->p, r->end, &len, &used)) {
            return PBW_ERR_INVALID;
        }
        r->p += used;
        if (len > (size_t)(r->end - r->p)) {
            return PBW_ERR_INVALID;
        }
        r->p += len;
        return PBW_OK;
    case PBW_WIRE_START_GROUP:
        return skip_group(r, number, depth, dlim);
    default:
        return PBW_ERR_INVALID;
    }
}

/* Skips a legacy group, matching its END_GROUP tag. Group skipping is
 * recursive, so nesting is bounded by PBW_MAX_DEPTH. */
static pbw_status
skip_group(reader *r, uint32_t number, uint32_t depth, uint32_t dlim)
{
    if (depth >= dlim) {
        return PBW_ERR_LIMIT;
    }
    for (;;) {
        uint32_t tag;
        size_t used;
        uint32_t wire_type;

        if (r->p >= r->end) {
            return PBW_ERR_INVALID;
        }
        if (!pbw_tag_read(r->p, r->end, &tag, &used)) {
            return PBW_ERR_INVALID;
        }
        r->p += used;
        wire_type = tag & 7;
        /* Field number 0 is invalid, even inside a skipped group: the reference
         * parser validates every tag it walks past. */
        if ((tag >> 3) == 0) {
            return PBW_ERR_INVALID;
        }
        if (wire_type == PBW_WIRE_END_GROUP) {
            return (tag >> 3) == number ? PBW_OK : PBW_ERR_INVALID;
        }
        {
            pbw_status status = skip_value(r, tag >> 3, wire_type, depth + 1, dlim);
            if (status != PBW_OK) {
                return status;
            }
        }
    }
}

/* Skips one field and keeps its bytes. `start` points at the tag. */
static pbw_status
skip_field(reader *r, pbw_message *msg, uint32_t number, uint32_t wire_type,
           uint32_t depth, uint32_t dlim, const uint8_t *start)
{
    pbw_status status = skip_value(r, number, wire_type, depth, dlim);

    if (status != PBW_OK) {
        return status;
    }
    return (pbw_status)pbw_message_add_unknown(msg, start,
                                               (size_t)(r->p - start));
}

/* Reads one scalar of the given type. Strings/bytes/messages are handled by the
 * caller. */
static pbw_status
read_scalar(reader *r, pbw_type type, uint64_t *out)
{
    uint64_t raw;

    switch (type) {
    case PBW_TYPE_INT32:
    case PBW_TYPE_ENUM:
        if (!read_varint(r, &raw)) {
            return PBW_ERR_INVALID;
        }
        *out = (uint64_t)(int64_t)(int32_t)raw;
        return PBW_OK;
    case PBW_TYPE_INT64:
    case PBW_TYPE_UINT64:
        return read_varint(r, out) ? PBW_OK : PBW_ERR_INVALID;
    case PBW_TYPE_UINT32:
        if (!read_varint(r, &raw)) {
            return PBW_ERR_INVALID;
        }
        *out = (uint32_t)raw;
        return PBW_OK;
    case PBW_TYPE_SINT32:
        if (!read_varint(r, &raw)) {
            return PBW_ERR_INVALID;
        }
        *out = (uint64_t)(int64_t)(int32_t)zigzag_decode(raw, 32);
        return PBW_OK;
    case PBW_TYPE_SINT64:
        if (!read_varint(r, &raw)) {
            return PBW_ERR_INVALID;
        }
        *out = zigzag_decode(raw, 64);
        return PBW_OK;
    case PBW_TYPE_BOOL:
        if (!read_varint(r, &raw)) {
            return PBW_ERR_INVALID;
        }
        *out = raw != 0 ? 1u : 0u;
        return PBW_OK;
    case PBW_TYPE_FIXED32:
    case PBW_TYPE_SFIXED32:
    case PBW_TYPE_FLOAT:
        if ((size_t)(r->end - r->p) < 4) {
            return PBW_ERR_INVALID;
        }
        *out = (uint64_t)pbw_read_le32(r->p);
        r->p += 4;
        return PBW_OK;
    case PBW_TYPE_FIXED64:
    case PBW_TYPE_SFIXED64:
    case PBW_TYPE_DOUBLE:
        if ((size_t)(r->end - r->p) < 8) {
            return PBW_ERR_INVALID;
        }
        *out = pbw_read_le64(r->p);
        r->p += 8;
        return PBW_OK;
    default:
        return PBW_ERR_INVALID;
    }
}

static pbw_status
read_length(reader *r, const uint8_t **data, size_t *len)
{
    size_t value;
    size_t used;

    if (!pbw_len_read(r->p, r->end, &value, &used)) {
        return PBW_ERR_INVALID;
    }
    r->p += used;
    if (value > (size_t)(r->end - r->p)) {
        return PBW_ERR_INVALID;
    }
    *data = r->p;
    *len = value;
    r->p += value;
    return PBW_OK;
}

/* Writes a scalar straight into its field slot: the common case of a singular,
 * non-oneof field seen for the first time. */
static inline void
store_scalar(pbw_message *msg, size_t index, uint64_t bits)
{
    pbw_value *value = &msg->values[index];

    value->kind = PBW_VALUE_UINT;
    value->flags = PBW_VF_EMBEDDED |
                   (msg->arena != NULL ? PBW_VF_ARENA : 0u);
    value->as.u64 = bits;
    pbw_msg_set_present(msg, index, 1);
}

/* Singular scalar fast path: first occurrence writes the slot directly,
 * later ones overwrite through the generic store. */
static inline void
store_first_or_put(pbw_message *msg, size_t index, uint64_t bits)
{
    if (!pbw_msg_present(msg, index)) {
        store_scalar(msg, index, bits);
        return;
    }
    {
        pbw_value value;

        value.kind = PBW_VALUE_UINT;
        value.flags = msg->arena != NULL ? PBW_VF_ARENA : 0;
        value.as.u64 = bits;
        pbw_message_put(msg, index, &value);
    }
}

/* Singular string/bytes first occurrence: payload straight into the slot. */
static int
store_bytes_first(pbw_message *msg, size_t index, const uint8_t *data,
                  size_t len)
{
    pbw_value *slot = &msg->values[index];
    uint8_t *copy;

    if (msg->arena != NULL) {
        copy = (uint8_t *)pbw_arena_alloc(msg->arena, len + 1);
    } else {
        copy = (uint8_t *)malloc(len + 1);
    }
    if (copy == NULL) {
        return 0;
    }
    if (len > 0) {
        memcpy(copy, data, len);
    }
    copy[len] = '\0';
    slot->kind = PBW_VALUE_BYTES;
    slot->flags = PBW_VF_EMBEDDED |
                  (msg->arena != NULL ? PBW_VF_ARENA : 0u);
    slot->as.bytes.data = copy;
    slot->as.bytes.len = len;
    pbw_msg_set_present(msg, index, 1);
    return 1;
}

/* Appends one value to a repeated field. Returns PBW_OK or PBW_ERR_NOMEM. */
static pbw_status
append_item(pbw_message *msg, const pbw_field *field, pbw_value *item)
{
    pbw_value *list;

    if (item == NULL) {
        return PBW_ERR_NOMEM;
    }
    list = pbw_message_list_for(msg, field);
    if (list == NULL || !pbw_value_list_push(list, item, msg->arena)) {
        pbw_value_free(item);
        return PBW_ERR_NOMEM;
    }
    return PBW_OK;
}

static pbw_status decode_fields(pbw_message *msg, const uint8_t *buf,
                                size_t len, uint32_t depth, uint32_t dlim);
static pbw_status decode_message(pbw_arena *arena, const pbw_descriptor *desc,
                                 const uint8_t *buf, size_t len,
                                 pbw_message **out, uint32_t depth, uint32_t dlim);
static pbw_status read_field(reader *r, const pbw_field *field,
                             pbw_message *msg, uint32_t depth, uint32_t dlim,
                             const uint8_t *start, int closed);
static pbw_status read_packed(reader *r, const pbw_field *field,
                              pbw_message *msg, int closed);

/* Reads the body of a group: fields of `group` until the END_GROUP tag that
 * matches `number`. Same field rules as decode_fields, only the terminator
 * differs. */
static pbw_status
decode_group_contents(reader *r, pbw_message *group, uint32_t number,
                      uint32_t depth, uint32_t dlim)
{
    const pbw_descriptor *desc = group->desc;

    if (depth >= dlim) {
        return PBW_ERR_LIMIT;
    }
    for (;;) {
        uint32_t tag;
        size_t used = 1;
        uint32_t wire_type;
        uint32_t fnum;
        const pbw_field *field;
        const uint8_t *start = r->p;
        pbw_status status;

        if (r->p >= r->end) {
            return PBW_ERR_INVALID; /* END_GROUP never came */
        }
        if (r->p[0] < 0x80) {
            tag = r->p[0];
            r->p++;
        } else if (pbw_tag_read(r->p, r->end, &tag, &used)) {
            r->p += used;
        } else {
            return PBW_ERR_INVALID;
        }
        wire_type = tag & 7;
        fnum = tag >> 3;
        if (fnum == 0 || wire_type > PBW_WIRE_32BIT) {
            return PBW_ERR_INVALID;
        }
        if (wire_type == PBW_WIRE_END_GROUP) {
            return fnum == number ? PBW_OK : PBW_ERR_INVALID;
        }
        if (desc->fnum_map != NULL) {
            field = fnum < desc->fnum_map_size ? desc->fnum_map[fnum] : NULL;
        } else {
            field = pbw_find_field(desc, fnum);
        }
        if (field == NULL) {
            status = skip_field(r, group, fnum, wire_type, depth, dlim, start);
        } else if (wire_type == field->wire_type) {
            status = read_field(r, field, group, depth, dlim, start,
                                desc->has_closed_enum);
        } else if (wire_type == PBW_WIRE_LEN &&
                   field->label == PBW_LABEL_REPEATED &&
                   pbw_is_packable(field->type)) {
            status = read_packed(r, field, group, desc->has_closed_enum);
        } else {
            status = skip_field(r, group, fnum, wire_type, depth, dlim, start);
        }
        if (status != PBW_OK) {
            return status;
        }
    }
}

/* A group field: START_GROUP was already consumed, the body runs to the
 * matching END_GROUP. Singular groups merge like singular messages. */
static pbw_status
read_group_field(reader *r, const pbw_field *field, pbw_message *msg,
                 uint32_t depth, uint32_t dlim)
{
    size_t index = (size_t)(field - msg->desc->fields);
    pbw_message *nested = NULL;
    pbw_status status;

    if (depth >= dlim) {
        return PBW_ERR_LIMIT;
    }
    if (field->label != PBW_LABEL_REPEATED &&
        (field->oneof_index == 0 ||
         msg->oneof_case[field->oneof_index] == field->number) &&
        pbw_msg_present(msg, index) &&
        msg->values[index].kind == PBW_VALUE_MESSAGE &&
        msg->values[index].as.msg != NULL &&
        msg->values[index].as.msg->desc == field->sub) {
        return decode_group_contents(r, msg->values[index].as.msg,
                                     field->number, depth + 1, dlim);
    }
    nested = pbw_message_new_a(msg->arena, field->sub);
    if (nested == NULL) {
        return PBW_ERR_NOMEM;
    }
    status = decode_group_contents(r, nested, field->number, depth + 1, dlim);
    if (status != PBW_OK) {
        pbw_message_free(nested);
        return status;
    }
    if (field->label == PBW_LABEL_REPEATED) {
        pbw_value *item = pbw_value_new_message_a(msg->arena, nested);

        if (item == NULL) {
            pbw_message_free(nested);
            return PBW_ERR_NOMEM;
        }
        return append_item(msg, field, item);
    }
    {
        pbw_value value;

        value.kind = PBW_VALUE_MESSAGE;
        value.flags = msg->arena != NULL ? PBW_VF_ARENA : 0;
        value.as.msg = nested;
        pbw_message_put(msg, index, &value);
    }
    return PBW_OK;
}

/* A repeated numeric field may arrive packed regardless of the descriptor
 * flag. */
static pbw_status
read_packed(reader *r, const pbw_field *field, pbw_message *msg, int closed)
{
    const uint8_t *data;
    size_t data_len;
    pbw_status status;
    reader sub;

    status = read_length(r, &data, &data_len);
    if (status != PBW_OK) {
        return status;
    }
    sub.p = data;
    sub.end = data + data_len;
    while (sub.p < sub.end) {
        const uint8_t *item = sub.p;
        uint64_t bits;

        status = read_scalar(&sub, field->type, &bits);
        if (status != PBW_OK) {
            return status;
        }
        if (closed && pbw_enum_is_closed(field) &&
            !pbw_enum_find_name(field->sub, (int32_t)bits, NULL)) {
            /* A closed enum's unknown value leaves the field: the reference
             * parsers keep it as an unpacked unknown field instead. */
            status = (pbw_status)pbw_message_add_unknown(msg, field->tag,
                                                         field->tag_len);
            if (status == PBW_OK) {
                status = (pbw_status)pbw_message_add_unknown(
                    msg, item, (size_t)(sub.p - item));
            }
            if (status != PBW_OK) {
                return status;
            }
            continue;
        }
        status = append_item(msg, field,
                             pbw_value_new_uint_a(msg->arena, bits));
        if (status != PBW_OK) {
            return status;
        }
    }
    return PBW_OK;
}

static pbw_status
read_string_field(reader *r, const pbw_field *field, pbw_message *msg)
{
    const uint8_t *data;
    size_t data_len;
    pbw_status status;

    status = read_length(r, &data, &data_len);
    if (status != PBW_OK) {
        return status;
    }
    if (field->label == PBW_LABEL_REPEATED) {
        return append_item(
            msg, field, pbw_value_new_bytes_a(msg->arena, data, data_len));
    }
    {
        pbw_value value;

        if (!pbw_value_init_bytes(&value, msg->arena, data, data_len)) {
            return PBW_ERR_NOMEM;
        }
        pbw_message_put(msg, (size_t)(field - msg->desc->fields), &value);
    }
    return PBW_OK;
}

static pbw_status
read_message_field(reader *r, const pbw_field *field, pbw_message *msg,
                   uint32_t depth, uint32_t dlim)
{
    const uint8_t *data;
    size_t data_len;
    size_t index = (size_t)(field - msg->desc->fields);
    pbw_message *nested = NULL;
    pbw_status status;

    if (depth >= dlim) {
        return PBW_ERR_LIMIT;
    }
    status = read_length(r, &data, &data_len);
    if (status != PBW_OK) {
        return status;
    }
    /* A singular message field that appears more than once is merged into the
     * existing submessage, not replaced (the wire format spec requires it;
     * reference implementations do the same). */
    if (field->label != PBW_LABEL_REPEATED &&
        (field->oneof_index == 0 ||
         msg->oneof_case[field->oneof_index] == field->number) &&
        pbw_msg_present(msg, index) &&
        msg->values[index].kind == PBW_VALUE_MESSAGE &&
        msg->values[index].as.msg != NULL &&
        msg->values[index].as.msg->desc == field->sub) {
        return decode_fields(msg->values[index].as.msg, data, data_len,
                             depth + 1, dlim);
    }
    status = decode_message(msg->arena, field->sub, data, data_len, &nested,
                            depth + 1, dlim);
    if (status != PBW_OK) {
        return status;
    }
    if (field->label == PBW_LABEL_REPEATED) {
        pbw_value *item = pbw_value_new_message_a(msg->arena, nested);

        if (item == NULL) {
            pbw_message_free(nested);
            return PBW_ERR_NOMEM;
        }
        return append_item(msg, field, item);
    }
    {
        pbw_value value;

        value.kind = PBW_VALUE_MESSAGE;
        value.flags = msg->arena != NULL ? PBW_VF_ARENA : 0;
        value.as.msg = nested;
        pbw_message_put(msg, index, &value);
    }
    return PBW_OK;
}

/* Handles one field the schema knows about, wire type already matching.
 * `start` points at the tag, so a value that turns out to belong to the unknown
 * fields can be kept byte for byte. */
static pbw_status
read_field(reader *r, const pbw_field *field, pbw_message *msg,
           uint32_t depth, uint32_t dlim, const uint8_t *start, int closed)
{
    size_t index = (size_t)(field - msg->desc->fields);
    pbw_status status;
    uint64_t bits;
    pbw_value value;

    if (field->type == PBW_TYPE_STRING || field->type == PBW_TYPE_BYTES) {
        return read_string_field(r, field, msg);
    }
    if (field->type == PBW_TYPE_MESSAGE) {
        return read_message_field(r, field, msg, depth, dlim);
    }
    if (field->type == PBW_TYPE_GROUP) {
        return read_group_field(r, field, msg, depth, dlim);
    }

    status = read_scalar(r, field->type, &bits);
    if (status != PBW_OK) {
        return status;
    }
    if (closed && pbw_enum_is_closed(field) &&
        !pbw_enum_find_name(field->sub, (int32_t)bits, NULL)) {
        /* A closed enum's unknown value leaves the field: the reference parsers
         * keep it as an unknown field instead. */
        return (pbw_status)pbw_message_add_unknown(msg, start,
                                                   (size_t)(r->p - start));
    }
    if (field->label != PBW_LABEL_REPEATED) {
        if (field->oneof_index == 0 && !pbw_msg_present(msg, index)) {
            /* Singular, first occurrence: write the slot directly. */
            store_scalar(msg, index, bits);
            return PBW_OK;
        }
        value.kind = PBW_VALUE_UINT;
        value.flags = msg->arena != NULL ? PBW_VF_ARENA : 0;
        value.as.u64 = bits;
        pbw_message_put(msg, index, &value);
        return PBW_OK;
    }
    return append_item(msg, field, pbw_value_new_uint_a(msg->arena, bits));
}

static pbw_status
decode_fields(pbw_message *msg, const uint8_t *buf, size_t len, uint32_t depth, uint32_t dlim)
{
    reader r;
    const pbw_descriptor *desc = msg->desc;
    pbw_field *const *fnum_map = desc->fnum_map;
    uint32_t fnum_map_size = desc->fnum_map_size;
    pbw_field *const *tag_map = desc->tag_map;
    const uint8_t *tag_action = desc->tag_action;
    int closed = desc->has_closed_enum;

    r.p = buf;
    r.end = buf + len;
    while (r.p < r.end) {
        uint32_t tag;
        size_t used = 1;
        uint32_t wire_type;
        uint32_t number;
        const pbw_field *field;
        const uint8_t *start = r.p;
        pbw_status status;
        uint8_t action = PBW_ACT_GENERIC;

        /* One-byte tag (field number < 16): the common case, dispatched by
         * the link-time tag table with a single indexed load. */
        if (r.p < r.end && r.p[0] < 0x80) {
            tag = r.p[0];
            r.p++;
            wire_type = tag & 7;
            number = tag >> 3;
            if (number == 0 || wire_type == PBW_WIRE_END_GROUP ||
                wire_type > PBW_WIRE_32BIT) {
                return PBW_ERR_INVALID;
            }
            /* No one-byte tag can be known when the table is missing: a
             * field numbered below 16 always encodes to one byte. */
            field = tag_map != NULL ? tag_map[tag] : NULL;
            action = tag_action != NULL ? tag_action[tag] : PBW_ACT_GENERIC;
            if (action != PBW_ACT_GENERIC) {
                size_t index = (size_t)(field - desc->fields);
                uint64_t bits;
                switch (action) {
                case PBW_ACT_I32:
                    if (!read_varint(&r, &bits)) {
                        return PBW_ERR_INVALID;
                    }
                    store_first_or_put(msg, index,
                                       (uint64_t)(int64_t)(int32_t)bits);
                    continue;
                case PBW_ACT_I64:
                    if (!read_varint(&r, &bits)) {
                        return PBW_ERR_INVALID;
                    }
                    store_first_or_put(msg, index, bits);
                    continue;
                case PBW_ACT_U32:
                    if (!read_varint(&r, &bits)) {
                        return PBW_ERR_INVALID;
                    }
                    store_first_or_put(msg, index, (uint64_t)(uint32_t)bits);
                    continue;
                case PBW_ACT_S32:
                    if (!read_varint(&r, &bits)) {
                        return PBW_ERR_INVALID;
                    }
                    bits = (uint64_t)(int64_t)(int32_t)zigzag_decode(bits, 32);
                    store_first_or_put(msg, index, bits);
                    continue;
                case PBW_ACT_S64:
                    if (!read_varint(&r, &bits)) {
                        return PBW_ERR_INVALID;
                    }
                    store_first_or_put(msg, index, zigzag_decode(bits, 64));
                    continue;
                case PBW_ACT_BOOL:
                    if (!read_varint(&r, &bits)) {
                        return PBW_ERR_INVALID;
                    }
                    store_first_or_put(msg, index, bits != 0 ? 1u : 0u);
                    continue;
                case PBW_ACT_F32:
                    if ((size_t)(r.end - r.p) < 4) {
                        return PBW_ERR_INVALID;
                    }
                    store_first_or_put(msg, index, pbw_read_le32(r.p));
                    r.p += 4;
                    continue;
                case PBW_ACT_F64:
                    if ((size_t)(r.end - r.p) < 8) {
                        return PBW_ERR_INVALID;
                    }
                    store_first_or_put(msg, index, pbw_read_le64(r.p));
                    r.p += 8;
                    continue;
                case PBW_ACT_STR: {
                    const uint8_t *data;
                    size_t data_len;
                    status = read_length(&r, &data, &data_len);
                    if (status != PBW_OK) {
                        return status;
                    }
                    if (!pbw_msg_present(msg, index)) {
                        if (!store_bytes_first(msg, index, data, data_len)) {
                            return PBW_ERR_NOMEM;
                        }
                    } else {
                        pbw_value value;
                        if (!pbw_value_init_bytes(&value, msg->arena, data,
                                                  data_len)) {
                            return PBW_ERR_NOMEM;
                        }
                        pbw_message_put(msg, index, &value);
                    }
                    continue;
                }
                case PBW_ACT_MSG: {
                    const uint8_t *data;
                    size_t data_len;
                    pbw_message *nested = NULL;
                    if (depth >= dlim) {
                        return PBW_ERR_LIMIT;
                    }
                    status = read_length(&r, &data, &data_len);
                    if (status != PBW_OK) {
                        return status;
                    }
                    if (pbw_msg_present(msg, index) &&
                        msg->values[index].kind == PBW_VALUE_MESSAGE &&
                        msg->values[index].as.msg != NULL &&
                        msg->values[index].as.msg->desc == field->sub) {
                        return decode_fields(msg->values[index].as.msg, data,
                                             data_len, depth + 1, dlim);
                    }
                    status = decode_message(msg->arena, field->sub, data,
                                            data_len, &nested, depth + 1, dlim);
                    if (status != PBW_OK) {
                        return status;
                    }
                    if (pbw_msg_present(msg, index)) {
                        pbw_value_destroy(&msg->values[index]);
                    }
                    msg->values[index].kind = PBW_VALUE_MESSAGE;
                    msg->values[index].flags = PBW_VF_EMBEDDED |
                                               (msg->arena != NULL ? PBW_VF_ARENA : 0u);
                    msg->values[index].as.msg = nested;
                    pbw_msg_set_present(msg, index, 1);
                    continue;
                }
                case PBW_ACT_PACKED:
                    status = read_packed(&r, field, msg, 0);
                    if (status != PBW_OK) {
                        return status;
                    }
                    continue;
                default:
                    break;
                }
            }
        } else if (pbw_tag_read(r.p, r.end, &tag, &used)) {
            r.p += used;
            wire_type = tag & 7;
            number = tag >> 3;
            if (number == 0 || wire_type == PBW_WIRE_END_GROUP ||
                wire_type > PBW_WIRE_32BIT) {
                return PBW_ERR_INVALID;
            }
            if (fnum_map != NULL) {
                field = number < fnum_map_size ? fnum_map[number] : NULL;
            } else {
                field = pbw_find_field(desc, number);
            }
        } else {
            return PBW_ERR_INVALID;
        }
        if (field == NULL) {
            status = skip_field(&r, msg, number, wire_type, depth, dlim, start);
        } else if (wire_type == field->wire_type) {
            status = read_field(&r, field, msg, depth, dlim, start, closed);
        } else if (wire_type == PBW_WIRE_LEN &&
                   field->label == PBW_LABEL_REPEATED &&
                   pbw_is_packable(field->type)) {
            /* A repeated numeric field may arrive packed regardless of the
             * descriptor flag. */
            status = read_packed(&r, field, msg, closed);
        } else {
            /* Wire type mismatch: treat as an unknown field, like the reference
             * decoders. */
            status = skip_field(&r, msg, number, wire_type, depth, dlim, start);
        }
        if (status != PBW_OK) {
            return status;
        }
    }
    return PBW_OK;
}

static pbw_status
decode_message(pbw_arena *arena, const pbw_descriptor *desc, const uint8_t *buf,
               size_t len, pbw_message **out, uint32_t depth, uint32_t dlim)
{
    pbw_message *msg = pbw_message_new_a(arena, desc);
    pbw_status status;

    if (msg == NULL) {
        return PBW_ERR_NOMEM;
    }
    status = decode_fields(msg, buf, len, depth, dlim);
    if (status != PBW_OK) {
        pbw_message_free(msg);
        return status;
    }
    *out = msg;
    return PBW_OK;
}

pbw_status
pbw_decode_arena_lim(pbw_arena *arena, const pbw_descriptor *desc,
                     const void *buf, size_t len, uint32_t dlim,
                     pbw_message **out)
{
    pbw_status status;

    if (desc == NULL || out == NULL || (buf == NULL && len > 0)) {
        return PBW_ERR_INVALID;
    }
    if (desc->is_enum) {
        return PBW_ERR_TYPE;
    }
    if (desc->schema != NULL && !desc->schema->linked) {
        return PBW_ERR_INVALID;
    }
    status = decode_message(arena, desc, (const uint8_t *)buf, len, out, 0,
                            dlim);
    /* The reference parsers check for missing `required` fields once the whole
     * message is in, not per nested occurrence, so this is the same point. The
     * schema-wide flag keeps the walk off every pure-proto3 decode. */
    if (status == PBW_OK && desc->schema != NULL &&
        desc->schema->has_required && !pbw_message_is_initialized(*out)) {
        pbw_message_free(*out);
        *out = NULL;
        return PBW_ERR_REQUIRED;
    }
    return status;
}

pbw_status
pbw_decode_arena(pbw_arena *arena, const pbw_descriptor *desc, const void *buf,
                 size_t len, pbw_message **out)
{
    return pbw_decode_arena_lim(arena, desc, buf, len, PBW_MAX_DEPTH, out);
}

/* Plain decoding owns one arena per message tree: the whole decode is a single
 * malloc instead of one per field, and pbw_message_free() releases it at once.
 * Wire length is a fair proxy for the tree size, so the first chunk is sized
 * from it and a small message rarely needs a second one. */
pbw_status
pbw_decode_limits(const pbw_descriptor *desc, const void *buf, size_t len,
                  const pbw_limits *limits, pbw_message **out)
{
    pbw_arena *arena;
    pbw_status status;

    if (desc == NULL || out == NULL || (buf == NULL && len > 0)) {
        return PBW_ERR_INVALID;
    }
    /* Every failure below leaves NULL: callers can rely on *out alone. */
    *out = NULL;
    if (limits != NULL && limits->max_bytes > 0 && len > limits->max_bytes) {
        return PBW_ERR_LIMIT;
    }
    arena = pbw_arena_new_packed(len <= 4096 ? len * 6 + 1024
                                             : len + len / 2 + 512);
    if (arena == NULL) {
        return PBW_ERR_NOMEM;
    }
    status = pbw_decode_arena_lim(arena, desc, buf, len,
                                  pbw_effective_depth(limits), out);
    if (status != PBW_OK) {
        pbw_arena_free(arena);
        return status;
    }
    (*out)->own = arena;
    return PBW_OK;
}

pbw_status
pbw_decode(const pbw_descriptor *desc, const void *buf, size_t len,
           pbw_message **out)
{
    return pbw_decode_limits(desc, buf, len, NULL, out);
}

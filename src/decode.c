/* Wire decoder: Protobuf binary -> dynamic message tree. Unknown fields are
 * skipped. */

#include "internal.h"

typedef struct {
    const uint8_t *p;
    const uint8_t *end;
} reader;

PBW_INLINE uint64_t
zigzag_decode(uint64_t value, unsigned width)
{
    uint64_t mask = width == 32 ? 0xffffffffull : ~0ull;
    uint64_t u = value & mask;

    return ((u >> 1) ^ (0ull - (u & 1))) & mask;
}

/* Little-endian loads without a cross-translation-unit call: the fixed-width
 * fast paths run them once per field. */
PBW_INLINE uint32_t
dec_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

PBW_INLINE uint64_t
dec_le64(const uint8_t *p)
{
    return (uint64_t)dec_le32(p) | ((uint64_t)dec_le32(p + 4) << 32);
}

/* Reads a varint. One- and two-byte values (most tags and small numbers) are
 * handled here; longer ones go through the general reader. */
PBW_INLINE int
read_varint(reader *r, uint64_t *out)
{
    size_t used;

    if (r == NULL || r->p == NULL || r->end == NULL || out == NULL) {
        return 0;
    }
    if (pbw_remaining(r->end, r->p) >= 2) {
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
static pbw_status decode_message(pbw_arena *arena, const pbw_descriptor *desc,
                                 const uint8_t *buf, size_t len,
                                 pbw_message **out, uint32_t depth,
                                 uint32_t dlim);

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
        if (pbw_remaining(r->end, r->p) < 8) {
            return PBW_ERR_INVALID;
        }
        r->p += 8;
        return PBW_OK;
    case PBW_WIRE_32BIT:
        if (pbw_remaining(r->end, r->p) < 4) {
            return PBW_ERR_INVALID;
        }
        r->p += 4;
        return PBW_OK;
    case PBW_WIRE_LEN:
        if (!pbw_len_read(r->p, r->end, &len, &used)) {
            return PBW_ERR_INVALID;
        }
        r->p += used;
        if (len > pbw_remaining(r->end, r->p)) {
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
    if (start == NULL || r->p == NULL || r->p < start) {
        return PBW_ERR_INVALID;
    }
    return (pbw_status)pbw_message_add_unknown(msg, start,
                                               pbw_remaining(r->p, start));
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
        if (pbw_remaining(r->end, r->p) < 4) {
            return PBW_ERR_INVALID;
        }
        *out = (uint64_t)dec_le32(r->p);
        if (type == PBW_TYPE_SFIXED32) {
            /* Signed fixed values are represented sign extended, like int32. */
            *out = (uint64_t)(int64_t)(int32_t)*out;
        }
        r->p += 4;
        return PBW_OK;
    case PBW_TYPE_FIXED64:
    case PBW_TYPE_SFIXED64:
    case PBW_TYPE_DOUBLE:
        if (pbw_remaining(r->end, r->p) < 8) {
            return PBW_ERR_INVALID;
        }
        *out = dec_le64(r->p);
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

    /* One-byte length (strings shorter than 128 bytes, the common case): the
     * generic reader is a call into wire.c for what is one compare and add. */
    if (r->p < r->end && r->p[0] < 0x80) {
        size_t n = r->p[0];
        size_t avail = pbw_remaining(r->end, r->p);

        if (avail < 1 || n > avail - 1) {
            return PBW_ERR_INVALID;
        }
        *data = r->p + 1;
        *len = n;
        r->p += 1 + n;
        return PBW_OK;
    }
    if (!pbw_len_read(r->p, r->end, &value, &used)) {
        return PBW_ERR_INVALID;
    }
    r->p += used;
    if (value > pbw_remaining(r->end, r->p)) {
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

    if (len > SIZE_MAX - 1) {
        return 0;
    }
    if (len > 0 && data == NULL) {
        return 0;
    }
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

/* One decode loop's repeated-append cache. A repeated field's list lives in
 * its message slot and never moves, so once created it stays valid for the
 * whole decode_fields()/decode_group_contents() invocation: every later
 * item of the same field skips the get-or-create call. The kind check keeps
 * this safe even if a slot ever held something else. */
typedef struct {
    const pbw_field *field;
    pbw_value *list;
} rep_cache;

/* Appends one value to a repeated field. Returns PBW_OK or PBW_ERR_NOMEM. */
PBW_INLINE pbw_status
rep_append(pbw_message *msg, const pbw_field *field, pbw_value *item,
           rep_cache *rep)
{
    pbw_value *list;

    if (item == NULL) {
        return PBW_ERR_NOMEM;
    }
    if (rep->field == field && rep->list != NULL &&
        rep->list->kind == PBW_VALUE_LIST) {
        list = rep->list;
    } else {
        list = pbw_message_list_for(msg, field);
        if (list == NULL) {
            pbw_value_free(item);
            return PBW_ERR_NOMEM;
        }
        rep->field = field;
        rep->list = list;
    }
    if (!pbw_value_list_push(list, item, msg->arena)) {
        pbw_value_free(item);
        return PBW_ERR_NOMEM;
    }
    return PBW_OK;
}

/* Two-byte tag reader: field numbers 16..2047 encode to two bytes
 * (first byte has the high bit set, second does not). The field lookup and
 * the action switch below are shared with every other tag size. */
PBW_INLINE int
decode_tag2(const uint8_t *p, const uint8_t *end, uint32_t *tag)
{
    if (p == NULL || end == NULL || tag == NULL ||
        pbw_remaining(end, p) < 2 || p[0] < 0x80 || p[1] >= 0x80) {
        return 0;
    }
    *tag = (uint32_t)(p[0] & 0x7f) | ((uint32_t)p[1] << 7);
    return 1;
}

static pbw_status decode_fields(pbw_message *msg, const uint8_t *buf,
                                size_t len, uint32_t depth, uint32_t dlim);
static pbw_status decode_message(pbw_arena *arena, const pbw_descriptor *desc,
                                 const uint8_t *buf, size_t len,
                                 pbw_message **out, uint32_t depth, uint32_t dlim);
static pbw_status read_field(reader *r, const pbw_field *field,
                             pbw_message *msg, uint32_t depth, uint32_t dlim,
                             const uint8_t *start, int closed, rep_cache *rep);
static pbw_status read_packed(reader *r, const pbw_field *field,
                              pbw_message *msg, int closed, rep_cache *rep);

/* Reads the body of a group: fields of `group` until the END_GROUP tag that
 * matches `number`. Same field rules as decode_fields, only the terminator
 * differs. */
static pbw_status
decode_group_contents(reader *r, pbw_message *group, uint32_t number,
                      uint32_t depth, uint32_t dlim)
{
    const pbw_descriptor *desc = group->desc;
    rep_cache rep = {NULL, NULL};

    /* No depth check here: both callers (read_group_field, and its merge path)
     * already applied the same cap before recursing, and a second check would
     * cost groups one nesting level against messages and unknown groups. */
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
                                desc->has_closed_enum, &rep);
        } else if (wire_type == PBW_WIRE_LEN &&
                   field->label == PBW_LABEL_REPEATED &&
                   pbw_is_packable(field->type)) {
            status = read_packed(r, field, group, desc->has_closed_enum, &rep);
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
                 uint32_t depth, uint32_t dlim, rep_cache *rep)
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
        return rep_append(msg, field, item, rep);
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
read_packed(reader *r, const pbw_field *field, pbw_message *msg, int closed,
            rep_cache *rep)
{
    const uint8_t *data;
    size_t data_len;
    pbw_status status;
    reader sub;
    pbw_value *vlist;
    pbw_value **items;
    pbw_value *batch = NULL;
    uint32_t batched = 0;
    uint32_t len;
    uint32_t cap;
    /* A closed enum drops items whose number it does not know, so a batched
     * node block would hold nodes nothing uses: keep that case per-item. */
    int batchable;

    status = read_length(r, &data, &data_len);
    if (status != PBW_OK) {
        return status;
    }
    if (data == NULL) {
        return data_len == 0 ? PBW_OK : PBW_ERR_INVALID;
    }
    if (data_len == 0) {
        /* An empty packed field stays absent: no list is created for it. */
        return PBW_OK;
    }
    /* A closed enum drops unknown values, so the scan count would over-reserve
     * and the node block would hold nodes nothing uses: that case keeps the
     * per-item path (and no scan, no reserve). Measured neutral against the
     * scan+reserve form on a 100k-value run, so the smaller diff wins. */
    batchable = !(closed && pbw_enum_is_closed(field));
    /* Fixed-width items divide the length exactly, so the count is known
     * before the first item: reserve it and skip the push growth chain.
     * Varint runs count their items the same way: every value ends on a
     * byte without the continuation bit. Only worth a scan past 64 bytes;
     * shorter runs grow at most three times. */
    vlist = pbw_message_list_for(msg, field);
    if (vlist == NULL) {
        return PBW_ERR_NOMEM;
    }
    rep->field = field;
    rep->list = vlist;
    if (batchable && data_len > 64 && field->type != PBW_TYPE_FIXED32 &&
        field->type != PBW_TYPE_SFIXED32 && field->type != PBW_TYPE_FLOAT &&
        field->type != PBW_TYPE_FIXED64 &&
        field->type != PBW_TYPE_SFIXED64 && field->type != PBW_TYPE_DOUBLE) {
        size_t n = 0;
        size_t k;

        for (k = 0; k < data_len; k++) {
            n += (size_t)(data[k] < 0x80);
        }
        if (!pbw_value_list_reserve(
                vlist, n > UINT32_MAX ? UINT32_MAX : (uint32_t)n,
                msg->arena)) {
            return PBW_ERR_NOMEM;
        }
        /* The scan counted the items: one bump for every node of this run
         * instead of one allocation per item. */
        if (n > 0 && n <= UINT32_MAX &&
            n <= SIZE_MAX / sizeof(pbw_value)) {
            batch = (pbw_value *)pbw_arena_alloc(
                msg->arena, n * sizeof(pbw_value));
        }
    } else if (field->type == PBW_TYPE_FIXED32 ||
               field->type == PBW_TYPE_SFIXED32 ||
               field->type == PBW_TYPE_FLOAT ||
               field->type == PBW_TYPE_FIXED64 ||
               field->type == PBW_TYPE_SFIXED64 ||
               field->type == PBW_TYPE_DOUBLE) {
        size_t width = (field->type == PBW_TYPE_FIXED64 ||
                        field->type == PBW_TYPE_SFIXED64 ||
                        field->type == PBW_TYPE_DOUBLE)
                           ? 8u
                           : 4u;
        size_t count = data_len / width;

        if (!pbw_value_list_reserve(
                vlist, count > UINT32_MAX ? UINT32_MAX : (uint32_t)count,
                msg->arena)) {
            return PBW_ERR_NOMEM;
        }
        /* Fixed-width items need no scan: the payload length divides exactly,
         * so the node block comes from one bump like a varint run's. A
         * truncated tail fails in read_scalar() before it can index past the
         * block: each successful read consumes `width` bytes. */
        if (batchable && count > 0 && count <= UINT32_MAX &&
            count <= SIZE_MAX / sizeof(pbw_value)) {
            batch = (pbw_value *)pbw_arena_alloc(
                msg->arena, count * sizeof(pbw_value));
        }
    }
    items = vlist->as.list.items;
    len = vlist->as.list.len;
    cap = vlist->as.list.cap;
    sub.p = data;
    sub.end = data + data_len;
    {
        int closed_enum = !batchable; /* loop-invariant */
        while (sub.p < sub.end) {
            const uint8_t *item = sub.p;
            uint64_t bits;
            pbw_value *node;

            status = read_scalar(&sub, field->type, &bits);
            if (status != PBW_OK) {
                return status;
            }
            if (closed_enum &&
                !pbw_enum_find_name(field->sub, (int32_t)bits, NULL)) {
                /* A closed enum's unknown value leaves the field: the reference
                 * parsers keep it as an unpacked unknown field instead. The
                 * list locals stay untouched: only the unknown bytes grow. */
                status = (pbw_status)pbw_message_add_unknown(msg, field->tag,
                                                             field->tag_len);
                if (status == PBW_OK) {
                    if (item == NULL || sub.p == NULL || sub.p < item) {
                        return PBW_ERR_INVALID;
                    }
                    status = (pbw_status)pbw_message_add_unknown(
                        msg, item, pbw_remaining(sub.p, item));
                }
                if (status != PBW_OK) {
                    return status;
                }
                continue;
            }
            if (batch != NULL) {
                node = &batch[batched++];
                node->kind = PBW_VALUE_UINT;
                node->flags = PBW_VF_ARENA;
                node->as.u64 = bits;
            } else {
                node = pbw_value_new_uint_a(msg->arena, bits);
                if (node == NULL) {
                    return PBW_ERR_NOMEM;
                }
            }
            if (len == cap) {
                /* Same doubling as pbw_value_list_push, through the shared
                 * reserve: a push-equivalent growth without a call per item. */
                uint32_t ncap = cap != 0 ? cap * 2u : 8u;
                vlist->as.list.items = items;
                vlist->as.list.len = len;
                if (ncap < cap ||
                    !pbw_value_list_reserve(vlist, ncap, msg->arena)) {
                    pbw_value_free(node);
                    return PBW_ERR_NOMEM;
                }
                items = vlist->as.list.items;
                cap = vlist->as.list.cap;
            }
            items[len++] = node;
        }
    }
    vlist->as.list.items = items;
    vlist->as.list.len = len;
    if (len == 0) {
        /* A closed-enum run whose values were all unknown stored nothing: the
         * field stays absent, like an empty packed run, and the cached slot is
         * dropped so the next occurrence creates a fresh list. */
        pbw_msg_set_present(msg, (size_t)(field - msg->desc->fields), 0);
        rep->field = NULL;
        rep->list = NULL;
    }
    return PBW_OK;
}

/* A `string` field whose utf8_validation resolves to VERIFY must carry valid
 * UTF-8: the reference parsers reject such a message instead of keeping bytes
 * that later render as replacement characters. proto2 strings and `bytes`
 * fields are unchecked, exactly like the reference. */
PBW_INLINE pbw_status
check_utf8(const pbw_field *field, const uint8_t *data, size_t len)
{
    if (field->utf8_check && !pbw_utf8_ok(data, len)) {
        return PBW_ERR_INVALID;
    }
    return PBW_OK;
}

static pbw_status
read_string_field(reader *r, const pbw_field *field, pbw_message *msg,
                  rep_cache *rep)
{
    const uint8_t *data;
    size_t data_len;
    pbw_status status;

    status = read_length(r, &data, &data_len);
    if (status != PBW_OK) {
        return status;
    }
    status = check_utf8(field, data, data_len);
    if (status != PBW_OK) {
        return status;
    }
    if (field->label == PBW_LABEL_REPEATED) {
        return rep_append(
            msg, field, pbw_value_new_bytes_a(msg->arena, data, data_len),
            rep);
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
                   uint32_t depth, uint32_t dlim, rep_cache *rep)
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
        return rep_append(msg, field, item, rep);
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
           uint32_t depth, uint32_t dlim, const uint8_t *start, int closed,
           rep_cache *rep)
{
    size_t index = (size_t)(field - msg->desc->fields);
    pbw_status status;
    uint64_t bits;
    pbw_value value;

    if (field->type == PBW_TYPE_STRING || field->type == PBW_TYPE_BYTES) {
        return read_string_field(r, field, msg, rep);
    }
    if (field->type == PBW_TYPE_MESSAGE) {
        /* Editions message_encoding = DELIMITED: the body runs to the
         * matching END_GROUP, so it decodes like a group. Only fields
         * that opted in take this path; anything else carrying group
         * tags stays an unknown field, which keeps MessageSet passthrough
         * byte-identical (see descriptor_set.c). */
        if (field->msg_enc == 2) {
            return read_group_field(r, field, msg, depth, dlim, rep);
        }
        return read_message_field(r, field, msg, depth, dlim, rep);
    }
    if (field->type == PBW_TYPE_GROUP) {
        return read_group_field(r, field, msg, depth, dlim, rep);
    }

    status = read_scalar(r, field->type, &bits);
    if (status != PBW_OK) {
        return status;
    }
    if (closed && pbw_enum_is_closed(field) &&
        !pbw_enum_find_name(field->sub, (int32_t)bits, NULL)) {
        /* A closed enum's unknown value leaves the field: the reference parsers
         * keep it as an unknown field instead. */
        if (start == NULL || r->p == NULL || r->p < start) {
            return PBW_ERR_INVALID;
        }
        return (pbw_status)pbw_message_add_unknown(msg, start,
                                                   pbw_remaining(r->p,
                                                                 start));
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
    return rep_append(msg, field, pbw_value_new_uint_a(msg->arena, bits),
                      rep);
}

/* A MessageSet item: START_GROUP(1) was consumed; the body holds type_id
 * (field 2, varint) and message (field 3, bytes). The walk runs on a copy
 * of the reader: anything anomalous (no id, no payload, unknown id, a
 * non-message extension, undecodable bytes) falls back to unknown
 * preservation from `start`, so verdicts never change — only successfully
 * parsed items populate their extension field, like the reference parsers. */
static pbw_status
read_messageset_item(reader *r, pbw_message *msg, uint32_t depth,
                     uint32_t dlim, const uint8_t *start)
{
    const pbw_descriptor *desc = msg->desc;
    reader t = *r;
    uint32_t type_id = 0;
    int has_id = 0;
    const uint8_t *payload = NULL;
    size_t payload_len = 0;
    int has_pay = 0;
    const pbw_field *ext;
    pbw_message *nested = NULL;
    pbw_status status;
    size_t index;
    uint32_t closer = 0;

    for (;;) {
        uint32_t tag;
        size_t used;
        uint32_t number;
        uint32_t wire_type;

        if (t.p >= t.end || !pbw_tag_read(t.p, t.end, &tag, &used)) {
            return skip_field(r, msg, 1, PBW_WIRE_START_GROUP, depth, dlim,
                              start);
        }
        t.p += used;
        number = tag >> 3;
        wire_type = tag & 7;
        if (number == 0) {
            return skip_field(r, msg, 1, PBW_WIRE_START_GROUP, depth, dlim,
                              start);
        }
        if (wire_type == PBW_WIRE_END_GROUP) {
            closer = number;
            break;
        }
        if (number == 2 && wire_type == PBW_WIRE_VARINT) {
            uint64_t id;
            size_t vused;

            if (!pbw_varint_read(t.p, t.end, &id, &vused)) {
                return skip_field(r, msg, 1, PBW_WIRE_START_GROUP, depth,
                                  dlim, start);
            }
            t.p += vused;
            /* Last one wins, like every singular field. */
            if (id >= 1 && id <= PBW_MAX_FIELD_NUMBER) {
                type_id = (uint32_t)id;
                has_id = 1;
            }
            continue;
        }
        if (number == 3 && wire_type == PBW_WIRE_LEN) {
            size_t len;

            if (!pbw_len_read(t.p, t.end, &len, &used)) {
                return skip_field(r, msg, 1, PBW_WIRE_START_GROUP, depth,
                                  dlim, start);
            }
            t.p += used;
            if (len > pbw_remaining(t.end, t.p)) {
                return skip_field(r, msg, 1, PBW_WIRE_START_GROUP, depth,
                                  dlim, start);
            }
            payload = t.p;
            payload_len = len;
            has_pay = 1;
            t.p += len;
            continue;
        }
        if (depth + 1 >= dlim && wire_type == PBW_WIRE_START_GROUP) {
            return skip_field(r, msg, 1, PBW_WIRE_START_GROUP, depth, dlim,
                              start);
        }
        status = skip_value(&t, number, wire_type, depth + 1, dlim);
        if (status != PBW_OK) {
            return skip_field(r, msg, 1, PBW_WIRE_START_GROUP, depth, dlim,
                              start);
        }
    }
    /* The closer must be END_GROUP(1); t.p already passed it. */
    if (closer != 1 || !has_id || !has_pay) {
        return skip_field(r, msg, 1, PBW_WIRE_START_GROUP, depth, dlim,
                          start);
    }
    ext = pbw_find_field(desc, type_id);
    if (ext == NULL || !ext->is_extension ||
        ext->type != PBW_TYPE_MESSAGE ||
        ext->label == PBW_LABEL_REPEATED) {
        return skip_field(r, msg, 1, PBW_WIRE_START_GROUP, depth, dlim,
                          start);
    }
    if (depth >= dlim) {
        return PBW_ERR_LIMIT;
    }
    index = (size_t)(ext - desc->fields);
    if (pbw_msg_present(msg, index) &&
        msg->values[index].kind == PBW_VALUE_MESSAGE &&
        msg->values[index].as.msg != NULL &&
        msg->values[index].as.msg->desc == ext->sub) {
        /* Singular messages merge, MessageSet items included. */
        status = decode_fields(msg->values[index].as.msg, payload,
                               payload_len, depth + 1, dlim);
    } else {
        status = decode_message(msg->arena, ext->sub, payload, payload_len,
                                &nested, depth + 1, dlim);
        if (status == PBW_OK) {
            pbw_value value;

            value.kind = PBW_VALUE_MESSAGE;
            value.flags = msg->arena != NULL ? PBW_VF_ARENA : 0;
            value.as.msg = nested;
            pbw_message_put(msg, index, &value);
        }
    }
    if (status != PBW_OK) {
        /* Undecodable payload: keep today's verdict (accept as unknown). */
        pbw_message_free(nested);
        return skip_field(r, msg, 1, PBW_WIRE_START_GROUP, depth, dlim,
                          start);
    }
    *r = t;
    return PBW_OK;
}

static pbw_status
decode_fields(pbw_message *msg, const uint8_t *buf, size_t len, uint32_t depth, uint32_t dlim)
{
    reader r;
    const pbw_descriptor *desc;
    pbw_field *const *fnum_map;
    uint32_t fnum_map_size;
    pbw_field *const *tag_map;
    int closed;
    rep_cache rep = {NULL, NULL};

    if (msg == NULL || msg->desc == NULL || (buf == NULL && len > 0)) {
        return PBW_ERR_INVALID;
    }
    if (buf == NULL) {
        buf = pbw_empty_input;
    }
    if (dlim == 0 || dlim > PBW_MAX_DEPTH) {
        dlim = PBW_MAX_DEPTH;
    }
    desc = msg->desc;
    fnum_map = desc->fnum_map;
    fnum_map_size = desc->fnum_map_size;
    tag_map = desc->tag_map;
    closed = desc->has_closed_enum;

    r.p = buf;
    /* buf is non-NULL here; len may be 0, so end is at most one past. */
    r.end = buf + len;
    while (r.p < r.end) {
        uint32_t tag;
        size_t used = 1;
        uint32_t wire_type;
        uint32_t number;
        const pbw_field *field;
        const uint8_t *start = r.p;
        pbw_status status;

        /* Tag sizes share one lookup: a one-byte tag indexes the link-time
         * table, anything longer resolves through the field-number map. */

        /* One-byte tag (field number < 16): the common case, dispatched by
         * the link-time tag table with a single indexed load. */
        if (PBW_EXPECT(r.p < r.end && r.p[0] < 0x80, 1)) {
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
        } else if (decode_tag2(r.p, r.end, &tag)) {
            /* Two-byte tag (field number 16..2047): same lookup as any
             * other multibyte tag; the action switch below is shared. */
            r.p += 2;
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
            if (wire_type == PBW_WIRE_START_GROUP && number == 1 &&
                desc->is_messageset) {
                /* A MessageSet item decodes into its extension; anything
                 * anomalous stays unknown, as before. */
                status = read_messageset_item(&r, msg, depth, dlim, start);
            } else {
                status =
                    skip_field(&r, msg, number, wire_type, depth, dlim, start);
            }
        } else if (wire_type == field->wire_type) {
            /* One action switch for every tag size. The link-time action
             * already encodes eligibility (oneof members, groups and closed
             * enums stay generic), and a one-byte tag implies its wire type
             * by construction, so no shape checks remain here. */
            size_t index = (size_t)(field - desc->fields);
            uint64_t bits;
            switch (field->dec_action) {
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
                if (pbw_remaining(r.end, r.p) < 4) {
                    return PBW_ERR_INVALID;
                }
                bits = dec_le32(r.p);
                if (field->type == PBW_TYPE_SFIXED32) {
                    bits = (uint64_t)(int64_t)(int32_t)bits;
                }
                store_first_or_put(msg, index, bits);
                r.p += 4;
                continue;
            case PBW_ACT_F64:
                if (pbw_remaining(r.end, r.p) < 8) {
                    return PBW_ERR_INVALID;
                }
                bits = dec_le64(r.p);
                if (field->type == PBW_TYPE_SFIXED64) {
                    bits = (uint64_t)(int64_t)bits;
                }
                store_first_or_put(msg, index, bits);
                r.p += 8;
                continue;
            case PBW_ACT_STR: {
                const uint8_t *data;
                size_t data_len;
                status = read_length(&r, &data, &data_len);
                if (status != PBW_OK) {
                    return status;
                }
                status = check_utf8(field, data, data_len);
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
                    status = decode_fields(msg->values[index].as.msg, data,
                                           data_len, depth + 1, dlim);
                    if (status != PBW_OK) {
                        return status;
                    }
                    continue;
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
            case PBW_ACT_REP_VARINT:
                status = read_scalar(&r, field->type, &bits);
                if (status != PBW_OK) {
                    return status;
                }
                status = rep_append(msg, field,
                                    pbw_value_new_uint_a(msg->arena, bits),
                                    &rep);
                if (status != PBW_OK) {
                    return status;
                }
                continue;
            case PBW_ACT_REP_F32:
                if (pbw_remaining(r.end, r.p) < 4) {
                    return PBW_ERR_INVALID;
                }
                bits = dec_le32(r.p);
                if (field->type == PBW_TYPE_SFIXED32) {
                    /* Sign extended like the singular path: the API hands
                     * back every signed value in the same representation,
                     * list items included. */
                    bits = (uint64_t)(int64_t)(int32_t)bits;
                }
                r.p += 4;
                status = rep_append(msg, field,
                                    pbw_value_new_uint_a(msg->arena, bits),
                                    &rep);
                if (status != PBW_OK) {
                    return status;
                }
                continue;
            case PBW_ACT_REP_F64:
                if (pbw_remaining(r.end, r.p) < 8) {
                    return PBW_ERR_INVALID;
                }
                bits = dec_le64(r.p);
                if (field->type == PBW_TYPE_SFIXED64) {
                    bits = (uint64_t)(int64_t)bits;
                }
                r.p += 8;
                status = rep_append(msg, field,
                                    pbw_value_new_uint_a(msg->arena, bits),
                                    &rep);
                if (status != PBW_OK) {
                    return status;
                }
                continue;
            case PBW_ACT_REP_STR: {
                const uint8_t *data;
                size_t data_len;
                status = read_length(&r, &data, &data_len);
                if (status != PBW_OK) {
                    return status;
                }
                status = check_utf8(field, data, data_len);
                if (status != PBW_OK) {
                    return status;
                }
                status = rep_append(
                    msg, field,
                    pbw_value_new_bytes_a(msg->arena, data, data_len),
                    &rep);
                if (status != PBW_OK) {
                    return status;
                }
                continue;
            }
            case PBW_ACT_REP_MSG: {
                const uint8_t *data;
                size_t data_len;
                pbw_message *nested = NULL;
                pbw_value *item;
                if (depth >= dlim) {
                    return PBW_ERR_LIMIT;
                }
                status = read_length(&r, &data, &data_len);
                if (status != PBW_OK) {
                    return status;
                }
                status = decode_message(msg->arena, field->sub, data,
                                        data_len, &nested, depth + 1, dlim);
                if (status != PBW_OK) {
                    return status;
                }
                item = pbw_value_new_message_a(msg->arena, nested);
                if (item == NULL) {
                    pbw_message_free(nested);
                    return PBW_ERR_NOMEM;
                }
                status = rep_append(msg, field, item, &rep);
                if (status != PBW_OK) {
                    return status;
                }
                continue;
            }
            default:
                break;
            }
            status = read_field(&r, field, msg, depth, dlim, start, closed,
                                &rep);
        } else if (wire_type == PBW_WIRE_LEN &&
                   field->label == PBW_LABEL_REPEATED &&
                   pbw_is_packable(field->type)) {
            /* A repeated numeric field may arrive packed regardless of the
             * descriptor flag. */
            status = read_packed(&r, field, msg, closed, &rep);
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
    pbw_message *msg;
    pbw_status status;

    if (arena == NULL || desc == NULL || out == NULL ||
        (buf == NULL && len > 0)) {
        return PBW_ERR_INVALID;
    }
    if (buf == NULL) {
        buf = pbw_empty_input;
    }
    *out = NULL;
    msg = pbw_message_new_a(arena, desc);
    if (msg == NULL) {
        return PBW_ERR_NOMEM;
    }
    status = decode_fields(msg, buf, len, depth, dlim);
    if (status != PBW_OK) {
        pbw_message_free(msg);
        return status;
    }
    if (depth == 0) {
        /* Root only: nested messages recurse with depth + 1. The encode
         * pre-size reads exactly this, so nested trees need no copy. */
        msg->wire_len = len;
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
    const uint8_t *bytes;

    if (out != NULL) {
        *out = NULL;
    }
    if (arena == NULL || desc == NULL || out == NULL ||
        (buf == NULL && len > 0)) {
        return PBW_ERR_INVALID;
    }
    if (dlim == 0 || dlim > PBW_MAX_DEPTH) {
        dlim = PBW_MAX_DEPTH;
    }
    if (desc->is_enum) {
        return PBW_ERR_TYPE;
    }
    if (desc->schema != NULL && !desc->schema->linked) {
        return PBW_ERR_INVALID;
    }
    bytes = pbw_normalize_input(buf, len);
    status = decode_message(arena, desc, bytes, len, out, 0, dlim);
    /* The reference parsers check for missing `required` fields once the whole
     * message is in, not per nested occurrence, so this is the same point. The
     * per-descriptor flag keeps the walk off subtrees without `required`. */
    if (status == PBW_OK && desc->has_required &&
        !pbw_message_is_initialized(*out)) {
        pbw_message_free(*out);
        *out = NULL;
        return PBW_ERR_REQUIRED;
    }
    return status;
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
    const uint8_t *bytes;

    if (out != NULL) {
        *out = NULL;
    }
    if (desc == NULL || out == NULL || (buf == NULL && len > 0)) {
        return PBW_ERR_INVALID;
    }
    /* Every failure below leaves NULL: callers can rely on *out alone. */
    if (limits != NULL && limits->max_bytes > 0 && len > limits->max_bytes) {
        return PBW_ERR_LIMIT;
    }
    {
        /* First-chunk hint from the wire length. Saturate instead of wrapping
         * for a length so large that the size arithmetic would overflow. */
        size_t hint = len;

        if (len <= 4096) {
            hint = len * 6 + 1024;
        } else if (len <= SIZE_MAX - len / 2 - 512) {
            hint = len + len / 2 + 512;
        }
        arena = pbw_arena_new_packed(hint);
    }
    if (arena == NULL) {
        return PBW_ERR_NOMEM;
    }
    bytes = pbw_normalize_input(buf, len);
    status = pbw_decode_arena_lim(arena, desc, bytes, len,
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

/* Event scan: one validating pass over a single message level, no tree, no
 * allocation. Reuses the tag dispatch and field lookup of decode_fields, but
 * the per-field work is only a wire-type switch plus one callback: values
 * stay raw (no zigzag/sign fold, no enum/required checks, no packed split),
 * so a counting/filtering pass never pays for materialization. */
#define VISIT_FAIL(off, st)     \
    do {                        \
        if (fail_off != NULL) { \
            *fail_off = (off);  \
        }                       \
        return (st);            \
    } while (0)

pbw_status
pbw_visit_limits(const pbw_descriptor *desc, const void *buf, size_t len,
                 const pbw_limits *limits, pbw_visit_fn cb, void *ud,
                 size_t *fail_off)
{
    reader r;
    const uint8_t *base;
    pbw_field *const *fnum_map;
    uint32_t fnum_map_size;
    pbw_field *const *tag_map;
    uint32_t dlim;

    if (desc == NULL || (buf == NULL && len > 0) || cb == NULL) {
        VISIT_FAIL(0, PBW_ERR_INVALID);
    }
    if (limits != NULL && limits->max_bytes > 0 && len > limits->max_bytes) {
        VISIT_FAIL(0, PBW_ERR_LIMIT);
    }
    if (desc->is_enum) {
        VISIT_FAIL(0, PBW_ERR_TYPE);
    }
    if (desc->schema != NULL && !desc->schema->linked) {
        VISIT_FAIL(0, PBW_ERR_INVALID);
    }
    base = pbw_normalize_input(buf, len);
    dlim = pbw_effective_depth(limits);
    fnum_map = desc->fnum_map;
    fnum_map_size = desc->fnum_map_size;
    tag_map = desc->tag_map;
    r.p = base;
    r.end = base + len;
    while (r.p < r.end) {
        uint32_t tag;
        size_t used = 1;
        uint32_t wire_type;
        uint32_t number;
        const pbw_field *field;
        const uint8_t *start = r.p;
        size_t at = (size_t)(start - base);
        pbw_visit_value value;
        pbw_status status;

        if (PBW_EXPECT(r.p < r.end && r.p[0] < 0x80, 1)) {
            tag = r.p[0];
            r.p++;
            wire_type = tag & 7;
            number = tag >> 3;
            if (number == 0 || wire_type == PBW_WIRE_END_GROUP ||
                wire_type > PBW_WIRE_32BIT) {
                VISIT_FAIL(at, PBW_ERR_INVALID);
            }
            field = tag_map != NULL ? tag_map[tag] : NULL;
            if (field == NULL) {
                /* Unknown or mismatched one-byte tag: fall back to the
                 * number so a known field carrying another wire type
                 * still resolves. Cold path only: the table hit above
                 * already handled the common case. */
                if (fnum_map != NULL) {
                    field =
                        number < fnum_map_size ? fnum_map[number] : NULL;
                } else {
                    field = pbw_find_field(desc, number);
                }
            }
        } else if (decode_tag2(r.p, r.end, &tag)) {
            r.p += 2;
            wire_type = tag & 7;
            number = tag >> 3;
            if (number == 0 || wire_type == PBW_WIRE_END_GROUP ||
                wire_type > PBW_WIRE_32BIT) {
                VISIT_FAIL(at, PBW_ERR_INVALID);
            }
            if (fnum_map != NULL) {
                field = number < fnum_map_size ? fnum_map[number] : NULL;
            } else {
                field = pbw_find_field(desc, number);
            }
        } else if (pbw_tag_read(r.p, r.end, &tag, &used)) {
            r.p += used;
            wire_type = tag & 7;
            number = tag >> 3;
            if (number == 0 || wire_type == PBW_WIRE_END_GROUP ||
                wire_type > PBW_WIRE_32BIT) {
                VISIT_FAIL(at, PBW_ERR_INVALID);
            }
            if (fnum_map != NULL) {
                field = number < fnum_map_size ? fnum_map[number] : NULL;
            } else {
                field = pbw_find_field(desc, number);
            }
        } else {
            VISIT_FAIL(at, PBW_ERR_INVALID);
        }
        value.u64 = 0;
        value.data = NULL;
        value.len = 0;
        switch (wire_type) {
        case PBW_WIRE_VARINT:
            if (!read_varint(&r, &value.u64)) {
                VISIT_FAIL(at, PBW_ERR_INVALID);
            }
            break;
        case PBW_WIRE_64BIT:
            if (pbw_remaining(r.end, r.p) < 8) {
                VISIT_FAIL(at, PBW_ERR_INVALID);
            }
            value.u64 = dec_le64(r.p);
            r.p += 8;
            break;
        case PBW_WIRE_32BIT:
            if (pbw_remaining(r.end, r.p) < 4) {
                VISIT_FAIL(at, PBW_ERR_INVALID);
            }
            value.u64 = dec_le32(r.p);
            r.p += 4;
            break;
        case PBW_WIRE_LEN: {
            const uint8_t *data;
            size_t data_len;

            status = read_length(&r, &data, &data_len);
            if (status != PBW_OK) {
                VISIT_FAIL(at, PBW_ERR_INVALID);
            }
            value.data = data;
            value.len = data_len;
            break;
        }
        default: /* START_GROUP: deliver the whole field as raw bytes. */
            status = skip_value(&r, number, wire_type, 0, dlim);
            if (status != PBW_OK) {
                VISIT_FAIL(at, status);
            }
            value.data = start;
            value.len = pbw_remaining(r.p, start);
            break;
        }
        status = cb(ud, field, number, wire_type, &value);
        if (status != PBW_OK) {
            VISIT_FAIL(at, status);
        }
    }
    if (fail_off != NULL) {
        *fail_off = len;
    }
    return PBW_OK;
}

#undef VISIT_FAIL

pbw_status
pbw_visit(const pbw_descriptor *desc, const void *buf, size_t len,
          pbw_visit_fn cb, void *ud, size_t *fail_off)
{
    return pbw_visit_limits(desc, buf, len, NULL, cb, ud, fail_off);
}

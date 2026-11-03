/* Wire encoder: dynamic message tree -> Protobuf binary.
 *
 * Single pass. The output buffer grows on demand, tags are copied from the
 * precomputed bytes in the descriptor, string and bytes lengths are written
 * before their payload, and nested messages use a one-byte length placeholder
 * that is patched afterwards (the payload is shifted right when the length
 * needs more than one byte). No sizing pass, no temporary buffer per nested
 * message, no allocation per field.
 */

#include "internal.h"

typedef struct {
    pbw_buf *buf;
    size_t pos;
    pbw_status status;
} writer;

int
pbw_is_packable(pbw_type type)
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

uint32_t
pbw_expected_wire_type(pbw_type type)
{
    switch (type) {
    case PBW_TYPE_DOUBLE:
    case PBW_TYPE_FIXED64:
    case PBW_TYPE_SFIXED64:
        return PBW_WIRE_64BIT;
    case PBW_TYPE_FLOAT:
    case PBW_TYPE_FIXED32:
    case PBW_TYPE_SFIXED32:
        return PBW_WIRE_32BIT;
    case PBW_TYPE_STRING:
    case PBW_TYPE_BYTES:
    case PBW_TYPE_MESSAGE:
        return PBW_WIRE_LEN;
    case PBW_TYPE_GROUP:
        return PBW_WIRE_START_GROUP;
    default:
        return PBW_WIRE_VARINT;
    }
}

static uint64_t
zigzag_encode(uint64_t value, unsigned width)
{
    uint64_t mask = width == 32 ? 0xffffffffull : ~0ull;
    uint64_t sign = width == 32 ? 31 : 63;
    uint64_t u = value & mask;

    return ((u << 1) ^ (0ull - (u >> sign))) & mask;
}

/* Value as it goes on the wire, before tag and length. */
static uint64_t
wire_value(pbw_type type, uint64_t u)
{
    switch (type) {
    case PBW_TYPE_INT32:
    case PBW_TYPE_ENUM:
        return (uint64_t)(int64_t)(int32_t)u;
    case PBW_TYPE_UINT32:
        return (uint32_t)u;
    case PBW_TYPE_SINT32:
        return zigzag_encode(u, 32);
    case PBW_TYPE_SINT64:
        return zigzag_encode(u, 64);
    case PBW_TYPE_BOOL:
        return u != 0 ? 1u : 0u;
    default:
        return u;
    }
}

/* ---------------------------------------------------------------- writing */

/* Rare path of w_reserve(): the buffer has to grow. */
static int
w_grow(writer *w, size_t extra)
{
    w->buf->len = w->pos;
    if (!pbw_buf_reserve(w->buf, extra)) {
        w->status = PBW_ERR_NOMEM;
        return 0;
    }
    return 1;
}

PBW_INLINE int
w_reserve(writer *w, size_t extra)
{
    if (w->buf->cap - w->pos >= extra) {
        return 1;
    }
    return w_grow(w, extra);
}

PBW_INLINE void
w_put(writer *w, const void *data, size_t len)
{
    if (len == 0) {
        return;
    }
    if (!w_reserve(w, len)) {
        return;
    }
    memcpy(w->buf->data + w->pos, data, len);
    w->pos += len;
}

PBW_INLINE void
w_varint(writer *w, uint64_t value)
{
    uint8_t *out;

    /* Reserve the maximum width so the value is written in place: encoding only
     * appends, so the slack past w->pos is never read. */
    if (!w_reserve(w, 10)) {
        return;
    }
    out = w->buf->data + w->pos;
    if (value < 0x80) {
        out[0] = (uint8_t)value;
        w->pos += 1;
        return;
    }
    if (value < 0x4000) {
        out[0] = (uint8_t)((value & 0x7f) | 0x80);
        out[1] = (uint8_t)(value >> 7);
        w->pos += 2;
        return;
    }
    {
        size_t len = 0;

        while (value >= 0x80) {
            out[len++] = (uint8_t)((value & 0x7f) | 0x80);
            value >>= 7;
        }
        out[len++] = (uint8_t)value;
        w->pos += len;
    }
}

/* Copies a precomputed tag. Tag lengths are one or two bytes for almost every
 * schema, so avoid a memcpy call for those. */
/* Writes a precomputed tag. Tags are at most five bytes and the buffer is
 * reserved for five, so the four-byte word can be stored unconditionally and
 * the slack is overwritten by the next write. */
PBW_INLINE void
w_tag(writer *w, uint32_t word, size_t len)
{
    if (!w_reserve(w, 5)) {
        return;
    }
    /* memcpy of a fixed size compiles to one store and does not assume the
     * output offset is aligned. */
    memcpy(w->buf->data + w->pos, &word, sizeof(word));
    w->pos += len;
}

/* Field-number >= 2^21 (five tag bytes) is rare enough to take the slow path. */
static void
w_tag_bytes(writer *w, const uint8_t *tag, size_t len)
{
    if (w_reserve(w, len)) {
        memcpy(w->buf->data + w->pos, tag, len);
        w->pos += len;
    }
}

PBW_INLINE void
w_field_tag(writer *w, const pbw_field *field, int packed)
{
    if ((packed ? field->len_tag_len : field->tag_len) <= 4) {
        w_tag(w, packed ? field->len_tag_word : field->tag_word,
              packed ? field->len_tag_len : field->tag_len);
        return;
    }
    w_tag_bytes(w, packed ? field->len_tag : field->tag,
                packed ? field->len_tag_len : field->tag_len);
}

/* A length-delimited payload needs its length before its content, but the
 * length is only known afterwards: write a one-byte placeholder, encode, then
 * patch it, shifting the payload right when the length varint is longer. */
static void
w_len_begin(writer *w, size_t *mark)
{
    *mark = w->pos;
    if (!w_reserve(w, 1)) {
        return;
    }
    w->buf->data[w->pos] = 0;
    w->pos += 1;
}

static void
w_len_end(writer *w, size_t mark)
{
    size_t payload = w->pos - mark - 1;
    size_t len_bytes = pbw_varint_size(payload);
    size_t shift = len_bytes - 1;
    uint8_t *base;

    if (shift > 0) {
        if (!w_reserve(w, shift)) {
            return;
        }
        base = w->buf->data;
        memmove(base + mark + 1 + shift, base + mark + 1, payload);
        w->pos += shift;
    }
    {
        uint8_t *out = w->buf->data + mark;

        while (payload >= 0x80) {
            *out++ = (uint8_t)((payload & 0x7f) | 0x80);
            payload >>= 7;
        }
        *out = (uint8_t)payload;
    }
}

PBW_INLINE void
w_fixed(writer *w, uint64_t value, size_t size)
{
    uint8_t *out;

    if (!w_reserve(w, size)) {
        return;
    }
    out = w->buf->data + w->pos;
    if (size == 4) {
        out[0] = (uint8_t)value;
        out[1] = (uint8_t)(value >> 8);
        out[2] = (uint8_t)(value >> 16);
        out[3] = (uint8_t)(value >> 24);
    } else {
        out[0] = (uint8_t)value;
        out[1] = (uint8_t)(value >> 8);
        out[2] = (uint8_t)(value >> 16);
        out[3] = (uint8_t)(value >> 24);
        out[4] = (uint8_t)(value >> 32);
        out[5] = (uint8_t)(value >> 40);
        out[6] = (uint8_t)(value >> 48);
        out[7] = (uint8_t)(value >> 56);
    }
    w->pos += size;
}

/* Writes one scalar (tag already written) for the packed path. */
PBW_INLINE void
w_scalar(writer *w, pbw_type type, uint64_t bits)
{
    switch (type) {
    case PBW_TYPE_FLOAT:
    case PBW_TYPE_FIXED32:
    case PBW_TYPE_SFIXED32:
        w_fixed(w, bits, 4);
        return;
    case PBW_TYPE_DOUBLE:
    case PBW_TYPE_FIXED64:
    case PBW_TYPE_SFIXED64:
        w_fixed(w, bits, 8);
        return;
    default:
        w_varint(w, wire_value(type, bits));
        return;
    }
}

static void encode_message(writer *w, const pbw_message *msg, uint32_t depth,
                           int det);

PBW_INLINE void
write_element(writer *w, const pbw_field *field, const pbw_value *value,
              uint32_t depth, int det)
{
    if (depth > PBW_MAX_DEPTH) {
        w->status = PBW_ERR_LIMIT;
        return;
    }
    switch (field->type) {
    case PBW_TYPE_STRING:
    case PBW_TYPE_BYTES:
        /* Length is known before the payload: write it directly. */
        w_field_tag(w, field, 0);
        w_varint(w, value->as.bytes.len);
        w_put(w, value->as.bytes.data, value->as.bytes.len);
        return;
    case PBW_TYPE_GROUP: {
        uint8_t end[5];
        size_t end_len;

        if (value->as.msg == NULL) {
            w->status = PBW_ERR_INVALID;
            return;
        }
        if (depth == PBW_MAX_DEPTH) {
            w->status = PBW_ERR_LIMIT;
            return;
        }
        /* Groups are rare: format the END_GROUP tag instead of precomputing
         * one more tag per field. */
        w_field_tag(w, field, 0);
        encode_message(w, value->as.msg, depth + 1, det);
        if (w->status != PBW_OK) {
            return;
        }
        end_len = pbw_varint_write(end, ((uint64_t)field->number << 3) |
                                            PBW_WIRE_END_GROUP);
        w_put(w, end, end_len);
        return;
    }
    case PBW_TYPE_MESSAGE: {
        size_t mark;

        if (value->as.msg == NULL) {
            w->status = PBW_ERR_INVALID;
            return;
        }
        if (depth == PBW_MAX_DEPTH) {
            w->status = PBW_ERR_LIMIT;
            return;
        }
        w_field_tag(w, field, 0);
        w_len_begin(w, &mark);
        encode_message(w, value->as.msg, depth + 1, det);
        if (w->status != PBW_OK) {
            return;
        }
        w_len_end(w, mark);
        return;
    }
    case PBW_TYPE_FLOAT:
    case PBW_TYPE_FIXED32:
    case PBW_TYPE_SFIXED32:
        w_field_tag(w, field, 0);
        w_fixed(w, value->as.u64, 4);
        return;
    case PBW_TYPE_DOUBLE:
    case PBW_TYPE_FIXED64:
    case PBW_TYPE_SFIXED64:
        w_field_tag(w, field, 0);
        w_fixed(w, value->as.u64, 8);
        return;
    default:
        w_field_tag(w, field, 0);
        w_varint(w, wire_value(field->type, value->as.u64));
        return;
    }
}

static void
write_packed(writer *w, const pbw_field *field, const pbw_value *list)
{
    size_t mark;
    size_t i;

    w_field_tag(w, field, 1);
    w_len_begin(w, &mark);
    for (i = 0; i < list->as.list.len; i++) {
        w_scalar(w, field->type, list->as.list.items[i]->as.u64);
        if (w->status != PBW_OK) {
            return;
        }
    }
    w_len_end(w, mark);
}

/* Deterministic encoding sorts map entries by key, like the reference
 * deterministic serializers. Only wire order changes: the bytes of each entry
 * are identical. */
static int
map_key_cmp(const pbw_field *kf, const pbw_value *a, const pbw_value *b)
{
    const pbw_message *ea = a->kind == PBW_VALUE_MESSAGE ? a->as.msg : NULL;
    const pbw_message *eb = b->kind == PBW_VALUE_MESSAGE ? b->as.msg : NULL;
    const pbw_value *ka = ea != NULL ? pbw_message_get(ea, 1) : NULL;
    const pbw_value *kb = eb != NULL ? pbw_message_get(eb, 1) : NULL;

    if (ka == kb) {
        return 0;
    }
    if (ka == NULL) {
        return -1;
    }
    if (kb == NULL) {
        return 1;
    }
    if (ka->kind == PBW_VALUE_BYTES && kb->kind == PBW_VALUE_BYTES) {
        size_t n = ka->as.bytes.len < kb->as.bytes.len ? ka->as.bytes.len
                                                       : kb->as.bytes.len;
        int r = n > 0 ? memcmp(ka->as.bytes.data, kb->as.bytes.data, n) : 0;

        if (r != 0) {
            return r < 0 ? -1 : 1;
        }
        if (ka->as.bytes.len != kb->as.bytes.len) {
            return ka->as.bytes.len < kb->as.bytes.len ? -1 : 1;
        }
        return 0;
    }
    switch (kf->type) {
    case PBW_TYPE_INT32:
    case PBW_TYPE_INT64:
    case PBW_TYPE_SINT32:
    case PBW_TYPE_SINT64:
        if ((int64_t)ka->as.u64 != (int64_t)kb->as.u64) {
            return (int64_t)ka->as.u64 < (int64_t)kb->as.u64 ? -1 : 1;
        }
        return 0;
    default:
        if (ka->as.u64 != kb->as.u64) {
            return ka->as.u64 < kb->as.u64 ? -1 : 1;
        }
        return 0;
    }
}

/* Merge sort: the recursion depth is log2(n), not message nesting, so the
 * PBW_MAX_DEPTH cap does not apply. */
static void
map_sort(const pbw_field *kf, pbw_value **items, pbw_value **tmp, size_t n)
{
    size_t mid;
    size_t i;
    size_t j;
    size_t k;

    if (n < 2) {
        return;
    }
    mid = n / 2;
    map_sort(kf, items, tmp, mid);
    map_sort(kf, items + mid, tmp + mid, n - mid);
    i = 0;
    j = mid;
    k = 0;
    while (i < mid && j < n) {
        if (map_key_cmp(kf, items[j], items[i]) < 0) {
            tmp[k++] = items[j++];
        } else {
            tmp[k++] = items[i++];
        }
    }
    while (i < mid) {
        tmp[k++] = items[i++];
    }
    while (j < n) {
        tmp[k++] = items[j++];
    }
    memcpy(items, tmp, n * sizeof(*items));
}

static void
write_map_sorted(writer *w, const pbw_field *field, const pbw_value *list,
                 uint32_t depth, int det)
{
    const pbw_field *kf = pbw_find_field(field->sub, 1);
    pbw_value **items;
    pbw_value **tmp;
    size_t n = list->as.list.len;
    size_t j;

    if (kf == NULL) {
        /* No key field: keep insertion order. */
        for (j = 0; j < n; j++) {
            write_element(w, field, list->as.list.items[j], depth, det);
            if (w->status != PBW_OK) {
                return;
            }
        }
        return;
    }
    items = (pbw_value **)malloc(n * sizeof(*items));
    tmp = (pbw_value **)malloc(n * sizeof(*tmp));
    if (items == NULL || tmp == NULL) {
        free(items);
        free(tmp);
        w->status = PBW_ERR_NOMEM;
        return;
    }
    memcpy(items, list->as.list.items, n * sizeof(*items));
    map_sort(kf, items, tmp, n);
    for (j = 0; j < n; j++) {
        write_element(w, field, items[j], depth, det);
        if (w->status != PBW_OK) {
            break;
        }
    }
    free(items);
    free(tmp);
}

static void
encode_message(writer *w, const pbw_message *msg, uint32_t depth, int det)
{
    size_t i;

    for (i = 0; i < msg->desc->field_count; i++) {
        const pbw_field *field = &msg->desc->fields[i];
        const pbw_value *value;
        size_t j;

        if (!pbw_msg_present(msg, i)) {
            continue;
        }
        value = &msg->values[i];
        if (field->label == PBW_LABEL_REPEATED) {
            if (value->kind != PBW_VALUE_LIST || value->as.list.len == 0) {
                continue;
            }
            if (field->packed && pbw_is_packable(field->type)) {
                write_packed(w, field, value);
                continue;
            }
            if (det && field->type == PBW_TYPE_MESSAGE && field->sub != NULL &&
                field->sub->map_entry) {
                write_map_sorted(w, field, value, depth, det);
                if (w->status != PBW_OK) {
                    return;
                }
                continue;
            }
            for (j = 0; j < value->as.list.len; j++) {
                write_element(w, field, value->as.list.items[j], depth, det);
                if (w->status != PBW_OK) {
                    return;
                }
            }
            continue;
        }
        write_element(w, field, value, depth, det);
        if (w->status != PBW_OK) {
            return;
        }
    }
    /* Unknown fields go last, in the order they were read: the reference
     * implementations do the same, so a re-encoded message stays byte-identical
     * for a peer that only knows part of the schema. */
    w_put(w, msg->unknown, msg->unknown_len);
}

/* Upper bound on the encoded size, used to give pbw_encode() a buffer that is
 * already big enough: one malloc instead of the doubling sequence. Varints are
 * charged their maximum width, so it is cheap to compute and never too small. */
static size_t
size_hint(const pbw_message *msg, uint32_t depth)
{
    size_t size = msg->unknown_len + 1;
    size_t i;

    if (depth >= PBW_MAX_DEPTH) {
        return size;
    }

    for (i = 0; i < msg->desc->field_count; i++) {
        const pbw_field *field = &msg->desc->fields[i];
        const pbw_value *value;

        if (!pbw_msg_present(msg, i)) {
            continue;
        }
        value = &msg->values[i];
        if (field->label == PBW_LABEL_REPEATED) {
            if (value->kind != PBW_VALUE_LIST) {
                continue;
            }
            if (field->packed && pbw_is_packable(field->type)) {
                size += (size_t)field->len_tag_len + 5 +
                        (size_t)value->as.list.len * 10;
            } else {
                /* Per-item payloads are not walked: a short hint only means the
                 * buffer grows again, which is cheap compared to the walk. */
                size += (size_t)value->as.list.len *
                        ((size_t)field->tag_len + 10);
            }
            continue;
        }
        size += (size_t)field->tag_len + 10;
        if (field->type == PBW_TYPE_STRING || field->type == PBW_TYPE_BYTES) {
            size += value->as.bytes.len;
        } else if ((field->type == PBW_TYPE_MESSAGE ||
                    field->type == PBW_TYPE_GROUP) &&
                   value->as.msg != NULL) {
            size += size_hint(value->as.msg, depth + 1);
        }
    }
    return size;
}

/* -------------------------------------------------------------- entry points */

/* Encodes into a caller-provided buffer whose capacity is reused across calls. */
pbw_status
pbw_encode_buf(pbw_buf *buf, const pbw_message *msg)
{
    writer w;

    if (msg == NULL || buf == NULL) {
        return PBW_ERR_INVALID;
    }
    buf->len = 0;
    w.buf = buf;
    w.pos = 0;
    w.status = PBW_OK;
    encode_message(&w, msg, 0, 0);
    buf->len = w.pos;
    return w.status;
}

static pbw_status
encode_alloc(const pbw_message *msg, int det, uint8_t **out, size_t *len)
{
    pbw_buf buf;
    writer w;

    if (msg == NULL || out == NULL || len == NULL) {
        return PBW_ERR_INVALID;
    }
    pbw_buf_init(&buf);
    if (!pbw_buf_reserve(&buf, size_hint(msg, 0))) {
        return PBW_ERR_NOMEM;
    }
    w.buf = &buf;
    w.pos = 0;
    w.status = PBW_OK;
    encode_message(&w, msg, 0, det);
    if (w.status != PBW_OK) {
        pbw_buf_free(&buf);
        return w.status;
    }
    if (buf.data == NULL) {
        /* Empty message: hand out a valid buffer the caller can free. */
        buf.data = (uint8_t *)malloc(1);
        if (buf.data == NULL) {
            return PBW_ERR_NOMEM;
        }
    }
    /* Transfer the buffer instead of copying: it is already at least the size
     * the message needed. */
    *out = buf.data;
    *len = w.pos;
    return PBW_OK;
}

pbw_status
pbw_encode(const pbw_message *msg, uint8_t **out, size_t *len)
{
    return encode_alloc(msg, 0, out, len);
}

pbw_status
pbw_encode_deterministic(const pbw_message *msg, uint8_t **out, size_t *len)
{
    return encode_alloc(msg, 1, out, len);
}

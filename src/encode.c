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

PBW_INLINE uint64_t
zigzag_encode(uint64_t value, unsigned width)
{
    uint64_t mask = width == 32 ? 0xffffffffull : ~0ull;
    uint64_t sign = width == 32 ? 31 : 63;
    uint64_t u = value & mask;

    return ((u << 1) ^ (0ull - (u >> sign))) & mask;
}

/* Value as it goes on the wire, before tag and length. Inlined: it runs once
 * per varint field on the encode hot path. */
PBW_INLINE uint64_t
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

/* Local packable check for the encode hot path (the global
 * pbw_is_packable stays for the other translation units). */
PBW_INLINE int
enc_packable(pbw_type type)
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
    if (PBW_EXPECT(w->buf->cap - w->pos >= extra, 1)) {
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

/* Same writes without a capacity check, for callers that reserved once for
 * the whole field (tag plus value). Encoding only appends, so the reserved
 * slack past w->pos is never read. */
PBW_INLINE void
w_tag_noreserve(writer *w, uint32_t word, size_t len)
{
    memcpy(w->buf->data + w->pos, &word, sizeof(word));
    w->pos += len;
}

PBW_INLINE void
w_varint_noreserve(writer *w, uint64_t value)
{
    uint8_t *out = w->buf->data + w->pos;
    size_t len = 0;

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
    while (value >= 0x80) {
        out[len++] = (uint8_t)((value & 0x7f) | 0x80);
        value >>= 7;
    }
    out[len++] = (uint8_t)value;
    w->pos += len;
}

PBW_INLINE void
w_fixed_noreserve(writer *w, uint64_t value, size_t size)
{
    uint8_t *out = w->buf->data + w->pos;

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

static void encode_message(writer *w, const pbw_message *msg, uint32_t depth,
                           int det);

PBW_INLINE void
write_element(writer *w, const pbw_field *field, const pbw_value *value,
              uint32_t depth, int det)
{
    /* No depth check for scalars: only messages and groups recurse, and they
     * guard against PBW_MAX_DEPTH below. Depth never exceeds the limit here
     * (top-level starts at 0, nesting is capped before recursing). */
    switch (field->type) {
    case PBW_TYPE_STRING:
    case PBW_TYPE_BYTES: {
        /* Length is known before the payload: reserve once for tag,
         * length and payload instead of three separate checks. The tag
         * write stores a whole word, so reserve 4 for it. */
        size_t lb = pbw_varint_size(value->as.bytes.len);
        if (field->tag_len <= 4) {
            if (!w_reserve(w, 4 + lb + value->as.bytes.len)) {
                w->status = PBW_ERR_NOMEM;
                return;
            }
            w_tag_noreserve(w, field->tag_word, field->tag_len);
            w_varint_noreserve(w, value->as.bytes.len);
            if (value->as.bytes.len > 0) {
                memcpy(w->buf->data + w->pos, value->as.bytes.data,
                       value->as.bytes.len);
                w->pos += value->as.bytes.len;
            }
            return;
        }
        w_field_tag(w, field, 0);
        w_varint(w, value->as.bytes.len);
        w_put(w, value->as.bytes.data, value->as.bytes.len);
        return;
    }
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
        /* Tag plus length placeholder in one reserve; the payload that
         * follows grows on demand like before. The tag write stores a
         * whole word, so reserve 4 for it. */
        if (field->tag_len <= 4) {
            if (!w_reserve(w, 4 + 1)) {
                w->status = PBW_ERR_NOMEM;
                return;
            }
            w_tag_noreserve(w, field->tag_word, field->tag_len);
            mark = w->pos;
            w->buf->data[w->pos] = 0;
            w->pos += 1;
        } else {
            w_field_tag(w, field, 0);
            w_len_begin(w, &mark);
            if (w->status != PBW_OK) {
                return;
            }
        }
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
        /* One reserve for tag plus value: the common small-message path
         * then writes without another capacity check. The tag write stores
         * a whole word, so reserve 4 for it. */
        if (field->tag_len <= 4) {
            if (!w_reserve(w, 4 + 4)) {
                w->status = PBW_ERR_NOMEM;
                return;
            }
            w_tag_noreserve(w, field->tag_word, field->tag_len);
            w_fixed_noreserve(w, value->as.u64, 4);
            return;
        }
        w_field_tag(w, field, 0);
        w_fixed(w, value->as.u64, 4);
        return;
    case PBW_TYPE_DOUBLE:
    case PBW_TYPE_FIXED64:
    case PBW_TYPE_SFIXED64:
        if (field->tag_len <= 4) {
            if (!w_reserve(w, 4 + 8)) {
                w->status = PBW_ERR_NOMEM;
                return;
            }
            w_tag_noreserve(w, field->tag_word, field->tag_len);
            w_fixed_noreserve(w, value->as.u64, 8);
            return;
        }
        w_field_tag(w, field, 0);
        w_fixed(w, value->as.u64, 8);
        return;
    default:
        if (field->tag_len <= 4) {
            if (!w_reserve(w, 4 + 10)) {
                w->status = PBW_ERR_NOMEM;
                return;
            }
            w_tag_noreserve(w, field->tag_word, field->tag_len);
            w_varint_noreserve(w, wire_value(field->type, value->as.u64));
            return;
        }
        w_field_tag(w, field, 0);
        w_varint(w, wire_value(field->type, value->as.u64));
        return;
    }
}

/* Bytes one packed item occupies. */
PBW_INLINE size_t
packed_item_size(pbw_type type, uint64_t bits)
{
    switch (type) {
    case PBW_TYPE_FLOAT:
    case PBW_TYPE_FIXED32:
    case PBW_TYPE_SFIXED32:
        return 4;
    case PBW_TYPE_DOUBLE:
    case PBW_TYPE_FIXED64:
    case PBW_TYPE_SFIXED64:
        return 8;
    default:
        return pbw_varint_size(wire_value(type, bits));
    }
}

/* One length-prefixed run of scalars. The payload size is computed first so the
 * length prefix can be written in place: no placeholder, no memmove of the
 * payload, and the reservation is exactly what the run needs (a caller-provided
 * buffer therefore does not need room for a 10-bytes-per-item worst case). */
/* Appends a varint to a caller-hoisted cursor: the blind packed write
 * loops advance a local pointer instead of the writer position per item. */
PBW_INLINE void
w_varint_out(uint8_t **out, uint64_t value)
{
    uint8_t *p = *out;

    while (value >= 0x80) {
        *p++ = (uint8_t)((value & 0x7f) | 0x80);
        value >>= 7;
    }
    *p++ = (uint8_t)value;
    *out = p;
}

static void
write_packed(writer *w, const pbw_field *field, const pbw_value *list)
{
    pbw_value **items;
    size_t n;
    size_t payload = 0;
    size_t i;

    if (w == NULL || w->status != PBW_OK || field == NULL || list == NULL) {
        if (w != NULL && w->status == PBW_OK) {
            w->status = PBW_ERR_INVALID;
        }
        return;
    }
    if (list->kind != PBW_VALUE_LIST) {
        w->status = PBW_ERR_INVALID;
        return;
    }
    items = list->as.list.items;
    n = list->as.list.len;
    if (n == 0) {
        return;
    }
    if (items == NULL) {
        w->status = PBW_ERR_INVALID;
        return;
    }
    if (n > (SIZE_MAX - 16) / 10) {
        w->status = PBW_ERR_LIMIT;
        return;
    }
    /* One dispatch for the whole run: every item shares the field's type.
     * Fixed-size kinds write in a single validated pass (their payload is
     * known upfront); varint kinds size first and then run a blind
     * kind-specific write. Every loop hoists the output pointer into a
     * local: advancing through the writer struct per item stalls the
     * pipeline and blocks vectorization. n is capped above, so n * 8 below
     * cannot wrap. */
    switch (field->type) {
    case PBW_TYPE_BOOL: {
        uint8_t *out;
        payload = n;
        if (!w_reserve(w, (field->len_tag_len <= 4 ? 4 : field->len_tag_len) +
                              pbw_varint_size(payload) + payload)) {
            w->status = PBW_ERR_NOMEM;
            return;
        }
        if (field->len_tag_len <= 4) {
            w_tag_noreserve(w, field->len_tag_word, field->len_tag_len);
        } else {
            memcpy(w->buf->data + w->pos, field->len_tag, field->len_tag_len);
            w->pos += field->len_tag_len;
        }
        w_varint_noreserve(w, payload);
        out = w->buf->data + w->pos;
        for (i = 0; i < n; i++) {
            pbw_value *item = items[i];
            if (item == NULL || item->kind != PBW_VALUE_UINT) {
                w->pos += i;
                w->status = PBW_ERR_INVALID;
                return;
            }
            out[i] = item->as.u64 != 0 ? 1u : 0u;
        }
        w->pos += n;
        return;
    }
    case PBW_TYPE_FLOAT:
    case PBW_TYPE_FIXED32:
    case PBW_TYPE_SFIXED32: {
        uint8_t *out;
        payload = n * 4;
        if (!w_reserve(w, (field->len_tag_len <= 4 ? 4 : field->len_tag_len) +
                              pbw_varint_size(payload) + payload)) {
            w->status = PBW_ERR_NOMEM;
            return;
        }
        if (field->len_tag_len <= 4) {
            w_tag_noreserve(w, field->len_tag_word, field->len_tag_len);
        } else {
            memcpy(w->buf->data + w->pos, field->len_tag, field->len_tag_len);
            w->pos += field->len_tag_len;
        }
        w_varint_noreserve(w, payload);
        out = w->buf->data + w->pos;
        for (i = 0; i < n; i++) {
            pbw_value *item = items[i];
            uint32_t v;
            if (item == NULL || item->kind != PBW_VALUE_UINT) {
                w->pos += i * 4;
                w->status = PBW_ERR_INVALID;
                return;
            }
            v = (uint32_t)item->as.u64;
            out[i * 4] = (uint8_t)v;
            out[i * 4 + 1] = (uint8_t)(v >> 8);
            out[i * 4 + 2] = (uint8_t)(v >> 16);
            out[i * 4 + 3] = (uint8_t)(v >> 24);
        }
        w->pos += payload;
        return;
    }
    case PBW_TYPE_DOUBLE:
    case PBW_TYPE_FIXED64:
    case PBW_TYPE_SFIXED64: {
        uint8_t *out;
        payload = n * 8;
        if (!w_reserve(w, (field->len_tag_len <= 4 ? 4 : field->len_tag_len) +
                              pbw_varint_size(payload) + payload)) {
            w->status = PBW_ERR_NOMEM;
            return;
        }
        if (field->len_tag_len <= 4) {
            w_tag_noreserve(w, field->len_tag_word, field->len_tag_len);
        } else {
            memcpy(w->buf->data + w->pos, field->len_tag, field->len_tag_len);
            w->pos += field->len_tag_len;
        }
        w_varint_noreserve(w, payload);
        out = w->buf->data + w->pos;
        for (i = 0; i < n; i++) {
            pbw_value *item = items[i];
            uint64_t v;
            if (item == NULL || item->kind != PBW_VALUE_UINT) {
                w->pos += i * 8;
                w->status = PBW_ERR_INVALID;
                return;
            }
            v = item->as.u64;
            out[i * 8] = (uint8_t)v;
            out[i * 8 + 1] = (uint8_t)(v >> 8);
            out[i * 8 + 2] = (uint8_t)(v >> 16);
            out[i * 8 + 3] = (uint8_t)(v >> 24);
            out[i * 8 + 4] = (uint8_t)(v >> 32);
            out[i * 8 + 5] = (uint8_t)(v >> 40);
            out[i * 8 + 6] = (uint8_t)(v >> 48);
            out[i * 8 + 7] = (uint8_t)(v >> 56);
        }
        w->pos += payload;
        return;
    }
    case PBW_TYPE_INT32:
    case PBW_TYPE_ENUM:
        for (i = 0; i < n; i++) {
            if (items[i] == NULL || items[i]->kind != PBW_VALUE_UINT) {
                w->status = PBW_ERR_INVALID;
                return;
            }
            payload += pbw_varint_size(
                (uint64_t)(int64_t)(int32_t)items[i]->as.u64);
        }
        break;
    case PBW_TYPE_UINT32:
        for (i = 0; i < n; i++) {
            if (items[i] == NULL || items[i]->kind != PBW_VALUE_UINT) {
                w->status = PBW_ERR_INVALID;
                return;
            }
            payload += pbw_varint_size((uint64_t)(uint32_t)items[i]->as.u64);
        }
        break;
    case PBW_TYPE_SINT32:
        for (i = 0; i < n; i++) {
            if (items[i] == NULL || items[i]->kind != PBW_VALUE_UINT) {
                w->status = PBW_ERR_INVALID;
                return;
            }
            payload += pbw_varint_size(
                zigzag_encode(items[i]->as.u64, 32));
        }
        break;
    case PBW_TYPE_SINT64:
        for (i = 0; i < n; i++) {
            if (items[i] == NULL || items[i]->kind != PBW_VALUE_UINT) {
                w->status = PBW_ERR_INVALID;
                return;
            }
            payload += pbw_varint_size(
                zigzag_encode(items[i]->as.u64, 64));
        }
        break;
    default:
        for (i = 0; i < n; i++) {
            if (items[i] == NULL || items[i]->kind != PBW_VALUE_UINT) {
                w->status = PBW_ERR_INVALID;
                return;
            }
            payload += pbw_varint_size(items[i]->as.u64);
        }
        break;
    }
    /* Four bytes cover the tag word store; a five-byte length tag (field
     * number >= 2^21) is copied byte by byte instead. */
    if (!w_reserve(w, (field->len_tag_len <= 4 ? 4 : field->len_tag_len) +
                          pbw_varint_size(payload) + payload)) {
        w->status = PBW_ERR_NOMEM;
        return;
    }
    if (field->len_tag_len <= 4) {
        w_tag_noreserve(w, field->len_tag_word, field->len_tag_len);
    } else {
        memcpy(w->buf->data + w->pos, field->len_tag, field->len_tag_len);
        w->pos += field->len_tag_len;
    }
    w_varint_noreserve(w, payload);
    /* Blind write pass: every item was validated above, so the output
     * pointer stays hoisted with no checks and no switches in the loop. */
    switch (field->type) {
    case PBW_TYPE_INT32:
    case PBW_TYPE_ENUM: {
        uint8_t *out = w->buf->data + w->pos;
        for (i = 0; i < n; i++) {
            w_varint_out(&out,
                         (uint64_t)(int64_t)(int32_t)items[i]->as.u64);
        }
        w->pos += payload;
        return;
    }
    case PBW_TYPE_UINT32: {
        uint8_t *out = w->buf->data + w->pos;
        for (i = 0; i < n; i++) {
            w_varint_out(&out, (uint64_t)(uint32_t)items[i]->as.u64);
        }
        w->pos += payload;
        return;
    }
    case PBW_TYPE_SINT32: {
        uint8_t *out = w->buf->data + w->pos;
        for (i = 0; i < n; i++) {
            w_varint_out(&out, zigzag_encode(items[i]->as.u64, 32));
        }
        w->pos += payload;
        return;
    }
    case PBW_TYPE_SINT64: {
        uint8_t *out = w->buf->data + w->pos;
        for (i = 0; i < n; i++) {
            w_varint_out(&out, zigzag_encode(items[i]->as.u64, 64));
        }
        w->pos += payload;
        return;
    }
    default: {
        uint8_t *out = w->buf->data + w->pos;
        for (i = 0; i < n; i++) {
            w_varint_out(&out, items[i]->as.u64);
        }
        w->pos += payload;
        return;
    }
    }
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
    /* The presence bitmap never changes while encoding, so hoist the first
     * word: most messages have 32 fields or fewer and then test bits without
     * reloading the bitmap per field. Larger descriptors use the generic
     * accessor. */
    int single_word = msg->desc->field_count <= 32;
    uint32_t present0 =
        (single_word && msg->desc->field_count > 0 && msg->present != NULL)
            ? msg->present[0]
            : 0;

    for (i = 0; i < msg->desc->field_count; i++) {
        const pbw_field *field = &msg->desc->fields[i];
        const pbw_value *value;
        size_t j;
        int present = single_word ? (int)((present0 >> (i & 31u)) & 1u)
                                  : pbw_msg_present(msg, i);

        if (!PBW_EXPECT(present, 1)) {
            continue;
        }
        value = &msg->values[i];
        if (field->label == PBW_LABEL_REPEATED) {
            if (value->kind != PBW_VALUE_LIST || value->as.list.len == 0) {
                continue;
            }
            if (field->packed && enc_packable(field->type)) {
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

/* ------------------------------------------------------------------ sizing */

/* Exact wire size, mirroring encode_message()/write_element() branch for
 * branch. The per-item helpers (packed_item_size, wire_value,
 * pbw_varint_size) are shared, so only the traversal itself can drift: the
 * self-checks and the bench difftest assert size == encoded length for every
 * message they walk, fuzzed ones included. */

/* a + b, refusing to wrap: lengths come from user data and a wrapped total
 * would make the caller allocate too little. */
static int
size_add(size_t *total, size_t extra)
{
    if (*total > SIZE_MAX - extra) {
        return 0;
    }
    *total += extra;
    return 1;
}

/* tag + length varint + payload, the shape of every length-delimited field. */
static pbw_status
size_len_prefixed(size_t *out, size_t tag_len, size_t body)
{
    size_t total = tag_len;

    if (!size_add(&total, pbw_varint_size(body)) || !size_add(&total, body)) {
        return PBW_ERR_LIMIT;
    }
    *out = total;
    return PBW_OK;
}

static pbw_status size_message(const pbw_message *msg, uint32_t depth,
                               size_t *out);

static pbw_status
size_element(const pbw_field *field, const pbw_value *value, uint32_t depth,
             size_t *out)
{
    size_t body;
    pbw_status status;

    switch (field->type) {
    case PBW_TYPE_STRING:
    case PBW_TYPE_BYTES:
        return size_len_prefixed(out, field->tag_len, value->as.bytes.len);
    case PBW_TYPE_MESSAGE:
        if (value->as.msg == NULL) {
            return PBW_ERR_INVALID;
        }
        if (depth == PBW_MAX_DEPTH) {
            return PBW_ERR_LIMIT;
        }
        status = size_message(value->as.msg, depth + 1, &body);
        if (status != PBW_OK) {
            return status;
        }
        return size_len_prefixed(out, field->tag_len, body);
    case PBW_TYPE_GROUP:
        if (value->as.msg == NULL) {
            return PBW_ERR_INVALID;
        }
        if (depth == PBW_MAX_DEPTH) {
            return PBW_ERR_LIMIT;
        }
        status = size_message(value->as.msg, depth + 1, &body);
        if (status != PBW_OK) {
            return status;
        }
        /* A group has no length prefix: the END_GROUP tag stands in for it. */
        if (!size_add(&body, pbw_varint_size(((uint64_t)field->number << 3) |
                                             PBW_WIRE_END_GROUP)) ||
            !size_add(&body, field->tag_len)) {
            return PBW_ERR_LIMIT;
        }
        *out = body;
        return PBW_OK;
    case PBW_TYPE_FLOAT:
    case PBW_TYPE_FIXED32:
    case PBW_TYPE_SFIXED32:
        *out = field->tag_len + 4;
        return PBW_OK;
    case PBW_TYPE_DOUBLE:
    case PBW_TYPE_FIXED64:
    case PBW_TYPE_SFIXED64:
        *out = field->tag_len + 8;
        return PBW_OK;
    default:
        body = pbw_varint_size(wire_value(field->type, value->as.u64));
        if (!size_add(&body, field->tag_len)) {
            return PBW_ERR_LIMIT;
        }
        *out = body;
        return PBW_OK;
    }
}

static pbw_status
size_message(const pbw_message *msg, uint32_t depth, size_t *out)
{
    size_t total = 0;
    size_t i;
    /* Same presence hoist as encode_message(): one bitmap word covers most
     * descriptors, so the walk does not reload it per field. */
    int single_word = msg->desc->field_count <= 32;
    uint32_t present0 =
        (single_word && msg->desc->field_count > 0 && msg->present != NULL)
            ? msg->present[0]
            : 0;

    for (i = 0; i < msg->desc->field_count; i++) {
        const pbw_field *field = &msg->desc->fields[i];
        const pbw_value *value;
        size_t part = 0;
        size_t j;
        int present = single_word ? (int)((present0 >> (i & 31u)) & 1u)
                                  : pbw_msg_present(msg, i);

        if (!PBW_EXPECT(present, 1)) {
            continue;
        }
        value = &msg->values[i];
        if (field->label == PBW_LABEL_REPEATED) {
            size_t n;
            pbw_status status;

            if (value->kind != PBW_VALUE_LIST || value->as.list.len == 0) {
                continue;
            }
            n = value->as.list.len;
            if (field->packed && enc_packable(field->type)) {
                size_t payload = 0;

                for (j = 0; j < n; j++) {
                    if (!size_add(&payload,
                                  packed_item_size(
                                      field->type,
                                      value->as.list.items[j]->as.u64))) {
                        return PBW_ERR_LIMIT;
                    }
                }
                status = size_len_prefixed(&part, field->len_tag_len, payload);
                if (status != PBW_OK) {
                    return status;
                }
            } else {
                for (j = 0; j < n; j++) {
                    size_t item = 0;

                    status = size_element(field, value->as.list.items[j],
                                          depth, &item);
                    if (status != PBW_OK) {
                        return status;
                    }
                    if (!size_add(&part, item)) {
                        return PBW_ERR_LIMIT;
                    }
                }
            }
        } else {
            pbw_status status = size_element(field, value, depth, &part);

            if (status != PBW_OK) {
                return status;
            }
        }
        if (!size_add(&total, part)) {
            return PBW_ERR_LIMIT;
        }
    }
    /* Unknown fields are written verbatim after the known ones. */
    if (!size_add(&total, msg->unknown_len)) {
        return PBW_ERR_LIMIT;
    }
    *out = total;
    return PBW_OK;
}

pbw_status
pbw_message_encoded_size(const pbw_message *msg, size_t *len)
{
    if (msg == NULL || len == NULL) {
        return PBW_ERR_INVALID;
    }
    return size_message(msg, 0, len);
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
    {
        /* Load-time hint first: a decoded tree re-encodes to about its wire
         * length, so no walk is needed (exact for untouched trees; the
         * writer grows if it was mutated, hence no invalidation).
         * Hand-built trees start from a small fixed buffer and grow on
         * demand: walking the tree to size it costs more than one or two
         * doublings for small messages, and large ones only pay a few
         * realloc copies (measured 2026-11). 256 stays in the fast tiny
         * allocation class on common allocators, so small messages avoid
         * both the sizing walk and a slow large malloc. No shared state is
         * touched, so concurrent encodes of one message stay independent. */
        size_t initial;

        if (msg->wire_len > 0 && msg->wire_len <= SIZE_MAX - 32) {
            initial = msg->wire_len + 32;
        } else {
            initial = 256;
        }
        if (!pbw_buf_reserve(&buf, initial)) {
            return PBW_ERR_NOMEM;
        }
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

pbw_status
pbw_encode_to(const pbw_message *msg, void *out, size_t cap, size_t *len)
{
    pbw_buf buf;
    writer w;
    pbw_status status;

    if (msg == NULL || len == NULL || (out == NULL && cap > 0)) {
        return PBW_ERR_INVALID;
    }
    pbw_buf_init_fixed(&buf, out, cap);
    w.buf = &buf;
    w.pos = 0;
    w.status = PBW_OK;
    encode_message(&w, msg, 0, 0);
    status = w.status;
    if (status == PBW_ERR_NOMEM && buf.fixed) {
        /* A fixed buffer cannot fail to allocate: the write did not fit. */
        status = PBW_ERR_LIMIT;
    }
    if (status == PBW_OK) {
        *len = w.pos;
    }
    return status;
}

/* Streaming helpers: length-delimited framing (varint length + message) over an
 * incremental byte stream. The reader buffers input until a whole frame
 * arrives and then decodes it with the one-shot decoder, so no second parsing
 * path can drift out of sync; the sink encodes as usual and hands the bytes
 * to a caller callback in 8 KiB chunks.
 */

#include "internal.h"

struct pbw_istream {
    uint8_t *data;
    size_t len;
    size_t cap;
    size_t off; /* consumed prefix, compacted when it grows */
    pbw_limits limits;
};

#define PBW_SINK_CHUNK 8192

pbw_istream *
pbw_istream_new(const pbw_limits *limits)
{
    pbw_istream *s = (pbw_istream *)calloc(1, sizeof(*s));

    if (s == NULL) {
        return NULL;
    }
    if (limits != NULL) {
        s->limits = *limits;
    }
    return s;
}

void
pbw_istream_free(pbw_istream *s)
{
    if (s == NULL) {
        return;
    }
    free(s->data);
    free(s);
}

void
pbw_istream_reset(pbw_istream *s)
{
    if (s == NULL) {
        return;
    }
    s->len = 0;
    s->off = 0;
}

pbw_status
pbw_istream_feed(pbw_istream *s, const void *data, size_t len)
{
    if (s == NULL || (data == NULL && len > 0)) {
        return PBW_ERR_INVALID;
    }
    if (len == 0) {
        return PBW_OK;
    }
    if (!pbw_cap_holds(s->cap, s->len) || s->off > s->len) {
        return PBW_ERR_INVALID;
    }
    if (s->data == NULL && (s->len > 0 || s->cap > 0 || s->off > 0)) {
        return PBW_ERR_INVALID;
    }
    /* Compact the consumed prefix only when the tail has no room: a steady
     * feed/next loop then appends without a memmove per chunk. */
    if (len > s->cap - s->len) {
        size_t cap;
        uint8_t *grown;

        if (s->off > 0) {
            if (s->data == NULL) {
                return PBW_ERR_INVALID;
            }
            memmove(s->data, s->data + s->off, s->len - s->off);
            s->len -= s->off;
            s->off = 0;
        }
        if (len > s->cap - s->len) {
            /* Compacting (or none needed): the tail still has to grow. */
            if (s->cap > SIZE_MAX - s->len) {
                return PBW_ERR_NOMEM;
            }
            cap = s->cap != 0 ? s->cap : 256;
            while (cap - s->len < len) {
                if (cap > SIZE_MAX / 2) {
                    /* No allocation can hold this chunk; report it instead of
                     * spinning on a wrapped capacity. */
                    return PBW_ERR_NOMEM;
                }
                cap *= 2;
            }
            grown = (uint8_t *)realloc(s->data, cap);
            if (grown == NULL) {
                return PBW_ERR_NOMEM;
            }
            s->data = grown;
            s->cap = cap;
        }
    }
    if (s->data == NULL) {
        return PBW_ERR_NOMEM;
    }
    if (s->len > SIZE_MAX - len) {
        return PBW_ERR_NOMEM;
    }
    memcpy(s->data + s->len, data, len);
    s->len += len;
    return PBW_OK;
}

/* One delimited frame from the buffered bytes: NULL + PBW_OK means more data
 * is needed. A frame that fails its limits or decodes badly leaves the buffer
 * untouched, so the caller can reset() and carry on with the next stream. */
pbw_status
pbw_istream_next(pbw_istream *s, const pbw_descriptor *desc, pbw_message **out)
{
    const uint8_t *base;
    size_t avail;
    size_t used = 0;
    size_t frame;
    pbw_status status;

    if (s == NULL || desc == NULL || out == NULL) {
        return PBW_ERR_INVALID;
    }
    *out = NULL;
    if (s->off > s->len || !pbw_cap_holds(s->cap, s->len)) {
        return PBW_ERR_INVALID;
    }
    avail = s->len - s->off;
    if (avail == 0) {
        return PBW_OK;
    }
    if (s->data == NULL) {
        return PBW_ERR_INVALID;
    }
    base = s->data + s->off;
    if (!pbw_len_read(base, base + avail, &frame, &used)) {
        /* A length varint is at most 5 bytes: anything longer is corrupt,
         * anything shorter may just be split across feeds. */
        if (avail >= 5) {
            return PBW_ERR_INVALID;
        }
        return PBW_OK;
    }
    if (s->limits.max_bytes > 0 && frame > s->limits.max_bytes) {
        return PBW_ERR_LIMIT;
    }
    if (used > avail || frame > avail - used) {
        return PBW_OK;
    }
    status = pbw_decode_limits(desc, base + used, frame, &s->limits, out);
    if (status != PBW_OK) {
        return status;
    }
    s->off += used + frame;
    return PBW_OK;
}

pbw_status
pbw_encode_sink(const pbw_message *msg, pbw_sink_fn sink, void *opaque)
{
    pbw_buf buf;
    pbw_status encode;
    size_t at = 0;

    if (msg == NULL || sink == NULL) {
        return PBW_ERR_INVALID;
    }
    pbw_buf_init(&buf);
    encode = pbw_encode_buf(&buf, msg);
    if (encode != PBW_OK) {
        pbw_buf_free(&buf);
        return encode;
    }
    /* shortcut: one callback per chunk, not per field; a per-field sink would
     * re-enter the encoder for every tag, upgrade when zero-copy matters. */
    while (at < buf.len) {
        size_t n = buf.len - at > PBW_SINK_CHUNK ? PBW_SINK_CHUNK : buf.len - at;
        pbw_status fed = sink(buf.data + at, n, opaque);

        if (fed != PBW_OK) {
            pbw_buf_free(&buf);
            return fed;
        }
        at += n;
    }
    pbw_buf_free(&buf);
    return PBW_OK;
}

pbw_status
pbw_encode_delimited(const pbw_message *msg, uint8_t **out, size_t *len)
{
    uint8_t *body = NULL;
    size_t body_len = 0;
    size_t prefix;
    uint8_t *framed;
    pbw_status status;

    if (msg == NULL || out == NULL || len == NULL) {
        return PBW_ERR_INVALID;
    }
    *out = NULL;
    *len = 0;
    status = pbw_encode(msg, &body, &body_len);
    if (status != PBW_OK) {
        return status;
    }
    prefix = pbw_varint_size(body_len);
    if (prefix > SIZE_MAX - body_len) {
        pbw_free(body);
        return PBW_ERR_NOMEM;
    }
    framed = (uint8_t *)malloc(prefix + body_len);
    if (framed == NULL) {
        pbw_free(body);
        return PBW_ERR_NOMEM;
    }
    pbw_varint_write(framed, body_len);
    if (body_len > 0) {
        memcpy(framed + prefix, body, body_len);
    }
    pbw_free(body);
    *out = framed;
    *len = prefix + body_len;
    return PBW_OK;
}

pbw_status
pbw_encode_delimited_to(const pbw_message *msg, void *out, size_t cap,
                        size_t *len)
{
    size_t body = 0;
    size_t prefix;
    size_t written = 0;
    pbw_status status;

    if (len == NULL) {
        return PBW_ERR_INVALID;
    }
    *len = 0;
    if (msg == NULL || (out == NULL && cap > 0)) {
        return PBW_ERR_INVALID;
    }
    status = pbw_message_encoded_size(msg, &body);
    if (status != PBW_OK) {
        return status;
    }
    prefix = pbw_varint_size(body);
    if (prefix > cap || body > cap - prefix) {
        return PBW_ERR_LIMIT;
    }
    pbw_varint_write((uint8_t *)out, body);
    /* The residual cap carries the slack pbw_encode_to() needs; the caller
     * sized the buffer, so a short one fails instead of growing. */
    status = pbw_encode_to(msg, (uint8_t *)out + prefix, cap - prefix, &written);
    if (status != PBW_OK) {
        return status;
    }
    if (written != body) {
        /* encoded_size() is exact, so a mismatch would emit a frame whose
         * declared length lies about its body. Never hand that back. */
        return PBW_ERR_INVALID;
    }
    *len = prefix + written;
    return PBW_OK;
}

/* Split one buffered length-delimited frame into its declared length and its
 * body. PBW_ERR_LIMIT when the declared length is past the cap (checked before
 * anything is decoded, like pbw_istream_next); PBW_ERR_INVALID on a corrupt
 * prefix or a frame that runs past the buffer, since a one-shot decode has
 * nowhere to wait for more bytes. */
static pbw_status
frame_split(const void *buf, size_t len, const pbw_limits *limits,
            const uint8_t **body, size_t *frame, size_t *used)
{
    const uint8_t *base = (const uint8_t *)buf;

    if (!pbw_len_read(base, base + len, frame, used)) {
        return PBW_ERR_INVALID;
    }
    if (*used > len || *frame > len - *used) {
        return PBW_ERR_INVALID;
    }
    if (limits != NULL && limits->max_bytes > 0 && *frame > limits->max_bytes) {
        return PBW_ERR_LIMIT;
    }
    *body = base + *used;
    return PBW_OK;
}

pbw_status
pbw_decode_delimited_limits(const pbw_descriptor *desc, const void *buf,
                            size_t len, const pbw_limits *limits,
                            pbw_message **out, size_t *consumed)
{
    const uint8_t *body = NULL;
    size_t frame = 0;
    size_t used = 0;
    pbw_status status;

    if (out != NULL) {
        *out = NULL;
    }
    if (consumed != NULL) {
        *consumed = 0;
    }
    if (desc == NULL || out == NULL || consumed == NULL ||
        (buf == NULL && len > 0)) {
        return PBW_ERR_INVALID;
    }
    if (len == 0) {
        return PBW_ERR_INVALID;
    }
    status = frame_split(buf, len, limits, &body, &frame, &used);
    if (status != PBW_OK) {
        return status;
    }
    status = pbw_decode_limits(desc, body, frame, limits, out);
    if (status != PBW_OK) {
        return status;
    }
    *consumed = used + frame;
    return PBW_OK;
}

pbw_status
pbw_decode_delimited_ctx_limits(pbw_ctx *ctx, const pbw_descriptor *desc,
                                const void *buf, size_t len,
                                const pbw_limits *limits, pbw_message **out,
                                size_t *consumed)
{
    const uint8_t *body = NULL;
    size_t frame = 0;
    size_t used = 0;
    pbw_status status;

    if (out != NULL) {
        *out = NULL;
    }
    if (consumed != NULL) {
        *consumed = 0;
    }
    if (ctx == NULL || desc == NULL || out == NULL || consumed == NULL ||
        (buf == NULL && len > 0)) {
        return PBW_ERR_INVALID;
    }
    if (len == 0) {
        return PBW_ERR_INVALID;
    }
    status = frame_split(buf, len, limits, &body, &frame, &used);
    if (status != PBW_OK) {
        return status;
    }
    status = pbw_decode_ctx_limits(ctx, desc, body, frame, limits, out);
    if (status != PBW_OK) {
        return status;
    }
    *consumed = used + frame;
    return PBW_OK;
}

pbw_status
pbw_decode_delimited(const pbw_descriptor *desc, const void *buf, size_t len,
                     pbw_message **out, size_t *consumed)
{
    return pbw_decode_delimited_limits(desc, buf, len, NULL, out, consumed);
}

pbw_status
pbw_decode_delimited_ctx(pbw_ctx *ctx, const pbw_descriptor *desc,
                         const void *buf, size_t len, pbw_message **out,
                         size_t *consumed)
{
    return pbw_decode_delimited_ctx_limits(ctx, desc, buf, len, NULL, out,
                                           consumed);
}

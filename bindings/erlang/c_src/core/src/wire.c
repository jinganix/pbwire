/* Wire format primitives: varints, tags, little-endian fixed values, output
 * buffer. */

#include <locale.h>
#include <stdio.h>

#include "internal.h"

const uint8_t pbw_empty_input[1] = {0};

size_t
pbw_varint_write(uint8_t *out, uint64_t value)
{
    size_t len = 0;
    while (value >= 0x80) {
        out[len++] = (uint8_t)((value & 0x7f) | 0x80);
        value >>= 7;
    }
    out[len++] = (uint8_t)value;
    return len;
}

/* Returns 1 on success, 0 on truncation or overflow. Rejects varints longer
 * than 10 bytes, which no valid encoder produces. Bits above bit 63 of the last
 * byte are dropped, matching the reference parser. */
int
pbw_varint_read(const uint8_t *p, const uint8_t *end, uint64_t *value,
                size_t *used)
{
    uint64_t result;
    unsigned i;
    size_t avail;

    if (p == NULL || end == NULL || value == NULL || used == NULL ||
        p >= end) {
        return 0;
    }
    /* One-byte varints (small tags and lengths) are the common case. */
    result = p[0];
    if (result < 0x80) {
        *value = result;
        *used = 1;
        return 1;
    }
    avail = pbw_remaining(end, p);
    if (avail < 10) {
        result = 0;
        for (i = 0; i < 10 && (size_t)i < avail; i++) {
            uint8_t byte = p[i];

            result |= (uint64_t)(byte & 0x7f) << (7 * i);
            if ((byte & 0x80) == 0) {
                *value = result;
                *used = i + 1;
                return 1;
            }
        }
        return 0;
    }
    /* Ten readable bytes: no per-byte bounds check, and the shifts are
     * constants, which is what makes long (negative int32/int64) varints cheap. */
    result = p[0] & 0x7f;
    result |= (uint64_t)(p[1] & 0x7f) << 7;
    if (p[1] < 0x80) {
        *value = result;
        *used = 2;
        return 1;
    }
    result |= (uint64_t)(p[2] & 0x7f) << 14;
    if (p[2] < 0x80) {
        *value = result;
        *used = 3;
        return 1;
    }
    result |= (uint64_t)(p[3] & 0x7f) << 21;
    if (p[3] < 0x80) {
        *value = result;
        *used = 4;
        return 1;
    }
    result |= (uint64_t)(p[4] & 0x7f) << 28;
    if (p[4] < 0x80) {
        *value = result;
        *used = 5;
        return 1;
    }
    result |= (uint64_t)(p[5] & 0x7f) << 35;
    if (p[5] < 0x80) {
        *value = result;
        *used = 6;
        return 1;
    }
    result |= (uint64_t)(p[6] & 0x7f) << 42;
    if (p[6] < 0x80) {
        *value = result;
        *used = 7;
        return 1;
    }
    result |= (uint64_t)(p[7] & 0x7f) << 49;
    if (p[7] < 0x80) {
        *value = result;
        *used = 8;
        return 1;
    }
    result |= (uint64_t)(p[8] & 0x7f) << 56;
    if (p[8] < 0x80) {
        *value = result;
        *used = 9;
        return 1;
    }
    result |= (uint64_t)(p[9] & 0x7f) << 63;
    if (p[9] < 0x80) {
        *value = result;
        *used = 10;
        return 1;
    }
    return 0;
}

int
pbw_tag_read(const uint8_t *p, const uint8_t *end, uint32_t *tag, size_t *used)
{
    uint32_t result;
    unsigned i;
    size_t avail;

    if (p == NULL || end == NULL || tag == NULL || used == NULL ||
        p >= end) {
        return 0;
    }
    result = p[0];
    if (result < 0x80) {
        *tag = result;
        *used = 1;
        return 1;
    }
    result &= 0x7f;
    avail = pbw_remaining(end, p);
    /* A tag is a varint32: the fifth byte only contributes its four low bits,
     * anything above is dropped exactly like the reference parser (a 5-byte tag
     * may overflow without being rejected). A 6-byte tag is invalid. */
    for (i = 1; i < 5 && (size_t)i < avail; i++) {
        uint32_t byte = p[i];

        result |= (byte & 0x7f) << (7 * i);
        if (byte < 0x80) {
            *tag = result;
            *used = i + 1;
            return 1;
        }
    }
    return 0;
}

/* A length prefix is a strict varint32 in the reference parser: at most five
 * bytes, and the fifth may only carry the four low bits. Unlike a tag, an
 * out-of-range length is rejected instead of truncated. */
int
pbw_len_read(const uint8_t *p, const uint8_t *end, size_t *len, size_t *used)
{
    uint32_t result;
    unsigned i;
    size_t avail;

    if (p == NULL || end == NULL || len == NULL || used == NULL ||
        p >= end) {
        return 0;
    }
    result = p[0];
    if (result < 0x80) {
        *len = result;
        *used = 1;
        return 1;
    }
    result &= 0x7f;
    avail = pbw_remaining(end, p);
    for (i = 1; i < 5 && (size_t)i < avail; i++) {
        uint32_t byte = p[i];

        if (i == 4 && (byte & 0x7f) > 0x0f) {
            return 0;
        }
        result |= (byte & 0x7f) << (7 * i);
        if (byte < 0x80) {
            *len = (size_t)result;
            *used = i + 1;
            return 1;
        }
    }
    return 0;
}

void
pbw_buf_init(pbw_buf *buf)
{
    buf->data = NULL;
    buf->len = 0;
    buf->cap = 0;
    buf->oom = 0;
    buf->fixed = 0;
}

void
pbw_buf_init_fixed(pbw_buf *buf, void *data, size_t cap)
{
    buf->data = (uint8_t *)data;
    buf->len = 0;
    buf->cap = cap;
    buf->oom = 0;
    buf->fixed = 1;
}

void
pbw_buf_free(pbw_buf *buf)
{
    free(buf->data);
    pbw_buf_init(buf);
}

int
pbw_buf_reserve(pbw_buf *buf, size_t extra)
{
    size_t need;
    size_t cap;
    uint8_t *data;

    if (buf == NULL || buf->oom) {
        return 0;
    }
    if (!pbw_cap_holds(buf->cap, buf->len)) {
        buf->oom = 1;
        return 0;
    }
    need = buf->len + extra;
    if (need < buf->len) {
        /* Size overflow: there is no buffer that big. */
        buf->oom = 1;
        return 0;
    }
    if (need <= buf->cap) {
        return 1;
    }
    if (buf->fixed) {
        /* Caller memory: report the overflow as "does not fit". */
        buf->oom = 1;
        return 0;
    }
    cap = buf->cap ? buf->cap : 64;
    while (cap < need) {
        if (cap > SIZE_MAX / 2) {
            cap = need;
            break;
        }
        cap *= 2;
    }
    data = (uint8_t *)realloc(buf->data, cap);
    if (data == NULL) {
        buf->oom = 1;
        return 0;
    }
    buf->data = data;
    buf->cap = cap;
    return 1;
}

uint32_t
pbw_read_le32(const uint8_t *in)
{
    return (uint32_t)in[0] | ((uint32_t)in[1] << 8) | ((uint32_t)in[2] << 16) |
           ((uint32_t)in[3] << 24);
}

/* JSON and text format define '.' as the decimal separator, but strtod and
 * snprintf follow LC_NUMERIC, a process-global the embedding host may have set.
 * The public entry points ask pbw_locale_dot() once and pass the answer down, so
 * the C-locale path is exactly the libc call it always was. */

int
pbw_locale_dot(void)
{
    const char *dp = localeconv()->decimal_point;

    return dp != NULL && dp[0] == '.' && dp[1] == '\0';
}

/* Rare path: the host locale's decimal separator is not '.'. */
double
pbw_strtod_slow(const char *text, char **end, double value, char *stop)
{
    const char *dp = localeconv()->decimal_point;
    size_t before = (size_t)(stop - text);
    size_t dpsize;
    size_t len;
    char stack[64];
    char *buf = stack;

    if (dp == NULL || (dp[0] == '.' && dp[1] == '\0')) {
        if (end != NULL) {
            *end = stop;
        }
        return value;
    }
    dpsize = strlen(dp);
    len = strlen(text) + 1;
    if (len + dpsize > sizeof(stack)) {
        buf = (char *)malloc(len + dpsize);
        if (buf == NULL) {
            if (end != NULL) {
                *end = (char *)text;
            }
            return 0.0;
        }
    }
    memcpy(buf, text, before);
    memcpy(buf + before, dp, dpsize);
    memcpy(buf + before + dpsize, stop + 1, len - before - 1);
    {
        char *translated_stop = NULL;
        size_t used = before;

        value = strtod(buf, &translated_stop);
        /* Map the translated end offset back onto the original text. A parse
         * that stopped inside the translated decimal separator keeps `before`
         * (the '.' position): subtracting the separator width there would
         * underflow and hand back an end pointer outside the buffer. */
        if (translated_stop != NULL &&
            translated_stop >= buf + (dpsize - 1)) {
            used = (size_t)(translated_stop - buf) - (dpsize - 1);
        }
        if (end != NULL) {
            *end = (char *)text + used;
        }
    }
    if (buf != stack) {
        free(buf);
    }
    return value;
}

int
pbw_format_g_slow(char *buf, int n)
{
    const char *dp = localeconv()->decimal_point;
    char *at;
    size_t dpsize;
    size_t tail;

    if (dp == NULL || (dp[0] == '.' && dp[1] == '\0')) {
        return n;
    }
    at = strstr(buf, dp);
    if (at == NULL) {
        return n;
    }
    dpsize = strlen(dp);
    tail = (size_t)n - (size_t)(at - buf) - dpsize;
    *at = '.';
    memmove(at + 1, at + dpsize, tail + 1);
    return (int)((size_t)n - (dpsize - 1));
}

void
pbw_free(void *ptr)
{
    free(ptr);
}

const char *
pbw_strerror(pbw_status status)
{
    switch (status) {
    case PBW_OK:
        return "ok";
    case PBW_ERR_INVALID:
        return "invalid serialized data";
    case PBW_ERR_NOMEM:
        return "out of memory";
    case PBW_ERR_NOT_FOUND:
        return "field not found";
    case PBW_ERR_TYPE:
        return "type mismatch";
    case PBW_ERR_LIMIT:
        return "limit exceeded";
    case PBW_ERR_REQUIRED:
        return "required field not set";
    default:
        return "unknown error";
    }
}

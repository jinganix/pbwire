/* Wire format primitives: varints, tags, little-endian fixed values, output
 * buffer. */

#include <math.h>
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

/* A word made only of bytes below 0x80 has no high bit set, on either
 * endianness, so one test covers eight bytes of an ASCII run: a long value
 * costs one load and one branch per 16 bytes instead of one per byte. The
 * rules for the non-ASCII sequences live in pbw_utf8_seq (internal.h). */
#define PBW_UTF8_HIGH 0x8080808080808080ull

int
pbw_utf8_valid(const uint8_t *data, size_t len)
{
    size_t i = 0;

    if (len == 0) {
        return 1; /* an empty buffer may carry a NULL data pointer */
    }
    while (i < len) {
        int n;

        if (data[i] >= 0x80) {
            n = pbw_utf8_seq(data + i, len - i);
            if (n == 0) {
                return 0;
            }
            i += (size_t)n;
            continue;
        }
        /* ASCII run: two words per test, then one, then the last few bytes. */
        while (i + 16 <= len) {
            uint64_t a, b;

            memcpy(&a, data + i, sizeof a);
            memcpy(&b, data + i + 8, sizeof b);
            if ((a | b) & PBW_UTF8_HIGH) {
                break;
            }
            i += 16;
        }
        while (i + 8 <= len) {
            uint64_t a;

            memcpy(&a, data + i, sizeof a);
            if (a & PBW_UTF8_HIGH) {
                break;
            }
            i += 8;
        }
        while (i < len && data[i] < 0x80) {
            i++;
        }
    }
    return 1;
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

/* Exact (a, b) = a + b with the rounding error in *e (Knuth TwoSum: no
 * branches, no FMA, exact with or without denormals as long as nothing
 * overflows to infinity). The compiler must not fuse these into an FMA:
 * there is no multiply here, so separate statements stay separate. */
static void
pbw_twosum(double a, double b, double *s, double *e)
{
    double v = a + b;
    double w = v - a;

    *s = v;
    *e = (a - (v - w)) + (b - w);
}

/* '.'-only strtod: the JSON and text grammars use '.' and never touch the
 * host locale, so neither a foreign LC_NUMERIC nor a concurrent setlocale
 * can change the result. Grammar: [space]* [+-]? (digits [. digits]? |
 * . digits) ([eE] [+-]? digits)?. End points at the first unconsumed byte
 * (the text start when nothing converts), like strtod.
 *
 * Precision: the first 25 significant digits accumulate into an exact-ish
 * (hi, lo) pair (error ~2^-100) and the decimal exponent scales it with
 * exact x10 steps, so short inputs round exactly once, like strtod.
 * Digits past the 25th shift the value by under 1e-25 relative (far below
 * half an ulp) and only cost position bookkeeping. Saturation (inf/zero)
 * is decided by integer decimal-exponent math, so DBL_MAX/DBL_MIN parse
 * exactly and gigantic exponents cannot spin the scaler. */
double
pbw_strtod_c(const char *text, char **end)
{
    const char *p = text;
    int neg = 0;
    /* Exact-ish value pair: hi + lo, error ~2^-100 relative. */
    double hi = 0.0;
    double lo = 0.0;
    long intpart = 0; /* significant digits before the '.' (all counted) */
    long lzfrac = 0;  /* zeros after the '.' before the first digit */
    long acc = 0;     /* significant digits accumulated (max 25) */
    int seendot = 0;
    int seendig = 0;
    long expval = 0;
    int expneg = 0;
    int seenexpdig = 0;
    /* Decimal exponent of the leading digit, then the scale the pair
     * still needs: both plain longs, so no float is involved. */
    long edec;
    long scale;
    long k;
    double v;

    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
        p++;
    }
    if (*p == '+' || *p == '-') {
        neg = *p == '-';
        p++;
    }
    /* strtod-compatible spellings the callers rely on: inf/nan in any case
     * and C99 hex ("0x10" for integer fields). Hex scales by powers of two,
     * which are exact. */
    if ((p[0] | 32) == 'i' && (p[1] | 32) == 'n' && (p[2] | 32) == 'f') {
        p += 3;
        if ((p[0] | 32) == 'i' && (p[1] | 32) == 'n' &&
            (p[2] | 32) == 'i' && (p[3] | 32) == 't' &&
            (p[4] | 32) == 'y') {
            p += 5;
        }
        if (end != NULL) {
            *end = (char *)p;
        }
        v = HUGE_VAL;
        return neg ? -v : v;
    }
    if ((p[0] | 32) == 'n' && (p[1] | 32) == 'a' && (p[2] | 32) == 'n') {
        /* No (n-char-seq): strtod's sequence handling differs per libc,
         * and no caller passes it. */
        if (end != NULL) {
            *end = (char *)(p + 3);
        }
        v = (double)NAN;
        return neg ? -v : v;
    }
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        const char *q = p + 2;
        uint64_t hmant = 0;
        long hfrac = 0; /* hex digits after the '.' (each is 4 bits) */
        long hdrop = 0; /* integer digits past the uint64 window */
        long hscale;
        long hexp = 0;
        int hexpneg = 0;
        int hdig = 0;
        int hdot = 0;

        for (;; q++) {
            int d;

            if (*q >= '0' && *q <= '9') {
                d = *q - '0';
            } else if ((*q | 32) >= 'a' && (*q | 32) <= 'f') {
                d = (*q | 32) - 'a' + 10;
            } else if (*q == '.' && !hdot) {
                hdot = 1;
                continue;
            } else {
                break;
            }
            hdig = 1;
            if ((hmant >> 60) != 0) {
                /* Window full: a dropped integer digit still shifts the
                 * value; a dropped fraction digit only holds position. */
                if (!hdot && hdrop < 999999L) {
                    hdrop++;
                }
                if (hdot && hfrac < 999999L) {
                    hfrac++;
                }
                continue;
            }
            if (hdot && hfrac < 999999L) {
                hfrac++;
            }
            hmant = hmant * 16u + (uint64_t)d;
        }
        if (hdig) {
            int seenp = 0;

            p = q;
            if (*p == 'p' || *p == 'P') {
                q = p + 1;
                if (*q == '+' || *q == '-') {
                    hexpneg = *q == '-';
                    q++;
                }
                while (*q >= '0' && *q <= '9') {
                    seenp = 1;
                    if (hexp < 999999L) {
                        hexp = hexp * 10L + (*q - '0');
                    }
                    q++;
                }
                if (seenp) {
                    p = q;
                }
            }
            if (end != NULL) {
                *end = (char *)p;
            }
            if (hmant == 0) {
                return neg ? -0.0 : 0.0;
            }
            hscale = (hexpneg ? -hexp : hexp) - 4L * hfrac + 4L * hdrop;
            if (hscale > 999999L) {
                hscale = 999999L;
            }
            if (hscale < -999999L) {
                hscale = -999999L;
            }
            v = (double)hmant;
            if (hscale < 0) {
                for (k = hscale; k < 0; k++) {
                    v /= 2.0;
                    if (v == 0.0) {
                        break;
                    }
                }
            } else {
                for (k = 0; k < hscale; k++) {
                    v *= 2.0;
                    if (isinf(v)) {
                        break;
                    }
                }
            }
            return neg ? -v : v;
        }
        /* else: a bare "0x" parses as decimal 0, like strtod. */
    }
    for (;; p++) {
        if (*p >= '0' && *p <= '9') {
            int d = *p - '0';
            double s1, s2, t1, t0, l10, u1, u0, w1, w0;

            seendig = 1;
            if (acc == 0 && d == 0) {
                /* Leading zeros are insignificant: only their position
                 * counts (after the dot), never their value. */
                if (seendot) {
                    lzfrac++;
                }
                continue;
            }
            if (!seendot) {
                intpart++;
            }
            if (acc >= 25) {
                /* Position already booked in Edec below; the value is
                 * settled far past half an ulp. Still consumed for end. */
                continue;
            }
            acc++;
            /* Pair digit step: (hi, lo) = (hi, lo) * 10 + d. The digit
             * joins the large term through an exact TwoSum first; the
             * tiny residuals fold at their own scale afterwards. Adding
             * them to the digit in one plain sum would round them away. */
            s1 = hi * 8.0;
            s2 = hi * 2.0;
            pbw_twosum(s1, s2, &t1, &t0);
            l10 = lo * 10.0;
            pbw_twosum(t1, l10, &u1, &u0);
            pbw_twosum(u1, (double)d, &w1, &w0);
            hi = w1;
            lo = w0 + u0 + t0;
        } else if (*p == '.' && !seendot) {
            seendot = 1;
        } else {
            break;
        }
    }
    if (!seendig) {
        if (end != NULL) {
            *end = (char *)text;
        }
        return 0.0;
    }
    if (*p == 'e' || *p == 'E') {
        const char *q = p + 1;
        int qneg = 0;

        if (*q == '+' || *q == '-') {
            qneg = *q == '-';
            q++;
        }
        while (*q >= '0' && *q <= '9') {
            seenexpdig = 1;
            /* Saturate: beyond +-999999 the scale below over/underflows
             * anyway, but every digit is still consumed for the end. */
            if (expval < 999999L) {
                expval = expval * 10L + (*q - '0');
            }
            q++;
        }
        if (seenexpdig) {
            p = q;
            expneg = qneg;
        }
        /* else: a bare 'e' (as in "1e") ends the number, like strtod. */
    }
    if (end != NULL) {
        *end = (char *)p;
    }
    if (acc == 0) {
        return neg ? -0.0 : 0.0;
    }
    /* Decimal exponent of the leading digit: exact integer math, so the
     * saturation verdicts below never depend on a rounded float. A value
     * at or past 1e309 overflows; below 1e-324 it is zero. */
    if (intpart > 0) {
        edec = (expneg ? -expval : expval) + intpart - 1;
    } else {
        edec = (expneg ? -expval : expval) - lzfrac - 1;
    }
    if (edec > 308) {
        return neg ? -HUGE_VAL : HUGE_VAL;
    }
    if (edec < -324) {
        return neg ? -0.0 : 0.0;
    }
    /* The pair holds `acc` significant digits, so shifting it by
     * Edec - acc + 1 lands on the true value. In range that shift is at
     * most ~350 steps, whatever the input looked like. */
    scale = edec - acc + 1;
    if (scale == 0) {
        v = hi + lo;
        return neg ? -v : v;
    }
    if (scale < 0) {
        for (k = scale; k < 0; k++) {
            /* Compensated /10: q1 carries the quotient, the TwoSum chain
             * folds the exact remainder back in. */
            double q1 = hi / 10.0;
            double p1 = q1 * 8.0;
            double p2 = q1 * 2.0;
            double s1, s0, d1, d0, e1, e0, l10, r, r1, rr, n1, n0;

            pbw_twosum(p1, p2, &s1, &s0);
            pbw_twosum(hi, -s1, &d1, &d0);
            pbw_twosum(d1, -s0, &e1, &e0);
            l10 = lo / 10.0;
            /* Every remainder piece scales: e1 is the large one, the
             * rest fold at their own tiny scale. Forgetting the /10 on
             * (e0 + d0) poisoned the correction by 10x. */
            r = (e0 + d0) / 10.0 + l10;
            r1 = e1 / 10.0;
            rr = r1 + r;
            pbw_twosum(q1, rr, &n1, &n0);
            hi = n1;
            lo = n0;
            if (hi == 0.0 && lo == 0.0) {
                break;
            }
        }
    } else {
        for (k = 0; k < scale; k++) {
            /* Exact x10: powers of two never round (an overflow verdict
             * stays correct: the true value overflows too). */
            double s1 = hi * 8.0;
            double s2 = hi * 2.0;
            double t1, t0, l10, u1, u0, v0, n1, n0;

            pbw_twosum(s1, s2, &t1, &t0);
            l10 = lo * 10.0;
            pbw_twosum(t1, l10, &u1, &u0);
            v0 = u0 + t0;
            pbw_twosum(u1, v0, &n1, &n0);
            hi = n1;
            lo = n0;
            /* An overflowed step poisons the pair through inf - inf =
             * NaN, so catch either and report infinity (inputs here are
             * finite, and the true value overflows with them). */
            if (!isfinite(hi)) {
                hi = HUGE_VAL;
                lo = 0.0;
                break;
            }
        }
    }
    v = hi + lo;
    return neg ? -v : v;
}

/* '.'-only %g: snprintf follows LC_NUMERIC, so a foreign locale may emit its
 * own separator. It is the one punctuation mark between the leading digits
 * and the exponent (or end): '.' in the C locale, the host separator
 * otherwise. inf/nan have no leading digits and pass through untouched. */
int
pbw_format_g_c(char *buf, size_t cap, int precision, double value)
{
    int n = snprintf(buf, cap, "%.*g", precision, value);
    char *p;
    char *digits;
    size_t seplen;

    if (n <= 0 || (size_t)n >= cap) {
        return n;
    }
    p = buf;
    if (*p == '-' || *p == '+') {
        p++;
    }
    digits = p;
    while (*p >= '0' && *p <= '9') {
        p++;
    }
    if (p == digits || *p == '.' || *p == 'e' || *p == 'E' || *p == '\0') {
        return n;
    }
    /* A foreign separator: ASCII is one byte, otherwise a UTF-8 sequence
     * whose lead byte gives the width. Anything odder (a non-UTF-8
     * multibyte) is consumed up to the next digit or exponent mark. */
    if ((*p & 0x80) == 0) {
        seplen = 1;
    } else if ((*p & 0xe0) == 0xc0) {
        seplen = 2;
    } else if ((*p & 0xf0) == 0xe0) {
        seplen = 3;
    } else if ((*p & 0xf8) == 0xf0) {
        seplen = 4;
    } else {
        seplen = 1;
        while (p[seplen] != '\0' && !(p[seplen] >= '0' && p[seplen] <= '9') &&
               p[seplen] != 'e' && p[seplen] != 'E') {
            seplen++;
        }
    }
    /* A locale whose decimal_point is a truncated UTF-8 lead byte makes the
     * inferred width longer than the bytes snprintf wrote; treating it as one
     * byte keeps the memmove length from wrapping. */
    if ((size_t)(p - buf) >= (size_t)n ||
        seplen > (size_t)n - (size_t)(p - buf)) {
        seplen = 1;
    }
    *p = '.';
    memmove(p + 1, p + seplen,
            (size_t)n - (size_t)(p - buf) - seplen + 1);
    return (int)((size_t)n - (seplen - 1));
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

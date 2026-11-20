/* cshim_codec.c - wire codec for the Go v2 probe.  See cshim_codec.h.
 *
 * Encode: one cgo call.  The loop below walks the layout rows (already in
 * ascending field-number order), reads the Go value through the row offset,
 * and appends the tag/value bytes to a Go-owned output buffer.  If the
 * buffer is too small the call stops early and Go retries with twice the
 * capacity, so the steady-state result is one allocation and no copy.
 * Nested messages need a length prefix, so a separate size pass computes
 * their payload length; that pass is a walk, not a tree.
 *
 * Decode: one cgo call.  The scan validates tags, wire types, lengths and
 * depth, then writes scalars straight into the zeroed Go struct.  Raw string
 * headers point into the caller's wire bytes; repeated fields go into
 * per-field accumulators and nested messages into a codec-owned bump arena.
 * Go copies every string into one Go arena and every repeated field into one
 * Go backing array before Unmarshal returns.  No C pointer is retained in C
 * memory and every raw pointer is dead by the time the next call starts.
 *
 * Only little-endian 64-bit hosts are targeted (amd64); the Go layout
 * offsets assume ptr==8, int==8 and little-endian fixed-width fields.
 */
#include "cshim_codec.h"

#include <stdlib.h>
#include <string.h>

#define CSHIM_MAX_DEPTH 256

/* --------------------------------------------------------------- integers */

static size_t
varint_size(uint64_t v)
{
    size_t n = 1;

    while (v >= 0x80) {
        v >>= 7;
        n++;
    }
    return n;
}

static uint8_t *
put_varint(uint8_t *p, uint64_t v)
{
    while (v >= 0x80) {
        *p++ = (uint8_t)((v & 0x7f) | 0x80);
        v >>= 7;
    }
    *p++ = (uint8_t)v;
    return p;
}

/* Bounded varint read.  Rejects a continuation that runs past end and a
 * 10th byte carrying more than the one remaining bit. */
static int
get_varint(const uint8_t *buf, size_t end, size_t *pos, uint64_t *out)
{
    uint64_t v = 0;
    unsigned shift = 0;
    size_t p = *pos;

    while (p < end && shift < 64) {
        uint8_t b = buf[p++];

        if (shift == 63 && (b & 0xfeu) != 0) {
            return -1;
        }
        v |= (uint64_t)(b & 0x7fu) << shift;
        if ((b & 0x80u) == 0) {
            *pos = p;
            *out = v;
            return 0;
        }
        shift += 7;
    }
    return -1;
}

static uint32_t
zigzag32(int32_t v)
{
    uint32_t u = (uint32_t)v;

    return (u << 1) ^ (uint32_t)(v < 0 ? 0xffffffffu : 0u);
}

static uint64_t
zigzag64(int64_t v)
{
    uint64_t u = (uint64_t)v;

    return (u << 1) ^ (uint64_t)(v < 0 ? UINT64_MAX : 0ull);
}

static uint32_t
unzigzag32(uint64_t v)
{
    uint32_t u = (uint32_t)v;

    return (u >> 1) ^ (uint32_t)(0u - (u & 1u));
}

static uint64_t
unzigzag64(uint64_t v)
{
    return (v >> 1) ^ (0ull - (v & 1ull));
}

static uint64_t
tag_value(uint32_t fnum, unsigned wire)
{
    return ((uint64_t)fnum << 3) | wire;
}

/* ---------------------------------------------------- raw memory accessors */

static uint32_t
rd_u32(const void *p)
{
    uint32_t v;

    memcpy(&v, p, 4);
    return v;
}

static uint64_t
rd_u64(const void *p)
{
    uint64_t v;

    memcpy(&v, p, 8);
    return v;
}

static const void *
rd_ptr(const void *p)
{
    void *v;

    memcpy(&v, p, sizeof(v));
    return v;
}

/* Interfaces are two words: the itab at +0, the data word at +8. */
static uintptr_t
rd_itab(const void *p)
{
    uintptr_t v;

    memcpy(&v, p, sizeof(v));
    return v;
}

static const void *
rd_data(const void *p)
{
    return rd_ptr((const uint8_t *)p + sizeof(void *));
}

/* For a set oneof member (itab already matched), the payload address: the
 * data word's value, i.e. the wrapper address. Scalars, strings and the
 * message pointer all live at wrapper offset zero. */
static const void *
oneof_payload(const uint8_t *base, const cshim_row *r)
{
    if (rd_itab(base + r->offset) != r->itab) {
        return NULL;
    }
    return rd_data(base + r->offset);
}

static intptr_t
rd_len(const void *p)
{
    intptr_t v;

    memcpy(&v, p, sizeof(v));
    return v;
}

static void
st_u32(void *p, uint32_t v)
{
    memcpy(p, &v, 4);
}

static void
st_u64(void *p, uint64_t v)
{
    memcpy(p, &v, 8);
}

static void
st_u8(void *p, uint8_t v)
{
    memcpy(p, &v, 1);
}

static void
st_ptr(void *p, void *v)
{
    memcpy(p, &v, sizeof(v));
}

static void
st_len(void *p, intptr_t v)
{
    memcpy(p, &v, sizeof(v));
}

/* ------------------------------------------------------------------ arena */

typedef struct cshim_block {
    struct cshim_block *next;
    size_t cap;
    size_t used;
    size_t pad;
    uint8_t data[];
} cshim_block;

typedef struct cshim_acc {
    uint8_t *buf;
    size_t len;
    size_t cap;
    size_t elem;
} cshim_acc;

typedef struct cshim_table_mem {
    cshim_row *rows;
    size_t nrows;
    size_t msg_size;
    const cshim_row **resolve;
    size_t maxfnum;
    cshim_acc *accs;
} cshim_table_mem;

struct cshim_codec {
    cshim_table_mem *tables;
    size_t ntables;
    size_t root;
    cshim_oneof_stage *ostages;
    size_t nstages;

    uint8_t *out;
    size_t out_len;
    size_t out_cap;
    size_t str_bytes;

    cshim_acc *accs;
    cshim_acc *sub_accs;

    cshim_block *blocks;
    cshim_block *cur;
    size_t block_cap;

    int err;
    size_t fail_off;
};

static void
arena_reset(cshim_codec *h)
{
    cshim_block *b;

    for (b = h->blocks; b != NULL; b = b->next) {
        b->used = 0;
    }
    h->cur = h->blocks;
}

static void *
arena_alloc(cshim_codec *h, size_t want)
{
    size_t n, cap;
    cshim_block *b, *last;

    if (want == 0) {
        return NULL;
    }
    if (want > SIZE_MAX - 15) {
        h->err = CSHIM_ERR_NOMEM;
        return NULL;
    }
    n = (want + 15) & ~(size_t)15;
    for (b = h->cur; b != NULL; b = b->next) {
        if (b->used <= b->cap && n <= b->cap - b->used) {
            void *p = b->data + b->used;

            h->cur = b;
            b->used += n;
            return p;
        }
    }
    /* cur is a suffix start, so nothing at or after it fits: append a
     * block.  Earlier blocks may retain a small hole; that is acceptable
     * for a per-call reset arena. */
    cap = h->block_cap != 0 ? h->block_cap : 4096;
    while (cap < n) {
        if (cap > SIZE_MAX / 2) {
            cap = n;
            break;
        }
        cap *= 2;
    }
    if (cap > SIZE_MAX - sizeof(*b)) {
        h->err = CSHIM_ERR_NOMEM;
        return NULL;
    }
    b = (cshim_block *)malloc(sizeof(*b) + cap);
    if (b == NULL) {
        h->err = CSHIM_ERR_NOMEM;
        return NULL;
    }
    b->next = NULL;
    b->cap = cap;
    b->used = n;
    b->pad = 0;
    last = h->blocks;
    if (last == NULL) {
        h->blocks = b;
    } else {
        while (last->next != NULL) {
            last = last->next;
        }
        last->next = b;
    }
    h->cur = b;
    h->block_cap = cap < SIZE_MAX / 2 ? cap * 2 : cap;
    return b->data;
}

/* -------------------------------------------------------- repeated accum */

static int
acc_push(cshim_codec *h, cshim_acc *a, const void *v)
{
    size_t elem = a->elem;

    if (elem == 0) {
        h->err = CSHIM_ERR_PLAN;
        return -1;
    }
    if (a->len == a->cap) {
        size_t cap = a->cap != 0 ? a->cap : 16;
        uint8_t *p;

        if (cap > SIZE_MAX / 2) {
            cap = a->len + 1;
        } else {
            cap *= 2;
        }
        if (cap <= a->len || cap > SIZE_MAX / elem) {
            h->err = CSHIM_ERR_NOMEM;
            return -1;
        }
        p = (uint8_t *)realloc(a->buf, cap * elem);
        if (p == NULL) {
            h->err = CSHIM_ERR_NOMEM;
            return -1;
        }
        a->buf = p;
        a->cap = cap;
    }
    memcpy(a->buf + a->len * elem, v, elem);
    a->len++;
    return 0;
}

/* -------------------------------------------------------------- encode buf */

/* The output buffer is supplied by Go and only live for this call.  There is
 * no realloc path: overflow tells Go to double its hint and retry. */
static int
out_reserve(cshim_codec *h, size_t extra)
{
    size_t need;

    if (extra > SIZE_MAX - h->out_len) {
        h->err = CSHIM_ERR_NOMEM;
        return -1;
    }
    need = h->out_len + extra;
    if (need <= h->out_cap) {
        return 0;
    }
    h->err = CSHIM_ERR_SPACE;
    return -1;
}

static uint8_t *
out_ptr(cshim_codec *h, size_t extra)
{
    if (out_reserve(h, extra)) {
        return NULL;
    }
    return h->out + h->out_len;
}

static int
wr_tag_varint(cshim_codec *h, uint32_t fnum, uint64_t v)
{
    uint64_t tag = tag_value(fnum, 0);
    size_t n = varint_size(tag) + varint_size(v);
    uint8_t *p = out_ptr(h, n);

    if (p == NULL) {
        return -1;
    }
    p = put_varint(p, tag);
    p = put_varint(p, v);
    h->out_len += n;
    return 0;
}

static int
wr_tag_fixed(cshim_codec *h, uint32_t fnum, unsigned wire, uint64_t bits)
{
    uint64_t tag = tag_value(fnum, wire);
    size_t n = varint_size(tag) + (wire == 5 ? 4 : 8);
    uint8_t *p = out_ptr(h, n);
    size_t i;

    if (p == NULL) {
        return -1;
    }
    p = put_varint(p, tag);
    for (i = 0; i < (wire == 5 ? 4u : 8u); i++) {
        p[i] = (uint8_t)(bits >> (8 * i));
    }
    h->out_len += n;
    return 0;
}

static int
wr_tag_bytes(cshim_codec *h, uint32_t fnum, const void *data, size_t len)
{
    uint64_t tag = tag_value(fnum, 2);
    size_t need = varint_size(tag) + varint_size(len) + len;
    uint8_t *p = out_ptr(h, need);

    if (p == NULL) {
        return -1;
    }
    p = put_varint(p, tag);
    p = put_varint(p, (uint64_t)len);
    if (len != 0) {
        memcpy(p, data, len);
    }
    h->out_len += need;
    return 0;
}

/* ------------------------------------------------------------------ size */

/* Bytes a nested message occupies, without allocating anything. */
static int
msg_size(cshim_codec *h, const cshim_table_mem *t, const void *base, int depth,
         size_t *out);

static int
add_size(cshim_codec *h, size_t *total, size_t add)
{
    if (add > SIZE_MAX - *total) {
        h->err = CSHIM_ERR_NOMEM;
        return -1;
    }
    *total += add;
    return 0;
}

static int
msg_size(cshim_codec *h, const cshim_table_mem *t, const void *base, int depth,
         size_t *out)
{
    size_t total = 0, i;

    if (depth > CSHIM_MAX_DEPTH) {
        h->err = CSHIM_ERR_DEPTH;
        return -1;
    }
    for (i = 0; i < t->nrows; i++) {
        const cshim_row *r = &t->rows[i];
        const uint8_t *p = (const uint8_t *)base + r->offset;
        size_t sz = 0;
        const uint8_t *vp = p;
        if (r->explicit) {
            vp = rd_ptr((const uint8_t *)base + r->has);
            if (vp == NULL) {
                continue;
            }
        } else if (r->itab != 0) {
            vp = oneof_payload(base, r);
            if (vp == NULL) {
                continue;
            }
        }

        if (r->repeated) {
            intptr_t cnt = rd_len((const uint8_t *)base + r->count);
            const uint8_t *e = (const uint8_t *)rd_ptr(
                (const uint8_t *)base + r->offset);

            if (cnt < 0) {
                h->err = CSHIM_ERR_PLAN;
                return -1;
            }
            if (cnt == 0) {
                continue;
            }
            if (e == NULL) {
                h->err = CSHIM_ERR_PLAN;
                return -1;
            }
            if (r->kind == CSHIMK_I32 || r->kind == CSHIMK_BOOL) {
                size_t payload = 0, k;
                uint64_t tag = tag_value(r->fnum, 2);

                if (r->kind == CSHIMK_I32) {
                    for (k = 0; k < (size_t)cnt; k++) {
                        int32_t v;

                        memcpy(&v, e + k * sizeof(int32_t), sizeof(v));
                        payload += varint_size((uint64_t)(int64_t)v);
                    }
                } else {
                    payload = (size_t)cnt;
                }
                sz = varint_size(tag) + varint_size(payload) + payload;
            } else if (r->kind == CSHIMK_STR || r->kind == CSHIMK_BYTES) {
                size_t k;
                uint64_t tag = tag_value(r->fnum, 2);

                for (k = 0; k < (size_t)cnt; k++) {
                    const uint8_t *sh = e + k * r->elem;
                    intptr_t sl = rd_len(sh + sizeof(void *));

                    if (sl < 0) {
                        h->err = CSHIM_ERR_PLAN;
                        return -1;
                    }
                    sz += varint_size(tag) + varint_size((size_t)sl) +
                          (size_t)sl;
                }
            } else if (r->kind == CSHIMK_MSG) {
                size_t k;
                uint64_t stag = tag_value(r->fnum, r->group ? 3u : 2u);
                uint64_t etag = tag_value(r->fnum, 4);

                for (k = 0; k < (size_t)cnt; k++) {
                    const void *sub = rd_ptr(e + k * r->elem);
                    size_t sub_size = 0;

                    if (sub != NULL &&
                        msg_size(h, &h->tables[r->sub], sub, depth + 1,
                                 &sub_size)) {
                        return -1;
                    }
                    if (r->group) {
                        sz += varint_size(stag) + sub_size + varint_size(etag);
                    } else {
                        sz += varint_size(stag) + varint_size(sub_size) + sub_size;
                    }
                }
            } else {
                h->err = CSHIM_ERR_PLAN;
                return -1;
            }
        } else {
            switch (r->kind) {
            case CSHIMK_I32: {
                int32_t v;

                memcpy(&v, vp, 4);
                if (v != 0 || r->required || r->explicit || r->itab) {
                    sz = varint_size(tag_value(r->fnum, 0)) +
                         varint_size((uint64_t)(int64_t)v);
                }
                break;
            }
            case CSHIMK_I64: {
                int64_t v;

                memcpy(&v, vp, 8);
                if (v != 0 || r->required || r->explicit || r->itab) {
                    sz = varint_size(tag_value(r->fnum, 0)) +
                         varint_size((uint64_t)v);
                }
                break;
            }
            case CSHIMK_U32: {
                uint32_t v;

                memcpy(&v, vp, 4);
                if (v != 0 || r->required || r->explicit || r->itab) {
                    sz = varint_size(tag_value(r->fnum, 0)) +
                         varint_size((uint64_t)v);
                }
                break;
            }
            case CSHIMK_U64: {
                uint64_t v;

                memcpy(&v, vp, 8);
                if (v != 0 || r->required || r->explicit || r->itab) {
                    sz = varint_size(tag_value(r->fnum, 0)) + varint_size(v);
                }
                break;
            }
            case CSHIMK_S32: {
                int32_t v;

                memcpy(&v, vp, 4);
                if (v != 0 || r->required || r->explicit || r->itab) {
                    sz = varint_size(tag_value(r->fnum, 0)) +
                         varint_size(zigzag32(v));
                }
                break;
            }
            case CSHIMK_S64: {
                int64_t v;

                memcpy(&v, vp, 8);
                if (v != 0 || r->required || r->explicit || r->itab) {
                    sz = varint_size(tag_value(r->fnum, 0)) +
                         varint_size(zigzag64(v));
                }
                break;
            }
            case CSHIMK_FIX32: {
                uint32_t v;

                memcpy(&v, vp, 4);
                if (v != 0 || r->required || r->explicit || r->itab) {
                    sz = varint_size(tag_value(r->fnum, 5)) + 4;
                }
                break;
            }
            case CSHIMK_FIX64: {
                uint64_t v;

                memcpy(&v, vp, 8);
                if (v != 0 || r->required || r->explicit || r->itab) {
                    sz = varint_size(tag_value(r->fnum, 1)) + 8;
                }
                break;
            }
            case CSHIMK_FLOAT: {
                float v;

                memcpy(&v, vp, 4);
                if (v != 0.0f || r->required || r->explicit || r->itab) {
                    sz = varint_size(tag_value(r->fnum, 5)) + 4;
                }
                break;
            }
            case CSHIMK_DOUBLE: {
                double v;

                memcpy(&v, vp, 8);
                if (v != 0.0 || r->required || r->explicit || r->itab) {
                    sz = varint_size(tag_value(r->fnum, 1)) + 8;
                }
                break;
            }
            case CSHIMK_BOOL: {
                uint8_t v;

                memcpy(&v, vp, 1);
                if (v != 0 || r->required || r->explicit || r->itab) {
                    sz = varint_size(tag_value(r->fnum, 0)) + 1;
                }
                break;
            }
            case CSHIMK_STR:
        case CSHIMK_BYTES: {
                intptr_t sl = (r->explicit || r->itab != 0) ?
                rd_len(vp + sizeof(void *)) :
                rd_len((const uint8_t *)base + r->count);

                if (sl < 0) {
                    h->err = CSHIM_ERR_PLAN;
                    return -1;
                }
                if (sl != 0 || r->required || r->explicit || r->itab) {
                    sz = varint_size(tag_value(r->fnum, 2)) +
                         varint_size((size_t)sl) + (size_t)sl;
                }
                break;
            }
            case CSHIMK_MSG: {
                const void *sub = rd_ptr(vp);
                size_t sub_size = 0;

                /* A set oneof member (itab already matched) encodes even
                 * when its payload pointer is nil: presence lives in the
                 * wrapper, so a typed-nil member is the empty message. */
                if (sub != NULL || r->required || r->itab != 0) {
                    if (sub != NULL &&
                        msg_size(h, &h->tables[r->sub], sub, depth + 1,
                                 &sub_size)) {
                        return -1;
                    }
                    if (r->group) {
                        sz = varint_size(tag_value(r->fnum, 3)) + sub_size +
                             varint_size(tag_value(r->fnum, 4));
                    } else {
                        sz = varint_size(tag_value(r->fnum, 2)) +
                             varint_size(sub_size) + sub_size;
                    }
                }
                break;
            }
            case CSHIMK_OPT_I32: {
                const void *op = rd_ptr((const uint8_t *)base + r->has);

                if (op != NULL) {
                    int32_t v;

                    memcpy(&v, op, 4);
                    sz = varint_size(tag_value(r->fnum, 0)) +
                         varint_size((uint64_t)(int64_t)v);
                }
                break;
            }
            case CSHIMK_OPT_STR: {
                const void *op = rd_ptr((const uint8_t *)base + r->has);

                if (op != NULL) {
                    intptr_t sl = rd_len((const uint8_t *)op + sizeof(void *));

                    if (sl < 0) {
                        h->err = CSHIM_ERR_PLAN;
                        return -1;
                    }
                    sz = varint_size(tag_value(r->fnum, 2)) +
                         varint_size((size_t)sl) + (size_t)sl;
                }
                break;
            }
            default:
                h->err = CSHIM_ERR_PLAN;
                return -1;
            }
        }
        if (add_size(h, &total, sz)) {
            return -1;
        }
    }
    *out = total;
    return 0;
}

/* ---------------------------------------------------------------- encoder */

static int
dec_body(cshim_codec *h, const cshim_table_mem *t, void *base, const uint8_t *buf, size_t len,
         size_t *posp, uint32_t endgroup, int depth);

static int
dec_msg(cshim_codec *h, const cshim_table_mem *t, void *base, const uint8_t *buf, size_t len,
        int depth);

static int
dec_group_body(cshim_codec *h, size_t subti, void *target, const uint8_t *buf, size_t len,
               size_t *pos, uint32_t gnum, int depth);

static int
enc_msg(cshim_codec *h, const cshim_table_mem *t, const void *base, int depth);

static int
enc_sub(cshim_codec *h, size_t subti, uint32_t fnum, const void *sub, int depth)
{
    size_t plen = 0, start;
    uint64_t tag = tag_value(fnum, 2);
    size_t head = varint_size(tag);
    uint8_t *p;

    if (sub != NULL &&
        msg_size(h, &h->tables[subti], sub, depth + 1, &plen)) {
        return -1;
    }
    head += varint_size(plen);
    start = h->out_len;
    p = out_ptr(h, head + plen);
    if (p == NULL) {
        return -1;
    }
    p = put_varint(p, tag);
    p = put_varint(p, (uint64_t)plen);
    h->out_len += head;
    if (plen != 0) {
        if (enc_msg(h, &h->tables[subti], sub, depth + 1)) {
            return -1;
        }
        if (h->out_len != start + head + plen) {
            h->err = CSHIM_ERR_PLAN;
            return -1;
        }
    }
    return 0;
}

/* Groups frame with start/end tags instead of a length prefix. The caller
 * buffer never moves (no realloc on overflow, just a NULL report), so the
 * end tag is written after the body through a fresh out_ptr. */
static int
enc_group(cshim_codec *h, size_t subti, uint32_t fnum, const void *sub, int depth)
{
    size_t blen = 0, start;
    size_t head = varint_size(tag_value(fnum, 3));
    size_t tail = varint_size(tag_value(fnum, 4));
    uint8_t *p;

    if (sub != NULL &&
        msg_size(h, &h->tables[subti], sub, depth + 1, &blen)) {
        return -1;
    }
    start = h->out_len;
    p = out_ptr(h, head + blen + tail);
    if (p == NULL) {
        return -1;
    }
    p = put_varint(p, tag_value(fnum, 3));
    h->out_len += head;
    if (blen != 0) {
        if (enc_msg(h, &h->tables[subti], sub, depth + 1)) {
            return -1;
        }
        if (h->out_len != start + head + blen) {
            h->err = CSHIM_ERR_PLAN;
            return -1;
        }
    }
    p = out_ptr(h, tail);
    if (p == NULL) {
        return -1;
    }
    p = put_varint(p, tag_value(fnum, 4));
    h->out_len += tail;
    return 0;
}

static int
enc_msg(cshim_codec *h, const cshim_table_mem *t, const void *base, int depth)
{
    size_t i;

    if (depth > CSHIM_MAX_DEPTH) {
        h->err = CSHIM_ERR_DEPTH;
        return -1;
    }
    for (i = 0; i < t->nrows; i++) {
        const cshim_row *r = &t->rows[i];
        const uint8_t *p = (const uint8_t *)base + r->offset;
        const uint8_t *vp = p;
        if (r->explicit) {
            vp = rd_ptr((const uint8_t *)base + r->has);
            if (vp == NULL) {
                continue;
            }
        } else if (r->itab != 0) {
            vp = oneof_payload(base, r);
            if (vp == NULL) {
                continue;
            }
        }

        if (r->repeated) {
            intptr_t cnt = rd_len((const uint8_t *)base + r->count);
            const uint8_t *e = (const uint8_t *)rd_ptr(p);
            size_t k;

            if (cnt < 0) {
                h->err = CSHIM_ERR_PLAN;
                return -1;
            }
            if (cnt == 0) {
                continue;
            }
            if (e == NULL) {
                h->err = CSHIM_ERR_PLAN;
                return -1;
            }
            if (r->kind == CSHIMK_I32) {
                size_t payload = 0, start, head;
                uint64_t tag = tag_value(r->fnum, 2);
                uint8_t *q;

                for (k = 0; k < (size_t)cnt; k++) {
                    int32_t v;

                    memcpy(&v, e + k * sizeof(int32_t), sizeof(v));
                    payload += varint_size((uint64_t)(int64_t)v);
                }
                head = varint_size(tag) + varint_size(payload);
                start = h->out_len;
                q = out_ptr(h, head + payload);
                if (q == NULL) {
                    return -1;
                }
                q = put_varint(q, tag);
                q = put_varint(q, (uint64_t)payload);
                for (k = 0; k < (size_t)cnt; k++) {
                    int32_t v;

                    memcpy(&v, e + k * sizeof(int32_t), sizeof(v));
                    q = put_varint(q, (uint64_t)(int64_t)v);
                }
                h->out_len = start + head + payload;
            } else if (r->kind == CSHIMK_BOOL) {
                size_t start, head;
                uint64_t tag = tag_value(r->fnum, 2);
                uint8_t *q;

                head = varint_size(tag) + varint_size((uint64_t)cnt);
                start = h->out_len;
                q = out_ptr(h, head + (size_t)cnt);
                if (q == NULL) {
                    return -1;
                }
                q = put_varint(q, tag);
                q = put_varint(q, (uint64_t)cnt);
                for (k = 0; k < (size_t)cnt; k++) {
                    q[k] = e[k] != 0 ? 1 : 0;
                }
                h->out_len = start + head + (size_t)cnt;
            } else if (r->kind == CSHIMK_STR || r->kind == CSHIMK_BYTES) {
                for (k = 0; k < (size_t)cnt; k++) {
                    const uint8_t *sh = e + k * r->elem;
                    const void *sd = rd_ptr(sh);
                    intptr_t sl = rd_len(sh + sizeof(void *));

                    if (sl < 0) {
                        h->err = CSHIM_ERR_PLAN;
                        return -1;
                    }
                    if (wr_tag_bytes(h, r->fnum, sd, (size_t)sl)) {
                        return -1;
                    }
                }
            } else if (r->kind == CSHIMK_MSG) {
                for (k = 0; k < (size_t)cnt; k++) {
                    const void *sub = rd_ptr(e + k * r->elem);

                    if (r->group) {
                        if (enc_group(h, (size_t)r->sub, r->fnum, sub, depth)) {
                            return -1;
                        }
                    } else if (enc_sub(h, (size_t)r->sub, r->fnum, sub, depth)) {
                        return -1;
                    }
                }
            } else {
                h->err = CSHIM_ERR_PLAN;
                return -1;
            }
            continue;
        }

        switch (r->kind) {
        case CSHIMK_I32: {
            int32_t v;

            memcpy(&v, vp, 4);
            if ((v != 0 || r->required || r->explicit || r->itab) &&
                wr_tag_varint(h, r->fnum, (uint64_t)(int64_t)v)) {
                return -1;
            }
            break;
        }
        case CSHIMK_I64: {
            int64_t v;

            memcpy(&v, vp, 8);
            if ((v != 0 || r->required || r->explicit || r->itab) && wr_tag_varint(h, r->fnum, (uint64_t)v)) {
                return -1;
            }
            break;
        }
        case CSHIMK_U32: {
            uint32_t v;

            memcpy(&v, vp, 4);
            if ((v != 0 || r->required || r->explicit || r->itab) && wr_tag_varint(h, r->fnum, (uint64_t)v)) {
                return -1;
            }
            break;
        }
        case CSHIMK_U64: {
            uint64_t v;

            memcpy(&v, vp, 8);
            if ((v != 0 || r->required || r->explicit || r->itab) && wr_tag_varint(h, r->fnum, v)) {
                return -1;
            }
            break;
        }
        case CSHIMK_S32: {
            int32_t v;

            memcpy(&v, vp, 4);
            if ((v != 0 || r->required || r->explicit || r->itab) && wr_tag_varint(h, r->fnum, zigzag32(v))) {
                return -1;
            }
            break;
        }
        case CSHIMK_S64: {
            int64_t v;

            memcpy(&v, vp, 8);
            if ((v != 0 || r->required || r->explicit || r->itab) && wr_tag_varint(h, r->fnum, zigzag64(v))) {
                return -1;
            }
            break;
        }
        case CSHIMK_FIX32: {
            uint32_t v;

            memcpy(&v, vp, 4);
            if ((v != 0 || r->required || r->explicit || r->itab) && wr_tag_fixed(h, r->fnum, 5, (uint64_t)v)) {
                return -1;
            }
            break;
        }
        case CSHIMK_FIX64: {
            uint64_t v;

            memcpy(&v, vp, 8);
            if ((v != 0 || r->required || r->explicit || r->itab) && wr_tag_fixed(h, r->fnum, 1, v)) {
                return -1;
            }
            break;
        }
        case CSHIMK_FLOAT: {
            float v;
            uint32_t bits;

            memcpy(&v, vp, 4);
            memcpy(&bits, p, 4);
            if ((v != 0.0f || r->required || r->explicit || r->itab) && wr_tag_fixed(h, r->fnum, 5, (uint64_t)bits)) {
                return -1;
            }
            break;
        }
        case CSHIMK_DOUBLE: {
            double v;
            uint64_t bits;

            memcpy(&v, vp, 8);
            memcpy(&bits, p, 8);
            if ((v != 0.0 || r->required || r->explicit || r->itab) && wr_tag_fixed(h, r->fnum, 1, bits)) {
                return -1;
            }
            break;
        }
        case CSHIMK_BOOL: {
            uint8_t v;

            memcpy(&v, vp, 1);
            if ((v != 0 || r->required || r->explicit || r->itab) && wr_tag_varint(h, r->fnum, (uint64_t)v)) {
                return -1;
            }
            break;
        }
        case CSHIMK_STR:
        case CSHIMK_BYTES: {
            const void *sd = rd_ptr(vp);
            intptr_t sl = (r->explicit || r->itab != 0) ?
                rd_len(vp + sizeof(void *)) :
                rd_len((const uint8_t *)base + r->count);

            if (sl < 0 || (sl != 0 && sd == NULL)) {
                h->err = CSHIM_ERR_PLAN;
                return -1;
            }
            if ((sl != 0 || r->required || r->explicit || r->itab) && wr_tag_bytes(h, r->fnum, sd, (size_t)sl)) {
                return -1;
            }
            break;
        }
        case CSHIMK_MSG: {
            const void *sub = rd_ptr(vp);

            if (r->group) {
                /* See the size pass: a matched oneof member encodes even
                 * with a nil payload, as the empty message/group. */
                if ((sub != NULL || r->required || r->itab != 0) &&
                    enc_group(h, (size_t)r->sub, r->fnum, sub, depth)) {
                    return -1;
                }
            } else if ((sub != NULL || r->required || r->itab != 0) &&
                       enc_sub(h, (size_t)r->sub, r->fnum, sub, depth)) {
                return -1;
            }
            break;
        }
        case CSHIMK_OPT_I32: {
            const void *op = rd_ptr((const uint8_t *)base + r->has);

            if (op != NULL) {
                int32_t v;

                memcpy(&v, op, 4);
                if (wr_tag_varint(h, r->fnum, (uint64_t)(int64_t)v)) {
                    return -1;
                }
            }
            break;
        }
        case CSHIMK_OPT_STR: {
            const void *op = rd_ptr((const uint8_t *)base + r->has);

            if (op != NULL) {
                const void *sd = rd_ptr(op);
                intptr_t sl = rd_len((const uint8_t *)op + sizeof(void *));

                if (sl < 0 || (sl != 0 && sd == NULL)) {
                    h->err = CSHIM_ERR_PLAN;
                    return -1;
                }
                if (wr_tag_bytes(h, r->fnum, sd, (size_t)sl)) {
                    return -1;
                }
            }
            break;
        }
        default:
            h->err = CSHIM_ERR_PLAN;
            return -1;
        }
    }
    return 0;
}

/* ---------------------------------------------------------------- decoder */

static int
skip_scalar(cshim_codec *h, const uint8_t *buf, size_t len, size_t *pos,
            unsigned wt)
{
    uint64_t v;

    if (*pos > len) {
        h->err = CSHIM_ERR_WIRE;
        return -1;
    }
    switch (wt) {
    case 0:
        if (get_varint(buf, len, pos, &v)) {
            h->err = CSHIM_ERR_WIRE;
            return -1;
        }
        return 0;
    case 1:
        if (len - *pos < 8) {
            h->err = CSHIM_ERR_WIRE;
            return -1;
        }
        *pos += 8;
        return 0;
    case 2:
        if (get_varint(buf, len, pos, &v)) {
            h->err = CSHIM_ERR_WIRE;
            return -1;
        }
        if (v > (uint64_t)(len - *pos)) {
            h->err = CSHIM_ERR_WIRE;
            return -1;
        }
        *pos += (size_t)v;
        return 0;
    case 5:
        if (len - *pos < 4) {
            h->err = CSHIM_ERR_WIRE;
            return -1;
        }
        *pos += 4;
        return 0;
    default:
        h->err = CSHIM_ERR_WIRE;
        return -1;
    }
}

static int
skip_group(cshim_codec *h, const uint8_t *buf, size_t len, size_t *pos,
           uint32_t gnum, int depth)
{
    if (depth > CSHIM_MAX_DEPTH) {
        h->err = CSHIM_ERR_DEPTH;
        return -1;
    }
    while (*pos < len) {
        uint64_t tag, fn;
        unsigned wt;

        if (get_varint(buf, len, pos, &tag)) {
            h->err = CSHIM_ERR_WIRE;
            return -1;
        }
        fn = tag >> 3;
        wt = (unsigned)(tag & 7u);
        if (fn == 0 || fn > 0x1fffffffull || wt > 5) {
            h->err = CSHIM_ERR_WIRE;
            return -1;
        }
        if (wt == 4) {
            if ((uint32_t)fn != gnum) {
                h->err = CSHIM_ERR_WIRE;
                return -1;
            }
            return 0;
        }
        if (wt == 3) {
            if (skip_group(h, buf, len, pos, (uint32_t)fn, depth + 1)) {
                return -1;
            }
        } else if (skip_scalar(h, buf, len, pos, wt)) {
            return -1;
        }
    }
    h->err = CSHIM_ERR_WIRE;
    return -1;
}

/* One set oneof member: stage (member, payload) for the Go fixup, which
 * owns all wrapper allocation. Last occurrence wins; split message
 * occurrences do not merge (documented follow-up). */
static int
dec_oneof_stage(cshim_codec *h, const cshim_row *r, const uint8_t *buf, size_t len,
                size_t *pos, int depth)
{
    cshim_oneof_stage *st;
    uint64_t v;

    if (r->ogrp >= h->nstages) {
        h->err = CSHIM_ERR_PLAN;
        return -1;
    }
    st = &h->ostages[r->ogrp];
    st->fnum = r->fnum;
    st->kind = r->kind;
    st->scalar = 0;
    st->ptr = NULL;
    st->len = 0;
    switch (r->kind) {
    case CSHIMK_I32:
        if (get_varint(buf, len, pos, &v)) {
            break;
        }
        st->scalar = (uint64_t)(int64_t)(int32_t)(uint32_t)v;
        return 0;
    case CSHIMK_I64:
    case CSHIMK_U64:
        if (get_varint(buf, len, pos, &v)) {
            break;
        }
        st->scalar = v;
        return 0;
    case CSHIMK_U32:
        if (get_varint(buf, len, pos, &v)) {
            break;
        }
        st->scalar = (uint64_t)(uint32_t)v;
        return 0;
    case CSHIMK_S32:
        if (get_varint(buf, len, pos, &v)) {
            break;
        }
        st->scalar = (uint64_t)(int64_t)(int32_t)unzigzag32(v);
        return 0;
    case CSHIMK_S64:
        if (get_varint(buf, len, pos, &v)) {
            break;
        }
        st->scalar = (uint64_t)unzigzag64(v);
        return 0;
    case CSHIMK_FIX32:
    case CSHIMK_FLOAT:
        if (len - *pos < 4) {
            break;
        }
        st->scalar = rd_u32(buf + *pos);
        *pos += 4;
        return 0;
    case CSHIMK_FIX64:
    case CSHIMK_DOUBLE:
        if (len - *pos < 8) {
            break;
        }
        st->scalar = rd_u64(buf + *pos);
        *pos += 8;
        return 0;
    case CSHIMK_BOOL:
        if (get_varint(buf, len, pos, &v)) {
            break;
        }
        st->scalar = v != 0 ? 1 : 0;
        return 0;
    case CSHIMK_STR:
    case CSHIMK_BYTES: {
        uint64_t sl;

        if (get_varint(buf, len, pos, &sl) || sl > (uint64_t)(len - *pos)) {
            break;
        }
        h->str_bytes += (size_t)sl;
        st->ptr = buf + *pos;
        st->len = (size_t)sl;
        *pos += (size_t)sl;
        return 0;
    }
    case CSHIMK_MSG: {
        uint64_t sl;
        void *sub;

        if (r->group) {
            sub = arena_alloc(h, h->tables[r->sub].msg_size);
            if (sub == NULL) {
                h->fail_off = *pos;
                return -1;
            }
            memset(sub, 0, h->tables[r->sub].msg_size);
            if (dec_group_body(h, (size_t)r->sub, sub, buf, len, pos,
                               r->fnum, depth)) {
                return -1;
            }
            st->ptr = sub;
            return 0;
        }
        if (get_varint(buf, len, pos, &sl) || sl > (uint64_t)(len - *pos)) {
            break;
        }
        sub = arena_alloc(h, h->tables[r->sub].msg_size);
        if (sub == NULL) {
            h->fail_off = *pos;
            return -1;
        }
        memset(sub, 0, h->tables[r->sub].msg_size);
        if (dec_msg(h, &h->tables[r->sub], sub, buf + *pos, (size_t)sl,
                    depth + 1)) {
            h->fail_off = *pos;
            return -1;
        }
        st->ptr = sub;
        *pos += (size_t)sl;
        return 0;
    }
    default:
        break;
    }
    h->err = CSHIM_ERR_WIRE;
    h->fail_off = *pos;
    return -1;
}

/* One repeated numeric element, already known to match the row kind. */
static int
collect_one(cshim_codec *h, const cshim_row *r, cshim_acc *a, const uint8_t *buf,
             size_t end, size_t *pos)
{
    uint64_t v;

    switch (r->kind) {
    case CSHIMK_I32: {
        int32_t x;

        if (get_varint(buf, end, pos, &v)) {
            h->err = CSHIM_ERR_WIRE;
            return -1;
        }
        x = (int32_t)(uint32_t)v;
        return acc_push(h, a, &x);
    }
    case CSHIMK_BOOL: {
        uint8_t x;

        if (get_varint(buf, end, pos, &v)) {
            h->err = CSHIM_ERR_WIRE;
            return -1;
        }
        x = v != 0 ? 1 : 0;
        return acc_push(h, a, &x);
    }
    default:
        h->err = CSHIM_ERR_PLAN;
        return -1;
    }
}

static int
dec_body(cshim_codec *h, const cshim_table_mem *t, void *base, const uint8_t *buf, size_t len,
        size_t *posp, uint32_t endgroup, int depth)
{
    size_t pos = *posp;

    if (depth > CSHIM_MAX_DEPTH) {
        h->err = CSHIM_ERR_DEPTH;
        h->fail_off = pos;
        return -1;
    }
    while (pos < len) {
        uint64_t tag, fn;
        unsigned wt;
        const cshim_row *r;

        if (get_varint(buf, len, &pos, &tag)) {
            h->err = CSHIM_ERR_WIRE;
            h->fail_off = pos;
            return -1;
        }
        fn = tag >> 3;
        wt = (unsigned)(tag & 7u);
        if (fn == 0 || fn > 0x1fffffffull || wt > 5) {
            h->err = CSHIM_ERR_WIRE;
            h->fail_off = pos;
            return -1;
        }
        if (wt == 4) {
            if (endgroup != 0 && fn == endgroup) {
                *posp = pos;
                return 0;
            }
            h->err = CSHIM_ERR_WIRE;
            h->fail_off = pos;
            return -1;
        }
        r = fn <= t->maxfnum ? t->resolve[fn] : NULL;
        if (r == NULL) {
            if (wt == 3) {
                if (skip_group(h, buf, len, &pos, (uint32_t)fn, depth + 1)) {
                    h->fail_off = pos;
                    return -1;
                }
            } else if (skip_scalar(h, buf, len, &pos, wt)) {
                h->fail_off = pos;
                return -1;
            }
            continue;
        }
        if (r->itab != 0) {
            if (dec_oneof_stage(h, r, buf, len, &pos, depth)) {
                h->fail_off = pos;
                return -1;
            }
            continue;
        }

        if (r->repeated) {
            cshim_acc *a = &t->accs[r - t->rows];

            if (r->kind == CSHIMK_STR || r->kind == CSHIMK_BYTES) {
                uint64_t sl;
                struct {
                    void *data;
                    intptr_t len;
                } rh;

                if (wt != 2) {
                    h->err = CSHIM_ERR_WIRE;
                    h->fail_off = pos;
                    return -1;
                }
                if (get_varint(buf, len, &pos, &sl) ||
                    sl > (uint64_t)(len - pos)) {
                    h->err = CSHIM_ERR_WIRE;
                    h->fail_off = pos;
                    return -1;
                }
                h->str_bytes += (size_t)sl;
                rh.data = (void *)(buf + pos);
                rh.len = (intptr_t)sl;
                if (acc_push(h, a, &rh)) {
                    h->fail_off = pos;
                    return -1;
                }
                pos += (size_t)sl;
            } else if (r->kind == CSHIMK_MSG) {
                void *sub;
                void *slot;

                if (r->group) {
                    if (wt != 3) {
                        h->err = CSHIM_ERR_WIRE;
                        h->fail_off = pos;
                        return -1;
                    }
                    sub = arena_alloc(h, h->tables[r->sub].msg_size);
                    if (sub == NULL) {
                        h->fail_off = pos;
                        return -1;
                    }
                    memset(sub, 0, h->tables[r->sub].msg_size);
                    if (dec_group_body(h, (size_t)r->sub, sub, buf, len, &pos,
                                       r->fnum, depth)) {
                        h->fail_off = pos;
                        return -1;
                    }
                    slot = sub;
                    if (acc_push(h, a, &slot)) {
                        h->fail_off = pos;
                        return -1;
                    }
                    continue;
                }
                {
                    uint64_t sl;

                    if (wt != 2) {
                        h->err = CSHIM_ERR_WIRE;
                        h->fail_off = pos;
                        return -1;
                    }
                    if (get_varint(buf, len, &pos, &sl) ||
                        sl > (uint64_t)(len - pos)) {
                        h->err = CSHIM_ERR_WIRE;
                        h->fail_off = pos;
                        return -1;
                    }
                    sub = arena_alloc(h, h->tables[r->sub].msg_size);
                    if (sub == NULL) {
                        h->fail_off = pos;
                        return -1;
                    }
                    memset(sub, 0, h->tables[r->sub].msg_size);
                    if (dec_msg(h, &h->tables[r->sub], sub, buf + pos,
                                (size_t)sl, depth + 1)) {
                        h->fail_off = pos;
                        return -1;
                    }
                    slot = sub;
                    if (acc_push(h, a, &slot)) {
                        h->fail_off = pos;
                        return -1;
                    }
                    pos += (size_t)sl;
                }
            } else {
                if (wt == 2) {
                    uint64_t plen;
                    size_t end;

                    if (get_varint(buf, len, &pos, &plen) ||
                        plen > (uint64_t)(len - pos)) {
                        h->err = CSHIM_ERR_WIRE;
                        h->fail_off = pos;
                        return -1;
                    }
                    end = pos + (size_t)plen;
                    while (pos < end) {
                        if (collect_one(h, r, a, buf, end, &pos)) {
                            h->fail_off = pos;
                            return -1;
                        }
                    }
                } else if (wt == r->wire) {
                    if (collect_one(h, r, a, buf, len, &pos)) {
                        h->fail_off = pos;
                        return -1;
                    }
                } else {
                    h->err = CSHIM_ERR_WIRE;
                    h->fail_off = pos;
                    return -1;
                }
            }
            continue;
        }

        if (wt != r->wire) {
            h->err = CSHIM_ERR_WIRE;
            h->fail_off = pos;
            return -1;
        }

        uint8_t *dp = (uint8_t *)base + r->offset;
        if (r->explicit) {
            void *slot = (void *)rd_ptr((uint8_t *)base + r->has);
            if (slot == NULL) {
                slot = arena_alloc(h, r->elem);
                if (slot == NULL) {
                    h->fail_off = pos;
                    return -1;
                }
                memset(slot, 0, r->elem);
                st_ptr((uint8_t *)base + r->has, slot);
            }
            dp = slot;
        }

        switch (r->kind) {
        case CSHIMK_I32: {
            uint64_t v;

            if (get_varint(buf, len, &pos, &v)) {
                goto badwire;
            }
            st_u32(dp, (uint32_t)v);
            break;
        }
        case CSHIMK_I64: {
            uint64_t v;

            if (get_varint(buf, len, &pos, &v)) {
                goto badwire;
            }
            st_u64(dp, v);
            break;
        }
        case CSHIMK_U32: {
            uint64_t v;

            if (get_varint(buf, len, &pos, &v)) {
                goto badwire;
            }
            st_u32(dp, (uint32_t)v);
            break;
        }
        case CSHIMK_U64: {
            uint64_t v;

            if (get_varint(buf, len, &pos, &v)) {
                goto badwire;
            }
            st_u64(dp, v);
            break;
        }
        case CSHIMK_S32: {
            uint64_t v;

            if (get_varint(buf, len, &pos, &v)) {
                goto badwire;
            }
            st_u32(dp, unzigzag32(v));
            break;
        }
        case CSHIMK_S64: {
            uint64_t v;

            if (get_varint(buf, len, &pos, &v)) {
                goto badwire;
            }
            st_u64(dp, unzigzag64(v));
            break;
        }
        case CSHIMK_FIX32:
        case CSHIMK_FLOAT: {
            size_t avail = len - pos;

            if (avail < 4) {
                goto badwire;
            }
            st_u32(dp, rd_u32(buf + pos));
            pos += 4;
            break;
        }
        case CSHIMK_FIX64:
        case CSHIMK_DOUBLE: {
            size_t avail = len - pos;

            if (avail < 8) {
                goto badwire;
            }
            st_u64(dp, rd_u64(buf + pos));
            pos += 8;
            break;
        }
        case CSHIMK_BOOL: {
            uint64_t v;

            if (get_varint(buf, len, &pos, &v)) {
                goto badwire;
            }
            st_u8(dp, v != 0 ? 1 : 0);
            break;
        }
        case CSHIMK_STR:
        case CSHIMK_BYTES: {
            uint64_t sl;

            if (get_varint(buf, len, &pos, &sl) ||
                sl > (uint64_t)(len - pos)) {
                goto badwire;
            }
            h->str_bytes += (size_t)sl;
            st_ptr(dp, (void *)(buf + pos));
            st_len(r->explicit ? dp + sizeof(void *) :
                                  (uint8_t *)base + r->count,
                   (intptr_t)sl);
            pos += (size_t)sl;
            break;
        }
        case CSHIMK_MSG: {
            if (r->group) {
                void *sub = (void *)rd_ptr(dp);

                if (sub == NULL) {
                    sub = arena_alloc(h, h->tables[r->sub].msg_size);
                    if (sub == NULL) {
                        h->fail_off = pos;
                        return -1;
                    }
                    memset(sub, 0, h->tables[r->sub].msg_size);
                    st_ptr(dp, sub);
                }
                if (dec_group_body(h, (size_t)r->sub, sub, buf, len, &pos,
                                   r->fnum, depth)) {
                    h->fail_off = pos;
                    return -1;
                }
            } else {
                uint64_t sl;
                void *sub;

                if (get_varint(buf, len, &pos, &sl) ||
                    sl > (uint64_t)(len - pos)) {
                    goto badwire;
                }
                sub = arena_alloc(h, h->tables[r->sub].msg_size);
                if (sub == NULL) {
                    h->fail_off = pos;
                    return -1;
                }
                memset(sub, 0, h->tables[r->sub].msg_size);
                if (dec_msg(h, &h->tables[r->sub], sub, buf + pos, (size_t)sl,
                            depth + 1)) {
                    h->fail_off = pos;
                    return -1;
                }
                st_ptr(dp, sub);
                pos += (size_t)sl;
            }
            break;
        }
        case CSHIMK_OPT_I32: {
            uint64_t v;
            const void *op;

            if (get_varint(buf, len, &pos, &v)) {
                goto badwire;
            }
            op = rd_ptr((uint8_t *)base + r->has);
            if (op == NULL) {
                op = arena_alloc(h, 4);
                if (op == NULL) {
                    h->fail_off = pos;
                    return -1;
                }
                st_ptr((uint8_t *)base + r->has, (void *)op);
            }
            st_u32((void *)op, (uint32_t)v);
            break;
        }
        case CSHIMK_OPT_STR: {
            uint64_t sl;
            const void *hp;

            if (get_varint(buf, len, &pos, &sl) ||
                sl > (uint64_t)(len - pos)) {
                goto badwire;
            }
            hp = rd_ptr((uint8_t *)base + r->has);
            if (hp == NULL) {
                hp = arena_alloc(h, sizeof(void *) + sizeof(intptr_t));
                if (hp == NULL) {
                    h->fail_off = pos;
                    return -1;
                }
                st_ptr((uint8_t *)base + r->has, (void *)hp);
            }
            h->str_bytes += (size_t)sl;
            st_ptr((void *)hp, (void *)(buf + pos));
            st_len((uint8_t *)hp + sizeof(void *), (intptr_t)sl);
            pos += (size_t)sl;
            break;
        }
        default:
            h->err = CSHIM_ERR_PLAN;
            h->fail_off = pos;
            return -1;
        }
        continue;

    badwire:
        h->err = CSHIM_ERR_WIRE;
        h->fail_off = pos;
        return -1;
    }
    if (endgroup != 0) {
        h->err = CSHIM_ERR_WIRE;
        h->fail_off = pos;
        return -1;
    }
    *posp = pos;
    return 0;
}

/* Length-delimited message: fresh cursor over the sub-slice. */
static int
dec_msg(cshim_codec *h, const cshim_table_mem *t, void *base, const uint8_t *buf, size_t len,
        int depth)
{
    size_t pos = 0;

    return dec_body(h, t, base, buf, len, &pos, 0, depth);
}

/* Group body: the same field loop over the caller's buffer, terminated by
 * the matching end tag instead of the buffer end. */
static int
dec_group_body(cshim_codec *h, size_t subti, void *target, const uint8_t *buf, size_t len,
               size_t *pos, uint32_t gnum, int depth)
{
    return dec_body(h, &h->tables[subti], target, buf, len, pos, gnum, depth + 1);
}

/* --------------------------------------------------------------- plan init */

static void
free_codec(cshim_codec *h)
{
    size_t i;
    cshim_block *b, *next;

    if (h == NULL) {
        return;
    }
    for (i = 0; i < h->ntables; i++) {
        size_t j;

        for (j = 0; j < h->tables[i].nrows; j++) {
            free(h->tables[i].accs[j].buf);
        }
        free(h->tables[i].accs);
        free(h->tables[i].resolve);
        free(h->tables[i].rows);
    }
    free(h->ostages);
    for (b = h->blocks; b != NULL; b = next) {
        next = b->next;
        free(b);
    }
    free(h->tables);
    free(h);
}

static int
width_of(uint32_t kind)
{
    switch (kind) {
    case CSHIMK_I32:
    case CSHIMK_U32:
    case CSHIMK_S32:
    case CSHIMK_FIX32:
    case CSHIMK_FLOAT:
        return 4;
    case CSHIMK_I64:
    case CSHIMK_U64:
    case CSHIMK_S64:
    case CSHIMK_FIX64:
    case CSHIMK_DOUBLE:
        return 8;
    case CSHIMK_BOOL:
        return 1;
    default:
        return 0;
    }
}

/* The plan table is generated by the Go half; these checks make a bad row
 * fail once at New time instead of writing through a wrong offset later.
 * Sub-table links need the whole registry, so check_registry verifies them
 * after every table passes its shape checks here. */
static int
check_table(const cshim_row *rows, size_t n, size_t struct_size, int is_sub)
{
    size_t i;
    size_t ptr = sizeof(void *);

    if (rows == NULL || n == 0 || struct_size == 0) {
        return -1;
    }
    for (i = 0; i < n; i++) {
        const cshim_row *r = &rows[i];
        uint32_t need = 0;

        if (r->fnum == 0 || (i != 0 && rows[i - 1].fnum >= r->fnum)) {
            return -1;
        }
        if (r->kind > CSHIMK_BYTES) {
            return -1;
        }
        if (r->wire != 0 && r->wire != 1 && r->wire != 2 && r->wire != 5 &&
            r->wire != 3) {
            return -1;
        }
        if (is_sub && r->repeated) {
            return -1;
        }
        if (r->group && (r->kind != CSHIMK_MSG || r->explicit)) {
            return -1;
        }
        if ((r->wire == 3) != (r->group != 0)) {
            return -1;
        }
        if (r->itab != 0) {
            int w;

            if (r->repeated || r->explicit || r->required || r->has != 0 ||
                r->count != 0) {
                return -1;
            }
            if ((uint64_t)r->offset + 2 * ptr > struct_size) {
                return -1;
            }
            /* Oneof members live in one interface word; the payload
             * shape rides the kind, so check elem here and skip the
             * flat-scalar shape section below (its count != 0 rule
             * is for plain strings, not interface members). */
            if (r->kind == CSHIMK_STR || r->kind == CSHIMK_BYTES) {
                if (r->elem != 2 * ptr) {
                    return -1;
                }
            } else if (r->kind == CSHIMK_MSG) {
                if (r->sub < 0) {
                    return -1;
                }
            } else {
                w = width_of(r->kind);
                if (w == 0 || r->elem != (uint32_t)w) {
                    return -1;
                }
            }
            continue;
        }
        if (r->ogrp != 0) {
            return -1;
        }
        if (r->required && (r->repeated || r->explicit)) {
            return -1;
        }
        if (r->explicit) {
            int w;

            if (r->repeated || r->kind == CSHIMK_MSG ||
                r->kind == CSHIMK_OPT_I32 || r->kind == CSHIMK_OPT_STR) {
                return -1;
            }
            if (r->kind == CSHIMK_STR || r->kind == CSHIMK_BYTES) {
                need = (uint32_t)(2 * ptr);
                if (r->elem != 2 * ptr) {
                    return -1;
                }
            } else {
                w = width_of(r->kind);
                if (w == 0 || r->elem != (uint32_t)w) {
                    return -1;
                }
                need = (uint32_t)w;
            }
            if (r->count != 0 ||
                (uint64_t)r->has + ptr > struct_size ||
                (uint64_t)r->offset + ptr > struct_size) {
                return -1;
            }
            need = (uint32_t)ptr;
        } else if (r->repeated) {
            if (r->kind == CSHIMK_I32) {
                need = (uint32_t)(3 * ptr);
                if (r->elem != 4) {
                    return -1;
                }
            } else if (r->kind == CSHIMK_BOOL) {
                need = (uint32_t)(3 * ptr);
                if (r->elem != 1) {
                    return -1;
                }
            } else if (r->kind == CSHIMK_STR || r->kind == CSHIMK_BYTES) {
                need = (uint32_t)(3 * ptr);
                if (r->elem != 2 * ptr) {
                    return -1;
                }
            } else if (r->kind == CSHIMK_MSG) {
                need = (uint32_t)(3 * ptr);
                if (r->elem != ptr) {
                    return -1;
                }
            } else {
                return -1;
            }
            if (r->count != r->offset + (uint32_t)ptr) {
                return -1;
            }
        } else if (r->kind == CSHIMK_STR || r->kind == CSHIMK_BYTES) {
            need = (uint32_t)(2 * ptr);
            if (r->elem != 2 * ptr) {
                return -1;
            }
            if (r->count != r->offset + (uint32_t)ptr) {
                return -1;
            }
        } else if (r->kind == CSHIMK_MSG) {
            need = (uint32_t)ptr;
        } else if (r->kind == CSHIMK_OPT_I32) {
            need = (uint32_t)ptr;
            if (r->required || r->has != r->offset || r->count != 0 || r->elem != 4) {
                return -1;
            }
        } else if (r->kind == CSHIMK_OPT_STR) {
            need = (uint32_t)ptr;
            if (r->required || r->has != r->offset || r->count != 0 ||
                r->elem != 2 * ptr) {
                return -1;
            }
        } else {
            int w = width_of(r->kind);

            if (w == 0 || r->elem != (uint32_t)w || r->count != 0 ||
                r->has != 0) {
                return -1;
            }
            need = (uint32_t)w;
        }
        if ((uint64_t)r->offset + need > struct_size) {
            return -1;
        }
    }
    return 0;
}

/* Registry-wide checks after every table passes its shape: sub indexes
 * resolve, message rows carry their sub-table's struct size, and only
 * message rows name a sub-table. */
static int
check_registry(const cshim_table *tables, size_t ntables, size_t root)
{
    size_t i, j;

    if (tables == NULL || ntables == 0 || root >= ntables) {
        return -1;
    }
    for (i = 0; i < ntables; i++) {
        if (check_table(tables[i].rows, tables[i].nrows, tables[i].msg_size,
                        (int)(i != root))) {
            return -1;
        }
    }
    for (i = 0; i < ntables; i++) {
        for (j = 0; j < tables[i].nrows; j++) {
            const cshim_row *r = &tables[i].rows[j];

            if (r->kind == CSHIMK_MSG) {
                if (r->sub < 0 || (size_t)r->sub >= ntables) {
                    return -1;
                }
                if (!r->repeated &&
                    r->elem != (uint32_t)tables[r->sub].msg_size) {
                    return -1;
                }
            } else if (r->sub != -1) {
                return -1;
            }
        }
    }
    return 0;
}

int
cshim_codec_new(cshim_codec **out, const cshim_table *tables, size_t ntables,
                size_t root)
{
    cshim_codec *h;
    size_t i, j;

    if (out == NULL) {
        return CSHIM_ERR_PLAN;
    }
    *out = NULL;
    if (check_registry(tables, ntables, root)) {
        return CSHIM_ERR_PLAN;
    }
    h = (cshim_codec *)calloc(1, sizeof(*h));
    if (h == NULL) {
        return CSHIM_ERR_NOMEM;
    }
    for (i = 0; i < ntables; i++) {
        size_t k;

        for (k = 0; k < tables[i].nrows; k++) {
            if (tables[i].rows[k].itab != 0 &&
                tables[i].rows[k].ogrp >= h->nstages) {
                h->nstages = (size_t)tables[i].rows[k].ogrp + 1;
            }
        }
    }
    if (h->nstages != 0) {
        h->ostages =
            (cshim_oneof_stage *)calloc(h->nstages, sizeof(*h->ostages));
        if (h->ostages == NULL) {
            free_codec(h);
            return CSHIM_ERR_NOMEM;
        }
    }
    h->tables = (cshim_table_mem *)calloc(ntables, sizeof(*h->tables));
    if (h->tables == NULL) {
        free_codec(h);
        return CSHIM_ERR_NOMEM;
    }
    h->ntables = ntables;
    h->root = root;
    for (i = 0; i < ntables; i++) {
        cshim_table_mem *t = &h->tables[i];

        t->nrows = tables[i].nrows;
        t->msg_size = tables[i].msg_size;
        t->rows = (cshim_row *)malloc(t->nrows * sizeof(*t->rows));
        t->accs = (cshim_acc *)calloc(t->nrows, sizeof(*t->accs));
        t->resolve = NULL;
        t->maxfnum = 0;
        if (t->rows == NULL || t->accs == NULL) {
            free_codec(h);
            return CSHIM_ERR_NOMEM;
        }
        memcpy(t->rows, tables[i].rows, t->nrows * sizeof(*t->rows));
        for (j = 0; j < t->nrows; j++) {
            if (t->rows[j].fnum > t->maxfnum) {
                t->maxfnum = t->rows[j].fnum;
            }
        }
        t->resolve =
            (const cshim_row **)calloc(t->maxfnum + 1, sizeof(*t->resolve));
        if (t->resolve == NULL) {
            free_codec(h);
            return CSHIM_ERR_NOMEM;
        }
        for (j = 0; j < t->nrows; j++) {
            t->resolve[t->rows[j].fnum] = &t->rows[j];
            t->accs[j].elem = t->rows[j].elem;
        }
    }
    *out = h;
    return CSHIM_OK;
}

void
cshim_codec_free(cshim_codec *h)
{
    free_codec(h);
}

/* ------------------------------------------------------------- entry points */

cshim_enc_result
cshim_encode(cshim_codec *h, uintptr_t base, uint8_t *out, size_t cap)
{
    cshim_enc_result res = { CSHIM_ERR_PLAN, 0 };

    if (h == NULL || base == 0 || (out == NULL && cap != 0)) {
        return res;
    }
    h->err = CSHIM_OK;
    h->out = out;
    h->out_cap = cap;
    h->out_len = 0;
    if (enc_msg(h, &h->tables[h->root], (const void *)base, 0)) {
        res.status = h->err;
    } else {
        res.status = CSHIM_OK;
        res.len = h->out_len;
    }
    /* Do not leave a Go pointer in C memory after this call. */
    h->out = NULL;
    h->out_cap = 0;
    return res;
}

cshim_dec_result
cshim_decode(cshim_codec *h, uintptr_t base, const uint8_t *wire, size_t wire_len)
{
    cshim_dec_result res = { CSHIM_ERR_PLAN, 0, 0 };
    size_t i;

    if (h == NULL || base == 0 || (wire == NULL && wire_len != 0)) {
        return res;
    }
    h->err = CSHIM_OK;
    h->fail_off = 0;
    h->str_bytes = 0;
    arena_reset(h);
    for (i = 0; i < h->ntables; i++) {
        size_t j;

        for (j = 0; j < h->tables[i].nrows; j++) {
            h->tables[i].accs[j].len = 0;
        }
    }
    for (i = 0; i < h->nstages; i++) {
        h->ostages[i].fnum = 0;
    }
    if (dec_msg(h, &h->tables[h->root], (void *)base,
                wire, wire_len, 0)) {
        res.status = h->err;
        res.fail_off = h->fail_off;
        return res;
    }
    /* Publish the accumulator buffers as raw Go slice headers.  Go copies
     * the elements immediately after the call; the next decode resets the
     * accumulators before writing them again. */
    for (i = 0; i < h->tables[h->root].nrows; i++) {
        const cshim_row *r = &h->tables[h->root].rows[i];
        cshim_acc *a = &h->tables[h->root].accs[i];
        uint8_t *hdr;
        void *data;
        intptr_t len;

        if (!r->repeated) {
            continue;
        }
        hdr = (uint8_t *)base + r->offset;
        data = a->len != 0 ? (void *)a->buf : NULL;
        len = (intptr_t)a->len;
        st_ptr(hdr, data);
        st_len(hdr + sizeof(void *), len);
        st_len(hdr + 2 * sizeof(void *), len);
    }
    res.status = CSHIM_OK;
    res.str_bytes = h->str_bytes;
    return res;
}

int
cshim_stage(const cshim_codec *h, size_t idx, uint32_t *fnum, uint32_t *kind,
            uint64_t *scalar, uintptr_t *ptr, size_t *len)
{
    const cshim_oneof_stage *st;

    if (h == NULL || fnum == NULL || kind == NULL || scalar == NULL ||
        ptr == NULL || len == NULL || idx >= h->nstages) {
        return -1;
    }
    st = &h->ostages[idx];
    *fnum = st->fnum;
    *kind = st->kind;
    *scalar = st->scalar;
    *ptr = (uintptr_t)st->ptr;
    *len = st->len;
    return 0;
}

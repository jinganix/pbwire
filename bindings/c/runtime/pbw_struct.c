/* Generic struct codec: the C binding runtime. See pbw_struct.h.
 *
 * Direct: the encoder walks a compiled plan and appends wire bytes, the
 * decoder reads the wire and stores into the struct. No pbw_message is built
 * anywhere. The dynamic core pays one allocation per value plus a second
 * traversal for the tree; a struct codec has the bytes at hand, so skipping
 * the tree is what makes it competitive with generated code (this is the same
 * design the Erlang binding uses in c_src/ep_encoder.c / ep_decoder.c).
 *
 * The plan is what keeps a *generic* engine close to straight-line generated
 * code: pbw_struct_codec_init resolves the schema once and precomputes, per
 * field, an opcode (which value transform), the tag bytes, offsets and the
 * sub-message plan. Encode and decode then dispatch on one small opcode per
 * field instead of re-deriving wire facts and re-computing tags every time.
 *
 * The schema is the truth for field numbers, wire types and proto2/proto3
 * semantics; the layout only answers "where in memory". Every schema field
 * must have a layout row (a missing row is PBW_ERR_INVALID, so a wrong field
 * number fails loudly instead of dropping data silently).
 *
 * Memory: encode borrows the struct and writes into a caller buffer. Decode
 * either mallocs every value (release with pbw_struct_free) or bump-allocates
 * the whole tree from a chunked arena that never moves a handed-out pointer,
 * so a steady-state decode loop allocates nothing. Pure C99, no dependencies
 * beyond pbwire itself.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pbw_struct.h"

/* Same cap as the core's PBW_MAX_DEPTH (src/internal.h, not public). */
#define SC_MAX_DEPTH 256

/* ------------------------------------------------------------------ wire */

static size_t
varint_size(uint64_t value)
{
    size_t n = 1;

    while (value >= 0x80) {
        value >>= 7;
        n++;
    }
    return n;
}

static uint8_t *
put_varint(uint8_t *out, uint64_t value)
{
    while (value >= 0x80) {
        *out++ = (uint8_t)((value & 0x7f) | 0x80);
        value >>= 7;
    }
    *out++ = (uint8_t)value;
    return out;
}

static uint32_t
zigzag32(uint32_t value)
{
    return (value << 1) ^ (uint32_t)(0u - (value >> 31));
}

static uint64_t
zigzag64(uint64_t value)
{
    return (value << 1) ^ (0ull - (value >> 63));
}

/* Mask first: an overlong varint must truncate like the reference. */
static uint32_t
unzigzag32(uint64_t value)
{
    uint32_t u = (uint32_t)value;

    return (u >> 1) ^ (uint32_t)(0u - (u & 1));
}

static uint64_t
unzigzag64(uint64_t value)
{
    return (value >> 1) ^ (0ull - (value & 1));
}

static unsigned
wire_type_of(pbw_type type)
{
    switch (type) {
    case PBW_TYPE_DOUBLE:
    case PBW_TYPE_FIXED64:
    case PBW_TYPE_SFIXED64:
        return 1;
    case PBW_TYPE_FLOAT:
    case PBW_TYPE_FIXED32:
    case PBW_TYPE_SFIXED32:
        return 5;
    case PBW_TYPE_STRING:
    case PBW_TYPE_BYTES:
        return 2;
    case PBW_TYPE_GROUP:
        return 3;
    case PBW_TYPE_MESSAGE:
        return 2;
    default:
        return 0;
    }
}

static int
packable(pbw_type type)
{
    return type != PBW_TYPE_STRING && type != PBW_TYPE_BYTES &&
           type != PBW_TYPE_MESSAGE && type != PBW_TYPE_GROUP;
}

static uint64_t
tag_value(uint32_t fnum, unsigned wt)
{
    return ((uint64_t)fnum << 3) | wt;
}

/* ---------------------------------------------------------------- layout */

/* Layout kind that can store a given wire type. No type maps to two kinds,
 * so this doubles as the "kind and type agree" check. */
static const uint8_t expected_kind[PBW_TYPE_SINT64 + 1] = {
    [PBW_TYPE_DOUBLE] = PBW_SF_DOUBLE,
    [PBW_TYPE_FLOAT] = PBW_SF_FLOAT,
    [PBW_TYPE_INT64] = PBW_SF_INT64,
    [PBW_TYPE_UINT64] = PBW_SF_UINT64,
    [PBW_TYPE_INT32] = PBW_SF_INT32,
    [PBW_TYPE_FIXED64] = PBW_SF_UINT64,
    [PBW_TYPE_FIXED32] = PBW_SF_UINT32,
    [PBW_TYPE_BOOL] = PBW_SF_BOOL,
    [PBW_TYPE_STRING] = PBW_SF_STRING,
    [PBW_TYPE_BYTES] = PBW_SF_BYTES,
    [PBW_TYPE_GROUP] = PBW_SF_MESSAGE,
    [PBW_TYPE_MESSAGE] = PBW_SF_MESSAGE,
    [PBW_TYPE_UINT32] = PBW_SF_UINT32,
    [PBW_TYPE_ENUM] = PBW_SF_ENUM,
    [PBW_TYPE_SFIXED32] = PBW_SF_INT32,
    [PBW_TYPE_SFIXED64] = PBW_SF_INT64,
    [PBW_TYPE_SINT32] = PBW_SF_INT32,
    [PBW_TYPE_SINT64] = PBW_SF_INT64,
};

/* Schema field type accepted for a layout kind. */
static int
kind_accepts(pbw_struct_kind_t kind, pbw_type type)
{
    switch (kind) {
    case PBW_SF_INT32:
        return type == PBW_TYPE_INT32 || type == PBW_TYPE_SINT32 ||
               type == PBW_TYPE_SFIXED32;
    case PBW_SF_UINT32:
        return type == PBW_TYPE_UINT32 || type == PBW_TYPE_FIXED32;
    case PBW_SF_INT64:
        return type == PBW_TYPE_INT64 || type == PBW_TYPE_SINT64 ||
               type == PBW_TYPE_SFIXED64;
    case PBW_SF_UINT64:
        return type == PBW_TYPE_UINT64 || type == PBW_TYPE_FIXED64;
    case PBW_SF_BOOL:
        return type == PBW_TYPE_BOOL;
    case PBW_SF_FLOAT:
        return type == PBW_TYPE_FLOAT;
    case PBW_SF_DOUBLE:
        return type == PBW_TYPE_DOUBLE;
    case PBW_SF_ENUM:
        return type == PBW_TYPE_ENUM;
    case PBW_SF_STRING:
        return type == PBW_TYPE_STRING;
    case PBW_SF_BYTES:
        return type == PBW_TYPE_BYTES;
    case PBW_SF_MESSAGE:
        return type == PBW_TYPE_MESSAGE || type == PBW_TYPE_GROUP;
    }
    return 0;
}

/* Everything the codec needs about one field, without touching the schema
 * when the layout row carries it (generated tables do; hand-written tables
 * written before those members existed fall back to the schema). */
typedef struct {
    pbw_type type;
    unsigned wire;
    int packed;
    int required;
} rinfo;

static int
row_info(const pbw_struct_field_t *row, const pbw_descriptor *desc, rinfo *out)
{
    if (row->type != 0) {
        if (expected_kind[row->type] != (uint8_t)row->kind ||
            row->wire != wire_type_of((pbw_type)row->type) ||
            (row->repeated != 0) != (row->count != 0)) {
            return 0;
        }
        out->type = (pbw_type)row->type;
        out->wire = row->wire;
        out->packed = row->packed != 0;
        out->required = row->required != 0;
        return 1;
    }
    if (desc != NULL) {
        const pbw_field *field = pbw_descriptor_find_field(desc, row->fnum);

        if (field != NULL && kind_accepts(row->kind, pbw_field_type(field))) {
            out->type = pbw_field_type(field);
            out->wire = wire_type_of(out->type);
            out->packed = pbw_field_is_packed(field) != 0;
            out->required = pbw_field_label(field) == PBW_LABEL_REQUIRED;
            return 1;
        }
    }
    return 0;
}

/* --------------------------------------------------------------- plan */

/* One opcode per field: which value transform the encoder/decoder applies.
 * The flag bits carry the per-field shape, so the hot loops branch on the
 * opcode alone. */
enum {
    SC_OP_I32 = 0, /* int32, enum: sign-extend 32 bits into a varint */
    SC_OP_I64,
    SC_OP_U32,
    SC_OP_U64,
    SC_OP_S32, /* sint32: zigzag */
    SC_OP_S64,
    SC_OP_BOOL,
    SC_OP_F32, /* 4-byte little-endian: float, fixed32, sfixed32 */
    SC_OP_F64, /* 8-byte little-endian: double, fixed64, sfixed64 */
    SC_OP_STR,
    SC_OP_BYTES, /* length-counted bytes, may contain NUL */
    SC_OP_GRP,   /* legacy group: START_GROUP + message + END_GROUP */
    SC_OP_MSG
};

#define SC_OP_MASK 0x0fu
#define SC_DFLT 0x10u /* singular enum with a non-zero default to seed */
#define SC_RPT 0x20u  /* repeated: *ptr + count member */
#define SC_PACK 0x40u /* repeated and packed on the wire */
#define SC_REQ 0x80u  /* proto2 required */

typedef struct sc_plan sc_plan;

typedef struct sc_entry {
    uint8_t op;
    uint8_t wire;    /* wire type of one element */
    uint8_t taglen;  /* 1..5, precomputed */
    uint8_t tag[8];  /* tag bytes, then zero padding for one wide store */
    uint8_t etaglen; /* END_GROUP tag length (groups only) */
    uint8_t etag[8]; /* END_GROUP tag bytes (groups only) */
    uint32_t fnum;
    int32_t dflt;                   /* enum default (SC_DFLT) */
    size_t off;                     /* value offset */
    size_t has;                     /* has-flag offset, 0 = none */
    size_t cnt;                     /* count offset (SC_RPT), 0 = singular */
    size_t elem;                    /* element size in memory: struct stride or scalar width */
    uint8_t oneof;                  /* 1 when the field is in a real oneof (cs selects) */
    size_t cs;                      /* oneof case-word offset */
    const pbw_struct_field_t *rows; /* sub-layout rows (MSG/GRP), else NULL */
    size_t nrows;                   /* sub-layout row count */
    sc_plan *sub;                   /* SC_OP_MSG */
} sc_entry;

struct sc_plan {
    sc_entry *e; /* ascending field number = canonical wire order */
    size_t n;
    uint32_t *byfnum; /* fnum -> index + 1, 0 = unknown; NULL = binary search */
    size_t maxfnum;
    const sc_entry **dflts; /* entries seeding an enum default */
    size_t ndflts;
    const sc_entry **reqs; /* proto2 required fields */
    size_t nreqs;
    /* Link-time prejudge: tail[i] bounds every unchecked byte written from
     * entry i on (see entry_bounded). One reserve of tail[0] at level entry
     * covers all singular scalars; strings reserve payload + the tail that
     * follows them, sub-messages re-reserve the following tail on return, so
     * no scalar ever checks capacity. Repeated fields contribute 0: their
     * count is only known at runtime, so they keep the exact checked path.
     * The sums cannot wrap: one entry costs at most 18 bytes while the
     * entries themselves are resident memory. */
    size_t *tail; /* n + 1 suffix sums, tail[n] == 0 */
};

static size_t
entry_bounded(const sc_entry *e)
{
    /* Worst case this entry writes without a runtime-sized payload. */
    switch (e->op & SC_OP_MASK) {
    case SC_OP_F32:
    case SC_OP_F64:
        return (e->op & SC_RPT) ? 0 : 8 + 8;
    case SC_OP_GRP:
        return (e->op & SC_RPT) ? 0 : 16; /* START_GROUP + END_GROUP tags */
    case SC_OP_STR:
    case SC_OP_BYTES:
        return (e->op & SC_RPT) ? 0 : 8 + 10;
    case SC_OP_MSG:
        return (e->op & SC_RPT) ? 0 : 8 + 1;
    default:
        return (e->op & SC_RPT) ? 0 : 8 + 10;
    }
}

static size_t
elem_size(const pbw_struct_field_t *row, const rinfo *info)
{
    switch (info->type) {
    case PBW_TYPE_BOOL:
        return 1;
    case PBW_TYPE_INT32:
    case PBW_TYPE_UINT32:
    case PBW_TYPE_SINT32:
    case PBW_TYPE_FIXED32:
    case PBW_TYPE_SFIXED32:
    case PBW_TYPE_FLOAT:
    case PBW_TYPE_ENUM:
        return 4;
    case PBW_TYPE_STRING:
        return sizeof(const char *);
    case PBW_TYPE_BYTES:
        return sizeof(pbw_bytes_t);
    case PBW_TYPE_MESSAGE:
    case PBW_TYPE_GROUP:
        return row->elem;
    default:
        return 8;
    }
}

static uint8_t
field_op(const pbw_struct_field_t *row, const rinfo *info)
{
    uint8_t op;

    switch (info->type) {
    case PBW_TYPE_INT32:
    case PBW_TYPE_ENUM:
        op = SC_OP_I32;
        break;
    case PBW_TYPE_UINT32:
        op = SC_OP_U32;
        break;
    case PBW_TYPE_SINT32:
        op = SC_OP_S32;
        break;
    case PBW_TYPE_INT64:
        op = SC_OP_I64;
        break;
    case PBW_TYPE_UINT64:
        op = SC_OP_U64;
        break;
    case PBW_TYPE_SINT64:
        op = SC_OP_S64;
        break;
    case PBW_TYPE_BOOL:
        op = SC_OP_BOOL;
        break;
    case PBW_TYPE_FLOAT:
    case PBW_TYPE_FIXED32:
    case PBW_TYPE_SFIXED32:
        op = SC_OP_F32;
        break;
    case PBW_TYPE_DOUBLE:
    case PBW_TYPE_FIXED64:
    case PBW_TYPE_SFIXED64:
        op = SC_OP_F64;
        break;
    case PBW_TYPE_STRING:
        op = SC_OP_STR;
        break;
    case PBW_TYPE_BYTES:
        op = SC_OP_BYTES;
        break;
    case PBW_TYPE_GROUP:
        op = SC_OP_GRP;
        break;
    default:
        op = SC_OP_MSG;
        break;
    }
    if (row->repeated) {
        op |= SC_RPT;
        if (info->packed && packable(info->type)) {
            op |= SC_PACK;
        }
    }
    if (info->required) {
        op |= SC_REQ;
    }
    return op;
}

static void
plan_free(sc_plan *p)
{
    size_t i;

    if (p == NULL) {
        return;
    }
    for (i = 0; i < p->n; i++) {
        plan_free(p->e[i].sub);
    }
    free(p->e);
    free(p->tail);
    free(p->byfnum);
    free(p->dflts);
    free(p->reqs);
    free(p);
}

/* Insertion sort: layouts are written in field order, so this is close to
 * linear, and it keeps the plan in canonical (ascending fnum) wire order. */
static void
plan_sort(sc_entry *e, size_t n)
{
    size_t i, j;

    for (i = 1; i < n; i++) {
        sc_entry tmp = e[i];

        for (j = i; j > 0 && e[j - 1].fnum > tmp.fnum; j--) {
            e[j] = e[j - 1];
        }
        e[j] = tmp;
    }
}

/* Resolve every row into an entry; recursive for MESSAGE rows. */
static pbw_status
plan_build(const pbw_descriptor *desc, const pbw_struct_field_t *fields,
           size_t nfields, unsigned depth, sc_plan **out)
{
    sc_plan *p;
    size_t i, ndflts = 0, nreqs = 0, maxfnum = 0;
    pbw_status status = PBW_ERR_INVALID;

    *out = NULL;
    if (depth >= SC_MAX_DEPTH) {
        return PBW_ERR_LIMIT;
    }
    p = (sc_plan *)calloc(1, sizeof(*p));
    if (p == NULL) {
        return PBW_ERR_NOMEM;
    }
    p->n = nfields;
    p->e = (sc_entry *)calloc(nfields ? nfields : 1, sizeof(*p->e));
    if (p->e == NULL) {
        plan_free(p);
        return PBW_ERR_NOMEM;
    }
    for (i = 0; i < nfields; i++) {
        const pbw_struct_field_t *row = &fields[i];
        sc_entry *e = &p->e[i];
        rinfo info;

        if (!row_info(row, desc, &info)) {
            status = PBW_ERR_INVALID;
            goto fail;
        }
        e->op = field_op(row, &info);
        if (row->kind == PBW_SF_ENUM && !row->repeated && row->dflt != 0) {
            /* The struct arrives zeroed, so only non-zero defaults need a
             * store; absent proto3 enums already read 0. */
            e->op |= SC_DFLT;
            e->dflt = row->dflt;
        }
        e->fnum = row->fnum;
        e->wire = (uint8_t)info.wire;
        e->off = row->offset;
        e->has = row->has;
        e->cnt = row->count;
        e->elem = elem_size(row, &info);
        e->oneof = row->oneof;
        e->cs = row->cs;
        e->taglen = (uint8_t)varint_size(tag_value(row->fnum, info.wire));
        put_varint(e->tag, tag_value(row->fnum, info.wire));
        if ((e->op & SC_OP_MASK) == SC_OP_GRP) {
            e->etaglen = (uint8_t)varint_size(tag_value(row->fnum, 4));
            put_varint(e->etag, tag_value(row->fnum, 4));
        }
        if ((e->op & SC_OP_MASK) == SC_OP_MSG ||
            (e->op & SC_OP_MASK) == SC_OP_GRP) {
            e->rows = row->fields;
            e->nrows = row->nfields;
            pbw_status sub = plan_build(desc, row->fields, row->nfields,
                                        depth + 1, &e->sub);

            if (sub != PBW_OK) {
                status = sub;
                goto fail;
            }
        }
        maxfnum = row->fnum > maxfnum ? row->fnum : maxfnum;
        ndflts += (e->op & SC_DFLT) != 0;
        nreqs += (e->op & SC_REQ) != 0;
    }
    plan_sort(p->e, p->n);
    p->maxfnum = maxfnum;
    /* Suffix worst-case bytes for the unchecked encode path (see enc_msg).
     * Computed after the sort; order does not matter to the sum. */
    p->tail = (size_t *)calloc(p->n + 1, sizeof(*p->tail));
    if (p->tail == NULL) {
        status = PBW_ERR_NOMEM;
        goto fail;
    }
    for (i = p->n; i > 0; i--) {
        p->tail[i - 1] = p->tail[i] + entry_bounded(&p->e[i - 1]);
    }
    /* A direct fnum -> entry map is one load per field on decode, which is
     * what a generated switch does; for sparse field numbers it would be a
     * big table for few fields, so fall back to binary search there. */
    if (maxfnum + 1 <= 8 * (p->n + 1)) {
        p->byfnum = (uint32_t *)calloc(maxfnum + 1, sizeof(*p->byfnum));
        if (p->byfnum == NULL) {
            status = PBW_ERR_NOMEM;
            goto fail;
        }
        for (i = 0; i < p->n; i++) {
            if (p->byfnum[p->e[i].fnum] == 0) {
                p->byfnum[p->e[i].fnum] = (uint32_t)i + 1;
            }
        }
    }
    if (ndflts > 0) {
        size_t k = 0;

        p->dflts = (const sc_entry **)calloc(ndflts, sizeof(*p->dflts));
        if (p->dflts == NULL) {
            status = PBW_ERR_NOMEM;
            goto fail;
        }
        for (i = 0; i < p->n; i++) {
            if (p->e[i].op & SC_DFLT) {
                p->dflts[k++] = &p->e[i];
            }
        }
        p->ndflts = k;
    }
    if (nreqs > 0) {
        size_t k = 0;

        p->reqs = (const sc_entry **)calloc(nreqs, sizeof(*p->reqs));
        if (p->reqs == NULL) {
            status = PBW_ERR_NOMEM;
            goto fail;
        }
        for (i = 0; i < p->n; i++) {
            if (p->e[i].op & SC_REQ) {
                p->reqs[k++] = &p->e[i];
            }
        }
        p->nreqs = k;
    }
    *out = p;
    return PBW_OK;
fail:
    plan_free(p);
    return status;
}

struct pbw_struct_codec {
    sc_plan *root;
    const pbw_struct_field_t *fields; /* for pbw_struct_free() on a failed
                                       * per-object decode */
    size_t nfields;
};

/* ----------------------------------------------------------------- writer */

typedef struct {
    uint8_t *data;
    size_t len;
    size_t cap;
    pbw_status status;
} sbuf;

static int
sb_reserve(sbuf *b, size_t extra)
{
    size_t need, cap;
    uint8_t *data;

    if (b->status != PBW_OK) {
        return 0;
    }
    need = b->len + extra;
    if (need <= b->cap) {
        return 1;
    }
    cap = b->cap == 0 ? 256 : b->cap;
    while (cap < need) {
        if (cap > SIZE_MAX - cap / 2) {
            cap = need;
            break;
        }
        cap += cap / 2;
    }
    data = (uint8_t *)realloc(b->data, cap);
    if (data == NULL) {
        b->status = PBW_ERR_NOMEM;
        return 0;
    }
    b->data = data;
    b->cap = cap;
    return 1;
}

/* Callers that reserved 16 bytes write tag and value without checking. */
static void
sb_tag(sbuf *b, const sc_entry *e)
{
    memcpy(b->data + b->len, e->tag, sizeof(e->tag));
    b->len += e->taglen;
}

static void
sb_varint(sbuf *b, uint64_t value)
{
    b->len = (size_t)(put_varint(b->data + b->len, value) - b->data);
}

static void
sb_fixed(sbuf *b, uint64_t value, size_t width)
{
    uint8_t *out = b->data + b->len;

    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
    out[2] = (uint8_t)(value >> 16);
    out[3] = (uint8_t)(value >> 24);
    if (width == 8) {
        out[4] = (uint8_t)(value >> 32);
        out[5] = (uint8_t)(value >> 40);
        out[6] = (uint8_t)(value >> 48);
        out[7] = (uint8_t)(value >> 56);
    }
    b->len += width;
}

/* A length-delimited payload needs its length first, but it is only known
 * afterwards: write a one-byte placeholder, encode, then patch it and shift
 * the payload right if the length needs more than one byte. */
static size_t
sb_len_begin(sbuf *b, uint32_t fnum)
{
    size_t mark;

    if (!sb_reserve(b, 16)) {
        return 0;
    }
    sb_varint(b, tag_value(fnum, 2));
    mark = b->len;
    b->data[b->len++] = 0;
    return mark;
}

static void
sb_len_end(sbuf *b, size_t mark)
{
    size_t payload, n, shift;

    if (b->status != PBW_OK) {
        return;
    }
    payload = b->len - mark - 1;
    n = varint_size(payload);
    shift = n - 1;
    if (shift > 0) {
        if (!sb_reserve(b, shift)) {
            return;
        }
        memmove(b->data + mark + 1 + shift, b->data + mark + 1, payload);
        b->len += shift;
    }
    put_varint(b->data + mark, payload);
}

static void
enc_msg(const sc_plan *p, sbuf *b, const char *base, unsigned depth);

/* Reserves the widest a scalar can be (5-byte tag + 10-byte varint) so the
 * value is written in place; the slack past b->len is never read. */
static void
enc_varint(sbuf *b, const sc_entry *e, uint64_t value)
{
    if (!sb_reserve(b, 16)) {
        return;
    }
    sb_tag(b, e);
    sb_varint(b, value);
}

static void
enc_bytes(sbuf *b, const sc_entry *e, const char *data, size_t len)
{
    if (!sb_reserve(b, 16 + len)) {
        return;
    }
    sb_tag(b, e);
    sb_varint(b, len);
    if (len > 0) {
        memcpy(b->data + b->len, data, len);
        b->len += len;
    }
}

static uint64_t
load_varint(uint8_t op, const char *v)
{
    uint32_t u32;
    uint64_t u64;

    switch (op & SC_OP_MASK) {
    case SC_OP_I32:
        memcpy(&u32, v, 4);
        return (uint64_t)(int64_t)(int32_t)u32;
    case SC_OP_U32:
        memcpy(&u32, v, 4);
        return u32;
    case SC_OP_S32:
        memcpy(&u32, v, 4);
        return zigzag32(u32);
    case SC_OP_BOOL:
        return *(const uint8_t *)v != 0;
    default: /* I64, U64, S64 */
        memcpy(&u64, v, 8);
        return (op & SC_OP_MASK) == SC_OP_S64 ? zigzag64(u64) : u64;
    }
}

/* data points at the first element: for a repeated field that is the value
 * held in the array slot, for a singular one the member itself. */
static void
enc_scalars(const sc_entry *e, sbuf *b, const char *data, size_t n)
{
    uint8_t op = e->op;
    size_t j;

    if (n == 0) {
        return; /* an empty packed run writes no tag at all */
    }
    if ((op & SC_OP_MASK) == SC_OP_F32 || (op & SC_OP_MASK) == SC_OP_F64) {
        size_t width = (op & SC_OP_MASK) == SC_OP_F32 ? 4 : 8;

        if (op & SC_PACK) {
            size_t mark = sb_len_begin(b, e->fnum);

            for (j = 0; j < n && b->status == PBW_OK; j++) {
                uint64_t bits = 0;

                memcpy(&bits, data + j * e->elem, width);
                if (!sb_reserve(b, width)) {
                    return;
                }
                sb_fixed(b, bits, width);
            }
            sb_len_end(b, mark);
            return;
        }
        for (j = 0; j < n; j++) {
            uint64_t bits = 0;

            memcpy(&bits, data + j * e->elem, width);
            if (!sb_reserve(b, 16)) {
                return;
            }
            sb_tag(b, e);
            sb_fixed(b, bits, width);
        }
        return;
    }
    if (op & SC_PACK) {
        size_t mark = sb_len_begin(b, e->fnum);

        for (j = 0; j < n && b->status == PBW_OK; j++) {
            if (!sb_reserve(b, 10)) {
                return;
            }
            sb_varint(b, load_varint(op, data + j * e->elem));
        }
        sb_len_end(b, mark);
        return;
    }
    for (j = 0; j < n; j++) {
        enc_varint(b, e, load_varint(op, data + j * e->elem));
    }
}

/* Writes a fixed-width value with no capacity check: the caller guarantees
 * room through the level tail. */
static uint8_t *
out_fixed(uint8_t *out, uint64_t value, size_t width)
{
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
    out[2] = (uint8_t)(value >> 16);
    out[3] = (uint8_t)(value >> 24);
    if (width == 8) {
        out[4] = (uint8_t)(value >> 32);
        out[5] = (uint8_t)(value >> 40);
        out[6] = (uint8_t)(value >> 48);
        out[7] = (uint8_t)(value >> 56);
    }
    return out + width;
}

static void
enc_msg(const sc_plan *p, sbuf *b, const char *base, unsigned depth)
{
    /* One link-time prejudge covers the whole level: tail[i] bounds every
     * unchecked byte written from entry i on, so a single reserve at entry
     * replaces the per-field checks. Singular scalars then advance a local
     * cursor with no capacity branch and no trip through b->len; strings
     * reserve payload + following tail once and write unchecked; sub-messages
     * and repeated runs use the exact checked path and re-reserve the
     * following tail afterwards. Tried and rejected: per-field reserves only
     * (the old shape of this function, ~14 ns slower on the 136 B tier:
     * every check chains through b->len) and an exact upfront pre-size via a
     * strlen walk (a second pass over the strings, the same extra walk the
     * generated ByteSize pays: slower, not faster). */
    size_t i;
    uint8_t *out;

    if (depth >= SC_MAX_DEPTH) {
        b->status = PBW_ERR_LIMIT;
        return;
    }
    if (p->n == 0) {
        return; /* empty message: nothing to write, b->len stays 0 */
    }
    if (!sb_reserve(b, p->tail[0])) {
        return;
    }
    out = b->data + b->len;
    for (i = 0; i < p->n && b->status == PBW_OK; i++) {
        const sc_entry *e = &p->e[i];
        uint8_t op = e->op;
        const char *v = base + e->off;
        const char *data = v;
        size_t n = 1;

        /* Oneof members ride only when the case selects them; anything else
         * in the union is stale storage, never wire content. */
        if (e->oneof != 0 &&
            *(const uint32_t *)(base + e->cs) != e->fnum) {
            continue;
        }

        switch (op & SC_OP_MASK) {
        case SC_OP_STR:
            if ((op & SC_RPT) == 0) {
                const char *str = *(const char *const *)v;
                size_t len;

                if (str == NULL) {
                    if (op & SC_REQ) {
                        b->status = PBW_ERR_REQUIRED;
                    }
                    continue;
                }
                len = strlen(str);
                b->len = (size_t)(out - b->data);
                if (!sb_reserve(b, len + p->tail[i + 1])) {
                    out = b->data + b->len;
                    continue;
                }
                out = b->data + b->len;
                memcpy(out, e->tag, sizeof(e->tag));
                out += e->taglen;
                out = put_varint(out, (uint64_t)len);
                if (len > 0) {
                    memcpy(out, str, len);
                    out += len;
                }
                continue;
            }
            {
                const char *const *items = *(const char *const *const *)v;

                n = *(const size_t *)(base + e->cnt);
                if (n == 0) {
                    continue;
                }
                if (items == NULL) {
                    b->status = PBW_ERR_INVALID;
                    continue;
                }
                b->len = (size_t)(out - b->data);
                for (; n > 0; n--, items++) {
                    if (*items == NULL) {
                        enc_bytes(b, e, "", 0);
                    } else {
                        enc_bytes(b, e, *items, strlen(*items));
                    }
                }
                out = b->data + b->len;
                if (!sb_reserve(b, p->tail[i + 1])) {
                    out = b->data + b->len;
                    continue;
                }
                out = b->data + b->len;
            }
            continue;
        case SC_OP_BYTES:
            if ((op & SC_RPT) == 0) {
                const pbw_bytes_t *bytes = (const pbw_bytes_t *)v;
                size_t len;

                if (bytes->data == NULL) {
                    if (op & SC_REQ) {
                        b->status = PBW_ERR_REQUIRED;
                    }
                    continue;
                }
                len = bytes->len;
                b->len = (size_t)(out - b->data);
                if (!sb_reserve(b, len + p->tail[i + 1])) {
                    out = b->data + b->len;
                    continue;
                }
                out = b->data + b->len;
                memcpy(out, e->tag, sizeof(e->tag));
                out += e->taglen;
                out = put_varint(out, (uint64_t)len);
                if (len > 0) {
                    memcpy(out, bytes->data, len);
                    out += len;
                }
                continue;
            }
            {
                const pbw_bytes_t *items = *(const pbw_bytes_t *const *)v;

                n = *(const size_t *)(base + e->cnt);
                if (n == 0) {
                    continue;
                }
                if (items == NULL) {
                    b->status = PBW_ERR_INVALID;
                    continue;
                }
                b->len = (size_t)(out - b->data);
                for (; n > 0; n--, items++) {
                    if (items->data == NULL) {
                        enc_bytes(b, e, "", 0);
                    } else {
                        enc_bytes(b, e, (const char *)items->data,
                                  items->len);
                    }
                }
                out = b->data + b->len;
                if (!sb_reserve(b, p->tail[i + 1])) {
                    out = b->data + b->len;
                    continue;
                }
                out = b->data + b->len;
            }
            continue;
        case SC_OP_GRP:
            if ((op & SC_RPT) == 0) {
                const char *item = *(const char *const *)v;

                if (item == NULL) {
                    if (op & SC_REQ) {
                        b->status = PBW_ERR_REQUIRED;
                    }
                    continue;
                }
                /* START_GROUP covered by tail[i]; the sub-message reserves
                 * its own budget on entry. */
                memcpy(out, e->tag, sizeof(e->tag));
                out += e->taglen;
                b->len = (size_t)(out - b->data);
                enc_msg(e->sub, b, item, depth + 1);
                out = b->data + b->len;
                if (!sb_reserve(b, 8 + p->tail[i + 1])) {
                    out = b->data + b->len;
                    continue;
                }
                out = b->data + b->len;
                memcpy(out, e->etag, sizeof(e->etag));
                out += e->etaglen;
                continue;
            }
            {
                const char *items = *(const char *const *)v;

                n = *(const size_t *)(base + e->cnt);
                if (n == 0) {
                    continue;
                }
                if (items == NULL) {
                    b->status = PBW_ERR_INVALID;
                    continue;
                }
                b->len = (size_t)(out - b->data);
                for (; n > 0 && b->status == PBW_OK; n--, items += e->elem) {
                    if (!sb_reserve(b, 8)) {
                        break;
                    }
                    sb_tag(b, e);
                    enc_msg(e->sub, b, items, depth + 1);
                    if (b->status != PBW_OK) {
                        break;
                    }
                    if (!sb_reserve(b, 8)) {
                        break;
                    }
                    memcpy(b->data + b->len, e->etag, sizeof(e->etag));
                    b->len += e->etaglen;
                }
                out = b->data + b->len;
                if (!sb_reserve(b, p->tail[i + 1])) {
                    out = b->data + b->len;
                    continue;
                }
                out = b->data + b->len;
            }
            continue;
        case SC_OP_MSG:
            if ((op & SC_RPT) == 0) {
                const char *item = *(const char *const *)v;
                size_t mark;

                if (item == NULL) {
                    if (op & SC_REQ) {
                        b->status = PBW_ERR_REQUIRED;
                    }
                    continue;
                }
                /* Tag plus placeholder with no check: covered by tail[i].
                 * The sub-message reserves its own budget on entry. */
                memcpy(out, e->tag, sizeof(e->tag));
                out += e->taglen;
                *out++ = 0;
                mark = (size_t)(out - b->data) - 1;
                b->len = (size_t)(out - b->data);
                enc_msg(e->sub, b, item, depth + 1);
                sb_len_end(b, mark);
                out = b->data + b->len;
                if (!sb_reserve(b, p->tail[i + 1])) {
                    out = b->data + b->len;
                    continue;
                }
                out = b->data + b->len;
                continue;
            }
            {
                const char *items = *(const char *const *)v;

                n = *(const size_t *)(base + e->cnt);
                if (n == 0) {
                    continue;
                }
                if (items == NULL) {
                    b->status = PBW_ERR_INVALID;
                    continue;
                }
                b->len = (size_t)(out - b->data);
                for (; n > 0; n--, items += e->elem) {
                    size_t mark = sb_len_begin(b, e->fnum);

                    enc_msg(e->sub, b, items, depth + 1);
                    sb_len_end(b, mark);
                }
                out = b->data + b->len;
                if (!sb_reserve(b, p->tail[i + 1])) {
                    out = b->data + b->len;
                    continue;
                }
                out = b->data + b->len;
            }
            continue;
        default:
            /* Scalars. */
            break;
        }
        if ((op & SC_RPT) == 0) {
            if (e->has != 0 && *(const int *)(base + e->has) == 0) {
                continue;
            }
            /* No capacity check: tail[i] covers tag plus value. */
            if ((op & SC_OP_MASK) >= SC_OP_F32) {
                uint64_t bits = 0;
                size_t width = (op & SC_OP_MASK) == SC_OP_F32 ? 4 : 8;

                memcpy(&bits, v, width);
                memcpy(out, e->tag, sizeof(e->tag));
                out += e->taglen;
                out = out_fixed(out, bits, width);
            } else {
                memcpy(out, e->tag, sizeof(e->tag));
                out += e->taglen;
                out = put_varint(out, load_varint(op, v));
            }
            continue;
        }
        data = *(const char *const *)v;
        n = *(const size_t *)(base + e->cnt);
        if (n == 0) {
            continue;
        }
        if (data == NULL) {
            b->status = PBW_ERR_INVALID;
            continue;
        }
        b->len = (size_t)(out - b->data);
        enc_scalars(e, b, data, n);
        out = b->data + b->len;
        if (!sb_reserve(b, p->tail[i + 1])) {
            out = b->data + b->len;
            continue;
        }
        out = b->data + b->len;
    }
    b->len = (size_t)(out - b->data);
}

/* ------------------------------------------------------------------ arena */

/* Blocks are chained and never moved, so pointers handed out by an earlier
 * allocation stay valid. A reset keeps the blocks and rewinds them, which is
 * what makes a steady-state decode loop allocation-free. */
typedef struct sc_block {
    struct sc_block *next;
    size_t used;
    size_t cap;
    unsigned char data[1]; /* padded to 16 by alloc_block */
} sc_block;

struct pbw_struct_arena {
    sc_block *head;
    sc_block *tail;
    sc_block *cur;
};

static sc_block *
block_new(size_t cap)
{
    sc_block *b;
    size_t total = sizeof(*b) + cap + 15;

    b = (sc_block *)malloc(total);
    if (b == NULL) {
        return NULL;
    }
    b->next = NULL;
    b->used = 0;
    b->cap = cap;
    return b;
}

/* First usable byte, rounded up so every allocation is 16-byte aligned. */
static unsigned char *
block_data(sc_block *b)
{
    return (unsigned char *)(((uintptr_t)b->data + 15u) & ~(uintptr_t)15u);
}

static void
arena_reset(pbw_struct_arena *a)
{
    sc_block *b;

    for (b = a->head; b != NULL; b = b->next) {
        b->used = 0;
    }
    a->cur = a->head;
}

static void *
arena_alloc(pbw_struct_arena *a, size_t n)
{
    sc_block *b = a->cur;
    void *p;

    if (n > SIZE_MAX - 15u) {
        return NULL;
    }
    n = (n + 15u) & ~(size_t)15u;
    if (n == 0) {
        n = 16;
    }
    while (b != NULL && b->cap - b->used < n) {
        b = b->next;
    }
    if (b == NULL) {
        size_t cap = a->tail != NULL ? a->tail->cap * 2 : 8192;

        if (cap < n) {
            cap = n;
        }
        b = block_new(cap);
        if (b == NULL) {
            return NULL;
        }
        if (a->tail != NULL) {
            a->tail->next = b;
        } else {
            a->head = b;
        }
        a->tail = b;
    }
    p = block_data(b) + b->used;
    b->used += n;
    a->cur = b;
    return p;
}

/* ---------------------------------------------------------------- decoding */

typedef struct {
    const uint8_t *p;
    const uint8_t *end;
} rdr;

static int
rd_varint(rdr *r, uint64_t *out)
{
    const uint8_t *p = r->p;
    const uint8_t *end = r->end;
    uint64_t value = 0;
    unsigned shift = 0;

    while (p < end && shift < 64) {
        uint8_t byte = *p++;

        value |= (uint64_t)(byte & 0x7f) << shift;
        if ((byte & 0x80) == 0) {
            r->p = p;
            *out = value;
            return 1;
        }
        shift += 7;
    }
    return 0;
}

static int
rd_fixed(rdr *r, uint64_t *out, size_t width)
{
    const uint8_t *p;

    if ((size_t)(r->end - r->p) < width) {
        return 0;
    }
    p = r->p;
    *out = p[0] | ((uint64_t)p[1] << 8) | ((uint64_t)p[2] << 16) |
           ((uint64_t)p[3] << 24);
    if (width == 8) {
        *out |= ((uint64_t)p[4] << 32) | ((uint64_t)p[5] << 40) |
                ((uint64_t)p[6] << 48) | ((uint64_t)p[7] << 56);
    }
    r->p += width;
    return 1;
}

static int
rd_tag(rdr *r, uint32_t *fnum, unsigned *wt)
{
    uint64_t value;

    if (!rd_varint(r, &value)) {
        return 0;
    }
    if ((value >> 3) == 0 || (value >> 3) > 0x1fffffffull) {
        return 0;
    }
    *wt = (unsigned)(value & 7);
    if (*wt > 5) {
        return 0;
    }
    *fnum = (uint32_t)(value >> 3);
    return 1;
}

/* Unknown or wrongly typed field: skip its value, groups included. */
static int
rd_skip(rdr *r, uint32_t fnum, unsigned wt, unsigned depth)
{
    switch (wt) {
    case 0: {
        uint64_t value;

        return rd_varint(r, &value);
    }
    case 1:
        if ((size_t)(r->end - r->p) < 8) {
            return 0;
        }
        r->p += 8;
        return 1;
    case 2: {
        uint64_t n;

        if (!rd_varint(r, &n) || n > (uint64_t)(r->end - r->p)) {
            return 0;
        }
        r->p += (size_t)n;
        return 1;
    }
    case 3:
        if (depth >= SC_MAX_DEPTH) {
            return 0;
        }
        for (;;) {
            uint32_t inner_fnum;
            unsigned inner_wt;

            if (!rd_tag(r, &inner_fnum, &inner_wt)) {
                return 0;
            }
            if (inner_wt == 4) {
                return inner_fnum == fnum;
            }
            if (!rd_skip(r, inner_fnum, inner_wt, depth + 1)) {
                return 0;
            }
        }
    case 5:
        if ((size_t)(r->end - r->p) < 4) {
            return 0;
        }
        r->p += 4;
        return 1;
    default:
        return 0; /* stray END_GROUP */
    }
}

static void
store_varint(uint8_t op, char *v, uint64_t value)
{
    switch (op & SC_OP_MASK) {
    case SC_OP_I32: {
        int32_t i = (int32_t)value;

        memcpy(v, &i, 4);
        return;
    }
    case SC_OP_U32: {
        uint32_t u = (uint32_t)value;

        memcpy(v, &u, 4);
        return;
    }
    case SC_OP_S32: {
        uint32_t u = unzigzag32(value);

        memcpy(v, &u, 4);
        return;
    }
    case SC_OP_BOOL: {
        unsigned char b = value != 0 ? 1 : 0;

        memcpy(v, &b, 1);
        return;
    }
    case SC_OP_S64:
        value = unzigzag64(value);
        memcpy(v, &value, 8);
        return;
    default:
        memcpy(v, &value, 8);
        return;
    }
}

static void *
sc_alloc(pbw_struct_arena *a, size_t n)
{
    return a == NULL ? malloc(n) : arena_alloc(a, n);
}

static void *
sc_alloc0(pbw_struct_arena *a, size_t n)
{
    void *p = sc_alloc(a, n);

    if (p != NULL) {
        memset(p, 0, n);
    }
    return p;
}

static void *
sc_grow(pbw_struct_arena *a, void *old, size_t old_bytes, size_t new_bytes)
{
    void *p = sc_alloc(a, new_bytes);

    if (p == NULL) {
        return NULL;
    }
    if (old != NULL) {
        memcpy(p, old, old_bytes);
        if (a == NULL) {
            free(old);
        }
    }
    return p;
}

/* Repeated fields grow by doubling: the count member is both the fill index
 * and (via powers of two) the capacity, so no sizing pass is needed and a
 * message with empty repeated fields allocates nothing. */
static void *
sc_slot(const sc_entry *e, char *base, pbw_struct_arena *a)
{
    size_t *cnt = (size_t *)(base + e->cnt);
    size_t i = *cnt;
    char **arr = (char **)(base + e->off);

    if (i == 0) {
        *arr = (char *)sc_alloc(a, 8 * e->elem);
        if (*arr == NULL) {
            return NULL;
        }
    } else if ((i & (i - 1)) == 0 && i >= 8) {
        char *grown = (char *)sc_grow(a, *arr, i * e->elem, 2 * i * e->elem);

        if (grown == NULL) {
            return NULL;
        }
        *arr = grown;
    }
    *cnt = i + 1;
    return *arr + i * e->elem;
}

static const sc_entry *
sc_lookup(const sc_plan *p, uint32_t fnum)
{
    if (p->byfnum != NULL) {
        return fnum <= p->maxfnum && p->byfnum[fnum] != 0
                   ? &p->e[p->byfnum[fnum] - 1]
                   : NULL;
    }
    if (p->n > 0) {
        size_t lo = 0, hi = p->n;

        while (lo < hi) {
            size_t mid = lo + (hi - lo) / 2;

            if (p->e[mid].fnum < fnum) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        if (lo < p->n && p->e[lo].fnum == fnum) {
            return &p->e[lo];
        }
    }
    return NULL;
}

/* proto2 required fields must be present; a required scalar without a
 * has-flag is always written by the encoder, so only pointers and counts can
 * be absent. Only called for message types that have required fields. */
static pbw_status
check_required(const sc_plan *p, const char *base)
{
    size_t i;

    for (i = 0; i < p->nreqs; i++) {
        const sc_entry *e = p->reqs[i];
        uint8_t op = e->op & SC_OP_MASK;

        if (e->op & SC_RPT) {
            if (*(const size_t *)(base + e->cnt) == 0) {
                return PBW_ERR_REQUIRED;
            }
            continue;
        }
        if ((op == SC_OP_STR || op == SC_OP_BYTES || op == SC_OP_MSG ||
             op == SC_OP_GRP) &&
            *(const void *const *)(base + e->off) == NULL) {
            return PBW_ERR_REQUIRED;
        }
    }
    return PBW_OK;
}

/* Reads a length-delimited payload header and returns the payload bounds. */
static int
rd_payload(rdr *r, rdr *inner)
{
    uint64_t len;

    if (!rd_varint(r, &len) || len > (uint64_t)(r->end - r->p)) {
        return 0;
    }
    inner->p = r->p;
    inner->end = r->p + (size_t)len;
    r->p = inner->end;
    return 1;
}

/* Wire type of END_GROUP; START_GROUP is 3 (see wire_type_of). */
#define SC_WT_END_GROUP 4

/* Releases what a oneof member holds so a case change cannot leak it.
 * Arena trees are abandoned, not freed: their pointers do not own
 * individual allocations. Plain trees free through the layout rows. */
static void
clear_oneof_prev(const sc_entry *e, char *base, pbw_struct_arena *a)
{
    char *v = base + e->off;

    if (e->op & SC_RPT) { /* invalid in a oneof; stay safe anyway */
        free(*(void **)v);
        *(void **)v = NULL;
        if (e->cnt != 0) {
            *(size_t *)(base + e->cnt) = 0;
        }
        return;
    }
    switch (e->op & SC_OP_MASK) {
    case SC_OP_STR:
        if (a == NULL) {
            free(*(void **)v);
        }
        *(void **)v = NULL;
        break;
    case SC_OP_BYTES: {
        pbw_bytes_t *bytes = (pbw_bytes_t *)v;

        if (a == NULL) {
            free(bytes->data);
        }
        bytes->data = NULL;
        bytes->len = 0;
        break;
    }
    case SC_OP_MSG:
    case SC_OP_GRP: {
        void *item = *(void **)v;

        if (item != NULL) {
            if (a == NULL && e->rows != NULL) {
                pbw_struct_free(e->rows, e->nrows, item);
                free(item);
            }
            *(void **)v = NULL;
        }
        break;
    }
    default:
        memset(v, 0, e->elem);
        break;
    }
    if (e->has != 0) {
        *(int *)(base + e->has) = 0;
    }
}

static pbw_status
dec_msg(const sc_plan *p, rdr *r, char *base, pbw_struct_arena *a,
        unsigned depth, uint32_t stop)
{
    size_t i;

    if (depth >= SC_MAX_DEPTH) {
        return PBW_ERR_LIMIT;
    }
    for (i = 0; i < p->ndflts; i++) {
        const sc_entry *e = p->dflts[i];

        memcpy(base + e->off, &e->dflt, 4);
    }
    while (r->p < r->end) {
        uint32_t fnum;
        unsigned wt;
        const sc_entry *e;
        uint8_t op;
        char *v;
        uint64_t value;

        if (!rd_tag(r, &fnum, &wt)) {
            return PBW_ERR_INVALID;
        }
        if (stop != 0 && wt == SC_WT_END_GROUP) {
            /* Group contents end at the matching END_GROUP; any other
             * END_GROUP inside is malformed. */
            if (fnum == stop) {
                return p->nreqs == 0 ? PBW_OK : check_required(p, base);
            }
            return PBW_ERR_INVALID;
        }
        e = sc_lookup(p, fnum);
        if (e == NULL) {
            if (!rd_skip(r, fnum, wt, depth)) {
                return PBW_ERR_INVALID;
            }
            continue;
        }
        op = e->op;
        v = base + e->off;

        /* Oneof: a new member clears the previous one (last wins, like the
         * reference decoders); the case is set before the store so a later
         * failure cannot leave a half-moved union. A failed decode discards
         * the tree either way. */
        if (e->oneof != 0) {
            uint32_t *csp = (uint32_t *)(base + e->cs);

            if (*csp != 0 && *csp != e->fnum) {
                const sc_entry *prev = sc_lookup(p, *csp);

                if (prev != NULL) {
                    clear_oneof_prev(prev, base, a);
                }
            }
            *csp = e->fnum;
        }

        switch (op & SC_OP_MASK) {
        case SC_OP_I32:
        case SC_OP_I64:
        case SC_OP_U32:
        case SC_OP_U64:
        case SC_OP_S32:
        case SC_OP_S64:
        case SC_OP_BOOL:
            if (op & SC_RPT) {
                if (wt == 2) { /* packed run for a numeric field */
                    rdr inner;

                    if (!rd_payload(r, &inner)) {
                        return PBW_ERR_INVALID;
                    }
                    while (inner.p < inner.end) {
                        char *slot;

                        if (!rd_varint(&inner, &value)) {
                            return PBW_ERR_INVALID;
                        }
                        slot = (char *)sc_slot(e, base, a);
                        if (slot == NULL) {
                            return PBW_ERR_NOMEM;
                        }
                        store_varint(op, slot, value);
                    }
                } else if (wt == e->wire) {
                    char *slot;

                    if (!rd_varint(r, &value)) {
                        return PBW_ERR_INVALID;
                    }
                    slot = (char *)sc_slot(e, base, a);
                    if (slot == NULL) {
                        return PBW_ERR_NOMEM;
                    }
                    store_varint(op, slot, value);
                } else if (!rd_skip(r, fnum, wt, depth)) {
                    return PBW_ERR_INVALID;
                }
                continue;
            }
            if (wt != e->wire) {
                if (!rd_skip(r, fnum, wt, depth)) {
                    return PBW_ERR_INVALID;
                }
                continue;
            }
            if (!rd_varint(r, &value)) {
                return PBW_ERR_INVALID;
            }
            store_varint(op, v, value);
            if (e->has != 0) {
                *(int *)(base + e->has) = 1;
            }
            continue;
        case SC_OP_F32:
        case SC_OP_F64: {
            size_t width = (op & SC_OP_MASK) == SC_OP_F32 ? 4 : 8;

            if (op & SC_RPT) {
                if (wt == 2) {
                    rdr inner;

                    if (!rd_payload(r, &inner)) {
                        return PBW_ERR_INVALID;
                    }
                    while (inner.p < inner.end) {
                        char *slot;

                        if (!rd_fixed(&inner, &value, width)) {
                            return PBW_ERR_INVALID;
                        }
                        slot = (char *)sc_slot(e, base, a);
                        if (slot == NULL) {
                            return PBW_ERR_NOMEM;
                        }
                        memcpy(slot, &value, width);
                    }
                } else if (wt == e->wire) {
                    char *slot;

                    if (!rd_fixed(r, &value, width)) {
                        return PBW_ERR_INVALID;
                    }
                    slot = (char *)sc_slot(e, base, a);
                    if (slot == NULL) {
                        return PBW_ERR_NOMEM;
                    }
                    memcpy(slot, &value, width);
                } else if (!rd_skip(r, fnum, wt, depth)) {
                    return PBW_ERR_INVALID;
                }
                continue;
            }
            if (wt != e->wire) {
                if (!rd_skip(r, fnum, wt, depth)) {
                    return PBW_ERR_INVALID;
                }
                continue;
            }
            if (!rd_fixed(r, &value, width)) {
                return PBW_ERR_INVALID;
            }
            memcpy(v, &value, width);
            if (e->has != 0) {
                *(int *)(base + e->has) = 1;
            }
            continue;
        }
        case SC_OP_STR: {
            rdr inner;
            char *dup;

            if (wt != e->wire) {
                if (!rd_skip(r, fnum, wt, depth)) {
                    return PBW_ERR_INVALID;
                }
                continue;
            }
            if (!rd_payload(r, &inner)) {
                return PBW_ERR_INVALID;
            }
            dup = (char *)sc_alloc(a, (size_t)(inner.end - inner.p) + 1);
            if (dup == NULL) {
                return PBW_ERR_NOMEM;
            }
            memcpy(dup, inner.p, (size_t)(inner.end - inner.p));
            dup[inner.end - inner.p] = '\0';
            if (op & SC_RPT) {
                char *slot = (char *)sc_slot(e, base, a);

                if (slot == NULL) {
                    if (a == NULL) {
                        free(dup);
                    }
                    return PBW_ERR_NOMEM;
                }
                *(char **)slot = dup;
            } else {
                *(char **)v = dup;
            }
            continue;
        }
        case SC_OP_BYTES: {
            rdr inner;
            size_t len;
            uint8_t *dup;

            if (wt != e->wire) {
                if (!rd_skip(r, fnum, wt, depth)) {
                    return PBW_ERR_INVALID;
                }
                continue;
            }
            if (!rd_payload(r, &inner)) {
                return PBW_ERR_INVALID;
            }
            len = (size_t)(inner.end - inner.p);
            dup = (uint8_t *)sc_alloc(a, len + 1);
            if (dup == NULL) {
                return PBW_ERR_NOMEM;
            }
            if (len > 0) {
                memcpy(dup, inner.p, len);
            }
            dup[len] = '\0'; /* NUL guard, not counted in len */
            if (op & SC_RPT) {
                pbw_bytes_t *slot = (pbw_bytes_t *)sc_slot(e, base, a);

                if (slot == NULL) {
                    if (a == NULL) {
                        free(dup);
                    }
                    return PBW_ERR_NOMEM;
                }
                slot->data = dup;
                slot->len = len;
            } else {
                pbw_bytes_t *dst = (pbw_bytes_t *)v;

                dst->data = dup;
                dst->len = len;
            }
            continue;
        }
        case SC_OP_MSG: {
            rdr inner;
            char *slot;

            if (wt != e->wire) {
                if (!rd_skip(r, fnum, wt, depth)) {
                    return PBW_ERR_INVALID;
                }
                continue;
            }
            if (!rd_payload(r, &inner)) {
                return PBW_ERR_INVALID;
            }
            if (op & SC_RPT) {
                slot = (char *)sc_slot(e, base, a);
                if (slot != NULL) {
                    memset(slot, 0, e->elem);
                }
            } else {
                slot = (char *)sc_alloc0(a, e->elem);
            }
            if (slot == NULL) {
                return PBW_ERR_NOMEM;
            }
            {
                pbw_status status = dec_msg(e->sub, &inner, slot, a, depth + 1, 0);

                if (status != PBW_OK) {
                    return status;
                }
            }
            if ((op & SC_RPT) == 0) {
                *(char **)v = slot;
            }
            continue;
        }
        case SC_OP_GRP: {
            char *slot;

            if (wt != e->wire) {
                if (!rd_skip(r, fnum, wt, depth)) {
                    return PBW_ERR_INVALID;
                }
                continue;
            }
            if (op & SC_RPT) {
                slot = (char *)sc_slot(e, base, a);
                if (slot != NULL) {
                    memset(slot, 0, e->elem);
                }
            } else {
                slot = (char *)sc_alloc0(a, e->elem);
            }
            if (slot == NULL) {
                return PBW_ERR_NOMEM;
            }
            /* Same reader: contents run to the matching END_GROUP. */
            {
                pbw_status status = dec_msg(e->sub, r, slot, a, depth + 1, e->fnum);

                if (status != PBW_OK) {
                    return status;
                }
            }
            if ((op & SC_RPT) == 0) {
                *(char **)v = slot;
            }
            continue;
        }
        }
    }
    return p->nreqs == 0 ? PBW_OK : check_required(p, base);
}

/* ------------------------------------------------------------------- api */

pbw_status
pbw_struct_codec_init(pbw_struct_codec **out, const pbw_schema *schema,
                      const char *message, const pbw_struct_field_t *fields,
                      size_t nfields)
{
    const pbw_descriptor *desc;
    pbw_struct_codec *codec;
    pbw_status status;

    if (out == NULL) {
        return PBW_ERR_INVALID;
    }
    *out = NULL;
    if (schema == NULL || message == NULL || fields == NULL) {
        return PBW_ERR_INVALID;
    }
    desc = pbw_schema_find(schema, message);
    if (desc == NULL) {
        return PBW_ERR_INVALID;
    }
    if (pbw_descriptor_field_count(desc) != nfields) {
        return PBW_ERR_INVALID;
    }
    codec = (pbw_struct_codec *)calloc(1, sizeof(*codec));
    if (codec == NULL) {
        return PBW_ERR_NOMEM;
    }
    status = plan_build(desc, fields, nfields, 0, &codec->root);
    if (status != PBW_OK) {
        free(codec);
        return status;
    }
    codec->fields = fields;
    codec->nfields = nfields;
    *out = codec;
    return PBW_OK;
}

void
pbw_struct_codec_free(pbw_struct_codec *codec)
{
    if (codec == NULL) {
        return;
    }
    plan_free(codec->root);
    free(codec);
}

pbw_status
pbw_struct_codec_encode(const pbw_struct_codec *codec, const void *base,
                        pbw_struct_buf *buf)
{
    sbuf b;

    if (codec == NULL || base == NULL || buf == NULL) {
        return PBW_ERR_INVALID;
    }
    b.data = buf->data;
    b.len = 0;
    b.cap = buf->cap;
    b.status = PBW_OK;
    enc_msg(codec->root, &b, (const char *)base, 0);
    buf->data = b.data;
    buf->cap = b.cap;
    buf->len = b.status == PBW_OK ? b.len : 0;
    return b.status;
}

pbw_status
pbw_struct_codec_decode(const pbw_struct_codec *codec, const uint8_t *wire,
                        size_t wire_len, pbw_struct_arena **arena, void *base)
{
    pbw_struct_arena *a = arena != NULL ? *arena : NULL;
    rdr r;
    pbw_status status;

    if (codec == NULL || base == NULL || (wire == NULL && wire_len != 0)) {
        return PBW_ERR_INVALID;
    }
    if (arena != NULL && a == NULL) {
        a = (pbw_struct_arena *)calloc(1, sizeof(*a));
        if (a == NULL) {
            return PBW_ERR_NOMEM;
        }
        *arena = a;
    }
    if (a != NULL) {
        arena_reset(a);
    }
    r.p = wire;
    r.end = wire + wire_len;
    status = dec_msg(codec->root, &r, (char *)base, a, 0, 0);
    if (status != PBW_OK && a == NULL) {
        /* Per-object allocations belong to the caller: hand them back so a
         * failed decode leaks nothing. */
        pbw_struct_free(codec->fields, codec->nfields, base);
    }
    return status;
}

void
pbw_struct_buf_free(pbw_struct_buf *buf)
{
    if (buf == NULL) {
        return;
    }
    free(buf->data);
    buf->data = NULL;
    buf->len = 0;
    buf->cap = 0;
}

void
pbw_struct_arena_free(pbw_struct_arena *arena)
{
    sc_block *b;
    sc_block *next;

    if (arena == NULL) {
        return;
    }
    for (b = arena->head; b != NULL; b = next) {
        next = b->next;
        free(b);
    }
    free(arena);
}

pbw_status
pbw_struct_encode(const pbw_schema *schema, const char *message,
                  const pbw_struct_field_t *fields, size_t nfields,
                  const void *base, uint8_t **wire, size_t *wire_len)
{
    pbw_struct_codec *codec = NULL;
    pbw_struct_buf buf = {NULL, 0, 0};
    pbw_status status;

    if (wire == NULL || wire_len == NULL) {
        return PBW_ERR_INVALID;
    }
    *wire = NULL;
    *wire_len = 0;
    if (base == NULL) {
        return PBW_ERR_INVALID;
    }
    status = pbw_struct_codec_init(&codec, schema, message, fields, nfields);
    if (status != PBW_OK) {
        return status;
    }
    status = pbw_struct_codec_encode(codec, base, &buf);
    pbw_struct_codec_free(codec);
    if (status != PBW_OK) {
        pbw_struct_buf_free(&buf);
        return status;
    }
    *wire = buf.data;
    *wire_len = buf.len;
    return PBW_OK;
}

pbw_status
pbw_struct_decode(const pbw_schema *schema, const char *message,
                  const pbw_struct_field_t *fields, size_t nfields,
                  const uint8_t *wire, size_t wire_len, void *base)
{
    pbw_struct_codec *codec = NULL;
    pbw_status status;

    status = pbw_struct_codec_init(&codec, schema, message, fields, nfields);
    if (status != PBW_OK) {
        return status;
    }
    status = pbw_struct_codec_decode(codec, wire, wire_len, NULL, base);
    pbw_struct_codec_free(codec);
    return status;
}

void
pbw_struct_free(const pbw_struct_field_t *fields, size_t nfields, void *base)
{
    size_t i, j;

    if (base == NULL || fields == NULL) {
        return;
    }
    for (i = 0; i < nfields; i++) {
        const pbw_struct_field_t *row = &fields[i];
        char *p = (char *)base + row->offset;

        if (row->kind == PBW_SF_MESSAGE) {
            if (!row->repeated) {
                void *item = *(void **)p;

                if (item != NULL) {
                    pbw_struct_free(row->fields, row->nfields, item);
                    free(item);
                    *(void **)p = NULL;
                }
            } else {
                void *items = *(void **)p;
                size_t n = *(size_t *)((char *)base + row->count);

                for (j = 0; j < n; j++) {
                    pbw_struct_free(row->fields, row->nfields,
                                    (char *)items + j * row->elem);
                }
                free(items);
                *(void **)p = NULL;
            }
        } else if (row->kind == PBW_SF_BYTES) {
            if (!row->repeated) {
                pbw_bytes_t *bytes = (pbw_bytes_t *)p;

                free(bytes->data);
                bytes->data = NULL;
                bytes->len = 0;
            } else {
                pbw_bytes_t *items = *(pbw_bytes_t **)p;
                size_t n = *(size_t *)((char *)base + row->count);

                for (j = 0; j < n; j++) {
                    free(items[j].data);
                }
                free(items);
                *(void **)p = NULL;
            }
        } else if (row->kind == PBW_SF_STRING) {
            if (!row->repeated) {
                free(*(void **)p);
                *(void **)p = NULL;
            } else {
                char **items = *(char ***)p;
                size_t n = *(size_t *)((char *)base + row->count);

                for (j = 0; j < n; j++) {
                    free(items[j]);
                }
                free(items);
                *(void **)p = NULL;
            }
        } else if (row->repeated) {
            free(*(void **)p);
            *(void **)p = NULL;
        }
    }
}

/* FileDescriptorSet bytes are copied into schema-owned memory by
 * pbw_descriptor_set_load, so the file buffer is released here. */
pbw_status
pbw_struct_schema_load(const char *path, pbw_schema **out)
{
    FILE *f;
    long size;
    size_t len;
    uint8_t *desc;
    pbw_status status;

    if (out == NULL) {
        return PBW_ERR_INVALID;
    }
    *out = NULL;
    if (path == NULL) {
        return PBW_ERR_INVALID;
    }
    f = fopen(path, "rb");
    if (f == NULL) {
        return PBW_ERR_INVALID;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return PBW_ERR_INVALID;
    }
    size = ftell(f);
    if (size <= 0) {
        fclose(f);
        return PBW_ERR_INVALID;
    }
    rewind(f);
    desc = malloc((size_t)size);
    if (desc == NULL) {
        fclose(f);
        return PBW_ERR_NOMEM;
    }
    len = fread(desc, 1, (size_t)size, f);
    fclose(f);
    if (len != (size_t)size) {
        free(desc);
        return PBW_ERR_INVALID;
    }
    status = pbw_descriptor_set_load(desc, len, out);
    free(desc);
    return status;
}

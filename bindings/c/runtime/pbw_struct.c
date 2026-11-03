/* Generic struct codec: the C binding runtime. See pbw_struct.h.
 *
 * Direct: the encoder walks the layout table and appends wire bytes, the
 * decoder reads the wire and stores into the struct. No pbw_message is built
 * anywhere. The dynamic core pays one allocation per value plus a second
 * traversal for the tree; a struct codec has the bytes at hand, so skipping
 * the tree is what makes it competitive with generated code (this is the same
 * design the Erlang binding uses in c_src/ep_encoder.c / ep_decoder.c).
 *
 * The schema is the truth for field numbers, wire types and proto2/proto3
 * semantics; the layout only answers "where in memory". Every schema field
 * must have a layout row (a missing row is PBW_ERR_INVALID, so a wrong field
 * number fails loudly instead of dropping data silently).
 *
 * Memory: encode borrows the struct. Decode allocates (strings, arrays,
 * sub-messages) into a zero-initialized struct; release with
 * pbw_struct_free(). Pure C99, no dependencies beyond pbwire itself.
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

static uint64_t
zigzag(uint64_t value, unsigned width)
{
    uint64_t mask = width == 32 ? 0xffffffffull : ~0ull;
    uint64_t sign = width == 32 ? 31 : 63;
    uint64_t u = value & mask;

    return ((u << 1) ^ (0ull - (u >> sign))) & mask;
}

static uint64_t
unzigzag(uint64_t value, unsigned width)
{
    uint64_t mask = width == 32 ? 0xffffffffull : ~0ull;

    return ((value >> 1) ^ (0ull - (value & 1))) & mask;
}

/* Value as it goes on the wire, before tag and length. */
static uint64_t
wire_value(pbw_type type, uint64_t bits)
{
    switch (type) {
    case PBW_TYPE_INT32:
    case PBW_TYPE_ENUM:
        return (uint64_t)(int64_t)(int32_t)bits;
    case PBW_TYPE_UINT32:
        return (uint32_t)bits;
    case PBW_TYPE_SINT32:
        return zigzag(bits, 32);
    case PBW_TYPE_SINT64:
        return zigzag(bits, 64);
    case PBW_TYPE_BOOL:
        return bits != 0 ? 1u : 0u;
    default:
        return bits;
    }
}

static unsigned
wire_type(pbw_type type)
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
    case PBW_TYPE_MESSAGE:
        return 2;
    default:
        return 0;
    }
}

static int
packable(pbw_type type)
{
    return type != PBW_TYPE_STRING && type != PBW_TYPE_MESSAGE &&
           type != PBW_TYPE_GROUP;
}

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
    case PBW_SF_MESSAGE:
        return type == PBW_TYPE_MESSAGE;
    }
    return 0;
}

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
    [PBW_TYPE_MESSAGE] = PBW_SF_MESSAGE,
    [PBW_TYPE_UINT32] = PBW_SF_UINT32,
    [PBW_TYPE_ENUM] = PBW_SF_ENUM,
    [PBW_TYPE_SFIXED32] = PBW_SF_INT32,
    [PBW_TYPE_SFIXED64] = PBW_SF_INT64,
    [PBW_TYPE_SINT32] = PBW_SF_INT32,
    [PBW_TYPE_SINT64] = PBW_SF_INT64,
};

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
            row->wire != wire_type((pbw_type)row->type) ||
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
            out->wire = wire_type(out->type);
            out->packed = pbw_field_is_packed(field) != 0;
            out->required = pbw_field_label(field) == PBW_LABEL_REQUIRED;
            return 1;
        }
    }
    return 0;
}

/* A generated layout row describes itself; expected_kind and wire_type above
 * are what keeps a hand-edited row from writing the wrong bytes or walking
 * past the struct. The one thing a single pass cannot see is a row that is not
 * there at all, so the entry points compare the row count with the schema. */

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
        cap += cap / 2;
    }
    data = realloc(b->data, cap);
    if (data == NULL) {
        b->status = PBW_ERR_NOMEM;
        return 0;
    }
    b->data = data;
    b->cap = cap;
    return 1;
}

static void
sb_byte(sbuf *b, uint8_t value)
{
    if (sb_reserve(b, 1)) {
        b->data[b->len++] = value;
    }
}

/* Reserves the maximum width so the value is written in place. Encoding only
 * appends, so the slack past b->len is never read. */
static void
sb_varint(sbuf *b, uint64_t value)
{
    if (sb_reserve(b, 10)) {
        b->len = (size_t)(put_varint(b->data + b->len, value) - b->data);
    }
}

/* Same writes without a capacity check. A field reserves tag (5) plus value
 * (10) once, which is the widest a scalar can be, then writes inline. */
static void
sb_varint_raw(sbuf *b, uint64_t value)
{
    b->len = (size_t)(put_varint(b->data + b->len, value) - b->data);
}

static void
sb_tag_raw(sbuf *b, uint32_t fnum, unsigned wt)
{
    sb_varint_raw(b, ((uint64_t)fnum << 3) | wt);
}

static void
sb_fixed_raw(sbuf *b, uint64_t value, size_t width)
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

static void
sb_tag(sbuf *b, uint32_t fnum, unsigned wt)
{
    sb_varint(b, ((uint64_t)fnum << 3) | wt);
}

/* A length-delimited payload needs its length first, but it is only known
 * afterwards: write a one-byte placeholder, encode, then patch it and shift
 * the payload right if the length needs more than one byte. */
static void
sb_len_begin(sbuf *b, size_t *mark)
{
    *mark = b->len;
    sb_byte(b, 0);
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

/* ------------------------------------------------------------------ layout */

/* Wire order usually repeats a field a few times in a row (repeated fields,
 * one per element), so the last hit is checked first. */
static const pbw_struct_field_t *
find_row_from(const pbw_struct_field_t *fields, size_t nfields, uint32_t fnum,
              size_t *cursor)
{
    size_t i = *cursor;

    if (i < nfields && fields[i].fnum == fnum) {
        return &fields[i];
    }
    for (i = 0; i < nfields; i++) {
        if (fields[i].fnum == fnum) {
            *cursor = i;
            return &fields[i];
        }
    }
    return NULL;
}

static size_t
scalar_size(pbw_struct_kind_t kind)
{
    switch (kind) {
    case PBW_SF_INT32:
    case PBW_SF_UINT32:
    case PBW_SF_FLOAT:
    case PBW_SF_ENUM:
        return 4;
    case PBW_SF_INT64:
    case PBW_SF_UINT64:
    case PBW_SF_DOUBLE:
        return 8;
    case PBW_SF_BOOL:
        return 1;
    default:
        return 0;
    }
}

/* ---------------------------------------------------------------- encoding */

static uint64_t
read_bits(pbw_struct_kind_t kind, const void *p)
{
    uint32_t u32;
    uint64_t u64;

    switch (kind) {
    case PBW_SF_BOOL: {
        unsigned char b;

        memcpy(&b, p, 1);
        return b != 0;
    }
    case PBW_SF_INT32:
    case PBW_SF_UINT32:
    case PBW_SF_ENUM:
    case PBW_SF_FLOAT:
        memcpy(&u32, p, 4);
        return u32;
    default:
        memcpy(&u64, p, 8);
        return u64;
    }
}

/* Caller must have reserved 10 bytes. */
static void
enc_scalar_raw(sbuf *b, pbw_type type, pbw_struct_kind_t kind, const void *p)
{
    uint64_t bits = read_bits(kind, p);

    switch (type) {
    case PBW_TYPE_FLOAT:
    case PBW_TYPE_FIXED32:
    case PBW_TYPE_SFIXED32:
        sb_fixed_raw(b, bits, 4);
        return;
    case PBW_TYPE_DOUBLE:
    case PBW_TYPE_FIXED64:
    case PBW_TYPE_SFIXED64:
        sb_fixed_raw(b, bits, 8);
        return;
    default:
        sb_varint_raw(b, wire_value(type, bits));
        return;
    }
}

static void
enc_len_bytes(sbuf *b, uint32_t fnum, const char *data, size_t len)
{
    if (!sb_reserve(b, 5 + 10 + len)) {
        return;
    }
    sb_tag_raw(b, fnum, 2);
    sb_varint_raw(b, len);
    if (len > 0) {
        memcpy(b->data + b->len, data, len);
        b->len += len;
    }
}

static void
enc_msg(sbuf *b, const pbw_descriptor *desc,
        const pbw_struct_field_t *fields, size_t nfields, const void *base,
        unsigned depth)
{
    size_t i;

    for (i = 0; i < nfields && b->status == PBW_OK; i++) {
        const pbw_struct_field_t *row = &fields[i];
        const char *p = (const char *)base + row->offset;
        rinfo info;
        pbw_type type;
        int repeated = row->repeated != 0;
        size_t j, n;

        if (!row_info(row, desc, &info)) {
            b->status = PBW_ERR_INVALID;
            return;
        }
        type = info.type;
        if (type == PBW_TYPE_GROUP) {
            b->status = PBW_ERR_INVALID; /* layout has no group support */
            return;
        }

        if (type == PBW_TYPE_STRING) {
            if (!repeated) {
                const char *s = *(const char *const *)p;

                if (s == NULL) {
                    if (info.required) {
                        b->status = PBW_ERR_REQUIRED;
                        return;
                    }
                    continue;
                }
                enc_len_bytes(b, row->fnum, s, strlen(s));
                continue;
            }
            n = *(const size_t *)((const char *)base + row->count);
            {
                const char *const *items = *(const char *const *const *)p;

                if (n > 0 && items == NULL) {
                    b->status = PBW_ERR_INVALID;
                    return;
                }
                for (j = 0; j < n; j++) {
                    if (items[j] == NULL) {
                        enc_len_bytes(b, row->fnum, "", 0);
                    } else {
                        enc_len_bytes(b, row->fnum, items[j],
                                      strlen(items[j]));
                    }
                }
            }
            continue;
        }

        if (type == PBW_TYPE_MESSAGE) {
            if (!repeated) {
                const void *item = *(void *const *)p;
                size_t mark;

                if (item == NULL) {
                    if (info.required) {
                        b->status = PBW_ERR_REQUIRED;
                        return;
                    }
                    continue;
                }
                if (!sb_reserve(b, 5 + 1)) {
                    return;
                }
                sb_tag_raw(b, row->fnum, 2);
                mark = b->len;
                b->data[b->len++] = 0;
                enc_msg(b, desc, row->fields, row->nfields, item,
                        depth + 1);
                sb_len_end(b, mark);
                continue;
            }
            n = *(const size_t *)((const char *)base + row->count);
            {
                const char *items = *(const char *const *)p;

                if (n > 0 && items == NULL) {
                    b->status = PBW_ERR_INVALID;
                    return;
                }
                for (j = 0; j < n; j++) {
                    size_t mark;

                    if (!sb_reserve(b, 5 + 1)) {
                        return;
                    }
                    sb_tag_raw(b, row->fnum, 2);
                    mark = b->len;
                    b->data[b->len++] = 0;
                    enc_msg(b, desc, row->fields, row->nfields,
                            items + j * row->elem, depth + 1);
                    sb_len_end(b, mark);
                }
            }
            continue;
        }

        if (!repeated) {
            if (row->has != 0 &&
                *(const int *)((const char *)base + row->has) == 0) {
                continue;
            }
            if (!sb_reserve(b, 15)) {
                return;
            }
            sb_tag_raw(b, row->fnum, info.wire);
            enc_scalar_raw(b, type, row->kind, p);
            continue;
        }

        n = *(const size_t *)((const char *)base + row->count);
        if (info.packed && packable(type)) {
            size_t mark, width = scalar_size(row->kind);
            const char *items = *(const char *const *)p;

            if (n == 0) {
                continue;
            }
            sb_tag(b, row->fnum, 2);
            sb_len_begin(b, &mark);
            for (j = 0; j < n; j++) {
                if (!sb_reserve(b, 10)) {
                    return;
                }
                enc_scalar_raw(b, type, row->kind, items + j * width);
            }
            sb_len_end(b, mark);
            continue;
        }
        {
            size_t width = scalar_size(row->kind);
            const char *items = *(const char *const *)p;

            if (n > 0 && items == NULL) {
                b->status = PBW_ERR_INVALID;
                return;
            }
            for (j = 0; j < n; j++) {
                if (!sb_reserve(b, 15)) {
                    return;
                }
                sb_tag_raw(b, row->fnum, info.wire);
                enc_scalar_raw(b, type, row->kind, items + j * width);
            }
        }
    }
}

pbw_status
pbw_struct_encode(const pbw_schema *schema, const char *message,
                  const pbw_struct_field_t *fields, size_t nfields,
                  const void *base, uint8_t **wire, size_t *wire_len)
{
    const pbw_descriptor *desc;
    sbuf b;

    if (wire == NULL || wire_len == NULL) {
        return PBW_ERR_INVALID;
    }
    *wire = NULL;
    *wire_len = 0;
    if (schema == NULL || message == NULL || fields == NULL || base == NULL) {
        return PBW_ERR_INVALID;
    }
    desc = pbw_schema_find(schema, message);
    if (desc == NULL) {
        return PBW_ERR_INVALID;
    }
    if (pbw_descriptor_field_count(desc) != nfields) {
        return PBW_ERR_INVALID;
    }
    memset(&b, 0, sizeof(b));
    enc_msg(&b, desc, fields, nfields, base, 0);
    if (b.status != PBW_OK) {
        free(b.data);
        return b.status;
    }
    *wire = b.data;
    *wire_len = b.len;
    return PBW_OK;
}

/* ---------------------------------------------------------------- decoding */

typedef struct {
    const uint8_t *p;
    const uint8_t *end;
} rdr;

static int
rd_bytes(rdr *r, size_t n)
{
    if ((size_t)(r->end - r->p) < n) {
        return 0;
    }
    r->p += n;
    return 1;
}

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
        return rd_bytes(r, 8);
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
        return rd_bytes(r, 4);
    default:
        return 0; /* stray END_GROUP */
    }
}

static void
store_varint(pbw_struct_kind_t kind, pbw_type type, void *p, uint64_t value)
{
    int32_t i32;
    uint32_t u32;
    int64_t i64;
    uint64_t u64;

    switch (type) {
    case PBW_TYPE_SINT32:
        value = unzigzag(value, 32);
        break;
    case PBW_TYPE_SINT64:
        value = unzigzag(value, 64);
        break;
    default:
        break;
    }
    switch (kind) {
    case PBW_SF_INT32:
    case PBW_SF_ENUM:
        i32 = (int32_t)value;
        memcpy(p, &i32, 4);
        return;
    case PBW_SF_UINT32:
        u32 = (uint32_t)value;
        memcpy(p, &u32, 4);
        return;
    case PBW_SF_BOOL: {
        unsigned char b = value != 0 ? 1 : 0;

        memcpy(p, &b, 1);
        return;
    }
    case PBW_SF_INT64:
        i64 = (int64_t)value;
        memcpy(p, &i64, 8);
        return;
    default:
        u64 = value;
        memcpy(p, &u64, 8);
        return;
    }
}

static void
store_fixed(pbw_struct_kind_t kind, void *p, uint64_t bits)
{
    uint32_t u32 = (uint32_t)bits;

    if (scalar_size(kind) == 4) {
        memcpy(p, &u32, 4);
    } else {
        memcpy(p, &bits, 8);
    }
}

/* A packed run carries the elements' wire type in the length-delimited form,
 * so it does not match wire_type() of the element type. */
static int
packed_run(const pbw_struct_field_t *row, pbw_type type, unsigned wt)
{
    return wt == 2 && row->repeated != 0 && packable(type);
}

/* Repeated fields grow by doubling: the count member is both the fill index
 * and (via powers of two) the capacity, so no sizing pass is needed and a
 * message with empty repeated fields allocates nothing. */
static void *
append_slot(const pbw_struct_field_t *row, void *base, size_t width)
{
    size_t *count = (size_t *)((char *)base + row->count);
    size_t idx = *count;
    char **array = (char **)((char *)base + row->offset);

    if (idx == 0) {
        *array = malloc(8 * width);
        if (*array == NULL) {
            return NULL;
        }
    } else if ((idx & (idx - 1)) == 0 && idx >= 8) {
        char *grown = realloc(*array, idx * 2 * width);

        if (grown == NULL) {
            return NULL;
        }
        *array = grown;
    }
    *count = idx + 1;
    return *array + idx * width;
}

/* Absent enums fall back to their schema default. Pre-filled before the wire
 * is read, so a decoded value simply overwrites it. */
static size_t
seed_enum_defaults(const pbw_struct_field_t *fields, size_t nfields,
                   void *base)
{
    size_t i, required = 0;

    for (i = 0; i < nfields; i++) {
        const pbw_struct_field_t *row = &fields[i];

        if (row->kind == PBW_SF_ENUM && !row->repeated) {
            memcpy((char *)base + row->offset, &row->dflt, 4);
        }
        required += row->required != 0;
    }
    return required;
}

/* proto2 required fields must be present; a required scalar without a has-flag
 * is always written by the encoder, so only pointers and counts can be absent.
 * Only called for message types that have required fields at all. */
static pbw_status
check_required(const pbw_struct_field_t *fields, size_t nfields, const void *base)
{
    size_t i;

    for (i = 0; i < nfields; i++) {
        const pbw_struct_field_t *row = &fields[i];
        rinfo info;
        const char *p;

        if (!row->required || !row_info(row, NULL, &info)) {
            continue;
        }
        p = (const char *)base + row->offset;
        if (row->repeated) {
            if (*(const size_t *)((const char *)base + row->count) == 0) {
                return PBW_ERR_REQUIRED;
            }
            continue;
        }
        if (info.type == PBW_TYPE_STRING || info.type == PBW_TYPE_MESSAGE) {
            if (*(void *const *)p == NULL) {
                return PBW_ERR_REQUIRED;
            }
        }
    }
    return PBW_OK;
}

static pbw_status
dec_msg(const pbw_descriptor *desc, const pbw_struct_field_t *fields,
        size_t nfields, const uint8_t *wire, size_t wire_len, void *base,
        unsigned depth)
{
    rdr r;
    size_t cursor = 0;
    size_t required;
    pbw_status status;

    if (depth >= SC_MAX_DEPTH) {
        return PBW_ERR_LIMIT;
    }
    required = seed_enum_defaults(fields, nfields, base);
    r.p = wire;
    r.end = wire + wire_len;
    while (r.p < r.end) {
        uint32_t fnum;
        unsigned wt;
        const pbw_struct_field_t *row;
        rinfo info;
        pbw_type type;
        char *p;
        char *slot;
        uint64_t value;

        if (!rd_tag(&r, &fnum, &wt)) {
            return PBW_ERR_INVALID;
        }
        row = find_row_from(fields, nfields, fnum, &cursor);
        if (row == NULL || !row_info(row, desc, &info)) {
            if (!rd_skip(&r, fnum, wt, depth)) {
                return PBW_ERR_INVALID;
            }
            continue;
        }
        type = info.type;
        p = (char *)base + row->offset;
        if (wt != info.wire && !packed_run(row, type, wt)) {
            /* Known field, wrong wire type: keep it out of the struct. */
            if (!rd_skip(&r, fnum, wt, depth)) {
                return PBW_ERR_INVALID;
            }
            continue;
        }

        if (type == PBW_TYPE_STRING) {
            uint64_t len;
            char *dup;

            if (!rd_varint(&r, &len) || len > (uint64_t)(r.end - r.p)) {
                return PBW_ERR_INVALID;
            }
            dup = malloc((size_t)len + 1);
            if (dup == NULL) {
                return PBW_ERR_NOMEM;
            }
            memcpy(dup, r.p, (size_t)len);
            dup[len] = '\0';
            r.p += (size_t)len;
            if (row->repeated) {
                char **slot = append_slot(row, base, sizeof(char *));

                if (slot == NULL) {
                    free(dup);
                    return PBW_ERR_NOMEM;
                }
                *slot = dup;
            } else {
                *(char **)p = dup;
            }
            continue;
        }

        if (type == PBW_TYPE_MESSAGE) {
            uint64_t len;
            void *slot;

            if (!rd_varint(&r, &len) || len > (uint64_t)(r.end - r.p)) {
                return PBW_ERR_INVALID;
            }
            if (row->repeated) {
                slot = append_slot(row, base, row->elem);
                if (slot == NULL) {
                    return PBW_ERR_NOMEM;
                }
                memset(slot, 0, row->elem);
            } else {
                slot = calloc(1, row->elem);
                if (slot == NULL) {
                    return PBW_ERR_NOMEM;
                }
                *(void **)p = slot;
            }
            status = dec_msg(desc, row->fields, row->nfields, r.p,
                             (size_t)len, slot, depth + 1);
            if (status != PBW_OK) {
                return status;
            }
            r.p += (size_t)len;
            continue;
        }

        if (packed_run(row, type, wt)) {
            /* Packed run for a repeated numeric field. */
            uint64_t len;
            rdr inner;
            size_t width = scalar_size(row->kind);

            if (!rd_varint(&r, &len) || len > (uint64_t)(r.end - r.p)) {
                return PBW_ERR_INVALID;
            }
            inner.p = r.p;
            inner.end = r.p + (size_t)len;
            while (inner.p < inner.end) {
                uint64_t bits;

                if (info.wire == 0) {
                    if (!rd_varint(&inner, &bits)) {
                        return PBW_ERR_INVALID;
                    }
                    slot = append_slot(row, base, width);
                    if (slot == NULL) {
                        return PBW_ERR_NOMEM;
                    }
                    store_varint(row->kind, type, slot, bits);
                } else {
                    if (!rd_fixed(&inner, &bits, info.wire == 1 ? 8 : 4)) {
                        return PBW_ERR_INVALID;
                    }
                    slot = append_slot(row, base, width);
                    if (slot == NULL) {
                        return PBW_ERR_NOMEM;
                    }
                    store_fixed(row->kind, slot, bits);
                }
            }
            r.p = inner.end;
            continue;
        }

        if (wt == 0) {
            if (!rd_varint(&r, &value)) {
                return PBW_ERR_INVALID;
            }
        } else {
            size_t width = wt == 1 ? 8 : 4;

            if (!rd_fixed(&r, &value, width)) {
                return PBW_ERR_INVALID;
            }
        }
        if (row->repeated) {
            size_t width = scalar_size(row->kind);

            slot = append_slot(row, base, width);
            if (slot == NULL) {
                return PBW_ERR_NOMEM;
            }
            if (wt == 0) {
                store_varint(row->kind, type, slot, value);
            } else {
                store_fixed(row->kind, slot, value);
            }
            continue;
        }
        if (wt == 0) {
            store_varint(row->kind, type, p, value);
        } else {
            store_fixed(row->kind, p, value);
        }
        if (row->has != 0) {
            *(int *)((char *)base + row->has) = 1;
        }
    }

    return required == 0 ? PBW_OK : check_required(fields, nfields, base);
}

pbw_status
pbw_struct_decode(const pbw_schema *schema, const char *message,
                  const pbw_struct_field_t *fields, size_t nfields,
                  const uint8_t *wire, size_t wire_len, void *base)
{
    const pbw_descriptor *desc;
    pbw_status status;

    if (schema == NULL || message == NULL || fields == NULL || base == NULL ||
        (wire == NULL && wire_len != 0)) {
        return PBW_ERR_INVALID;
    }
    desc = pbw_schema_find(schema, message);
    if (desc == NULL) {
        return PBW_ERR_INVALID;
    }
    if (pbw_descriptor_field_count(desc) != nfields) {
        return PBW_ERR_INVALID;
    }
    status = dec_msg(desc, fields, nfields, wire, wire_len, base, 0);
    if (status != PBW_OK) {
        pbw_struct_free(fields, nfields, base);
    }
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

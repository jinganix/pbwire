/* Malformed-input robustness: illegal wire bytes, truncated frames and hostile
 * API arguments must return a status, never crash. Deterministic (fixed-seed
 * xorshift), so `make test` replays the same stream everywhere. */

#include "pbwire.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        if (!(cond)) {                                                               \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                              \
        }                                                                            \
    } while (0)

#define CHECK_STATUS(expect, expr)                                              \
    do {                                                                        \
        pbw_status status_ = (expr);                                            \
        if (status_ != (expect)) {                                              \
            fprintf(stderr, "%s:%d: expected %s, got %s\n", __FILE__, __LINE__, \
                    pbw_strerror(expect), pbw_strerror(status_));               \
            failures++;                                                         \
        }                                                                       \
    } while (0)

static uint64_t rng_state = 0x9e3779b97f4a7c15ull;

static uint64_t
rnd(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

static pbw_schema *
build_schema(void)
{
    pbw_schema *s = pbw_schema_new();
    pbw_descriptor *m;
    pbw_descriptor *sub;
    pbw_descriptor *g;
    pbw_field *mf;
    pbw_field *mr;
    pbw_field *gf;

    if (s == NULL) {
        return NULL;
    }
    m = pbw_schema_add_message(s, "Fuzz");
    sub = pbw_schema_add_message(s, "Fuzz.Sub");
    g = pbw_schema_add_message(s, "Fuzz.G");
    if (m == NULL || sub == NULL || g == NULL) {
        pbw_schema_free(s);
        return NULL;
    }
    CHECK(pbw_message_add_field(m, "i32", 1, PBW_TYPE_INT32,
                                PBW_LABEL_OPTIONAL) != NULL);
    CHECK(pbw_message_add_field(m, "s", 2, PBW_TYPE_STRING,
                                PBW_LABEL_OPTIONAL) != NULL);
    CHECK(pbw_message_add_field(m, "rep", 3, PBW_TYPE_INT32,
                                PBW_LABEL_REPEATED) != NULL);
    /* Explicitly unpacked: packed bytes for it take the slow packed path. */
    {
        pbw_field *up = pbw_message_add_field(m, "up", 31,
                                              PBW_TYPE_INT32,
                                              PBW_LABEL_REPEATED);

        CHECK(up != NULL);
        if (up != NULL) {
            pbw_field_set_packed(up, 0);
        }
    }
    CHECK(pbw_message_add_field(m, "big", 16, PBW_TYPE_STRING,
                                PBW_LABEL_OPTIONAL) != NULL);
    CHECK(pbw_message_add_field(m, "huge", 2048, PBW_TYPE_INT32,
                                PBW_LABEL_OPTIONAL) != NULL);
    CHECK(pbw_message_add_field(m, "maxf", 536870911u, PBW_TYPE_INT32,
                                PBW_LABEL_OPTIONAL) != NULL);
    /* every fixed-width and zigzag scalar, for truncated-value branches */
    CHECK(pbw_message_add_field(m, "f32", 7, PBW_TYPE_FIXED32,
                                PBW_LABEL_OPTIONAL) != NULL);
    CHECK(pbw_message_add_field(m, "f64", 8, PBW_TYPE_FIXED64,
                                PBW_LABEL_OPTIONAL) != NULL);
    CHECK(pbw_message_add_field(m, "flt", 9, PBW_TYPE_FLOAT,
                                PBW_LABEL_OPTIONAL) != NULL);
    CHECK(pbw_message_add_field(m, "dbl", 10, PBW_TYPE_DOUBLE,
                                PBW_LABEL_OPTIONAL) != NULL);
    CHECK(pbw_message_add_field(m, "s32", 11, PBW_TYPE_SINT32,
                                PBW_LABEL_OPTIONAL) != NULL);
    CHECK(pbw_message_add_field(m, "s64", 12, PBW_TYPE_SINT64,
                                PBW_LABEL_OPTIONAL) != NULL);
    CHECK(pbw_message_add_field(m, "flag", 13, PBW_TYPE_BOOL,
                                PBW_LABEL_OPTIONAL) != NULL);
    CHECK(pbw_message_add_field(m, "u32", 14, PBW_TYPE_UINT32,
                                PBW_LABEL_OPTIONAL) != NULL);
    /* 5-byte tags (field number >= 2^25): the reserving writer paths */
    CHECK(pbw_message_add_field(m, "f32h", 33554432u,
                                PBW_TYPE_FIXED32,
                                PBW_LABEL_OPTIONAL) != NULL);
    CHECK(pbw_message_add_field(m, "u64h", 33554433u,
                                PBW_TYPE_UINT64,
                                PBW_LABEL_OPTIONAL) != NULL);
    CHECK(pbw_message_add_field(m, "f64h", 33554440u,
                                PBW_TYPE_FIXED64,
                                PBW_LABEL_OPTIONAL) != NULL);
    CHECK(pbw_message_add_field(m, "bigh", 33554441u,
                                PBW_TYPE_BYTES,
                                PBW_LABEL_OPTIONAL) != NULL);
    mf = pbw_message_add_field(m, "mh", 33554442u, PBW_TYPE_MESSAGE,
                               PBW_LABEL_OPTIONAL);
    CHECK(mf != NULL);
    if (mf != NULL) {
        pbw_field_set_type_name(mf, "Fuzz.Sub");
    }
    /* 11 more scalars push the field count past 32, so encode and size
     * take the slow present-bit path instead of the single-word fast path */
    CHECK(pbw_message_add_field(m, "p20", 20, PBW_TYPE_INT32,
                                PBW_LABEL_OPTIONAL) != NULL);
    CHECK(pbw_message_add_field(m, "p21", 21, PBW_TYPE_INT32,
                                PBW_LABEL_OPTIONAL) != NULL);
    CHECK(pbw_message_add_field(m, "p22", 22, PBW_TYPE_INT32,
                                PBW_LABEL_OPTIONAL) != NULL);
    CHECK(pbw_message_add_field(m, "p23", 23, PBW_TYPE_INT32,
                                PBW_LABEL_OPTIONAL) != NULL);
    CHECK(pbw_message_add_field(m, "p24", 24, PBW_TYPE_INT32,
                                PBW_LABEL_OPTIONAL) != NULL);
    CHECK(pbw_message_add_field(m, "p25", 25, PBW_TYPE_INT32,
                                PBW_LABEL_OPTIONAL) != NULL);
    CHECK(pbw_message_add_field(m, "p26", 26, PBW_TYPE_INT32,
                                PBW_LABEL_OPTIONAL) != NULL);
    CHECK(pbw_message_add_field(m, "p27", 27, PBW_TYPE_INT32,
                                PBW_LABEL_OPTIONAL) != NULL);
    CHECK(pbw_message_add_field(m, "p28", 28, PBW_TYPE_INT32,
                                PBW_LABEL_OPTIONAL) != NULL);
    CHECK(pbw_message_add_field(m, "p29", 29, PBW_TYPE_INT32,
                                PBW_LABEL_OPTIONAL) != NULL);
    CHECK(pbw_message_add_field(m, "p30", 30, PBW_TYPE_INT32,
                                PBW_LABEL_OPTIONAL) != NULL);
    /* huge-tag packed runs for the reserving packed writer */
    {
        pbw_field *rp;

        rp = pbw_message_add_field(m, "rep2", 33554434u,
                                   PBW_TYPE_INT32,
                                   PBW_LABEL_REPEATED);
        CHECK(rp != NULL);
        pbw_field_set_packed(rp, 1);
        rp = pbw_message_add_field(m, "rep3", 33554435u,
                                   PBW_TYPE_BOOL, PBW_LABEL_REPEATED);
        CHECK(rp != NULL);
        pbw_field_set_packed(rp, 1);
        rp = pbw_message_add_field(m, "rep4", 33554436u,
                                   PBW_TYPE_FIXED32,
                                   PBW_LABEL_REPEATED);
        CHECK(rp != NULL);
        pbw_field_set_packed(rp, 1);
        rp = pbw_message_add_field(m, "rep5", 33554437u,
                                   PBW_TYPE_DOUBLE,
                                   PBW_LABEL_REPEATED);
        CHECK(rp != NULL);
        pbw_field_set_packed(rp, 1);
    }
    CHECK(pbw_message_add_field(sub, "x", 1, PBW_TYPE_INT32,
                                PBW_LABEL_OPTIONAL) != NULL);
    mf = pbw_message_add_field(sub, "sub", 2, PBW_TYPE_MESSAGE,
                               PBW_LABEL_OPTIONAL);
    CHECK(mf != NULL);
    if (mf != NULL) {
        pbw_field_set_type_name(mf, "Fuzz.Sub");
    }
    mf = pbw_message_add_field(m, "sub", 4, PBW_TYPE_MESSAGE,
                               PBW_LABEL_OPTIONAL);
    CHECK(mf != NULL);
    /* Set the type name before the next add: the field array can move. */
    if (mf != NULL) {
        pbw_field_set_type_name(mf, "Fuzz.Sub");
    }
    mr = pbw_message_add_field(m, "subs", 6, PBW_TYPE_MESSAGE,
                               PBW_LABEL_REPEATED);
    CHECK(mr != NULL);
    if (mr != NULL) {
        pbw_field_set_type_name(mr, "Fuzz.Sub");
    }
    CHECK(pbw_message_add_field(g, "y", 1, PBW_TYPE_INT32,
                                PBW_LABEL_OPTIONAL) != NULL);
    gf = pbw_message_add_field(m, "grp", 5, PBW_TYPE_GROUP,
                               PBW_LABEL_OPTIONAL);
    CHECK(gf != NULL);
    if (gf != NULL) {
        pbw_field_set_type_name(gf, "Fuzz.G");
    }
    if (pbw_schema_link(s) != PBW_OK) {
        pbw_schema_free(s);
        return NULL;
    }
    return s;
}

/* Decodes `len` bytes; on success re-encodes and checks the size helpers so a
 * decoded tree can never crash the encoder. */
static void
try_decode(const pbw_descriptor *d, const uint8_t *buf, size_t len)
{
    pbw_message *m = NULL;
    pbw_status st = pbw_decode(d, buf, len, &m);

    if (st == PBW_OK) {
        uint8_t *o = NULL;
        size_t ol = 0;
        size_t el = 0;

        CHECK(m != NULL);
        CHECK(pbw_encode(m, &o, &ol) == PBW_OK);
        pbw_free(o);
        CHECK(pbw_message_encoded_size(m, &el) == PBW_OK);
        if (el > 0) {
            uint8_t *tmp = (uint8_t *)malloc(el + 16);
            size_t wl = 0;

            CHECK(tmp != NULL);
            if (tmp != NULL) {
                CHECK(pbw_encode_to(m, tmp, el + 16, &wl) == PBW_OK);
                CHECK(wl <= el + 16);
                /* Too small must fail cleanly, not crash. */
                if (el > 1) {
                    CHECK(pbw_encode_to(m, tmp, el - 1, &wl) ==
                          PBW_ERR_LIMIT);
                }
                free(tmp);
            }
        }
        pbw_message_free(m);
    } else {
        CHECK(m == NULL);
    }
}

static void
test_vectors(const pbw_descriptor *d)
{
    static const uint8_t truncated[] = {0x08, 0x80, 0x80, 0x80};
    static const uint8_t varint10[] = {0x08, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                       0xFF, 0xFF, 0xFF, 0xFF, 0x7F};
    static const uint8_t varint11[] = {0x08, 0x80, 0x80, 0x80, 0x80, 0x80,
                                       0x80, 0x80, 0x80, 0x80, 0x80, 0x01};
    static const uint8_t field0[] = {0x00};
    static const uint8_t wire6[] = {0x0E, 0x01};
    static const uint8_t wire7[] = {0x0F, 0x01};
    static const uint8_t tag5[] = {0xFF, 0xFF, 0xFF, 0xFF, 0x7F, 0x01};
    static const uint8_t tag6[] = {0x80, 0x80, 0x80,
                                   0x80, 0x80, 0x01};
    static const uint8_t huge_len[] = {0x12, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F,
                                       0x41};
    static const uint8_t len6[] = {0x12, 0x80, 0x80, 0x80,
                                   0x80, 0x80, 0x01};
    static const uint8_t open_group[] = {0x2B, 0x08, 0x01};
    static const uint8_t bad_end[] = {0x2B, 0x08, 0x01, 0x2C};
    static const uint8_t packed_bad[] = {0x1A, 0x03, 0x80, 0x80};
    static const uint8_t one_ff[] = {0xFF};

    try_decode(d, truncated, sizeof(truncated));
    try_decode(d, varint10, sizeof(varint10));
    try_decode(d, varint11, sizeof(varint11));
    try_decode(d, field0, sizeof(field0));
    try_decode(d, wire6, sizeof(wire6));
    try_decode(d, wire7, sizeof(wire7));
    try_decode(d, tag5, sizeof(tag5));
    try_decode(d, tag6, sizeof(tag6));
    try_decode(d, huge_len, sizeof(huge_len));
    try_decode(d, len6, sizeof(len6));
    try_decode(d, open_group, sizeof(open_group));
    try_decode(d, bad_end, sizeof(bad_end));
    try_decode(d, packed_bad, sizeof(packed_bad));
    try_decode(d, one_ff, sizeof(one_ff));
    /* truncated fixed-width and zigzag values, per scalar type */
    {
        static const uint8_t t_f32[] = {0x3D, 0x01, 0x02};
        static const uint8_t t_f64[] = {0x41, 0x01, 0x02, 0x03,
                                        0x04, 0x05};
        static const uint8_t t_flt[] = {0x4D, 0x01};
        static const uint8_t t_dbl[] = {0x51, 0x01, 0x02, 0x03,
                                        0x04, 0x05, 0x06, 0x07};
        static const uint8_t t_s32[] = {0x58, 0x80};
        static const uint8_t t_s64[] = {0x60, 0x80, 0x80};
        /* 5-byte tag (field 33554432, fixed32) + 2 value bytes */
        static const uint8_t t_f32h[] = {0x85, 0x80, 0x80,
                                         0x80, 0x01, 0x01, 0x02};
        /* truncated varint for the remaining scalar actions */
        static const uint8_t t_bool[] = {0x68, 0x80};
        static const uint8_t t_u32[] = {0x70, 0x80, 0x80};
        /* corrupt inner message: outer lengths are fine, field 1 of Sub
         * is cut off, so the whole decode fails with nothing out */
        static const uint8_t nested_bad[] = {0x22, 0x02, 0x08, 0x80};
        /* the same Sub twice: singular messages merge instead of failing */
        static const uint8_t nested_dup[] = {0x22, 0x02, 0x08, 0x01,
                                             0x22, 0x02, 0x08, 0x02};
        /* valid bytes for the huge-tagged bytes field */
        static const uint8_t big_ok[] = {0xCA, 0x80, 0x80, 0x80, 0x01,
                                         0x03, 0x41, 0x42, 0x43};
        /* truncated message length: field 4 claims 5 bytes, 2 follow */
        static const uint8_t msg_cut[] = {0x22, 0x05, 0x08, 0x01};
        /* packed bytes for the explicitly unpacked field */
        static const uint8_t up_packed[] = {0xFA, 0x01, 0x02, 0x01,
                                            0x02};

        try_decode(d, t_f32, sizeof(t_f32));
        try_decode(d, t_f64, sizeof(t_f64));
        try_decode(d, t_flt, sizeof(t_flt));
        try_decode(d, t_dbl, sizeof(t_dbl));
        try_decode(d, t_s32, sizeof(t_s32));
        try_decode(d, t_s64, sizeof(t_s64));
        try_decode(d, t_f32h, sizeof(t_f32h));
        try_decode(d, t_bool, sizeof(t_bool));
        try_decode(d, t_u32, sizeof(t_u32));
        try_decode(d, nested_bad, sizeof(nested_bad));
        try_decode(d, nested_dup, sizeof(nested_dup));
        try_decode(d, big_ok, sizeof(big_ok));
        try_decode(d, msg_cut, sizeof(msg_cut));
        try_decode(d, up_packed, sizeof(up_packed));
        /* empty packed run, valid uint32, more truncated scalars */
        {
            static const uint8_t empty_packed[] = {0x1A, 0x00};
            static const uint8_t u32_ok[] = {0x70, 0x96, 0x01};
            static const uint8_t i32_cut[] = {0x08, 0x80};
            static const uint8_t f32_cut[] = {0x3D, 0x01, 0x02};
            static const uint8_t str_cut[] = {0x12, 0x05, 0x41, 0x42};

            try_decode(d, empty_packed, sizeof(empty_packed));
            try_decode(d, u32_ok, sizeof(u32_ok));
            try_decode(d, i32_cut, sizeof(i32_cut));
            try_decode(d, f32_cut, sizeof(f32_cut));
            try_decode(d, str_cut, sizeof(str_cut));
        }
        /* duplicate singular submessages merge: the last value wins */
        {
            pbw_message *m = NULL;
            pbw_value *sub = NULL;

            CHECK_STATUS(PBW_OK,
                         pbw_decode(d, nested_dup, sizeof(nested_dup),
                                    &m));
            if (m != NULL) {
                sub = pbw_message_get(m, 4);
                CHECK(sub != NULL &&
                      pbw_value_get_int32(
                          pbw_message_get(pbw_value_get_message(sub),
                                          1)) == 2);
                pbw_message_free(m);
            }
        }
    }
    try_decode(d, NULL, 0);
    try_decode(d, (const uint8_t *)"", 0);
    {
        /* 300-deep nesting must hit the depth limit, not the stack. */
        uint8_t *deep = (uint8_t *)malloc(602);
        size_t pos = 0;
        int i;

        CHECK(deep != NULL);
        if (deep != NULL) {
            for (i = 0; i < 300; i++) {
                deep[pos++] = 0x22;
                deep[pos++] = 0x02;
            }
            deep[pos++] = 0x00;
            deep[pos++] = 0x00;
            try_decode(d, deep, pos);
            free(deep);
        }
    }
    {
        pbw_message *m = NULL;

        CHECK_STATUS(PBW_ERR_INVALID, pbw_decode(d, NULL, 5, &m));
        CHECK(m == NULL);
    }
}

static void
test_api_robustness(const pbw_descriptor *d)
{
    uint8_t *o = NULL;
    size_t ol = 0;
    size_t wl = 0;
    pbw_istream *is;

    CHECK_STATUS(PBW_ERR_INVALID, pbw_encode(NULL, &o, &ol));
    CHECK_STATUS(PBW_ERR_INVALID, pbw_message_encoded_size(NULL, &ol));
    CHECK_STATUS(PBW_ERR_INVALID, pbw_encode_to(NULL, NULL, 0, &wl));
    {
        pbw_value *v = pbw_value_new_bytes(NULL, 5);
        size_t bl = 0;
        const void *bd;

        CHECK(v != NULL);
        bd = pbw_value_get_bytes(v, &bl);
        CHECK(bl == 5 && bd != NULL && ((const uint8_t *)bd)[0] == 0);
        pbw_value_free(v);
    }
    is = pbw_istream_new(NULL);
    CHECK(is != NULL);
    if (is != NULL) {
        CHECK_STATUS(PBW_ERR_NOMEM, pbw_istream_feed(is, "x", SIZE_MAX));
        pbw_istream_free(is);
    }
    {
        static const uint8_t bad[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        pbw_schema *s = NULL;

        CHECK(pbw_descriptor_set_load(bad, sizeof(bad), &s) != PBW_OK);
        if (s != NULL) {
            pbw_schema_free(s);
        }
        s = (pbw_schema *)0x1;
        CHECK_STATUS(PBW_ERR_INVALID,
                     pbw_descriptor_set_load(NULL, 0, &s));
        CHECK(s == NULL);
    }
    {
        char deep[1000];
        pbw_message *m = NULL;

        memset(deep, '[', 500);
        memset(deep + 500, ']', 500);
        CHECK(pbw_json_parse(d, deep, sizeof(deep), &m) != PBW_OK);
        CHECK(m == NULL);
        CHECK(pbw_text_parse(d, "a { a { a { ", 12, &m) != PBW_OK);
        CHECK(m == NULL);
    }
}

static void
test_istream_split(const pbw_descriptor *d)
{
    pbw_message *tmp = pbw_message_new(d);
    pbw_value *v = pbw_value_new_uint(42);
    uint8_t *body = NULL;
    size_t bl = 0;
    pbw_istream *is;
    size_t i;

    CHECK(tmp != NULL && v != NULL);
    if (tmp == NULL || v == NULL) {
        pbw_value_free(v);
        pbw_message_free(tmp);
        return;
    }
    CHECK(pbw_message_set(tmp, 1, v) == PBW_OK);
    CHECK(pbw_encode_delimited(tmp, &body, &bl) == PBW_OK);
    is = pbw_istream_new(NULL);
    CHECK(is != NULL);
    if (is != NULL && body != NULL) {
        for (i = 0; i < bl; i++) {
            CHECK(pbw_istream_feed(is, body + i, 1) == PBW_OK);
            {
                pbw_message *got = NULL;

                CHECK(pbw_istream_next(is, d, &got) == PBW_OK);
                if (got != NULL) {
                    pbw_message_free(got);
                }
            }
        }
        pbw_istream_free(is);
    }
    pbw_free(body);
    pbw_message_free(tmp);
}

static void
test_random(const pbw_descriptor *d)
{
    uint8_t buf[256];
    pbw_ctx *ctx = pbw_ctx_new();
    long i;

    CHECK(ctx != NULL);
    for (i = 0; i < 5000; i++) {
        size_t len = (size_t)(rnd() % 64);
        size_t k;

        if (i % 17 == 0) {
            len = (size_t)(rnd() % sizeof(buf));
        }
        for (k = 0; k < len; k++) {
            buf[k] = (uint8_t)(rnd() & 0xFF);
        }
        try_decode(d, buf, len);
        pbw_ctx_reset(ctx);
        {
            pbw_message *m = NULL;

            CHECK(pbw_decode_ctx(ctx, d, buf, len, &m) == PBW_OK ||
                  m == NULL);
            /* ctx owns m; reset frees it. */
        }
        if (i % 10 == 0) {
            pbw_schema *s = NULL;
            pbw_status st = pbw_descriptor_set_load(buf, len, &s);

            /* Failure still hands back the partial schema for the error. */
            CHECK(s != NULL);
            (void)st;
            if (s != NULL) {
                pbw_schema_free(s);
            }
        }
        {
            pbw_message *m = NULL;

            CHECK(pbw_json_parse(d, (const char *)buf, len, &m) == PBW_OK ||
                  m == NULL);
            if (m != NULL) {
                pbw_message_free(m);
            }
        }
        {
            pbw_message *m = NULL;

            CHECK(pbw_text_parse(d, (const char *)buf, len, &m) == PBW_OK ||
                  m == NULL);
            if (m != NULL) {
                pbw_message_free(m);
            }
        }
    }
    pbw_ctx_free(ctx);
}

/* Every strict prefix of a valid document must parse safely: either it is
 * itself valid (free it) or it fails with no message out. This walks every
 * incomplete-input branch of both text grammars without asserting verdicts. */
static void
test_prefix_sweep(const pbw_descriptor *d)
{
    static const char *docs[] = {
        ("{\"i32\":-5,\"s\":\"hi\\u00e9\",\"rep\":[1,2],"
         "\"sub\":{\"x\":1},\"subs\":[{\"x\":2}],\"big\":\"b\"}"),
        "{\"s\":\"a\\\\b\\\"c\\n\",\"rep\":[],\"sub\":{}}",
        "i32: -5 s: \"hi\" rep: 1 rep: 2 sub { x: 1 } big: \"b\"",
        "grp { y: 1 } subs { x: 2 } rep: [1, 2] s: \"a\\303\\251\"",
    };
    size_t i;

    for (i = 0; i < sizeof(docs) / sizeof(docs[0]); i++) {
        size_t full = strlen(docs[i]);
        size_t k;

        for (k = 0; k < full; k++) {
            pbw_message *m = NULL;
            pbw_status st;

            if (docs[i][0] == '{') {
                st = pbw_json_parse(d, docs[i], k, &m);
            } else {
                st = pbw_text_parse(d, docs[i], k, &m);
            }
            CHECK(st == PBW_OK || m == NULL);
            pbw_message_free(m);
        }
    }
}

/* Huge tags take the reserving writer paths; every scalar kind encodes
 * and decodes back at its edges. */
static void
test_huge_tags(const pbw_descriptor *d)
{
    pbw_message *m = pbw_message_new(d);
    pbw_message *dec = NULL;
    uint8_t *wire = NULL;
    size_t wire_len = 0;
    int i;

    CHECK(m != NULL);
    if (m == NULL) {
        return;
    }
    CHECK_STATUS(PBW_OK, pbw_message_set(m, 1, pbw_value_new_int(-1)));
    CHECK_STATUS(PBW_OK, pbw_message_set(m, 7, pbw_value_new_uint(0xFFFFFFFFu)));
    CHECK_STATUS(PBW_OK, pbw_message_set(m, 8,
                                         pbw_value_new_uint(0xFFFFFFFFFFFFFFFFull)));
    CHECK_STATUS(PBW_OK, pbw_message_set(m, 9, pbw_value_new_float(3.5f)));
    CHECK_STATUS(PBW_OK, pbw_message_set(m, 10, pbw_value_new_double(-2.25)));
    CHECK_STATUS(PBW_OK, pbw_message_set(m, 11, pbw_value_new_int(INT32_MIN)));
    CHECK_STATUS(PBW_OK, pbw_message_set(m, 12, pbw_value_new_int(INT64_MIN)));
    CHECK_STATUS(PBW_OK, pbw_message_set(m, 33554432u,
                                         pbw_value_new_uint(0xDEADBEEFu)));
    CHECK_STATUS(PBW_OK, pbw_message_set(m, 33554433u,
                                         pbw_value_new_uint(0xFFFFFFFFFFFFFFFFull)));
    CHECK_STATUS(PBW_OK, pbw_message_set(m, 33554440u,
                                         pbw_value_new_uint(0x0102030405060708ull)));
    {
        static const uint8_t blob[64] = {1};

        CHECK_STATUS(PBW_OK,
                     pbw_message_set(m, 33554441u,
                                     pbw_value_new_bytes(blob,
                                                         sizeof(blob))));
    }
    {
        pbw_message *subm = pbw_message_set_message(m, 33554442u);

        CHECK(subm != NULL);
        CHECK_STATUS(PBW_OK,
                     pbw_message_set(subm, 1, pbw_value_new_int(9)));
    }
    CHECK_STATUS(PBW_OK, pbw_message_set(m, 536870911u,
                                         pbw_value_new_uint(0xFFFFFFFFFFFFFFFFull)));
    for (i = 0; i < 10; i++) {
        CHECK_STATUS(PBW_OK, pbw_message_set(m, 33554434u, pbw_value_new_int(i)));
        CHECK_STATUS(PBW_OK, pbw_message_set(m, 33554435u, pbw_value_new_bool(i & 1)));
        CHECK_STATUS(PBW_OK, pbw_message_set(m, 33554436u, pbw_value_new_uint((uint32_t)i)));
        CHECK_STATUS(PBW_OK, pbw_message_set(m, 33554437u, pbw_value_new_double((double)i)));
    }
    CHECK_STATUS(PBW_OK, pbw_encode(m, &wire, &wire_len));
    if (wire != NULL) {
        CHECK_STATUS(PBW_OK, pbw_decode(d, wire, wire_len, &dec));
        if (dec != NULL) {
            CHECK(pbw_value_get_uint(pbw_message_get(dec, 7)) ==
                  0xFFFFFFFFu);
            CHECK(pbw_value_get_uint(pbw_message_get(dec, 33554432u)) ==
                  0xDEADBEEFu);
            pbw_message_free(dec);
            dec = NULL;
        }
        /* packed huge-tag field decodes through the packed path too */
        pbw_free(wire);
        wire = NULL;
    }
    /* medium values on huge tags take the short varint branches */
    CHECK_STATUS(PBW_OK,
                 pbw_message_set(m, 33554433u, pbw_value_new_uint(0x2000u)));
    CHECK_STATUS(PBW_OK,
                 pbw_message_set(m, 536870911u, pbw_value_new_uint(0x7Fu)));
    CHECK_STATUS(PBW_OK, pbw_encode(m, &wire, &wire_len));
    pbw_free(wire);
    pbw_message_free(m);
}

/* 300-deep nesting hits the depth limit; a corrupt inner message fails
 * the whole decode (status propagation, no partial tree). */
static void
test_deep_binary(const pbw_descriptor *d)
{
    uint8_t *inner = (uint8_t *)malloc(2);
    size_t inner_len = 2;
    int i;

    CHECK(inner != NULL);
    if (inner == NULL) {
        return;
    }
    inner[0] = 0x08;
    inner[1] = 0x01;
    /* Sub{sub{...x:1...}} inside-out, 300 levels, then one Fuzz wrapper
     * (field 4, LEN) around the outermost Sub. */
    for (i = 0; i < 300; i++) {
        size_t lb = inner_len < 128 ? 1 : 2;
        uint8_t *outer = (uint8_t *)malloc(1 + lb + inner_len);

        CHECK(outer != NULL);
        if (outer == NULL) {
            free(inner);
            return;
        }
        outer[0] = 0x12;
        if (lb == 1) {
            outer[1] = (uint8_t)inner_len;
        } else {
            outer[1] = (uint8_t)((inner_len & 0x7F) | 0x80);
            outer[2] = (uint8_t)(inner_len >> 7);
        }
        memcpy(outer + 1 + lb, inner, inner_len);
        free(inner);
        inner = outer;
        inner_len = 1 + lb + inner_len;
    }
    {
        size_t lb = inner_len < 128 ? 1 : 2;
        uint8_t *outer = (uint8_t *)malloc(1 + lb + inner_len);
        pbw_message *m = NULL;

        CHECK(outer != NULL);
        if (outer != NULL) {
            outer[0] = 0x22; /* field 4, LEN */
            if (lb == 1) {
                outer[1] = (uint8_t)inner_len;
            } else {
                outer[1] = (uint8_t)((inner_len & 0x7F) | 0x80);
                outer[2] = (uint8_t)(inner_len >> 7);
            }
            memcpy(outer + 1 + lb, inner, inner_len);
            CHECK_STATUS(PBW_ERR_LIMIT,
                         pbw_decode(d, outer, 1 + lb + inner_len, &m));
            CHECK(m == NULL);
            /* truncating the tail fails instead of yielding a partial tree */
            CHECK(pbw_decode(d, outer, (1 + lb + inner_len) / 2, &m) !=
                      PBW_OK ||
                  m != NULL);
            pbw_message_free(m);
            free(outer);
        }
        free(inner);
    }
}

static pbw_status
count_cb(void *ud, const pbw_field *field, uint32_t number,
         uint32_t wire_type, const pbw_visit_value *value)
{
    size_t *n = (size_t *)ud;

    (void)field;
    (void)number;
    (void)wire_type;
    (void)value;
    (*n)++;
    return PBW_OK;
}

/* The validating scan over hostile bytes: no crash, fail_off always
 * written, bounded variants agree with the unbounded one. */
static void
test_visit_vectors(const pbw_descriptor *d)
{
    static const uint8_t field0[] = {0x00};
    static const uint8_t wire6[] = {0x0E, 0x01};
    static const uint8_t wire7[] = {0x0F, 0x01};
    static const uint8_t tag5[] = {0xFF, 0xFF, 0xFF, 0xFF, 0x7F, 0x01};
    static const uint8_t open_group[] = {0x2B, 0x08, 0x01};
    static const uint8_t bad_end[] = {0x2B, 0x08, 0x01, 0x2C};
    static const uint8_t valid[] = {0x08, 0x01, 0x12, 0x02, 0x68, 0x69};
    /* unknown field numbers scan fine: the callback sees a NULL field */
    static const uint8_t unknown_ok[] = {0x98, 0x06, 0x01};
    const struct {
        const uint8_t *buf;
        size_t len;
        int ok;
    } cases[] = {
        /* wire types 6/7 are reserved and always invalid; a closed
         * unknown group scans fine */
        {field0, sizeof(field0), 0},
        {wire6, sizeof(wire6), 0},
        {wire7, sizeof(wire7), 0},
        {tag5, sizeof(tag5), 0},
        {open_group, sizeof(open_group), 0},
        {bad_end, sizeof(bad_end), 1},
        {valid, sizeof(valid), 1},
        {unknown_ok, sizeof(unknown_ok), 1},
        {NULL, 0, 1},
    };
    size_t i;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        size_t n = 0;
        size_t off = 0;
        size_t n_null = 0;
        size_t n_lim = 0;
        size_t off_lim = 0;
        pbw_status st;
        pbw_limits lim = {0, 0};
        pbw_limits tiny = {1, 0};

        st = pbw_visit(d, cases[i].buf, cases[i].len, count_cb, &n, &off);
        if (cases[i].ok) {
            CHECK_STATUS(PBW_OK, st);
            CHECK(off == cases[i].len);
        } else {
            CHECK(st != PBW_OK);
        }
        /* NULL fail_off is allowed; bounded defaults agree */
        CHECK(pbw_visit(d, cases[i].buf, cases[i].len, count_cb, &n_null,
                        NULL) == st);
        CHECK(pbw_visit_limits(d, cases[i].buf, cases[i].len, &lim,
                               count_cb, &n_lim, &off_lim) == st);
        if (st == PBW_OK) {
            CHECK(n_null == n && n_lim == n && off_lim == off);
        }
        /* a byte cap below the input fails cleanly */
        if (cases[i].len > 1) {
            size_t n_tiny = 0;
            size_t off_tiny = 0;

            CHECK(pbw_visit_limits(d, cases[i].buf, cases[i].len, &tiny,
                                   count_cb, &n_tiny, &off_tiny) ==
                  PBW_ERR_LIMIT);
        }
    }
    /* two top-level fields visited; the depth cap only bites groups,
     * which the single-level scan skips without recursing */
    {
        size_t n = 0;
        size_t off = 0;
        pbw_limits shallow = {0, 1};
        pbw_limits roomy = {0, 100};
        /* ten nested unknown groups, closed: field 99 START_GROUP x10
         * then END_GROUP x10 (0x9C 0x06). Unclosed groups always fail. */
        uint8_t groups[40];
        size_t g;

        CHECK_STATUS(PBW_OK,
                     pbw_visit(d, valid, sizeof(valid), count_cb, &n, &off));
        CHECK(n == 2 && off == sizeof(valid));
        for (g = 0; g < 10; g++) {
            groups[2 * g] = 0x9B;
            groups[2 * g + 1] = 0x06;
            groups[20 + 2 * g] = 0x9C;
            groups[20 + 2 * g + 1] = 0x06;
        }
        CHECK_STATUS(PBW_ERR_LIMIT,
                     pbw_visit_limits(d, groups, sizeof(groups), &shallow,
                                      count_cb, &n, &off));
        CHECK_STATUS(PBW_OK,
                     pbw_visit_limits(d, groups, sizeof(groups), &roomy,
                                      count_cb, &n, &off));
    }
}

int
main(void)
{
    pbw_schema *s = build_schema();

    CHECK(s != NULL);
    if (s != NULL) {
        const pbw_descriptor *d = pbw_schema_find(s, "Fuzz");

        CHECK(d != NULL);
        if (d != NULL) {
            test_vectors(d);
            test_api_robustness(d);
            test_istream_split(d);
            test_prefix_sweep(d);
            test_huge_tags(d);
            test_deep_binary(d);
            test_visit_vectors(d);
            test_random(d);
        }
        pbw_schema_free(s);
    }
    if (failures == 0) {
        printf("test_malformed: all checks passed\n");
        return 0;
    }
    printf("test_malformed: %d check(s) failed\n", failures);
    return 1;
}

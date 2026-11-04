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
    CHECK(pbw_message_add_field(m, "big", 16, PBW_TYPE_STRING,
                                PBW_LABEL_OPTIONAL) != NULL);
    CHECK(pbw_message_add_field(m, "huge", 2048, PBW_TYPE_INT32,
                                PBW_LABEL_OPTIONAL) != NULL);
    CHECK(pbw_message_add_field(m, "maxf", 536870911u, PBW_TYPE_INT32,
                                PBW_LABEL_OPTIONAL) != NULL);
    CHECK(pbw_message_add_field(sub, "x", 1, PBW_TYPE_INT32,
                                PBW_LABEL_OPTIONAL) != NULL);
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

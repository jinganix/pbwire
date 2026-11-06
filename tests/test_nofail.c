/* Allocation-failure injection: the Makefile builds the core a second time
 * with -Dmalloc=pbw_fail_malloc (and calloc/realloc/free), so every core
 * allocation routes through the hooks below. The workload must never crash;
 * every fallible call has to return a status, and everything still owned
 * must be freed. This is the executable form of the "no uncaught errors"
 * invariant: error paths that only trigger on NOMEM get no other coverage.
 *
 * Iteration i fails the i-th core allocation (fail_at counts down from i);
 * the final iteration runs clean. The count run first measures N so the
 * sweep is exact and deterministic: same workload, same order, every time.
 * Run under ASan/UBSan (like the sanitizers CI job does for `make test`)
 * to also catch leaks and UB on the failure paths. */

#include "pbwire.h"

#include "desc_set.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <locale.h>

static long fail_at = -1; /* countdown; <0 never fails */
static long alloc_count = 0;
static int saw_injected_fail = 0;
static int failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
                    #cond);                                                  \
            failures++;                                                      \
        }                                                                    \
    } while (0)

void *
pbw_fail_malloc(size_t n)
{
    alloc_count++;
    if (fail_at == 0) {
        saw_injected_fail = 1;
        return NULL;
    }
    if (fail_at > 0) {
        fail_at--;
    }
    return malloc(n);
}

void *
pbw_fail_calloc(size_t count, size_t size)
{
    alloc_count++;
    if (fail_at == 0) {
        saw_injected_fail = 1;
        return NULL;
    }
    if (fail_at > 0) {
        fail_at--;
    }
    return calloc(count, size);
}

void *
pbw_fail_realloc(void *ptr, size_t n)
{
    alloc_count++;
    if (fail_at == 0) {
        saw_injected_fail = 1;
        return NULL;
    }
    if (fail_at > 0) {
        fail_at--;
    }
    return realloc(ptr, n);
}

void
pbw_fail_free(void *ptr)
{
    free(ptr);
}

/* Growable sink buffer for the encode_sink path. */
struct sink_buf {
    uint8_t *data;
    size_t len;
    size_t cap;
};

static pbw_status
collect_sink(const uint8_t *data, size_t len, void *opaque)
{
    struct sink_buf *s = (struct sink_buf *)opaque;
    uint8_t *grown;

    if (s->len + len < s->len) {
        return PBW_ERR_LIMIT;
    }
    if (s->len + len > s->cap) {
        grown = (uint8_t *)realloc(s->data, s->len + len);
        if (grown == NULL) {
            return PBW_ERR_NOMEM;
        }
        s->data = grown;
        s->cap = s->len + len;
    }
    memcpy(s->data + s->len, data, len);
    s->len += len;
    return PBW_OK;
}

/* Minimal FileDescriptorSet with one service (S.M): exercises the
 * service/method loader, including its NOMEM branches, on every
 * injection iteration. */
static const uint8_t svc_set[] = {
    0x0A,
    0x38,
    0x0A,
    0x07,
    0x73,
    0x2E,
    0x70,
    0x72,
    0x6F,
    0x74,
    0x6F,
    0x12,
    0x01,
    0x73,
    0x22,
    0x03,
    0x0A,
    0x01,
    0x4D,
    0x32,
    0x25,
    0x0A,
    0x01,
    0x53,
    0x12,
    0x0F,
    0x0A,
    0x01,
    0x4D,
    0x12,
    0x04,
    0x2E,
    0x73,
    0x2E,
    0x4D,
    0x1A,
    0x04,
    0x2E,
    0x73,
    0x2E,
    0x4D,
    0x12,
    0x0F,
    0x0A,
    0x01,
    0x4E,
    0x12,
    0x04,
    0x2E,
    0x73,
    0x2E,
    0x4D,
    0x1A,
    0x04,
    0x2E,
    0x73,
    0x2E,
    0x4D,
};
/* Fixed hostile bytes: decode/parse/descriptor-load error paths, including
 * their NOMEM branches, on top of the valid workload below. */
static const uint8_t hostile[] = {
    0xFF,
    0xFF,
    0xFF,
    0xFF,
    0x07,
    0x12,
    0x04,
    0xDE,
    0xAD,
    0xBE,
    0xEF,
    0x22,
    0x82,
    0x01,
    0x08,
    0x96,
    0x01,
    0x7B,
    0x22,
    0x3A,
    0x7B,
    0x5B,
};

static void
workload(void)
{
    pbw_schema *s = pbw_schema_new();
    pbw_descriptor *m = NULL;
    pbw_descriptor *sub = NULL;
    pbw_descriptor *entry = NULL;
    pbw_descriptor *en = NULL;
    pbw_field *f = NULL;
    pbw_message *msg = NULL;
    pbw_message *dec = NULL;
    pbw_message *subm = NULL;
    uint8_t *wire = NULL;
    size_t wire_len = 0;
    char *text = NULL;
    size_t text_len = 0;
    int i;

    if (s == NULL) {
        return;
    }
    m = pbw_schema_add_message(s, "M");
    sub = pbw_schema_add_message(s, "Sub");
    entry = pbw_schema_add_message(s, "M.E");
    en = pbw_schema_add_enum(s, "C");
    if (m == NULL || sub == NULL || entry == NULL || en == NULL) {
        pbw_schema_free(s);
        return;
    }
    pbw_enum_add_value(en, "C_ZERO", 0);
    pbw_enum_add_value(en, "C_ONE", 1);
    if (pbw_message_add_field(sub, "a", 1, PBW_TYPE_INT32,
                              PBW_LABEL_OPTIONAL) == NULL ||
        pbw_message_add_field(entry, "key", 1, PBW_TYPE_STRING,
                              PBW_LABEL_OPTIONAL) == NULL ||
        pbw_message_add_field(entry, "value", 2, PBW_TYPE_INT32,
                              PBW_LABEL_OPTIONAL) == NULL) {
        pbw_schema_free(s);
        return;
    }
    pbw_descriptor_set_map_entry(entry);
    if (pbw_message_add_field(m, "i", 1, PBW_TYPE_INT32,
                              PBW_LABEL_OPTIONAL) == NULL ||
        pbw_message_add_field(m, "s", 2, PBW_TYPE_STRING,
                              PBW_LABEL_OPTIONAL) == NULL ||
        pbw_message_add_field(m, "b", 3, PBW_TYPE_BYTES,
                              PBW_LABEL_OPTIONAL) == NULL) {
        pbw_schema_free(s);
        return;
    }
    f = pbw_message_add_field(m, "sub", 4, PBW_TYPE_MESSAGE,
                              PBW_LABEL_OPTIONAL);
    if (f != NULL) {
        pbw_field_set_type_name(f, "Sub");
    }
    f = pbw_message_add_field(m, "rep", 5, PBW_TYPE_INT32,
                              PBW_LABEL_REPEATED);
    if (f == NULL) {
        pbw_schema_free(s);
        return;
    }
    pbw_field_set_packed(f, 1);
    f = pbw_message_add_field(m, "mm", 6, PBW_TYPE_MESSAGE,
                              PBW_LABEL_REPEATED);
    if (f != NULL) {
        pbw_field_set_type_name(f, "M.E");
    }
    f = pbw_message_add_field(m, "c", 7, PBW_TYPE_ENUM, PBW_LABEL_OPTIONAL);
    if (f != NULL) {
        pbw_field_set_type_name(f, "C");
    }
    /* packed runs, floats and submessages exercise the remaining
     * encoder, JSON/text and arena paths under injection */
    /* Set packed/flags immediately: each add may move the field array,
     * invalidating pointers from earlier adds. */
    {
        pbw_descriptor *val;
        pbw_descriptor *st;
        pbw_descriptor *fe;
        pbw_descriptor *lv;
        pbw_descriptor *any;

        val = pbw_schema_add_message(s, "google.protobuf.Value");
        st = pbw_schema_add_message(s, "google.protobuf.Struct");
        fe = pbw_schema_add_message(s,
                                    "google.protobuf.Struct.FieldsEntry");
        lv = pbw_schema_add_message(s, "google.protobuf.ListValue");
        any = pbw_schema_add_message(s, "google.protobuf.Any");
        if (val == NULL || st == NULL || fe == NULL || lv == NULL ||
            any == NULL) {
            pbw_schema_free(s);
            return;
        }
        if (pbw_message_add_field(val, "n", 2, PBW_TYPE_DOUBLE,
                                  PBW_LABEL_OPTIONAL) == NULL ||
            pbw_message_add_field(val, "s", 3, PBW_TYPE_STRING,
                                  PBW_LABEL_OPTIONAL) == NULL ||
            pbw_message_add_field(fe, "key", 1, PBW_TYPE_STRING,
                                  PBW_LABEL_OPTIONAL) == NULL) {
            pbw_schema_free(s);
            return;
        }
        f = pbw_message_add_field(fe, "value", 2, PBW_TYPE_MESSAGE,
                                  PBW_LABEL_OPTIONAL);
        if (f != NULL) {
            pbw_field_set_type_name(f, "google.protobuf.Value");
        }
        f = pbw_message_add_field(st, "fields", 1, PBW_TYPE_MESSAGE,
                                  PBW_LABEL_REPEATED);
        if (f != NULL) {
            pbw_field_set_type_name(f,
                                    "google.protobuf.Struct.FieldsEntry");
        }
        f = pbw_message_add_field(lv, "values", 1, PBW_TYPE_MESSAGE,
                                  PBW_LABEL_REPEATED);
        if (f != NULL) {
            pbw_field_set_type_name(f, "google.protobuf.Value");
        }
        if (pbw_message_add_field(any, "type_url", 1, PBW_TYPE_STRING,
                                  PBW_LABEL_OPTIONAL) == NULL ||
            pbw_message_add_field(any, "value", 2, PBW_TYPE_BYTES,
                                  PBW_LABEL_OPTIONAL) == NULL) {
            pbw_schema_free(s);
            return;
        }
        pbw_descriptor_set_map_entry(fe);
        f = pbw_message_add_field(m, "st", 18, PBW_TYPE_MESSAGE,
                                  PBW_LABEL_OPTIONAL);
        if (f != NULL) {
            pbw_field_set_type_name(f, "google.protobuf.Struct");
        }
        f = pbw_message_add_field(m, "av", 19, PBW_TYPE_MESSAGE,
                                  PBW_LABEL_OPTIONAL);
        if (f != NULL) {
            pbw_field_set_type_name(f, "google.protobuf.Any");
        }
    }
    {
        pbw_field *r;

        r = pbw_message_add_field(m, "rep2", 8, PBW_TYPE_INT32,
                                  PBW_LABEL_REPEATED);
        if (r == NULL) {
            pbw_schema_free(s);
            return;
        }
        pbw_field_set_packed(r, 1);
        if (pbw_message_add_field(m, "flt", 9, PBW_TYPE_FLOAT,
                                  PBW_LABEL_OPTIONAL) == NULL ||
            pbw_message_add_field(m, "f64", 15, PBW_TYPE_FIXED64,
                                  PBW_LABEL_OPTIONAL) == NULL) {
            pbw_schema_free(s);
            return;
        }
        r = pbw_message_add_field(m, "rep3", 16, PBW_TYPE_FIXED32,
                                  PBW_LABEL_REPEATED);
        if (r == NULL) {
            pbw_schema_free(s);
            return;
        }
        pbw_field_set_packed(r, 1);
        r = pbw_message_add_field(m, "rep4", 17, PBW_TYPE_BOOL,
                                  PBW_LABEL_REPEATED);
        if (r == NULL) {
            pbw_schema_free(s);
            return;
        }
        pbw_field_set_packed(r, 1);
    }
    {
        pbw_descriptor *g = pbw_schema_add_message(s, "G");
        pbw_field *gf;

        if (g == NULL ||
            pbw_message_add_field(g, "x", 1, PBW_TYPE_INT32,
                                  PBW_LABEL_OPTIONAL) == NULL) {
            pbw_schema_free(s);
            return;
        }
        gf = pbw_message_add_field(m, "grp", 10, PBW_TYPE_GROUP,
                                   PBW_LABEL_OPTIONAL);
        if (gf == NULL) {
            pbw_schema_free(s);
            return;
        }
        pbw_field_set_type_name(gf, "G");
        gf = pbw_message_add_field(m, "o1", 11, PBW_TYPE_INT32,
                                   PBW_LABEL_OPTIONAL);
        if (gf == NULL) {
            pbw_schema_free(s);
            return;
        }
        pbw_field_set_oneof(gf, 1);
        gf = pbw_message_add_field(m, "o2", 12, PBW_TYPE_STRING,
                                   PBW_LABEL_OPTIONAL);
        if (gf == NULL) {
            pbw_schema_free(s);
            return;
        }
        pbw_field_set_oneof(gf, 1);
        gf = pbw_message_add_field(m, "subs", 13, PBW_TYPE_MESSAGE,
                                   PBW_LABEL_REPEATED);
        if (gf == NULL) {
            pbw_schema_free(s);
            return;
        }
        pbw_field_set_type_name(gf, "Sub");
    }
    if (pbw_schema_link(s) != PBW_OK) {
        pbw_schema_free(s);
        return;
    }
    msg = pbw_message_new(m);
    if (msg == NULL) {
        pbw_schema_free(s);
        return;
    }
    /* Failures below just leave fields unset; every call is NULL-safe. */
    (void)pbw_message_set(msg, 1, pbw_value_new_int(-1));
    (void)pbw_message_set(msg, 2, pbw_value_new_string("x"));
    (void)pbw_message_set(msg, 3, pbw_value_new_bytes(hostile, sizeof(hostile)));
    (void)pbw_message_set(msg, 7, pbw_value_new_uint(1));
    subm = pbw_message_set_message(msg, 4);
    if (subm != NULL) {
        (void)pbw_message_set(subm, 1, pbw_value_new_int(2));
    }
    {
        pbw_message *g = pbw_message_set_message(msg, 10);

        if (g != NULL) {
            (void)pbw_message_set(g, 1, pbw_value_new_int(3));
        }
    }
    (void)pbw_message_set(msg, 11, pbw_value_new_int(4));
    {
        /* A Struct entry and an Any exercise the WKT parse/store paths,
         * including their NOMEM branches, on every injection iteration. */
        pbw_message *stm = pbw_message_set_message(msg, 18);
        pbw_message *avm = pbw_message_set_message(msg, 19);
        const pbw_descriptor *fed = pbw_schema_find(s,
                                                    "google.protobuf.Struct.FieldsEntry");

        if (stm != NULL && fed != NULL) {
            pbw_message *kv = pbw_message_new(fed);
            pbw_message *vv = NULL;
            pbw_value *v = NULL;

            if (kv != NULL) {
                vv = pbw_message_new(
                    pbw_schema_find(s, "google.protobuf.Value"));
            }
            if (kv != NULL && vv != NULL) {
                (void)pbw_message_set(kv, 1,
                                      pbw_value_new_string("k"));
                (void)pbw_message_set(vv, 2,
                                      pbw_value_new_double(1.5));
                (void)pbw_message_set(kv, 2,
                                      pbw_value_new_message(vv));
                v = pbw_value_new_message(kv);
                if (v != NULL) {
                    (void)pbw_message_set(stm, 1, v);
                } else {
                    pbw_message_free(kv);
                }
            } else {
                pbw_message_free(kv);
                pbw_message_free(vv);
            }
        }
        if (avm != NULL) {
            (void)pbw_message_set(avm, 1,
                                  pbw_value_new_string("type.googleapis.com/Sub"));
            (void)pbw_message_set(avm, 2,
                                  pbw_value_new_bytes((const uint8_t *)"\x08\x01", 2));
        }
    }
    {
        /* Map entries exercise the map parse/store paths, including their
         * NOMEM branches, on every injection iteration. */
        const pbw_descriptor *entry = pbw_schema_find(s, "M.E");
        int k;

        for (k = 0; k < 3; k++) {
            pbw_message *kv = entry != NULL ? pbw_message_new(entry)
                                            : NULL;
            char ks[16];
            pbw_value *v = NULL;

            if (kv == NULL) {
                break;
            }
            snprintf(ks, sizeof(ks), "k%d", k);
            (void)pbw_message_set(kv, 1, pbw_value_new_string(ks));
            (void)pbw_message_set(kv, 2, pbw_value_new_int(k));
            v = pbw_value_new_message(kv);
            if (v == NULL) {
                pbw_message_free(kv);
                break;
            }
            if (pbw_message_set(msg, 6, v) != PBW_OK) {
                break;
            }
        }
    }
    {
        pbw_message *s1 = pbw_message_set_message(msg, 13);

        if (s1 != NULL) {
            (void)pbw_message_set(s1, 1, pbw_value_new_int(5));
        }
    }
    /* 64 appends force repeated-list growth through reserve. */
    for (i = 0; i < 64; i++) {
        pbw_message *slot = NULL;
        char key[16];

        (void)pbw_message_set(msg, 5, pbw_value_new_int(i));
        (void)pbw_message_set(msg, 8, pbw_value_new_int(i));
        snprintf(key, sizeof(key), "k%d", i);
        slot = pbw_message_set_message(msg, 6);
        if (slot == NULL) {
            break;
        }
        if (pbw_message_set(slot, 1, pbw_value_new_string(key)) != PBW_OK ||
            pbw_message_set(slot, 2, pbw_value_new_int(i)) != PBW_OK) {
            break;
        }
    }
    (void)pbw_message_set(msg, 9, pbw_value_new_float(1.5f));
    (void)pbw_message_set(msg, 15,
                          pbw_value_new_uint(0x0102030405060708ull));
    {
        int j;

        for (j = 0; j < 16; j++) {
            (void)pbw_message_set(msg, 16, pbw_value_new_uint((uint32_t)j));
            (void)pbw_message_set(msg, 17, pbw_value_new_bool(j & 1));
        }
    }
    if (pbw_encode(msg, &wire, &wire_len) == PBW_OK) {
        uint8_t small[4];
        size_t small_len = 0;
        pbw_message *d2 = NULL;
        uint8_t *det = NULL;
        size_t det_len = 0;

        /* deterministic encoding sorts any maps the same way */
        (void)pbw_encode_deterministic(msg, &det, &det_len);
        pbw_free(det);
        /* an empty message encodes to zero bytes */
        {
            pbw_message *empty = pbw_message_new(m);
            uint8_t *ewire = NULL;
            size_t ewlen = 0;

            if (empty != NULL) {
                (void)pbw_encode(empty, &ewire, &ewlen);
                pbw_free(ewire);
                pbw_message_free(empty);
            }
        }

        CHECK(pbw_decode(m, wire, wire_len, &dec) != PBW_OK ||
              dec != NULL);
        pbw_message_free(dec);
        dec = NULL;
        /* Too-small buffer fails cleanly (writes nothing past the end). */
        CHECK(pbw_encode_to(msg, small, sizeof(small), &small_len) ==
                  PBW_ERR_LIMIT ||
              small_len <= sizeof(small));
        /* Truncated and hostile inputs fail with nothing out. */
        if (wire_len > 0) {
            CHECK(pbw_decode(m, wire, wire_len - 1, &d2) != PBW_OK ||
                  d2 != NULL);
            pbw_message_free(d2);
            d2 = NULL;
        }
        CHECK(pbw_decode(m, hostile, sizeof(hostile), &d2) != PBW_OK ||
              d2 != NULL);
        pbw_message_free(d2);
        pbw_free(wire);
        wire = NULL;
    }
    if (pbw_json_print(msg, &text, &text_len) == PBW_OK) {
        pbw_message *d2 = NULL;

        CHECK(pbw_json_parse(m, text, text_len, &d2) != PBW_OK ||
              d2 != NULL);
        pbw_message_free(d2);
        pbw_free(text);
        text = NULL;
    }
    if (pbw_text_print(msg, &text, &text_len) == PBW_OK) {
        pbw_message *d2 = NULL;

        CHECK(pbw_text_parse(m, text, text_len, &d2) != PBW_OK ||
              d2 != NULL);
        pbw_message_free(d2);
        pbw_free(text);
        text = NULL;
    }
    {
        pbw_message *d2 = NULL;
        pbw_json_parse_options opts;

        opts.ignore_unknown_fields = 1;
        (void)pbw_json_parse_opts(m, "{\"zzz\":[1]}", 11, &opts, &d2);
        pbw_message_free(d2);
        (void)pbw_text_parse(m, "zzz: 1", 6, &d2);
        pbw_message_free(d2);
        (void)pbw_text_parse(m, "s: \"a\\nb\\u00e9\" flt: 1e3",
                             strlen("s: \"a\\nb\\u00e9\" flt: 1e3"),
                             &d2);
        pbw_message_free(d2);
        (void)pbw_json_parse(m, (const char *)hostile, sizeof(hostile),
                             &d2);
        pbw_message_free(d2);
    }
    {
        pbw_ctx *ctx = pbw_ctx_new();

        if (ctx != NULL) {
            pbw_message *built = pbw_message_new_ctx(ctx, m);
            pbw_message *dctx = NULL;

            if (built != NULL) {
                (void)pbw_message_set(
                    built, 2, pbw_value_new_bytes_ctx(ctx, hostile, sizeof(hostile)));
                if (pbw_encode(msg, &wire, &wire_len) == PBW_OK) {
                    (void)pbw_decode_ctx(ctx, m, wire, wire_len, &dctx);
                    pbw_free(wire);
                    wire = NULL;
                }
            }
            pbw_ctx_reset(ctx);
            pbw_ctx_free(ctx);
        }
    }
    {
        pbw_pool *pool = pbw_pool_new(2, 0);

        if (pool != NULL) {
            pbw_ctx *a = pbw_pool_acquire(pool);

            if (a != NULL) {
                pbw_pool_release(pool, a);
            }
            pbw_pool_free(pool);
        }
    }
    {
        uint8_t *frame = NULL;
        size_t frame_len = 0;
        size_t consumed = 0;
        pbw_message *d2 = NULL;

        if (pbw_encode_delimited(msg, &frame, &frame_len) == PBW_OK) {
            (void)pbw_decode_delimited(m, frame, frame_len, &d2,
                                       &consumed);
            pbw_message_free(d2);
            /* incremental read of the same frame */
            {
                pbw_istream *is = pbw_istream_new(NULL);
                pbw_message *d3 = NULL;
                size_t half = frame_len / 2;

                if (is != NULL) {
                    (void)pbw_istream_feed(is, frame, half);
                    (void)pbw_istream_next(is, m, &d3);
                    pbw_message_free(d3);
                    d3 = NULL;
                    (void)pbw_istream_feed(is, frame + half,
                                           frame_len - half);
                    (void)pbw_istream_next(is, m, &d3);
                    pbw_message_free(d3);
                    pbw_istream_free(is);
                }
            }
            /* sink path */
            {
                struct sink_buf sunk = {NULL, 0, 0};

                (void)pbw_encode_sink(msg, collect_sink, &sunk);
                free(sunk.data);
            }
            pbw_free(frame);
        }
    }
    {
        /* The pinned descriptor set: loader NOMEM paths plus a proto2
         * message with a missing required field. */
        pbw_schema *loaded = NULL;

        if (pbw_descriptor_set_load(desc_set, sizeof(desc_set), &loaded) ==
            PBW_OK) {
            const pbw_descriptor *old =
                pbw_schema_find(loaded, "pbwire.test.Old");
            pbw_message *d2 = NULL;

            if (old != NULL) {
                (void)pbw_decode(old, NULL, 0, &d2);
                pbw_message_free(d2);
            }
            pbw_schema_free(loaded);
        } else {
            pbw_schema_free(loaded);
        }
        /* Service loader: NOMEM branches fire as the countdown passes
         * through its allocations. */
        {
            pbw_schema *svc = NULL;

            (void)pbw_descriptor_set_load(svc_set, sizeof(svc_set),
                                          &svc);
            pbw_schema_free(svc);
        }
    }
    /* text through a loaded schema: extensions, reserved skipping,
     * Any expansion and their error paths */
    {
        pbw_schema *loaded = NULL;
        const pbw_descriptor *old;
        pbw_message *d2 = NULL;

        if (pbw_descriptor_set_load(desc_set, sizeof(desc_set), &loaded) ==
            PBW_OK) {
            old = pbw_schema_find(loaded, "pbwire.test.Old");
            if (old != NULL) {
                static const char good[] =
                    "id: 1 payload { "
                    "[type.googleapis.com/pbwire.test.Old] { id: 2 } }";
                static const char good_json[] =
                    "{\"id\":1,\"payload\":{\"@type\":"
                    "\"type.googleapis.com/pbwire.test.Old\","
                    "\"id\":2}}";

                (void)pbw_text_parse(old, good, sizeof(good) - 1, &d2);
                pbw_message_free(d2);
                d2 = NULL;
                (void)pbw_json_parse(old, good_json,
                                     sizeof(good_json) - 1, &d2);
                pbw_message_free(d2);
                d2 = NULL;
                (void)pbw_text_parse(old, "id: 1 ancient: 7",
                                     strlen("id: 1 ancient: 7"), &d2);
                pbw_message_free(d2);
                d2 = NULL;
            }
            /* maps, including the trailing-comma legacy form */
            {
                static const char map_json[] =
                    "{\"tags\":{\"k\":1,}}";
                const pbw_descriptor *outer =
                    pbw_schema_find(loaded, "pbwire.test.Outer");

                if (outer != NULL) {
                    (void)pbw_json_parse(outer, map_json,
                                         sizeof(map_json) - 1, &d2);
                    pbw_message_free(d2);
                    d2 = NULL;
                }
            }
            pbw_schema_free(loaded);
        } else {
            pbw_schema_free(loaded);
        }
    }
    pbw_message_free(msg);
    pbw_schema_free(s);
}

int
main(void)
{
    long total;
    long i;
    const char *saved_locale = setlocale(LC_NUMERIC, NULL);
    char saved[64];

    /* Comma-decimal locales take the slow float paths; the sweep then
     * covers their failure branches too. Missing locales just skip. */
    if (saved_locale != NULL && strlen(saved_locale) < sizeof(saved)) {
        memcpy(saved, saved_locale, strlen(saved_locale) + 1);
    } else {
        saved[0] = '\0';
    }
    if (setlocale(LC_NUMERIC, "de_DE.UTF-8") == NULL &&
        setlocale(LC_NUMERIC, "de_DE") == NULL) {
        /* keep the C locale */
    }

    /* Count run: never fail, just measure. */
    fail_at = -1;
    alloc_count = 0;
    workload();
    total = alloc_count;
    printf("nofail: %ld allocations per workload\n", total);
    if (failures != 0) {
        printf("nofail: %d check(s) failed on the clean run\n", failures);
        return 1;
    }
    if (total <= 0 || total > 100000) {
        printf("nofail: implausible allocation count %ld\n", total);
        return 1;
    }
    /* Sweep: fail the i-th allocation, then one clean tail run. */
    for (i = 1; i <= total + 1; i++) {
        fail_at = i;
        alloc_count = 0;
        workload();
        if (failures != 0) {
            printf("nofail: %d check(s) failed with fail_at=%ld\n", failures,
                   i);
            return 1;
        }
    }
    if (!saw_injected_fail) {
        printf("nofail: injection never fired\n");
        return 1;
    }
    printf("nofail: all %ld injection points survived\n", total);
    if (saved[0] != '\0') {
        setlocale(LC_NUMERIC, saved);
    }
    return 0;
}

/* Descriptor set loading self-check: a FileDescriptorSet produced by protoc is
 * turned into a schema, then used to encode and decode. The bytes below are
 * pinned, so the loader is covered without protoc at build time.
 *
 * Regenerate with `make desc-fixture` (protoc 36.2) from
 * tests/data/desc_proto3.proto, which imports tests/data/desc_proto2.proto.
 */

#include "pbwire.h"

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

#define CHECK_OK(expr)                                                              \
    do {                                                                            \
        pbw_status status_ = (expr);                                                \
        if (status_ != PBW_OK) {                                                    \
            fprintf(stderr, "%s:%d: expected PBW_OK, got %s\n", __FILE__, __LINE__, \
                    pbw_strerror(status_));                                         \
            failures++;                                                             \
        }                                                                           \
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

/* The pinned FileDescriptorSet this test loads (shared with test_threads.c). */
#include "desc_set.h"

/* Editions FeatureSet fixture: `protoc --descriptor_set_out` output for
 * tests/data/editions_feat.proto (edition 2023, see the .proto for the
 * regeneration command). Pinned so no protoc is needed at build time. */
static const unsigned char ed_set[] = {
    0x0a, 0xa7, 0x02, 0x0a, 0x13, 0x65, 0x64, 0x69, 0x74, 0x69, 0x6f, 0x6e,
    0x73, 0x5f, 0x66, 0x65, 0x61, 0x74, 0x2e, 0x70, 0x72, 0x6f, 0x74, 0x6f,
    0x12, 0x02, 0x65, 0x64, 0x22, 0x13, 0x0a, 0x03, 0x53, 0x75, 0x62, 0x12,
    0x0c, 0x0a, 0x01, 0x78, 0x18, 0x01, 0x20, 0x01, 0x28, 0x05, 0x52, 0x01,
    0x78, 0x22, 0xbb, 0x01, 0x0a, 0x01, 0x4d, 0x12, 0x21, 0x0a, 0x08, 0x75,
    0x6e, 0x70, 0x61, 0x63, 0x6b, 0x65, 0x64, 0x18, 0x01, 0x20, 0x03, 0x28,
    0x05, 0x42, 0x05, 0xaa, 0x01, 0x02, 0x18, 0x02, 0x52, 0x08, 0x75, 0x6e,
    0x70, 0x61, 0x63, 0x6b, 0x65, 0x64, 0x12, 0x17, 0x0a, 0x03, 0x72, 0x65,
    0x71, 0x18, 0x02, 0x20, 0x01, 0x28, 0x05, 0x42, 0x05, 0xaa, 0x01, 0x02,
    0x08, 0x03, 0x52, 0x03, 0x72, 0x65, 0x71, 0x12, 0x1d, 0x0a, 0x06, 0x63,
    0x6c, 0x6f, 0x73, 0x65, 0x64, 0x18, 0x03, 0x20, 0x01, 0x28, 0x0e, 0x32,
    0x05, 0x2e, 0x65, 0x64, 0x2e, 0x45, 0x52, 0x06, 0x63, 0x6c, 0x6f, 0x73,
    0x65, 0x64, 0x12, 0x24, 0x0a, 0x05, 0x64, 0x65, 0x6c, 0x69, 0x6d, 0x18,
    0x04, 0x20, 0x01, 0x28, 0x0b, 0x32, 0x07, 0x2e, 0x65, 0x64, 0x2e, 0x53,
    0x75, 0x62, 0x42, 0x05, 0xaa, 0x01, 0x02, 0x28, 0x02, 0x52, 0x05, 0x64,
    0x65, 0x6c, 0x69, 0x6d, 0x12, 0x16, 0x0a, 0x06, 0x70, 0x61, 0x63, 0x6b,
    0x65, 0x64, 0x18, 0x05, 0x20, 0x03, 0x28, 0x05, 0x52, 0x06, 0x70, 0x61,
    0x63, 0x6b, 0x65, 0x64, 0x12, 0x1d, 0x0a, 0x06, 0x6f, 0x70, 0x65, 0x6e,
    0x65, 0x64, 0x18, 0x06, 0x20, 0x01, 0x28, 0x0e, 0x32, 0x05, 0x2e, 0x65,
    0x64, 0x2e, 0x46, 0x52, 0x06, 0x6f, 0x70, 0x65, 0x6e, 0x65, 0x64, 0x2a,
    0x17, 0x0a, 0x01, 0x45, 0x12, 0x05, 0x0a, 0x01, 0x41, 0x10, 0x00, 0x12,
    0x05, 0x0a, 0x01, 0x42, 0x10, 0x01, 0x1a, 0x04, 0x3a, 0x02, 0x10, 0x02,
    0x2a, 0x13, 0x0a, 0x01, 0x46, 0x12, 0x06, 0x0a, 0x02, 0x46, 0x41, 0x10,
    0x00, 0x12, 0x06, 0x0a, 0x02, 0x46, 0x42, 0x10, 0x01, 0x62, 0x08, 0x65,
    0x64, 0x69, 0x74, 0x69, 0x6f, 0x6e, 0x73, 0x70, 0xe8, 0x07};

/* Editions FeatureSet: file/message/field resolution plus the codec shapes
 * it selects (packed default, required, closed enum, delimited message). */
static void
test_editions(void)
{
    /* field 1 unpacked x2, field 2 req, field 5 packed x2 (field order) */
    static const unsigned char rep[] = {0x08, 0x01, 0x08, 0x02, 0x10, 0x07,
                                        0x2a, 0x02, 0x01, 0x02};
    /* same, without the required field */
    static const unsigned char rep_noreq[] = {0x08, 0x01, 0x2a, 0x01, 0x01};
    static const unsigned char closed_unknown[] = {0x18, 0x63, 0x10, 0x07};
    static const unsigned char opened_unknown[] = {0x30, 0x63, 0x10, 0x07};
    /* field 2 req, field 4 delimited: START_GROUP(4) { x = 5 } END_GROUP(4) */
    static const unsigned char delim[] = {0x10, 0x07, 0x23, 0x08, 0x05,
                                          0x24};
    pbw_schema *schema = NULL;
    const pbw_descriptor *m;
    const pbw_descriptor *e;
    const pbw_descriptor *f;
    pbw_message *msg = NULL;
    uint8_t *out = NULL;
    size_t out_len = 0;

    CHECK_OK(pbw_descriptor_set_load(ed_set, sizeof(ed_set), &schema));
    m = pbw_schema_find(schema, "ed.M");
    e = pbw_schema_find(schema, "ed.E");
    f = pbw_schema_find(schema, "ed.F");
    CHECK(m != NULL && e != NULL && f != NULL);
    /* editions 2023 reads as proto3, except the CLOSED enum type */
    CHECK(pbw_descriptor_is_proto3(m) == 1);
    CHECK(pbw_descriptor_is_proto3(e) == 0);
    CHECK(pbw_descriptor_is_proto3(f) == 1);
    /* packing: field override wins, the sibling keeps the file default */
    CHECK(pbw_field_is_packed(pbw_descriptor_find_field(m, 1)) == 0);
    CHECK(pbw_field_is_packed(pbw_descriptor_find_field(m, 5)) == 1);
    /* the packed/unpacked shapes encode exactly as declared */
    CHECK_OK(pbw_decode(m, rep, sizeof(rep), &msg));
    CHECK_OK(pbw_message_encoded_size(msg, &out_len));
    CHECK(out_len == sizeof(rep));
    CHECK_OK(pbw_encode(msg, &out, &out_len));
    CHECK(out_len == sizeof(rep) && memcmp(out, rep, out_len) == 0);
    pbw_free(out);
    out = NULL;
    pbw_message_free(msg);
    msg = NULL;
    /* LEGACY_REQUIRED: missing fails, present passes */
    CHECK_STATUS(PBW_ERR_REQUIRED, pbw_decode(m, rep_noreq,
                                              sizeof(rep_noreq), &msg));
    CHECK(msg == NULL);
    pbw_schema_free(schema);
    CHECK_OK(pbw_descriptor_set_load(ed_set, sizeof(ed_set), &schema));
    m = pbw_schema_find(schema, "ed.M");
    /* CLOSED enum: unknown number leaves the field for the unknowns */
    CHECK_OK(pbw_decode(m, closed_unknown, sizeof(closed_unknown), &msg));
    CHECK(pbw_message_get(msg, 3) == NULL);
    CHECK(pbw_message_unknown_len(msg) == 2);
    pbw_message_free(msg);
    msg = NULL;
    /* OPEN enum in the same file: unknown number stays in the field */
    CHECK_OK(pbw_decode(m, opened_unknown, sizeof(opened_unknown), &msg));
    CHECK(pbw_message_get(msg, 6) != NULL);
    CHECK(pbw_value_get_int(pbw_message_get(msg, 6)) == 99);
    pbw_message_free(msg);
    msg = NULL;
    /* Text and JSON apply the same openness rule as the binary decoder:
     * unknown numbers fail for the CLOSED enum, stay for the OPEN one. `req`
     * is LEGACY_REQUIRED, so every valid text/JSON input spells it. */
    {
        pbw_message *tm = NULL;

        CHECK_STATUS(PBW_ERR_INVALID,
                     pbw_text_parse(m, "req: 7 closed: 99", 17, &tm));
        CHECK(tm == NULL);
        /* JSON mirrors the binary decoder instead: the unknown number moves
         * into the unknown fields and the field stays absent. */
        CHECK_OK(pbw_json_parse(m, "{\"req\":7,\"closed\":99}", 21, &tm));
        CHECK(tm != NULL && pbw_message_get(tm, 3) == NULL);
        CHECK(pbw_message_unknown_len(tm) == 2);
        pbw_message_free(tm);
        tm = NULL;
        CHECK_OK(pbw_text_parse(m, "req: 7 opened: 99", 17, &tm));
        CHECK(tm != NULL && pbw_value_get_int(pbw_message_get(tm, 6)) == 99);
        pbw_message_free(tm);
        tm = NULL;
        CHECK_OK(pbw_json_parse(m, "{\"req\":7,\"opened\":99}", 21, &tm));
        CHECK(tm != NULL && pbw_value_get_int(pbw_message_get(tm, 6)) == 99);
        pbw_message_free(tm);
    }
    /* DELIMITED message: group tags decode into the submessage ... */
    CHECK_OK(pbw_decode(m, delim, sizeof(delim), &msg));
    CHECK(pbw_message_get(msg, 4) != NULL);
    CHECK(pbw_value_get_int(pbw_message_get(
              pbw_value_get_message(pbw_message_get(msg, 4)), 1)) == 5);
    /* ... and encode back to the same group tags */
    CHECK_OK(pbw_encode(msg, &out, &out_len));
    CHECK(out_len == sizeof(delim) && memcmp(out, delim, out_len) == 0);
    pbw_free(out);
    pbw_message_free(msg);
    pbw_schema_free(schema);
}

/* A hand-built FileDescriptorSet with a MessageSet (ms.Set, extension
 * ms.item = 4). protoc output for tests/data/messageset.proto, pinned so
 * no protoc is needed at build time. */
static const unsigned char ms_set[] = {
    0x0a, 0x68, 0x0a, 0x10, 0x6d, 0x65, 0x73, 0x73, 0x61, 0x67, 0x65, 0x73,
    0x65, 0x74, 0x2e, 0x70, 0x72, 0x6f, 0x74, 0x6f, 0x12, 0x02, 0x6d, 0x73,
    0x22, 0x14, 0x0a, 0x04, 0x49, 0x74, 0x65, 0x6d, 0x12, 0x0c, 0x0a, 0x01,
    0x78, 0x18, 0x01, 0x20, 0x01, 0x28, 0x05, 0x52, 0x01, 0x78, 0x22, 0x13,
    0x0a, 0x03, 0x53, 0x65, 0x74, 0x2a, 0x08, 0x08, 0x04, 0x10, 0xff, 0xff,
    0xff, 0xff, 0x07, 0x3a, 0x02, 0x08, 0x01, 0x3a, 0x25, 0x0a, 0x04, 0x69,
    0x74, 0x65, 0x6d, 0x12, 0x07, 0x2e, 0x6d, 0x73, 0x2e, 0x53, 0x65, 0x74,
    0x18, 0x04, 0x20, 0x01, 0x28, 0x0b, 0x32, 0x08, 0x2e, 0x6d, 0x73, 0x2e,
    0x49, 0x74, 0x65, 0x6d, 0x52, 0x04, 0x69, 0x74, 0x65, 0x6d};

/* MessageSet: group items decode into their extension fields and encode
 * back to identical group bytes (verified against the official runtime:
 * Set{item{x:5}} is 0b 10 04 1a 02 08 05 0c). Unknown type ids stay
 * unknown, like any other undeclared field. */
static void
test_messageset(void)
{
    /* Set{item{x:5}} in MessageSet wire form */
    static const unsigned char item[] = {0x0b, 0x10, 0x04, 0x1a, 0x02,
                                         0x08, 0x05, 0x0c};
    /* same shape, undeclared type id 9 */
    static const unsigned char foreign[] = {0x0b, 0x10, 0x09, 0x1a, 0x02,
                                            0x08, 0x05, 0x0c};
    /* two items for the same extension merge, like singular messages */
    static const unsigned char merged[] = {0x0b, 0x10, 0x04, 0x1a, 0x02,
                                           0x08, 0x05, 0x0c, 0x0b, 0x10,
                                           0x04, 0x1a, 0x02, 0x08, 0x07,
                                           0x0c};
    static const unsigned char merged_want[] = {0x0b, 0x10, 0x04, 0x1a,
                                                0x02, 0x08, 0x07, 0x0c};
    pbw_schema *schema = NULL;
    const pbw_descriptor *set;
    const pbw_field *ext;
    pbw_message *msg = NULL;
    pbw_value *v;
    uint8_t *out = NULL;
    size_t out_len = 0;

    CHECK_OK(pbw_descriptor_set_load(ms_set, sizeof(ms_set), &schema));
    set = pbw_schema_find(schema, "ms.Set");
    CHECK(set != NULL);
    ext = pbw_descriptor_find_field(set, 4);
    CHECK(ext != NULL && pbw_field_is_extension(ext) == 1);
    CHECK_OK(pbw_decode(set, item, sizeof(item), &msg));
    v = pbw_message_get(msg, 4);
    CHECK(v != NULL && pbw_value_get_kind(v) == PBW_VALUE_MESSAGE);
    CHECK(pbw_value_get_int(pbw_message_get(pbw_value_get_message(v), 1)) ==
          5);
    CHECK(pbw_message_unknown_len(msg) == 0);
    CHECK_OK(pbw_message_encoded_size(msg, &out_len));
    CHECK(out_len == sizeof(item));
    CHECK_OK(pbw_encode(msg, &out, &out_len));
    CHECK(out_len == sizeof(item) && memcmp(out, item, out_len) == 0);
    pbw_free(out);
    out = NULL;
    pbw_message_free(msg);
    msg = NULL;
    /* undeclared type id: kept as unknown bytes, byte-identical */
    CHECK_OK(pbw_decode(set, foreign, sizeof(foreign), &msg));
    CHECK(pbw_message_get(msg, 4) == NULL);
    CHECK(pbw_message_unknown_len(msg) == sizeof(foreign));
    CHECK_OK(pbw_encode(msg, &out, &out_len));
    CHECK(out_len == sizeof(foreign) && memcmp(out, foreign, out_len) == 0);
    pbw_free(out);
    out = NULL;
    pbw_message_free(msg);
    msg = NULL;
    /* repeated items merge; re-encode is the one merged item */
    CHECK_OK(pbw_decode(set, merged, sizeof(merged), &msg));
    v = pbw_message_get(msg, 4);
    CHECK(v != NULL);
    CHECK(pbw_value_get_int(pbw_message_get(pbw_value_get_message(v), 1)) ==
          7);
    CHECK_OK(pbw_encode(msg, &out, &out_len));
    CHECK(out_len == sizeof(merged_want) &&
          memcmp(out, merged_want, out_len) == 0);
    pbw_free(out);
    out = NULL;
    pbw_message_free(msg);
    msg = NULL;
    /* Malformed items: a bad inner message and items missing their id
     * or payload fall back to unknown bytes; a truncated item fails. */
    {
        /* type 4, inner message is a truncated varint */
        static const unsigned char bad_inner[] = {0x0b, 0x10, 0x04,
                                                  0x1a, 0x01, 0x80,
                                                  0x0c};
        /* type 4, payload missing */
        static const unsigned char no_pay[] = {0x0b, 0x10, 0x04,
                                               0x0c};
        /* payload without a type id */
        static const unsigned char no_id[] = {0x0b, 0x1a, 0x02,
                                              0x08, 0x05, 0x0c};
        /* truncated payload length: rejected, not preserved */
        static const unsigned char cut[] = {0x0b, 0x10, 0x04, 0x1a,
                                            0x02, 0x08};
        const unsigned char *vecs[] = {bad_inner, no_pay, no_id};
        size_t lens[] = {sizeof(bad_inner), sizeof(no_pay),
                         sizeof(no_id)};
        size_t k;

        for (k = 0; k < 3; k++) {
            CHECK_OK(pbw_decode(set, vecs[k], lens[k], &msg));
            CHECK(pbw_message_get(msg, 4) == NULL);
            CHECK(pbw_message_unknown_len(msg) == lens[k]);
            CHECK_OK(pbw_encode(msg, &out, &out_len));
            CHECK(out_len == lens[k] &&
                  memcmp(out, vecs[k], out_len) == 0);
            pbw_free(out);
            out = NULL;
            pbw_message_free(msg);
            msg = NULL;
        }
        CHECK(pbw_decode(set, cut, sizeof(cut), &msg) != PBW_OK);
        CHECK(msg == NULL);
    }
    pbw_schema_free(schema);
}

/* A hand-built FileDescriptorSet with one service (shop.Orders: Place is
 * client-streaming, Watch is server-streaming). Written by hand so the
 * service loader is covered without protoc at build time. */
static const unsigned char svc_set[] = {
    0x0a, 0x76, 0x0a, 0x09, 0x73, 0x76, 0x63, 0x2e, 0x70, 0x72, 0x6f, 0x74,
    0x6f, 0x12, 0x04, 0x73, 0x68, 0x6f, 0x70, 0x22, 0x07, 0x0a, 0x05, 0x4f,
    0x72, 0x64, 0x65, 0x72, 0x32, 0x52, 0x0a, 0x06, 0x4f, 0x72, 0x64, 0x65,
    0x72, 0x73, 0x12, 0x23, 0x0a, 0x05, 0x50, 0x6c, 0x61, 0x63, 0x65, 0x12,
    0x0b, 0x2e, 0x73, 0x68, 0x6f, 0x70, 0x2e, 0x4f, 0x72, 0x64, 0x65, 0x72,
    0x1a, 0x0b, 0x2e, 0x73, 0x68, 0x6f, 0x70, 0x2e, 0x4f, 0x72, 0x64, 0x65,
    0x72, 0x28, 0x01, 0x12, 0x23, 0x0a, 0x05, 0x57, 0x61, 0x74, 0x63, 0x68,
    0x12, 0x0b, 0x2e, 0x73, 0x68, 0x6f, 0x70, 0x2e, 0x4f, 0x72, 0x64, 0x65,
    0x72, 0x1a, 0x0b, 0x2e, 0x73, 0x68, 0x6f, 0x70, 0x2e, 0x4f, 0x72, 0x64,
    0x65, 0x72, 0x30, 0x01, 0x62, 0x06, 0x70, 0x72, 0x6f, 0x74, 0x6f, 0x33};

/* protoc-generated FileDescriptorSet with all four streaming shapes plus an
 * extension (tests/data/desc_service.proto). Regenerate with:
 * protoc -Itests/data --descriptor_set_out=/tmp/svc.pb \
 *     tests/data/desc_service.proto && xxd -i /tmp/svc.pb */
static const unsigned char svc2_set[] = {

    0x0a, 0xa6, 0x02, 0x0a, 0x12, 0x64, 0x65, 0x73, 0x63, 0x5f, 0x73, 0x65,
    0x72, 0x76, 0x69, 0x63, 0x65, 0x2e, 0x70, 0x72, 0x6f, 0x74, 0x6f, 0x12,
    0x0a, 0x70, 0x62, 0x77, 0x69, 0x72, 0x65, 0x2e, 0x73, 0x76, 0x63, 0x22,
    0x1c, 0x0a, 0x03, 0x4a, 0x6f, 0x62, 0x12, 0x0e, 0x0a, 0x02, 0x69, 0x64,
    0x18, 0x01, 0x20, 0x01, 0x28, 0x09, 0x52, 0x02, 0x69, 0x64, 0x2a, 0x05,
    0x08, 0x64, 0x10, 0xc8, 0x01, 0x32, 0xb8, 0x01, 0x0a, 0x04, 0x4a, 0x6f,
    0x62, 0x73, 0x12, 0x27, 0x0a, 0x03, 0x47, 0x65, 0x74, 0x12, 0x0f, 0x2e,
    0x70, 0x62, 0x77, 0x69, 0x72, 0x65, 0x2e, 0x73, 0x76, 0x63, 0x2e, 0x4a,
    0x6f, 0x62, 0x1a, 0x0f, 0x2e, 0x70, 0x62, 0x77, 0x69, 0x72, 0x65, 0x2e,
    0x73, 0x76, 0x63, 0x2e, 0x4a, 0x6f, 0x62, 0x12, 0x2c, 0x0a, 0x06, 0x53,
    0x75, 0x62, 0x6d, 0x69, 0x74, 0x12, 0x0f, 0x2e, 0x70, 0x62, 0x77, 0x69,
    0x72, 0x65, 0x2e, 0x73, 0x76, 0x63, 0x2e, 0x4a, 0x6f, 0x62, 0x1a, 0x0f,
    0x2e, 0x70, 0x62, 0x77, 0x69, 0x72, 0x65, 0x2e, 0x73, 0x76, 0x63, 0x2e,
    0x4a, 0x6f, 0x62, 0x28, 0x01, 0x12, 0x2b, 0x0a, 0x05, 0x57, 0x61, 0x74,
    0x63, 0x68, 0x12, 0x0f, 0x2e, 0x70, 0x62, 0x77, 0x69, 0x72, 0x65, 0x2e,
    0x73, 0x76, 0x63, 0x2e, 0x4a, 0x6f, 0x62, 0x1a, 0x0f, 0x2e, 0x70, 0x62,
    0x77, 0x69, 0x72, 0x65, 0x2e, 0x73, 0x76, 0x63, 0x2e, 0x4a, 0x6f, 0x62,
    0x30, 0x01, 0x12, 0x2c, 0x0a, 0x04, 0x43, 0x68, 0x61, 0x74, 0x12, 0x0f,
    0x2e, 0x70, 0x62, 0x77, 0x69, 0x72, 0x65, 0x2e, 0x73, 0x76, 0x63, 0x2e,
    0x4a, 0x6f, 0x62, 0x1a, 0x0f, 0x2e, 0x70, 0x62, 0x77, 0x69, 0x72, 0x65,
    0x2e, 0x73, 0x76, 0x63, 0x2e, 0x4a, 0x6f, 0x62, 0x28, 0x01, 0x30, 0x01,
    0x3a, 0x2b, 0x0a, 0x08, 0x70, 0x72, 0x69, 0x6f, 0x72, 0x69, 0x74, 0x79,
    0x12, 0x0f, 0x2e, 0x70, 0x62, 0x77, 0x69, 0x72, 0x65, 0x2e, 0x73, 0x76,
    0x63, 0x2e, 0x4a, 0x6f, 0x62, 0x18, 0x64, 0x20, 0x01, 0x28, 0x05, 0x52,
    0x08, 0x70, 0x72, 0x69, 0x6f, 0x72, 0x69, 0x74, 0x79};

static void
test_services_protoc(void)
{
    pbw_schema *schema = NULL;
    const pbw_service *svc;
    const pbw_method *get;
    const pbw_method *submit;
    const pbw_method *watch;
    const pbw_method *chat;
    const pbw_descriptor *job;
    const pbw_field *pri;

    CHECK_OK(pbw_descriptor_set_load(svc2_set, sizeof(svc2_set), &schema));
    if (schema == NULL) {
        return;
    }
    svc = pbw_schema_service_find(schema, "pbwire.svc.Jobs");
    CHECK(svc != NULL);
    if (svc == NULL) {
        pbw_schema_free(schema);
        return;
    }
    CHECK(pbw_service_method_count(svc) == 4);
    get = pbw_service_method_find(svc, "Get");
    submit = pbw_service_method_find(svc, "Submit");
    watch = pbw_service_method_find(svc, "Watch");
    chat = pbw_service_method_find(svc, "Chat");
    CHECK(get != NULL && submit != NULL && watch != NULL && chat != NULL);
    /* unary, client-streaming, server-streaming, bidirectional */
    CHECK(pbw_method_client_streaming(get) == 0);
    CHECK(pbw_method_server_streaming(get) == 0);
    CHECK(pbw_method_client_streaming(submit) == 1);
    CHECK(pbw_method_server_streaming(submit) == 0);
    CHECK(pbw_method_client_streaming(watch) == 0);
    CHECK(pbw_method_server_streaming(watch) == 1);
    CHECK(pbw_method_client_streaming(chat) == 1);
    CHECK(pbw_method_server_streaming(chat) == 1);
    CHECK(strcmp(pbw_method_input_type(get), "pbwire.svc.Job") == 0);
    CHECK(strcmp(pbw_method_output_type(chat), "pbwire.svc.Job") == 0);
    /* the extension attached to its extendee */
    job = pbw_schema_find(schema, "pbwire.svc.Job");
    CHECK(job != NULL);
    pri = job != NULL ? pbw_descriptor_find_field(job, 100) : NULL;
    CHECK(pri != NULL);
    if (pri != NULL) {
        CHECK(pbw_field_type(pri) == PBW_TYPE_INT32);
        CHECK(pbw_field_is_extension(pri));
        /* JSON extensions parse by full name and print the same way. */
        {
            static const char ein[] = "{\"[pbwire.svc.priority]\":5}";
            pbw_message *em = NULL;
            char *eout = NULL;
            size_t elen = 0;

            CHECK_OK(pbw_json_parse(job, ein, strlen(ein), &em));
            CHECK(pbw_message_get(em, 100) != NULL);
            CHECK_OK(pbw_json_print(em, &eout, &elen));
            CHECK(elen == strlen(ein) && memcmp(eout, ein, elen) == 0);
            pbw_free(eout);
            pbw_message_free(em);
        }
    }
    pbw_schema_free(schema);
    schema = NULL;
    /* truncated inputs fail instead of loading partial services */
    {
        static const size_t cuts[] = {1, 10, 40, 100, 200};
        size_t k;

        for (k = 0; k < sizeof(cuts) / sizeof(cuts[0]); k++) {
            CHECK(pbw_descriptor_set_load(svc2_set, cuts[k], &schema) !=
                  PBW_OK);
            pbw_schema_free(schema);
            schema = NULL;
        }
    }
    /* corrupting the extendee name breaks the load: the extension has no
     * target. The extendee is the last occurrence of the message name. */
    {
        static const char target[] = "pbwire.svc.Job";
        unsigned char *mut;
        size_t i;
        size_t at = 0;
        int found = 0;

        mut = (unsigned char *)malloc(sizeof(svc2_set));
        CHECK(mut != NULL);
        if (mut == NULL) {
            return;
        }
        memcpy(mut, svc2_set, sizeof(svc2_set));
        for (i = 0; i + sizeof(target) - 1 <= sizeof(svc2_set); i++) {
            if (memcmp(mut + i, target, sizeof(target) - 1) == 0) {
                at = i;
                found = 1;
            }
        }
        CHECK(found);
        mut[at] = (unsigned char)'X';
        CHECK_STATUS(PBW_ERR_NOT_FOUND,
                     pbw_descriptor_set_load(mut, sizeof(svc2_set),
                                             &schema));
        pbw_schema_free(schema);
        free(mut);
    }
}

/* A hand-built FileDescriptorSet whose message declares field number 1
 * twice. The loader accepts the bytes; linking rejects the duplicate. */
static const unsigned char dupnum_set[] = {
    0x0a, 0x24, 0x0a, 0x07, 0x64, 0x2e, 0x70, 0x72, 0x6f, 0x74, 0x6f,
    0x22, 0x19, 0x0a, 0x01, 0x4d, 0x12, 0x09, 0x0a, 0x01, 0x61, 0x18,
    0x01, 0x20, 0x01, 0x28, 0x05, 0x12, 0x09, 0x0a, 0x01, 0x62, 0x18,
    0x01, 0x20, 0x01, 0x28, 0x05};

static void
test_duplicate_field_number(void)
{
    pbw_schema *schema = NULL;

    CHECK_STATUS(PBW_ERR_INVALID,
                 pbw_descriptor_set_load(dupnum_set, sizeof(dupnum_set),
                                         &schema));
    pbw_schema_free(schema);
}

static void
test_services(void)
{
    pbw_schema *schema = NULL;
    const pbw_service *svc;
    const pbw_method *place;
    const pbw_method *watch;

    CHECK_OK(pbw_descriptor_set_load(svc_set, sizeof(svc_set), &schema));
    CHECK(pbw_schema_service_count(schema) == 1);
    CHECK(pbw_schema_service_count(NULL) == 0);
    svc = pbw_schema_service_find(schema, "shop.Orders");
    CHECK(svc != NULL);
    CHECK(svc == pbw_schema_service_at(schema, 0));
    CHECK(pbw_schema_service_at(schema, 1) == NULL);
    CHECK(pbw_schema_service_find(schema, "shop.Missing") == NULL);
    CHECK(strcmp(pbw_service_name(svc), "shop.Orders") == 0);
    CHECK(pbw_service_method_count(svc) == 2);
    place = pbw_service_method_find(svc, "Place");
    watch = pbw_service_method_find(svc, "Watch");
    CHECK(place != NULL && watch != NULL);
    CHECK(place == pbw_service_method_at(svc, 0));
    CHECK(watch == pbw_service_method_at(svc, 1));
    CHECK(pbw_service_method_at(svc, 2) == NULL);
    CHECK(pbw_service_method_find(svc, "Cancel") == NULL);
    CHECK(strcmp(pbw_method_name(place), "Place") == 0);
    CHECK(strcmp(pbw_method_input_type(place), "shop.Order") == 0);
    CHECK(strcmp(pbw_method_output_type(place), "shop.Order") == 0);
    CHECK(pbw_method_client_streaming(place) == 1);
    CHECK(pbw_method_server_streaming(place) == 0);
    CHECK(pbw_method_client_streaming(watch) == 0);
    CHECK(pbw_method_server_streaming(watch) == 1);
    /* the message in the same file still loads beside the service */
    CHECK(pbw_schema_find(schema, "shop.Order") != NULL);
    pbw_schema_free(schema);
}

/* Minimal FileDescriptorSet writer for hostile-input tests: no protoc,
 * fixed buffers (the longest hostile name below is 600 bytes). */
typedef struct {
    uint8_t *p;
    size_t len;
} ds_w;

static void
w_varint(ds_w *w, uint64_t v)
{
    do {
        w->p[w->len++] = (uint8_t)((v & 0x7f) | (v > 0x7f ? 0x80 : 0));
        v >>= 7;
    } while (v > 0);
}

static void
w_tag(ds_w *w, uint32_t field, uint32_t wire)
{
    w_varint(w, ((uint64_t)field << 3) | wire);
}

static void
w_bytes(ds_w *w, const void *data, size_t n)
{
    memcpy(w->p + w->len, data, n);
    w->len += n;
}

static void
w_str(ds_w *w, uint32_t field, const char *s)
{
    w_tag(w, field, 2);
    w_varint(w, strlen(s));
    w_bytes(w, s, strlen(s));
}

static void
w_sub(ds_w *w, uint32_t field, const uint8_t *body, size_t n)
{
    w_tag(w, field, 2);
    w_varint(w, n);
    w_bytes(w, body, n);
}

static void
w_int(ds_w *w, uint32_t field, uint64_t v)
{
    w_tag(w, field, 0);
    w_varint(w, v);
}

/* Hostile descriptor bytes: over-long names (LIMIT), extensions in oneofs
 * and wrapped oneof indexes (INVALID). Built by hand, so the loader's
 * hostile paths are covered without protoc. */
static void
test_load_hostile(void)
{
    pbw_schema *schema = NULL;
    uint8_t file[2048];
    uint8_t inner[1600];
    uint8_t method[1400];
    uint8_t set[2200];
    static char big[601];
    ds_w w;
    ds_w iw;
    ds_w mw;
    ds_w sw;
    size_t i;

    for (i = 0; i < sizeof(big) - 1; i++) {
        big[i] = 'A';
    }
    big[sizeof(big) - 1] = '\0';
    /* 1. a 600-byte message name exceeds DS_MAX_NAME: LIMIT with a reason
     * (bare file names are unused by the loader, so they never fail). */
    iw.p = inner;
    iw.len = 0;
    w_str(&iw, 1, big);
    w.p = file;
    w.len = 0;
    w_str(&w, 1, "n.proto");
    w_str(&w, 2, "p");
    w_sub(&w, 4, inner, iw.len);
    w_str(&w, 12, "proto3");
    sw.p = set;
    sw.len = 0;
    w_sub(&sw, 1, file, w.len);
    CHECK_STATUS(PBW_ERR_LIMIT,
                 pbw_descriptor_set_load(set, sw.len, &schema));
    CHECK(schema != NULL && strlen(pbw_schema_error(schema)) > 0);
    pbw_schema_free(schema);
    schema = NULL;
    /* 2. a 600-byte method input type: LIMIT as well */
    mw.p = method;
    mw.len = 0;
    w_str(&mw, 1, "Go");
    w_str(&mw, 2, big);
    w_str(&mw, 3, ".p.M");
    w.p = file;
    w.len = 0;
    w_str(&w, 1, "s.proto");
    w_str(&w, 2, "p");
    {
        uint8_t msg[32];
        ds_w mwb;

        mwb.p = msg;
        mwb.len = 0;
        w_str(&mwb, 1, "M");
        w_sub(&w, 4, msg, mwb.len);
    }
    {
        uint8_t svc[1500];
        ds_w svw;

        svw.p = svc;
        svw.len = 0;
        w_str(&svw, 1, "S");
        w_sub(&svw, 2, method, mw.len);
        w_sub(&w, 6, svc, svw.len);
    }
    w_str(&w, 12, "proto3");
    sw.len = 0;
    w_sub(&sw, 1, file, w.len);
    CHECK_STATUS(PBW_ERR_LIMIT,
                 pbw_descriptor_set_load(set, sw.len, &schema));
    pbw_schema_free(schema);
    schema = NULL;
    /* 3. an extension carrying oneof_index: INVALID */
    iw.len = 0;
    w_str(&iw, 1, "M");
    w.len = 0;
    w_str(&w, 1, "e.proto");
    w_str(&w, 2, "p");
    w_sub(&w, 4, inner, iw.len);
    mw.len = 0;
    w_str(&mw, 1, "e");
    w_str(&mw, 2, ".p.M");
    w_int(&mw, 3, 100);
    w_int(&mw, 4, 1);
    w_int(&mw, 5, 9);
    w_int(&mw, 9, 0);
    w_sub(&w, 7, method, mw.len);
    w_str(&w, 12, "proto3");
    sw.len = 0;
    w_sub(&sw, 1, file, w.len);
    CHECK_STATUS(PBW_ERR_INVALID,
                 pbw_descriptor_set_load(set, sw.len, &schema));
    CHECK(schema != NULL && strlen(pbw_schema_error(schema)) > 0);
    pbw_schema_free(schema);
    schema = NULL;
    /* 4. a field oneof_index of UINT32_MAX wraps to 0: INVALID */
    iw.len = 0;
    w_str(&iw, 1, "M");
    {
        uint8_t oneof[16];
        ds_w ow;
        uint8_t fld[64];
        ds_w fw;

        ow.p = oneof;
        ow.len = 0;
        w_str(&ow, 1, "o");
        w_sub(&iw, 8, oneof, ow.len);
        fw.p = fld;
        fw.len = 0;
        w_str(&fw, 1, "f");
        w_int(&fw, 3, 1);
        w_int(&fw, 4, 1);
        w_int(&fw, 5, 9);
        w_int(&fw, 9, 0xffffffffull);
        w_sub(&iw, 2, fld, fw.len);
    }
    w.len = 0;
    w_str(&w, 1, "o.proto");
    w_str(&w, 2, "p");
    w_sub(&w, 4, inner, iw.len);
    w_str(&w, 12, "proto3");
    sw.len = 0;
    w_sub(&sw, 1, file, w.len);
    CHECK_STATUS(PBW_ERR_INVALID,
                 pbw_descriptor_set_load(set, sw.len, &schema));
    pbw_schema_free(schema);
    schema = NULL;
    /* 5. an enum value without a name: INVALID */
    iw.len = 0;
    w_str(&iw, 1, "E");
    {
        uint8_t val[16];
        ds_w vw;

        vw.p = val;
        vw.len = 0;
        w_int(&vw, 2, 1);
        w_sub(&iw, 2, val, vw.len);
    }
    w.len = 0;
    w_str(&w, 1, "n.proto");
    w_str(&w, 2, "p");
    w_sub(&w, 5, inner, iw.len);
    w_str(&w, 12, "proto3");
    sw.len = 0;
    w_sub(&sw, 1, file, w.len);
    CHECK_STATUS(PBW_ERR_INVALID,
                 pbw_descriptor_set_load(set, sw.len, &schema));
    pbw_schema_free(schema);
    schema = NULL;
    /* 6. a 600-byte enum value name: LIMIT */
    iw.len = 0;
    w_str(&iw, 1, "E");
    {
        uint8_t val[700];
        ds_w vw;

        vw.p = val;
        vw.len = 0;
        w_str(&vw, 1, big);
        w_int(&vw, 2, 1);
        w_sub(&iw, 2, val, vw.len);
    }
    w.len = 0;
    w_str(&w, 1, "n.proto");
    w_str(&w, 2, "p");
    w_sub(&w, 5, inner, iw.len);
    w_str(&w, 12, "proto3");
    sw.len = 0;
    w_sub(&sw, 1, file, w.len);
    CHECK_STATUS(PBW_ERR_LIMIT,
                 pbw_descriptor_set_load(set, sw.len, &schema));
    pbw_schema_free(schema);
    schema = NULL;
    /* 7. a field pointing at a oneof that was never declared: INVALID. The
     * shape protoc never emits silently aliases the members of that index,
     * so the loader rejects it after the message body has been walked. */
    iw.len = 0;
    w_str(&iw, 1, "M");
    {
        uint8_t oneof[16];
        ds_w ow;
        uint8_t fld[64];
        ds_w fw;

        ow.p = oneof;
        ow.len = 0;
        w_str(&ow, 1, "o");
        w_sub(&iw, 8, oneof, ow.len);
        /* field a: oneof_index 0 on the wire = group 1, declared */
        fw.p = fld;
        fw.len = 0;
        w_str(&fw, 1, "a");
        w_int(&fw, 3, 1);
        w_int(&fw, 4, 1);
        w_int(&fw, 5, 9);
        w_int(&fw, 9, 0);
        w_sub(&iw, 2, fld, fw.len);
        /* field b: oneof_index 1 on the wire = group 2, undeclared */
        fw.len = 0;
        w_str(&fw, 1, "b");
        w_int(&fw, 3, 2);
        w_int(&fw, 4, 1);
        w_int(&fw, 5, 9);
        w_int(&fw, 9, 1);
        w_sub(&iw, 2, fld, fw.len);
    }
    w.len = 0;
    w_str(&w, 1, "oo.proto");
    w_str(&w, 2, "p");
    w_sub(&w, 4, inner, iw.len);
    w_str(&w, 12, "proto3");
    sw.len = 0;
    w_sub(&sw, 1, file, w.len);
    CHECK_STATUS(PBW_ERR_INVALID,
                 pbw_descriptor_set_load(set, sw.len, &schema));
    CHECK(schema != NULL && strlen(pbw_schema_error(schema)) > 0);
    pbw_schema_free(schema);
    schema = NULL;
    /* 8. a map entry that does not have exactly optional key #1 and value #2:
     * INVALID. The JSON/text printers read those two numbers directly, so a
     * other shape would fail the whole print much later. */
    iw.len = 0;
    w_str(&iw, 1, "E");
    {
        uint8_t opt[8];
        ds_w ow;
        uint8_t fld[64];
        ds_w fw;

        ow.p = opt;
        ow.len = 0;
        w_int(&ow, 7, 1); /* MessageOptions.map_entry */
        w_sub(&iw, 7, opt, ow.len);
        fw.p = fld;
        fw.len = 0;
        w_str(&fw, 1, "value");
        w_int(&fw, 3, 2);
        w_int(&fw, 4, 1);
        w_int(&fw, 5, 9);
        w_sub(&iw, 2, fld, fw.len);
    }
    w.len = 0;
    w_str(&w, 1, "me.proto");
    w_str(&w, 2, "p");
    w_sub(&w, 4, inner, iw.len);
    w_str(&w, 12, "proto3");
    sw.len = 0;
    w_sub(&sw, 1, file, w.len);
    CHECK_STATUS(PBW_ERR_INVALID,
                 pbw_descriptor_set_load(set, sw.len, &schema));
    pbw_schema_free(schema);
    schema = NULL;
    /* 9. the shape protoc emits (key #1, value #2, both optional) loads and
     * stays a map entry: the check above must not over-reject. */
    iw.len = 0;
    w_str(&iw, 1, "Good");
    {
        uint8_t opt[8];
        ds_w ow;
        uint8_t fld[64];
        ds_w fw;

        ow.p = opt;
        ow.len = 0;
        w_int(&ow, 7, 1);
        w_sub(&iw, 7, opt, ow.len);
        fw.p = fld;
        fw.len = 0;
        w_str(&fw, 1, "key");
        w_int(&fw, 3, 1);
        w_int(&fw, 4, 1);
        w_int(&fw, 5, 5); /* int32 */
        w_sub(&iw, 2, fld, fw.len);
        fw.len = 0;
        w_str(&fw, 1, "value");
        w_int(&fw, 3, 2);
        w_int(&fw, 4, 1);
        w_int(&fw, 5, 9); /* string */
        w_sub(&iw, 2, fld, fw.len);
    }
    w.len = 0;
    w_str(&w, 1, "good.proto");
    w_str(&w, 2, "p");
    w_sub(&w, 4, inner, iw.len);
    w_str(&w, 12, "proto3");
    sw.len = 0;
    w_sub(&sw, 1, file, w.len);
    CHECK_OK(pbw_descriptor_set_load(set, sw.len, &schema));
    CHECK(pbw_descriptor_is_map_entry(pbw_schema_find(schema, "p.Good")));
    pbw_schema_free(schema);
}

static void
test_service_resolve(void)
{
    pbw_schema *schema = NULL;
    const pbw_descriptor *in = (const pbw_descriptor *)0x1;
    const pbw_descriptor *out = (const pbw_descriptor *)0x1;

    CHECK_OK(pbw_descriptor_set_load(svc_set, sizeof(svc_set), &schema));
    CHECK_OK(pbw_service_resolve(schema, "shop.Orders", "Place", &in, &out));
    CHECK(in == pbw_schema_find(schema, "shop.Order"));
    CHECK(out == pbw_schema_find(schema, "shop.Order"));
    /* NULL slots are allowed; unknown names and NULLs fail loudly */
    CHECK_OK(pbw_service_resolve(schema, "shop.Orders", "Place", NULL, NULL));
    CHECK_STATUS(PBW_ERR_INVALID,
                 pbw_service_resolve(schema, "shop.Missing", "Place", &in,
                                     &out));
    CHECK_STATUS(PBW_ERR_INVALID,
                 pbw_service_resolve(schema, "shop.Orders", "Cancel", &in,
                                     &out));
    CHECK_STATUS(PBW_ERR_INVALID,
                 pbw_service_resolve(NULL, "shop.Orders", "Place", &in,
                                     &out));
    CHECK_STATUS(PBW_ERR_INVALID,
                 pbw_service_resolve(schema, NULL, "Place", &in, &out));
    CHECK_STATUS(PBW_ERR_INVALID,
                 pbw_service_resolve(schema, "shop.Orders", NULL, &in, &out));
    CHECK(in == NULL && out == NULL);
    pbw_schema_free(schema);
}

static void
test_load_limits(void)
{
    pbw_schema *schema = NULL;
    pbw_limits lim = {0, 0};

    /* NULL or zeroed limits behave exactly like the plain loader */
    CHECK_OK(
        pbw_descriptor_set_load_limits(svc_set, sizeof(svc_set), NULL, &schema));
    CHECK(pbw_schema_find(schema, "shop.Order") != NULL);
    pbw_schema_free(schema);
    CHECK_OK(
        pbw_descriptor_set_load_limits(svc_set, sizeof(svc_set), &lim, &schema));
    pbw_schema_free(schema);
    CHECK_STATUS(PBW_ERR_INVALID,
                 pbw_descriptor_set_load_limits(NULL, 0, &lim, &schema));
    CHECK_STATUS(PBW_ERR_INVALID,
                 pbw_descriptor_set_load_limits(svc_set, sizeof(svc_set), &lim,
                                                NULL));
    /* a byte cap below the input fails with an explainable schema */
    lim.max_bytes = sizeof(svc_set) - 1;
    CHECK_STATUS(PBW_ERR_LIMIT,
                 pbw_descriptor_set_load_limits(svc_set, sizeof(svc_set), &lim,
                                                &schema));
    CHECK(schema != NULL && strlen(pbw_schema_error(schema)) > 0);
    pbw_schema_free(schema);
    lim.max_bytes = sizeof(svc_set);
    CHECK_OK(
        pbw_descriptor_set_load_limits(svc_set, sizeof(svc_set), &lim, &schema));
    pbw_schema_free(schema);
    lim.max_bytes = 0;
    /* above PBW_MAX_DEPTH clamps: a flat set still loads */
    lim.max_depth = PBW_MAX_DEPTH + 100;
    CHECK_OK(
        pbw_descriptor_set_load_limits(svc_set, sizeof(svc_set), &lim, &schema));
    pbw_schema_free(schema);
    /* desc_set nests Outer.Inner one level deep: depth 1 rejects it */
    lim.max_depth = 1;
    CHECK_STATUS(PBW_ERR_LIMIT,
                 pbw_descriptor_set_load_limits(desc_set, sizeof(desc_set),
                                                &lim, &schema));
    pbw_schema_free(schema);
    lim.max_depth = 2;
    CHECK_OK(
        pbw_descriptor_set_load_limits(desc_set, sizeof(desc_set), &lim, &schema));
    CHECK(pbw_schema_find(schema, "pbwire.test.Outer.Inner") != NULL);
    pbw_schema_free(schema);
}

static void
test_schema(void)
{
    pbw_schema *schema = NULL;
    const pbw_descriptor *outer;
    const pbw_descriptor *inner;
    const pbw_descriptor *tags;
    const pbw_descriptor *old_msg;
    const pbw_descriptor *legacy;
    const pbw_descriptor *color;
    const pbw_field *field;
    const char *name = NULL;
    int32_t number = 0;

    CHECK_STATUS(PBW_OK, pbw_descriptor_set_load(desc_set, sizeof(desc_set), &schema));
    if (schema == NULL) {
        return;
    }
    outer = pbw_schema_find(schema, "pbwire.test.Outer");
    inner = pbw_schema_find(schema, "pbwire.test.Outer.Inner");
    tags = pbw_schema_find(schema, "pbwire.test.Outer.TagsEntry");
    old_msg = pbw_schema_find(schema, "pbwire.test.Old");
    legacy = pbw_schema_find(schema, "pbwire.test.Legacy");
    color = pbw_schema_find(schema, "pbwire.test.Color");
    CHECK(outer != NULL && inner != NULL && tags != NULL);
    CHECK(old_msg != NULL && legacy != NULL && color != NULL);
    /* names are stored fully qualified, without the leading dot */
    CHECK(pbw_schema_find(schema, ".pbwire.test.Outer") == NULL);
    CHECK(!pbw_descriptor_is_enum(outer));
    CHECK(pbw_descriptor_is_enum(legacy));

    /* scalar, default presence and proto3 default packing */
    field = pbw_descriptor_find_field_name(outer, "plain");
    CHECK(field != NULL);
    CHECK(pbw_field_number(field) == 1);
    CHECK(pbw_field_type(field) == PBW_TYPE_INT32);
    CHECK(pbw_field_label(field) == PBW_LABEL_OPTIONAL);
    CHECK(!pbw_field_is_packed(field));
    field = pbw_descriptor_find_field_name(outer, "auto_packed");
    CHECK(field != NULL && pbw_field_is_packed(field));
    CHECK(pbw_field_label(field) == PBW_LABEL_REPEATED);
    field = pbw_descriptor_find_field_name(outer, "unpacked");
    CHECK(field != NULL && pbw_field_type(field) == PBW_TYPE_SINT32);
    CHECK(!pbw_field_is_packed(field));

    /* nested type and map entry */
    field = pbw_descriptor_find_field_name(outer, "inner");
    CHECK(field != NULL && pbw_field_type(field) == PBW_TYPE_MESSAGE);
    CHECK(pbw_field_message_type(field) == inner);
    field = pbw_descriptor_find_field_name(outer, "tags");
    CHECK(field != NULL && pbw_field_type(field) == PBW_TYPE_MESSAGE);
    CHECK(pbw_field_label(field) == PBW_LABEL_REPEATED);
    CHECK(pbw_field_message_type(field) == tags);
    CHECK(pbw_descriptor_find_field_name(tags, "key") != NULL);
    CHECK(pbw_descriptor_find_field_name(tags, "value") != NULL);

    /* utf8_validation follows the reference: the proto3 `string` field
     * validates at parse, the proto2 one (Old.name) does not. */
    CHECK(pbw_field_requires_utf8_validation(
              pbw_descriptor_find_field_name(outer, "b")) == 1);
    CHECK(pbw_field_requires_utf8_validation(
              pbw_descriptor_find_field_name(outer, "plain")) == 0);
    CHECK(pbw_field_requires_utf8_validation(
              pbw_descriptor_find_field_name(old_msg, "name")) == 0);
    {
        /* Outer.b(7) = one 0xFF byte: proto3 rejects it. */
        static const unsigned char bad3[] = {0x3A, 0x01, 0xFF};
        /* Old.id(1) = 7 (required), Old.name(2) = one 0xFF byte: proto2
         * keeps the malformed string byte for byte. */
        static const unsigned char bad2[] = {0x08, 0x07, 0x12, 0x01, 0xFF};
        pbw_message *m = NULL;
        uint8_t *bytes = NULL;
        size_t bytes_len = 0;

        CHECK_STATUS(PBW_ERR_INVALID, pbw_decode(outer, bad3, sizeof(bad3), &m));
        CHECK(m == NULL);
        CHECK_OK(pbw_decode(old_msg, bad2, sizeof(bad2), &m));
        CHECK_OK(pbw_encode(m, &bytes, &bytes_len));
        CHECK(bytes_len == sizeof(bad2) && memcmp(bytes, bad2, bytes_len) == 0);
        pbw_free(bytes);
        pbw_message_free(m);
    }

    /* oneof membership: both cases share one non-zero index */
    field = pbw_descriptor_find_field_name(outer, "a");
    CHECK(field != NULL && pbw_field_oneof_index(field) != 0);
    number = (int32_t)pbw_field_oneof_index(field);
    field = pbw_descriptor_find_field_name(outer, "b");
    CHECK(field != NULL && (int32_t)pbw_field_oneof_index(field) == number);

    /* cross-file references */
    field = pbw_descriptor_find_field_name(outer, "legacy");
    CHECK(field != NULL && pbw_field_message_type(field) == old_msg);
    field = pbw_descriptor_find_field_name(outer, "color");
    CHECK(field != NULL && pbw_field_type(field) == PBW_TYPE_ENUM);
    CHECK(pbw_field_message_type(field) == color);

    /* proto2 presence and non-packed default */
    field = pbw_descriptor_find_field_name(old_msg, "id");
    CHECK(field != NULL && pbw_field_label(field) == PBW_LABEL_REQUIRED);
    field = pbw_descriptor_find_field_name(old_msg, "vals");
    CHECK(field != NULL && pbw_field_type(field) == PBW_TYPE_SINT64);
    CHECK(pbw_field_label(field) == PBW_LABEL_REPEATED);
    CHECK(!pbw_field_is_packed(field));

    /* a group field loads like a message field: decoding fills the group
     * submessage and encoding writes it back byte for byte */
    {
        const pbw_descriptor *grouped =
            pbw_schema_find(schema, "pbwire.test.Grouped");
        const pbw_field *group_field = NULL;
        const pbw_descriptor *inner_msg = NULL;
        static const uint8_t wire[] = {0x0b, 0x10, 0x07, 0x0c, 0x18, 0x09};
        pbw_message *decoded = NULL;
        pbw_message *group = NULL;
        uint8_t *buf = NULL;
        size_t len = 0;

        CHECK(grouped != NULL);
        CHECK(pbw_descriptor_field_count(grouped) == 2);
        group_field = pbw_descriptor_find_field(grouped, 1);
        CHECK(group_field != NULL && pbw_field_type(group_field) == PBW_TYPE_GROUP);
        CHECK(!pbw_field_is_extension(group_field));
        inner_msg = pbw_field_message_type(group_field);
        CHECK(inner_msg != NULL &&
              strcmp(pbw_descriptor_name(inner_msg),
                     "pbwire.test.Grouped.Inner") == 0);
        CHECK_OK(pbw_decode(grouped, wire, sizeof(wire), &decoded));
        CHECK(pbw_value_get_int32(pbw_message_get(decoded, 3)) == 9);
        group = pbw_value_get_message(pbw_message_get(decoded, 1));
        CHECK(group != NULL);
        CHECK(pbw_value_get_int32(pbw_message_get(group, 2)) == 7);
        CHECK_OK(pbw_encode(decoded, &buf, &len));
        CHECK(len == sizeof(wire) && memcmp(buf, wire, sizeof(wire)) == 0);
        pbw_free(buf);
        pbw_message_free(decoded);
    }

    /* enum values, including a negative one */
    CHECK(pbw_enum_find_value(legacy, "LEGACY_NEG", &number) && number == -2);
    CHECK(pbw_enum_find_name(legacy, 1, &name) && strcmp(name, "LEGACY_ONE") == 0);
    CHECK(pbw_enum_find_value(color, "COLOR_RED", &number) && number == 1);

    pbw_schema_free(schema);
}

static void
test_wire(void)
{
    static const uint8_t expected[] = {0x08, 0x07, 0x12, 0x04, 0x01,
                                       0x02, 0xac, 0x02, 0x18, 0x01};
    pbw_schema *schema = NULL;
    const pbw_descriptor *outer;
    const pbw_descriptor *old_msg;
    pbw_message *msg;
    pbw_message *decoded = NULL;
    pbw_message *nested;
    pbw_value *value;
    uint8_t *buf = NULL;
    size_t len = 0;

    CHECK_STATUS(PBW_OK, pbw_descriptor_set_load(desc_set, sizeof(desc_set), &schema));
    if (schema == NULL) {
        return;
    }
    outer = pbw_schema_find(schema, "pbwire.test.Outer");
    old_msg = pbw_schema_find(schema, "pbwire.test.Old");
    CHECK(outer != NULL && old_msg != NULL);
    if (outer == NULL || old_msg == NULL) {
        pbw_schema_free(schema);
        return;
    }

    /* The loaded descriptor decides field numbers and packing: this is the byte
     * string protoc-generated code produces for the same values. */
    msg = pbw_message_new(outer);
    CHECK_OK(pbw_message_set(msg, 1, pbw_value_new_int(7)));
    CHECK_OK(pbw_message_set(msg, 2, pbw_value_new_int(1)));
    CHECK_OK(pbw_message_set(msg, 2, pbw_value_new_int(2)));
    CHECK_OK(pbw_message_set(msg, 2, pbw_value_new_int(300)));
    CHECK_OK(pbw_message_set(msg, 3, pbw_value_new_int(-1)));
    CHECK_OK(pbw_encode(msg, &buf, &len));
    CHECK(len == sizeof(expected));
    CHECK(len == sizeof(expected) && memcmp(buf, expected, len) == 0);
    CHECK_OK(pbw_decode(outer, buf, len, &decoded));
    value = pbw_message_get(decoded, 2);
    CHECK(pbw_value_count(value) == 3);
    CHECK(pbw_value_get_int32(pbw_value_at(value, 2)) == 300);
    value = pbw_message_get(decoded, 3);
    CHECK(pbw_value_count(value) == 1);
    CHECK(pbw_value_get_int32(pbw_value_at(value, 0)) == -1);
    pbw_message_free(decoded);
    decoded = NULL;
    pbw_free(buf);
    buf = NULL;
    pbw_message_free(msg);

    /* nested message, map entry, oneof and a required field */
    msg = pbw_message_new(outer);
    nested = pbw_message_set_message(msg, 4);
    CHECK(nested != NULL);
    CHECK_OK(pbw_message_set(nested, 1, pbw_value_new_string("hi")));
    nested = pbw_message_set_message(msg, 5);
    CHECK(nested != NULL);
    CHECK_OK(pbw_message_set(nested, 1, pbw_value_new_string("k")));
    CHECK_OK(pbw_message_set(nested, 2, pbw_value_new_int(5)));
    CHECK_OK(pbw_message_set(msg, 6, pbw_value_new_int(42)));
    CHECK_OK(pbw_message_set(msg, 7, pbw_value_new_string("x")));
    CHECK(pbw_message_get(msg, 6) == NULL); /* oneof: b replaced a */
    nested = pbw_message_set_message(msg, 8);
    CHECK(nested != NULL);
    CHECK_OK(pbw_message_set(nested, 1, pbw_value_new_int(9)));
    CHECK_OK(pbw_message_set(msg, 9, pbw_value_new_int(1)));
    CHECK_OK(pbw_encode(msg, &buf, &len));
    CHECK_OK(pbw_decode(outer, buf, len, &decoded));
    CHECK(decoded != NULL);
    if (decoded != NULL) {
        CHECK(strcmp(pbw_value_get_string(pbw_message_get(
                         pbw_value_get_message(pbw_message_get(decoded, 4)), 1)),
                     "hi") == 0);
        value = pbw_message_get(decoded, 5);
        CHECK(pbw_value_count(value) == 1);
        nested = pbw_value_get_message(pbw_value_at(value, 0));
        CHECK(strcmp(pbw_value_get_string(pbw_message_get(nested, 1)), "k") == 0);
        CHECK(pbw_value_get_int32(pbw_message_get(nested, 2)) == 5);
        CHECK(pbw_message_get(decoded, 6) == NULL);
        CHECK(strcmp(pbw_value_get_string(pbw_message_get(decoded, 7)), "x") == 0);
        CHECK(pbw_value_get_int32(pbw_message_get(
                  pbw_value_get_message(pbw_message_get(decoded, 8)), 1)) == 9);
        CHECK(pbw_value_get_int32(pbw_message_get(decoded, 9)) == 1);
        pbw_message_free(decoded);
    }
    pbw_free(buf);
    pbw_message_free(msg);
    pbw_schema_free(schema);
}

static void
test_errors(void)
{
    pbw_schema *schema = NULL;

    CHECK_STATUS(PBW_ERR_INVALID, pbw_descriptor_set_load(NULL, 0, &schema));
    CHECK(schema == NULL);

    /* a truncated set is rejected and says so */
    schema = NULL;
    CHECK(pbw_descriptor_set_load(desc_set, sizeof(desc_set) - 1, &schema) !=
          PBW_OK);
    CHECK(schema != NULL);
    if (schema != NULL) {
        CHECK(pbw_schema_error(schema)[0] != '\0');
        pbw_schema_free(schema);
    }

    /* an empty set is a valid, empty schema */
    schema = NULL;
    CHECK_STATUS(PBW_OK, pbw_descriptor_set_load(desc_set, 0, &schema));
    CHECK(schema != NULL && pbw_schema_find(schema, "pbwire.test.Outer") == NULL);
    pbw_schema_free(schema);
}

/* Two loads of the same bytes are independent schemas: no state is shared, and
 * freeing one must not disturb the other. */
static void
test_independent_loads(void)
{
    pbw_schema *a = NULL;
    pbw_schema *b = NULL;
    const pbw_descriptor *outer_a;
    const pbw_descriptor *outer_b;
    pbw_message *msg;
    uint8_t *buf = NULL;
    size_t len = 0;

    CHECK_STATUS(PBW_OK,
                 pbw_descriptor_set_load(desc_set, sizeof(desc_set), &a));
    CHECK_STATUS(PBW_OK,
                 pbw_descriptor_set_load(desc_set, sizeof(desc_set), &b));
    CHECK(a != NULL && b != NULL && a != b);
    if (a == NULL || b == NULL) {
        pbw_schema_free(a);
        pbw_schema_free(b);
        return;
    }
    CHECK(pbw_schema_error(a)[0] == '\0');
    outer_a = pbw_schema_find(a, "pbwire.test.Outer");
    outer_b = pbw_schema_find(b, "pbwire.test.Outer");
    CHECK(outer_a != NULL && outer_b != NULL && outer_a != outer_b);

    /* the same message built from each schema encodes identically */
    msg = pbw_message_new(outer_a);
    CHECK_OK(pbw_message_set(msg, 1, pbw_value_new_int(7)));
    CHECK_OK(pbw_encode(msg, &buf, &len));
    pbw_message_free(msg);
    pbw_schema_free(a); /* b must be unaffected */
    CHECK(pbw_schema_find(b, "pbwire.test.Outer") == outer_b);
    {
        pbw_message *decoded = NULL;

        CHECK_OK(pbw_decode(outer_b, buf, len, &decoded));
        CHECK(decoded != NULL &&
              pbw_value_get_int32(pbw_message_get(decoded, 1)) == 7);
        pbw_message_free(decoded);
    }
    pbw_free(buf);
    pbw_schema_free(b);
}

/* Number and name lookup must agree for every declared field, and must not
 * answer for numbers or names the message does not have. */
static void
test_lookup_tables(void)
{
    static const char *const names[] = {
        "pbwire.test.Outer", "pbwire.test.Outer.Inner",
        "pbwire.test.Outer.TagsEntry", "pbwire.test.Old",
        "pbwire.test.Grouped"};
    pbw_schema *schema = NULL;
    size_t n;

    CHECK_STATUS(PBW_OK,
                 pbw_descriptor_set_load(desc_set, sizeof(desc_set), &schema));
    if (schema == NULL) {
        return;
    }
    for (n = 0; n < sizeof(names) / sizeof(names[0]); n++) {
        const pbw_descriptor *type = pbw_schema_find(schema, names[n]);
        size_t i;
        uint32_t previous = 0;

        CHECK(type != NULL);
        if (type == NULL) {
            continue;
        }
        CHECK(pbw_descriptor_field_count(type) > 0);
        for (i = 0; i < pbw_descriptor_field_count(type); i++) {
            const pbw_field *field = pbw_descriptor_field_at(type, i);

            CHECK(field != NULL);
            if (field == NULL) {
                continue;
            }
            /* sorted by number, and every lookup table agrees with the array */
            CHECK(pbw_field_number(field) > previous);
            previous = pbw_field_number(field);
            CHECK(pbw_descriptor_find_field(type, pbw_field_number(field)) ==
                  field);
            CHECK(pbw_descriptor_find_field_name(type, pbw_field_name(field)) ==
                  field);
        }
        CHECK(pbw_descriptor_field_at(type, pbw_descriptor_field_count(type)) ==
              NULL);
        CHECK(pbw_descriptor_find_field(type, 0) == NULL);
        CHECK(pbw_descriptor_find_field(type, 536870911u) == NULL);
        CHECK(pbw_descriptor_find_field_name(type, "no_such_field") == NULL);
        CHECK(pbw_descriptor_find_field_name(type, "") == NULL);
    }
    /* an enum is not a message: no field lookups, no decoding */
    CHECK(pbw_descriptor_find_field(pbw_schema_find(schema, "pbwire.test.Color"),
                                    1) == NULL);
    CHECK(pbw_message_new(pbw_schema_find(schema, "pbwire.test.Color")) == NULL);
    pbw_schema_free(schema);
}

/* The set itself is a protobuf message: unknown fields in it are skipped, and
 * anything malformed is reported instead of crashing. */
static void
test_load_tolerance(void)
{
    /* FileDescriptorSet has one declared field (1 = file); field 999 is an
     * unknown varint, which the loader must skip. */
    static const unsigned char trailing[] = {0xf8, 0x3e, 0x05};
    static const unsigned char bad_group[] = {0x0b, 0x0c};
    static const unsigned char bad_wire_type[] = {0x0e};
    unsigned char *buf = (unsigned char *)malloc(sizeof(desc_set) + 8);
    pbw_schema *schema = NULL;

    CHECK(buf != NULL);
    if (buf == NULL) {
        return;
    }
    memcpy(buf, desc_set, sizeof(desc_set));
    memcpy(buf + sizeof(desc_set), trailing, sizeof(trailing));
    CHECK_STATUS(PBW_OK, pbw_descriptor_set_load(buf, sizeof(desc_set) + 3,
                                                 &schema));
    CHECK(schema != NULL &&
          pbw_schema_find(schema, "pbwire.test.Outer") != NULL);
    CHECK(pbw_schema_error(schema)[0] == '\0');
    pbw_schema_free(schema);

    /* a group or an invalid wire type inside the set is malformed */
    schema = NULL;
    memcpy(buf, bad_group, sizeof(bad_group));
    CHECK_STATUS(PBW_ERR_INVALID,
                 pbw_descriptor_set_load(buf, sizeof(bad_group), &schema));
    CHECK(schema != NULL && pbw_schema_error(schema)[0] != '\0');
    pbw_schema_free(schema);

    schema = NULL;
    memcpy(buf, bad_wire_type, sizeof(bad_wire_type));
    CHECK_STATUS(PBW_ERR_INVALID,
                 pbw_descriptor_set_load(buf, sizeof(bad_wire_type), &schema));
    CHECK(schema != NULL && pbw_schema_error(schema)[0] != '\0');
    pbw_schema_free(schema);

    /* a set that declares nothing is a valid, empty schema */
    schema = NULL;
    memcpy(buf, desc_set, sizeof(desc_set));
    CHECK_STATUS(PBW_OK, pbw_descriptor_set_load(buf, 0, &schema));
    CHECK(schema != NULL && pbw_schema_find(schema, "pbwire.test.Outer") == NULL);
    pbw_schema_free(schema);
    free(buf);
}

/* Descriptor bytes are untrusted input: a mutated or truncated set must produce
 * a status, never a crash. 2000 deterministic mutations, no RNG dependency. */
static void
test_mutation_sweep(void)
{
    unsigned char buf[sizeof(desc_set)];
    unsigned state = 20260219u;
    int i;

    for (i = 0; i < 2000; i++) {
        pbw_schema *schema = NULL;
        pbw_status status;
        size_t available;

        memcpy(buf, desc_set, sizeof(desc_set));
        state = state * 1103515245u + 12345u;
        buf[state % sizeof(desc_set)] ^= (unsigned char)(1u << ((state >> 16) & 7u));
        available = sizeof(desc_set) - (size_t)(state >> 20) % 16;
        status = pbw_descriptor_set_load(buf, available, &schema);
        /* *out is always written for a non-NULL input, and a failure explains
         * itself through pbw_schema_error(). */
        CHECK(schema != NULL);
        if (status != PBW_OK) {
            CHECK(pbw_schema_error(schema)[0] != '\0');
        }
        pbw_schema_free(schema);
    }
}

/* The dialect comes from each file's `syntax`, so a proto2 message loaded from
 * a set has closed enums and a required-field check. */
static void
test_proto2_dialect(void)
{
    pbw_schema *schema = NULL;
    const pbw_descriptor *old_msg;
    pbw_message *decoded = NULL;
    uint8_t *buf = NULL;
    size_t len = 0;
    /* id = 1, kind = 99 (not a Legacy value) */
    static const uint8_t unknown_enum[] = {0x08, 0x01, 0x20, 0x63};
    /* id = 1, kind = LEGACY_NEG (-2) */
    static const uint8_t negative_enum[] = {0x08, 0x01, 0x20, 0xfe, 0xff, 0xff,
                                            0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                            0x01};
    /* kind without the required id */
    static const uint8_t missing_id[] = {0x20, 0x01};

    CHECK_OK(pbw_descriptor_set_load(desc_set, sizeof(desc_set), &schema));
    if (schema == NULL) {
        return;
    }
    old_msg = pbw_schema_find(schema, "pbwire.test.Old");
    CHECK(old_msg != NULL);
    if (old_msg == NULL) {
        pbw_schema_free(schema);
        return;
    }

    /* an unknown closed-enum number leaves the field ... */
    CHECK_OK(pbw_decode(old_msg, unknown_enum, sizeof(unknown_enum), &decoded));
    CHECK(decoded != NULL);
    if (decoded != NULL) {
        CHECK(pbw_message_get(decoded, 4) == NULL);
        CHECK(pbw_value_get_int32(pbw_message_get(decoded, 1)) == 1);
        /* ... and is written back byte for byte */
        CHECK_OK(pbw_encode(decoded, &buf, &len));
        CHECK(len == sizeof(unknown_enum) &&
              memcmp(buf, unknown_enum, len) == 0);
        pbw_free(buf);
        buf = NULL;
        pbw_message_free(decoded);
        decoded = NULL;
    }

    /* a declared value, negative included, stays in the field */
    CHECK_OK(pbw_decode(old_msg, negative_enum, sizeof(negative_enum), &decoded));
    CHECK(decoded != NULL);
    if (decoded != NULL) {
        CHECK(pbw_value_get_int32(pbw_message_get(decoded, 4)) == -2);
        CHECK_OK(pbw_encode(decoded, &buf, &len));
        CHECK(len == sizeof(negative_enum) &&
              memcmp(buf, negative_enum, len) == 0);
        pbw_free(buf);
        buf = NULL;
        pbw_message_free(decoded);
        decoded = NULL;
    }

    /* the required field is enforced, and only when it is missing */
    CHECK_STATUS(PBW_ERR_REQUIRED,
                 pbw_decode(old_msg, missing_id, sizeof(missing_id), &decoded));
    CHECK(decoded == NULL);
    CHECK_OK(pbw_decode(old_msg, unknown_enum, sizeof(unknown_enum), &decoded));
    CHECK(decoded != NULL && pbw_message_is_initialized(decoded) == 1);
    pbw_message_free(decoded);
    decoded = NULL;

    /* extensions attach to their extendee and decode like regular fields */
    {
        const pbw_field *ext_int =
            pbw_descriptor_find_field(old_msg, 100);
        const pbw_field *ext_str =
            pbw_descriptor_find_field_name(old_msg, "ext_str");
        /* id = 1, ext_int = 42, ext_str = "hi" */
        static const uint8_t with_ext[] = {0x08, 0x01, 0xa0, 0x06, 0x2a,
                                           0xaa, 0x06, 0x02, 'h', 'i'};
        pbw_message *msg;

        CHECK(ext_int != NULL && pbw_field_is_extension(ext_int));
        CHECK(ext_str != NULL && pbw_field_is_extension(ext_str));
        CHECK(ext_int != NULL &&
              strcmp(pbw_field_default_value(ext_int), "5") == 0);
        /* regular fields are not extensions; most have no default */
        CHECK(!pbw_field_is_extension(
            pbw_descriptor_find_field(old_msg, 1)));
        CHECK(pbw_field_default_value(
                  pbw_descriptor_find_field(old_msg, 1)) == NULL);
        CHECK(strcmp(pbw_field_default_value(
                         pbw_descriptor_find_field(old_msg, 2)),
                     "x") == 0);
        CHECK_OK(pbw_decode(old_msg, with_ext, sizeof(with_ext), &decoded));
        CHECK(decoded != NULL);
        if (decoded != NULL) {
            CHECK(pbw_value_get_int32(pbw_message_get(decoded, 100)) == 42);
            CHECK(strcmp(pbw_value_get_string(
                             pbw_message_get(decoded, 101)),
                         "hi") == 0);
            CHECK_OK(pbw_encode(decoded, &buf, &len));
            CHECK(len == sizeof(with_ext) &&
                  memcmp(buf, with_ext, len) == 0);
            pbw_free(buf);
            buf = NULL;
            pbw_message_free(decoded);
            decoded = NULL;
        }
        /* an absent extension stays absent: the default is reported,
         * not materialized */
        CHECK_OK(pbw_decode(old_msg, unknown_enum, sizeof(unknown_enum),
                            &decoded));
        CHECK(decoded != NULL && pbw_message_get(decoded, 100) == NULL);
        if (decoded != NULL) {
            CHECK_OK(pbw_encode(decoded, &buf, &len));
            CHECK(len == sizeof(unknown_enum) &&
                  memcmp(buf, unknown_enum, len) == 0);
            pbw_free(buf);
            buf = NULL;
            pbw_message_free(decoded);
            decoded = NULL;
        }
        /* set through the API, encoded with the extension number */
        msg = pbw_message_new(old_msg);
        CHECK(msg != NULL);
        if (msg != NULL) {
            CHECK_OK(pbw_message_set(msg, 1, pbw_value_new_int(1)));
            CHECK_OK(pbw_message_set(msg, 100, pbw_value_new_int(42)));
            CHECK_OK(pbw_encode(msg, &buf, &len));
            CHECK(len == 5 && buf[0] == 0x08 && buf[1] == 0x01 &&
                  buf[2] == 0xa0 && buf[3] == 0x06 && buf[4] == 0x2a);
            pbw_free(buf);
            buf = NULL;
            pbw_message_free(msg);
        }
    }
    pbw_schema_free(schema);
}

/* Text format through a loaded descriptor set: full-name extensions, reserved
 * names skipped, Any round trip. */
static void
test_text_loader(void)
{
    pbw_schema *schema = NULL;
    const pbw_descriptor *old_msg;
    pbw_message *decoded = NULL;
    char *out = NULL;
    size_t len = 0;
    static const char text[] =
        "id: 1 [pbwire.test.ext_int]: 42 ancient: 7\n"
        "payload { [type.googleapis.com/pbwire.test.Old] { id: 2 } }";
    static const char want[] =
        "id: 1\n"
        "payload {\n"
        "  type_url: \"type.googleapis.com/pbwire.test.Old\"\n"
        "  value: \"\\010\\002\"\n"
        "}\n"
        "[pbwire.test.ext_int]: 42\n";

    CHECK_STATUS(PBW_OK,
                 pbw_descriptor_set_load(desc_set, sizeof(desc_set), &schema));
    if (schema == NULL) {
        return;
    }
    old_msg = pbw_schema_find(schema, "pbwire.test.Old");
    CHECK(old_msg != NULL);
    if (old_msg == NULL) {
        pbw_schema_free(schema);
        return;
    }
    /* extension by full name parses; the reserved name vanishes silently */
    CHECK_OK(pbw_text_parse(old_msg, text, sizeof(text) - 1, &decoded));
    CHECK(decoded != NULL);
    if (decoded != NULL) {
        pbw_message *payload =
            pbw_value_get_message(pbw_message_get(decoded, 5));

        CHECK(pbw_value_get_int32(pbw_message_get(decoded, 100)) == 42);
        CHECK(payload != NULL);
        CHECK(strcmp(pbw_value_get_string(pbw_message_get(payload, 1)),
                     "type.googleapis.com/pbwire.test.Old") == 0);
        CHECK_OK(pbw_text_print(decoded, &out, &len));
        CHECK(len == sizeof(want) - 1 && memcmp(out, want, len) == 0);
        pbw_free(out);
        out = NULL;
        /* print(parse(print)) is stable */
        CHECK_OK(pbw_text_print(decoded, &out, &len));
        {
            pbw_message *again = NULL;

            CHECK_OK(pbw_text_parse(old_msg, out, len, &again));
            pbw_free(out);
            out = NULL;
            CHECK(again != NULL);
            pbw_message_free(again);
        }
        pbw_message_free(decoded);
        decoded = NULL;
    }
    /* a short extension name is not a field; an unknown name fails */
    CHECK_STATUS(PBW_ERR_INVALID,
                 pbw_text_parse(old_msg, "ext_int: 1", 10, &decoded));
    CHECK(decoded == NULL);
    /* malformed type URLs fail: no slash, leading slash, bad escape */
    {
        static const char *badurl[] = {
            "id: 1 payload [foo] { id: 1 }",
            "id: 1 payload [/pbwire.test.Old] { id: 1 }",
            "id: 1 payload [type%2/foo] { id: 1 }",
            "id: 1 payload [type%2Zapis.com/Old] { id: 1 }",
        };
        size_t k;

        for (k = 0; k < sizeof(badurl) / sizeof(badurl[0]); k++) {
            CHECK_STATUS(PBW_ERR_INVALID,
                         pbw_text_parse(old_msg, badurl[k],
                                        strlen(badurl[k]), &decoded));
            CHECK(decoded == NULL);
        }
    }
    CHECK_STATUS(PBW_ERR_INVALID,
                 pbw_text_parse(old_msg, "id: 1 nope: 2",
                                strlen("id: 1 nope: 2"), &decoded));
    CHECK(decoded == NULL);
    /* an unresolvable Any type fails */
    CHECK_STATUS(PBW_ERR_INVALID,
                 pbw_text_parse(old_msg,
                                "id: 1 payload { [type.googleapis.com/Nope]"
                                " { id: 1 } }",
                                strlen("id: 1 payload { "
                                       "[type.googleapis.com/Nope]"
                                       " { id: 1 } }"),
                                &decoded));
    CHECK(decoded == NULL);
    /* a broken inner message fails the expansion */
    CHECK_STATUS(PBW_ERR_INVALID,
                 pbw_text_parse(old_msg,
                                "id: 1 payload { "
                                "[type.googleapis.com/pbwire.test.Old]"
                                " { id: } }",
                                strlen("id: 1 payload { "
                                       "[type.googleapis.com/pbwire.test.Old]"
                                       " { id: } }"),
                                &decoded));
    CHECK(decoded == NULL);
    /* deeply nested lists under a reserved name stop at the depth cap
     * instead of recursing on the C stack; shallow ones still skip. */
    {
        char deep[1024];
        size_t at = 0;
        size_t i;
        const char head[] = "id: 1 ancient: ";

        memcpy(deep + at, head, sizeof(head) - 1);
        at += sizeof(head) - 1;
        for (i = 0; i < 300; i++) {
            deep[at++] = '[';
        }
        deep[at++] = '0';
        for (i = 0; i < 300; i++) {
            deep[at++] = ']';
        }
        deep[at] = '\0';
        CHECK_STATUS(PBW_ERR_LIMIT,
                     pbw_text_parse(old_msg, deep, at, &decoded));
        CHECK(decoded == NULL);
        CHECK_OK(pbw_text_parse(old_msg, "id: 1 ancient: [[0]]",
                                strlen("id: 1 ancient: [[0]]"), &decoded));
        pbw_message_free(decoded);
        decoded = NULL;
        /* reserved names skip every value shape: messages (both
         * brackets), lists, strings with escapes and bare scalars */
        CHECK_OK(pbw_text_parse(
            old_msg,
            "id: 1 ancient { id: 2 payload { } } "
            "ancient < id: 3 > ancient: [1, \"x\\n\", { id: 4 }] "
            "ancient: \"s\" ancient: 5",
            strlen(
                "id: 1 ancient { id: 2 payload { } } "
                "ancient < id: 3 > ancient: [1, \"x\\n\", { id: 4 }] "
                "ancient: \"s\" ancient: 5"),
            &decoded));
        CHECK(pbw_value_get_int32(pbw_message_get(decoded, 1)) == 1);
        pbw_message_free(decoded);
        decoded = NULL;
        /* every skipped value shape, valid and truncated */
        {
            static const char *skip_ok[] = {
                "id: 1 ancient { [foo.bar]: 1 }",
                "id: 1 ancient { [foo.bar] { x: 1 } }",
                "id: 1 ancient < id: { x: 1 } >",
                "id: 1 ancient: \"a\\n\" \"b\"",
                "id: 1 ancient: true",
                "id: 1 ancient: -1.5",
                "id: 1 ancient: 1e3",
                "id: 1 ancient: inf",
                "id: 1 ancient: [1, \"x\", { id: 1 }, [2]]",
                "id: 1 ancient: { id: 1, name: \"x\"; vals: [1] }",
                "id: 1 ancient: {}",
                "id: 1 ancient: []",
                ("id: 1 ancient { "
                 "[type.googleapis.com/foo]: { a: 1 } }"),
            };
            static const char *skip_bad[] = {
                "id: 1 ancient \"abc",
                "id: 1 ancient { id: 1",
                "id: 1 ancient < id: 2",
                "id: 1 ancient [1,",
                "id: 1 ancient { [unclosed: 1 }",
                "id: 1 ancient { id }",
                "id: 1 ancient { id: }",
            };
            size_t k;

            for (k = 0; k < sizeof(skip_ok) / sizeof(skip_ok[0]); k++) {
                CHECK_OK(pbw_text_parse(old_msg, skip_ok[k],
                                        strlen(skip_ok[k]), &decoded));
                if (decoded != NULL) {
                    CHECK(pbw_value_get_int32(
                              pbw_message_get(decoded, 1)) == 1);
                    pbw_message_free(decoded);
                    decoded = NULL;
                }
            }
            for (k = 0; k < sizeof(skip_bad) / sizeof(skip_bad[0]); k++) {
                CHECK_STATUS(PBW_ERR_INVALID,
                             pbw_text_parse(old_msg, skip_bad[k],
                                            strlen(skip_bad[k]), &decoded));
                CHECK(decoded == NULL);
            }
        }
        {
            static const char *trunc[] = {
                "id: 1 ancient: [1,",
                "id: 1 ancient { id:",
                "id: 1 ancient: \"abc",
                "id: 1 ancient:",
                "id: 1 ancient",
            };
            size_t k;

            for (k = 0; k < sizeof(trunc) / sizeof(trunc[0]); k++) {
                CHECK_STATUS(PBW_ERR_INVALID,
                             pbw_text_parse(old_msg, trunc[k],
                                            strlen(trunc[k]), &decoded));
                CHECK(decoded == NULL);
            }
        }
    }
    pbw_schema_free(schema);
}

static void
test_json_loader(void)
{
    pbw_schema *schema = NULL;
    const pbw_descriptor *old_msg;
    pbw_message *decoded = NULL;
    pbw_json_parse_options ignore = {1};
    char *out = NULL;
    size_t len = 0;
    static const char input[] =
        "{\"id\":1,\"kind\":\"LEGACY_NEG\","
        "\"payload\":{\"@type\":\"type.googleapis.com/"
        "pbwire.test.Old\",\"id\":2},"
        "\"[pbwire.test.ext_int]\":42}";
    /* known Any types print expanded, like the reference */
    static const char want[] =
        "{\"id\":1,\"kind\":\"LEGACY_NEG\","
        "\"payload\":{\"@type\":\"type.googleapis.com/"
        "pbwire.test.Old\",\"id\":2},"
        "\"[pbwire.test.ext_int]\":42}";

    CHECK_STATUS(PBW_OK,
                 pbw_descriptor_set_load(desc_set, sizeof(desc_set), &schema));
    if (schema == NULL) {
        return;
    }
    old_msg = pbw_schema_find(schema, "pbwire.test.Old");
    CHECK(old_msg != NULL);
    if (old_msg == NULL) {
        pbw_schema_free(schema);
        return;
    }
    /* extension by full name parses; the embedded message prints raw */
    CHECK_OK(pbw_json_parse(old_msg, input, sizeof(input) - 1, &decoded));
    CHECK(decoded != NULL);
    if (decoded != NULL) {
        pbw_message *payload =
            pbw_value_get_message(pbw_message_get(decoded, 5));

        CHECK(pbw_value_get_int32(pbw_message_get(decoded, 100)) == 42);
        CHECK(payload != NULL);
        CHECK(strcmp(pbw_value_get_string(pbw_message_get(payload, 1)),
                     "type.googleapis.com/pbwire.test.Old") == 0);
        CHECK_OK(pbw_json_print(decoded, &out, &len));
        CHECK(len == sizeof(want) - 1 && memcmp(out, want, len) == 0);
        pbw_free(out);
        out = NULL;
        /* print(parse(print)) is stable */
        CHECK_OK(pbw_json_print(decoded, &out, &len));
        {
            pbw_message *again = NULL;

            CHECK_OK(pbw_json_parse(old_msg, out, len, &again));
            pbw_free(out);
            out = NULL;
            CHECK(again != NULL);
            pbw_message_free(again);
        }
        pbw_message_free(decoded);
        decoded = NULL;
    }
    /* unknown fields fail by default but pass when ignored */
    CHECK_STATUS(PBW_ERR_INVALID,
                 pbw_json_parse(old_msg, "{\"id\":1,\"nope\":2}",
                                strlen("{\"id\":1,\"nope\":2}"),
                                &decoded));
    CHECK(decoded == NULL);
    CHECK_OK(pbw_json_parse_opts(old_msg, "{\"id\":1,\"nope\":2}",
                                 strlen("{\"id\":1,\"nope\":2}"),
                                 &ignore, &decoded));
    pbw_message_free(decoded);
    decoded = NULL;
    /* an unresolvable Any type fails */
    CHECK_STATUS(PBW_ERR_INVALID,
                 pbw_json_parse(old_msg,
                                "{\"id\":1,\"payload\":{\"@type\":\"type."
                                "googleapis.com/Nope\"}}",
                                strlen("{\"id\":1,\"payload\":{\"@type\":\"type."
                                       "googleapis.com/Nope\"}}"),
                                &decoded));
    CHECK(decoded == NULL);
    /* required fields are enforced */
    CHECK_STATUS(PBW_ERR_REQUIRED,
                 pbw_json_parse(old_msg, "{\"name\":\"x\"}",
                                strlen("{\"name\":\"x\"}"), &decoded));
    CHECK(decoded == NULL);
    pbw_schema_free(schema);
}

int
main(void)
{
    test_schema();
    test_wire();
    test_proto2_dialect();
    test_text_loader();
    test_json_loader();
    test_errors();
    test_independent_loads();
    test_lookup_tables();
    test_load_tolerance();
    test_mutation_sweep();
    test_services();
    test_load_hostile();
    test_service_resolve();
    test_load_limits();
    test_duplicate_field_number();
    test_services_protoc();
    test_editions();
    test_messageset();

    if (failures != 0) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("test_descriptor: all checks passed\n");
    return 0;
}

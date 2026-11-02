/* Descriptor set loading self-check: a FileDescriptorSet produced by protoc is
 * turned into a schema, then used to encode and decode. The bytes below are
 * pinned, so the loader is covered without protoc at build time.
 *
 * Regenerate with `make desc-fixture` (protoc 36.2) from
 * tests/data/desc_proto3.proto, which imports tests/data/desc_proto2.proto.
 */

#include "pbwire.h"

#include <stdio.h>
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

/* FileDescriptorSet from protoc, see the comment at the top. */
static const unsigned char desc_set[] = {
    0x0a,
    0xb9,
    0x02,
    0x0a,
    0x11,
    0x64,
    0x65,
    0x73,
    0x63,
    0x5f,
    0x70,
    0x72,
    0x6f,
    0x74,
    0x6f,
    0x32,
    0x2e,
    0x70,
    0x72,
    0x6f,
    0x74,
    0x6f,
    0x12,
    0x0b,
    0x70,
    0x62,
    0x77,
    0x69,
    0x72,
    0x65,
    0x2e,
    0x74,
    0x65,
    0x73,
    0x74,
    0x22,
    0x69,
    0x0a,
    0x03,
    0x4f,
    0x6c,
    0x64,
    0x12,
    0x0e,
    0x0a,
    0x02,
    0x69,
    0x64,
    0x18,
    0x01,
    0x20,
    0x02,
    0x28,
    0x05,
    0x52,
    0x02,
    0x69,
    0x64,
    0x12,
    0x15,
    0x0a,
    0x04,
    0x6e,
    0x61,
    0x6d,
    0x65,
    0x18,
    0x02,
    0x20,
    0x01,
    0x28,
    0x09,
    0x3a,
    0x01,
    0x78,
    0x52,
    0x04,
    0x6e,
    0x61,
    0x6d,
    0x65,
    0x12,
    0x12,
    0x0a,
    0x04,
    0x76,
    0x61,
    0x6c,
    0x73,
    0x18,
    0x03,
    0x20,
    0x03,
    0x28,
    0x12,
    0x52,
    0x04,
    0x76,
    0x61,
    0x6c,
    0x73,
    0x12,
    0x27,
    0x0a,
    0x04,
    0x6b,
    0x69,
    0x6e,
    0x64,
    0x18,
    0x04,
    0x20,
    0x01,
    0x28,
    0x0e,
    0x32,
    0x13,
    0x2e,
    0x70,
    0x62,
    0x77,
    0x69,
    0x72,
    0x65,
    0x2e,
    0x74,
    0x65,
    0x73,
    0x74,
    0x2e,
    0x4c,
    0x65,
    0x67,
    0x61,
    0x63,
    0x79,
    0x52,
    0x04,
    0x6b,
    0x69,
    0x6e,
    0x64,
    0x22,
    0x68,
    0x0a,
    0x07,
    0x47,
    0x72,
    0x6f,
    0x75,
    0x70,
    0x65,
    0x64,
    0x12,
    0x30,
    0x0a,
    0x05,
    0x69,
    0x6e,
    0x6e,
    0x65,
    0x72,
    0x18,
    0x01,
    0x20,
    0x01,
    0x28,
    0x0a,
    0x32,
    0x1a,
    0x2e,
    0x70,
    0x62,
    0x77,
    0x69,
    0x72,
    0x65,
    0x2e,
    0x74,
    0x65,
    0x73,
    0x74,
    0x2e,
    0x47,
    0x72,
    0x6f,
    0x75,
    0x70,
    0x65,
    0x64,
    0x2e,
    0x49,
    0x6e,
    0x6e,
    0x65,
    0x72,
    0x52,
    0x05,
    0x69,
    0x6e,
    0x6e,
    0x65,
    0x72,
    0x12,
    0x14,
    0x0a,
    0x05,
    0x61,
    0x66,
    0x74,
    0x65,
    0x72,
    0x18,
    0x03,
    0x20,
    0x01,
    0x28,
    0x05,
    0x52,
    0x05,
    0x61,
    0x66,
    0x74,
    0x65,
    0x72,
    0x1a,
    0x15,
    0x0a,
    0x05,
    0x49,
    0x6e,
    0x6e,
    0x65,
    0x72,
    0x12,
    0x0c,
    0x0a,
    0x01,
    0x78,
    0x18,
    0x02,
    0x20,
    0x01,
    0x28,
    0x05,
    0x52,
    0x01,
    0x78,
    0x2a,
    0x42,
    0x0a,
    0x06,
    0x4c,
    0x65,
    0x67,
    0x61,
    0x63,
    0x79,
    0x12,
    0x0f,
    0x0a,
    0x0b,
    0x4c,
    0x45,
    0x47,
    0x41,
    0x43,
    0x59,
    0x5f,
    0x4e,
    0x4f,
    0x4e,
    0x45,
    0x10,
    0x00,
    0x12,
    0x0e,
    0x0a,
    0x0a,
    0x4c,
    0x45,
    0x47,
    0x41,
    0x43,
    0x59,
    0x5f,
    0x4f,
    0x4e,
    0x45,
    0x10,
    0x01,
    0x12,
    0x17,
    0x0a,
    0x0a,
    0x4c,
    0x45,
    0x47,
    0x41,
    0x43,
    0x59,
    0x5f,
    0x4e,
    0x45,
    0x47,
    0x10,
    0xfe,
    0xff,
    0xff,
    0xff,
    0xff,
    0xff,
    0xff,
    0xff,
    0xff,
    0x01,
    0x0a,
    0xfb,
    0x03,
    0x0a,
    0x11,
    0x64,
    0x65,
    0x73,
    0x63,
    0x5f,
    0x70,
    0x72,
    0x6f,
    0x74,
    0x6f,
    0x33,
    0x2e,
    0x70,
    0x72,
    0x6f,
    0x74,
    0x6f,
    0x12,
    0x0b,
    0x70,
    0x62,
    0x77,
    0x69,
    0x72,
    0x65,
    0x2e,
    0x74,
    0x65,
    0x73,
    0x74,
    0x1a,
    0x11,
    0x64,
    0x65,
    0x73,
    0x63,
    0x5f,
    0x70,
    0x72,
    0x6f,
    0x74,
    0x6f,
    0x32,
    0x2e,
    0x70,
    0x72,
    0x6f,
    0x74,
    0x6f,
    0x22,
    0x8e,
    0x03,
    0x0a,
    0x05,
    0x4f,
    0x75,
    0x74,
    0x65,
    0x72,
    0x12,
    0x14,
    0x0a,
    0x05,
    0x70,
    0x6c,
    0x61,
    0x69,
    0x6e,
    0x18,
    0x01,
    0x20,
    0x01,
    0x28,
    0x05,
    0x52,
    0x05,
    0x70,
    0x6c,
    0x61,
    0x69,
    0x6e,
    0x12,
    0x1f,
    0x0a,
    0x0b,
    0x61,
    0x75,
    0x74,
    0x6f,
    0x5f,
    0x70,
    0x61,
    0x63,
    0x6b,
    0x65,
    0x64,
    0x18,
    0x02,
    0x20,
    0x03,
    0x28,
    0x05,
    0x52,
    0x0a,
    0x61,
    0x75,
    0x74,
    0x6f,
    0x50,
    0x61,
    0x63,
    0x6b,
    0x65,
    0x64,
    0x12,
    0x1e,
    0x0a,
    0x08,
    0x75,
    0x6e,
    0x70,
    0x61,
    0x63,
    0x6b,
    0x65,
    0x64,
    0x18,
    0x03,
    0x20,
    0x03,
    0x28,
    0x11,
    0x42,
    0x02,
    0x10,
    0x00,
    0x52,
    0x08,
    0x75,
    0x6e,
    0x70,
    0x61,
    0x63,
    0x6b,
    0x65,
    0x64,
    0x12,
    0x2e,
    0x0a,
    0x05,
    0x69,
    0x6e,
    0x6e,
    0x65,
    0x72,
    0x18,
    0x04,
    0x20,
    0x01,
    0x28,
    0x0b,
    0x32,
    0x18,
    0x2e,
    0x70,
    0x62,
    0x77,
    0x69,
    0x72,
    0x65,
    0x2e,
    0x74,
    0x65,
    0x73,
    0x74,
    0x2e,
    0x4f,
    0x75,
    0x74,
    0x65,
    0x72,
    0x2e,
    0x49,
    0x6e,
    0x6e,
    0x65,
    0x72,
    0x52,
    0x05,
    0x69,
    0x6e,
    0x6e,
    0x65,
    0x72,
    0x12,
    0x30,
    0x0a,
    0x04,
    0x74,
    0x61,
    0x67,
    0x73,
    0x18,
    0x05,
    0x20,
    0x03,
    0x28,
    0x0b,
    0x32,
    0x1c,
    0x2e,
    0x70,
    0x62,
    0x77,
    0x69,
    0x72,
    0x65,
    0x2e,
    0x74,
    0x65,
    0x73,
    0x74,
    0x2e,
    0x4f,
    0x75,
    0x74,
    0x65,
    0x72,
    0x2e,
    0x54,
    0x61,
    0x67,
    0x73,
    0x45,
    0x6e,
    0x74,
    0x72,
    0x79,
    0x52,
    0x04,
    0x74,
    0x61,
    0x67,
    0x73,
    0x12,
    0x0e,
    0x0a,
    0x01,
    0x61,
    0x18,
    0x06,
    0x20,
    0x01,
    0x28,
    0x05,
    0x48,
    0x00,
    0x52,
    0x01,
    0x61,
    0x12,
    0x0e,
    0x0a,
    0x01,
    0x62,
    0x18,
    0x07,
    0x20,
    0x01,
    0x28,
    0x09,
    0x48,
    0x00,
    0x52,
    0x01,
    0x62,
    0x12,
    0x28,
    0x0a,
    0x06,
    0x6c,
    0x65,
    0x67,
    0x61,
    0x63,
    0x79,
    0x18,
    0x08,
    0x20,
    0x01,
    0x28,
    0x0b,
    0x32,
    0x10,
    0x2e,
    0x70,
    0x62,
    0x77,
    0x69,
    0x72,
    0x65,
    0x2e,
    0x74,
    0x65,
    0x73,
    0x74,
    0x2e,
    0x4f,
    0x6c,
    0x64,
    0x52,
    0x06,
    0x6c,
    0x65,
    0x67,
    0x61,
    0x63,
    0x79,
    0x12,
    0x28,
    0x0a,
    0x05,
    0x63,
    0x6f,
    0x6c,
    0x6f,
    0x72,
    0x18,
    0x09,
    0x20,
    0x01,
    0x28,
    0x0e,
    0x32,
    0x12,
    0x2e,
    0x70,
    0x62,
    0x77,
    0x69,
    0x72,
    0x65,
    0x2e,
    0x74,
    0x65,
    0x73,
    0x74,
    0x2e,
    0x43,
    0x6f,
    0x6c,
    0x6f,
    0x72,
    0x52,
    0x05,
    0x63,
    0x6f,
    0x6c,
    0x6f,
    0x72,
    0x1a,
    0x15,
    0x0a,
    0x05,
    0x49,
    0x6e,
    0x6e,
    0x65,
    0x72,
    0x12,
    0x0c,
    0x0a,
    0x01,
    0x73,
    0x18,
    0x01,
    0x20,
    0x01,
    0x28,
    0x09,
    0x52,
    0x01,
    0x73,
    0x1a,
    0x37,
    0x0a,
    0x09,
    0x54,
    0x61,
    0x67,
    0x73,
    0x45,
    0x6e,
    0x74,
    0x72,
    0x79,
    0x12,
    0x10,
    0x0a,
    0x03,
    0x6b,
    0x65,
    0x79,
    0x18,
    0x01,
    0x20,
    0x01,
    0x28,
    0x09,
    0x52,
    0x03,
    0x6b,
    0x65,
    0x79,
    0x12,
    0x14,
    0x0a,
    0x05,
    0x76,
    0x61,
    0x6c,
    0x75,
    0x65,
    0x18,
    0x02,
    0x20,
    0x01,
    0x28,
    0x05,
    0x52,
    0x05,
    0x76,
    0x61,
    0x6c,
    0x75,
    0x65,
    0x3a,
    0x02,
    0x38,
    0x01,
    0x42,
    0x08,
    0x0a,
    0x06,
    0x63,
    0x68,
    0x6f,
    0x69,
    0x63,
    0x65,
    0x2a,
    0x2d,
    0x0a,
    0x05,
    0x43,
    0x6f,
    0x6c,
    0x6f,
    0x72,
    0x12,
    0x15,
    0x0a,
    0x11,
    0x43,
    0x4f,
    0x4c,
    0x4f,
    0x52,
    0x5f,
    0x55,
    0x4e,
    0x53,
    0x50,
    0x45,
    0x43,
    0x49,
    0x46,
    0x49,
    0x45,
    0x44,
    0x10,
    0x00,
    0x12,
    0x0d,
    0x0a,
    0x09,
    0x43,
    0x4f,
    0x4c,
    0x4f,
    0x52,
    0x5f,
    0x52,
    0x45,
    0x44,
    0x10,
    0x01,
    0x62,
    0x06,
    0x70,
    0x72,
    0x6f,
    0x74,
    0x6f,
    0x33,
};

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

    /* a legacy group field is dropped by the loader instead of failing the
     * whole set: the message still loads, and the group's number behaves like
     * an unknown field (kept on decode, written back unchanged) */
    {
        const pbw_descriptor *grouped =
            pbw_schema_find(schema, "pbwire.test.Grouped");
        static const uint8_t wire[] = {0x0b, 0x10, 0x07, 0x0c, 0x18, 0x09};
        static const uint8_t expected[] = {0x18, 0x09, 0x0b, 0x10, 0x07, 0x0c};
        pbw_message *decoded = NULL;
        uint8_t *buf = NULL;
        size_t len = 0;

        CHECK(grouped != NULL);
        CHECK(pbw_descriptor_field_count(grouped) == 1);
        CHECK(pbw_descriptor_find_field(grouped, 1) == NULL);
        CHECK(pbw_descriptor_find_field(grouped, 3) != NULL);
        CHECK_OK(pbw_decode(grouped, wire, sizeof(wire), &decoded));
        CHECK(pbw_value_get_int32(pbw_message_get(decoded, 3)) == 9);
        CHECK(pbw_message_get(decoded, 1) == NULL);
        CHECK_OK(pbw_encode(decoded, &buf, &len));
        CHECK(len == sizeof(expected) &&
              memcmp(buf, expected, sizeof(expected)) == 0);
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
        CHECK(status != PBW_OK || schema != NULL);
        pbw_schema_free(schema);
    }
}

int
main(void)
{
    test_schema();
    test_wire();
    test_errors();
    test_mutation_sweep();

    if (failures != 0) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("test_descriptor: all checks passed\n");
    return 0;
}

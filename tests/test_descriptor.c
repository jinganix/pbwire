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

    if (failures != 0) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("test_descriptor: all checks passed\n");
    return 0;
}

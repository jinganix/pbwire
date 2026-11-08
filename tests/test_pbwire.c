/* Assert-based self-check for the pbwire runtime: no test framework, no
 * dependencies. */

#include "pbwire.h"
#include "internal.h"

#include <locale.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

/* Nesting limit shared by messages and legacy groups. */
#define PBW_GROUP_DEPTH 300

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

struct field_def {
    const char *name;
    uint32_t number;
    pbw_type type;
    pbw_label label;
};

static void
add_fields(pbw_descriptor *desc, const struct field_def *defs,
           size_t count)
{
    size_t i;

    for (i = 0; i < count; i++) {
        CHECK(pbw_message_add_field(desc, defs[i].name, defs[i].number,
                                    defs[i].type, defs[i].label) != NULL);
    }
}

static const struct field_def SCALAR_FIELDS[] = {
    {"i32", 1, PBW_TYPE_INT32, PBW_LABEL_OPTIONAL},
    {"i64", 2, PBW_TYPE_INT64, PBW_LABEL_OPTIONAL},
    {"u32", 3, PBW_TYPE_UINT32, PBW_LABEL_OPTIONAL},
    {"u64", 4, PBW_TYPE_UINT64, PBW_LABEL_OPTIONAL},
    {"s32", 5, PBW_TYPE_SINT32, PBW_LABEL_OPTIONAL},
    {"s64", 6, PBW_TYPE_SINT64, PBW_LABEL_OPTIONAL},
    {"f32", 7, PBW_TYPE_FIXED32, PBW_LABEL_OPTIONAL},
    {"f64", 8, PBW_TYPE_FIXED64, PBW_LABEL_OPTIONAL},
    {"sf32", 9, PBW_TYPE_SFIXED32, PBW_LABEL_OPTIONAL},
    {"sf64", 10, PBW_TYPE_SFIXED64, PBW_LABEL_OPTIONAL},
    {"fl", 11, PBW_TYPE_FLOAT, PBW_LABEL_OPTIONAL},
    {"db", 12, PBW_TYPE_DOUBLE, PBW_LABEL_OPTIONAL},
    {"b", 13, PBW_TYPE_BOOL, PBW_LABEL_OPTIONAL},
    {"str", 14, PBW_TYPE_STRING, PBW_LABEL_OPTIONAL},
    {"byt", 15, PBW_TYPE_BYTES, PBW_LABEL_OPTIONAL},
    {"color", 16, PBW_TYPE_ENUM, PBW_LABEL_OPTIONAL},
};

static pbw_schema *
make_schema(void)
{
    pbw_schema *schema = pbw_schema_new();
    pbw_descriptor *color;
    pbw_descriptor *scalar;
    pbw_descriptor *person;
    pbw_descriptor *child;
    pbw_descriptor *parent;
    pbw_descriptor *oneof;
    pbw_descriptor *entry;
    pbw_descriptor *mapmsg;
    pbw_descriptor *deep;
    pbw_descriptor *packed;
    pbw_descriptor *plain;
    pbw_field *field;

    CHECK(schema != NULL);
    if (schema == NULL) {
        return NULL;
    }

    color = pbw_schema_add_enum(schema, "Color");
    pbw_enum_add_value(color, "RED", 0);
    pbw_enum_add_value(color, "GREEN", 1);
    pbw_enum_add_value(color, "BLUE", 2);

    scalar = pbw_schema_add_message(schema, "Scalar");
    add_fields(scalar, SCALAR_FIELDS,
               sizeof(SCALAR_FIELDS) / sizeof(SCALAR_FIELDS[0]));
    field = (pbw_field *)pbw_descriptor_find_field_name(scalar, "color");
    pbw_field_set_type_name(field, "Color");

    person = pbw_schema_add_message(schema, "Person");
    pbw_message_add_field(person, "name", 1, PBW_TYPE_STRING, PBW_LABEL_OPTIONAL);
    pbw_message_add_field(person, "id", 2, PBW_TYPE_INT32, PBW_LABEL_OPTIONAL);

    child = pbw_schema_add_message(schema, "Child");
    pbw_message_add_field(child, "name", 1, PBW_TYPE_STRING, PBW_LABEL_OPTIONAL);
    pbw_message_add_field(child, "id", 2, PBW_TYPE_INT32, PBW_LABEL_OPTIONAL);

    parent = pbw_schema_add_message(schema, "Parent");
    field = pbw_message_add_field(parent, "child", 1, PBW_TYPE_MESSAGE,
                                  PBW_LABEL_OPTIONAL);
    pbw_field_set_type_name(field, "Child");
    field = pbw_message_add_field(parent, "nums", 2, PBW_TYPE_INT32,
                                  PBW_LABEL_REPEATED);
    pbw_field_set_packed(field, 1);
    field = pbw_message_add_field(parent, "kids", 3, PBW_TYPE_MESSAGE,
                                  PBW_LABEL_REPEATED);
    pbw_field_set_type_name(field, "Child");

    oneof = pbw_schema_add_message(schema, "OneOf");
    field =
        pbw_message_add_field(oneof, "a", 1, PBW_TYPE_STRING, PBW_LABEL_OPTIONAL);
    pbw_field_set_oneof(field, 1);
    field =
        pbw_message_add_field(oneof, "b", 2, PBW_TYPE_INT32, PBW_LABEL_OPTIONAL);
    pbw_field_set_oneof(field, 1);

    entry = pbw_schema_add_message(schema, "MapEntry");
    pbw_message_add_field(entry, "key", 1, PBW_TYPE_STRING, PBW_LABEL_OPTIONAL);
    pbw_message_add_field(entry, "value", 2, PBW_TYPE_INT32, PBW_LABEL_OPTIONAL);

    mapmsg = pbw_schema_add_message(schema, "MapMsg");
    field = pbw_message_add_field(mapmsg, "entries", 1, PBW_TYPE_MESSAGE,
                                  PBW_LABEL_REPEATED);
    pbw_field_set_type_name(field, "MapEntry");

    deep = pbw_schema_add_message(schema, "Deep");
    field = pbw_message_add_field(deep, "next", 1, PBW_TYPE_MESSAGE,
                                  PBW_LABEL_OPTIONAL);
    pbw_field_set_type_name(field, "Deep");

    packed = pbw_schema_add_message(schema, "NumsPacked");
    field =
        pbw_message_add_field(packed, "v", 1, PBW_TYPE_INT32, PBW_LABEL_REPEATED);
    pbw_field_set_packed(field, 1);

    plain = pbw_schema_add_message(schema, "NumsPlain");
    pbw_message_add_field(plain, "v", 1, PBW_TYPE_INT32, PBW_LABEL_REPEATED);

    /* Field numbers where the tag varint grows: 1, 2, 3, 4, 5 bytes. */
    {
        static const uint32_t numbers[] = {1, 15, 16, 2047, 2048, 262143, 262144,
                                           536870911u};
        static const char *names[] = {"f1", "f15", "f16", "f2047", "f2048",
                                      "f262143", "f262144", "fmax"};
        size_t i;
        pbw_descriptor *wide = pbw_schema_add_message(schema, "Wide");

        for (i = 0; i < sizeof(numbers) / sizeof(numbers[0]); i++) {
            pbw_message_add_field(wide, names[i], numbers[i], PBW_TYPE_INT32,
                                  PBW_LABEL_OPTIONAL);
        }
    }

    /* A packed field whose length tag is five bytes (number >= 2^21). */
    {
        pbw_descriptor *wide = pbw_schema_add_message(schema, "WidePacked");

        pbw_field_set_packed(
            pbw_message_add_field(wide, "big", 33554432u, PBW_TYPE_INT32,
                                  PBW_LABEL_REPEATED),
            1);
    }

    /* Every packable type, packed and unpacked variants of the same numbers. */
    {
        static const struct field_def packable[] = {
            {"d", 1, PBW_TYPE_DOUBLE, PBW_LABEL_REPEATED},
            {"f", 2, PBW_TYPE_FLOAT, PBW_LABEL_REPEATED},
            {"i32", 3, PBW_TYPE_INT32, PBW_LABEL_REPEATED},
            {"i64", 4, PBW_TYPE_INT64, PBW_LABEL_REPEATED},
            {"u32", 5, PBW_TYPE_UINT32, PBW_LABEL_REPEATED},
            {"u64", 6, PBW_TYPE_UINT64, PBW_LABEL_REPEATED},
            {"s32", 7, PBW_TYPE_SINT32, PBW_LABEL_REPEATED},
            {"s64", 8, PBW_TYPE_SINT64, PBW_LABEL_REPEATED},
            {"fx32", 9, PBW_TYPE_FIXED32, PBW_LABEL_REPEATED},
            {"fx64", 10, PBW_TYPE_FIXED64, PBW_LABEL_REPEATED},
            {"sfx32", 11, PBW_TYPE_SFIXED32, PBW_LABEL_REPEATED},
            {"sfx64", 12, PBW_TYPE_SFIXED64, PBW_LABEL_REPEATED},
            {"b", 13, PBW_TYPE_BOOL, PBW_LABEL_REPEATED},
            {"e", 14, PBW_TYPE_ENUM, PBW_LABEL_REPEATED},
        };
        pbw_descriptor *packed_all = pbw_schema_add_message(schema, "PackAll");
        pbw_descriptor *plain_all = pbw_schema_add_message(schema, "PackAllPlain");
        size_t i;

        /* Descriptors are not linked yet, so keep the field handles returned by
         * pbw_message_add_field instead of looking them up by number. */
        for (i = 0; i < sizeof(packable) / sizeof(packable[0]); i++) {
            pbw_field *packed_field =
                pbw_message_add_field(packed_all, packable[i].name,
                                      packable[i].number, packable[i].type,
                                      packable[i].label);
            pbw_field *plain_field =
                pbw_message_add_field(plain_all, packable[i].name,
                                      packable[i].number, packable[i].type,
                                      packable[i].label);
            CHECK(packed_field != NULL);
            CHECK(plain_field != NULL);
            if (packable[i].type == PBW_TYPE_ENUM) {
                pbw_field_set_type_name(packed_field, "Color");
                pbw_field_set_type_name(plain_field, "Color");
            } else {
                pbw_field_set_packed(packed_field, 1);
            }
        }
    }

    /* Recursive message type: a linked schema must allow self reference. */
    {
        pbw_descriptor *tree = pbw_schema_add_message(schema, "Tree");
        pbw_message_add_field(tree, "value", 1, PBW_TYPE_INT32, PBW_LABEL_OPTIONAL);
        field = pbw_message_add_field(tree, "kids", 2, PBW_TYPE_MESSAGE,
                                      PBW_LABEL_REPEATED);
        pbw_field_set_type_name(field, "Tree");
    }

    CHECK_OK(pbw_schema_link(schema));
    return schema;
}

static const pbw_descriptor *
desc(pbw_schema *schema, const char *name)
{
    const pbw_descriptor *desc_ = pbw_schema_find(schema, name);

    CHECK(desc_ != NULL);
    return desc_;
}

static void
roundtrip(const pbw_descriptor *type, const pbw_message *msg,
          pbw_message **out)
{
    uint8_t *buf = NULL;
    size_t len = 0;

    *out = NULL;
    CHECK_OK(pbw_encode(msg, &buf, &len));
    CHECK(buf != NULL);
    CHECK_OK(pbw_decode(type, buf, len, out));
    pbw_free(buf);
}

/* Values chosen to exercise every wire type at its edges. */
static pbw_message *
make_scalars(pbw_schema *schema)
{
    pbw_message *msg = pbw_message_new(desc(schema, "Scalar"));
    static const uint8_t raw[] = {0x00, 0xff, 0x7f};

    CHECK(msg != NULL);
    CHECK_OK(pbw_message_set(msg, 1, pbw_value_new_int(-1)));
    CHECK_OK(pbw_message_set(msg, 2, pbw_value_new_int(INT64_MIN)));
    CHECK_OK(pbw_message_set(msg, 3, pbw_value_new_uint(4294967295u)));
    CHECK_OK(pbw_message_set(msg, 4, pbw_value_new_uint(UINT64_MAX)));
    CHECK_OK(pbw_message_set(msg, 5, pbw_value_new_int(INT32_MIN)));
    CHECK_OK(pbw_message_set(msg, 6, pbw_value_new_int(INT64_MAX)));
    CHECK_OK(pbw_message_set(msg, 7, pbw_value_new_uint(0xdeadbeefu)));
    CHECK_OK(pbw_message_set(msg, 8, pbw_value_new_uint(0x0102030405060708ull)));
    CHECK_OK(pbw_message_set(msg, 9, pbw_value_new_int(-2)));
    CHECK_OK(pbw_message_set(msg, 10, pbw_value_new_int(-3)));
    CHECK_OK(pbw_message_set(msg, 11, pbw_value_new_float(3.5f)));
    CHECK_OK(pbw_message_set(msg, 12, pbw_value_new_double(-2.25)));
    CHECK_OK(pbw_message_set(msg, 13, pbw_value_new_bool(1)));
    CHECK_OK(pbw_message_set(msg, 14, pbw_value_new_string("hello")));
    CHECK_OK(pbw_message_set(msg, 15, pbw_value_new_bytes(raw, sizeof(raw))));
    CHECK_OK(pbw_message_set(msg, 16, pbw_value_new_uint(2)));
    return msg;
}

/* pbw_message_encoded_size() must agree with the encoder, and a buffer sized
 * to it plus PBW_ENCODE_SLACK must be enough for pbw_encode_to(). Called from
 * the tests that build the interesting trees (packed runs, wide tags, maps,
 * groups, unknowns, proto2). */
static void
check_encoded_size(const pbw_message *msg)
{
    uint8_t *wire = NULL;
    uint8_t *buf = NULL;
    size_t wire_len = 0;
    size_t size = (size_t)-1;
    size_t len = 0;
    size_t cap;

    CHECK_OK(pbw_message_encoded_size(msg, &size));
    CHECK_OK(pbw_encode(msg, &wire, &wire_len));
    CHECK(size == wire_len);
    cap = size + PBW_ENCODE_SLACK;
    buf = (uint8_t *)malloc(cap > 0 ? cap : 1);
    CHECK(buf != NULL);
    if (buf != NULL) {
        memset(buf, 0xaa, cap > 0 ? cap : 1);
        CHECK_OK(pbw_encode_to(msg, buf, cap, &len));
        CHECK(len == size);
        CHECK(len == wire_len && memcmp(buf, wire, wire_len) == 0);
        free(buf);
    }
    pbw_free(wire);
}

/* Wire bytes produced by protoc 36.2 for the same field values
 * (`protoc --encode` over an equivalent .proto). Pins cross-implementation parity. */
static const uint8_t PROTOC_SCALARS[] = {
    0x08,
    0xff,
    0xff,
    0xff,
    0xff,
    0xff,
    0xff,
    0xff,
    0xff,
    0xff,
    0x01,
    0x10,
    0x80,
    0x80,
    0x80,
    0x80,
    0x80,
    0x80,
    0x80,
    0x80,
    0x80,
    0x01,
    0x18,
    0xff,
    0xff,
    0xff,
    0xff,
    0x0f,
    0x20,
    0xff,
    0xff,
    0xff,
    0xff,
    0xff,
    0xff,
    0xff,
    0xff,
    0xff,
    0x01,
    0x28,
    0xff,
    0xff,
    0xff,
    0xff,
    0x0f,
    0x30,
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
    0x3d,
    0xef,
    0xbe,
    0xad,
    0xde,
    0x41,
    0x08,
    0x07,
    0x06,
    0x05,
    0x04,
    0x03,
    0x02,
    0x01,
    0x4d,
    0xfe,
    0xff,
    0xff,
    0xff,
    0x51,
    0xfd,
    0xff,
    0xff,
    0xff,
    0xff,
    0xff,
    0xff,
    0xff,
    0x5d,
    0x00,
    0x00,
    0x60,
    0x40,
    0x61,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x02,
    0xc0,
    0x68,
    0x01,
    0x72,
    0x05,
    0x68,
    0x65,
    0x6c,
    0x6c,
    0x6f,
    0x7a,
    0x03,
    0x00,
    0xff,
    0x7f,
    0x80,
    0x01,
    0x02,
};

static void
test_scalars_golden(pbw_schema *schema)
{
    pbw_message *msg = make_scalars(schema);
    uint8_t *buf = NULL;
    size_t len = 0;

    CHECK_OK(pbw_encode(msg, &buf, &len));
    CHECK(len == sizeof(PROTOC_SCALARS));
    CHECK(buf != NULL && memcmp(buf, PROTOC_SCALARS, sizeof(PROTOC_SCALARS)) == 0);
    pbw_free(buf);
    pbw_message_free(msg);
}

static void
test_scalars(pbw_schema *schema)
{
    const pbw_descriptor *type = desc(schema, "Scalar");
    pbw_message *msg = make_scalars(schema);
    pbw_message *decoded = NULL;
    pbw_value *value;

    CHECK(msg != NULL);
    check_encoded_size(msg);

    roundtrip(type, msg, &decoded);
    CHECK(decoded != NULL);
    if (decoded == NULL) {
        pbw_message_free(msg);
        return;
    }
    CHECK(pbw_value_get_int32(pbw_message_get(decoded, 1)) == -1);
    CHECK(pbw_value_get_int(pbw_message_get(decoded, 2)) == INT64_MIN);
    CHECK(pbw_value_get_uint32(pbw_message_get(decoded, 3)) == 4294967295u);
    CHECK(pbw_value_get_uint(pbw_message_get(decoded, 4)) == UINT64_MAX);
    CHECK(pbw_value_get_int32(pbw_message_get(decoded, 5)) == INT32_MIN);
    CHECK(pbw_value_get_int(pbw_message_get(decoded, 6)) == INT64_MAX);
    CHECK(pbw_value_get_uint32(pbw_message_get(decoded, 7)) == 0xdeadbeefu);
    CHECK(pbw_value_get_uint(pbw_message_get(decoded, 8)) ==
          0x0102030405060708ull);
    CHECK(pbw_value_get_int32(pbw_message_get(decoded, 9)) == -2);
    CHECK(pbw_value_get_int(pbw_message_get(decoded, 10)) == -3);
    CHECK(pbw_value_get_float(pbw_message_get(decoded, 11)) == 3.5f);
    CHECK(pbw_value_get_double(pbw_message_get(decoded, 12)) == -2.25);
    CHECK(pbw_value_get_bool(pbw_message_get(decoded, 13)) == 1);
    CHECK(strcmp(pbw_value_get_string(pbw_message_get(decoded, 14)), "hello") ==
          0);
    value = pbw_message_get(decoded, 15);
    {
        static const uint8_t raw[] = {0x00, 0xff, 0x7f};
        size_t len = 0;
        const void *data = pbw_value_get_bytes(value, &len);
        CHECK(len == sizeof(raw));
        CHECK(data != NULL && memcmp(data, raw, sizeof(raw)) == 0);
    }
    CHECK(pbw_value_get_int32(pbw_message_get(decoded, 16)) == 2);

    pbw_message_free(msg);
    pbw_message_free(decoded);
}

static void
test_golden_bytes(pbw_schema *schema)
{
    const pbw_descriptor *type = desc(schema, "Person");
    pbw_message *msg = pbw_message_new(type);
    pbw_message *decoded = NULL;
    static const uint8_t expected[] = {
        0x0a, 0x03, 'a', 'b', 'c', /* name = "abc" */
        0x10, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0x01 /* id = -1 */
    };
    uint8_t *buf = NULL;
    size_t len = 0;

    CHECK_OK(pbw_message_set(msg, 1, pbw_value_new_string("abc")));
    CHECK_OK(pbw_message_set(msg, 2, pbw_value_new_int(-1)));
    CHECK_OK(pbw_encode(msg, &buf, &len));
    CHECK(len == sizeof(expected));
    CHECK(buf != NULL && memcmp(buf, expected, sizeof(expected)) == 0);
    pbw_free(buf);

    roundtrip(type, msg, &decoded);
    CHECK(pbw_message_field_count(decoded) == 2);
    pbw_message_free(msg);
    pbw_message_free(decoded);
}

static void
test_reflection(pbw_schema *schema)
{
    const pbw_descriptor *type = desc(schema, "Person");
    pbw_message *msg = pbw_message_new(type);
    pbw_message *decoded = NULL;
    const pbw_field *field;
    pbw_value *value = NULL;
    size_t index;

    CHECK_OK(pbw_message_set(msg, 1, pbw_value_new_string("abc")));
    CHECK_OK(pbw_message_set(msg, 2, pbw_value_new_int(7)));
    roundtrip(type, msg, &decoded);
    CHECK(decoded != NULL);
    if (decoded == NULL) {
        pbw_message_free(msg);
        return;
    }
    CHECK(pbw_message_field_count(decoded) == 2);
    for (index = 0; index < pbw_message_field_count(decoded); index++) {
        field = pbw_message_field_at(decoded, index, &value);
        CHECK(field != NULL);
        CHECK(value != NULL);
        CHECK(pbw_field_number(field) == index + 1);
    }
    CHECK(pbw_message_field_at(decoded, 2, &value) == NULL);
    CHECK(pbw_field_name(pbw_descriptor_find_field(type, 2)) != NULL);
    CHECK(strcmp(pbw_field_name(pbw_descriptor_find_field(type, 2)), "id") == 0);
    CHECK(pbw_descriptor_find_field(type, 42) == NULL);
    pbw_message_free(msg);
    pbw_message_free(decoded);
}

static void
test_nested(pbw_schema *schema)
{
    const pbw_descriptor *type = desc(schema, "Parent");
    pbw_message *msg = pbw_message_new(type);
    pbw_message *decoded = NULL;
    pbw_message *child;
    pbw_value *value;

    child = pbw_message_set_message(msg, 1);
    CHECK(child != NULL);
    CHECK_OK(pbw_message_set(child, 1, pbw_value_new_string("first")));
    CHECK_OK(pbw_message_set(msg, 2, pbw_value_new_int(1)));
    CHECK_OK(pbw_message_set(msg, 2, pbw_value_new_int(-5)));
    CHECK_OK(pbw_message_set(msg, 2, pbw_value_new_int(300)));
    child = pbw_message_set_message(msg, 3);
    CHECK_OK(pbw_message_set(child, 1, pbw_value_new_string("kid0")));
    child = pbw_message_set_message(msg, 3);
    CHECK_OK(pbw_message_set(child, 1, pbw_value_new_string("kid1")));
    check_encoded_size(msg);

    roundtrip(type, msg, &decoded);
    CHECK(decoded != NULL);
    if (decoded == NULL) {
        pbw_message_free(msg);
        return;
    }
    value = pbw_message_get(decoded, 2);
    CHECK(pbw_value_count(value) == 3);
    CHECK(pbw_value_get_int32(pbw_value_at(value, 0)) == 1);
    CHECK(pbw_value_get_int32(pbw_value_at(value, 1)) == -5);
    CHECK(pbw_value_get_int32(pbw_value_at(value, 2)) == 300);
    value = pbw_message_get(decoded, 3);
    CHECK(pbw_value_count(value) == 2);
    CHECK(strcmp(pbw_value_get_string(pbw_message_get(
                     pbw_value_get_message(pbw_value_at(value, 1)), 1)),
                 "kid1") == 0);
    value = pbw_message_get(decoded, 1);
    CHECK(strcmp(pbw_value_get_string(
                     pbw_message_get(pbw_value_get_message(value), 1)),
                 "first") == 0);

    pbw_message_free(msg);
    pbw_message_free(decoded);
}

static void
test_packed_compat(pbw_schema *schema)
{
    const pbw_descriptor *packed = desc(schema, "NumsPacked");
    const pbw_descriptor *plain = desc(schema, "NumsPlain");
    pbw_message *msg = pbw_message_new(packed);
    pbw_message *decoded = NULL;
    pbw_value *value;
    uint8_t *buf = NULL;
    size_t len = 0;

    CHECK_OK(pbw_message_set(msg, 1, pbw_value_new_int(1)));
    CHECK_OK(pbw_message_set(msg, 1, pbw_value_new_int(2)));
    CHECK_OK(pbw_message_set(msg, 1, pbw_value_new_int(300)));
    check_encoded_size(msg);

    /* packed encode -> decode with an unpacked descriptor */
    CHECK_OK(pbw_encode(msg, &buf, &len));
    CHECK(len == 6); /* tag + len + payload (varints 1, 2, 300 -> 4 bytes) */
    CHECK_OK(pbw_decode(plain, buf, len, &decoded));
    value = pbw_message_get(decoded, 1);
    CHECK(pbw_value_count(value) == 3);
    CHECK(pbw_value_get_int32(pbw_value_at(value, 2)) == 300);
    pbw_message_free(decoded);
    pbw_free(buf);

    /* unpacked encode -> decode with a packed descriptor */
    pbw_message_free(msg);
    msg = pbw_message_new(plain);
    CHECK_OK(pbw_message_set(msg, 1, pbw_value_new_int(1)));
    CHECK_OK(pbw_message_set(msg, 1, pbw_value_new_int(2)));
    CHECK_OK(pbw_message_set(msg, 1, pbw_value_new_int(300)));
    check_encoded_size(msg);
    CHECK_OK(pbw_encode(msg, &buf, &len));
    CHECK(len == 7); /* three tag+varint pairs */
    decoded = NULL;
    CHECK_OK(pbw_decode(packed, buf, len, &decoded));
    value = pbw_message_get(decoded, 1);
    CHECK(pbw_value_count(value) == 3);
    CHECK(pbw_value_get_int32(pbw_value_at(value, 1)) == 2);
    pbw_message_free(decoded);
    pbw_free(buf);
    pbw_message_free(msg);
}

static void
test_map(pbw_schema *schema)
{
    const pbw_descriptor *type = desc(schema, "MapMsg");
    pbw_message *msg = pbw_message_new(type);
    pbw_message *decoded = NULL;
    pbw_message *entry;
    pbw_value *value;

    entry = pbw_message_set_message(msg, 1);
    CHECK_OK(pbw_message_set(entry, 1, pbw_value_new_string("a")));
    CHECK_OK(pbw_message_set(entry, 2, pbw_value_new_int(1)));
    entry = pbw_message_set_message(msg, 1);
    CHECK_OK(pbw_message_set(entry, 1, pbw_value_new_string("b")));
    CHECK_OK(pbw_message_set(entry, 2, pbw_value_new_int(2)));
    check_encoded_size(msg);

    roundtrip(type, msg, &decoded);
    CHECK(decoded != NULL);
    if (decoded != NULL) {
        value = pbw_message_get(decoded, 1);
        CHECK(pbw_value_count(value) == 2);
        entry = pbw_value_get_message(pbw_value_at(value, 0));
        CHECK(strcmp(pbw_value_get_string(pbw_message_get(entry, 1)), "a") == 0);
        CHECK(pbw_value_get_int32(pbw_message_get(entry, 2)) == 1);
        pbw_message_free(decoded);
    }
    /* A list item belongs to its list: freeing it through the public API is
     * the documented no-op, and handing it to another message is rejected
     * instead of moving one wrapper into two lists. */
    {
        pbw_message *heap = pbw_message_new(type);
        pbw_message *kid = pbw_message_new(desc(schema, "MapEntry"));
        pbw_message *other = pbw_message_new(type);
        pbw_value *item;

        CHECK(heap != NULL && kid != NULL && other != NULL);
        CHECK_OK(pbw_message_set(kid, 1, pbw_value_new_string("a")));
        CHECK_OK(pbw_message_set(heap, 1, pbw_value_new_message(kid)));
        item = pbw_value_at(pbw_message_get(heap, 1), 0);
        pbw_value_free(item);
        CHECK(pbw_value_count(pbw_message_get(heap, 1)) == 1);
        CHECK(strcmp(pbw_value_get_string(
                         pbw_message_get(pbw_value_get_message(
                                             pbw_value_at(pbw_message_get(heap, 1), 0)),
                                         1)),
                     "a") == 0);
        CHECK_STATUS(PBW_ERR_INVALID, pbw_message_set(other, 1, item));
        pbw_message_free(other);
        pbw_message_free(heap); /* frees the item wrapper exactly once */
    }
    pbw_message_free(msg);
}

static void
test_oneof(pbw_schema *schema)
{
    const pbw_descriptor *type = desc(schema, "OneOf");
    pbw_message *msg = pbw_message_new(type);
    pbw_message *decoded = NULL;

    CHECK_OK(pbw_message_set(msg, 1, pbw_value_new_string("x")));
    CHECK_OK(pbw_message_set(msg, 2, pbw_value_new_int(5)));
    CHECK(pbw_message_get(msg, 1) == NULL);
    CHECK(pbw_message_get(msg, 2) != NULL);

    roundtrip(type, msg, &decoded);
    CHECK(decoded != NULL);
    if (decoded != NULL) {
        CHECK(pbw_message_get(decoded, 1) == NULL);
        CHECK(pbw_value_get_int32(pbw_message_get(decoded, 2)) == 5);
        pbw_message_free(decoded);
    }

    /* last one wins on the wire as well */
    {
        static const uint8_t wire[] = {0x0a, 0x01, 'x', 0x10, 0x05};
        decoded = NULL;
        CHECK_OK(pbw_decode(type, wire, sizeof(wire), &decoded));
        CHECK(pbw_message_get(decoded, 1) == NULL);
        CHECK(pbw_value_get_int32(pbw_message_get(decoded, 2)) == 5);
        pbw_message_free(decoded);
    }
    /* Clearing a member that is not the set case keeps the set one addressable
     * (and the next set of the sibling still replaces it). */
    {
        pbw_message *m = pbw_message_new(type);

        CHECK_OK(pbw_message_set(m, 2, pbw_value_new_int(5)));
        pbw_message_clear(m, 1);
        CHECK(pbw_message_get(m, 2) != NULL);
        CHECK_OK(pbw_message_set(m, 1, pbw_value_new_string("y")));
        CHECK(pbw_message_get(m, 2) == NULL);
        CHECK(pbw_message_get(m, 1) != NULL);
        pbw_message_free(m);
    }
    pbw_message_free(msg);
}

/* Boundary arguments the library must reject instead of computing with them:
 * sizes that would wrap the length arithmetic in the bytes constructor and the
 * unknown-field accumulator, and a nested decoded root (which owns an arena)
 * stored inside another message. */

/* JSON and text format are defined with '.' as the decimal separator; the host
 * process's LC_NUMERIC must not change what is written or accepted. */
static void
test_locale_independence(pbw_schema *schema)
{
    const pbw_descriptor *type = desc(schema, "Scalar");
    char saved[64];
    const char *current = setlocale(LC_NUMERIC, NULL);

    if (current == NULL || strlen(current) >= sizeof(saved)) {
        return; /* no locale support: nothing to check */
    }
    memcpy(saved, current, strlen(current) + 1);
    if (setlocale(LC_NUMERIC, "de_DE.UTF-8") == NULL &&
        setlocale(LC_NUMERIC, "de_DE") == NULL &&
        setlocale(LC_NUMERIC, "fr_FR.UTF-8") == NULL) {
        return; /* no comma locale installed on this machine */
    }
    if (!(localeconv()->decimal_point[0] == ',')) {
        setlocale(LC_NUMERIC, saved);
        return;
    }
    {
        pbw_message *msg = pbw_message_new(type);
        pbw_message *parsed = NULL;
        char *out = NULL;
        size_t out_len = 0;

        CHECK(msg != NULL);
        CHECK_OK(pbw_message_set(msg, 12, pbw_value_new_double(1.5)));
        CHECK_OK(pbw_json_print(msg, &out, &out_len));
        CHECK(out != NULL && strcmp(out, "{\"db\":1.5}") == 0);
        pbw_free(out);
        out = NULL;
        CHECK_OK(pbw_text_print(msg, &out, &out_len));
        CHECK(out != NULL && strcmp(out, "db: 1.5\n") == 0);
        pbw_free(out);
        /* an integer-valued double prints without a decimal point, and a
         * long mantissa takes the heap path in the slow converter */
        {
            static const char big[] =
                "db: 0.000000000000000000000000000000000000000000000000000000000000001";

            CHECK_OK(pbw_message_set(msg, 12, pbw_value_new_double(2.0)));
            CHECK_OK(pbw_text_print(msg, &out, &out_len));
            CHECK(out != NULL && strcmp(out, "db: 2\n") == 0);
            pbw_free(out);
            out = NULL;
            CHECK_OK(
                pbw_text_parse(type, big, strlen(big), &parsed));
            CHECK(parsed != NULL);
            pbw_message_free(parsed);
            parsed = NULL;
        }
        pbw_message_free(msg);

        CHECK_OK(pbw_json_parse(type, "{\"db\":1.5}", strlen("{\"db\":1.5}"), &parsed));
        CHECK(parsed != NULL &&
              pbw_value_get_double(pbw_message_get(parsed, 12)) == 1.5);
        pbw_message_free(parsed);
        parsed = NULL;
        CHECK_OK(pbw_text_parse(type, "db: 1.5", 7, &parsed));
        CHECK(parsed != NULL &&
              pbw_value_get_double(pbw_message_get(parsed, 12)) == 1.5);
        pbw_message_free(parsed);
    }
    setlocale(LC_NUMERIC, saved);
}

static void
test_hostile_arguments(pbw_schema *schema)
{
    const pbw_descriptor *person = desc(schema, "Person");
    const pbw_descriptor *parent = desc(schema, "Parent");

    /* len + 1 must not wrap. */
    CHECK(pbw_value_new_bytes(NULL, SIZE_MAX) == NULL);
    {
        pbw_value *value = pbw_value_new_bytes("x", 1);

        CHECK(value != NULL);
        CHECK(pbw_value_get_bytes(value, NULL) != NULL);
        pbw_value_free(value);
    }

    /* Unknown-field growth past the first 64-byte block, on the heap path and
     * on the arena path, keeps every byte. */
    {
        size_t n = 256 * 1024;
        uint8_t *blob = (uint8_t *)malloc(n);
        pbw_message *msg = pbw_message_new(person);
        size_t i;

        CHECK(blob != NULL);
        CHECK(msg != NULL);
        for (i = 0; i < n; i++) {
            blob[i] = (uint8_t)i;
        }
        CHECK_OK(pbw_message_add_unknown_bytes(msg, blob, n));
        CHECK(pbw_message_unknown_len(msg) == n);
        CHECK(memcmp(pbw_message_unknown_bytes(msg), blob, n) == 0);
        pbw_message_free(msg);
        free(blob);
    }
    {
        /* A decoded root owns its arena; storing it as a submessage of a heap
         * message must free that arena with the parent (no leak, no double
         * free). */
        static const uint8_t child_wire[] = {0x0a, 0x01, 'k', 0x10, 0x01};
        pbw_message *child = NULL;
        pbw_message *container = pbw_message_new(parent);
        pbw_value *value;

        CHECK_OK(pbw_decode(desc(schema, "Child"), child_wire,
                            sizeof(child_wire), &child));
        value = pbw_value_new_message(child);
        CHECK(value != NULL);
        CHECK_OK(pbw_message_set(container, 1, value));
        {
            pbw_message *nested =
                pbw_value_get_message(pbw_message_get(container, 1));

            CHECK(nested != NULL);
            CHECK(pbw_value_get_int32(pbw_message_get(nested, 2)) == 1);
        }
        pbw_message_free(container);
    }

    /* The layout freezes at the first successful link: a later field would
     * desynchronize the value slots of messages that already exist. */
    {
        pbw_schema *late = pbw_schema_new();
        pbw_descriptor *msg_type;

        CHECK(late != NULL);
        msg_type = pbw_schema_add_message(late, "Late");
        CHECK(msg_type != NULL);
        CHECK(pbw_message_add_field(msg_type, "a", 1, PBW_TYPE_INT32,
                                    PBW_LABEL_OPTIONAL) != NULL);
        CHECK_OK(pbw_schema_link(late));
        {
            pbw_message *msg = pbw_message_new(msg_type);

            CHECK(msg != NULL);
            CHECK(pbw_message_add_field(msg_type, "b", 2, PBW_TYPE_INT32,
                                        PBW_LABEL_OPTIONAL) == NULL);
            CHECK(pbw_descriptor_field_count(msg_type) == 1);
            CHECK(pbw_descriptor_find_field(msg_type, 2) == NULL);
            CHECK_OK(pbw_message_set(msg, 1, pbw_value_new_int(5)));
            CHECK(pbw_value_get_int32(pbw_message_get(msg, 1)) == 5);
            pbw_message_free(msg);
        }
        pbw_schema_free(late);
    }

    /* A JSON number is not limited to the reference-agnostic 63 characters the
     * parser used to accept. */
    {
        static const char long_number[] =
            "{\"db\":1.00000000000000000000000000000000000000000000000000000000000000000000001}";
        pbw_message *parsed = NULL;

        CHECK_OK(pbw_json_parse(desc(schema, "Scalar"), long_number,
                                sizeof(long_number) - 1, &parsed));
        CHECK(parsed != NULL);
        pbw_message_free(parsed);
    }

    /* A feed chunk no allocation can hold fails instead of spinning on a
     * wrapped capacity. */
    {
        pbw_istream *stream = pbw_istream_new(NULL);

        CHECK(stream != NULL);
        CHECK_STATUS(PBW_ERR_NOMEM, pbw_istream_feed(stream, "x", SIZE_MAX));
        CHECK_OK(pbw_istream_feed(stream, "ab", 2));
        pbw_istream_free(stream);
    }

    /* A one-byte length prefix that runs past the buffer is invalid, on the
     * fast path as well as the general one. */
    {
        static const uint8_t truncated[] = {0x0a, 0x05, 'a'};
        static const uint8_t complete[] = {0x0a, 0x01, 'a'};
        pbw_message *msg = NULL;

        CHECK_STATUS(PBW_ERR_INVALID,
                     pbw_decode(person, truncated, sizeof(truncated), &msg));
        CHECK_OK(pbw_decode(person, complete, sizeof(complete), &msg));
        CHECK(strcmp(pbw_value_get_string(pbw_message_get(msg, 1)), "a") == 0);
        pbw_message_free(msg);
    }
}

static void
test_unknown_fields(pbw_schema *schema)
{
    const pbw_descriptor *type = desc(schema, "Person");
    static const uint8_t wire[] = {
        0x0a, 0x03, 'a', 'b', 'c',          /* name */
        0x98, 0x06, 0x05,                   /* field 99, varint */
        0x9a, 0x06, 0x02, 'x', 'y',         /* field 99, length delimited */
        0x9d, 0x06, 0x01, 0x02, 0x03, 0x04, /* field 99, 32-bit */
        0x99, 0x06, 0x01, 0x02, 0x03, 0x04,
        0x05, 0x06, 0x07, 0x08, /* field 99, 64-bit */
        0x10, 0x07              /* id = 7 */
    };
    pbw_message *decoded = NULL;

    CHECK_OK(pbw_decode(type, wire, sizeof(wire), &decoded));
    CHECK(decoded != NULL);
    if (decoded != NULL) {
        check_encoded_size(decoded);
        CHECK(strcmp(pbw_value_get_string(pbw_message_get(decoded, 1)), "abc") ==
              0);
        CHECK(pbw_value_get_int32(pbw_message_get(decoded, 2)) == 7);
        /* Unknown fields survive a decode/encode round trip byte for byte and
         * are written after the known fields, in the order they were read: the
         * reference encoders reorder the same way. */
        {
            static const uint8_t expected[] = {
                0x0a, 0x03, 'a', 'b', 'c', /* name, known */
                0x10, 0x07,                /* id, known */
                0x98, 0x06, 0x05,          /* then the unknowns */
                0x9a, 0x06, 0x02, 'x', 'y',
                0x9d, 0x06, 0x01, 0x02, 0x03, 0x04,
                0x99, 0x06, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
            uint8_t *buf = NULL;
            size_t len = 0;

            CHECK_OK(pbw_encode(decoded, &buf, &len));
            CHECK(len == sizeof(expected));
            if (len == sizeof(expected)) {
                CHECK(memcmp(buf, expected, len) == 0);
            }
            pbw_free(buf);
        }
        pbw_message_free(decoded);
    }

    /* a known field with the wrong wire type is treated as unknown and kept:
     * id is a varint, so a length-delimited field 2 must not be dropped */
    {
        static const uint8_t id_as_bytes[] = {0x12, 0x01, 0x07};
        uint8_t *buf = NULL;
        size_t len = 0;

        decoded = NULL;
        CHECK_OK(pbw_decode(type, id_as_bytes, sizeof(id_as_bytes), &decoded));
        CHECK(pbw_message_get(decoded, 2) == NULL);
        CHECK_OK(pbw_encode(decoded, &buf, &len));
        CHECK(len == sizeof(id_as_bytes) &&
              memcmp(buf, id_as_bytes, len) == 0);
        pbw_free(buf);
        pbw_message_free(decoded);
    }

    /* legacy groups with unknown field numbers are skipped, nesting bounded */
    {
        static const uint8_t group[] = {0x0b, 0x08, 0x01, 0x0c, 0x10, 0x02};
        uint8_t *buf = NULL;
        size_t len = 0;

        decoded = NULL;
        CHECK_OK(pbw_decode(type, group, sizeof(group), &decoded));
        CHECK(pbw_value_get_int32(pbw_message_get(decoded, 2)) == 2);
        /* the group is unknown, so it is kept as-is and written last */
        {
            static const uint8_t expected[] = {0x10, 0x02, 0x0b, 0x08, 0x01,
                                               0x0c};

            CHECK_OK(pbw_encode(decoded, &buf, &len));
            CHECK(len == sizeof(expected));
            if (len == sizeof(expected)) {
                CHECK(memcmp(buf, expected, len) == 0);
            }
        }
        pbw_free(buf);
        pbw_message_free(decoded);
    }
    /* an unterminated group is invalid, not silently skipped */
    {
        static const uint8_t open_group[] = {0x0b, 0x08, 0x01};
        CHECK_STATUS(PBW_ERR_INVALID,
                     pbw_decode(type, open_group, sizeof(open_group), &decoded));
    }
    /* a group whose contents contain field number 0 is invalid */
    {
        static const uint8_t bad_inner[] = {0x0b, 0x09, 0x01, 0x0c};
        CHECK_STATUS(PBW_ERR_INVALID,
                     pbw_decode(type, bad_inner, sizeof(bad_inner), &decoded));
    }
}

/* Tag and varint width rules, pinned against libprotobuf 36:
 *   - a tag is a varint32, so at most 5 bytes; a longer tag is invalid
 *   - bits above bit 31 of a 5-byte tag are dropped (reference truncates)
 *   - a value varint may be up to 10 bytes, never more */
static void
test_tag_limits(pbw_schema *schema)
{
    const pbw_descriptor *type = desc(schema, "Person");
    pbw_message *decoded = NULL;

    /* 6-byte tag varint: rejected, even though its value fits in 32 bits. */
    {
        static const uint8_t long_tag[] = {0x88, 0x80, 0x80, 0x80, 0x80, 0x00, 0x01};
        CHECK_STATUS(PBW_ERR_INVALID,
                     pbw_decode(type, long_tag, sizeof(long_tag), &decoded));
    }
    /* A 5-byte tag varint above UINT32_MAX truncates to 32 bits, exactly like
     * the reference parser: this one becomes a valid tag for field 2.
     * (The conformance BadTag tests pin the shared truncation.) */
    {
        static const uint8_t wide_tag[] = {0x90, 0x80, 0x80, 0x80, 0x70, 0x01};
        decoded = NULL;
        CHECK_OK(pbw_decode(type, wide_tag, sizeof(wide_tag), &decoded));
        CHECK(pbw_message_field_count(decoded) == 1);
        pbw_message_free(decoded);
        /* Four bits in the fifth byte still fit in 32 bits: a legal tag for a
         * field number nobody declared, so it is skipped as unknown. */
        static const uint8_t high_ok[] = {0x90, 0x80, 0x80, 0x80, 0x0f, 0x01};
        decoded = NULL;
        CHECK_OK(pbw_decode(type, high_ok, sizeof(high_ok), &decoded));
        CHECK(pbw_message_field_count(decoded) == 0);
        pbw_message_free(decoded);
    }
    /* 5-byte non-minimal tag for field 2 is accepted (reference is lenient). */
    {
        static const uint8_t padded[] = {0x90, 0x80, 0x80, 0x80, 0x00, 0x01};
        decoded = NULL;
        CHECK_OK(pbw_decode(type, padded, sizeof(padded), &decoded));
        CHECK(pbw_value_get_int32(pbw_message_get(decoded, 2)) == 1);
        pbw_message_free(decoded);
    }
    /* 11-byte value varint: rejected. */
    {
        static const uint8_t long_value[] = {0x10, 0x80, 0x80, 0x80, 0x80,
                                             0x80, 0x80, 0x80, 0x80, 0x80,
                                             0x80, 0x00};
        CHECK_STATUS(
            PBW_ERR_INVALID,
            pbw_decode(type, long_value, sizeof(long_value), &decoded));
    }
    /* 10-byte value varint holding all 64 bits: accepted. */
    {
        static const uint8_t full_value[] = {0x10, 0xff, 0xff, 0xff, 0xff, 0xff,
                                             0xff, 0xff, 0xff, 0xff, 0x01};
        decoded = NULL;
        CHECK_OK(pbw_decode(type, full_value, sizeof(full_value), &decoded));
        CHECK(pbw_value_get_int32(pbw_message_get(decoded, 2)) == -1);
        pbw_message_free(decoded);
    }
}

/* ---------------------------------------------------------- wire structure */

/* Tag varints grow at 16, 2048, 262144 and 2^28; values from protoc 36.2. */
static void
test_field_number_edges(pbw_schema *schema)
{
    const pbw_descriptor *type = desc(schema, "Wide");
    static const uint8_t expected[] = {
        0x08, 0x07, 0x78, 0x07, 0x80, 0x01, 0x07, 0xf8, 0x7f, 0x07,
        0x80, 0x80, 0x01, 0x07, 0xf8, 0xff, 0x7f, 0x07, 0x80, 0x80,
        0x80, 0x01, 0x07, 0xf8, 0xff, 0xff, 0xff, 0x0f, 0x07};
    static const uint32_t numbers[] = {1, 15, 16, 2047, 2048, 262143, 262144,
                                       536870911u};
    pbw_message *msg = pbw_message_new(type);
    pbw_message *decoded = NULL;
    uint8_t *buf = NULL;
    size_t len = 0;
    size_t i;

    for (i = 0; i < sizeof(numbers) / sizeof(numbers[0]); i++) {
        CHECK_OK(pbw_message_set(msg, numbers[i], pbw_value_new_int(7)));
    }
    CHECK_OK(pbw_encode(msg, &buf, &len));
    CHECK(len == sizeof(expected));
    CHECK(buf != NULL && memcmp(buf, expected, sizeof(expected)) == 0);
    CHECK_OK(pbw_decode(type, buf, len, &decoded));
    for (i = 0; i < sizeof(numbers) / sizeof(numbers[0]); i++) {
        CHECK(pbw_value_get_int32(pbw_message_get(decoded, numbers[i])) == 7);
    }
    CHECK(pbw_message_field_count(decoded) == 8);
    pbw_free(buf);
    pbw_message_free(decoded);
    pbw_message_free(msg);
}

static void
test_packed_wide_tag(pbw_schema *schema)
{
    const pbw_descriptor *type = desc(schema, "WidePacked");
    pbw_message *msg = pbw_message_new(type);
    pbw_message *decoded = NULL;
    uint8_t *buf = NULL;
    size_t len = 0;

    CHECK_OK(pbw_message_set(msg, 33554432u, pbw_value_new_int(1)));
    CHECK_OK(pbw_message_set(msg, 33554432u, pbw_value_new_int(150)));
    check_encoded_size(msg);
    CHECK_OK(pbw_encode(msg, &buf, &len));
    /* Tag 0x82 0x80 0x80 0x80 0x01, length 3, then the two varints. */
    {
        static const uint8_t expected[] = {0x82, 0x80, 0x80, 0x80,
                                           0x01, 0x03, 0x01, 0x96, 0x01};

        CHECK(len == sizeof(expected));
        CHECK(buf != NULL && memcmp(buf, expected, sizeof(expected)) == 0);
    }
    CHECK_OK(pbw_decode(type, buf, len, &decoded));
    CHECK(decoded != NULL);
    CHECK(pbw_value_count(pbw_message_get(decoded, 33554432u)) == 2);
    pbw_message_free(decoded);
    pbw_message_free(msg);
    pbw_free(buf);
}

/* Every packable type, packed and unpacked, decoded by both descriptors. */
static void
test_pack_all_types(pbw_schema *schema)
{
    const pbw_descriptor *packed = desc(schema, "PackAll");
    const pbw_descriptor *plain = desc(schema, "PackAllPlain");
    const size_t values[] = {3, 1};
    size_t t;

    for (t = 0; t < 2; t++) {
        const pbw_descriptor *source = t == 0 ? packed : plain;
        const pbw_descriptor *target = t == 0 ? plain : packed;
        pbw_message *msg = pbw_message_new(source);
        pbw_message *decoded = NULL;
        uint8_t *buf = NULL;
        size_t len = 0;
        size_t v;

        for (v = 0; v < sizeof(values) / sizeof(values[0]); v++) {
            int64_t value = (int64_t)values[v];
            CHECK_OK(pbw_message_set(msg, 1, pbw_value_new_double(-2.25 * value)));
            CHECK_OK(pbw_message_set(msg, 2, pbw_value_new_float(1.5f * value)));
            CHECK_OK(pbw_message_set(msg, 3, pbw_value_new_int(-value)));
            CHECK_OK(pbw_message_set(msg, 4, pbw_value_new_int(INT64_MIN + value)));
            CHECK_OK(pbw_message_set(msg, 5, pbw_value_new_uint(4000000000u)));
            CHECK_OK(pbw_message_set(msg, 6, pbw_value_new_uint(UINT64_MAX)));
            CHECK_OK(pbw_message_set(msg, 7, pbw_value_new_int(-value)));
            CHECK_OK(pbw_message_set(msg, 8, pbw_value_new_int(value)));
            CHECK_OK(pbw_message_set(msg, 9, pbw_value_new_uint(0xdeadbeefu)));
            CHECK_OK(pbw_message_set(msg, 10, pbw_value_new_uint(0x0102030405060708ull)));
            CHECK_OK(pbw_message_set(msg, 11, pbw_value_new_int(-value)));
            CHECK_OK(pbw_message_set(msg, 12, pbw_value_new_int(-value)));
            CHECK_OK(pbw_message_set(msg, 13, pbw_value_new_bool((int)value)));
            CHECK_OK(pbw_message_set(msg, 14, pbw_value_new_int(2)));
        }
        check_encoded_size(msg);
        CHECK_OK(pbw_encode(msg, &buf, &len));
        /* The opposite descriptor must decode it: packedness is a hint. */
        CHECK_OK(pbw_decode(target, buf, len, &decoded));
        CHECK(pbw_value_count(pbw_message_get(decoded, 1)) == 2);
        CHECK(pbw_value_count(pbw_message_get(decoded, 14)) == 2);
        CHECK(pbw_value_get_uint32(pbw_value_at(pbw_message_get(decoded, 5), 0)) ==
              4000000000u);
        CHECK(pbw_value_get_uint(pbw_value_at(pbw_message_get(decoded, 6), 0)) ==
              UINT64_MAX);
        CHECK(pbw_value_get_int32(pbw_value_at(pbw_message_get(decoded, 3), 0)) == -3);
        CHECK(pbw_value_get_float(pbw_value_at(pbw_message_get(decoded, 2), 0)) ==
              4.5f);
        /* Signed fixed-width repeated items are sign extended, packed and
         * unpacked alike: the same representation as a singular field. */
        CHECK(pbw_value_get_int(pbw_value_at(pbw_message_get(decoded, 11), 0)) ==
              -3);
        CHECK(pbw_value_get_int(pbw_value_at(pbw_message_get(decoded, 12), 0)) ==
              -3);
        pbw_free(buf);
        pbw_message_free(decoded);
        pbw_message_free(msg);
    }
}

/* Packed fixed-width fields reserve their exact item count up front (no
 * growth chain), and an empty packed field on the wire stays absent. */
static void
test_packed_presize(pbw_schema *schema)
{
    const pbw_descriptor *packed = desc(schema, "PackAll");
    /* Field 9 (fixed32) tag with an empty length. */
    static const uint8_t empty_packed[] = {0x4A, 0x00};
    pbw_message *msg = pbw_message_new(packed);
    pbw_message *decoded = NULL;
    pbw_message *empty = NULL;
    uint8_t *buf = NULL;
    size_t len = 0;

    CHECK_OK(pbw_message_set(msg, 9, pbw_value_new_uint(1)));
    CHECK_OK(pbw_message_set(msg, 9, pbw_value_new_uint(2)));
    CHECK_OK(pbw_message_set(msg, 12, pbw_value_new_int(-1)));
    CHECK_OK(pbw_encode(msg, &buf, &len));
    CHECK_OK(pbw_decode(packed, buf, len, &decoded));
    CHECK(decoded->values[8].as.list.len == 2);
    CHECK(decoded->values[8].as.list.cap == 2);
    CHECK(decoded->values[11].as.list.len == 1);
    CHECK(decoded->values[11].as.list.cap == 1);
    CHECK_OK(pbw_decode(packed, empty_packed, sizeof(empty_packed), &empty));
    CHECK(!pbw_msg_present(empty, 8));
    pbw_free(buf);
    pbw_message_free(decoded);
    pbw_message_free(empty);
    pbw_message_free(msg);
}

/* A value whose kind does not match the field is rejected with TYPE and
 * leaves the message encodable. (Corrupt trees built by poking internals
 * are not a supported state; only the public boundary is tested here.) */
static void
test_packed_hostile(pbw_schema *schema)
{
    const pbw_descriptor *packed = desc(schema, "PackAll");
    /* varint, fixed-width, bool and enum representatives */
    static const uint32_t fields[] = {1, 9, 13, 14};
    size_t i;

    for (i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        pbw_message *msg = pbw_message_new(packed);
        uint8_t *buf = NULL;
        size_t len = 0;

        CHECK(msg != NULL);
        {
            pbw_value *rejected = pbw_value_new_string("x");
            pbw_value *rejected_msg = pbw_value_new_message(
                pbw_message_new(packed));

            CHECK_STATUS(PBW_ERR_TYPE,
                         pbw_message_set(msg, fields[i], rejected));
            pbw_value_free(rejected);
            CHECK_STATUS(PBW_ERR_TYPE,
                         pbw_message_set(msg, fields[i], rejected_msg));
            pbw_value_free(rejected_msg);
        }
        /* the rejected stores left nothing behind: still encodable */
        CHECK_OK(pbw_message_set(msg, fields[i],
                                 pbw_value_new_uint(1)));
        CHECK_OK(pbw_encode(msg, &buf, &len));
        pbw_free(buf);
        pbw_message_free(msg);
    }
}

static void
test_recursive_type(pbw_schema *schema)
{
    const pbw_descriptor *type = desc(schema, "Tree");
    pbw_message *root = pbw_message_new(type);
    pbw_message *branch = pbw_message_set_message(root, 2);
    pbw_message *leaf_a;
    pbw_message *leaf_b;
    pbw_message *decoded = NULL;
    uint8_t *buf = NULL;
    size_t len = 0;

    CHECK_OK(pbw_message_set(root, 1, pbw_value_new_int(1)));
    CHECK_OK(pbw_message_set(branch, 1, pbw_value_new_int(2)));
    leaf_a = pbw_message_set_message(branch, 2);
    leaf_b = pbw_message_set_message(branch, 2);
    CHECK_OK(pbw_message_set(leaf_a, 1, pbw_value_new_int(3)));
    CHECK_OK(pbw_message_set(leaf_b, 1, pbw_value_new_int(4)));

    CHECK_OK(pbw_encode(root, &buf, &len));
    CHECK_OK(pbw_decode(type, buf, len, &decoded));
    {
        /* kids is a repeated field, so the message lives inside a list value */
        pbw_value *root_kids = pbw_message_get(decoded, 2);
        pbw_message *back;
        pbw_value *kids;

        CHECK(pbw_value_count(root_kids) == 1);
        back = pbw_value_get_message(pbw_value_at(root_kids, 0));
        CHECK(back != NULL);
        CHECK(pbw_value_get_int32(pbw_message_get(back, 1)) == 2);
        kids = pbw_message_get(back, 2);
        CHECK(pbw_value_count(kids) == 2);
        CHECK(pbw_value_get_int32(pbw_message_get(
                  pbw_value_get_message(pbw_value_at(kids, 1)), 1)) == 4);
    }
    pbw_free(buf);
    pbw_message_free(decoded);
    pbw_message_free(root);
}

/* A hand-built tree deeper than the nesting cap must fail loudly in every
 * formatter instead of recursing until the stack gives out: the binary
 * encoder, the JSON printer and the text printer share the same
 * PBW_MAX_DEPTH rule as the decoder. */
static void
test_print_depth_limit(pbw_schema *schema)
{
    const pbw_descriptor *type = desc(schema, "Tree");
    pbw_message *root = pbw_message_new(type);
    pbw_message *cur = root;
    char *text = NULL;
    size_t text_len = 0;
    uint8_t *buf = NULL;
    size_t len = 0;
    int i;

    CHECK(root != NULL);
    for (i = 0; i <= (int)PBW_MAX_DEPTH; i++) {
        /* kids (field 2) is repeated: every call appends one level */
        cur = pbw_message_set_message(cur, 2);
        CHECK(cur != NULL);
    }
    CHECK_STATUS(PBW_ERR_LIMIT, pbw_encode(root, &buf, &len));
    CHECK(buf == NULL && len == 0);
    /* The buffer pointer is replaced on both paths: a caller reusing one
     * never keeps a stale pointer after the failure. */
    buf = (uint8_t *)&text_len;
    len = 99;
    CHECK_STATUS(PBW_ERR_LIMIT, pbw_encode(root, &buf, &len));
    CHECK(buf == NULL && len == 0);
    CHECK_STATUS(PBW_ERR_LIMIT, pbw_json_print(root, &text, &text_len));
    CHECK(text == NULL && text_len == 0);
    CHECK_STATUS(PBW_ERR_LIMIT, pbw_text_print(root, &text, &text_len));
    CHECK(text == NULL && text_len == 0);
    pbw_message_free(root);
}

/* A group decodes exactly as deep as a message with the same cap: the group
 * body must not spend an extra nesting level on its own check. */
static void
test_group_depth_limit(void)
{
    pbw_schema *schema = pbw_schema_new();
    pbw_descriptor *grp;
    pbw_descriptor *msg;
    pbw_field *f;
    pbw_message *root;
    pbw_message *cur;
    pbw_message *decoded = NULL;
    pbw_limits lim;
    uint8_t *buf = NULL;
    size_t len = 0;
    int i;

    CHECK(schema != NULL);
    if (schema == NULL) {
        return;
    }
    pbw_schema_set_proto3(schema, 0);
    grp = pbw_schema_add_message(schema, "G");
    msg = pbw_schema_add_message(schema, "M");
    f = pbw_message_add_field(grp, "n", 1, PBW_TYPE_GROUP, PBW_LABEL_OPTIONAL);
    pbw_field_set_type_name(f, "G");
    f = pbw_message_add_field(msg, "n", 1, PBW_TYPE_MESSAGE,
                              PBW_LABEL_OPTIONAL);
    pbw_field_set_type_name(f, "M");
    CHECK_OK(pbw_schema_link(schema));

    memset(&lim, 0, sizeof(lim));
    lim.max_depth = 3;
    root = pbw_message_new(grp);
    CHECK(root != NULL);
    cur = root;
    for (i = 0; i < 2; i++) {
        cur = pbw_message_set_message(cur, 1);
        CHECK(cur != NULL);
    }
    CHECK_OK(pbw_encode(root, &buf, &len));
    CHECK_OK(pbw_decode_limits(grp, buf, len, &lim, &decoded));
    pbw_message_free(decoded);
    decoded = NULL;
    pbw_free(buf);
    buf = NULL;
    pbw_message_free(root);

    root = pbw_message_new(msg);
    CHECK(root != NULL);
    cur = root;
    for (i = 0; i < 2; i++) {
        cur = pbw_message_set_message(cur, 1);
        CHECK(cur != NULL);
    }
    CHECK_OK(pbw_encode(root, &buf, &len));
    CHECK_OK(pbw_decode_limits(msg, buf, len, &lim, &decoded));
    pbw_message_free(decoded);
    decoded = NULL;
    /* One level below the nesting: both shapes fail together. */
    lim.max_depth = 1;
    CHECK_STATUS(PBW_ERR_LIMIT,
                 pbw_decode_limits(msg, buf, len, &lim, &decoded));
    CHECK(decoded == NULL);
    pbw_free(buf);
    pbw_message_free(root);
    pbw_schema_free(schema);

    /* An unlinked descriptor has no tags yet: both directions refuse it. */
    schema = pbw_schema_new();
    msg = pbw_schema_add_message(schema, "U");
    pbw_message_add_field(msg, "a", 1, PBW_TYPE_INT32, PBW_LABEL_OPTIONAL);
    CHECK_OK(pbw_schema_link(schema));
    root = pbw_message_new(msg);
    CHECK(root != NULL);
    CHECK_OK(pbw_message_set(root, 1, pbw_value_new_int(1)));
    /* any schema edit after link drops the link (and the precomputed tags) */
    pbw_field_set_type_name((pbw_field *)pbw_descriptor_field_at(msg, 0),
                            NULL);
    buf = (uint8_t *)&len;
    len = 99;
    CHECK_STATUS(PBW_ERR_INVALID, pbw_encode(root, &buf, &len));
    CHECK(buf == NULL && len == 0);
    pbw_message_free(root);
    pbw_schema_free(schema);
}

/* bool accepts any non-zero varint; float/double keep their exact bit pattern. */
static void
test_bool_and_float_bits(pbw_schema *schema)
{
    const pbw_descriptor *type = desc(schema, "Scalar");
    static const uint32_t float_bits[] = {0x00000000u, 0x80000000u, 0x7f800000u,
                                          0xff800000u, 0x7fc00001u, 0x00000001u};
    static const uint64_t double_bits[] = {0x8000000000000000ull,
                                           0x7ff0000000000000ull,
                                           0xfff0000000000001ull,
                                           0x0000000000000001ull};
    size_t i;

    for (i = 0; i < sizeof(float_bits) / sizeof(float_bits[0]); i++) {
        pbw_message *msg = pbw_message_new(type);
        pbw_message *decoded = NULL;

        CHECK_OK(pbw_message_set(msg, 11, pbw_value_new_uint(float_bits[i])));
        roundtrip(type, msg, &decoded);
        CHECK(pbw_value_get_uint32(pbw_message_get(decoded, 11)) == float_bits[i]);
        pbw_message_free(decoded);
        pbw_message_free(msg);
    }
    for (i = 0; i < sizeof(double_bits) / sizeof(double_bits[0]); i++) {
        pbw_message *msg = pbw_message_new(type);
        pbw_message *decoded = NULL;

        CHECK_OK(pbw_message_set(msg, 12, pbw_value_new_uint(double_bits[i])));
        roundtrip(type, msg, &decoded);
        CHECK(pbw_value_get_uint(pbw_message_get(decoded, 12)) == double_bits[i]);
        pbw_message_free(decoded);
        pbw_message_free(msg);
    }

    /* Any non-zero bool on the wire means true; it is re-emitted as 1. */
    {
        static const uint8_t wire[] = {0x68, 0x02};
        pbw_message *decoded = NULL;
        uint8_t *buf = NULL;
        size_t len = 0;

        CHECK_OK(pbw_decode(type, wire, sizeof(wire), &decoded));
        CHECK(pbw_value_get_bool(pbw_message_get(decoded, 13)) == 1);
        CHECK_OK(pbw_encode(decoded, &buf, &len));
        CHECK(len == 2 && buf[0] == 0x68 && buf[1] == 0x01);
        pbw_free(buf);
        pbw_message_free(decoded);
    }
}

/* Enums are open: an unknown number is kept, negative values round-trip. */
static void
test_enum_edges(pbw_schema *schema)
{
    const pbw_descriptor *type = desc(schema, "Scalar");
    static const int32_t numbers[] = {0, 2, 99, -1, INT32_MIN, INT32_MAX};
    size_t i;

    for (i = 0; i < sizeof(numbers) / sizeof(numbers[0]); i++) {
        pbw_message *msg = pbw_message_new(type);
        pbw_message *decoded = NULL;

        CHECK_OK(pbw_message_set(msg, 16, pbw_value_new_int(numbers[i])));
        roundtrip(type, msg, &decoded);
        CHECK(pbw_value_get_int32(pbw_message_get(decoded, 16)) == numbers[i]);
        pbw_message_free(decoded);
        pbw_message_free(msg);
    }
}

/* pbwire has explicit presence for every field: encoding a field set to its
 * type's zero value emits it, unlike proto3 generated code. Missing stays
 * missing. */
static void
test_explicit_presence(pbw_schema *schema)
{
    const pbw_descriptor *person = desc(schema, "Person");
    const pbw_descriptor *type = desc(schema, "Scalar");
    pbw_message *msg = pbw_message_new(person);
    pbw_message *decoded = NULL;
    static const uint8_t zero_wire[] = {0x0a, 0x00, 0x10, 0x00};
    uint8_t *buf = NULL;
    size_t len = 0;

    CHECK_OK(pbw_message_set(msg, 1, pbw_value_new_string("")));
    CHECK_OK(pbw_message_set(msg, 2, pbw_value_new_int(0)));
    CHECK_OK(pbw_encode(msg, &buf, &len));
    CHECK(len == sizeof(zero_wire));
    CHECK(buf != NULL && memcmp(buf, zero_wire, sizeof(zero_wire)) == 0);
    CHECK_OK(pbw_decode(person, buf, len, &decoded));
    CHECK(pbw_message_field_count(decoded) == 2);
    CHECK(pbw_value_get_string(pbw_message_get(decoded, 1)) != NULL);
    CHECK(strcmp(pbw_value_get_string(pbw_message_get(decoded, 1)), "") == 0);
    CHECK(pbw_value_get_int32(pbw_message_get(decoded, 2)) == 0);
    pbw_free(buf);
    pbw_message_free(decoded);
    pbw_message_free(msg);

    /* nothing set -> nothing on the wire, nothing after decode */
    msg = pbw_message_new(type);
    CHECK_OK(pbw_encode(msg, &buf, &len));
    CHECK(len == 0);
    CHECK(buf != NULL);
    CHECK_OK(pbw_decode(type, buf, 0, &decoded));
    CHECK(pbw_message_field_count(decoded) == 0);
    pbw_free(buf);
    pbw_message_free(decoded);
    pbw_message_free(msg);
}

/* Output is always in ascending field-number order, whatever order the caller
 * set the values in. */
static void
test_field_order(pbw_schema *schema)
{
    const pbw_descriptor *type = desc(schema, "Scalar");
    pbw_message *msg = pbw_message_new(type);
    pbw_message *decoded = NULL;
    pbw_value *value = NULL;
    uint8_t *buf = NULL;
    size_t len = 0;
    size_t i;
    uint32_t last = 0;

    CHECK_OK(pbw_message_set(msg, 16, pbw_value_new_int(2)));
    CHECK_OK(pbw_message_set(msg, 2, pbw_value_new_int(1)));
    CHECK_OK(pbw_message_set(msg, 9, pbw_value_new_int(-2)));
    CHECK_OK(pbw_encode(msg, &buf, &len));
    CHECK_OK(pbw_decode(type, buf, len, &decoded));
    for (i = 0; i < pbw_message_field_count(decoded); i++) {
        const pbw_field *field = pbw_message_field_at(decoded, i, &value);

        CHECK(field != NULL);
        if (field != NULL) {
            CHECK(pbw_field_number(field) > last);
            last = pbw_field_number(field);
        }
    }
    pbw_free(buf);
    pbw_message_free(decoded);
    pbw_message_free(msg);
}

/* The schema-wide name index: adding many descriptors must rebuild it (it
 * doubles) and every name must stay findable, with a clean miss for an
 * unknown one. Also the fallback scan (no index) must agree with it. */
static void
test_schema_name_index(void)
{
    enum { N = 200 };
    pbw_schema *schema = pbw_schema_new();
    char name[32];
    int i;

    CHECK(schema != NULL);
    for (i = 0; i < N; i++) {
        snprintf(name, sizeof(name), "pkg.M%d", i);
        CHECK(pbw_schema_add_message(schema, name) != NULL);
    }
    CHECK(pbw_schema_add_message(schema, "pkg.M7") == NULL); /* duplicate */
    CHECK_OK(pbw_schema_link(schema));
    for (i = 0; i < N; i++) {
        pbw_descriptor *found;

        snprintf(name, sizeof(name), "pkg.M%d", i);
        found = pbw_schema_find(schema, name);
        CHECK(found != NULL && strcmp(pbw_descriptor_name(found), name) == 0);
    }
    CHECK(pbw_schema_find(schema, "pkg.M999") == NULL);
    CHECK(pbw_schema_find(schema, "pkg.M1x") == NULL);
    CHECK(pbw_schema_find(schema, "") == NULL);
    pbw_schema_free(schema);
}

/* Duplicate occurrences: scalars last-one-wins, singular messages merge,
 * oneof members replace each other, repeated fields append. */
static void
test_duplicate_occurrences(pbw_schema *schema)
{
    const pbw_descriptor *person = desc(schema, "Person");
    const pbw_descriptor *parent = desc(schema, "Parent");
    const pbw_descriptor *oneof = desc(schema, "OneOf");
    pbw_message *decoded = NULL;

    /* name "a" then name "b" */
    {
        static const uint8_t wire[] = {0x0a, 0x01, 'a', 0x0a, 0x01, 'b'};
        CHECK_OK(pbw_decode(person, wire, sizeof(wire), &decoded));
        CHECK(strcmp(pbw_value_get_string(pbw_message_get(decoded, 1)), "b") == 0);
        pbw_message_free(decoded);
    }
    /* singular message: two occurrences merge (name from the first, id from the
     * second) instead of replacing each other */
    {
        static const uint8_t wire[] = {0x0a, 0x03, 0x0a, 0x01, 'k',
                                       0x0a, 0x02, 0x10, 0x2a};
        CHECK_OK(pbw_decode(parent, wire, sizeof(wire), &decoded));
        {
            pbw_message *merged =
                pbw_value_get_message(pbw_message_get(decoded, 1));
            CHECK(merged != NULL);
            CHECK(strcmp(pbw_value_get_string(pbw_message_get(merged, 1)), "k") ==
                  0);
            CHECK(pbw_value_get_int32(pbw_message_get(merged, 2)) == 42);
        }
        pbw_message_free(decoded);
    }
    /* merged singular message followed by another outer field: the outer
     * decode keeps going after the merge (one-byte-tag fast path). */
    {
        static const uint8_t wire[] = {0x0a, 0x03, 0x0a, 0x01, 'k',
                                       0x0a, 0x02, 0x10, 0x2a, 0x10, 0x07};
        CHECK_OK(pbw_decode(parent, wire, sizeof(wire), &decoded));
        {
            pbw_message *merged =
                pbw_value_get_message(pbw_message_get(decoded, 1));
            CHECK(merged != NULL);
            CHECK(strcmp(pbw_value_get_string(pbw_message_get(merged, 1)), "k") ==
                  0);
            CHECK(pbw_value_get_int32(pbw_message_get(merged, 2)) == 42);
        }
        CHECK(pbw_value_count(pbw_message_get(decoded, 2)) == 1);
        CHECK(pbw_value_get_int32(pbw_value_at(pbw_message_get(decoded, 2), 0)) ==
              7);
        pbw_message_free(decoded);
    }
    {
        static const uint8_t wire[] = {0x0a, 0x01, 'x', 0x10, 0x05};
        CHECK_OK(pbw_decode(oneof, wire, sizeof(wire), &decoded));
        CHECK(pbw_message_get(decoded, 1) == NULL);
        CHECK(pbw_value_get_int32(pbw_message_get(decoded, 2)) == 5);
        pbw_message_free(decoded);
    }
    /* repeated: three occurrences -> three elements */
    {
        static const uint8_t wire[] = {0x10, 0x01, 0x10, 0x02, 0x10, 0x03};
        CHECK_OK(pbw_decode(parent, wire, sizeof(wire), &decoded));
        CHECK(pbw_value_count(pbw_message_get(decoded, 2)) == 3);
        CHECK(pbw_value_get_int32(pbw_value_at(pbw_message_get(decoded, 2), 2)) ==
              3);
        pbw_message_free(decoded);
    }
}

/* Legacy groups are skipped, never decoded. Nesting is bounded by the same
 * depth limit as messages, and an END_GROUP for the wrong field number is
 * invalid. */
static void
test_groups(pbw_schema *schema)
{
    const pbw_descriptor *person = desc(schema, "Person");
    pbw_message *decoded = NULL;

    /* group 1 containing group 1 containing field 1: everything unknown */
    {
        static const uint8_t nested[] = {0x0b, 0x0b, 0x08, 0x01, 0x0c, 0x0c};
        CHECK_OK(pbw_decode(person, nested, sizeof(nested), &decoded));
        CHECK(pbw_message_field_count(decoded) == 0);
        pbw_message_free(decoded);
    }
    /* a known field number inside a group is skipped with the group */
    {
        static const uint8_t inside[] = {0x0b, 0x0a, 0x01, 'x', 0x0c};
        CHECK_OK(pbw_decode(person, inside, sizeof(inside), &decoded));
        CHECK(pbw_message_field_count(decoded) == 0);
        pbw_message_free(decoded);
    }
    /* END_GROUP for a different field number */
    {
        static const uint8_t mismatched[] = {0x0b, 0x14};
        CHECK_STATUS(PBW_ERR_INVALID,
                     pbw_decode(person, mismatched, sizeof(mismatched),
                                &decoded));
    }
    /* stray END_GROUP at the top level */
    {
        static const uint8_t stray[] = {0x0c};
        CHECK_STATUS(PBW_ERR_INVALID,
                     pbw_decode(person, stray, sizeof(stray), &decoded));
    }
    /* deep group nesting hits the depth limit instead of the stack */
    {
        uint8_t *wire = (uint8_t *)calloc(PBW_GROUP_DEPTH * 2, 1);
        size_t i;

        CHECK(wire != NULL);
        for (i = 0; i < PBW_GROUP_DEPTH; i++) {
            wire[i] = 0x0b;
            wire[PBW_GROUP_DEPTH * 2 - 1 - i] = 0x0c;
        }
        CHECK_STATUS(PBW_ERR_LIMIT,
                     pbw_decode(person, wire, PBW_GROUP_DEPTH * 2, &decoded));
        free(wire);
    }
}

/* A message made only of unknown fields decodes to a message with no fields and
 * re-encodes to exactly the same bytes. */
static void
test_unknown_only(pbw_schema *schema)
{
    const pbw_descriptor *person = desc(schema, "Person");
    static const uint8_t wire[] = {0x98, 0x06, 0x05, 0x9a, 0x06, 0x02, 'x', 'y',
                                   0x9d, 0x06, 0x01, 0x02, 0x03, 0x04};
    pbw_message *decoded = NULL;
    uint8_t *buf = NULL;
    size_t len = 0;

    CHECK_OK(pbw_decode(person, wire, sizeof(wire), &decoded));
    CHECK(pbw_message_field_count(decoded) == 0);
    CHECK_OK(pbw_encode(decoded, &buf, &len));
    CHECK(len == sizeof(wire));
    if (len == sizeof(wire)) {
        CHECK(memcmp(buf, wire, len) == 0);
    }
    pbw_free(buf);
    pbw_message_free(decoded);

    /* the context path keeps unknown fields too, in arena memory */
    {
        pbw_ctx *ctx = pbw_ctx_new();
        const uint8_t *encoded = NULL;
        size_t encoded_len = 0;

        CHECK(ctx != NULL);
        if (ctx != NULL) {
            CHECK_OK(pbw_decode_ctx(ctx, person, wire, sizeof(wire), &decoded));
            CHECK_OK(pbw_encode_ctx(ctx, decoded, &encoded, &encoded_len));
            CHECK(encoded_len == sizeof(wire));
            CHECK(encoded != NULL && memcmp(encoded, wire, sizeof(wire)) == 0);
            /* Empty input through a context decodes to an empty message. */
            {
                pbw_message *empty = NULL;

                CHECK_OK(pbw_decode_ctx(ctx, person, NULL, 0, &empty));
                CHECK(empty != NULL);
            }
            pbw_ctx_free(ctx);
        }
    }
}

/* Every prefix of a valid message must fail cleanly or decode; the last byte
 * missing must fail. Same for single-byte mutations: never crash. */
static void
test_truncation_and_mutation(pbw_schema *schema)
{
    const pbw_descriptor *type = desc(schema, "Parent");
    pbw_message *msg = pbw_message_new(type);
    pbw_message *child = pbw_message_set_message(msg, 1);
    pbw_message *decoded = NULL;
    uint8_t *wire = NULL;
    size_t len = 0;
    size_t i;

    CHECK_OK(pbw_message_set(child, 1, pbw_value_new_string("first")));
    CHECK_OK(pbw_message_set(msg, 2, pbw_value_new_int(1)));
    CHECK_OK(pbw_message_set(msg, 2, pbw_value_new_int(-5)));
    CHECK_OK(pbw_message_set(msg, 3, pbw_value_new_message(pbw_message_new(desc(schema, "Child")))));
    CHECK_OK(pbw_encode(msg, &wire, &len));
    CHECK(len > 8);

    /* the whole thing decodes, one byte less does not */
    CHECK_OK(pbw_decode(type, wire, len, &decoded));
    pbw_message_free(decoded);
    CHECK_STATUS(PBW_ERR_INVALID, pbw_decode(type, wire, len - 1, &decoded));

    for (i = 0; i < len; i++) {
        /* truncation: any outcome except a crash is acceptable */
        if (pbw_decode(type, wire, i, &decoded) == PBW_OK) {
            pbw_message_free(decoded);
        }
    }
    for (i = 0; i < len; i++) {
        static const uint8_t replacements[] = {0x00, 0xff, 0x80};
        size_t r;

        for (r = 0; r < sizeof(replacements); r++) {
            wire[i] = replacements[r];
            if (pbw_decode(type, wire, len, &decoded) == PBW_OK) {
                pbw_message_free(decoded);
            }
        }
        /* also the high-bit flip of the original byte */
        wire[i] = (uint8_t)(wire[i] ^ 0x80);
        if (pbw_decode(type, wire, len, &decoded) == PBW_OK) {
            pbw_message_free(decoded);
        }
    }
    pbw_free(wire);
    pbw_message_free(msg);
}

/* Payloads that need multi-byte lengths and repeat lists that outgrow their
 * initial capacity. */
static void
test_large_payloads(pbw_schema *schema)
{
    const pbw_descriptor *person = desc(schema, "Person");
    const pbw_descriptor *parent = desc(schema, "Parent");
    const size_t size = 70000;
    char *text = (char *)malloc(size + 1);
    pbw_message *msg;
    pbw_message *decoded = NULL;
    uint8_t *wire = NULL;
    size_t len = 0;
    size_t i;

    CHECK(text != NULL);
    memset(text, 'z', size);
    text[size] = '\0';

    msg = pbw_message_new(person);
    CHECK_OK(pbw_message_set(msg, 1, pbw_value_new_bytes(text, size)));
    check_encoded_size(msg);
    CHECK_OK(pbw_encode(msg, &wire, &len));
    CHECK(len > size);
    CHECK_OK(pbw_decode(person, wire, len, &decoded));
    {
        size_t got = 0;
        const void *data = pbw_value_get_bytes(pbw_message_get(decoded, 1), &got);

        CHECK(got == size);
        CHECK(data != NULL && memcmp(data, text, size) == 0);
    }
    pbw_free(wire);
    pbw_message_free(decoded);
    pbw_message_free(msg);

    /* 2000 repeated elements: list growth, and a message over 4 KB */
    msg = pbw_message_new(parent);
    for (i = 0; i < 2000; i++) {
        CHECK_OK(pbw_message_set(msg, 2, pbw_value_new_int((int64_t)i)));
    }
    CHECK_OK(pbw_encode(msg, &wire, &len));
    CHECK(len > 2000);
    CHECK_OK(pbw_decode(parent, wire, len, &decoded));
    CHECK(pbw_value_count(pbw_message_get(decoded, 2)) == 2000);
    CHECK(pbw_value_get_int32(pbw_value_at(pbw_message_get(decoded, 2), 1999)) ==
          1999);
    pbw_free(wire);
    pbw_message_free(decoded);
    pbw_message_free(msg);
    free(text);
}

/* The encoder writes a one-byte length placeholder and patches it afterwards,
 * shifting the payload when the length needs more bytes. These are the sizes
 * where the length varint grows: 127/128 and 16383/16384. */
static void
test_length_patch_boundaries(pbw_schema *schema)
{
    const pbw_descriptor *type = desc(schema, "Parent");
    const size_t sizes[] = {0, 1, 120, 125, 126, 127, 128, 129,
                            16380, 16383, 16384, 16385};
    size_t i;

    for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        size_t size = sizes[i];
        pbw_message *msg = pbw_message_new(type);
        pbw_message *child = pbw_message_set_message(msg, 1);
        pbw_message *decoded = NULL;
        char *buffer = (char *)malloc(size + 1);
        size_t len = 0;
        uint8_t *wire = NULL;

        CHECK(buffer != NULL);
        memset(buffer, 'x', size);
        buffer[size] = '\0';
        CHECK_OK(pbw_message_set(child, 1, pbw_value_new_string(buffer)));
        CHECK_OK(pbw_encode(msg, &wire, &len));
        /* nested payload = name field with its own tag and length */
        CHECK(len > size);
        CHECK_OK(pbw_decode(type, wire, len, &decoded));
        CHECK(decoded != NULL);
        if (decoded != NULL) {
            pbw_message *back = pbw_value_get_message(pbw_message_get(decoded, 1));
            const char *name = pbw_value_get_string(pbw_message_get(back, 1));
            CHECK(back != NULL);
            CHECK(name != NULL && strlen(name) == size);
            CHECK(name != NULL && memcmp(name, buffer, size) == 0);
            pbw_message_free(decoded);
        }
        pbw_free(wire);
        pbw_message_free(msg);
        free(buffer);
    }
}

/* Arena-built trees (pbw_message_new_ctx + pbw_value_new_*_ctx) encode
 * exactly like heap trees, and a decode seals the arena read-only. */
static void
test_ctx_build(pbw_schema *schema)
{
    const pbw_descriptor *parent = desc(schema, "Parent");
    pbw_ctx *ctx = pbw_ctx_new();
    pbw_message *msg;
    pbw_message *kid;
    pbw_message *decoded = NULL;
    const uint8_t *bytes = NULL;
    size_t len = 0;

    CHECK(ctx != NULL);
    pbw_ctx_reset(ctx);
    msg = pbw_message_new_ctx(ctx, parent);
    CHECK(msg != NULL);
    CHECK(pbw_message_set_message(msg, 1) != NULL);
    CHECK_OK(pbw_message_set(pbw_value_get_message(pbw_message_get(msg, 1)), 2, pbw_value_new_uint_ctx(ctx, 7)));
    CHECK_OK(pbw_message_set(msg, 2, pbw_value_new_uint_ctx(ctx, 1)));
    CHECK_OK(pbw_message_set(msg, 2, pbw_value_new_uint_ctx(ctx, 2)));
    kid = pbw_message_set_message(msg, 3);
    CHECK(kid != NULL);
    CHECK_OK(pbw_message_set(kid, 1, pbw_value_new_bytes_ctx(ctx, "x", 1)));
    CHECK(pbw_message_is_initialized(msg));
    CHECK_STATUS(PBW_OK, pbw_encode_ctx(ctx, msg, &bytes, &len));
    CHECK(len > 0);

    /* Same bytes survive a decode roundtrip. */
    pbw_ctx_reset(ctx);
    CHECK_STATUS(PBW_OK, pbw_decode_ctx(ctx, parent, bytes, len, &decoded));
    CHECK(decoded != NULL);
    CHECK(pbw_value_get_int32(pbw_message_get(pbw_value_get_message(pbw_message_get(decoded, 1)), 2)) == 7);
    CHECK(pbw_value_count(pbw_message_get(decoded, 2)) == 2);

    /* Sealed: building into a decoded arena fails. */
    {
        pbw_value *v = pbw_value_new_uint(9);

        CHECK(pbw_message_set(decoded, 2, v) == PBW_ERR_INVALID);
        pbw_value_free(v);
    }
    pbw_ctx_free(ctx);
}

static void
test_ctx(pbw_schema *schema)
{
    const pbw_descriptor *person = desc(schema, "Person");
    const pbw_descriptor *parent = desc(schema, "Parent");
    pbw_ctx *ctx = pbw_ctx_new();
    pbw_message *msg = pbw_message_new(person);
    pbw_message *decoded = NULL;
    uint8_t *heap_bytes = NULL;
    size_t heap_len = 0;
    const uint8_t *ctx_bytes = NULL;
    size_t ctx_len = 0;
    size_t used_before;

    CHECK(ctx != NULL);
    /* Binding slot: set once, preserved by reset, freed with the context. */
    CHECK(pbw_ctx_user(ctx) == NULL);
    pbw_ctx_set_user(ctx, &heap_len);
    CHECK(pbw_ctx_user(ctx) == &heap_len);
    pbw_ctx_reset(ctx);
    CHECK(pbw_ctx_user(ctx) == &heap_len);
    pbw_ctx_set_user(ctx, NULL);
    CHECK_OK(pbw_message_set(msg, 1, pbw_value_new_string("abc")));
    CHECK_OK(pbw_message_set(msg, 2, pbw_value_new_int(7)));
    CHECK_OK(pbw_encode(msg, &heap_bytes, &heap_len));

    /* same bytes as the plain encoder, into the context buffer */
    CHECK_STATUS(
        PBW_OK, pbw_encode_ctx(ctx, msg, &ctx_bytes, &ctx_len));
    CHECK(ctx_len == heap_len);
    CHECK(ctx_bytes != NULL && memcmp(ctx_bytes, heap_bytes, heap_len) == 0);

    /* decode owns nothing per value: no way to observe that directly, so check
     * that freeing is a no-op and the values stay readable */
    CHECK_STATUS(PBW_OK, pbw_decode_ctx(ctx, person, heap_bytes, heap_len,
                                        &decoded));
    CHECK(decoded != NULL);
    CHECK(strcmp(pbw_value_get_string(pbw_message_get(decoded, 1)), "abc") == 0);
    CHECK(pbw_value_get_int32(pbw_message_get(decoded, 2)) == 7);
    pbw_ctx_reset(ctx); /* would invalidate decoded; free must not crash on it */
    pbw_message_free(decoded);

    /* read-only: setters refuse instead of storing a value the arena cannot own */
    CHECK_STATUS(PBW_OK,
                 pbw_decode_ctx(ctx, person, heap_bytes, heap_len, &decoded));
    {
        pbw_value *rejected = pbw_value_new_int(1);
        CHECK_STATUS(PBW_ERR_INVALID, pbw_message_set(decoded, 2, rejected));
        pbw_value_free(rejected); /* a rejected value stays owned by the caller */
    }
    CHECK(pbw_message_set_message(decoded, 2) == NULL);

    /* repeated + nested values grow out of the same arena */
    {
        pbw_message *src = pbw_message_new(parent);
        pbw_message *nested = pbw_message_set_message(src, 1);
        pbw_message *kid;
        uint8_t *wire = NULL;
        size_t len = 0;
        size_t i;

        CHECK_OK(pbw_message_set(nested, 1, pbw_value_new_string("kid")));
        for (i = 0; i < 40; i++) {
            CHECK_OK(pbw_message_set(src, 2, pbw_value_new_int((int64_t)i)));
            kid = pbw_message_set_message(src, 3);
            CHECK(kid != NULL);
            CHECK_OK(pbw_message_set(kid, 1, pbw_value_new_string("k")));
        }
        CHECK_OK(pbw_encode(src, &wire, &len));
        pbw_ctx_reset(ctx);
        CHECK_STATUS(PBW_OK, pbw_decode_ctx(ctx, parent, wire, len, &decoded));
        /* Every arena allocation is 8-byte aligned: on ILP32 the chunk header
         * is 12 bytes, so a payload that simply follows it would put these 4
         * bytes off and fault where a 64-bit slot needs 8. */
        CHECK(((uintptr_t)decoded & 7u) == 0);
        CHECK(((uintptr_t)pbw_value_at(pbw_message_get(decoded, 2), 0) & 7u) ==
              0);
        CHECK(pbw_value_count(pbw_message_get(decoded, 2)) == 40);
        CHECK(pbw_value_count(pbw_message_get(decoded, 3)) == 40);
        CHECK(pbw_value_get_int32(
                  pbw_value_at(pbw_message_get(decoded, 2), 39)) == 39);
        pbw_free(wire);
        pbw_message_free(src);
    }

    /* reset recycles instead of leaking: the same workload must not grow */
    pbw_ctx_reset(ctx);
    used_before = pbw_ctx_used(ctx);
    CHECK(used_before == 0);
    CHECK_STATUS(PBW_OK,
                 pbw_decode_ctx(ctx, person, heap_bytes, heap_len, &decoded));
    CHECK(((uintptr_t)decoded & 7u) == 0);
    CHECK(pbw_ctx_used(ctx) > 0);
    pbw_ctx_reset(ctx);
    CHECK(pbw_ctx_used(ctx) == 0);

    pbw_free(heap_bytes);
    pbw_message_free(msg);
    pbw_ctx_free(ctx);
}

/* Retain cap: an outlier must not pin the context, and cap 0 keeps nothing.
 * Resident size is invisible to the public API, so this peeks at the arena
 * head (a pbw_ctx starts with its pbw_arena). */
static size_t
test_head_cap(pbw_ctx *ctx)
{
    const pbw_arena *arena = (const pbw_arena *)(const void *)ctx;

    return arena->head != NULL ? arena->head->cap : 0;
}

static void
test_ctx_retain_cap(pbw_schema *schema)
{
    const pbw_descriptor *person = desc(schema, "Person");
    pbw_ctx *ctx = pbw_ctx_new();
    pbw_message *msg;
    pbw_message *decoded = NULL;
    uint8_t *big_wire = NULL;
    size_t big_len = 0;
    const uint8_t *out = NULL;
    size_t out_len = 0;

    CHECK(ctx != NULL);
    msg = pbw_message_new(person);
    CHECK(msg != NULL);
    {
        /* One 2 MiB string: a single allocation above the default budget. */
        size_t n = (size_t)2 << 20;
        char *blob = (char *)malloc(n);

        CHECK(blob != NULL);
        memset(blob, 'x', n - 1);
        blob[n - 1] = '\0';
        CHECK_OK(pbw_message_set(msg, 1, pbw_value_new_string(blob)));
        free(blob);
    }
    CHECK_OK(pbw_encode(msg, &big_wire, &big_len));
    pbw_message_free(msg);

    /* Default budget: the outlier bypasses the reusable chunk, so reset
     * leaves the head small instead of pinning 2 MiB. */
    CHECK_STATUS(PBW_OK,
                 pbw_decode_ctx(ctx, person, big_wire, big_len, &decoded));
    CHECK(decoded != NULL);
    CHECK(decoded->wire_len == big_len); /* load-time size for encode */
    CHECK(pbw_ctx_used(ctx) > ((size_t)2 << 20));
    pbw_ctx_reset(ctx);
    CHECK(test_head_cap(ctx) <= PBW_ARENA_LARGE_THRESHOLD);
    {
        /* Same through the plain path. */
        pbw_message *plain = NULL;

        CHECK_STATUS(PBW_OK, pbw_decode(person, big_wire, big_len, &plain));
        CHECK(plain != NULL && plain->wire_len == big_len);
        pbw_message_free(plain);
    }

    /* A small budget drops even a 4 KiB head on reset, and decoding still
     * works afterwards. */
    pbw_ctx_set_retain_cap(ctx, 1024);
    CHECK_STATUS(PBW_OK,
                 pbw_decode_ctx(ctx, person, big_wire, big_len, &decoded));
    pbw_ctx_reset(ctx);
    CHECK(test_head_cap(ctx) <= 1024);
    CHECK_STATUS(PBW_OK,
                 pbw_decode_ctx(ctx, person, big_wire, big_len, &decoded));
    CHECK(decoded != NULL);
    CHECK(pbw_message_get(decoded, 1) != NULL);
    pbw_ctx_reset(ctx);

    /* Cap 0 keeps nothing but stays functional, for decode and encode. */
    pbw_ctx_set_retain_cap(ctx, 0);
    CHECK_STATUS(PBW_OK,
                 pbw_decode_ctx(ctx, person, big_wire, big_len, &decoded));
    CHECK(decoded != NULL);
    CHECK(strlen(pbw_value_get_string(pbw_message_get(decoded, 1))) ==
          ((size_t)2 << 20) - 1);
    pbw_ctx_reset(ctx);
    CHECK(test_head_cap(ctx) == 0);
    msg = pbw_message_new(person);
    CHECK(msg != NULL);
    CHECK_OK(pbw_message_set(msg, 1, pbw_value_new_string("ok")));
    CHECK_STATUS(PBW_OK, pbw_encode_ctx(ctx, msg, &out, &out_len));
    CHECK(out_len > 0);
    pbw_message_free(msg);

    pbw_free(big_wire);
    pbw_ctx_free(ctx);
}

static void
test_ctx_pool(pbw_schema *schema)
{
    const pbw_descriptor *person = desc(schema, "Person");
    pbw_pool *pool = pbw_pool_new(2, 0);
    pbw_message *msg = pbw_message_new(person);
    pbw_message *decoded = NULL;
    uint8_t *wire = NULL;
    size_t len = 0;
    pbw_ctx *a;
    pbw_ctx *b;
    pbw_ctx *c;

    CHECK(pool != NULL);
    CHECK(pbw_pool_idle(pool) == 0);
    CHECK_OK(pbw_message_set(msg, 1, pbw_value_new_string("pooled")));
    CHECK_OK(pbw_encode(msg, &wire, &len));
    pbw_message_free(msg);

    /* acquire gives distinct contexts; release resets, so reuse needs no
     * reset and the idle count is observable instead of printed. */
    a = pbw_pool_acquire(pool);
    b = pbw_pool_acquire(pool);
    CHECK(a != NULL && b != NULL && a != b);
    CHECK(pbw_pool_idle(pool) == 0);
    CHECK_STATUS(PBW_OK, pbw_decode_ctx(a, person, wire, len, &decoded));
    CHECK(decoded != NULL);
    pbw_pool_release(pool, a);
    CHECK(pbw_pool_idle(pool) == 1);
    c = pbw_pool_acquire(pool);
    CHECK(c == a); /* freelist reuse, already reset */
    CHECK_STATUS(PBW_OK, pbw_decode_ctx(c, person, wire, len, &decoded));
    CHECK(decoded != NULL);
    CHECK(strcmp(pbw_value_get_string(pbw_message_get(decoded, 1)),
                 "pooled") == 0);

    /* releases past max_idle are freed, not kept */
    pbw_pool_release(pool, b);
    pbw_pool_release(pool, c);
    CHECK(pbw_pool_idle(pool) == 2);
    a = pbw_pool_acquire(pool);
    b = pbw_pool_acquire(pool);
    c = pbw_pool_acquire(pool); /* pool empty: fresh context */
    CHECK(a != NULL && b != NULL && c != NULL);
    pbw_pool_release(pool, a);
    pbw_pool_release(pool, b);
    pbw_pool_release(pool, c);
    CHECK(pbw_pool_idle(pool) == 2);

    /* NULL-safe, and max_idle 0 degenerates to new/free */
    CHECK(pbw_pool_acquire(NULL) == NULL);
    CHECK(pbw_pool_idle(NULL) == 0);
    pbw_pool_release(NULL, NULL);
    pbw_pool_release(pool, NULL);
    {
        pbw_pool *none = pbw_pool_new(0, 0);
        pbw_ctx *one;

        CHECK(none != NULL);
        one = pbw_pool_acquire(none);
        CHECK(one != NULL);
        CHECK_STATUS(PBW_OK,
                     pbw_decode_ctx(one, person, wire, len, &decoded));
        pbw_pool_release(none, one);
        CHECK(pbw_pool_idle(none) == 0);
        pbw_pool_free(none);
    }

    pbw_free(wire);
    pbw_pool_free(pool);
    pbw_pool_free(NULL);
}

static void
test_encode_to(pbw_schema *schema)
{
    const pbw_descriptor *type = desc(schema, "Scalar");
    pbw_message *msg = make_scalars(schema);
    pbw_message *decoded = NULL;
    uint8_t *wire = NULL;
    size_t wire_len = 0;
    uint8_t *buf = NULL;
    size_t len = 0;
    size_t size = 0;

    CHECK(msg != NULL);
    CHECK(pbw_message_wire_size(msg) == 0);
    CHECK_OK(pbw_encode(msg, &wire, &wire_len));
    CHECK_OK(pbw_message_encoded_size(msg, &size));
    CHECK(size == wire_len);
    CHECK_STATUS(PBW_ERR_INVALID, pbw_message_encoded_size(msg, NULL));
    CHECK_STATUS(PBW_ERR_INVALID, pbw_message_encoded_size(NULL, &size));

    /* The caller buffer needs slack: the writer over-reserves tag/varint space,
     * so exactly PBW_ENCODE_SLACK past the encoded size is the smallest cap that
     * fits, and nothing past the message is ever written. */
    buf = (uint8_t *)malloc(wire_len + PBW_ENCODE_SLACK);
    CHECK(buf != NULL);
    if (buf != NULL) {
        memset(buf, 0xaa, wire_len + PBW_ENCODE_SLACK);
        CHECK_OK(pbw_encode_to(msg, buf, wire_len + PBW_ENCODE_SLACK, &len));
        CHECK(len == wire_len);
        CHECK(memcmp(buf, wire, wire_len) == 0);
        CHECK(buf[wire_len + PBW_ENCODE_SLACK - 1] == 0xaa);
        CHECK_STATUS(PBW_ERR_LIMIT, pbw_encode_to(msg, buf, 4, &len));
        CHECK(len == 0); /* *len is written on the failure path too */
        CHECK_STATUS(PBW_ERR_LIMIT, pbw_encode_to(msg, buf, wire_len, &len));
        CHECK(len == 0);
        CHECK_STATUS(PBW_ERR_INVALID, pbw_encode_to(msg, NULL, wire_len + 16, &len));
        CHECK_STATUS(PBW_ERR_INVALID, pbw_encode_to(NULL, buf, wire_len + 16, &len));
        CHECK_STATUS(PBW_ERR_INVALID, pbw_encode_to(msg, buf, wire_len + 16, NULL));
        free(buf);
    }

    /* An empty message encodes to nothing and needs no buffer. */
    {
        pbw_message *empty = pbw_message_new(type);

        CHECK(empty != NULL);
        CHECK_OK(pbw_message_encoded_size(empty, &size));
        CHECK(size == 0);
        CHECK_OK(pbw_encode_to(empty, NULL, 0, &len));
        CHECK(len == 0);
        pbw_message_free(empty);
    }

    /* A decoded tree reports the wire length it came from. */
    CHECK_OK(pbw_decode(type, wire, wire_len, &decoded));
    CHECK(decoded != NULL);
    CHECK(pbw_message_wire_size(decoded) == wire_len);
    CHECK_OK(pbw_message_encoded_size(decoded, &size));
    CHECK(size == wire_len);
    buf = (uint8_t *)malloc(wire_len + 16);
    CHECK(buf != NULL);
    if (buf != NULL) {
        CHECK_OK(pbw_encode_to(decoded, buf, wire_len + 16, &len));
        CHECK(len == wire_len);
        CHECK(memcmp(buf, wire, wire_len) == 0);
        free(buf);
    }
    CHECK(pbw_message_wire_size(NULL) == 0);

    pbw_message_free(decoded);
    pbw_message_free(msg);
    pbw_free(wire);
}

static void
test_errors(pbw_schema *schema)
{
    const pbw_descriptor *type = desc(schema, "Person");
    pbw_message *msg = pbw_message_new(type);
    pbw_message *decoded = NULL;
    static const uint8_t truncated[] = {0x0a, 0x03, 'a', 'b'};
    static const uint8_t bad_tag[] = {0x00, 0x00};
    static const uint8_t bad_varint[] = {0x10, 0xff, 0xff, 0xff, 0xff, 0xff,
                                         0xff, 0xff, 0xff, 0xff, 0xff};

    /* A rejected value stays owned by the caller. */
    {
        pbw_value *rejected = pbw_value_new_int(1);
        CHECK_STATUS(PBW_ERR_TYPE, pbw_message_set(msg, 1, rejected));
        pbw_value_free(rejected);
        rejected = pbw_value_new_int(1);
        CHECK_STATUS(PBW_ERR_NOT_FOUND, pbw_message_set(msg, 42, rejected));
        pbw_value_free(rejected);
    }
    CHECK_STATUS(PBW_ERR_INVALID, pbw_encode(msg, NULL, NULL));
    CHECK_STATUS(PBW_ERR_INVALID,
                 pbw_decode(type, truncated, sizeof(truncated), &decoded));
    CHECK_STATUS(PBW_ERR_INVALID,
                 pbw_decode(type, bad_tag, sizeof(bad_tag), &decoded));
    CHECK_STATUS(PBW_ERR_INVALID,
                 pbw_decode(type, bad_varint, sizeof(bad_varint), &decoded));
    pbw_message_free(msg);
}

static void
test_depth_limit(pbw_schema *schema)
{
    const pbw_descriptor *type = desc(schema, "Deep");
    pbw_message *msg = pbw_message_new(type);
    pbw_message *cursor = msg;
    uint8_t *buf = NULL;
    size_t len = 0;
    int i;

    for (i = 0; i < 300; i++) {
        cursor = pbw_message_set_message(cursor, 1);
        CHECK(cursor != NULL);
    }
    CHECK_STATUS(PBW_ERR_LIMIT, pbw_encode(msg, &buf, &len));
    CHECK_STATUS(PBW_ERR_LIMIT, pbw_message_encoded_size(msg, &len));
    pbw_message_free(msg);

    /* Build 300 levels of nested length-delimited frames by hand and decode. */
    {
        pbw_message *decoded = NULL;
        uint8_t *wire = NULL;
        size_t wire_len = 0;
        size_t cap = 0;

        for (i = 0; i < 300; i++) {
            uint8_t header[3];
            size_t header_len;
            size_t need = wire_len + 3;

            if (need > cap) {
                size_t new_cap = cap ? cap * 2 : 64;
                uint8_t *grown = (uint8_t *)realloc(wire, new_cap);
                CHECK(grown != NULL);
                if (grown == NULL) {
                    pbw_free(wire);
                    return;
                }
                wire = grown;
                cap = new_cap;
            }
            header[0] = 0x0a;
            header_len = 1;
            if (wire_len >= 0x80) {
                header[1] = (uint8_t)((wire_len & 0x7f) | 0x80);
                header[2] = (uint8_t)(wire_len >> 7);
                header_len = 3;
            } else {
                header[1] = (uint8_t)wire_len;
                header_len = 2;
            }
            memmove(wire + header_len, wire, wire_len);
            memcpy(wire, header, header_len);
            wire_len += header_len;
        }
        CHECK_STATUS(PBW_ERR_LIMIT, pbw_decode(type, wire, wire_len, &decoded));
        pbw_free(wire);
    }
}

static void
test_enum(pbw_schema *schema)
{
    const pbw_descriptor *color = pbw_schema_find(schema, "Color");
    const char *name = NULL;
    int32_t number = -1;

    CHECK(pbw_descriptor_is_enum(color) == 1);
    CHECK(pbw_enum_find_name(color, 2, &name) == 1);
    CHECK(name != NULL && strcmp(name, "BLUE") == 0);
    CHECK(pbw_enum_find_value(color, "GREEN", &number) == 1);
    CHECK(number == 1);
    CHECK(pbw_enum_find_value(color, "PURPLE", &number) == 0);
    CHECK(pbw_enum_find_name(color, 42, &name) == 0);
}

static void
test_schema_errors(void)
{
    pbw_schema *schema = pbw_schema_new();
    pbw_descriptor *msg = pbw_schema_add_message(schema, "M");
    pbw_field *field = pbw_message_add_field(msg, "ref", 1, PBW_TYPE_MESSAGE,
                                             PBW_LABEL_OPTIONAL);

    CHECK(pbw_message_add_field(msg, "dup", 1, PBW_TYPE_INT32,
                                PBW_LABEL_OPTIONAL) == NULL);
    CHECK_STATUS(PBW_ERR_INVALID, pbw_schema_link(schema));
    CHECK(pbw_schema_error(schema)[0] != '\0');

    pbw_field_set_type_name(field, "M"); /* self reference resolves */
    CHECK_OK(pbw_schema_link(schema));

    pbw_field_set_type_name(field, "Missing");
    CHECK_STATUS(PBW_ERR_NOT_FOUND, pbw_schema_link(schema));
    pbw_schema_free(schema);

    /* oneof index past the field count fails at link */
    schema = pbw_schema_new();
    msg = pbw_schema_add_message(schema, "M");
    field = pbw_message_add_field(msg, "a", 1, PBW_TYPE_INT32,
                                  PBW_LABEL_OPTIONAL);
    CHECK(field != NULL);
    pbw_field_set_oneof(field, 99);
    CHECK_STATUS(PBW_ERR_INVALID, pbw_schema_link(schema));
    pbw_schema_free(schema);

    /* field and descriptor name/number validation */
    schema = pbw_schema_new();
    msg = pbw_schema_add_message(schema, "M");
    CHECK(msg != NULL);
    CHECK(pbw_schema_add_message(schema, "M") == NULL); /* duplicate name */
    CHECK(pbw_message_add_field(msg, "a", 0, PBW_TYPE_INT32,
                                PBW_LABEL_OPTIONAL) == NULL);
    CHECK(pbw_message_add_field(msg, "a", 536870912u, PBW_TYPE_INT32,
                                PBW_LABEL_OPTIONAL) == NULL);
    CHECK(pbw_message_add_field(msg, "a", 1, PBW_TYPE_INT32,
                                PBW_LABEL_OPTIONAL) != NULL);
    CHECK(pbw_message_add_field(msg, "a", 2, PBW_TYPE_INT32,
                                PBW_LABEL_OPTIONAL) == NULL); /* duplicate name */
    CHECK(pbw_message_add_field(msg, "b", 1, PBW_TYPE_INT32,
                                PBW_LABEL_OPTIONAL) == NULL); /* duplicate number */
    CHECK_OK(pbw_schema_link(schema));
    pbw_schema_free(schema);

    /* type kind mismatches */
    schema = pbw_schema_new();
    msg = pbw_schema_add_message(schema, "M");
    pbw_schema_add_enum(schema, "E");
    field = pbw_message_add_field(msg, "as_msg", 1, PBW_TYPE_MESSAGE,
                                  PBW_LABEL_OPTIONAL);
    pbw_field_set_type_name(field, "E");
    CHECK_STATUS(PBW_ERR_TYPE, pbw_schema_link(schema));
    pbw_field_set_type_name(field, "M"); /* message type again: fine */
    CHECK_OK(pbw_schema_link(schema));
    pbw_schema_free(schema);

    /* a message field without a type name cannot be linked */
    schema = pbw_schema_new();
    msg = pbw_schema_add_message(schema, "M");
    pbw_message_add_field(msg, "ref", 1, PBW_TYPE_MESSAGE, PBW_LABEL_OPTIONAL);
    CHECK_STATUS(PBW_ERR_INVALID, pbw_schema_link(schema));
    pbw_schema_free(schema);

    /* groups are real fields: typeless ones still fail, typed ones link */
    schema = pbw_schema_new();
    msg = pbw_schema_add_message(schema, "M");
    pbw_message_add_field(msg, "g", 1, PBW_TYPE_GROUP, PBW_LABEL_OPTIONAL);
    CHECK_STATUS(PBW_ERR_INVALID, pbw_schema_link(schema));
    pbw_schema_free(schema);

    /* packed is only meaningful for repeated numeric fields */
    schema = pbw_schema_new();
    msg = pbw_schema_add_message(schema, "M");
    field = pbw_message_add_field(msg, "s", 1, PBW_TYPE_STRING,
                                  PBW_LABEL_REPEATED);
    pbw_field_set_packed(field, 1);
    CHECK_STATUS(PBW_ERR_INVALID, pbw_schema_link(schema));
    pbw_schema_free(schema);

    /* an unlinked schema cannot decode, and an enum descriptor is not a message */
    schema = pbw_schema_new();
    msg = pbw_schema_add_message(schema, "M");
    pbw_message_add_field(msg, "a", 1, PBW_TYPE_INT32, PBW_LABEL_OPTIONAL);
    {
        pbw_message *decoded = NULL;
        static const uint8_t wire[] = {0x08, 0x01};
        pbw_descriptor *enum_desc =
            pbw_schema_add_enum(schema, "E");

        CHECK_STATUS(PBW_ERR_INVALID,
                     pbw_decode(msg, wire, sizeof(wire), &decoded));
        CHECK(pbw_message_new(enum_desc) == NULL);
        CHECK_STATUS(PBW_ERR_TYPE, pbw_decode(enum_desc, wire, sizeof(wire),
                                              &decoded));
    }
    pbw_schema_free(schema);
}

/* proto2 semantics on a hand-built schema: required enforcement. */
static void
test_proto2_required(void)
{
    pbw_schema *schema = pbw_schema_new();
    pbw_descriptor *msg;
    pbw_message *m;
    pbw_message *decoded;
    uint8_t *bytes;
    size_t len;

    CHECK(schema != NULL);
    pbw_schema_set_proto3(schema, 0);
    msg = pbw_schema_add_message(schema, "Req");
    CHECK(msg != NULL);
    CHECK(pbw_message_add_field(msg, "a", 1, PBW_TYPE_INT32,
                                PBW_LABEL_REQUIRED) != NULL);
    CHECK(pbw_message_add_field(msg, "b", 2, PBW_TYPE_STRING,
                                PBW_LABEL_OPTIONAL) != NULL);
    CHECK_OK(pbw_schema_link(schema));
    /* An empty payload misses the required field. */
    decoded = NULL;
    CHECK_STATUS(PBW_ERR_REQUIRED, pbw_decode(msg, NULL, 0, &decoded));
    CHECK(decoded == NULL);
    /* Only the optional field: still missing. */
    {
        static const uint8_t only_b[] = {0x12, 0x01, 0x78};

        decoded = NULL;
        CHECK_STATUS(PBW_ERR_REQUIRED,
                     pbw_decode(msg, only_b, sizeof(only_b), &decoded));
        CHECK(decoded == NULL);
    }
    /* With the required field set: fine, and the tree reports ready. */
    {
        static const uint8_t full[] = {0x08, 0x05, 0x12, 0x01, 0x78};

        CHECK_OK(pbw_decode(msg, full, sizeof(full), &decoded));
        CHECK(decoded != NULL);
        CHECK(pbw_message_is_initialized(decoded));
        pbw_message_free(decoded);
        decoded = NULL;
    }
    /* A hand-built tree without it is not initialized, but still encodes
     * (the reference serializer writes whatever the tree holds). */
    m = pbw_message_new(msg);
    CHECK(m != NULL);
    CHECK(!pbw_message_is_initialized(m));
    CHECK(pbw_message_set(m, 1, pbw_value_new_int(7)) == PBW_OK);
    CHECK(pbw_message_is_initialized(m));
    bytes = NULL;
    CHECK_OK(pbw_encode(m, &bytes, &len));
    CHECK(len == 2 && bytes[0] == 0x08 && bytes[1] == 0x07);
    pbw_free(bytes);
    pbw_message_free(m);
    pbw_schema_free(schema);
}

/* A closed (proto2) enum diverts unknown numbers to the unknown fields,
 * like the binary decoder; an open enum keeps them as values. */
static void
test_closed_enum(void)
{
    static const uint8_t foreign[] = {0x08, 0xe7, 0x07}; /* field 1 = 999 */
    pbw_schema *schema = pbw_schema_new();
    pbw_descriptor *color;
    pbw_descriptor *msg;
    pbw_field *f;
    pbw_message *decoded;
    char *out;
    size_t len;
    uint8_t *bytes;

    CHECK(schema != NULL);
    pbw_schema_set_proto3(schema, 0);
    color = pbw_schema_add_enum(schema, "E");
    pbw_enum_add_value(color, "E_ZERO", 0);
    pbw_enum_add_value(color, "E_ONE", 1);
    msg = pbw_schema_add_message(schema, "M");
    f = pbw_message_add_field(msg, "e", 1, PBW_TYPE_ENUM,
                              PBW_LABEL_OPTIONAL);
    pbw_field_set_type_name(f, "E");
    CHECK_OK(pbw_schema_link(schema));
    /* Closed: the number is not a value (text shows nothing), but the
     * bytes round-trip through the unknown fields. */
    CHECK_OK(pbw_decode(msg, foreign, sizeof(foreign), &decoded));
    CHECK(decoded != NULL);
    CHECK(pbw_message_get(decoded, 1) == NULL);
    out = NULL;
    CHECK_OK(pbw_text_print_opts(decoded, 0, &out, &len));
    CHECK(len == 0);
    pbw_free(out);
    bytes = NULL;
    CHECK_OK(pbw_encode(decoded, &bytes, &len));
    CHECK(len == sizeof(foreign) && memcmp(bytes, foreign, len) == 0);
    pbw_free(bytes);
    pbw_message_free(decoded);
    decoded = NULL;
    pbw_schema_free(schema);
    /* Open: the same bytes land on the field and print as a number. */
    schema = pbw_schema_new();
    CHECK(schema != NULL);
    color = pbw_schema_add_enum(schema, "E");
    pbw_enum_add_value(color, "E_ZERO", 0);
    pbw_enum_add_value(color, "E_ONE", 1);
    msg = pbw_schema_add_message(schema, "M");
    f = pbw_message_add_field(msg, "e", 1, PBW_TYPE_ENUM,
                              PBW_LABEL_OPTIONAL);
    pbw_field_set_type_name(f, "E");
    CHECK_OK(pbw_schema_link(schema));
    CHECK_OK(pbw_decode(msg, foreign, sizeof(foreign), &decoded));
    CHECK(pbw_message_get(decoded, 1) != NULL);
    out = NULL;
    CHECK_OK(pbw_text_print(decoded, &out, &len));
    CHECK(len == sizeof("e: 999\n") - 1 &&
          memcmp(out, "e: 999\n", len) == 0);
    pbw_free(out);
    pbw_message_free(decoded);
    pbw_schema_free(schema);
    /* Packed bytes for a repeated closed enum: the reference parser accepts
     * the packed form for an unpacked-declared field, keeps known values in
     * the field and moves each unknown value into the unknown set as one
     * unpacked item. Field 2 has a one-byte tag, which routes the packed form
     * through the link-time tag map. */
    {
        static const uint8_t packed[] = {0x12, 0x02, 0xe7,
                                         0x07}; /* rep = [999] */
        static const uint8_t unpacked[] = {0x10, 0xe7,
                                           0x07}; /* rep = 999 */
        static const uint8_t mixed[] = {0x12, 0x04, 0x00, 0xe7, 0x07,
                                        0x01};       /* rep = [0, 999, 1] */
        static const uint8_t empty[] = {0x12, 0x00}; /* rep = [] */
        /* the unknown value keeps the field's varint tag, unpacked */
        static const uint8_t unknown_item[] = {0x10, 0xe7, 0x07};
        /* mixed, re-encoded: known values unpacked, then the unknown item */
        static const uint8_t mixed_out[] = {0x10, 0x00, 0x10, 0x01,
                                            0x10, 0xe7, 0x07};
        pbw_value *list;

        schema = pbw_schema_new();
        CHECK(schema != NULL);
        pbw_schema_set_proto3(schema, 0);
        color = pbw_schema_add_enum(schema, "E");
        pbw_enum_add_value(color, "E_ZERO", 0);
        pbw_enum_add_value(color, "E_ONE", 1);
        msg = pbw_schema_add_message(schema, "M");
        f = pbw_message_add_field(msg, "rep", 2, PBW_TYPE_ENUM,
                                  PBW_LABEL_REPEATED);
        pbw_field_set_type_name(f, "E");
        pbw_field_set_packed(f, 0); /* unpacked: packed bytes still parse */
        CHECK_OK(pbw_schema_link(schema));

        /* one unknown value: the field stays absent, the item is unknown */
        CHECK_OK(pbw_decode(msg, packed, sizeof(packed), &decoded));
        CHECK(pbw_message_get(decoded, 2) == NULL);
        CHECK(pbw_message_unknown_len(decoded) == sizeof(unknown_item));
        CHECK(memcmp(pbw_message_unknown_bytes(decoded), unknown_item,
                     sizeof(unknown_item)) == 0);
        bytes = NULL;
        CHECK_OK(pbw_encode(decoded, &bytes, &len));
        CHECK(len == sizeof(unknown_item) &&
              memcmp(bytes, unknown_item, len) == 0);
        pbw_free(bytes);
        pbw_message_free(decoded);
        decoded = NULL;

        /* the unpacked form of the same value behaves identically */
        CHECK_OK(pbw_decode(msg, unpacked, sizeof(unpacked), &decoded));
        CHECK(pbw_message_get(decoded, 2) == NULL);
        CHECK(pbw_message_unknown_len(decoded) == sizeof(unpacked));
        pbw_message_free(decoded);
        decoded = NULL;

        /* known values land in the field, the unknown one stays out */
        CHECK_OK(pbw_decode(msg, mixed, sizeof(mixed), &decoded));
        list = pbw_message_get(decoded, 2);
        CHECK(list != NULL && pbw_value_get_kind(list) == PBW_VALUE_LIST);
        CHECK(pbw_value_count(list) == 2);
        CHECK(pbw_value_get_int32(pbw_value_at(list, 0)) == 0);
        CHECK(pbw_value_get_int32(pbw_value_at(list, 1)) == 1);
        CHECK(pbw_message_unknown_len(decoded) == sizeof(unknown_item));
        bytes = NULL;
        CHECK_OK(pbw_encode(decoded, &bytes, &len));
        CHECK(len == sizeof(mixed_out) && memcmp(bytes, mixed_out, len) == 0);
        pbw_free(bytes);
        pbw_message_free(decoded);
        decoded = NULL;

        /* two runs: the first all unknown (absent), the second stores values */
        {
            static const uint8_t two_runs[] = {0x12, 0x01, 0x63, 0x12, 0x02,
                                               0x00, 0x01};
            static const uint8_t two_out[] = {0x10, 0x00, 0x10, 0x01, 0x10,
                                              0x63};

            CHECK_OK(pbw_decode(msg, two_runs, sizeof(two_runs), &decoded));
            list = pbw_message_get(decoded, 2);
            CHECK(list != NULL && pbw_value_get_kind(list) == PBW_VALUE_LIST);
            CHECK(pbw_value_count(list) == 2);
            CHECK(pbw_value_get_int32(pbw_value_at(list, 0)) == 0);
            CHECK(pbw_value_get_int32(pbw_value_at(list, 1)) == 1);
            CHECK(pbw_message_unknown_len(decoded) == 2);
            bytes = NULL;
            CHECK_OK(pbw_encode(decoded, &bytes, &len));
            CHECK(len == sizeof(two_out) && memcmp(bytes, two_out, len) == 0);
            pbw_free(bytes);
            pbw_message_free(decoded);
            decoded = NULL;
        }

        /* an empty packed run stays absent and leaves no unknown bytes */
        CHECK_OK(pbw_decode(msg, empty, sizeof(empty), &decoded));
        CHECK(pbw_message_get(decoded, 2) == NULL);
        CHECK(pbw_message_unknown_len(decoded) == 0);
        bytes = NULL;
        CHECK_OK(pbw_encode(decoded, &bytes, &len));
        CHECK(len == 0);
        pbw_free(bytes);
        pbw_message_free(decoded);
        decoded = NULL;
        pbw_schema_free(schema);
    }
}

/* Groups decode into their submessage and print by type name. */
static void
test_group_fields(void)
{
    pbw_schema *schema = pbw_schema_new();
    pbw_descriptor *grp;
    pbw_descriptor *msg;
    pbw_field *f;
    pbw_message *decoded;
    char *out;
    size_t len;
    static const uint8_t wire[] = {0x0b, 0x08, 0x01, 0x0c};

    CHECK(schema != NULL);
    pbw_schema_set_proto3(schema, 0);
    grp = pbw_schema_add_message(schema, "Grp");
    CHECK(grp != NULL);
    CHECK(pbw_message_add_field(grp, "x", 1, PBW_TYPE_INT32,
                                PBW_LABEL_OPTIONAL) != NULL);
    msg = pbw_schema_add_message(schema, "M");
    CHECK(msg != NULL);
    f = pbw_message_add_field(msg, "g", 1, PBW_TYPE_GROUP,
                              PBW_LABEL_OPTIONAL);
    pbw_field_set_type_name(f, "Grp");
    CHECK_OK(pbw_schema_link(schema));
    CHECK_OK(pbw_decode(msg, wire, sizeof(wire), &decoded));
    CHECK(decoded != NULL);
    check_encoded_size(decoded);
    CHECK(pbw_message_get(decoded, 1) != NULL);
    CHECK_OK(pbw_text_print(decoded, &out, &len));
    CHECK(len == sizeof("Grp {\n  x: 1\n}\n") - 1 &&
          memcmp(out, "Grp {\n  x: 1\n}\n", len) == 0);
    pbw_free(out);
    out = NULL;
    pbw_message_free(decoded);
    decoded = NULL;
    /* The type name parses back, lowercase too. */
    CHECK_OK(pbw_text_parse(msg, "Grp { x: 2 }", 12, &decoded));
    {
        uint8_t *bytes = NULL;

        CHECK_OK(pbw_encode(decoded, &bytes, &len));
        CHECK(len == 4 && bytes[0] == 0x0b && bytes[1] == 0x08 &&
              bytes[2] == 0x02 && bytes[3] == 0x0c);
        pbw_free(bytes);
    }
    pbw_message_free(decoded);
    decoded = NULL;
    pbw_schema_free(schema);
    /* A group holding a singular message: the inner message decodes through
     * the slow field path, merges when repeated, and fails the group when
     * truncated. Groups themselves merge when repeated. */
    {
        /* g { sub { a: 1 } sub { a: 2 } }: field 1 START_GROUP, field 2
         * LEN (twice), field 1 END_GROUP */
        static const uint8_t gwire[] = {
            0x0b,
            0x12,
            0x02,
            0x08,
            0x01,
            0x12,
            0x02,
            0x08,
            0x02,
            0x0c,
        };
        /* truncated inner message: LEN says 2, only 1 byte follows */
        static const uint8_t gbad[] = {0x0b, 0x12, 0x02, 0x08, 0x0c};
        /* length overruns the group: only 2 bytes for a 5-byte message */
        static const uint8_t gcut[] = {0x0b, 0x12, 0x05, 0x08, 0x0c};
        /* inner message is a truncated varint */
        static const uint8_t gbad_inner[] = {0x0b, 0x12, 0x01, 0x80,
                                             0x0c};
        /* empty packed run, truncated scalar, unknown enum, valid string */
        static const uint8_t gempty[] = {0x0b, 0x1a, 0x00, 0x0c};
        static const uint8_t gtrunc[] = {0x0b, 0x08, 0x80, 0x0c};
        static const uint8_t gunknown[] = {0x0b, 0x20, 0xe7, 0x07,
                                           0x0c};
        static const uint8_t gstr[] = {0x0b, 0x2a, 0x01, 0x41,
                                       0x0c};
        /* the group twice: contents merge */
        static const uint8_t gdup[] = {
            0x0b,
            0x08,
            0x01,
            0x0c,
            0x0b,
            0x08,
            0x02,
            0x0c,
        };
        /* packed bytes for the group's unpacked run */
        static const uint8_t gpacked[] = {0x0b, 0x1a, 0x02,
                                          0x01, 0x02, 0x0c};
        schema = pbw_schema_new();
        CHECK(schema != NULL);
        pbw_schema_set_proto3(schema, 0);
        grp = pbw_schema_add_message(schema, "Grp2");
        CHECK(grp != NULL);
        CHECK(pbw_message_add_field(grp, "x", 1, PBW_TYPE_INT32,
                                    PBW_LABEL_OPTIONAL) != NULL);
        CHECK(pbw_message_add_field(grp, "s", 5, PBW_TYPE_STRING,
                                    PBW_LABEL_OPTIONAL) != NULL);
        {
            pbw_descriptor *ev = pbw_schema_add_enum(schema, "Ev");
            pbw_field *ef;

            CHECK(ev != NULL);
            pbw_enum_add_value(ev, "EV_ZERO", 0);
            pbw_enum_add_value(ev, "EV_ONE", 1);
            ef = pbw_message_add_field(grp, "e", 4, PBW_TYPE_ENUM,
                                       PBW_LABEL_REPEATED);
            CHECK(ef != NULL);
            if (ef != NULL) {
                pbw_field_set_type_name(ef, "Ev");
                pbw_field_set_packed(ef, 0);
            }
        }
        {
            pbw_field *up = pbw_message_add_field(grp, "up", 3,
                                                  PBW_TYPE_INT32,
                                                  PBW_LABEL_REPEATED);

            CHECK(up != NULL);
            if (up != NULL) {
                pbw_field_set_packed(up, 0);
            }
        }
        msg = pbw_schema_add_message(schema, "Sub2");
        CHECK(msg != NULL);
        CHECK(pbw_message_add_field(msg, "a", 1, PBW_TYPE_INT32,
                                    PBW_LABEL_OPTIONAL) != NULL);
        f = pbw_message_add_field(grp, "sub", 2, PBW_TYPE_MESSAGE,
                                  PBW_LABEL_OPTIONAL);
        pbw_field_set_type_name(f, "Sub2");
        msg = pbw_schema_add_message(schema, "M2");
        CHECK(msg != NULL);
        f = pbw_message_add_field(msg, "g", 1, PBW_TYPE_GROUP,
                                  PBW_LABEL_OPTIONAL);
        pbw_field_set_type_name(f, "Grp2");
        CHECK_OK(pbw_schema_link(schema));
        CHECK_OK(pbw_decode(msg, gwire, sizeof(gwire), &decoded));
        {
            const pbw_value *g = pbw_message_get(decoded, 1);
            const pbw_value *s = NULL;

            CHECK(g != NULL);
            if (g != NULL) {
                s = pbw_message_get(pbw_value_get_message(g), 2);
            }
            CHECK(s != NULL &&
                  pbw_value_get_int32(pbw_message_get(
                      pbw_value_get_message(s), 1)) == 2);
        }
        pbw_message_free(decoded);
        decoded = NULL;
        CHECK(pbw_decode(msg, gbad, sizeof(gbad), &decoded) !=
              PBW_OK);
        CHECK(decoded == NULL);
        CHECK(pbw_decode(msg, gcut, sizeof(gcut), &decoded) !=
              PBW_OK);
        CHECK(decoded == NULL);
        CHECK(pbw_decode(msg, gbad_inner, sizeof(gbad_inner),
                         &decoded) != PBW_OK);
        CHECK(decoded == NULL);
        CHECK_OK(pbw_decode(msg, gempty, sizeof(gempty), &decoded));
        pbw_message_free(decoded);
        decoded = NULL;
        CHECK(pbw_decode(msg, gtrunc, sizeof(gtrunc), &decoded) !=
              PBW_OK);
        CHECK(decoded == NULL);
        CHECK_OK(pbw_decode(msg, gunknown, sizeof(gunknown), &decoded));
        {
            const pbw_value *g = pbw_message_get(decoded, 1);

            CHECK(g != NULL);
            if (g != NULL) {
                CHECK(pbw_message_get(pbw_value_get_message(g), 4) ==
                      NULL);
            }
        }
        pbw_message_free(decoded);
        decoded = NULL;
        CHECK_OK(pbw_decode(msg, gstr, sizeof(gstr), &decoded));
        {
            const pbw_value *g = pbw_message_get(decoded, 1);
            const pbw_value *s = NULL;

            CHECK(g != NULL);
            if (g != NULL) {
                s = pbw_message_get(pbw_value_get_message(g), 5);
            }
            CHECK(s != NULL);
        }
        pbw_message_free(decoded);
        decoded = NULL;
        CHECK_OK(pbw_decode(msg, gpacked, sizeof(gpacked), &decoded));
        {
            const pbw_value *g = pbw_message_get(decoded, 1);
            const pbw_value *u = NULL;

            CHECK(g != NULL);
            if (g != NULL) {
                u = pbw_message_get(pbw_value_get_message(g), 3);
            }
            CHECK(u != NULL);
        }
        pbw_message_free(decoded);
        decoded = NULL;
        CHECK_OK(pbw_decode(msg, gdup, sizeof(gdup), &decoded));
        {
            const pbw_value *g = pbw_message_get(decoded, 1);

            CHECK(g != NULL &&
                  pbw_value_get_int32(pbw_message_get(
                      pbw_value_get_message(g), 1)) == 2);
        }
        pbw_message_free(decoded);
        decoded = NULL;
        pbw_schema_free(schema);
    }
}

/* Extensions attach by number and print under their full name. */
static void
test_hand_built_extensions(void)
{
    pbw_schema *schema = pbw_schema_new();
    pbw_descriptor *msg;
    pbw_field *ext;
    pbw_message *decoded;
    char *out;
    size_t len;
    static const uint8_t wire[] = {0xa0, 0x06, 0x05};

    CHECK(schema != NULL);
    msg = pbw_schema_add_message(schema, "M");
    CHECK(msg != NULL);
    ext = pbw_message_add_extension(msg, "ext_int", 100, PBW_TYPE_INT32,
                                    PBW_LABEL_OPTIONAL);
    CHECK(ext != NULL);
    CHECK(pbw_field_is_extension(ext));
    pbw_field_set_full_name(ext, "t.ext_int");
    CHECK_OK(pbw_schema_link(schema));
    CHECK_OK(pbw_decode(msg, wire, sizeof(wire), &decoded));
    CHECK_OK(pbw_text_print(decoded, &out, &len));
    CHECK(len == sizeof("[t.ext_int]: 5\n") - 1 &&
          memcmp(out, "[t.ext_int]: 5\n", len) == 0);
    pbw_free(out);
    pbw_message_free(decoded);
    pbw_schema_free(schema);
}

/* Text format: printer goldens, accepted/rejected spellings, required
 * fields, unknown-field display. */
/* Every strict prefix of a valid document parses safely: either it is
 * itself valid, or it fails with no message out. */
static void
check_text_prefix_sweep(const pbw_descriptor *msg)
{
    static const char *docs[] = {
        "a: 1 color: C_RED str: \"x\" flag: true h: -2 u: 3",
        "kid { a: 1 } nums: [1, 2] grp { x: 1 } [t.x_int]: 5",
        "subs { a: 1 } subs { a: 2 } o1: 1",
    };
    size_t i;

    for (i = 0; i < sizeof(docs) / sizeof(docs[0]); i++) {
        size_t full = strlen(docs[i]);
        size_t k;

        for (k = 0; k < full; k++) {
            pbw_message *m = NULL;
            pbw_status st = pbw_text_parse(msg, docs[i], k, &m);

            CHECK(st == PBW_OK || m == NULL);
            pbw_message_free(m);
        }
    }
}

static void
test_text_format(void)
{
    pbw_schema *schema = pbw_schema_new();
    pbw_descriptor *color;
    pbw_descriptor *sub;
    pbw_descriptor *grp;
    pbw_descriptor *msg;
    pbw_descriptor *req;
    pbw_field *f;
    pbw_message *decoded;
    char *out;
    size_t len;
    static const char *golden =
        "a: 1\n"
        "blob: \"\\000\\377\\177\"\n"
        "f32: 3.5\n"
        "f64: -2.25\n"
        "color: C_RED\n"
        "kid {\n"
        "  a: 1\n"
        "}\n"
        "nums: 1\n"
        "nums: -2\n"
        "Grp {\n"
        "  x: 1\n"
        "}\n"
        "str: \"a\\nb\\\"c\\\\d\\'e\"\n"
        "[t.x_int]: 5\n";

    CHECK(schema != NULL);
    color = pbw_schema_add_enum(schema, "C");
    pbw_enum_add_value(color, "C_ZERO", 0);
    pbw_enum_add_value(color, "C_RED", 1);
    sub = pbw_schema_add_message(schema, "Sub");
    CHECK(sub != NULL);
    CHECK(pbw_message_add_field(sub, "a", 1, PBW_TYPE_INT32,
                                PBW_LABEL_OPTIONAL) != NULL);
    grp = pbw_schema_add_message(schema, "Grp");
    CHECK(grp != NULL);
    CHECK(pbw_message_add_field(grp, "x", 1, PBW_TYPE_INT32,
                                PBW_LABEL_OPTIONAL) != NULL);
    msg = pbw_schema_add_message(schema, "M");
    CHECK(msg != NULL);
    CHECK(pbw_message_add_field(msg, "a", 1, PBW_TYPE_INT32,
                                PBW_LABEL_OPTIONAL) != NULL);
    CHECK(pbw_message_add_field(msg, "blob", 2, PBW_TYPE_BYTES,
                                PBW_LABEL_OPTIONAL) != NULL);
    CHECK(pbw_message_add_field(msg, "f32", 3, PBW_TYPE_FLOAT,
                                PBW_LABEL_OPTIONAL) != NULL);
    CHECK(pbw_message_add_field(msg, "f64", 4, PBW_TYPE_DOUBLE,
                                PBW_LABEL_OPTIONAL) != NULL);
    f = pbw_message_add_field(msg, "color", 5, PBW_TYPE_ENUM,
                              PBW_LABEL_OPTIONAL);
    pbw_field_set_type_name(f, "C");
    f = pbw_message_add_field(msg, "kid", 6, PBW_TYPE_MESSAGE,
                              PBW_LABEL_OPTIONAL);
    pbw_field_set_type_name(f, "Sub");
    CHECK(pbw_message_add_field(msg, "nums", 7, PBW_TYPE_INT32,
                                PBW_LABEL_REPEATED) != NULL);
    f = pbw_message_add_field(msg, "grp", 8, PBW_TYPE_GROUP,
                              PBW_LABEL_OPTIONAL);
    pbw_field_set_type_name(f, "Grp");
    f = pbw_message_add_extension(msg, "x_int", 100, PBW_TYPE_INT32,
                                  PBW_LABEL_OPTIONAL);
    pbw_field_set_full_name(f, "t.x_int");
    CHECK(pbw_message_add_field(msg, "str", 9, PBW_TYPE_STRING,
                                PBW_LABEL_OPTIONAL) != NULL);
    CHECK(pbw_message_add_field(msg, "flag", 10, PBW_TYPE_BOOL,
                                PBW_LABEL_OPTIONAL) != NULL);
    f = pbw_message_add_field(msg, "subs", 11, PBW_TYPE_MESSAGE,
                              PBW_LABEL_REPEATED);
    pbw_field_set_type_name(f, "Sub");
    f = pbw_message_add_field(msg, "o1", 12, PBW_TYPE_INT32,
                              PBW_LABEL_OPTIONAL);
    pbw_field_set_oneof(f, 1);
    f = pbw_message_add_field(msg, "o2", 13, PBW_TYPE_INT32,
                              PBW_LABEL_OPTIONAL);
    pbw_field_set_oneof(f, 1);
    CHECK(pbw_message_add_field(msg, "h", 14, PBW_TYPE_INT64,
                                PBW_LABEL_OPTIONAL) != NULL);
    CHECK(pbw_message_add_field(msg, "u", 15, PBW_TYPE_UINT64,
                                PBW_LABEL_OPTIONAL) != NULL);
    CHECK(pbw_message_add_field(msg, "tags", 16, PBW_TYPE_STRING,
                                PBW_LABEL_REPEATED) != NULL);
    CHECK_OK(pbw_schema_link(schema));
    /* The golden parses and prints back byte-identically. */
    out = NULL;
    CHECK_OK(pbw_text_parse(msg, golden, strlen(golden), &decoded));
    CHECK_OK(pbw_text_print(decoded, &out, &len));
    CHECK(len == strlen(golden) && memcmp(out, golden, len) == 0);
    pbw_free(out);
    out = NULL;
    pbw_message_free(decoded);
    decoded = NULL;
    /* 64-bit, enum-number, bool and unsigned integers print canonically. */
    {
        static const char input[] =
            "a: 1 color: 2 str: \"x\" flag: true h: -2 u: 3";
        static const char want[] =
            "a: 1\ncolor: 2\nstr: \"x\"\nflag: true\nh: -2\nu: 3\n";

        decoded = NULL;
        CHECK_OK(pbw_text_parse(msg, input, sizeof(input) - 1, &decoded));
        CHECK_OK(pbw_text_print(decoded, &out, &len));
        CHECK(len == sizeof(want) - 1 && memcmp(out, want, len) == 0);
        pbw_free(out);
        out = NULL;
        pbw_message_free(decoded);
        decoded = NULL;
    }
    /* Accepted spellings. */
    {
        static const char *ok[] = {
            "a: 1, blob: \"x\";",                                                                /* separators */
            "a: 1 # comment\n",                                                                  /* comments */
            "a:0x10",                                                                            /* hex */
            "a:010",                                                                             /* octal */
            "a: -5",                                                                             /* negative */
            "f64: 1.00000000000000000000000000000000000000000000000000000000000000000000000001", /* long */
            "nums: [1, 2]",                                                                      /* list */
            "nums: []",                                                                          /* empty list */
            "tags: [\"a\", \"b\"]",                                                              /* string list */
            "subs [{ a: 1 }, < a: 2 >]",                                                         /* message list */
            "blob: \"\\u00e9\\u4e2d\\U0001F600\"",                                               /* \u/\U escapes */
            "f32: inf",                                                                          /* lowercase infinity */
            "f64: -INFINITY",                                                                    /* long infinity */
            "f32: nAn",                                                                          /* mixed-case NaN */
            "kid < a: 1 >",                                                                      /* angle brackets */
            "kid: { a: 1 }",                                                                     /* colon before message */
            "grp { x: 1 }",                                                                      /* lowercase group name */
            "Grp: { x: 1 }",                                                                     /* colon with group */
            "blob: \"a\" \"b\"",                                                                 /* adjacent strings */
            "str: \"hi\"",                                                                       /* string field */
            "str: \"\\u4e2d\\U0001F600\"",                                                       /* multibyte escapes */
            "str: \"a\\nb\\tc\\\\d\\'e\\\"f\\?g\\ah\\bi\\fj\\vk\"",
            "a: 0x7fffffff",               /* hex int32 max */
            "a: -2147483648",              /* int32 min */
            "f32: .5",                     /* leading-dot float */
            "f32: .5e5",                   /* leading-dot exponent */
            "f32: .5e+5",                  /* signed leading-dot exponent */
            "f32: .5f",                    /* leading-dot f suffix */
            "f32: 5.",                     /* trailing-dot float */
            "f32: 1e2",                    /* exponent */
            "f32: 1f",                     /* f suffix */
            "f32: 1e5",                    /* exponent */
            "f32: 1e+5",                   /* signed exponent */
            "f32: 1E-5",                   /* capital E */
            "f64: -inf",                   /* lowercase infinity */
            "f64: nan",                    /* lowercase NaN */
            "color: 1",                    /* numeric enum */
            "color: 0x1",                  /* hex enum */
            "color: 2",                    /* unknown open-enum number */
            "blob: \"\\xff\"",             /* bytes skip UTF-8 validation */
            "flag: true",                  /* bool */
            "flag: 1",                     /* int for bool */
            "flag: f",                     /* short bool */
            "subs { a: 1 } subs { a: 2 }", /* repeated messages */
            "subs [ { a: 1 }, < a: 2 > ]", /* list of messages */
        };
        size_t i;

        for (i = 0; i < sizeof(ok) / sizeof(ok[0]); i++) {
            CHECK_OK(pbw_text_parse(msg, ok[i], strlen(ok[i]), &decoded));
            pbw_message_free(decoded);
            decoded = NULL;
        }
    }
    /* Rejected spellings. */
    {
        static const char *bad[] = {
            "a: 2147483648",          /* range */
            "a: 0x100000000",         /* hex range */
            "h: 0x10000000000000000", /* hex range 64 */
            "a: 0x",                  /* empty hex */
            "a: 1.5",                 /* float for int */
            "f64: 1e",                /* exponent without digits */
            "f64: 1e+",
            "f64: 1E-",
            "f64: .5e",
            "blob: \"\\x\"",                                                                                                                                             /* bad escape */
            "blob: \"abc",                                                                                                                                               /* unterminated */
            "bogus: 1",                                                                                                                                                  /* unknown name */
            "[x_int]: 5",                                                                                                                                                /* short names never match extensions */
            "color: BOGUS",                                                                                                                                              /* unknown enum */
            "color: 1.5",                                                                                                                                                /* float for enum */
            "f32: 0x10",                                                                                                                                                 /* hex float */
            "f32: +1.5",                                                                                                                                                 /* leading plus */
            "a: +1",                                                                                                                                                     /* leading plus on int */
            "a: -2147483649",                                                                                                                                            /* int32 range */
            "f32: 0x1p3",                                                                                                                                                /* hex float */
            "a: 0b101",                                                                                                                                                  /* binary */
            "a: 0108",                                                                                                                                                   /* bad octal digit */
            "a: xyz",                                                                                                                                                    /* not a number */
            "a: 12x",                                                                                                                                                    /* trailing garbage */
            "f32: xyz",                                                                                                                                                  /* not a float */
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa: 1", /* name too long */
            "str: \"\\u12xy\"",                                                                                                                                          /* bad hex in \\u */
            "a: 1_000",                                                                                                                                                  /* underscores */
            "str: \"\\xff\"",                                                                                                                                            /* invalid UTF-8 in string */
            "str: \"\\uD800\"",                                                                                                                                          /* lone surrogate */
            "str: \"\\U00110000\"",                                                                                                                                      /* out of range */
            "str: \"\\x\"",                                                                                                                                              /* truncated hex escape */
            "str: \"\\q\"",                                                                                                                                              /* unknown escape */
            "str: \"\\777\"",                                                                                                                                            /* octal out of range */
            "str: \"abc\\",                                                                                                                                              /* backslash at end */
            "u: -1",                                                                                                                                                     /* negative for unsigned */
            "u: 18446744073709551616",                                                                                                                                   /* uint64 overflow */
            "str: \"\\xC0\\xAF\"",                                                                                                                                       /* overlong UTF-8 */
            "str: \"\\xED\\xA0\\x80\"",                                                                                                                                  /* surrogate UTF-8 */
            "str: \"\\xF4\\x90\\x80\\x80\"",                                                                                                                             /* out of range */
            "str: \"\\xE2\\x82\"",                                                                                                                                       /* truncated UTF-8 */
            "f32: 1e",                                                                                                                                                   /* exponent without digits */
            "f32: 1e+",                                                                                                                                                  /* exponent without digits */
            "f32: .e5",                                                                                                                                                  /* missing mantissa */
            "f32: 03.5",                                                                                                                                                 /* leading zero is octal */
            "f32: 01e5",                                                                                                                                                 /* octal with an exponent */
            "flag: yes",                                                                                                                                                 /* not a bool */
            "flag: 2",                                                                                                                                                   /* out of range for bool */
            "subs [1]",                                                                                                                                                  /* scalar in a message list */
            "subs [{ a: 1 },]",                                                                                                                                          /* trailing comma */
            "subs [1",                                                                                                                                                   /* truncated list */
            "subs { a: }",                                                                                                                                               /* missing value */
            "[x_int",                                                                                                                                                    /* unterminated extension */
            "[a b]: 1",                                                                                                                                                  /* bad extension name */
            "o1: 1 o2: 2",                                                                                                                                               /* oneof conflict */
            "kid: 1",                                                                                                                                                    /* scalar for message */
            "a: 1 bogus",                                                                                                                                                /* trailing garbage */
        };
        size_t i;

        for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
            CHECK_STATUS(PBW_ERR_INVALID,
                         pbw_text_parse(msg, bad[i], strlen(bad[i]),
                                        &decoded));
            CHECK(decoded == NULL);
        }
    }
    /* A missing required field fails the parse. */
    {
        pbw_schema *s2 = pbw_schema_new();

        CHECK(s2 != NULL);
        pbw_schema_set_proto3(s2, 0);
        req = pbw_schema_add_message(s2, "R");
        CHECK(req != NULL);
        CHECK(pbw_message_add_field(req, "a", 1, PBW_TYPE_INT32,
                                    PBW_LABEL_REQUIRED) != NULL);
        CHECK_OK(pbw_schema_link(s2));
        decoded = NULL;
        CHECK_STATUS(PBW_ERR_REQUIRED,
                     pbw_text_parse(req, "", 0, &decoded));
        CHECK(decoded == NULL);
        CHECK_OK(pbw_text_parse(req, "a: 1", 4, &decoded));
        pbw_message_free(decoded);
        decoded = NULL;
        pbw_schema_free(s2);
    }
    /* Unknown fields from the wire hide by default and show with the flag;
     * the text parser itself rejects unknown names. */
    {
        static const uint8_t with_unknown[] = {0x08, 0x01, 0x90, 0x03, 0x2a};

        decoded = NULL;
        CHECK_OK(
            pbw_decode(msg, with_unknown, sizeof(with_unknown), &decoded));
    }
    CHECK_OK(pbw_text_print_opts(decoded, 0, &out, &len));
    CHECK(len == 5 && memcmp(out, "a: 1\n", 5) == 0);
    pbw_free(out);
    out = NULL;
    CHECK_OK(pbw_text_print_opts(decoded, 1, &out, &len));
    CHECK(len == sizeof("a: 1\n50: 42\n") - 1 &&
          memcmp(out, "a: 1\n50: 42\n", len) == 0);
    pbw_free(out);
    pbw_message_free(decoded);
    decoded = NULL;
    /* ...while the text parser itself rejects the unknown name. */
    CHECK_STATUS(PBW_ERR_INVALID, pbw_text_parse(msg, "50: 42", 6, &decoded));
    CHECK(decoded == NULL);
    /* An unknown that fails to scan as a message prints quoted: the
     * outer field is complete, but its body holds a bad tag, a short
     * value, an overrunning length or an unclosed group. */
    {
        static const struct {
            const uint8_t *bytes;
            size_t len;
            const char *want;
        } cases[] = {
            /* field 1 varint, truncated */
            {(const uint8_t *)"\xE2\x03\x04\x08\x80\x80\x80", 7,
             "60: \"\\010\\200\\200\\200\"\n"},
            /* field 1 fixed32, short */
            {(const uint8_t *)"\xE2\x03\x03\x0D\x01\x02", 6,
             "60: \"\\r\\001\\002\"\n"},
            /* field 1 fixed64, short */
            {(const uint8_t *)"\xE2\x03\x04\x09\x01\x02\x03", 7,
             "60: \"\\t\\001\\002\\003\"\n"},
            /* field 2 LEN overruns the body */
            {(const uint8_t *)"\xE2\x03\x03\x12\x05\x41", 6,
             "60: \"\\022\\005A\"\n"},
            /* field number 0 */
            {(const uint8_t *)"\xE2\x03\x02\x00\x01", 5,
             "60: \"\\000\\001\"\n"},
            /* wire type 6 */
            {(const uint8_t *)"\xE2\x03\x01\x0E", 4,
             "60: \"\\016\"\n"},
            /* group opens but never closes */
            {(const uint8_t *)"\xE2\x03\x02\xFB\x03", 5,
             "60: \"\\373\\003\"\n"},
            /* END_GROUP where a field belongs */
            {(const uint8_t *)"\xE2\x03\x01\x1C", 4,
             "60: \"\\034\"\n"},
            /* fixed-width bodies scan as messages */
            {(const uint8_t *)"\xE2\x03\x05\x0D\x78\x56\x34\x12",
             8, "60 {\n  1: 0x12345678\n}\n"},
            {(const uint8_t *)"\xE2\x03\x09\x09\x01\x02\x03\x04\x05\x06\x07\x08",
             12, "60 {\n  1: 0x0807060504030201\n}\n"},
            /* an empty body is not a message */
            {(const uint8_t *)"\xE2\x03\x00", 3, "60: \"\"\n"},
        };
        size_t k;

        for (k = 0; k < sizeof(cases) / sizeof(cases[0]); k++) {
            decoded = NULL;
            CHECK_OK(
                pbw_decode(msg, cases[k].bytes, cases[k].len, &decoded));
            CHECK_OK(pbw_text_print(decoded, &out, &len));
            CHECK(len == strlen(cases[k].want) &&
                  memcmp(out, cases[k].want, len) == 0);
            pbw_free(out);
            out = NULL;
            pbw_message_free(decoded);
            decoded = NULL;
        }
    }
    /* Nested groups scan as messages, including fixed-width fields; a
     * truncated or invalid group body falls back to quoted bytes. */
    {
        static const uint8_t nested_grp[] = {
            0xE2,
            0x03,
            0x13,
            0xFB,
            0x03,
            0x11,
            0x01,
            0x02,
            0x03,
            0x04,
            0x05,
            0x06,
            0x07,
            0x08,
            0x83,
            0x04,
            0x08,
            0x01,
            0x84,
            0x04,
            0xFC,
            0x03,
        };
        static const char nested_want[] =
            "60 {\n  63 {\n    2: 0x0807060504030201\n"
            "    64 {\n      1: 1\n    }\n  }\n}\n";
        /* group body defects, each quoted verbatim */
        static const uint8_t bad_field0[] = {0xE2, 0x03, 0x05,
                                             0xFB, 0x03, 0x00, 0xFC,
                                             0x03};
        static const uint8_t bad_wire[] = {0xE2, 0x03, 0x05,
                                           0xFB, 0x03, 0x0E, 0xFC,
                                           0x03};
        static const uint8_t trunc_varint[] = {0xE2, 0x03, 0x04,
                                               0xFB, 0x03, 0x08,
                                               0x80};
        static const uint8_t trunc_fixed[] = {0xE2, 0x03, 0x05,
                                              0xFB, 0x03, 0x0D, 0x01,
                                              0x02};
        static const uint8_t trunc_len[] = {0xE2, 0x03, 0x07,
                                            0xFB, 0x03, 0x12, 0x05,
                                            0x41, 0xFC, 0x03};
        static const struct {
            const uint8_t *bytes;
            size_t len;
            const char *want;
        } grpcases[] = {
            {nested_grp, sizeof(nested_grp), nested_want},
            {bad_field0, sizeof(bad_field0),
             "60: \"\\373\\003\\000\\374\\003\"\n"},
            {bad_wire, sizeof(bad_wire),
             "60: \"\\373\\003\\016\\374\\003\"\n"},
            {trunc_varint, sizeof(trunc_varint),
             "60: \"\\373\\003\\010\\200\"\n"},
            {trunc_fixed, sizeof(trunc_fixed),
             "60: \"\\373\\003\\r\\001\\002\"\n"},
            {trunc_len, sizeof(trunc_len),
             "60: \"\\373\\003\\022\\005A\\374\\003\"\n"},
        };
        size_t k;

        for (k = 0; k < sizeof(grpcases) / sizeof(grpcases[0]); k++) {
            decoded = NULL;
            CHECK_OK(pbw_decode(msg, grpcases[k].bytes,
                                grpcases[k].len, &decoded));
            CHECK_OK(pbw_text_print(decoded, &out, &len));
            CHECK(len == strlen(grpcases[k].want) &&
                  memcmp(out, grpcases[k].want, len) == 0);
            pbw_free(out);
            out = NULL;
            pbw_message_free(decoded);
            decoded = NULL;
        }
    }
    /* A length-delimited unknown that scans as a message prints nested. */
    {
        /* a:1, then unknown field 60 (LEN) holding bytes 08 01 */
        static const uint8_t nested[] = {0x08, 0x01, 0xE2,
                                         0x03, 0x02, 0x08, 0x01};
        static const char want[] = "a: 1\n60 {\n  1: 1\n}\n";

        decoded = NULL;
        CHECK_OK(pbw_decode(msg, nested, sizeof(nested), &decoded));
        CHECK_OK(pbw_text_print(decoded, &out, &len));
        CHECK(len == sizeof(want) - 1 && memcmp(out, want, len) == 0);
        pbw_free(out);
        out = NULL;
        pbw_message_free(decoded);
        decoded = NULL;
    }
    /* Every other unknown wire type prints on one line. */
    {
        /* a:1, unknown 61 fixed64 DEADBEEFCAFEBABE, 62 fixed32
         * 0x12345678, group 63 holding x:1 */
        static const uint8_t mixed[] = {
            0x08,
            0x01,
            0xE9,
            0x03,
            0xBE,
            0xBA,
            0xFE,
            0xCA,
            0xEF,
            0xBE,
            0xAD,
            0xDE,
            0xF5,
            0x03,
            0x78,
            0x56,
            0x34,
            0x12,
            0xFB,
            0x03,
            0x08,
            0x01,
            0xFC,
            0x03,
        };
        static const char want[] =
            "a: 1\n61: 0xdeadbeefcafebabe\n62: 0x12345678\n"
            "63 {\n  1: 1\n}\n";

        decoded = NULL;
        CHECK_OK(pbw_decode(msg, mixed, sizeof(mixed), &decoded));
        CHECK_OK(pbw_text_print(decoded, &out, &len));
        CHECK(len == sizeof(want) - 1 && memcmp(out, want, len) == 0);
        pbw_free(out);
        out = NULL;
        pbw_message_free(decoded);
        decoded = NULL;
    }
    /* Unknown groups nest: fixed widths, a nested group and a length
     * prefix all scan as message content. */
    {
        static const uint8_t nested_grp[] = {
            0xE2,
            0x03,
            0x13,
            0xFB,
            0x03,
            0x11,
            0x01,
            0x02,
            0x03,
            0x04,
            0x05,
            0x06,
            0x07,
            0x08,
            0x83,
            0x04,
            0x08,
            0x01,
            0x84,
            0x04,
            0xFC,
            0x03,
        };
        static const char want[] =
            "60 {\n  63 {\n    2: 0x0807060504030201\n"
            "    64 {\n      1: 1\n    }\n  }\n}\n";

        decoded = NULL;
        CHECK_OK(pbw_decode(msg, nested_grp, sizeof(nested_grp),
                            &decoded));
        CHECK_OK(pbw_text_print(decoded, &out, &len));
        CHECK(len == sizeof(want) - 1 && memcmp(out, want, len) == 0);
        pbw_free(out);
        out = NULL;
        pbw_message_free(decoded);
        decoded = NULL;
    }
    check_text_prefix_sweep(msg);
    pbw_schema_free(schema);
}

/* JSON mapping: printer goldens, accepted/rejected spellings, WKT forms. */
/* Every strict prefix of a valid document parses safely: either it is
 * itself valid, or it fails with no message out. This walks the
 * incomplete-input branches without asserting verdicts. */
static void
check_json_prefix_sweep(const pbw_descriptor *msg)
{
    static const char *docs[] = {
        "{\"i32\":-5,\"flag\":true,\"name\":\"x\","
        "\"nums\":[1],\"kid\":{\"a\":1}}",
        "{\"when\":\"1970-01-01T00:00:00.5Z\","
        "\"howLong\":\"-3.5s\",\"mask\":\"fooBar\"}",
        "{\"props\":{\"k\":1,\"n\":null},"
        "\"payload\":{\"a\":2,\"@type\":\"type.googleapis.com/Sub\"},"
        "\"count\":7}",
        "{\"scores\":{\"b\":2,\"a\":1},"
        "\"ecol\":{\"x\":\"C_ONE\"},\"imap\":{\"1\":2}}",
    };
    size_t i;

    for (i = 0; i < sizeof(docs) / sizeof(docs[0]); i++) {
        size_t full = strlen(docs[i]);
        size_t k;

        for (k = 0; k < full; k++) {
            pbw_message *m = NULL;
            pbw_status st = pbw_json_parse(msg, docs[i], k, &m);

            CHECK(st == PBW_OK || m == NULL);
            pbw_message_free(m);
        }
    }
}

static void
test_json_format(void)
{
    pbw_schema *schema = pbw_schema_new();
    pbw_descriptor *color;
    pbw_descriptor *msg;
    pbw_descriptor *sub;
    pbw_descriptor *ts;
    pbw_descriptor *dur;
    pbw_descriptor *fm;
    pbw_descriptor *st;
    pbw_descriptor *entry;
    pbw_descriptor *mapentry;
    pbw_descriptor *val;
    pbw_descriptor *lval;
    pbw_descriptor *null;
    pbw_descriptor *any;
    pbw_descriptor *wrap;
    pbw_field *f;
    pbw_message *decoded;
    char *out;
    size_t len;
    static const struct field_def defs[] = {
        {"i32", 1, PBW_TYPE_INT32, PBW_LABEL_OPTIONAL},
        {"i64", 2, PBW_TYPE_INT64, PBW_LABEL_OPTIONAL},
        {"u64", 3, PBW_TYPE_UINT64, PBW_LABEL_OPTIONAL},
        {"u32", 31, PBW_TYPE_UINT32, PBW_LABEL_OPTIONAL},
        {"flag", 4, PBW_TYPE_BOOL, PBW_LABEL_OPTIONAL},
        {"name", 5, PBW_TYPE_STRING, PBW_LABEL_OPTIONAL},
        {"blob", 6, PBW_TYPE_BYTES, PBW_LABEL_OPTIONAL},
        {"f32", 7, PBW_TYPE_FLOAT, PBW_LABEL_OPTIONAL},
        {"f64", 8, PBW_TYPE_DOUBLE, PBW_LABEL_OPTIONAL},
        {"color", 9, PBW_TYPE_ENUM, PBW_LABEL_OPTIONAL},
        {"kid", 10, PBW_TYPE_MESSAGE, PBW_LABEL_OPTIONAL},
        {"nums", 11, PBW_TYPE_INT32, PBW_LABEL_REPEATED},
        {"tags", 12, PBW_TYPE_STRING, PBW_LABEL_REPEATED},
        {"when", 13, PBW_TYPE_MESSAGE, PBW_LABEL_OPTIONAL},
        {"howLong", 14, PBW_TYPE_MESSAGE, PBW_LABEL_OPTIONAL},
        {"mask", 15, PBW_TYPE_MESSAGE, PBW_LABEL_OPTIONAL},
        {"props", 16, PBW_TYPE_MESSAGE, PBW_LABEL_OPTIONAL},
        {"payload", 17, PBW_TYPE_MESSAGE, PBW_LABEL_OPTIONAL},
        {"count", 18, PBW_TYPE_MESSAGE, PBW_LABEL_OPTIONAL},
        {"alt", 19, PBW_TYPE_INT32, PBW_LABEL_OPTIONAL},
        {"scores", 20, PBW_TYPE_MESSAGE, PBW_LABEL_REPEATED},
        {"ecol", 21, PBW_TYPE_MESSAGE, PBW_LABEL_REPEATED},
        {"imap", 22, PBW_TYPE_MESSAGE, PBW_LABEL_REPEATED},
        {"emsg", 23, PBW_TYPE_MESSAGE, PBW_LABEL_REPEATED},
        {"bmap", 24, PBW_TYPE_MESSAGE, PBW_LABEL_REPEATED},
        {"lmap", 25, PBW_TYPE_MESSAGE, PBW_LABEL_REPEATED},
        {"umap", 26, PBW_TYPE_MESSAGE, PBW_LABEL_REPEATED},
        {"lv", 27, PBW_TYPE_MESSAGE, PBW_LABEL_OPTIONAL},
        {"em", 28, PBW_TYPE_MESSAGE, PBW_LABEL_OPTIONAL},
        {"vv", 29, PBW_TYPE_MESSAGE, PBW_LABEL_OPTIONAL},
        {"Ab_c", 30, PBW_TYPE_STRING, PBW_LABEL_OPTIONAL},
        {"vmap", 32, PBW_TYPE_MESSAGE, PBW_LABEL_REPEATED},
        {"ekey", 33, PBW_TYPE_MESSAGE, PBW_LABEL_REPEATED},
        {"vrep", 34, PBW_TYPE_MESSAGE, PBW_LABEL_REPEATED},
        {"my_field", 35, PBW_TYPE_INT32, PBW_LABEL_OPTIONAL},
    };
    /* \u00e9 in, raw UTF-8 out: the printer passes valid text through. */
    static const char *input =
        "{\"i32\":-5,\"i64\":\"-6\",\"u64\":\"7\",\"flag\":true,"
        "\"name\":\"hi\\u00e9\",\"blob\":\"AP8/\",\"f32\":3.5,"
        "\"f64\":\"NaN\",\"color\":\"C_RED\",\"kid\":{\"a\":1},"
        "\"nums\":[1,2],\"tags\":[\"x\"]}";
    static const char *golden =
        "{\"i32\":-5,\"i64\":\"-6\",\"u64\":\"7\",\"flag\":true,"
        "\"name\":\"hi\u00e9\",\"blob\":\"AP8/\",\"f32\":3.5,"
        "\"f64\":\"NaN\",\"color\":\"C_RED\",\"kid\":{\"a\":1},"
        "\"nums\":[1,2],\"tags\":[\"x\"]}";

    CHECK(schema != NULL);
    color = pbw_schema_add_enum(schema, "C");
    CHECK(color != NULL);
    pbw_enum_add_value(color, "C_ZERO", 0);
    pbw_enum_add_value(color, "C_RED", 1);
    sub = pbw_schema_add_message(schema, "Sub");
    CHECK(sub != NULL);
    CHECK(pbw_message_add_field(sub, "a", 1, PBW_TYPE_INT32,
                                PBW_LABEL_OPTIONAL) != NULL);
    ts = pbw_schema_add_message(schema, "google.protobuf.Timestamp");
    dur = pbw_schema_add_message(schema, "google.protobuf.Duration");
    fm = pbw_schema_add_message(schema, "google.protobuf.FieldMask");
    st = pbw_schema_add_message(schema, "google.protobuf.Struct");
    entry = pbw_schema_add_message(schema, "google.protobuf.Struct.FieldsEntry");
    val = pbw_schema_add_message(schema, "google.protobuf.Value");
    lval = pbw_schema_add_message(schema, "google.protobuf.ListValue");
    null = pbw_schema_add_enum(schema, "google.protobuf.NullValue");
    pbw_enum_add_value(null, "NULL_VALUE", 0);
    any = pbw_schema_add_message(schema, "google.protobuf.Any");
    wrap = pbw_schema_add_message(schema, "google.protobuf.Int32Value");
    CHECK(pbw_schema_add_message(schema, "google.protobuf.Empty") != NULL);
    CHECK(ts != NULL && dur != NULL && fm != NULL && st != NULL &&
          any != NULL && wrap != NULL);
    {
        struct field_def tf[] = {
            {"seconds", 1, PBW_TYPE_INT64, PBW_LABEL_OPTIONAL},
            {"nanos", 2, PBW_TYPE_INT32, PBW_LABEL_OPTIONAL},
        };

        add_fields(ts, tf, 2);
        add_fields(dur, tf, 2);
    }
    CHECK(pbw_message_add_field(fm, "paths", 1, PBW_TYPE_STRING,
                                PBW_LABEL_REPEATED) != NULL);
    CHECK(pbw_message_add_field(st, "fields", 1, PBW_TYPE_MESSAGE,
                                PBW_LABEL_REPEATED) != NULL);
    CHECK(pbw_message_add_field(entry, "key", 1, PBW_TYPE_STRING,
                                PBW_LABEL_OPTIONAL) != NULL);
    f = pbw_message_add_field(entry, "value", 2, PBW_TYPE_MESSAGE,
                              PBW_LABEL_OPTIONAL);
    pbw_field_set_type_name(f, "google.protobuf.Value");
    pbw_descriptor_set_map_entry(entry);
    f = pbw_message_add_field(val, "null_value", 1, PBW_TYPE_ENUM,
                              PBW_LABEL_OPTIONAL);
    pbw_field_set_type_name(f, "google.protobuf.NullValue");
    CHECK(pbw_message_add_field(val, "number_value", 2, PBW_TYPE_DOUBLE,
                                PBW_LABEL_OPTIONAL) != NULL);
    CHECK(pbw_message_add_field(val, "string_value", 3, PBW_TYPE_STRING,
                                PBW_LABEL_OPTIONAL) != NULL);
    CHECK(pbw_message_add_field(val, "bool_value", 4, PBW_TYPE_BOOL,
                                PBW_LABEL_OPTIONAL) != NULL);
    mapentry =
        pbw_schema_add_message(schema, "M.ScoresEntry");
    CHECK(mapentry != NULL);
    CHECK(pbw_message_add_field(mapentry, "key", 1, PBW_TYPE_STRING,
                                PBW_LABEL_OPTIONAL) != NULL);
    CHECK(pbw_message_add_field(mapentry, "value", 2, PBW_TYPE_INT32,
                                PBW_LABEL_OPTIONAL) != NULL);
    pbw_descriptor_set_map_entry(mapentry);
    /* message-, enum- and int-keyed maps for the map parser paths */
    {
        pbw_descriptor *ee;
        pbw_descriptor *ie;
        pbw_descriptor *me;
        pbw_descriptor *ke;

        ee = pbw_schema_add_message(schema, "M.EcolEntry");
        CHECK(ee != NULL);
        CHECK(pbw_message_add_field(ee, "key", 1, PBW_TYPE_STRING,
                                    PBW_LABEL_OPTIONAL) != NULL);
        f = pbw_message_add_field(ee, "value", 2, PBW_TYPE_ENUM,
                                  PBW_LABEL_OPTIONAL);
        pbw_field_set_type_name(f, "C");
        pbw_descriptor_set_map_entry(ee);
        ie = pbw_schema_add_message(schema, "M.ImapEntry");
        CHECK(ie != NULL);
        CHECK(pbw_message_add_field(ie, "key", 1, PBW_TYPE_INT32,
                                    PBW_LABEL_OPTIONAL) != NULL);
        CHECK(pbw_message_add_field(ie, "value", 2, PBW_TYPE_INT32,
                                    PBW_LABEL_OPTIONAL) != NULL);
        pbw_descriptor_set_map_entry(ie);
        me = pbw_schema_add_message(schema, "M.EmsgEntry");
        CHECK(me != NULL);
        CHECK(pbw_message_add_field(me, "key", 1, PBW_TYPE_STRING,
                                    PBW_LABEL_OPTIONAL) != NULL);
        f = pbw_message_add_field(me, "value", 2, PBW_TYPE_MESSAGE,
                                  PBW_LABEL_OPTIONAL);
        pbw_field_set_type_name(f, "Sub");
        pbw_descriptor_set_map_entry(me);
        /* enum keys print by name when known, by number otherwise */
        ke = pbw_schema_add_message(schema, "M.EkeyEntry");
        CHECK(ke != NULL);
        f = pbw_message_add_field(ke, "key", 1, PBW_TYPE_ENUM,
                                  PBW_LABEL_OPTIONAL);
        pbw_field_set_type_name(f, "C");
        CHECK(pbw_message_add_field(ke, "value", 2, PBW_TYPE_INT32,
                                    PBW_LABEL_OPTIONAL) != NULL);
        pbw_descriptor_set_map_entry(ke);
    }
    /* bool- and int64-keyed maps for the key printer paths */
    {
        pbw_descriptor *be;
        pbw_descriptor *le;

        be = pbw_schema_add_message(schema, "M.BmapEntry");
        CHECK(be != NULL);
        CHECK(pbw_message_add_field(be, "key", 1, PBW_TYPE_BOOL,
                                    PBW_LABEL_OPTIONAL) != NULL);
        CHECK(pbw_message_add_field(be, "value", 2, PBW_TYPE_INT32,
                                    PBW_LABEL_OPTIONAL) != NULL);
        pbw_descriptor_set_map_entry(be);
        le = pbw_schema_add_message(schema, "M.LmapEntry");
        CHECK(le != NULL);
        CHECK(pbw_message_add_field(le, "key", 1, PBW_TYPE_INT64,
                                    PBW_LABEL_OPTIONAL) != NULL);
        CHECK(pbw_message_add_field(le, "value", 2, PBW_TYPE_INT32,
                                    PBW_LABEL_OPTIONAL) != NULL);
        pbw_descriptor_set_map_entry(le);
        {
            pbw_descriptor *ue;

            ue = pbw_schema_add_message(schema, "M.UmapEntry");
            CHECK(ue != NULL);
            CHECK(pbw_message_add_field(ue, "key", 1, PBW_TYPE_UINT32,
                                        PBW_LABEL_OPTIONAL) != NULL);
            CHECK(pbw_message_add_field(ue, "value", 2, PBW_TYPE_INT32,
                                        PBW_LABEL_OPTIONAL) != NULL);
            pbw_descriptor_set_map_entry(ue);
        }
        {
            pbw_descriptor *ve;

            ve = pbw_schema_add_message(schema, "M.VmapEntry");
            CHECK(ve != NULL);
            CHECK(pbw_message_add_field(ve, "key", 1, PBW_TYPE_STRING,
                                        PBW_LABEL_OPTIONAL) != NULL);
            f = pbw_message_add_field(ve, "value", 2, PBW_TYPE_MESSAGE,
                                      PBW_LABEL_OPTIONAL);
            pbw_field_set_type_name(f, "google.protobuf.Value");
            pbw_descriptor_set_map_entry(ve);
        }
    }
    f = pbw_message_add_field(val, "struct_value", 5, PBW_TYPE_MESSAGE,
                              PBW_LABEL_OPTIONAL);
    pbw_field_set_type_name(f, "google.protobuf.Struct");
    f = pbw_message_add_field(val, "list_value", 6, PBW_TYPE_MESSAGE,
                              PBW_LABEL_OPTIONAL);
    pbw_field_set_type_name(f, "google.protobuf.ListValue");
    f = pbw_message_add_field(lval, "values", 1, PBW_TYPE_MESSAGE,
                              PBW_LABEL_REPEATED);
    pbw_field_set_type_name(f, "google.protobuf.Value");
    CHECK(pbw_message_add_field(any, "type_url", 1, PBW_TYPE_STRING,
                                PBW_LABEL_OPTIONAL) != NULL);
    CHECK(pbw_message_add_field(any, "value", 2, PBW_TYPE_BYTES,
                                PBW_LABEL_OPTIONAL) != NULL);
    CHECK(pbw_message_add_field(wrap, "value", 1, PBW_TYPE_INT32,
                                PBW_LABEL_OPTIONAL) != NULL);
    msg = pbw_schema_add_message(schema, "M");
    CHECK(msg != NULL);
    add_fields(msg, defs, sizeof(defs) / sizeof(defs[0]));
    f = (pbw_field *)pbw_descriptor_find_field_name(msg, "color");
    pbw_field_set_type_name(f, "C");
    f = (pbw_field *)pbw_descriptor_find_field_name(msg, "kid");
    pbw_field_set_type_name(f, "Sub");
    f = (pbw_field *)pbw_descriptor_find_field_name(msg, "when");
    pbw_field_set_type_name(f, "google.protobuf.Timestamp");
    f = (pbw_field *)pbw_descriptor_find_field_name(msg, "howLong");
    pbw_field_set_type_name(f, "google.protobuf.Duration");
    f = (pbw_field *)pbw_descriptor_find_field_name(msg, "mask");
    pbw_field_set_type_name(f, "google.protobuf.FieldMask");
    f = (pbw_field *)pbw_descriptor_find_field_name(msg, "props");
    pbw_field_set_type_name(f, "google.protobuf.Struct");
    f = (pbw_field *)pbw_descriptor_find_field_name(st, "fields");
    pbw_field_set_type_name(f, "google.protobuf.Struct.FieldsEntry");
    f = (pbw_field *)pbw_descriptor_find_field_name(msg, "payload");
    pbw_field_set_type_name(f, "google.protobuf.Any");
    f = (pbw_field *)pbw_descriptor_find_field_name(msg, "count");
    pbw_field_set_type_name(f, "google.protobuf.Int32Value");
    /* name(5) and alt(19) share oneof 1: setting both fails */
    f = (pbw_field *)pbw_descriptor_find_field_name(msg, "alt");
    pbw_field_set_oneof(f, 1);
    f = (pbw_field *)pbw_descriptor_find_field_name(msg, "scores");
    pbw_field_set_type_name(f, "M.ScoresEntry");
    f = (pbw_field *)pbw_descriptor_find_field_name(msg, "ecol");
    pbw_field_set_type_name(f, "M.EcolEntry");
    f = (pbw_field *)pbw_descriptor_find_field_name(msg, "imap");
    pbw_field_set_type_name(f, "M.ImapEntry");
    f = (pbw_field *)pbw_descriptor_find_field_name(msg, "emsg");
    pbw_field_set_type_name(f, "M.EmsgEntry");
    f = (pbw_field *)pbw_descriptor_find_field_name(msg, "bmap");
    pbw_field_set_type_name(f, "M.BmapEntry");
    f = (pbw_field *)pbw_descriptor_find_field_name(msg, "lmap");
    pbw_field_set_type_name(f, "M.LmapEntry");
    f = (pbw_field *)pbw_descriptor_find_field_name(msg, "umap");
    pbw_field_set_type_name(f, "M.UmapEntry");
    f = (pbw_field *)pbw_descriptor_find_field_name(msg, "lv");
    pbw_field_set_type_name(f, "google.protobuf.ListValue");
    f = (pbw_field *)pbw_descriptor_find_field_name(msg, "em");
    pbw_field_set_type_name(f, "google.protobuf.Empty");
    f = (pbw_field *)pbw_descriptor_find_field_name(msg, "vv");
    pbw_field_set_type_name(f, "google.protobuf.Value");
    f = (pbw_field *)pbw_descriptor_find_field_name(msg, "vmap");
    pbw_field_set_type_name(f, "M.VmapEntry");
    f = (pbw_field *)pbw_descriptor_find_field_name(msg, "ekey");
    pbw_field_set_type_name(f, "M.EkeyEntry");
    f = (pbw_field *)pbw_descriptor_find_field_name(msg, "vrep");
    pbw_field_set_type_name(f, "google.protobuf.Value");
    f = (pbw_field *)pbw_descriptor_find_field_name(msg, "name");
    pbw_field_set_oneof(f, 1);
    CHECK_OK(pbw_schema_link(schema));
    /* The golden parses and prints back identically. */
    out = NULL;
    CHECK_OK(pbw_json_parse(msg, input, strlen(input), &decoded));
    CHECK_OK(pbw_json_print(decoded, &out, &len));
    CHECK(len == strlen(golden) && memcmp(out, golden, len) == 0);
    pbw_free(out);
    out = NULL;
    pbw_message_free(decoded);
    decoded = NULL;
    /* Every short escape prints back identically. */
    {
        static const char *esc_in =
            "{\"name\":\"a\\\"b\\\\c\\bd\\fe\\ng\\rh\\ti\\u0001\"}";

        CHECK_OK(pbw_json_parse(msg, esc_in, strlen(esc_in), &decoded));
        CHECK_OK(pbw_json_print(decoded, &out, &len));
        CHECK(len == strlen(esc_in) && memcmp(out, esc_in, len) == 0);
        pbw_free(out);
        out = NULL;
        pbw_message_free(decoded);
        decoded = NULL;
        /* A 3-byte escape prints as raw UTF-8. */
        {
            static const char esc3_in[] = "{\"name\":\"\\u4e2d\"}";
            static const char esc3_want[] = "{\"name\":\"\xe4\xb8\xad\"}";

            CHECK_OK(pbw_json_parse(msg, esc3_in, strlen(esc3_in),
                                    &decoded));
            CHECK_OK(pbw_json_print(decoded, &out, &len));
            CHECK(len == sizeof(esc3_want) - 1 &&
                  memcmp(out, esc3_want, len) == 0);
            pbw_free(out);
            out = NULL;
            pbw_message_free(decoded);
            decoded = NULL;
        }
    }
    /* Binary input may carry bytes the text grammar forbids: `<` and `>`
     * print as \u escapes, and invalid UTF-8 fails the print. */
    {
        /* name(5) = "a<b" */
        static const uint8_t lt[] = {0x2A, 0x03, 0x61, 0x3C, 0x62};
        static const char want[] = "{\"name\":\"a\\u003cb\"}";
        /* name(5) = "a>b" */
        static const uint8_t gt[] = {0x2A, 0x03, 0x61, 0x3E, 0x62};
        static const char want_gt[] = "{\"name\":\"a\\u003eb\"}";
        /* name(5) = single 0xFF byte */
        static const uint8_t badutf8[] = {0x2A, 0x01, 0xFF};

        decoded = NULL;
        CHECK_OK(pbw_decode(msg, lt, sizeof(lt), &decoded));
        CHECK_OK(pbw_json_print(decoded, &out, &len));
        CHECK(len == sizeof(want) - 1 &&
              memcmp(out, want, len) == 0);
        pbw_free(out);
        out = NULL;
        pbw_message_free(decoded);
        decoded = NULL;
        decoded = NULL;
        CHECK_OK(pbw_decode(msg, gt, sizeof(gt), &decoded));
        CHECK_OK(pbw_json_print(decoded, &out, &len));
        CHECK(len == sizeof(want_gt) - 1 &&
              memcmp(out, want_gt, len) == 0);
        pbw_free(out);
        out = NULL;
        pbw_message_free(decoded);
        decoded = NULL;
        CHECK_OK(pbw_decode(msg, badutf8, sizeof(badutf8), &decoded));
        CHECK_STATUS(PBW_ERR_INVALID,
                     pbw_json_print(decoded, &out, &len));
        CHECK(out == NULL);
        pbw_message_free(decoded);
        decoded = NULL;
    }
    /* Astral characters print as raw UTF-8; malformed byte sequences
     * fail the print instead of emitting garbage. */
    {
        /* name(5) = U+1F600, then truncated/bad/overlong forms */
        static const uint8_t astral[] = {0x2A, 0x04, 0xF0,
                                         0x9F, 0x98, 0x80};
        static const char astral_want[] = "{\"name\":\"\xF0\x9F\x98\x80\"}";
        static const uint8_t trunc3[] = {0x2A, 0x02, 0xE4, 0xB8};
        static const uint8_t badcont[] = {0x2A, 0x02, 0xE4, 0x28};
        static const uint8_t overlong[] = {0x2A, 0x02, 0xC0, 0xAF};
        static const uint8_t surrogate[] = {0x2A, 0x03, 0xED,
                                            0xA0, 0x80};
        static const uint8_t toobig[] = {0x2A, 0x04, 0xF4,
                                         0x90, 0x80, 0x80};
        static const struct {
            const uint8_t *bytes;
            size_t len;
        } bad[] = {{trunc3, sizeof(trunc3)},
                   {badcont, sizeof(badcont)},
                   {overlong, sizeof(overlong)},
                   {surrogate, sizeof(surrogate)},
                   {toobig, sizeof(toobig)}};
        size_t k;

        decoded = NULL;
        CHECK_OK(pbw_decode(msg, astral, sizeof(astral), &decoded));
        CHECK_OK(pbw_json_print(decoded, &out, &len));
        CHECK(len == sizeof(astral_want) - 1 &&
              memcmp(out, astral_want, len) == 0);
        pbw_free(out);
        out = NULL;
        pbw_message_free(decoded);
        decoded = NULL;
        for (k = 0; k < sizeof(bad) / sizeof(bad[0]); k++) {
            decoded = NULL;
            CHECK_OK(
                pbw_decode(msg, bad[k].bytes, bad[k].len, &decoded));
            CHECK_STATUS(PBW_ERR_INVALID,
                         pbw_json_print(decoded, &out, &len));
            CHECK(out == NULL);
            pbw_message_free(decoded);
            decoded = NULL;
        }
    }
    /* Raw multibyte UTF-8 in keys peeks (then fails as unknown). */
    {
        /* {"<U+4E2D>":1} and {"<U+1F600>":1} built from byte escapes
         * so the source stays ASCII */
        static const char k3[] = "{\"\xe4\xb8\xad\":1}";
        static const char k4[] = "{\"\xf0\x9f\x98\x80\":1}";

        CHECK_STATUS(PBW_ERR_INVALID,
                     pbw_json_parse(msg, k3, sizeof(k3) - 1, &decoded));
        CHECK(decoded == NULL);
        CHECK_STATUS(PBW_ERR_INVALID,
                     pbw_json_parse(msg, k4, sizeof(k4) - 1, &decoded));
        CHECK(decoded == NULL);
    }
    /* A proto name starting uppercase prints its json name. */
    {
        static const char in[] = "{\"Ab_c\":\"v\"}";
        static const char want[] = "{\"AbC\":\"v\"}";

        CHECK_OK(pbw_json_parse(msg, in, strlen(in), &decoded));
        CHECK_OK(pbw_json_print(decoded, &out, &len));
        CHECK(len == strlen(want) && memcmp(out, want, len) == 0);
        pbw_free(out);
        out = NULL;
        pbw_message_free(decoded);
        decoded = NULL;
    }
    /* Accepted spellings. */
    {
        static const char *ok[] = {
            "{}",
            "{\"i32\":\"5\"}",
            "{\"u32\":4294967295}",
            "{\"i32\":\"5.0\"}",
            "{\"i32\":\"0x10\"}",
            "{\"i32\":5.0}",
            "{\"i32\":1e3}",
            "{\"flag\":\"true\"}",
            "{\"flag\":\"YES\"}",
            "{\"flag\":null}",
            "{\"f64\":\"Infinity\"}",
            "{\"f64\":\"-Infinity\"}",
            "{\"color\":1}",
            "{\"color\":\"1\"}",
            "{\"name\":null}",
            "{\"kid\":null}",
            "{\"nums\":null}",
            "{\"nums\":[]}",
            "{\"nums\":[[]]}",
            "{\"lv\":[]}",
            "{\"emsg\":{}}",
            "{\"color\":99}",
            "{\"myField\":7}",
            "{\"my_field\":7}",
            "{\"nums\":[1,null,2]}",
            "{\"nums\":[[1,2]]}",
            "{\"nums\":[1,null]}",
            "{\"tags\":[null]}",
            "{'i32':1}",
            "{i32:1}",
            "{\"i32\":1,}",
            "{\"when\":\"1970-01-01T00:00:00Z\"}",
            "{\"when\":\"1970-01-01T00:00:00.5Z\"}",
            "{\"when\":\"1970-01-01T08:00:00+08:00\"}",
            "{\"when\":null}",
            "{\"when\":{\"seconds\":\"5\"}}",
            "{\"\\u0069\\u0033\\u0032\":5}",
            "{\"\\u0063\\u006F\\u006C\\u006F\\u0072\":\"C_RED\"}",
            "{\"name\":\"\\ud83d\\ude00\"}",
            "{\"name\":\"\\ud83D\\uDE00\"}",
            "{\"howLong\":\"-3.5s\"}",
            "{\"mask\":\"fooBar,baz\"}",
            "{\"mask\":\"\"}",
            "{\"props\":{\"k\":1}}",
            "{\"props\":{\"b\":false}}",
            "{\"props\":{}}",
            "{\"props\":null}",
            "{\"props\":{\"l\":[1,]}}",
            "{\"props\":{\"k\":1,}}",
            "{\"payload\":{\"a\":2,\"@type\":\"type.googleapis.com/Sub\"}}",
            "{\"payload\":{}}",
            "{\"count\":7}",
            "{\"count\":null}",
            "{\"flag\":false}",
            "{\"scores\":{\"a\":1}}",
            "{\"scores\":{}}",
            "{\"blob\":\"AP8-\"}",
            "{\"blob\":\"AP8_\"}",
            "{\"blob\":\"AP8\"}",
            "{\"blob\":\"AQI=\"}",
            "{\"blob\":\"AQ==\"}",
            "{\"blob\":\"\"}",
        };
        size_t i;

        for (i = 0; i < sizeof(ok) / sizeof(ok[0]); i++) {
            CHECK_OK(pbw_json_parse(msg, ok[i], strlen(ok[i]), &decoded));
            pbw_message_free(decoded);
            decoded = NULL;
        }
    }
    /* Rejected spellings. */
    {
        static const char *bad[] = {
            "",
            " ",
            "{",
            "123",
            "[1]",
            "\"x\"",
            "null",
            "{\"i32\":}",
            "{\"i32\":123null}",
            "{\"i32\":1true}",
            "{\"i32\":01}",
            "{\"i32\":1.}",
            "{\"i32\":+1}",
            "{\"i32\":\"5 \"}",
            "{\"i32\":\"1.5\"}",
            "{\"i32\":1.5}",
            "{\"u64\":-1}",
            "{\"u64\":\"-1\"}",
            "{\"u64\":\"0.5\"}",
            "{\"u64\":0.5}",
            "{\"f32\":1e400}",
            "{\"flag\":1}",
            "{\"flag\":\"2\"}",
            "{\"flag\":1.5}",
            "{\"flag\":\"x\"}",
            "{\"f32\":\"abc\"}",
            "{\"i32\":\"\"}",
            "{\"howLong\":\"3s x\"}",
            "{\"howLong\":\"s\"}",
            "{\"howLong\":\"--3s\"}",
            "{\"name\":5}",
            "{\"name\":\"\xff\"}",
            "{\"name\":\"\\u12\"}",
            "{\"name\":\"\\uDC00\"}",
            "{\"i32\":1e}",
            "{\"i32\":\"+\"}",
            "{\"flag\":\"12345678\"}",
            "{\"flag\":falseX}",
            "{\"f32\":1e40}",
            "{\"blob\":\"!\"}",
            "{\"blob\":\"AP8!\"}",
            "{\"blob\":\"A\"}",
            "{\"blob\":\"A\"}",
            "{\"blob\":\"A===\"}",
            "{\"blob\":\"!!\"}",
            "{\"blob\":\"!!!\"}",
            "{\"color\":\"BOGUS\"}",
            "{\"color\":9999999999}",
            "{\"nums\":1}",
            "{\"kid\":[]}",
            "{\"scores\":{\"a\":1,\"a\":2}}",
            "{\"bogus\":1}",
            "{\"name\":\"x\",\"alt\":1}",
            "{\"when\":\"10000-01-01T00:00:00Z\"}",
            "{\"when\":\"1970-01-01T00:00:00+0800\"}",
            "{\"when\":\"1970-01-01\"}",
            "{\"when\":\"\"}",
            "{\"when\":\"1970-01-01T00:00:01+0800\"}",
            "{\"howLong\":\"3\"}",
            "{\"howLong\":\"3.0000000000s\"}",
            "{\"payload\":{\"a\":1}}",
            "{\"payload\":{\"value\":{}}}",
            "{\"payload\":{\"@type\":\"type.googleapis.com/google.protobuf.DoubleValue\",\"x\":1}}",
            "{\"props\":1}",
            "{\"props\":[]}",
            "{\"props\":{\"k\":}}",
            "{\"props\":{\"k\":undefined}}",
            "{\"props\":{\"k\":1,\"k\":2}}",
            "{\"kid\":{\"a\":}}",
            "{\"scores\":1}",
            "{\"scores\":{\"a\":1,\"b\":}}",
            "{\"emsg\":{\"k\":{\"a\":}}}",
            "{\"payload\":1}",
            "{\"payload\":[]}",
            "{\"payload\":{\"@type\":1}}",
            "{\"payload\":{\"@type\":\"type.googleapis.com/\"}}",
            "{\"payload\":{\"x\"}}",
            "{\"payload\":{\"@type\":\"type.googleapis.com/Sub\",\"@type\":\"x\"}}",
            "{\"payload\":{\"@type\":\"type.googleapis.com/Sub\",\"x\":}}",
            "{\"payload\":{1:2}}",
            "{\"payload\":{\"@type\"}}",
            "{\"payload\":{\"@type\":\"foo\"}}",
            "{\"payload\":{\"@type\":\"\"}}",
            "{\"payload\":{1:2}}",
            "{\"payload\":{\"@type\":\"type.googleapis.com/Sub\",\"value\":{\"a\":}}}",
            "{\"payload\":{\"@type\":\"type.googleapis.com/Sub\",\"value\":1}}",
            "{\"kid\":[1]}",
            "{\"kid\":\"x\"}",
            "{\"color\":{}}",
            "{\"color\":{}}",
            "{\"when\":123}",
            "{\"howLong\":true}",
            "{\"mask\":1}",
            "{\"when\":\"1970-01-01T00:00:00+08:0\"}",
            "{\"when\":\"1970-011-01T00:00:00Z\"}",
            "{\"when\":\"1970-01-011T00:00:00Z\"}",
            "{\"when\":\"1970-01-01T000:00:00Z\"}",
            "{\"when\":\"1970-01-01T00:000:00Z\"}",
            "{\"when\":\"1970-01-01T00:00:000Z\"}",
            "{\"when\":\"1970/01/01T00:00:00Z\"}",
            "{\"when\":\"1970-01-01T00:00:00\"}",
            "{\"when\":\"1970-01-01T00:00:00.5xZ\"}",
            "{\"when\":\"1970-01-01T00:00:00.1234567890Z\"}",
            "{\"when\":\"1970-01-01T00:00:00+8:00\"}",
            "{\"when\":\"1970-01-01T0:00:00Z\"}",
            "{\"when\":\"1970-01-01T00:00:00.5+08\"}",
            "{\"when\":\"1970-01-01T00:00:00.5+08:0X\"}",
            "{\"when\":\"1970-01-01T00:00:006\"}",
            "{\"when\":123}",
            "{\"when\":true}",
            "{\"howLong\":true}",
            "{\"mask\":1}",
            "{\"color\":{}}",
            "{\"props\":{\"k\":undefined}}",
            "{\"props\":{\"k\":1,\"k2\":}}",
            "{\"props\":{\"k\":1,\"k\":}}",
            "{\"props\":\"x\"}",
            "{\"props\":{1:2}}",
            "{\"props\":{\"k\"}}",
            "{\"props\":{\"l\":}}",
            "{\"props\":{\"l\":[tru]}}",
            "{\"scores\":{\"a\":1}",
            "{\"lv\":1}",
            "{\"lv\":[1}",
            "{\"lv\":nul}",
            "{\"subs\":[null]}",
            "{\"lv\":[1,tru]}",
            "{\"em\":1}",
            "{\"payload\":{\"a\":1,\"@type\":\"type.googleapis.com/Nope\"}}",
            "{\"name\":\"\\ud800\"}",
            "{\"name\":\"\\ude00\"}",
            "{\"name\":\"\\ud83d\"}",
            "{\"name\":\"\\ud83dx\"}",
            "{\"name\":\"\\u00\"}",
            "{\"name\":\"\\uzzzz\"}",
            "{\"name\":\"\\ud83d\\ude0\"}",
            "{\"props\":{\"k\":\"\\xff\"}}",
            "{\"\\u00\":1}",
            "{\"\\ud800\":1}",
            "{\"a\\/b\":1}",
            "{\"a\\'b\":1}",
            "{\"true\":1}",
        };
        size_t i;

        for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
            CHECK_STATUS(PBW_ERR_INVALID,
                         pbw_json_parse(msg, bad[i], strlen(bad[i]),
                                        &decoded));
            CHECK(decoded == NULL);
        }
        /* Truncated escapes fail instead of reading past the end. */
        CHECK_STATUS(PBW_ERR_INVALID,
                     pbw_json_parse(msg, "{\"name\":\"\\u12", 12,
                                    &decoded));
        CHECK(decoded == NULL);
    }
    /* Deeply nested structs stop at the depth cap. */
    {
        char deep[4096];
        size_t at = 0;
        int i;
        const char head[] = "{\"props\":{\"k\":";

        memcpy(deep + at, head, sizeof(head) - 1);
        at += sizeof(head) - 1;
        for (i = 0; i < 300; i++) {
            memcpy(deep + at, "{\"k\":", 5);
            at += 5;
        }
        deep[at++] = '1';
        for (i = 0; i < 302; i++) {
            deep[at++] = '}';
        }
        deep[at] = '\0';
        CHECK_STATUS(PBW_ERR_LIMIT,
                     pbw_json_parse(msg, deep, at, &decoded));
        CHECK(decoded == NULL);
    }
    /* Wrappers print their bare value, or nothing when null. */
    {
        static const char *in[] = {
            "{\"count\":7}",
            "{\"count\":null}",
        };
        static const char *want[] = {
            "{\"count\":7}",
            "{}",
        };
        size_t i;

        for (i = 0; i < sizeof(in) / sizeof(in[0]); i++) {
            CHECK_OK(
                pbw_json_parse(msg, in[i], strlen(in[i]), &decoded));
            CHECK_OK(pbw_json_print(decoded, &out, &len));
            CHECK(len == strlen(want[i]) &&
                  memcmp(out, want[i], len) == 0);
            pbw_free(out);
            out = NULL;
            pbw_message_free(decoded);
            decoded = NULL;
        }
        /* Enum keys (unrepresentable in .proto, buildable by hand) print
         * by name when known, by number otherwise. */
        {
            const pbw_descriptor *ke = desc(schema, "M.EkeyEntry");
            const pbw_field *kf;
            pbw_message *m2 = NULL;
            pbw_message *e1 = NULL;
            pbw_message *e2 = NULL;
            pbw_value *v = NULL;

            CHECK(ke != NULL);
            kf = pbw_descriptor_find_field(msg, 33);
            CHECK(kf != NULL);
            (void)kf;
            m2 = pbw_message_new(msg);
            CHECK(m2 != NULL);
            e1 = pbw_message_new(ke);
            e2 = pbw_message_new(ke);
            CHECK(e1 != NULL && e2 != NULL);
            if (m2 != NULL && e1 != NULL && e2 != NULL) {
                CHECK_OK(pbw_message_set(e1, 1,
                                         pbw_value_new_int(1)));
                CHECK_OK(pbw_message_set(e1, 2,
                                         pbw_value_new_int(10)));
                CHECK_OK(pbw_message_set(e2, 1,
                                         pbw_value_new_int(5)));
                CHECK_OK(pbw_message_set(e2, 2,
                                         pbw_value_new_int(20)));
                v = pbw_value_new_message(e1);
                e1 = NULL;
                CHECK_OK(pbw_message_set(m2, 33, v));
                v = pbw_value_new_message(e2);
                e2 = NULL;
                CHECK_OK(pbw_message_set(m2, 33, v));
                CHECK_OK(pbw_json_print(m2, &out, &len));
                CHECK(len == strlen("{\"ekey\":{\"C_RED\":10,\"5\":20}}") &&
                      memcmp(out, "{\"ekey\":{\"C_RED\":10,\"5\":20}}",
                             len) == 0);
                pbw_free(out);
                out = NULL;
                pbw_message_free(m2);
            } else {
                pbw_message_free(e1);
                pbw_message_free(e2);
                pbw_message_free(m2);
            }
        }
    }
    /* 32-bit unsigned integers print without sign extension. */
    {
        static const char in[] = "{\"u32\":4294967295}";

        CHECK_OK(pbw_json_parse(msg, in, strlen(in), &decoded));
        CHECK_OK(pbw_json_print(decoded, &out, &len));
        CHECK(len == strlen(in) && memcmp(out, in, len) == 0);
        pbw_free(out);
        out = NULL;
        pbw_message_free(decoded);
        decoded = NULL;
    }
    /* Trailing commas and empty objects are legacy-valid; they print in
     * canonical form. */
    {
        static const char *in[] = {
            "{\"props\":{\"l\":[1,]}}",
            "{\"lv\":{}}",
            "{\"props\":{\"k\":1,}}",
            "{\"scores\":{\"a\":1,}}",
        };
        static const char *want[] = {
            "{\"props\":{\"l\":[1]}}",
            "{\"lv\":[]}",
            "{\"props\":{\"k\":1}}",
            "{\"scores\":{\"a\":1}}",
        };
        size_t i;

        for (i = 0; i < sizeof(in) / sizeof(in[0]); i++) {
            CHECK_OK(
                pbw_json_parse(msg, in[i], strlen(in[i]), &decoded));
            CHECK_OK(pbw_json_print(decoded, &out, &len));
            CHECK(len == strlen(want[i]) &&
                  memcmp(out, want[i], len) == 0);
            pbw_free(out);
            out = NULL;
            pbw_message_free(decoded);
            decoded = NULL;
        }
    }
    /* Base64 prints canonically regardless of input alphabet. */
    {
        static const char *in[] = {
            "{\"blob\":\"AP8-\"}",
            "{\"blob\":\"AP8_\"}",
            "{\"blob\":\"AQI=\"}",
            "{\"blob\":\"AQ==\"}",
            "{\"blob\":\"\"}",
        };
        static const char *want[] = {
            "{\"blob\":\"AP8+\"}",
            "{\"blob\":\"AP8/\"}",
            "{\"blob\":\"AQI=\"}",
            "{\"blob\":\"AQ==\"}",
            /* proto3 implicit presence: an empty bytes value is the default,
             * so the reference printer omits it. */
            "{}",
        };
        size_t i;

        for (i = 0; i < sizeof(in) / sizeof(in[0]); i++) {
            CHECK_OK(
                pbw_json_parse(msg, in[i], strlen(in[i]), &decoded));
            CHECK_OK(pbw_json_print(decoded, &out, &len));
            CHECK(len == strlen(want[i]) &&
                  memcmp(out, want[i], len) == 0);
            pbw_free(out);
            out = NULL;
            pbw_message_free(decoded);
            decoded = NULL;
        }
    }
    /* An overlong type URL fails instead of truncating. */
    {
        char big[512];
        size_t at = 0;
        const char head[] = "{\"payload\":{\"@type\":\"type.googleapis.com/";
        size_t i;

        memcpy(big + at, head, sizeof(head) - 1);
        at += sizeof(head) - 1;
        for (i = 0; i < 300; i++) {
            big[at++] = 'A';
        }
        memcpy(big + at, "\"}}", 3);
        at += 3;
        CHECK(at < sizeof(big));
        CHECK_STATUS(PBW_ERR_INVALID,
                     pbw_json_parse(msg, big, at, &decoded));
        CHECK(decoded == NULL);
    }
    /* Trailing commas in Any objects are rejected. */
    {
        static const char in[] = "{\"payload\":{\"a\":1,}}";

        decoded = NULL;
        CHECK_STATUS(PBW_ERR_INVALID,
                     pbw_json_parse(msg, in, strlen(in), &decoded));
        CHECK(decoded == NULL);
    }
    /* An unresolved Any keeps its bytes; printing shows an empty object.
     * Trailing commas are tolerated like elsewhere. */
    {
        pbw_json_parse_options opts;
        static const char want[] = "{\"payload\":{}}";
        static const char trailing[] =
            "{\"payload\":{\"@type\":"
            "\"type.googleapis.com/Sub\",\"a\":1,}}";

        opts.ignore_unknown_fields = 1;
        decoded = NULL;
        CHECK_OK(pbw_json_parse_opts(
            msg,
            "{\"payload\":{\"@type\":\"type.googleapis.com/Nope\"}}",
            strlen("{\"payload\":{\"@type\":\"type.googleapis.com/Nope\"}}"),
            &opts, &decoded));
        CHECK_OK(pbw_json_print(decoded, &out, &len));
        CHECK(len == sizeof(want) - 1 &&
              memcmp(out, want, len) == 0);
        pbw_free(out);
        out = NULL;
        pbw_message_free(decoded);
        decoded = NULL;
        decoded = NULL;
        CHECK_OK(pbw_json_parse_opts(msg, trailing, strlen(trailing),
                                     &opts, &decoded));
        CHECK_OK(pbw_json_print(decoded, &out, &len));
        {
            /* trailing comma dropped, value form preserved */
            static const char want2[] =
                "{\"payload\":{\"@type\":"
                "\"type.googleapis.com/Sub\",\"a\":1}}";

            CHECK(len == sizeof(want2) - 1 &&
                  memcmp(out, want2, len) == 0);
        }
        pbw_free(out);
        out = NULL;
        pbw_message_free(decoded);
        decoded = NULL;
    }
    /* value bytes without a type URL cannot print */
    {
        pbw_message *submsg;

        decoded = pbw_message_new(msg);
        CHECK(decoded != NULL);
        submsg = pbw_message_set_message(decoded, 17);
        CHECK(submsg != NULL);
        CHECK_OK(pbw_message_set(submsg, 2,
                                 pbw_value_new_bytes("x", 1)));
        CHECK_STATUS(PBW_ERR_INVALID,
                     pbw_json_print(decoded, &out, &len));
        CHECK(out == NULL);
        /* a read-only context tree rejects unknown bytes and stores */
        {
            pbw_ctx *ctx = pbw_ctx_new();
            pbw_message *dctx = NULL;
            uint8_t *dwire = NULL;
            size_t dwire_len = 0;

            CHECK(ctx != NULL);
            CHECK_OK(pbw_encode(decoded, &dwire, &dwire_len));
            CHECK_OK(
                pbw_decode_ctx(ctx, msg, dwire, dwire_len, &dctx));
            CHECK(dctx != NULL);
            CHECK_STATUS(PBW_ERR_INVALID,
                         pbw_message_add_unknown_bytes(dctx, "x", 1));
            {
                pbw_value *v = pbw_value_new_int(1);

                /* rejected values stay owned by the caller */
                CHECK_STATUS(PBW_ERR_INVALID, pbw_message_set(dctx, 1, v));
                pbw_value_free(v);
            }
            pbw_ctx_free(ctx);
            pbw_free(dwire);
        }
        pbw_message_free(decoded);
        decoded = NULL;
    }
    /* A partial google.protobuf.Any (missing value field) fails cleanly. */
    {
        pbw_schema *s2 = pbw_schema_new();
        pbw_descriptor *anymsg;
        pbw_descriptor *holder;
        pbw_field *f;

        CHECK(s2 != NULL);
        anymsg = pbw_schema_add_message(s2, "google.protobuf.Any");
        CHECK(anymsg != NULL);
        CHECK(pbw_message_add_field(anymsg, "type_url", 1,
                                    PBW_TYPE_STRING,
                                    PBW_LABEL_OPTIONAL) != NULL);
        holder = pbw_schema_add_message(s2, "H");
        CHECK(holder != NULL);
        f = pbw_message_add_field(holder, "payload", 1,
                                  PBW_TYPE_MESSAGE, PBW_LABEL_OPTIONAL);
        CHECK(f != NULL);
        pbw_field_set_type_name(f, "google.protobuf.Any");
        CHECK_OK(pbw_schema_link(s2));
        decoded = NULL;
        CHECK_STATUS(PBW_ERR_INVALID,
                     pbw_text_parse(holder,
                                    "payload [type.googleapis.com/H] {}",
                                    38, &decoded));
        CHECK(decoded == NULL);
        /* Bad percent-escapes in the type URL fail. */
        CHECK_STATUS(PBW_ERR_INVALID,
                     pbw_text_parse(holder,
                                    "payload [type.googleapis.com/%ZZ] {}",
                                    36, &decoded));
        CHECK(decoded == NULL);
        CHECK_STATUS(PBW_ERR_INVALID,
                     pbw_text_parse(holder,
                                    "payload [type.googleapis.com/%4] {}",
                                    35, &decoded));
        CHECK(decoded == NULL);
        CHECK_STATUS(PBW_ERR_INVALID,
                     pbw_json_parse(holder, "{\"payload\":{}}", 15,
                                    &decoded));
        CHECK(decoded == NULL);
        pbw_schema_free(s2);
    }
    /* An empty Value in a map prints as an empty object. A truly empty
     * Value (built programmatically, no fields set) is skipped. */
    {
        static const char in[] = "{\"vmap\":{\"a\":1,\"b\":{}}}";
        static const char want[] = "{\"vmap\":{\"a\":1,\"b\":{}}}";
        pbw_message *slot;

        decoded = NULL;
        CHECK_OK(pbw_json_parse(msg, in, strlen(in), &decoded));
        CHECK_OK(pbw_json_print(decoded, &out, &len));
        CHECK(len == strlen(want) && memcmp(out, want, len) == 0);
        pbw_free(out);
        out = NULL;
        slot = pbw_message_set_message(decoded, 32);
        CHECK(slot != NULL);
        CHECK_OK(pbw_message_set(slot, 1, pbw_value_new_string("z")));
        {
            pbw_message *empty = pbw_message_new(
                pbw_schema_find(schema, "google.protobuf.Value"));

            CHECK(empty != NULL);
            CHECK_OK(pbw_message_set(slot, 2,
                                     pbw_value_new_message(empty)));
        }
        CHECK_OK(pbw_json_print(decoded, &out, &len));
        CHECK(strstr(out, "\"z\":") == NULL);
        CHECK(strstr(out, "\"a\":1") != NULL);
        pbw_free(out);
        out = NULL;
        pbw_message_free(decoded);
        decoded = NULL;
        /* Empty Values in a repeated field are skipped when printing. */
        {
            pbw_message *m2 = pbw_message_new(msg);
            pbw_message *one = NULL;
            pbw_message *two = NULL;

            CHECK(m2 != NULL);
            one = pbw_message_new(
                pbw_schema_find(schema, "google.protobuf.Value"));
            two = pbw_message_new(
                pbw_schema_find(schema, "google.protobuf.Value"));
            CHECK(one != NULL && two != NULL);
            if (m2 != NULL && one != NULL && two != NULL) {
                CHECK_OK(pbw_message_set(one, 3,
                                         pbw_value_new_string("x")));
                CHECK_OK(pbw_message_set(m2, 34,
                                         pbw_value_new_message(one)));
                CHECK_OK(pbw_message_set(m2, 34,
                                         pbw_value_new_message(two)));
                CHECK_OK(pbw_json_print(m2, &out, &len));
                CHECK(len == strlen("{\"vrep\":[\"x\"]}") &&
                      memcmp(out, "{\"vrep\":[\"x\"]}", len) == 0);
                pbw_free(out);
                out = NULL;
                pbw_message_free(m2);
            } else {
                pbw_message_free(one);
                pbw_message_free(two);
                pbw_message_free(m2);
            }
        }
    }
    /* Unknown fields fail unless ignored; unknown Any types too. */
    decoded = NULL;
    CHECK_STATUS(PBW_ERR_INVALID,
                 pbw_json_parse(msg, "{\"zzz\":1}", 9, &decoded));
    CHECK(decoded == NULL);
    {
        pbw_json_parse_options opts;

        opts.ignore_unknown_fields = 1;
        CHECK_OK(
            pbw_json_parse_opts(msg, "{\"zzz\":1}", 9, &opts, &decoded));
        pbw_message_free(decoded);
        decoded = NULL;
        CHECK_OK(pbw_json_parse_opts(
            msg,
            "{\"payload\":{\"@type\":\"type.googleapis.com/Nope\"}}",
            48, &opts, &decoded));
        pbw_message_free(decoded);
        decoded = NULL;
    }
    /* Durations and field masks print their canonical forms. */
    {
        static const char *in[] = {
            "{\"howLong\":\"7s\"}",
            "{\"howLong\":\"-3.5s\"}",
            "{\"howLong\":\"0.000000001s\"}",
            "{\"howLong\":null}",
            "{\"mask\":\"fooBar,baz\"}",
            "{\"mask\":\"\"}",
            "{\"mask\":\"foo_bar\"}",
            "{\"mask\":\"foo_-bar\"}",
        };
        /* millis precision is kept, not trimmed to one digit */
        static const char *want[] = {
            "{\"howLong\":\"7s\"}",
            "{\"howLong\":\"-3.500s\"}",
            "{\"howLong\":\"0.000000001s\"}",
            "{}",
            "{\"mask\":\"fooBar,baz\"}",
            "{\"mask\":\"\"}",
            "{\"mask\":\"fooBar\"}",
            "{\"mask\":\"foo_-bar\"}",
        };
        size_t i;

        for (i = 0; i < sizeof(in) / sizeof(in[0]); i++) {
            CHECK_OK(
                pbw_json_parse(msg, in[i], strlen(in[i]), &decoded));
            CHECK_OK(pbw_json_print(decoded, &out, &len));
            CHECK(len == strlen(want[i]) &&
                  memcmp(out, want[i], len) == 0);
            pbw_free(out);
            out = NULL;
            pbw_message_free(decoded);
            decoded = NULL;
        }
    }
    /* Ignored unknowns skip nested objects, arrays and strings. */
    {
        pbw_json_parse_options opts;

        opts.ignore_unknown_fields = 1;
        CHECK_OK(pbw_json_parse_opts(
            msg,
            "{\"u\":{\"a\":[1,{\"b\":2}],\"s\":\"x\"},"
            "\"arr\":[[1]],\"i32\":1}",
            strlen("{\"u\":{\"a\":[1,{\"b\":2}],\"s\":\"x\"},"
                   "\"arr\":[[1]],\"i32\":1}"),
            &opts, &decoded));
        CHECK_OK(pbw_json_print(decoded, &out, &len));
        CHECK(len == 9 && memcmp(out, "{\"i32\":1}", len) == 0);
        pbw_free(out);
        out = NULL;
        pbw_message_free(decoded);
        decoded = NULL;
        /* truncated and mistyped unknowns still fail, even ignored */
        {
            static const char *badskip[] = {
                "{\"u\":{\"a\":1",
                "{\"u\":tru}",
                "{\"u\":[1}",
                "{\"u\":\"abc",
                "{\"u\":12x}",
                "{\"u\":nul}",
                "{\"u\":]}",
                "{\"u\":}",
                "{\"u\":fals}",
                "{\"u\":{\"a\":1,",
                "{\"u\":[1,tru]",
                "{\"u\":[[1]}",
            };
            size_t k;

            for (k = 0; k < sizeof(badskip) / sizeof(badskip[0]); k++) {
                CHECK_STATUS(PBW_ERR_INVALID,
                             pbw_json_parse_opts(msg, badskip[k],
                                                 strlen(badskip[k]),
                                                 &opts, &decoded));
                CHECK(decoded == NULL);
            }
        }
        /* empty arrays and objects skip cleanly, as do false and null */
        {
            static const char skips[] =
                "{\"u\":[],\"v\":{},\"w\":false,\"x\":null,\"i32\":2}";

            CHECK_OK(pbw_json_parse_opts(msg, skips, strlen(skips),
                                         &opts, &decoded));
            CHECK_OK(pbw_json_print(decoded, &out, &len));
            CHECK(len == 9 && memcmp(out, "{\"i32\":2}", len) == 0);
            pbw_free(out);
            out = NULL;
            pbw_message_free(decoded);
            decoded = NULL;
        }
        /* deeply nested unknowns stop at the depth cap */
        {
            char deep[4096];
            size_t at = 0;
            int i;

            for (i = 0; i < 300; i++) {
                memcpy(deep + at, "{\"u\":", 5);
                at += 5;
            }
            deep[at++] = '1';
            for (i = 0; i < 300; i++) {
                deep[at++] = '}';
            }
            CHECK(at < sizeof(deep));
            CHECK_STATUS(PBW_ERR_LIMIT,
                         pbw_json_parse_opts(msg, deep, at, &opts,
                                             &decoded));
            CHECK(decoded == NULL);
            /* deeply nested arrays stop at the depth cap too */
            at = 0;
            for (i = 0; i < 300; i++) {
                memcpy(deep + at, "{\"u\":[", 6);
                at += 6;
            }
            deep[at++] = '1';
            for (i = 0; i < 300; i++) {
                deep[at++] = ']';
            }
            for (i = 0; i < 300; i++) {
                deep[at++] = '}';
            }
            CHECK(at < sizeof(deep));
            decoded = NULL;
            CHECK_STATUS(PBW_ERR_LIMIT,
                         pbw_json_parse_opts(msg, deep, at, &opts,
                                             &decoded));
            CHECK(decoded == NULL);
        }
    }
    /* A map round-trips through parse and print. */
    CHECK_OK(pbw_json_parse(msg, "{\"scores\":{\"b\":2,\"a\":1}}", 24,
                            &decoded));
    CHECK_OK(pbw_json_print(decoded, &out, &len));
    CHECK(len == 24 && memcmp(out, "{\"scores\":{\"b\":2,\"a\":1}}",
                              len) == 0);
    pbw_free(out);
    out = NULL;
    pbw_message_free(decoded);
    decoded = NULL;
    /* Timestamps print whole and fractional seconds (civil-date path). */
    {
        static const char *ts[] = {
            "{\"when\":\"1970-01-01T00:00:00Z\"}",
            "{\"when\":\"1970-01-01T00:00:00.5Z\"}",
            "{\"when\":\"2026-11-07T01:02:03.123456789Z\"}",
        };
        static const char *want[] = {
            "{\"when\":\"1970-01-01T00:00:00Z\"}",
            "{\"when\":\"1970-01-01T00:00:00.500Z\"}",
            "{\"when\":\"2026-11-07T01:02:03.123456789Z\"}",
        };
        size_t i;

        for (i = 0; i < sizeof(ts) / sizeof(ts[0]); i++) {
            CHECK_OK(
                pbw_json_parse(msg, ts[i], strlen(ts[i]), &decoded));
            CHECK_OK(pbw_json_print(decoded, &out, &len));
            CHECK(len == strlen(want[i]) &&
                  memcmp(out, want[i], len) == 0);
            pbw_free(out);
            out = NULL;
            pbw_message_free(decoded);
            decoded = NULL;
        }
    }
    /* Struct/Value/ListValue print every kind; printing parses back to
     * the same bytes (covers null, nesting and the list path). */
    {
        static const char *stext =
            "{\"props\":{\"k\":1,\"n\":null,\"s\":\"x\","
            "\"b\":true,\"l\":[1,{\"z\":0}],\"o\":{\"w\":[]}}}";
        char *again = NULL;
        size_t again_len = 0;

        CHECK_OK(pbw_json_parse(msg, stext, strlen(stext), &decoded));
        CHECK_OK(pbw_json_print(decoded, &out, &len));
        CHECK(strstr(out, "\"n\":null") != NULL);
        pbw_message_free(decoded);
        decoded = NULL;
        CHECK_OK(pbw_json_parse(msg, out, len, &decoded));
        CHECK_OK(pbw_json_print(decoded, &again, &again_len));
        CHECK(again_len == len && memcmp(again, out, len) == 0);
        pbw_free(out);
        out = NULL;
        pbw_free(again);
        pbw_message_free(decoded);
        decoded = NULL;
    }
    /* Message-, enum- and int-keyed maps round-trip. */
    {
        static const char *in[] = {
            "{\"ecol\":{\"a\":\"C_RED\",\"b\":0}}",
            "{\"ecol\":{\"a\":99}}",
            "{\"imap\":{\"-1\":2,\"10\":3}}",
            "{\"umap\":{\"1\":2,\"4294967295\":3}}",
            "{\"emsg\":{\"k\":{\"a\":1}}}",
            "{\"emsg\":{}}",
            "{\"bmap\":{\"true\":1,\"false\":2}}",
            "{\"lmap\":{\"-5\":1,\"42\":2}}",
            "{\"lv\":[1,\"x\",true,null]}",
            "{\"em\":{}}",
            "{\"vv\":1}",
            "{\"kid\":null}",
            "{\"lv\":null}",
        };
        static const char *want[] = {
            "{\"ecol\":{\"a\":\"C_RED\",\"b\":\"C_ZERO\"}}",
            "{\"ecol\":{\"a\":99}}",
            "{\"imap\":{\"-1\":2,\"10\":3}}",
            "{\"umap\":{\"1\":2,\"4294967295\":3}}",
            "{\"emsg\":{\"k\":{\"a\":1}}}",
            "{}", /* empty maps stay out of the print */
            "{\"bmap\":{\"true\":1,\"false\":2}}",
            "{\"lmap\":{\"-5\":1,\"42\":2}}",
            "{\"lv\":[1,\"x\",true,null]}",
            "{\"em\":{}}",
            "{\"vv\":1}",
            "{}",
            "{}",
        };
        size_t i;

        for (i = 0; i < sizeof(in) / sizeof(in[0]); i++) {
            CHECK_OK(
                pbw_json_parse(msg, in[i], strlen(in[i]), &decoded));
            CHECK_OK(pbw_json_print(decoded, &out, &len));
            CHECK(len == strlen(want[i]) &&
                  memcmp(out, want[i], len) == 0);
            pbw_free(out);
            out = NULL;
            pbw_message_free(decoded);
            decoded = NULL;
        }
        /* bad keys, bad values and duplicate keys fail */
        {
            static const char *badmap[] = {
                "{\"imap\":{\"x\":1}}",
                "{\"ecol\":{\"a\":\"BOGUS\"}}",
                "{\"emsg\":{\"a\":1}}",
                "{\"emsg\":{\"k\":{\"a\":1},\"k\":{\"a\":2}}}",
                "{\"imap\":{\"1\":1,\"1\":2}}",
                "{\"bmap\":{\"yes\":1}}",
                "{\"bmap\":{\"True\":1}}",
                "{\"lmap\":{\"0x10\":1}}",
                "{\"umap\":{\"x\":1}}",
                "{\"umap\":{\"4294967296\":1}}",
                "{\"lv\":1}",
                "{\"em\":1}",
            };
            size_t k;

            for (k = 0; k < sizeof(badmap) / sizeof(badmap[0]); k++) {
                CHECK_STATUS(PBW_ERR_INVALID,
                             pbw_json_parse(msg, badmap[k],
                                            strlen(badmap[k]), &decoded));
                CHECK(decoded == NULL);
            }
        }
    }
    /* An unlinked schema prints through the malloc+sort path: field
     * pointers (not numbers) put values in place, and printing still
     * orders them by field number. */
    {
        pbw_schema *s2 = pbw_schema_new();
        pbw_descriptor *um;
        pbw_message *u = NULL;
        static const char want[] = "{\"a\":\"x\",\"b\":7}";

        CHECK(s2 != NULL);
        um = pbw_schema_add_message(s2, "U");
        CHECK(um != NULL);
        CHECK(pbw_message_add_field(um, "b", 2, PBW_TYPE_INT32,
                                    PBW_LABEL_OPTIONAL) != NULL);
        CHECK(pbw_message_add_field(um, "a", 1, PBW_TYPE_STRING,
                                    PBW_LABEL_OPTIONAL) != NULL);
        u = pbw_message_new(um);
        CHECK(u != NULL);
        if (u != NULL) {
            CHECK_OK(pbw_message_set_field(u, &um->fields[0],
                                           pbw_value_new_int(7)));
            CHECK_OK(pbw_message_set_field(u, &um->fields[1],
                                           pbw_value_new_string("x")));
            CHECK_OK(pbw_json_print(u, &out, &len));
            CHECK(len == sizeof(want) - 1 &&
                  memcmp(out, want, len) == 0);
            pbw_free(out);
            out = NULL;
            pbw_message_free(u);
        }
        pbw_schema_free(s2);
    }
    /* Fields decode in wire order but print sorted by field number. */
    {
        /* name(5)="x" then i32(1)=7 on the wire */
        static const uint8_t reversed[] = {0x2A, 0x01, 0x78, 0x08,
                                           0x07};
        static const char want[] = "{\"i32\":7,\"name\":\"x\"}";

        decoded = NULL;
        CHECK_OK(pbw_decode(msg, reversed, sizeof(reversed), &decoded));
        CHECK_OK(pbw_json_print(decoded, &out, &len));
        CHECK(len == sizeof(want) - 1 &&
              memcmp(out, want, len) == 0);
        pbw_free(out);
        out = NULL;
        pbw_message_free(decoded);
        decoded = NULL;
    }
    /* Binary input may repeat a map key; printing keeps the last. */
    {
        static const uint8_t dupmap[] = {
            0xA2,
            0x01,
            0x05,
            0x0A,
            0x01,
            0x61,
            0x10,
            0x01,
            0xA2,
            0x01,
            0x05,
            0x0A,
            0x01,
            0x61,
            0x10,
            0x02,
        };
        static const char want[] = "{\"scores\":{\"a\":2}}";

        decoded = NULL;
        CHECK_OK(pbw_decode(msg, dupmap, sizeof(dupmap), &decoded));
        CHECK_OK(pbw_json_print(decoded, &out, &len));
        CHECK(len == sizeof(want) - 1 &&
              memcmp(out, want, len) == 0);
        pbw_free(out);
        out = NULL;
        /* a NULL entry fails the print instead of crashing */
        {
            pbw_value *list = pbw_message_get(decoded, 20);

            CHECK(list != NULL && list->kind == PBW_VALUE_LIST);
            list->as.list.items[0] = NULL;
            CHECK_STATUS(PBW_ERR_INVALID,
                         pbw_json_print(decoded, &out, &len));
            CHECK(out == NULL);
        }
        pbw_message_free(decoded);
        decoded = NULL;
    }
    /* Past 32 entries map printing takes the precomputed-winners path;
     * distinct keys keep input order and survive a re-parse. */
    {
        char big[1600];
        size_t off = 0;
        int i;

        off += (size_t)snprintf(big + off, sizeof(big) - off,
                                "{\"scores\":{");
        for (i = 0; i < 40; i++) {
            off += (size_t)snprintf(big + off, sizeof(big) - off,
                                    "%s\"k%d\":%d", i ? "," : "",
                                    i, i);
        }
        off += (size_t)snprintf(big + off, sizeof(big) - off, "}}");
        CHECK(off < sizeof(big));
        CHECK_OK(pbw_json_parse(msg, big, off, &decoded));
        /* a duplicate shadows the first occurrence; partial entries
         * print with default keys and values */
        {
            pbw_message *slot;

            slot = pbw_message_set_message(decoded, 20);
            CHECK(slot != NULL);
            CHECK_OK(pbw_message_set(slot, 1, pbw_value_new_string("k0")));
            CHECK_OK(pbw_message_set(slot, 2, pbw_value_new_int(1000)));
            slot = pbw_message_set_message(decoded, 20);
            CHECK(slot != NULL);
            CHECK_OK(pbw_message_set(slot, 2, pbw_value_new_int(7)));
            slot = pbw_message_set_message(decoded, 20);
            CHECK(slot != NULL);
            CHECK_OK(
                pbw_message_set(slot, 1, pbw_value_new_string("zz")));
        }
        CHECK_OK(pbw_json_print(decoded, &out, &len));
        CHECK(strstr(out, "\"k39\":39") != NULL);
        CHECK(strstr(out, "\"k0\":1000") != NULL);
        CHECK(strstr(out, "\"zz\":0") != NULL);
        CHECK(strstr(out, "\"\":7") != NULL);
        pbw_free(out);
        out = NULL;
        pbw_message_free(decoded);
        decoded = NULL;
    }
    /* All-unique large maps free the shadow table and scan as before. */
    {
        char big[1600];
        size_t off = 0;
        int i;

        off += (size_t)snprintf(big + off, sizeof(big) - off,
                                "{\"scores\":{");
        for (i = 0; i < 40; i++) {
            off += (size_t)snprintf(big + off, sizeof(big) - off,
                                    "%s\"u%d\":%d", i ? "," : "",
                                    i, i);
        }
        off += (size_t)snprintf(big + off, sizeof(big) - off, "}}");
        CHECK(off < sizeof(big));
        CHECK_OK(pbw_json_parse(msg, big, off, &decoded));
        CHECK_OK(pbw_json_print(decoded, &out, &len));
        CHECK(strstr(out, "\"u39\":39") != NULL);
        pbw_free(out);
        out = NULL;
        pbw_message_free(decoded);
        decoded = NULL;
    }
    /* Proto3 zero fields stay out of the print. */
    CHECK_OK(pbw_json_parse(msg, "{\"i32\":0}", 9, &decoded));
    CHECK_OK(pbw_json_print(decoded, &out, &len));
    CHECK(len == 2 && memcmp(out, "{}", 2) == 0);
    pbw_free(out);
    pbw_message_free(decoded);
    /* A missing proto2 required field fails with REQUIRED, not INVALID. */
    {
        pbw_schema *s2 = pbw_schema_new();
        pbw_descriptor *req;

        CHECK(s2 != NULL);
        pbw_schema_set_proto3(s2, 0);
        req = pbw_schema_add_message(s2, "R");
        CHECK(req != NULL);
        CHECK(pbw_message_add_field(req, "a", 1, PBW_TYPE_INT32,
                                    PBW_LABEL_REQUIRED) != NULL);
        CHECK_OK(pbw_schema_link(s2));
        decoded = NULL;
        CHECK_STATUS(PBW_ERR_REQUIRED,
                     pbw_json_parse(req, "{}", 2, &decoded));
        CHECK(decoded == NULL);
        CHECK_OK(pbw_json_parse(req, "{\"a\":1}", 7, &decoded));
        CHECK(pbw_value_get_int32(pbw_message_get(decoded, 1)) == 1);
        pbw_message_free(decoded);
        decoded = NULL;
        pbw_schema_free(s2);
    }
    check_json_prefix_sweep(msg);
    pbw_schema_free(schema);
}

struct sink_buf {
    uint8_t *data;
    size_t len;
    size_t cap;
    pbw_status inject; /* returned instead of PBW_OK once armed */
    int fail_at;       /* 1-based sink call to fail, 0 = never */
    int calls;
};

static pbw_status
collect_sink(const uint8_t *data, size_t len, void *opaque)
{
    struct sink_buf *s = (struct sink_buf *)opaque;
    uint8_t *grown;
    size_t cap;

    if (s->inject != PBW_OK) {
        s->calls++;
        if (s->fail_at <= 0 || s->calls >= s->fail_at) {
            return s->inject;
        }
    }
    cap = s->cap != 0 ? s->cap : 64;
    if (len > cap - s->len) {
        while (cap - s->len < len) {
            cap *= 2;
        }
        grown = (uint8_t *)realloc(s->data, cap);
        if (grown == NULL) {
            return PBW_ERR_NOMEM;
        }
        s->data = grown;
        s->cap = cap;
    } else if (s->data == NULL) {
        /* First chunk fit the initial guess but the buffer is not out yet. */
        s->data = (uint8_t *)malloc(cap);
        if (s->data == NULL) {
            return PBW_ERR_NOMEM;
        }
        s->cap = cap;
    }
    memcpy(s->data + s->len, data, len);
    s->len += len;
    return PBW_OK;
}

static const char *
entry_key(const pbw_value *list, size_t i)
{
    return pbw_value_get_string(
        pbw_message_get(pbw_value_get_message(pbw_value_at(list, i)), 1));
}

static void
test_deterministic(void)
{
    pbw_schema *schema = pbw_schema_new();
    pbw_descriptor *entry;
    pbw_descriptor *holder;
    pbw_descriptor *ientry;
    pbw_descriptor *iholder;
    pbw_field *field;
    pbw_message *msg;
    pbw_message *sub;
    pbw_message *decoded = NULL;
    pbw_value *entries;
    uint8_t *plain = NULL;
    uint8_t *det = NULL;
    uint8_t *det2 = NULL;
    size_t plain_len = 0;
    size_t det_len = 0;
    size_t det2_len = 0;
    static const char *keys[] = {"c", "a", "b"};
    static const int32_t vals[] = {3, 1, 2};
    static const int32_t ikeys[] = {3, -5, 0};
    size_t i;

    CHECK(schema != NULL);
    entry = pbw_schema_add_message(schema, "SEntry");
    pbw_message_add_field(entry, "key", 1, PBW_TYPE_STRING,
                          PBW_LABEL_OPTIONAL);
    pbw_message_add_field(entry, "value", 2, PBW_TYPE_INT32,
                          PBW_LABEL_OPTIONAL);
    pbw_descriptor_set_map_entry(entry);
    holder = pbw_schema_add_message(schema, "SMap");
    field = pbw_message_add_field(holder, "entries", 1, PBW_TYPE_MESSAGE,
                                  PBW_LABEL_REPEATED);
    pbw_field_set_type_name(field, "SEntry");
    ientry = pbw_schema_add_message(schema, "IEntry");
    pbw_message_add_field(ientry, "key", 1, PBW_TYPE_INT32,
                          PBW_LABEL_OPTIONAL);
    pbw_message_add_field(ientry, "value", 2, PBW_TYPE_INT32,
                          PBW_LABEL_OPTIONAL);
    pbw_descriptor_set_map_entry(ientry);
    iholder = pbw_schema_add_message(schema, "IMap");
    field = pbw_message_add_field(iholder, "entries", 1, PBW_TYPE_MESSAGE,
                                  PBW_LABEL_REPEATED);
    pbw_field_set_type_name(field, "IEntry");
    /* Keyless entries keep insertion order even when deterministic. */
    {
        pbw_descriptor *kentry = pbw_schema_add_message(schema, "KEntry");
        pbw_descriptor *kholder;

        pbw_message_add_field(kentry, "value", 2, PBW_TYPE_INT32,
                              PBW_LABEL_OPTIONAL);
        pbw_descriptor_set_map_entry(kentry);
        kholder = pbw_schema_add_message(schema, "KMap");
        field = pbw_message_add_field(kholder, "entries", 1,
                                      PBW_TYPE_MESSAGE,
                                      PBW_LABEL_REPEATED);
        pbw_field_set_type_name(field, "KEntry");
    }
    CHECK_OK(pbw_schema_link(schema));

    /* Insert out of order: plain encode keeps it, deterministic sorts it. */
    msg = pbw_message_new(desc(schema, "SMap"));
    for (i = 0; i < 3; i++) {
        sub = pbw_message_set_message(msg, 1);
        CHECK(sub != NULL);
        CHECK_OK(pbw_message_set(sub, 1, pbw_value_new_string(keys[i])));
        CHECK_OK(pbw_message_set(sub, 2, pbw_value_new_int(vals[i])));
    }
    CHECK_OK(pbw_encode(msg, &plain, &plain_len));
    CHECK_OK(pbw_encode_deterministic(msg, &det, &det_len));
    CHECK(plain_len == det_len);
    CHECK(memcmp(plain, det, plain_len) != 0);
    CHECK_OK(pbw_decode(desc(schema, "SMap"), det, det_len, &decoded));
    entries = pbw_message_get(decoded, 1);
    CHECK(pbw_value_count(entries) == 3);
    CHECK(strcmp(entry_key(entries, 0), "a") == 0);
    CHECK(pbw_value_get_int32(pbw_message_get(
              pbw_value_get_message(pbw_value_at(entries, 0)), 2)) == 1);
    CHECK(strcmp(entry_key(entries, 1), "b") == 0);
    CHECK(strcmp(entry_key(entries, 2), "c") == 0);
    pbw_message_free(decoded);
    decoded = NULL;
    /* Deterministic output is stable across runs. */
    CHECK_OK(pbw_encode_deterministic(msg, &det2, &det2_len));
    CHECK(det_len == det2_len && memcmp(det, det2, det_len) == 0);
    pbw_free(det2);
    pbw_free(det);
    det = NULL;
    pbw_message_free(msg);
    msg = NULL;

    /* Keyless entries keep insertion order even when deterministic. */
    msg = pbw_message_new(desc(schema, "KMap"));
    for (i = 0; i < 2; i++) {
        sub = pbw_message_set_message(msg, 1);
        CHECK(sub != NULL);
        CHECK_OK(pbw_message_set(sub, 2, pbw_value_new_int(7 - (int)i)));
    }
    CHECK_OK(pbw_encode_deterministic(msg, &det, &det_len));
    CHECK_OK(pbw_decode(desc(schema, "KMap"), det, det_len, &decoded));
    entries = pbw_message_get(decoded, 1);
    CHECK(pbw_value_get_int32(pbw_message_get(
              pbw_value_get_message(pbw_value_at(entries, 0)), 2)) == 7);
    pbw_message_free(decoded);
    decoded = NULL;
    pbw_free(det);
    det = NULL;
    pbw_message_free(msg);
    msg = NULL;

    /* Signed integer keys sort numerically, not by raw bits. */
    msg = pbw_message_new(desc(schema, "IMap"));
    for (i = 0; i < 3; i++) {
        sub = pbw_message_set_message(msg, 1);
        CHECK(sub != NULL);
        CHECK_OK(pbw_message_set(sub, 1, pbw_value_new_int(ikeys[i])));
        CHECK_OK(pbw_message_set(sub, 2, pbw_value_new_int(0)));
    }
    pbw_free(det);
    det = NULL;
    CHECK_OK(pbw_encode_deterministic(msg, &det, &det_len));
    CHECK_OK(pbw_decode(desc(schema, "IMap"), det, det_len, &decoded));
    entries = pbw_message_get(decoded, 1);
    CHECK(pbw_value_get_int32(pbw_message_get(
              pbw_value_get_message(pbw_value_at(entries, 0)), 1)) == -5);
    CHECK(pbw_value_get_int32(pbw_message_get(
              pbw_value_get_message(pbw_value_at(entries, 1)), 1)) == 0);
    CHECK(pbw_value_get_int32(pbw_message_get(
              pbw_value_get_message(pbw_value_at(entries, 2)), 1)) == 3);
    pbw_message_free(decoded);
    pbw_message_free(msg);

    /* No maps: deterministic bytes equal plain bytes. */
    msg = pbw_message_new(desc(schema, "SMap"));
    for (i = 0; i < 3; i++) {
        static const char *ordered[] = {"a", "b", "c"};
        static const int32_t ovals[] = {1, 2, 3};

        sub = pbw_message_set_message(msg, 1);
        CHECK(sub != NULL);
        CHECK_OK(pbw_message_set(sub, 1, pbw_value_new_string(ordered[i])));
        CHECK_OK(pbw_message_set(sub, 2, pbw_value_new_int(ovals[i])));
    }
    pbw_free(plain);
    plain = NULL;
    pbw_free(det);
    det = NULL;
    CHECK_OK(pbw_encode(msg, &plain, &plain_len));
    CHECK_OK(pbw_encode_deterministic(msg, &det, &det_len));
    CHECK(plain_len == det_len && memcmp(plain, det, plain_len) == 0);
    pbw_message_free(msg);

    /* A failing call still writes the out-parameters; use a scratch pair so
     * the documented overwrite cannot drop the buffer encoded above. */
    {
        uint8_t *scratch = (uint8_t *)&det_len;
        size_t scratch_len = 99;

        CHECK_STATUS(PBW_ERR_INVALID,
                     pbw_encode_deterministic(NULL, &scratch, &scratch_len));
        CHECK(scratch == NULL && scratch_len == 0);
    }
    pbw_free(plain);
    pbw_free(det);
    pbw_schema_free(schema);
}

static void
test_limits(pbw_schema *schema)
{
    const pbw_descriptor *type = desc(schema, "Deep");
    pbw_message *msg = pbw_message_new(type);
    pbw_message *cursor = msg;
    pbw_message *decoded = NULL;
    pbw_ctx *ctx = pbw_ctx_new();
    pbw_limits limits;
    uint8_t *buf = NULL;
    size_t len = 0;
    int i;

    for (i = 0; i < 5; i++) {
        cursor = pbw_message_set_message(cursor, 1);
        CHECK(cursor != NULL);
    }
    CHECK_OK(pbw_encode(msg, &buf, &len));

    /* No limits: fine. */
    CHECK_OK(pbw_decode_limits(type, buf, len, NULL, &decoded));
    pbw_message_free(decoded);
    decoded = NULL;
    CHECK_OK(pbw_decode_ctx_limits(ctx, type, buf, len, NULL, &decoded));
    pbw_ctx_reset(ctx);
    decoded = NULL;

    /* Zero limits mean the defaults. */
    limits.max_bytes = 0;
    limits.max_depth = 0;
    CHECK_OK(pbw_decode_limits(type, buf, len, &limits, &decoded));
    pbw_message_free(decoded);
    decoded = NULL;

    /* A byte cap below the input fails; an exact one passes. */
    limits.max_bytes = len - 1;
    limits.max_depth = 0;
    decoded = (pbw_message *)0x1;
    CHECK_STATUS(PBW_ERR_LIMIT,
                 pbw_decode_limits(type, buf, len, &limits, &decoded));
    CHECK(decoded == NULL);
    CHECK_STATUS(PBW_ERR_LIMIT,
                 pbw_decode_ctx_limits(ctx, type, buf, len, &limits,
                                       &decoded));
    CHECK(decoded == NULL);
    limits.max_bytes = len;
    CHECK_OK(pbw_decode_limits(type, buf, len, &limits, &decoded));
    pbw_message_free(decoded);
    decoded = NULL;

    /* A depth cap below the nesting fails; above passes. */
    limits.max_bytes = 0;
    limits.max_depth = 2;
    decoded = (pbw_message *)0x1;
    CHECK_STATUS(PBW_ERR_LIMIT,
                 pbw_decode_limits(type, buf, len, &limits, &decoded));
    CHECK(decoded == NULL);
    limits.max_depth = 100;
    CHECK_OK(pbw_decode_limits(type, buf, len, &limits, &decoded));
    pbw_message_free(decoded);
    decoded = NULL;
    /* Over-large caps clamp to the default instead of failing. */
    limits.max_depth = 1000000;
    CHECK_OK(pbw_decode_limits(type, buf, len, &limits, &decoded));
    pbw_message_free(decoded);
    decoded = NULL;

    /* The same depth cap through the context workspace. */
    limits.max_bytes = 0;
    limits.max_depth = 2;
    decoded = (pbw_message *)0x1;
    CHECK_STATUS(PBW_ERR_LIMIT, pbw_decode_ctx_limits(ctx, type, buf, len,
                                                      &limits, &decoded));
    CHECK(decoded == NULL);
    limits.max_depth = 6;
    CHECK_OK(pbw_decode_ctx_limits(ctx, type, buf, len, &limits, &decoded));
    pbw_ctx_reset(ctx);
    decoded = NULL;
    /* ... and through a delimited stream. */
    {
        pbw_limits capped;
        pbw_istream *s;
        uint8_t *framed = NULL;
        size_t framed_len = 0;

        capped.max_bytes = 0;
        capped.max_depth = 2;
        s = pbw_istream_new(&capped);
        CHECK(s != NULL);
        CHECK_OK(pbw_encode_delimited(msg, &framed, &framed_len));
        CHECK_OK(pbw_istream_feed(s, framed, framed_len));
        decoded = NULL;
        CHECK_STATUS(PBW_ERR_LIMIT, pbw_istream_next(s, type, &decoded));
        CHECK(decoded == NULL);
        pbw_istream_free(s);
        pbw_free(framed);
    }

    CHECK_STATUS(PBW_ERR_INVALID,
                 pbw_decode_limits(NULL, buf, len, NULL, &decoded));
    pbw_message_free(msg);
    pbw_ctx_free(ctx);
    pbw_free(buf);
}

struct visit_hit {
    const pbw_field *field;
    const uint8_t *data;
    uint32_t number;
    uint32_t wire;
    uint64_t u64;
    uint8_t bytes[16];
    size_t len;
};

struct visit_log {
    struct visit_hit hits[16];
    size_t count;
    size_t abort_at; /* 1-based hit index to abort on, 0 = never */
    pbw_status abort_with;
};

static pbw_status
visit_collect(void *ud, const pbw_field *field, uint32_t number,
              uint32_t wire_type, const pbw_visit_value *value)
{
    struct visit_log *log = (struct visit_log *)ud;
    struct visit_hit *hit;

    if (log->count >= sizeof(log->hits) / sizeof(log->hits[0])) {
        return PBW_ERR_LIMIT;
    }
    hit = &log->hits[log->count++];
    hit->field = field;
    hit->data = value->data;
    hit->number = number;
    hit->wire = wire_type;
    hit->u64 = value->u64;
    hit->len = value->len;
    if (value->data != NULL && value->len > 0) {
        size_t n =
            value->len < sizeof(hit->bytes) ? value->len : sizeof(hit->bytes);
        memcpy(hit->bytes, value->data, n);
    }
    if (log->abort_at > 0 && log->count == log->abort_at) {
        return log->abort_with;
    }
    return PBW_OK;
}

/* Well-known-type helpers: Any pack/unpack plus Timestamp/Duration millis.
 * Mirrors with the same field numbers stand in for the real WKTs. */
static void
test_wkt(void)
{
    pbw_schema *schema = pbw_schema_new();
    pbw_descriptor *any;
    pbw_descriptor *ts;
    pbw_descriptor *dur;
    pbw_message *packed = NULL;
    pbw_message *decoded = NULL;
    pbw_message *empty = NULL;
    pbw_message *stamp = NULL;
    uint8_t *buf = NULL;
    size_t len = 0;
    size_t pay_len = 0;
    const void *payload;
    int64_t millis = 0;
    static const uint8_t inner[] = {0x0a, 0x03, 'a', 'b', 'c'};

    CHECK(schema != NULL);
    if (schema == NULL) {
        return;
    }
    any = pbw_schema_add_message(schema, "google.protobuf.Any");
    ts = pbw_schema_add_message(schema, "google.protobuf.Timestamp");
    dur = pbw_schema_add_message(schema, "google.protobuf.Duration");
    CHECK(any != NULL && ts != NULL && dur != NULL);
    pbw_message_add_field(any, "type_url", 1, PBW_TYPE_STRING,
                          PBW_LABEL_OPTIONAL);
    pbw_message_add_field(any, "value", 2, PBW_TYPE_BYTES,
                          PBW_LABEL_OPTIONAL);
    pbw_message_add_field(ts, "seconds", 1, PBW_TYPE_INT64,
                          PBW_LABEL_OPTIONAL);
    pbw_message_add_field(ts, "nanos", 2, PBW_TYPE_INT32,
                          PBW_LABEL_OPTIONAL);
    pbw_message_add_field(dur, "seconds", 1, PBW_TYPE_INT64,
                          PBW_LABEL_OPTIONAL);
    pbw_message_add_field(dur, "nanos", 2, PBW_TYPE_INT32,
                          PBW_LABEL_OPTIONAL);
    CHECK_OK(pbw_schema_link(schema));
    any = (pbw_descriptor *)pbw_schema_find(schema, "google.protobuf.Any");
    ts = (pbw_descriptor *)pbw_schema_find(schema,
                                           "google.protobuf.Timestamp");
    dur = (pbw_descriptor *)pbw_schema_find(schema,
                                            "google.protobuf.Duration");

    /* pack, wire round trip, unpack */
    CHECK_OK(pbw_any_pack(any, "type.googleapis.com/pkg.Msg", inner,
                          sizeof(inner), &packed));
    CHECK(strcmp(pbw_any_type_url(packed), "type.googleapis.com/pkg.Msg") ==
          0);
    payload = pbw_any_payload(packed, &pay_len);
    CHECK(payload != NULL && pay_len == sizeof(inner) &&
          memcmp(payload, inner, pay_len) == 0);
    CHECK_OK(pbw_encode(packed, &buf, &len));
    CHECK_OK(pbw_decode(any, buf, len, &decoded));
    CHECK(strcmp(pbw_any_type_url(decoded), "type.googleapis.com/pkg.Msg") ==
          0);
    payload = pbw_any_payload(decoded, &pay_len);
    CHECK(payload != NULL && pay_len == sizeof(inner) &&
          memcmp(payload, inner, pay_len) == 0);
    pbw_free(buf);
    buf = NULL;
    pbw_message_free(decoded);
    decoded = NULL;
    pbw_message_free(packed);
    packed = NULL;

    /* absent fields read back empty, hostile arguments fail */
    empty = pbw_message_new(any);
    CHECK(empty != NULL);
    CHECK(pbw_any_type_url(empty) == NULL);
    CHECK(pbw_any_payload(empty, &pay_len) == NULL && pay_len == 0);
    CHECK(pbw_any_type_url(NULL) == NULL);
    CHECK(pbw_any_payload(NULL, NULL) == NULL);
    CHECK_STATUS(PBW_ERR_INVALID,
                 pbw_any_pack(NULL, "x", inner, sizeof(inner), &packed));
    CHECK_STATUS(PBW_ERR_INVALID,
                 pbw_any_pack(any, NULL, inner, sizeof(inner), &packed));
    CHECK_STATUS(PBW_ERR_INVALID, pbw_any_pack(any, "x", NULL, 1, &packed));
    CHECK_STATUS(PBW_ERR_INVALID, pbw_any_pack(any, "x", inner,
                                               sizeof(inner), NULL));
    pbw_message_free(empty);

    /* a descriptor whose field 1 is not a string is not an Any */
    CHECK_STATUS(PBW_ERR_TYPE,
                 pbw_any_pack(ts, "x", inner, sizeof(inner), &packed));
    /* set() rejects the value: the half-built tree is released, nothing out */
    CHECK(packed == NULL);
    CHECK_STATUS(PBW_ERR_TYPE, pbw_timestamp_from_millis(any, 7, &stamp));
    CHECK(stamp == NULL);

    /* timestamps: 1.5s, -1.5s, missing seconds, bad nanos, overflow */
    stamp = pbw_message_new(ts);
    CHECK(stamp != NULL);
    CHECK_OK(pbw_message_set(stamp, 1, pbw_value_new_int(1)));
    CHECK_OK(pbw_message_set(stamp, 2, pbw_value_new_int(500000000)));
    CHECK_OK(pbw_timestamp_to_millis(stamp, &millis));
    CHECK(millis == 1500);
    pbw_message_free(stamp);
    stamp = pbw_message_new(ts);
    CHECK_OK(pbw_message_set(stamp, 1, pbw_value_new_int(-1)));
    CHECK_OK(pbw_message_set(stamp, 2, pbw_value_new_int(-500000000)));
    CHECK_OK(pbw_timestamp_to_millis(stamp, &millis));
    CHECK(millis == -1500);
    pbw_message_clear(stamp, 2);
    CHECK_OK(pbw_timestamp_to_millis(stamp, &millis));
    CHECK(millis == -1000);
    pbw_message_free(stamp);
    stamp = pbw_message_new(ts);
    CHECK_STATUS(PBW_ERR_INVALID, pbw_timestamp_to_millis(stamp, &millis));
    CHECK_OK(pbw_message_set(stamp, 1, pbw_value_new_int(0)));
    CHECK_OK(pbw_message_set(stamp, 2, pbw_value_new_int(1000000000)));
    CHECK_STATUS(PBW_ERR_INVALID, pbw_timestamp_to_millis(stamp, &millis));
    CHECK_STATUS(PBW_ERR_INVALID, pbw_timestamp_to_millis(NULL, &millis));
    CHECK_STATUS(PBW_ERR_INVALID, pbw_timestamp_to_millis(stamp, NULL));
    pbw_message_free(stamp);
    stamp = pbw_message_new(dur);
    CHECK(stamp != NULL);
    CHECK_OK(pbw_message_set(stamp, 1, pbw_value_new_int(INT64_MAX)));
    CHECK_STATUS(PBW_ERR_LIMIT, pbw_duration_to_millis(stamp, &millis));
    CHECK_OK(pbw_message_set(stamp, 1, pbw_value_new_int(90)));
    CHECK_OK(pbw_duration_to_millis(stamp, &millis));
    CHECK(millis == 90000);
    pbw_message_free(stamp);
    /* seconds at the int64/1000 edge: adding the sub-second part must report
     * an overflow instead of wrapping the int64 multiply. */
    stamp = pbw_message_new(dur);
    CHECK(stamp != NULL);
    CHECK_OK(pbw_message_set(stamp, 1,
                             pbw_value_new_int(INT64_MAX / 1000)));
    CHECK_OK(pbw_message_set(stamp, 2, pbw_value_new_int(999999999)));
    CHECK_STATUS(PBW_ERR_LIMIT, pbw_duration_to_millis(stamp, &millis));
    CHECK_OK(pbw_message_set(stamp, 1,
                             pbw_value_new_int((INT64_MAX - 999) / 1000)));
    CHECK_OK(pbw_duration_to_millis(stamp, &millis));
    CHECK(millis == (INT64_MAX - 999) / 1000 * 1000 + 999);
    CHECK_OK(pbw_message_set(stamp, 1,
                             pbw_value_new_int(INT64_MIN / 1000)));
    CHECK_OK(pbw_message_set(stamp, 2, pbw_value_new_int(-999999999)));
    CHECK_STATUS(PBW_ERR_LIMIT, pbw_duration_to_millis(stamp, &millis));
    CHECK_OK(pbw_message_set(stamp, 1,
                             pbw_value_new_int((INT64_MIN + 999) / 1000)));
    CHECK_OK(pbw_duration_to_millis(stamp, &millis));
    CHECK(millis == (INT64_MIN + 999) / 1000 * 1000 - 999);
    pbw_message_free(stamp);
    /* pack direction round-trips, including negatives */
    CHECK_OK(pbw_timestamp_from_millis(ts, 1500, &stamp));
    CHECK_OK(pbw_timestamp_to_millis(stamp, &millis));
    CHECK(millis == 1500);
    pbw_message_free(stamp);
    /* Timestamp nanos are never negative: a pre-epoch millisecond borrows a
     * second instead of producing nanos -500000000. */
    CHECK_OK(pbw_timestamp_from_millis(ts, -1500, &stamp));
    CHECK(pbw_value_get_int(pbw_message_get(stamp, 1)) == -2);
    CHECK(pbw_value_get_int(pbw_message_get(stamp, 2)) == 500000000);
    CHECK_OK(pbw_timestamp_to_millis(stamp, &millis));
    CHECK(millis == -1500);
    pbw_message_free(stamp);
    stamp = NULL;
    CHECK_OK(pbw_duration_from_millis(dur, -1501, &stamp));
    CHECK_OK(pbw_duration_to_millis(stamp, &millis));
    CHECK(millis == -1501);
    pbw_message_free(stamp);
    stamp = NULL;
    CHECK_STATUS(PBW_ERR_INVALID,
                 pbw_timestamp_from_millis(NULL, 0, &stamp));
    CHECK_STATUS(PBW_ERR_INVALID,
                 pbw_duration_from_millis(dur, 0, NULL));
    CHECK(stamp == NULL);
    /* FieldMask paths without materializing the list */
    {
        /* the shared schema already linked, so masks get a fresh one */
        pbw_schema *s2 = pbw_schema_new();
        pbw_descriptor *fm2;
        pbw_message *mask = NULL;
        CHECK(s2 != NULL);
        if (s2 != NULL) {
            fm2 = pbw_schema_add_message(s2, "google.protobuf.FieldMask");
            pbw_message_add_field(fm2, "paths", 1, PBW_TYPE_STRING,
                                  PBW_LABEL_REPEATED);
            CHECK_OK(pbw_schema_link(s2));
            fm2 = (pbw_descriptor *)pbw_schema_find(s2,
                                                    "google.protobuf.FieldMask");
            mask = pbw_message_new(fm2);
            CHECK(mask != NULL);
            CHECK(pbw_fieldmask_path_count(mask) == 0);
            CHECK(pbw_fieldmask_path_at(mask, 0) == NULL);
            /* repeated set() appends: two paths land in one list */
            CHECK_OK(
                pbw_message_set(mask, 1, pbw_value_new_string("a.b")));
            CHECK_OK(pbw_message_set(mask, 1, pbw_value_new_string("c")));
            CHECK(pbw_fieldmask_path_count(mask) == 2);
            CHECK(strcmp(pbw_fieldmask_path_at(mask, 0), "a.b") == 0);
            CHECK(strcmp(pbw_fieldmask_path_at(mask, 1), "c") == 0);
            CHECK(pbw_fieldmask_path_at(mask, 2) == NULL);
            pbw_message_free(mask);
            pbw_schema_free(s2);
        }
        CHECK(pbw_fieldmask_path_count(NULL) == 0);
        CHECK(pbw_fieldmask_path_at(NULL, 0) == NULL);
    }
    pbw_schema_free(schema);
}

/* Hostile and edge JSON: every WKT error spelling, map key rules, enum
 * edges and the lenient forms the mapping allows. */
static void
test_json_edges(void)
{
    pbw_schema *schema = pbw_schema_new();
    pbw_descriptor *color;
    pbw_descriptor *msg;
    pbw_descriptor *sub;
    pbw_descriptor *entry;
    pbw_descriptor *ientry;
    pbw_descriptor *ts;
    pbw_descriptor *dur;
    pbw_descriptor *fm;
    pbw_descriptor *st;
    pbw_descriptor *sentry;
    pbw_descriptor *val;
    pbw_descriptor *lv;
    pbw_descriptor *nll;
    pbw_descriptor *any;
    pbw_field *f;
    pbw_message *decoded = NULL;
    pbw_value *got = NULL;
    int64_t millis = 0;
    /* NOTE: message/enum fields are added individually below so their
     * pbw_field handles survive for set_type_name: find_field only works
     * after link, which is too late to resolve type names. */
    static const struct field_def defs[] = {
        {"nums", 11, PBW_TYPE_INT32, PBW_LABEL_REPEATED},
        {"i64", 12, PBW_TYPE_INT64, PBW_LABEL_OPTIONAL},
        {"flag", 13, PBW_TYPE_BOOL, PBW_LABEL_OPTIONAL},
    };
#define JP(json) \
    pbw_json_parse(msg, json, strlen(json), &decoded)

    CHECK(schema != NULL);
    if (schema == NULL) {
        return;
    }
    color = pbw_schema_add_enum(schema, "C");
    pbw_enum_add_value(color, "C_ZERO", 0);
    pbw_enum_add_value(color, "C_ONE", 1);
    msg = pbw_schema_add_message(schema, "M");
    sub = pbw_schema_add_message(schema, "Sub");
    ts = pbw_schema_add_message(schema, "google.protobuf.Timestamp");
    dur = pbw_schema_add_message(schema, "google.protobuf.Duration");
    fm = pbw_schema_add_message(schema, "google.protobuf.FieldMask");
    st = pbw_schema_add_message(schema, "google.protobuf.Struct");
    sentry =
        pbw_schema_add_message(schema, "google.protobuf.Struct.FieldsEntry");
    val = pbw_schema_add_message(schema, "google.protobuf.Value");
    lv = pbw_schema_add_message(schema, "google.protobuf.ListValue");
    nll = pbw_schema_add_enum(schema, "google.protobuf.NullValue");
    any = pbw_schema_add_message(schema, "google.protobuf.Any");
    entry = pbw_schema_add_message(schema, "M.SMapEntry");
    ientry = pbw_schema_add_message(schema, "M.ISMapEntry");
    add_fields(msg, defs, sizeof(defs) / sizeof(defs[0]));
    /* capture every handle up front: the field array may move on the next
     * add, so no pointer survives another add (see fuzz_decode.c). */
    f = pbw_message_add_field(msg, "when", 1, PBW_TYPE_MESSAGE,
                              PBW_LABEL_OPTIONAL);
    if (f != NULL) {
        pbw_field_set_type_name(f, "google.protobuf.Timestamp");
    }
    f = pbw_message_add_field(msg, "howLong", 2, PBW_TYPE_MESSAGE,
                              PBW_LABEL_OPTIONAL);
    if (f != NULL) {
        pbw_field_set_type_name(f, "google.protobuf.Duration");
    }
    f = pbw_message_add_field(msg, "mask", 3, PBW_TYPE_MESSAGE,
                              PBW_LABEL_OPTIONAL);
    if (f != NULL) {
        pbw_field_set_type_name(f, "google.protobuf.FieldMask");
    }
    f = pbw_message_add_field(msg, "props", 4, PBW_TYPE_MESSAGE,
                              PBW_LABEL_OPTIONAL);
    if (f != NULL) {
        pbw_field_set_type_name(f, "google.protobuf.Struct");
    }
    f = pbw_message_add_field(msg, "payload", 5, PBW_TYPE_MESSAGE,
                              PBW_LABEL_OPTIONAL);
    if (f != NULL) {
        pbw_field_set_type_name(f, "google.protobuf.Any");
    }
    f = pbw_message_add_field(msg, "lv", 6, PBW_TYPE_MESSAGE,
                              PBW_LABEL_OPTIONAL);
    if (f != NULL) {
        pbw_field_set_type_name(f, "google.protobuf.ListValue");
    }
    f = pbw_message_add_field(msg, "vv", 7, PBW_TYPE_MESSAGE,
                              PBW_LABEL_OPTIONAL);
    if (f != NULL) {
        pbw_field_set_type_name(f, "google.protobuf.Value");
    }
    f = pbw_message_add_field(msg, "smap", 8, PBW_TYPE_MESSAGE,
                              PBW_LABEL_REPEATED);
    if (f != NULL) {
        pbw_field_set_type_name(f, "M.SMapEntry");
    }
    f = pbw_message_add_field(msg, "ismap", 9, PBW_TYPE_MESSAGE,
                              PBW_LABEL_REPEATED);
    if (f != NULL) {
        pbw_field_set_type_name(f, "M.ISMapEntry");
    }
    f = pbw_message_add_field(msg, "color", 10, PBW_TYPE_ENUM,
                              PBW_LABEL_OPTIONAL);
    if (f != NULL) {
        pbw_field_set_type_name(f, "C");
    }
    f = pbw_message_add_field(msg, "sub", 14, PBW_TYPE_MESSAGE,
                              PBW_LABEL_OPTIONAL);
    if (f != NULL) {
        pbw_field_set_type_name(f, "Sub");
    }
    f = pbw_message_add_field(msg, "nullv", 15, PBW_TYPE_ENUM,
                              PBW_LABEL_OPTIONAL);
    if (f != NULL) {
        pbw_field_set_type_name(f, "google.protobuf.NullValue");
    }
    f = NULL;
    pbw_message_add_field(sub, "a", 1, PBW_TYPE_INT32, PBW_LABEL_OPTIONAL);
    pbw_message_add_field(sub, "b", 2, PBW_TYPE_INT32, PBW_LABEL_OPTIONAL);
    pbw_message_add_field(ts, "seconds", 1, PBW_TYPE_INT64,
                          PBW_LABEL_OPTIONAL);
    pbw_message_add_field(ts, "nanos", 2, PBW_TYPE_INT32,
                          PBW_LABEL_OPTIONAL);
    pbw_message_add_field(dur, "seconds", 1, PBW_TYPE_INT64,
                          PBW_LABEL_OPTIONAL);
    pbw_message_add_field(dur, "nanos", 2, PBW_TYPE_INT32,
                          PBW_LABEL_OPTIONAL);
    pbw_message_add_field(fm, "paths", 1, PBW_TYPE_STRING,
                          PBW_LABEL_REPEATED);
    f = pbw_message_add_field(val, "null_value", 1, PBW_TYPE_ENUM,
                              PBW_LABEL_OPTIONAL);
    if (f != NULL) {
        pbw_field_set_type_name(f, "google.protobuf.NullValue");
    }
    pbw_message_add_field(val, "number_value", 2, PBW_TYPE_DOUBLE,
                          PBW_LABEL_OPTIONAL);
    pbw_message_add_field(val, "string_value", 3, PBW_TYPE_STRING,
                          PBW_LABEL_OPTIONAL);
    pbw_message_add_field(val, "bool_value", 4, PBW_TYPE_BOOL,
                          PBW_LABEL_OPTIONAL);
    f = pbw_message_add_field(val, "struct_value", 5, PBW_TYPE_MESSAGE,
                              PBW_LABEL_OPTIONAL);
    if (f != NULL) {
        pbw_field_set_type_name(f, "google.protobuf.Struct");
    }
    f = pbw_message_add_field(val, "list_value", 6, PBW_TYPE_MESSAGE,
                              PBW_LABEL_OPTIONAL);
    if (f != NULL) {
        pbw_field_set_type_name(f, "google.protobuf.ListValue");
    }
    pbw_message_add_field(sentry, "key", 1, PBW_TYPE_STRING,
                          PBW_LABEL_OPTIONAL);
    f = pbw_message_add_field(sentry, "value", 2, PBW_TYPE_MESSAGE,
                              PBW_LABEL_OPTIONAL);
    pbw_field_set_type_name(f, "google.protobuf.Value");
    f = pbw_message_add_field(st, "fields", 1, PBW_TYPE_MESSAGE,
                              PBW_LABEL_REPEATED);
    pbw_field_set_type_name(f, "google.protobuf.Struct.FieldsEntry");
    f = pbw_message_add_field(lv, "values", 1, PBW_TYPE_MESSAGE,
                              PBW_LABEL_REPEATED);
    pbw_field_set_type_name(f, "google.protobuf.Value");
    pbw_enum_add_value(nll, "NULL_VALUE", 0);
    pbw_message_add_field(any, "type_url", 1, PBW_TYPE_STRING,
                          PBW_LABEL_OPTIONAL);
    pbw_message_add_field(any, "value", 2, PBW_TYPE_BYTES,
                          PBW_LABEL_OPTIONAL);
    pbw_message_add_field(entry, "key", 1, PBW_TYPE_STRING,
                          PBW_LABEL_OPTIONAL);
    pbw_message_add_field(entry, "value", 2, PBW_TYPE_INT32,
                          PBW_LABEL_OPTIONAL);
    pbw_message_add_field(ientry, "key", 1, PBW_TYPE_INT32,
                          PBW_LABEL_OPTIONAL);
    pbw_message_add_field(ientry, "value", 2, PBW_TYPE_STRING,
                          PBW_LABEL_OPTIONAL);
    pbw_descriptor_set_map_entry(entry);
    pbw_descriptor_set_map_entry(ientry);
    pbw_descriptor_set_map_entry(sentry);
    CHECK_OK(pbw_schema_link(schema));
    msg = (pbw_descriptor *)pbw_schema_find(schema, "M");

    /* timestamps: null skips, epoch parses, bad shapes fail */
    CHECK_OK(JP("{\"when\":null}"));
    CHECK(pbw_message_get(decoded, 1) == NULL);
    pbw_message_free(decoded);
    decoded = NULL;
    CHECK_OK(JP("{\"when\":\"1970-01-01T00:00:00Z\"}"));
    got = pbw_message_get(decoded, 1);
    CHECK(got != NULL);
    if (got != NULL) {
        CHECK_OK(
            pbw_timestamp_to_millis(pbw_value_get_message(got), &millis));
        CHECK(millis == 0);
    }
    pbw_message_free(decoded);
    decoded = NULL;
    CHECK_STATUS(PBW_ERR_INVALID, JP("{\"when\":\"\"}"));
    CHECK_STATUS(PBW_ERR_INVALID, JP("{\"when\":\"1970-01-01 00:00:00Z\"}"));
    CHECK_STATUS(PBW_ERR_INVALID, JP("{\"when\":\"1970-01-01T00:00:00\"}"));
    /* the reference accepts out-of-range offsets verbatim (verified against
     * protobuf 6.33: +25:00 lands at -90000s), so the parser does too */
    CHECK_OK(JP("{\"when\":\"1970-01-01T00:00:00+25:00\"}"));
    got = pbw_message_get(decoded, 1);
    CHECK(got != NULL);
    if (got != NULL) {
        CHECK_OK(
            pbw_timestamp_to_millis(pbw_value_get_message(got), &millis));
        CHECK(millis == -90000000);
    }
    pbw_message_free(decoded);
    decoded = NULL;
    CHECK_STATUS(
        PBW_ERR_INVALID,
        JP("{\"when\":\"1970-01-01T00:00:00.1234567890Z\"}"));
    CHECK_STATUS(PBW_ERR_INVALID, JP("{\"when\":\"0000-01-01T00:00:00Z\"}"));
    CHECK_STATUS(PBW_ERR_INVALID, JP("{\"when\":\"1970-00-10T00:00:00Z\"}"));
    /* durations: null skips, suffix-less and unit-less fail */
    CHECK_OK(JP("{\"howLong\":null}"));
    CHECK(pbw_message_get(decoded, 2) == NULL);
    pbw_message_free(decoded);
    decoded = NULL;
    CHECK_OK(JP("{\"howLong\":\"3.5s\"}"));
    pbw_message_free(decoded);
    decoded = NULL;
    CHECK_OK(JP("{\"howLong\":\"-3s\"}"));
    pbw_message_free(decoded);
    decoded = NULL;
    CHECK_STATUS(PBW_ERR_INVALID, JP("{\"howLong\":\"3.5\"}"));
    CHECK_STATUS(PBW_ERR_INVALID, JP("{\"howLong\":\"3.5x\"}"));
    CHECK_STATUS(PBW_ERR_INVALID, JP("{\"howLong\":\"s\"}"));
    /* field masks: null skips, arrays fail, camelCase folds */
    CHECK_OK(JP("{\"mask\":null}"));
    CHECK(pbw_message_get(decoded, 3) == NULL);
    pbw_message_free(decoded);
    decoded = NULL;
    CHECK_OK(JP("{\"mask\":\"aB,cdEf\"}"));
    got = pbw_message_get(decoded, 3);
    CHECK(got != NULL);
    {
        const pbw_message *fmm =
            got == NULL ? NULL : pbw_value_get_message(got);

        CHECK(fmm != NULL);
        if (fmm != NULL) {
            CHECK(pbw_fieldmask_path_count(fmm) == 2);
            CHECK(strcmp(pbw_fieldmask_path_at(fmm, 0), "a_b") == 0);
            CHECK(strcmp(pbw_fieldmask_path_at(fmm, 1), "cd_ef") == 0);
        }
    }
    pbw_message_free(decoded);
    decoded = NULL;
    CHECK_STATUS(PBW_ERR_INVALID, JP("{\"mask\":[\"a\"]}"));
    /* structs: null skips, objects and every Value shape parse */
    CHECK_OK(JP("{\"props\":null}"));
    CHECK(pbw_message_get(decoded, 4) == NULL);
    pbw_message_free(decoded);
    decoded = NULL;
    CHECK_OK(JP("{\"props\":{\"a\":1,\"b\":null,\"c\":[true,\"x\"],\"d\":{}}}"));
    pbw_message_free(decoded);
    decoded = NULL;
    CHECK_STATUS(PBW_ERR_INVALID, JP("{\"props\":[]}"));
    CHECK_STATUS(PBW_ERR_INVALID, JP("{\"props\":5}"));
    CHECK_OK(JP("{\"lv\":[1,null,\"x\"]}"));
    pbw_message_free(decoded);
    decoded = NULL;
    CHECK_OK(JP("{\"vv\":null}"));
    pbw_message_free(decoded);
    decoded = NULL;
    /* google.protobuf.NullValue is the one enum that takes JSON null: it means
     * NULL_VALUE (0), and the printer drops the implicit-presence zero again. */
    CHECK_OK(JP("{\"nullv\":null}"));
    got = pbw_message_get(decoded, 15);
    CHECK(got != NULL);
    if (got != NULL) {
        CHECK(pbw_value_get_kind(got) == PBW_VALUE_UINT);
        CHECK(pbw_value_get_uint(got) == 0);
    }
    {
        char *json = NULL;
        size_t json_len = 0;

        CHECK_OK(pbw_json_print(decoded, &json, &json_len));
        CHECK(strcmp(json, "{}") == 0);
        pbw_free(json);
    }
    pbw_message_free(decoded);
    decoded = NULL;
    /* Legacy duplicate singular message key: the reference merges into the
     * submessage already parsed instead of dropping the first copy. */
    CHECK_OK(JP("{\"sub\":{\"a\":1},\"sub\":{\"b\":2}}"));
    got = pbw_message_get(decoded, 14);
    CHECK(got != NULL);
    CHECK(pbw_value_get_int32(
              pbw_message_get(pbw_value_get_message(got), 1)) == 1);
    CHECK(pbw_value_get_int32(
              pbw_message_get(pbw_value_get_message(got), 2)) == 2);
    pbw_message_free(decoded);
    decoded = NULL;
    /* Any: resolved @type parses, missing/unknown/mistyped @type fails */
    CHECK_OK(JP("{\"payload\":{\"@type\":\"type.googleapis.com/Sub\",\"a\":1}}"));
    pbw_message_free(decoded);
    decoded = NULL;
    CHECK_STATUS(PBW_ERR_INVALID, JP("{\"payload\":{\"a\":1}}"));
    CHECK_STATUS(
        PBW_ERR_INVALID,
        JP("{\"payload\":{\"@type\":\"type.googleapis.com/Nope\",\"a\":1}}"));
    CHECK_STATUS(PBW_ERR_INVALID, JP("{\"payload\":{\"@type\":5}}"));
    /* maps: duplicates (incl. normalized ints) and mistyped keys fail */
    CHECK_OK(JP("{\"smap\":{\"a\":1}}"));
    pbw_message_free(decoded);
    decoded = NULL;
    CHECK_STATUS(PBW_ERR_INVALID, JP("{\"smap\":{\"a\":1,\"a\":2}}"));
    CHECK_STATUS(PBW_ERR_INVALID,
                 JP("{\"ismap\":{\"1\":\"a\",\"01\":\"b\"}}"));
    CHECK_STATUS(PBW_ERR_INVALID, JP("{\"ismap\":{\"x\":\"a\"}}"));
    CHECK_STATUS(PBW_ERR_INVALID, JP("{\"smap\":[1]}"));
    /* enums: unknown names fail, open numbers land on the field */
    CHECK_STATUS(PBW_ERR_INVALID, JP("{\"color\":\"C_NOPE\"}"));
    CHECK_OK(JP("{\"color\":99}"));
    pbw_message_free(decoded);
    decoded = NULL;
    /* the lenient spellings the mapping allows */
    CHECK_OK(JP("{\"i64\":5}"));
    pbw_message_free(decoded);
    decoded = NULL;
    CHECK_OK(JP("{\"flag\":\"true\"}"));
    pbw_message_free(decoded);
    decoded = NULL;
    CHECK_OK(JP("{'i64':'5'}"));
    pbw_message_free(decoded);
    decoded = NULL;
    CHECK_OK(JP("{nums:[1,2]}"));
    pbw_message_free(decoded);
    decoded = NULL;
    CHECK(decoded == NULL);
    /* proto2 closed enums: unknown numbers stay out of the field (kept as
     * unknown), unknown names fail */
    {
        pbw_schema *s2 = pbw_schema_new();
        pbw_descriptor *ce;
        pbw_descriptor *m2;
        pbw_field *cf;
        pbw_message *dd = NULL;

        CHECK(s2 != NULL);
        if (s2 != NULL) {
            ce = pbw_schema_add_enum(s2, "CE");
            pbw_enum_add_value(ce, "CE_ZERO", 0);
            pbw_enum_add_value(ce, "CE_ONE", 1);
            m2 = pbw_schema_add_message(s2, "M2");
            cf = pbw_message_add_field(m2, "ec", 1, PBW_TYPE_ENUM,
                                       PBW_LABEL_OPTIONAL);
            if (cf != NULL) {
                pbw_field_set_type_name(cf, "CE");
            }
            pbw_schema_set_proto3(s2, 0);
            CHECK_OK(pbw_schema_link(s2));
            m2 = (pbw_descriptor *)pbw_schema_find(s2, "M2");
            /* unlike the binary wire, JSON has no unknown-enum keeping:
             * the reference rejects both spellings */
            CHECK_STATUS(PBW_ERR_INVALID,
                         pbw_json_parse(m2, "{\"ec\":99}", 10, &dd));
            CHECK_STATUS(PBW_ERR_INVALID,
                         pbw_json_parse(m2, "{\"ec\":\"CE_NOPE\"}", 16, &dd));
            CHECK_OK(pbw_json_parse(m2, "{\"ec\":\"CE_ONE\"}", 15, &dd));
            pbw_message_free(dd);
            dd = NULL;
            pbw_schema_free(s2);
        }
    }
    /* A hand-built Value -> ListValue chain nests through message fields the
     * printer does not walk field by field: the cap must still apply instead
     * of overflowing the stack. */
    {
        pbw_message *root = pbw_message_new(val);
        pbw_message *cur = root;
        char *out = NULL;
        size_t out_len = 0;
        int i;

        CHECK(root != NULL);
        for (i = 0; i < (int)PBW_MAX_DEPTH + 2; i++) {
            pbw_message *lv_next =
                pbw_message_set_message(cur, 6 /* list_value */);
            pbw_message *next;

            CHECK(lv_next != NULL);
            next = pbw_message_new(val);
            CHECK(next != NULL);
            CHECK_OK(pbw_message_set(lv_next, 1, pbw_value_new_message(next)));
            cur = next;
        }
        CHECK_STATUS(PBW_ERR_LIMIT, pbw_json_print(root, &out, &out_len));
        CHECK(out == NULL && out_len == 0);
        CHECK_STATUS(PBW_ERR_LIMIT, pbw_text_print(root, &out, &out_len));
        CHECK(out == NULL && out_len == 0);
        pbw_message_free(root);
    }
    pbw_schema_free(schema);
#undef JP
}

/* Hostile and edge text format: unknown-field printing (every wire type),
 * float spellings, Any blocks, escapes, numeric enums, bracket lists. */
static void
test_text_edges(void)
{
    pbw_schema *schema = pbw_schema_new();
    pbw_descriptor *color;
    pbw_descriptor *msg;
    pbw_descriptor *sub;
    pbw_descriptor *any;
    pbw_field *f;
    pbw_message *m = NULL;
    pbw_message *decoded = NULL;
    pbw_value *got = NULL;
    char *out = NULL;
    size_t len = 0;
    /* field 99 varint 7, 100 fixed64, 101 fixed32, 102 LEN "hi",
     * 103 group { 1:1 }: every unknown wire type in one blob. */
    static const uint8_t unknowns[] = {
        0x98,
        0x06,
        0x07,
        0xa1,
        0x06,
        0x01,
        0x02,
        0x03,
        0x04,
        0x05,
        0x06,
        0x07,
        0x08,
        0xad,
        0x06,
        0x09,
        0x0a,
        0x0b,
        0x0c,
        0xb2,
        0x06,
        0x02,
        0x68,
        0x69,
        0xbb,
        0x06,
        0x08,
        0x01,
        0xbc,
        0x06,
    };
    static const uint8_t short_len[] = {0xb2, 0x06, 0x05, 0x68, 0x69};

    CHECK(schema != NULL);
    if (schema == NULL) {
        return;
    }
    color = pbw_schema_add_enum(schema, "C");
    pbw_enum_add_value(color, "C_ZERO", 0);
    pbw_enum_add_value(color, "C_ONE", 1);
    msg = pbw_schema_add_message(schema, "T");
    sub = pbw_schema_add_message(schema, "Sub");
    any = pbw_schema_add_message(schema, "google.protobuf.Any");
    pbw_message_add_field(sub, "a", 1, PBW_TYPE_INT32, PBW_LABEL_OPTIONAL);
    pbw_message_add_field(any, "type_url", 1, PBW_TYPE_STRING,
                          PBW_LABEL_OPTIONAL);
    pbw_message_add_field(any, "value", 2, PBW_TYPE_BYTES,
                          PBW_LABEL_OPTIONAL);
    f = pbw_message_add_field(msg, "sub", 1, PBW_TYPE_MESSAGE,
                              PBW_LABEL_OPTIONAL);
    if (f != NULL) {
        pbw_field_set_type_name(f, "Sub");
    }
    pbw_message_add_field(msg, "flt", 2, PBW_TYPE_FLOAT, PBW_LABEL_OPTIONAL);
    pbw_message_add_field(msg, "dbl", 3, PBW_TYPE_DOUBLE,
                          PBW_LABEL_OPTIONAL);
    pbw_message_add_field(msg, "nums", 4, PBW_TYPE_INT32, PBW_LABEL_REPEATED);
    f = pbw_message_add_field(msg, "color", 5, PBW_TYPE_ENUM,
                              PBW_LABEL_OPTIONAL);
    if (f != NULL) {
        pbw_field_set_type_name(f, "C");
    }
    f = pbw_message_add_field(msg, "payload", 6, PBW_TYPE_MESSAGE,
                              PBW_LABEL_OPTIONAL);
    if (f != NULL) {
        pbw_field_set_type_name(f, "google.protobuf.Any");
    }
    pbw_message_add_field(msg, "s", 7, PBW_TYPE_STRING, PBW_LABEL_OPTIONAL);
    CHECK_OK(pbw_schema_link(schema));
    msg = (pbw_descriptor *)pbw_schema_find(schema, "T");
#define TP(text) pbw_text_parse(msg, text, strlen(text), &decoded)
    m = pbw_message_new(msg);
    CHECK(m != NULL);
    CHECK_OK(pbw_message_add_unknown_bytes(m, unknowns, sizeof(unknowns)));
    /* every wire type prints as a numeric line, hidden by default */
    CHECK_OK(pbw_text_print_opts(m, 1, &out, &len));
    CHECK(strstr(out, "99:") != NULL);
    CHECK(strstr(out, "100:") != NULL);
    CHECK(strstr(out, "101:") != NULL);
    CHECK(strstr(out, "102") != NULL); /* LEN with message bytes: brace form */
    CHECK(strstr(out, "103") != NULL);
    pbw_free(out);
    out = NULL;
    CHECK_OK(pbw_text_print_opts(m, 0, &out, &len));
    CHECK(strstr(out, "99:") == NULL);
    pbw_free(out);
    out = NULL;
    pbw_message_free(m);
    m = pbw_message_new(msg);
    CHECK_OK(pbw_message_add_unknown_bytes(m, short_len, sizeof(short_len)));
    CHECK_STATUS(PBW_ERR_INVALID, pbw_text_print_opts(m, 1, &out, &len));
    pbw_message_free(m);
    /* float spellings: inf/nan families, trailing f, no hex */
    CHECK_OK(TP("flt: inf"));
    pbw_message_free(decoded);
    decoded = NULL;
    CHECK_OK(TP("flt: -inf"));
    pbw_message_free(decoded);
    decoded = NULL;
    CHECK_OK(TP("flt: nan"));
    pbw_message_free(decoded);
    decoded = NULL;
    /* the reference lowercases the identifier: any casing of inf/infinity/nan
     * is a value */
    CHECK_OK(TP("flt: NAN"));
    pbw_message_free(decoded);
    decoded = NULL;
    CHECK_OK(TP("flt: inFiniTy"));
    pbw_message_free(decoded);
    decoded = NULL;
    CHECK_OK(TP("flt: Inf"));
    pbw_message_free(decoded);
    decoded = NULL;
    CHECK_STATUS(PBW_ERR_INVALID, TP("flt: 0x10"));
    CHECK_OK(TP("flt: 1.5f"));
    pbw_message_free(decoded);
    decoded = NULL;
    CHECK_STATUS(PBW_ERR_INVALID, TP("flt: 1e"));
    /* Any blocks live inside the Any value: resolved type parses,
     * bad/unknown/truncated fails */
    CHECK_OK(TP("payload { [type.googleapis.com/Sub] { a: 1 } }"));
    pbw_message_free(decoded);
    decoded = NULL;
    CHECK_OK(TP("payload { [type.googleapis.com/Sub]: { a: 1 } }"));
    pbw_message_free(decoded);
    decoded = NULL;
    CHECK_OK(TP("payload { [a%20b/Sub] { a: 1 } }"));
    pbw_message_free(decoded);
    decoded = NULL;
    CHECK_STATUS(PBW_ERR_INVALID, TP("payload { [bad] }"));
    CHECK_STATUS(PBW_ERR_INVALID, TP("payload { [x/Bad-Name] { } }"));
    CHECK_STATUS(PBW_ERR_INVALID, TP("payload { [a%2x/Sub] { } }"));
    CHECK_STATUS(PBW_ERR_INVALID, TP("payload { [a%2/Sub] { } }"));
    CHECK_STATUS(PBW_ERR_INVALID,
                 TP("payload { [type.googleapis.com/Nope] { } }"));
    CHECK_STATUS(PBW_ERR_INVALID,
                 TP("payload { [type.googleapis.com/Sub] xyz }"));
    CHECK_STATUS(PBW_ERR_INVALID, TP("[type.googleapis.com/Sub"));
    /* escapes: \u0041 passes, a surrogate pair combines to U+1F600,
     * lone parts fail (bad UTF-8, which this parser rejects) */
    CHECK_OK(TP("s: \"\\u0041\""));
    pbw_message_free(decoded);
    decoded = NULL;
    CHECK_OK(TP("s: \"\\uD83D\\uDE00\""));
    got = pbw_message_get(decoded, 7);
    CHECK(got != NULL);
    if (got != NULL) {
        size_t slen = 0;
        const void *sval = pbw_value_get_bytes(got, &slen);
        static const uint8_t smile[] = {0xf0, 0x9f, 0x98, 0x80};

        CHECK(sval != NULL && slen == sizeof(smile) &&
              memcmp(sval, smile, slen) == 0);
    }
    pbw_message_free(decoded);
    decoded = NULL;
    CHECK_STATUS(PBW_ERR_INVALID, TP("s: \"\\uD800\""));
    CHECK_STATUS(PBW_ERR_INVALID, TP("s: \"\\x\""));
    CHECK_STATUS(PBW_ERR_INVALID, TP("s: \"abc\\"));
    /* enums take numbers, unknown names fail; lists take brackets */
    CHECK_OK(TP("color: 1"));
    pbw_message_free(decoded);
    decoded = NULL;
    CHECK_STATUS(PBW_ERR_INVALID, TP("color: NOPE"));
    CHECK_OK(TP("nums: [1, 2]"));
    pbw_message_free(decoded);
    decoded = NULL;
    CHECK_STATUS(PBW_ERR_INVALID, TP("nums: [1,"));
    CHECK_STATUS(PBW_ERR_INVALID, TP("nope: 1"));
    CHECK(decoded == NULL);
    /* an Any-shaped imposter whose field 1 is not a string: the expansion
     * parses, then the store rejects it */
    {
        pbw_schema *s2 = pbw_schema_new();
        pbw_descriptor *t2;
        pbw_descriptor *fake;
        pbw_descriptor *sub2;
        pbw_field *ff;
        pbw_message *dd = NULL;

        CHECK(s2 != NULL);
        if (s2 != NULL) {
            t2 = pbw_schema_add_message(s2, "T2");
            fake = pbw_schema_add_message(s2, "google.protobuf.Any");
            sub2 = pbw_schema_add_message(s2, "Sub");
            pbw_message_add_field(sub2, "a", 1, PBW_TYPE_INT32,
                                  PBW_LABEL_OPTIONAL);
            pbw_message_add_field(fake, "type_url", 1, PBW_TYPE_INT32,
                                  PBW_LABEL_OPTIONAL);
            pbw_message_add_field(fake, "value", 2, PBW_TYPE_BYTES,
                                  PBW_LABEL_OPTIONAL);
            ff = pbw_message_add_field(t2, "payload", 6, PBW_TYPE_MESSAGE,
                                       PBW_LABEL_OPTIONAL);
            if (ff != NULL) {
                pbw_field_set_type_name(ff, "google.protobuf.Any");
            }
            CHECK_OK(pbw_schema_link(s2));
            t2 = (pbw_descriptor *)pbw_schema_find(s2, "T2");
            {
                static const char *anytext =
                    "payload { [type.googleapis.com/Sub] { a: 1 } }";

                CHECK_STATUS(PBW_ERR_INVALID,
                             pbw_text_parse(t2, anytext, strlen(anytext),
                                            &dd));
            }
            CHECK(dd == NULL);
            pbw_schema_free(s2);
        }
    }
    pbw_schema_free(schema);
#undef TP
}

static void
test_visit(pbw_schema *schema)
{
    const pbw_descriptor *person = desc(schema, "Person");
    const pbw_descriptor *parent = desc(schema, "Parent");
    const pbw_descriptor *child = desc(schema, "Child");
    const pbw_descriptor *wide = desc(schema, "Wide");
    const pbw_descriptor *color = desc(schema, "Color");
    struct visit_log log;
    pbw_message *msg = pbw_message_new(person);
    pbw_message *par = NULL;
    pbw_message *kid = NULL;
    uint8_t *buf = NULL;
    size_t len = 0;
    size_t off = 0;
    pbw_limits limits;

    CHECK(msg != NULL);
    CHECK_OK(pbw_message_set(msg, 1, pbw_value_new_string("abc")));
    CHECK_OK(pbw_message_set(msg, 2, pbw_value_new_int(-1)));
    CHECK_OK(pbw_encode(msg, &buf, &len));
    pbw_message_free(msg);

    /* Two known fields: string payload plus a raw negative-int32 varint. */
    memset(&log, 0, sizeof(log));
    off = 999;
    CHECK_OK(pbw_visit(person, buf, len, visit_collect, &log, &off));
    CHECK(off == len);
    CHECK(log.count == 2);
    CHECK(log.hits[0].field != NULL && log.hits[0].number == 1);
    CHECK(log.hits[0].wire == 2 && log.hits[0].len == 3);
    CHECK(memcmp(log.hits[0].bytes, "abc", 3) == 0);
    CHECK(log.hits[0].data >= buf && log.hits[0].data < buf + len);
    CHECK(log.hits[1].field != NULL && log.hits[1].number == 2);
    CHECK(log.hits[1].wire == 0 && log.hits[1].u64 == ~0ull);
    CHECK(log.hits[1].data == NULL && log.hits[1].len == 0);

    /* Aborting on the second field reports its offset (5 = 2 + 3 payload). */
    memset(&log, 0, sizeof(log));
    log.abort_at = 2;
    log.abort_with = PBW_ERR_LIMIT;
    off = 999;
    CHECK_STATUS(PBW_ERR_LIMIT,
                 pbw_visit(person, buf, len, visit_collect, &log, &off));
    CHECK(off == 5);
    CHECK(log.count == 2);

    /* Unknown numbers arrive with a NULL field; a wire-type mismatch keeps
     * its field so the caller can tell the two cases apart. */
    {
        /* Field 99, varint 7: tag 0x98 0x06. */
        static const uint8_t unknown[] = {0x98, 0x06, 0x07};
        /* Field 2 carrying LEN instead of its varint. */
        static const uint8_t mismatch[] = {0x12, 0x01, 'x'};
        uint8_t *with = (uint8_t *)malloc(len + sizeof(unknown));
        uint8_t *bad = (uint8_t *)malloc(len + sizeof(mismatch));

        CHECK(with != NULL && bad != NULL);
        memcpy(with, buf, len);
        memcpy(with + len, unknown, sizeof(unknown));
        memset(&log, 0, sizeof(log));
        CHECK_OK(pbw_visit(person, with, len + sizeof(unknown),
                           visit_collect, &log, NULL));
        CHECK(log.count == 3);
        CHECK(log.hits[2].field == NULL && log.hits[2].number == 99);
        CHECK(log.hits[2].wire == 0 && log.hits[2].u64 == 7);
        memcpy(bad, buf, len);
        memcpy(bad + len, mismatch, sizeof(mismatch));
        memset(&log, 0, sizeof(log));
        CHECK_OK(pbw_visit(person, bad, len + sizeof(mismatch),
                           visit_collect, &log, NULL));
        CHECK(log.count == 3);
        CHECK(log.hits[2].field != NULL && log.hits[2].number == 2);
        CHECK(log.hits[2].wire == 2 && log.hits[2].len == 1);
        CHECK(log.hits[2].bytes[0] == 'x');
        free(with);
        free(bad);
    }
    pbw_free(buf);
    buf = NULL;

    /* Nested messages arrive as LEN payloads the caller recurses into. */
    par = pbw_message_new(parent);
    CHECK(par != NULL);
    kid = pbw_message_set_message(par, 3);
    CHECK(kid != NULL);
    CHECK_OK(pbw_message_set(kid, 1, pbw_value_new_string("bo")));
    CHECK_OK(pbw_message_set(kid, 2, pbw_value_new_int(42)));
    CHECK_OK(
        pbw_message_set(par, 2, pbw_value_new_int(300))); /* packed run */
    CHECK_OK(pbw_encode(par, &buf, &len));
    pbw_message_free(par);
    memset(&log, 0, sizeof(log));
    CHECK_OK(pbw_visit(parent, buf, len, visit_collect, &log, NULL));
    CHECK(log.count == 2);
    /* Field order on the wire follows field number: nums(2) before kids(3). */
    CHECK(log.hits[0].number == 2 && log.hits[0].wire == 2);
    CHECK(log.hits[0].len == 2); /* 300 = 0xAC 0x02 */
    CHECK(log.hits[0].bytes[0] == 0xac && log.hits[0].bytes[1] == 0x02);
    CHECK(log.hits[1].number == 3 && log.hits[1].wire == 2);
    {
        struct visit_log inner;

        memset(&inner, 0, sizeof(inner));
        CHECK_OK(pbw_visit(child, log.hits[1].data, log.hits[1].len,
                           visit_collect, &inner, NULL));
        CHECK(inner.count == 2);
        CHECK(inner.hits[0].number == 1 && inner.hits[0].len == 2);
        CHECK(inner.hits[1].number == 2 && inner.hits[1].u64 == 42);
    }
    pbw_free(buf);
    buf = NULL;

    /* Multibyte tags take the two-byte and generic lookup paths. */
    msg = pbw_message_new(wide);
    CHECK(msg != NULL);
    CHECK_OK(pbw_message_set(msg, 16, pbw_value_new_int(7)));
    CHECK_OK(pbw_message_set(msg, 2048, pbw_value_new_int(9)));
    CHECK_OK(pbw_encode(msg, &buf, &len));
    pbw_message_free(msg);
    memset(&log, 0, sizeof(log));
    CHECK_OK(pbw_visit(wide, buf, len, visit_collect, &log, NULL));
    CHECK(log.count == 2);
    CHECK(log.hits[0].field != NULL && log.hits[0].number == 16);
    CHECK(log.hits[0].u64 == 7);
    CHECK(log.hits[1].field != NULL && log.hits[1].number == 2048);
    CHECK(log.hits[1].u64 == 9);
    pbw_free(buf);
    buf = NULL;

    /* Empty input visits nothing; truncation fails at the field start. */
    memset(&log, 0, sizeof(log));
    off = 999;
    CHECK_OK(pbw_visit(person, NULL, 0, visit_collect, &log, &off));
    CHECK(log.count == 0 && off == 0);
    {
        static const uint8_t cut_tag[] = {0x0a};
        static const uint8_t cut_val[] = {0x10};
        /* Unknown fixed64 (field 99): 8 payload bytes visit, 3 fail. */
        static const uint8_t fix64[] = {0x99, 0x06, 0x01, 0x02,
                                        0x03, 0x04, 0x05, 0x06,
                                        0x07, 0x08};
        static const uint8_t fix64_cut[] = {0x99, 0x06, 0x01,
                                            0x02, 0x03};
        /* Unknown fixed32 (field 100): 4 payload bytes visit, 1 fails. */
        static const uint8_t fix32[] = {0xA5, 0x06, 0x01, 0x02,
                                        0x03, 0x04};
        static const uint8_t fix32_cut[] = {0xA5, 0x06, 0x01};
        /* Unknown LEN (field 101) claiming 4 bytes with 1: skip fails. */
        static const uint8_t len_cut[] = {0xAA, 0x06, 0x04, 0x01};

        off = 999;
        CHECK_STATUS(PBW_ERR_INVALID,
                     pbw_visit(person, cut_tag, sizeof(cut_tag),
                               visit_collect, &log, &off));
        CHECK(off == 0);
        off = 999;
        CHECK_STATUS(PBW_ERR_INVALID,
                     pbw_visit(person, cut_val, sizeof(cut_val),
                               visit_collect, &log, &off));
        CHECK(off == 0);
        memset(&log, 0, sizeof(log));
        CHECK_OK(pbw_visit(person, fix64, sizeof(fix64),
                           visit_collect, &log, NULL));
        CHECK(log.count == 1 && log.hits[0].field == NULL);
        CHECK(log.hits[0].number == 99 && log.hits[0].wire == 1);
        CHECK(log.hits[0].u64 == 0x0807060504030201ull);
        CHECK_STATUS(PBW_ERR_INVALID,
                     pbw_visit(person, fix64_cut, sizeof(fix64_cut),
                               visit_collect, &log, NULL));
        memset(&log, 0, sizeof(log));
        CHECK_OK(pbw_visit(person, fix32, sizeof(fix32),
                           visit_collect, &log, NULL));
        CHECK(log.count == 1 && log.hits[0].field == NULL);
        CHECK(log.hits[0].number == 100 && log.hits[0].wire == 5);
        CHECK(log.hits[0].u64 == 0x04030201u);
        CHECK_STATUS(PBW_ERR_INVALID,
                     pbw_visit(person, fix32_cut, sizeof(fix32_cut),
                               visit_collect, &log, NULL));
        CHECK_STATUS(PBW_ERR_INVALID,
                     pbw_visit(person, len_cut, sizeof(len_cut),
                               visit_collect, &log, NULL));
    }

    /* Bounds and argument checks mirror the decoder. */
    msg = pbw_message_new(person);
    CHECK(msg != NULL);
    CHECK_OK(pbw_message_set(msg, 1, pbw_value_new_string("q")));
    CHECK_OK(pbw_encode(msg, &buf, &len));
    pbw_message_free(msg);
    limits.max_bytes = len - 1;
    limits.max_depth = 0;
    off = 999;
    CHECK_STATUS(PBW_ERR_LIMIT,
                 pbw_visit_limits(person, buf, len, &limits,
                                  visit_collect, &log, &off));
    CHECK(off == 0);
    limits.max_bytes = len;
    CHECK_OK(pbw_visit_limits(person, buf, len, &limits, visit_collect,
                              &log, NULL));
    CHECK_STATUS(PBW_ERR_INVALID,
                 pbw_visit(NULL, buf, len, visit_collect, &log, NULL));
    CHECK_STATUS(PBW_ERR_INVALID,
                 pbw_visit(person, buf, len, NULL, &log, NULL));
    CHECK_STATUS(PBW_ERR_TYPE,
                 pbw_visit(color, buf, len, visit_collect, &log, NULL));
    pbw_free(buf);
}

static void
test_streaming(pbw_schema *schema)
{
    const pbw_descriptor *type = desc(schema, "Person");
    pbw_message *m1 = pbw_message_new(type);
    pbw_message *m2 = pbw_message_new(type);
    pbw_message *got = NULL;
    pbw_istream *s = pbw_istream_new(NULL);
    struct sink_buf sunk = {NULL, 0, 0, PBW_OK, 0, 0};
    uint8_t *f1 = NULL;
    uint8_t *f2 = NULL;
    uint8_t *wire = NULL;
    uint8_t *plain = NULL;
    size_t f1_len = 0;
    size_t f2_len = 0;
    size_t plain_len = 0;
    size_t i;
    char big[200];

    CHECK(s != NULL);
    CHECK_OK(pbw_message_set(m1, 1, pbw_value_new_string("alice")));
    CHECK_OK(pbw_message_set(m1, 2, pbw_value_new_int(1)));
    memset(big, 'x', sizeof(big));
    CHECK_OK(pbw_message_set(
        m2, 1, pbw_value_new_bytes(big, sizeof(big))));
    CHECK_OK(pbw_message_set(m2, 2, pbw_value_new_int(2)));
    CHECK_OK(pbw_encode_delimited(m1, &f1, &f1_len));
    CHECK_OK(pbw_encode_delimited(m2, &f2, &f2_len));
    wire = (uint8_t *)malloc(f1_len + f2_len);
    CHECK(wire != NULL);
    memcpy(wire, f1, f1_len);
    memcpy(wire + f1_len, f2, f2_len);

    /* Whole buffer at once: two messages, then nothing left. */
    CHECK_OK(pbw_istream_feed(s, wire, f1_len + f2_len));
    CHECK_OK(pbw_istream_next(s, type, &got));
    CHECK(got != NULL);
    CHECK(strcmp(pbw_value_get_string(pbw_message_get(got, 1)), "alice") ==
          0);
    pbw_message_free(got);
    got = NULL;
    CHECK_OK(pbw_istream_next(s, type, &got));
    CHECK(got != NULL);
    CHECK(pbw_value_get_int32(pbw_message_get(got, 2)) == 2);
    pbw_message_free(got);
    got = NULL;
    CHECK_OK(pbw_istream_next(s, type, &got));
    CHECK(got == NULL);

    /* Byte-by-byte: split length varints and bodies all reassemble. */
    pbw_istream_reset(s);
    for (i = 0; i < f1_len + f2_len; i++) {
        CHECK_OK(pbw_istream_feed(s, wire + i, 1));
        CHECK_OK(pbw_istream_next(s, type, &got));
        if (i + 1 == f1_len) {
            CHECK(got != NULL);
            CHECK(pbw_value_get_int32(pbw_message_get(got, 2)) == 1);
            pbw_message_free(got);
            got = NULL;
        } else if (i + 1 == f1_len + f2_len) {
            CHECK(got != NULL);
            CHECK(pbw_value_get_int32(pbw_message_get(got, 2)) == 2);
            pbw_message_free(got);
            got = NULL;
        } else {
            CHECK(got == NULL);
        }
    }

    /* Partial frame: nothing until the rest arrives. Half the first frame. */
    pbw_istream_reset(s);
    CHECK_OK(pbw_istream_feed(s, wire, f1_len / 2));
    CHECK_OK(pbw_istream_next(s, type, &got));
    CHECK(got == NULL);
    CHECK_OK(pbw_istream_feed(s, wire + f1_len / 2, f1_len - f1_len / 2));
    CHECK_OK(pbw_istream_next(s, type, &got));
    CHECK(got != NULL);
    pbw_message_free(got);
    got = NULL;

    /* Sink hands over the same bytes one-shot encode produces. */
    CHECK_OK(pbw_encode(m2, &plain, &plain_len));
    CHECK_OK(pbw_encode_sink(m2, collect_sink, &sunk));
    CHECK(sunk.len == plain_len && memcmp(sunk.data, plain, plain_len) == 0);
    free(sunk.data);
    sunk.data = NULL;
    sunk.len = 0;
    sunk.cap = 0;
    sunk.inject = PBW_ERR_INVALID;
    CHECK_STATUS(PBW_ERR_INVALID, pbw_encode_sink(m2, collect_sink, &sunk));
    CHECK(sunk.data == NULL);
    sunk.inject = PBW_OK;

    /* Bad length varint fails instead of waiting forever. */
    {
        static const uint8_t bad[] = {0xff, 0xff, 0xff, 0xff, 0xff};

        pbw_istream_reset(s);
        CHECK_OK(pbw_istream_feed(s, bad, sizeof(bad)));
        CHECK_STATUS(PBW_ERR_INVALID, pbw_istream_next(s, type, &got));
        CHECK(got == NULL);
    }
    /* A frame over max_bytes is refused. */
    {
        pbw_limits limits;
        pbw_istream *capped;

        limits.max_bytes = 4;
        limits.max_depth = 0;
        capped = pbw_istream_new(&limits);
        CHECK(capped != NULL);
        CHECK_OK(pbw_istream_feed(capped, wire, f1_len + f2_len));
        CHECK_STATUS(PBW_ERR_LIMIT, pbw_istream_next(capped, type, &got));
        CHECK(got == NULL);
        pbw_istream_free(capped);
    }
    CHECK_STATUS(PBW_ERR_INVALID, pbw_istream_feed(NULL, wire, 1));
    CHECK_STATUS(PBW_ERR_INVALID, pbw_istream_next(s, type, NULL));
    CHECK_STATUS(PBW_ERR_INVALID, pbw_encode_sink(NULL, collect_sink, &sunk));
    CHECK_STATUS(PBW_ERR_INVALID, pbw_encode_sink(m2, NULL, &sunk));
    CHECK_STATUS(PBW_ERR_INVALID,
                 pbw_encode_delimited(NULL, &f1, &f1_len));

    pbw_istream_free(s);
    pbw_message_free(m1);
    pbw_message_free(m2);
    pbw_free(f1);
    pbw_free(f2);
    pbw_free(plain);
    free(wire);
}

static void
test_streaming_edges(pbw_schema *schema)
{
    const pbw_descriptor *type = desc(schema, "Person");

    /* A message spanning several sink chunks: success delivers all of them,
     * abort on a later chunk keeps the prefix and reports the sink status. */
    {
        static uint8_t blob[20000];
        pbw_message *big = pbw_message_new(type);
        struct sink_buf sunk = {NULL, 0, 0, PBW_OK, 0, 0};
        uint8_t *plain = NULL;
        size_t plain_len = 0;
        size_t i;

        for (i = 0; i < sizeof(blob); i++) {
            blob[i] = (uint8_t)(i & 0xff);
        }
        CHECK_OK(
            pbw_message_set(big, 1, pbw_value_new_bytes(blob, sizeof(blob))));
        CHECK_OK(pbw_encode(big, &plain, &plain_len));
        CHECK(plain_len > 16384);
        CHECK_OK(pbw_encode_sink(big, collect_sink, &sunk));
        CHECK(sunk.len == plain_len &&
              memcmp(sunk.data, plain, plain_len) == 0);
        free(sunk.data);
        sunk.data = NULL;
        sunk.len = 0;
        sunk.cap = 0;
        sunk.calls = 0;
        sunk.inject = PBW_ERR_LIMIT;
        sunk.fail_at = 2;
        CHECK_STATUS(PBW_ERR_LIMIT,
                     pbw_encode_sink(big, collect_sink, &sunk));
        CHECK(sunk.len > 0 && sunk.len < plain_len);
        free(sunk.data);
        pbw_free(plain);
        pbw_message_free(big);
    }
    /* A good frame followed by garbage: the frame decodes, then the
     * leftover fails without consuming anything. */
    {
        static const uint8_t garbage[] = {0xff, 0xff, 0xff, 0xff, 0xff};
        pbw_message *m = pbw_message_new(type);
        pbw_istream *s = pbw_istream_new(NULL);
        pbw_message *got = NULL;
        uint8_t *f = NULL;
        size_t f_len = 0;
        uint8_t *wire;

        CHECK_OK(pbw_message_set(m, 1, pbw_value_new_string("hi")));
        CHECK_OK(pbw_encode_delimited(m, &f, &f_len));
        wire = (uint8_t *)malloc(f_len + sizeof(garbage));
        CHECK(wire != NULL);
        memcpy(wire, f, f_len);
        memcpy(wire + f_len, garbage, sizeof(garbage));
        CHECK_OK(pbw_istream_feed(s, wire, f_len + sizeof(garbage)));
        CHECK_OK(pbw_istream_next(s, type, &got));
        CHECK(got != NULL);
        CHECK(strcmp(pbw_value_get_string(pbw_message_get(got, 1)), "hi") ==
              0);
        pbw_message_free(got);
        got = NULL;
        CHECK_STATUS(PBW_ERR_INVALID, pbw_istream_next(s, type, &got));
        CHECK(got == NULL);
        pbw_istream_free(s);
        pbw_message_free(m);
        pbw_free(f);
        free(wire);
    }
    /* Empty feeds are no-ops; NULL data and missing descriptors fail. A
     * fully consumed buffer compacts instead of growing on the next feed,
     * and a corrupt length prefix fails only once five bytes are in. */
    {
        pbw_istream *s = pbw_istream_new(NULL);
        pbw_message *m = pbw_message_new(type);
        pbw_message *got = NULL;
        uint8_t *f = NULL;
        size_t f_len = 0;
        static const uint8_t prefix5[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        static const uint8_t prefix2[] = {0xFF, 0xFF};
        char pad[200];

        CHECK(s != NULL);
        CHECK_OK(pbw_istream_feed(s, "x", 0));
        CHECK_OK(pbw_istream_next(s, type, &got));
        CHECK(got == NULL);
        CHECK_STATUS(PBW_ERR_INVALID, pbw_istream_feed(s, NULL, 1));
        CHECK_STATUS(PBW_ERR_INVALID, pbw_istream_next(s, NULL, &got));
        memset(pad, 'y', sizeof(pad));
        CHECK_OK(pbw_message_set(m, 1, pbw_value_new_bytes(pad, sizeof(pad))));
        CHECK_OK(pbw_encode_delimited(m, &f, &f_len));
        CHECK(f_len < 256 && f_len > 128);
        CHECK_OK(pbw_istream_feed(s, f, f_len));
        CHECK_OK(pbw_istream_next(s, type, &got));
        CHECK(got != NULL);
        pbw_message_free(got);
        got = NULL;
        /* buffer fully consumed: the next feed reuses it in place */
        CHECK_OK(pbw_istream_feed(s, f, f_len));
        CHECK_OK(pbw_istream_next(s, type, &got));
        CHECK(got != NULL);
        pbw_message_free(got);
        pbw_istream_free(s);
        /* corrupt length prefixes */
        s = pbw_istream_new(NULL);
        CHECK(s != NULL);
        CHECK_OK(pbw_istream_feed(s, prefix5, sizeof(prefix5)));
        CHECK_STATUS(PBW_ERR_INVALID, pbw_istream_next(s, type, &got));
        CHECK(got == NULL);
        pbw_istream_reset(s);
        CHECK_OK(pbw_istream_feed(s, prefix2, sizeof(prefix2)));
        CHECK_OK(pbw_istream_next(s, type, &got));
        CHECK(got == NULL);
        pbw_istream_free(s);
        pbw_message_free(m);
        pbw_free(f);
    }
}

static void
test_decode_delimited(pbw_schema *schema)
{
    const pbw_descriptor *type = desc(schema, "Person");
    pbw_message *m = pbw_message_new(type);
    pbw_message *got = NULL;
    uint8_t *f = NULL;
    size_t f_len = 0;
    size_t used = 0;
    pbw_limits capped;

    CHECK_OK(pbw_message_set(m, 1, pbw_value_new_string("hi")));
    CHECK_OK(pbw_encode_delimited(m, &f, &f_len));

    /* Exact frame: everything is consumed. */
    CHECK_OK(pbw_decode_delimited(type, f, f_len, &got, &used));
    CHECK(got != NULL && used == f_len);
    CHECK(strcmp(pbw_value_get_string(pbw_message_get(got, 1)), "hi") ==
          0);
    pbw_message_free(got);
    got = NULL;

    /* Trailing bytes are left for the caller, not rejected. */
    {
        uint8_t *wire = (uint8_t *)malloc(f_len + 1);

        CHECK(wire != NULL);
        memcpy(wire, f, f_len);
        wire[f_len] = 0x00; /* one more frame's worth of future bytes */
        CHECK_OK(pbw_decode_delimited(type, wire, f_len + 1, &got, &used));
        CHECK(got != NULL && used == f_len);
        pbw_message_free(got);
        got = NULL;
        free(wire);
    }

    /* Truncated frame, bad arguments, tight limits. */
    used = 999;
    CHECK_STATUS(PBW_ERR_INVALID,
                 pbw_decode_delimited(type, f, f_len - 1, &got, &used));
    CHECK(got == NULL && used == 0);
    CHECK_STATUS(PBW_ERR_INVALID,
                 pbw_decode_delimited(NULL, f, f_len, &got, &used));
    CHECK_STATUS(PBW_ERR_INVALID,
                 pbw_decode_delimited(type, f, f_len, NULL, &used));
    CHECK_STATUS(PBW_ERR_INVALID,
                 pbw_decode_delimited(type, f, f_len, &got, NULL));
    capped.max_bytes = 0;
    capped.max_depth = 0;
    CHECK_OK(pbw_decode_delimited_limits(type, f, f_len, &capped, &got,
                                         &used));
    CHECK(used == f_len);
    pbw_message_free(got);
    got = NULL;
    capped.max_bytes = 1;
    CHECK_STATUS(PBW_ERR_LIMIT,
                 pbw_decode_delimited_limits(type, f, f_len, &capped,
                                             &got, &used));
    CHECK(got == NULL);

    /* The caller-buffer encoder writes the same frame the allocating one does,
     * and the context twin decodes it into the arena instead of one block. */
    {
        size_t need = 0;
        size_t framed = 0;
        pbw_ctx *ctx;
        size_t budget;

        CHECK_OK(pbw_message_encoded_size(m, &need));
        budget = need + PBW_ENCODE_SLACK + 2; /* + the length prefix */
        {
            uint8_t *out = (uint8_t *)malloc(budget);

            CHECK(out != NULL);
            CHECK_OK(pbw_encode_delimited_to(m, out, budget, &framed));
            CHECK(framed == f_len && memcmp(out, f, f_len) == 0);
            /* One byte short now: the frame needs framing + slack headroom. */
            framed = 99;
            CHECK_STATUS(PBW_ERR_LIMIT,
                         pbw_encode_delimited_to(m, out, f_len - 1, &framed));
            CHECK(framed == 0);
            CHECK_STATUS(PBW_ERR_INVALID,
                         pbw_encode_delimited_to(m, out, budget, NULL));
            CHECK_STATUS(PBW_ERR_INVALID,
                         pbw_encode_delimited_to(NULL, out, budget, &framed));
            CHECK_STATUS(PBW_ERR_LIMIT,
                         pbw_encode_delimited_to(m, NULL, 0, &framed));
            CHECK(framed == 0);
            /* An empty message still needs its one-byte prefix. */
            {
                pbw_message *empty = pbw_message_new(type);

                CHECK(empty != NULL);
                CHECK_OK(pbw_encode_delimited_to(empty, out, budget, &framed));
                CHECK(framed == 1 && out[0] == 0x00);
                pbw_message_free(empty);
            }
            free(out);
        }

        ctx = pbw_ctx_new();
        CHECK(ctx != NULL);
        CHECK_OK(pbw_decode_delimited_ctx(ctx, type, f, f_len, &got, &used));
        CHECK(got != NULL && used == f_len);
        CHECK(ctx == NULL || pbw_ctx_used(ctx) > 0);
        CHECK(strcmp(pbw_value_get_string(pbw_message_get(got, 1)), "hi") ==
              0);
        /* A decoded arena tree is read-only, like pbw_decode_ctx(). */
        {
            pbw_value *v = pbw_value_new_string("no");

            CHECK_STATUS(PBW_ERR_INVALID, pbw_message_set(got, 1, v));
            pbw_value_free(v);
        }
        pbw_ctx_reset(ctx);
        got = NULL;
        capped.max_bytes = 1;
        CHECK_STATUS(PBW_ERR_LIMIT,
                     pbw_decode_delimited_ctx_limits(ctx, type, f, f_len,
                                                     &capped, &got, &used));
        CHECK(got == NULL && used == 0);
        capped.max_bytes = 0;
        CHECK_STATUS(PBW_ERR_INVALID,
                     pbw_decode_delimited_ctx(NULL, type, f, f_len, &got,
                                              &used));
        CHECK_STATUS(PBW_ERR_INVALID,
                     pbw_decode_delimited_ctx(ctx, type, f, f_len - 1, &got,
                                              &used));
        CHECK_STATUS(PBW_ERR_INVALID,
                     pbw_decode_delimited_ctx(ctx, type, f, f_len, &got, NULL));
        CHECK_STATUS(PBW_ERR_INVALID,
                     pbw_decode_delimited_ctx(ctx, type, NULL, 0, &got,
                                              &used));
        pbw_ctx_free(ctx);
    }

    pbw_message_free(m);
    pbw_free(f);
}

static void
test_deterministic_edges(void)
{
    pbw_schema *schema = pbw_schema_new();
    pbw_descriptor *sentry;
    pbw_descriptor *smap;
    pbw_descriptor *bentry;
    pbw_descriptor *bmap;
    pbw_descriptor *uentry;
    pbw_descriptor *umap;
    pbw_field *field;
    pbw_message *msg;
    pbw_message *sub;
    pbw_message *decoded = NULL;
    pbw_value *entries;
    uint8_t *plain = NULL;
    uint8_t *det = NULL;
    size_t plain_len = 0;
    size_t det_len = 0;

    CHECK(schema != NULL);
    sentry = pbw_schema_add_message(schema, "SE");
    pbw_message_add_field(sentry, "key", 1, PBW_TYPE_STRING,
                          PBW_LABEL_OPTIONAL);
    pbw_message_add_field(sentry, "value", 2, PBW_TYPE_INT32,
                          PBW_LABEL_OPTIONAL);
    pbw_descriptor_set_map_entry(sentry);
    smap = pbw_schema_add_message(schema, "SMap");
    field = pbw_message_add_field(smap, "entries", 1, PBW_TYPE_MESSAGE,
                                  PBW_LABEL_REPEATED);
    pbw_field_set_type_name(field, "SE");
    bentry = pbw_schema_add_message(schema, "BE");
    pbw_message_add_field(bentry, "key", 1, PBW_TYPE_BOOL,
                          PBW_LABEL_OPTIONAL);
    pbw_message_add_field(bentry, "value", 2, PBW_TYPE_INT32,
                          PBW_LABEL_OPTIONAL);
    pbw_descriptor_set_map_entry(bentry);
    bmap = pbw_schema_add_message(schema, "BMap");
    field = pbw_message_add_field(bmap, "entries", 1, PBW_TYPE_MESSAGE,
                                  PBW_LABEL_REPEATED);
    pbw_field_set_type_name(field, "BE");
    uentry = pbw_schema_add_message(schema, "UE");
    pbw_message_add_field(uentry, "key", 1, PBW_TYPE_UINT64,
                          PBW_LABEL_OPTIONAL);
    pbw_message_add_field(uentry, "value", 2, PBW_TYPE_INT32,
                          PBW_LABEL_OPTIONAL);
    pbw_descriptor_set_map_entry(uentry);
    umap = pbw_schema_add_message(schema, "UMap");
    field = pbw_message_add_field(umap, "entries", 1, PBW_TYPE_MESSAGE,
                                  PBW_LABEL_REPEATED);
    pbw_field_set_type_name(field, "UE");
    CHECK_OK(pbw_schema_link(schema));

    /* Empty map: deterministic bytes equal plain bytes. */
    msg = pbw_message_new(desc(schema, "SMap"));
    CHECK_OK(pbw_encode(msg, &plain, &plain_len));
    CHECK_OK(pbw_encode_deterministic(msg, &det, &det_len));
    CHECK(plain_len == det_len && memcmp(plain, det, plain_len) == 0);
    pbw_message_free(msg);

    /* A hand-built entry without a key sorts first and never crashes. */
    msg = pbw_message_new(desc(schema, "SMap"));
    sub = pbw_message_set_message(msg, 1);
    CHECK(sub != NULL);
    CHECK_OK(pbw_message_set(sub, 1, pbw_value_new_string("a")));
    CHECK_OK(pbw_message_set(sub, 2, pbw_value_new_int(1)));
    sub = pbw_message_set_message(msg, 1);
    CHECK(sub != NULL);
    CHECK_OK(pbw_message_set(sub, 2, pbw_value_new_int(9)));
    pbw_free(det);
    det = NULL;
    CHECK_OK(pbw_encode_deterministic(msg, &det, &det_len));
    CHECK_OK(pbw_decode(desc(schema, "SMap"), det, det_len, &decoded));
    entries = pbw_message_get(decoded, 1);
    CHECK(pbw_value_count(entries) == 2);
    CHECK(pbw_message_get(pbw_value_get_message(pbw_value_at(entries, 0)),
                          1) == NULL);
    CHECK(strcmp(entry_key(entries, 1), "a") == 0);
    pbw_message_free(decoded);
    decoded = NULL;
    pbw_message_free(msg);

    /* Bool keys sort false before true. */
    msg = pbw_message_new(desc(schema, "BMap"));
    sub = pbw_message_set_message(msg, 1);
    CHECK(sub != NULL);
    CHECK_OK(pbw_message_set(sub, 1, pbw_value_new_bool(1)));
    sub = pbw_message_set_message(msg, 1);
    CHECK(sub != NULL);
    CHECK_OK(pbw_message_set(sub, 1, pbw_value_new_bool(0)));
    pbw_free(det);
    det = NULL;
    CHECK_OK(pbw_encode_deterministic(msg, &det, &det_len));
    CHECK_OK(pbw_decode(desc(schema, "BMap"), det, det_len, &decoded));
    entries = pbw_message_get(decoded, 1);
    CHECK(pbw_value_get_bool(pbw_message_get(
              pbw_value_get_message(pbw_value_at(entries, 0)), 1)) == 0);
    CHECK(pbw_value_get_bool(pbw_message_get(
              pbw_value_get_message(pbw_value_at(entries, 1)), 1)) != 0);
    pbw_message_free(decoded);
    decoded = NULL;
    pbw_message_free(msg);

    /* Unsigned 64-bit keys sort numerically, not by raw length. */
    msg = pbw_message_new(desc(schema, "UMap"));
    {
        static const uint64_t ukeys[] = {UINT64_MAX, 0, 5};
        static const uint64_t expect[] = {0, 5, UINT64_MAX};
        size_t i;

        for (i = 0; i < 3; i++) {
            sub = pbw_message_set_message(msg, 1);
            CHECK(sub != NULL);
            CHECK_OK(pbw_message_set(sub, 1, pbw_value_new_uint(ukeys[i])));
        }
        pbw_free(det);
        det = NULL;
        CHECK_OK(pbw_encode_deterministic(msg, &det, &det_len));
        CHECK_OK(pbw_decode(desc(schema, "UMap"), det, det_len, &decoded));
        entries = pbw_message_get(decoded, 1);
        for (i = 0; i < 3; i++) {
            CHECK(pbw_value_get_uint(pbw_message_get(
                      pbw_value_get_message(pbw_value_at(entries, i)), 1)) ==
                  expect[i]);
        }
        pbw_message_free(decoded);
        decoded = NULL;
    }
    /* Unknown fields survive a deterministic round-trip, still last: the
     * map sorts, the unknowns stay exactly where they were. */
    {
        static const uint8_t unknown[] = {0x90, 0x03, 0x2a};
        uint8_t *mixed;
        size_t mixed_len;

        pbw_message_free(msg);
        msg = pbw_message_new(desc(schema, "SMap"));
        sub = pbw_message_set_message(msg, 1);
        CHECK(sub != NULL);
        CHECK_OK(pbw_message_set(sub, 1, pbw_value_new_string("b")));
        CHECK_OK(pbw_message_set(sub, 2, pbw_value_new_int(2)));
        sub = pbw_message_set_message(msg, 1);
        CHECK(sub != NULL);
        CHECK_OK(pbw_message_set(sub, 1, pbw_value_new_string("a")));
        CHECK_OK(pbw_message_set(sub, 2, pbw_value_new_int(1)));
        pbw_free(det);
        det = NULL;
        CHECK_OK(pbw_encode_deterministic(msg, &det, &det_len));
        pbw_message_free(msg);
        msg = NULL;
        mixed_len = det_len + sizeof(unknown);
        mixed = (uint8_t *)malloc(mixed_len);
        CHECK(mixed != NULL);
        memcpy(mixed, det, det_len);
        memcpy(mixed + det_len, unknown, sizeof(unknown));
        CHECK_OK(
            pbw_decode(desc(schema, "SMap"), mixed, mixed_len, &decoded));
        pbw_free(det);
        det = NULL;
        CHECK_OK(pbw_encode_deterministic(decoded, &det, &det_len));
        CHECK(det_len == mixed_len && memcmp(det, mixed, mixed_len) == 0);
        pbw_message_free(decoded);
        decoded = NULL;
        free(mixed);
    }
    pbw_message_free(msg);
    pbw_free(plain);
    pbw_free(det);
    pbw_schema_free(schema);
}

/* Small API surfaces with no other home: strerror text, clear/get_field,
 * the delimited one-shot twins and the initialized check. */
static void
test_api_misc(pbw_schema *schema)
{
    const pbw_descriptor *type = desc(schema, "Scalar");
    const pbw_field *f1;
    pbw_message *msg;
    pbw_message *decoded = NULL;
    uint8_t *frame = NULL;
    size_t frame_len = 0;
    size_t consumed = 0;
    int s;

    /* every status has text, including out-of-range codes */
    for (s = PBW_OK; s <= PBW_ERR_REQUIRED; s++) {
        CHECK(pbw_strerror((pbw_status)s) != NULL);
    }
    CHECK(pbw_strerror((pbw_status)999) != NULL);
    CHECK(pbw_abi_version() == PBW_ABI_VERSION);
    CHECK(pbw_message_new_ctx(NULL, type) == NULL);
    CHECK(pbw_value_new_uint_ctx(NULL, 1) == NULL);
    CHECK(pbw_value_new_bytes_ctx(NULL, "x", 1) == NULL);

    msg = pbw_message_new(type);
    CHECK(msg != NULL);
    f1 = pbw_descriptor_find_field(type, 1);
    CHECK(f1 != NULL);
    /* unknown bytes: validation plus a growth path */
    CHECK_STATUS(PBW_ERR_INVALID,
                 pbw_message_add_unknown_bytes(NULL, "x", 1));
    CHECK_STATUS(PBW_ERR_INVALID,
                 pbw_message_add_unknown_bytes(msg, NULL, 1));
    CHECK_OK(pbw_message_add_unknown_bytes(msg, NULL, 0));
    CHECK_OK(pbw_message_add_unknown_bytes(msg, "x", 0));
    {
        /* a megabyte of unknowns forces the accumulator to grow; the
         * bytes repeat one valid unknown field (500: 1) */
        static uint8_t *big = NULL;
        static size_t big_cap = 0;
        size_t k;

        if (big_cap < 1048576) {
            free(big);
            big = (uint8_t *)malloc(1048576);
            big_cap = big != NULL ? 1048576 : 0;
        }
        CHECK(big != NULL);
        if (big != NULL) {
            for (k = 0; k + 3 <= 1048576; k += 3) {
                big[k] = 0xA0;
                big[k + 1] = 0x0F;
                big[k + 2] = 0x01;
            }
            CHECK_OK(pbw_message_add_unknown_bytes(
                msg, big, (1048576 / 3) * 3));
            CHECK(pbw_message_unknown_len(msg) == (1048576 / 3) * 3);
        }
    }
    /* a megabyte string field exercises the arena big-chunk paths on
     * decode, and their release on free */
    {
        uint8_t *huge = (uint8_t *)malloc(1048576);
        pbw_message *fat = NULL;
        uint8_t *wire2 = NULL;
        size_t wire2_len = 0;
        pbw_message *back = NULL;

        CHECK(huge != NULL);
        if (huge != NULL) {
            memset(huge, 'z', 1048576);
            fat = pbw_message_new(type);
            CHECK(fat != NULL);
            CHECK_OK(
                pbw_message_set(fat, 15, pbw_value_new_bytes(huge, 1048576)));
            CHECK_OK(pbw_encode(fat, &wire2, &wire2_len));
            CHECK_OK(pbw_decode(type, wire2, wire2_len, &back));
            CHECK(back != NULL &&
                  pbw_message_get(back, 15) != NULL);
            /* storing into a decoded tree marks heap payloads for the
             * release walk on free */
            CHECK_OK(pbw_message_set(back, 1, pbw_value_new_int(1)));
            pbw_message_free(back);
            pbw_message_free(fat);
            pbw_free(wire2);
            free(huge);
        }
    }
    /* clearing a oneof member resets the case */
    {
        const pbw_descriptor *oneof = desc(schema, "OneOf");
        pbw_message *o = pbw_message_new(oneof);

        CHECK(o != NULL);
        CHECK_OK(pbw_message_set(o, 1, pbw_value_new_string("x")));
        pbw_message_clear(o, 1);
        CHECK(pbw_message_get(o, 1) == NULL);
        pbw_message_clear(o, 2); /* absent and unknown numbers */
        pbw_message_clear(o, 42);
        pbw_message_free(o);
    }
    /* NULL-tolerant accessors */
    CHECK(pbw_message_get_field(msg, f1) == NULL);
    CHECK(pbw_message_get_field(msg, NULL) == NULL);
    CHECK(pbw_message_get_field(NULL, f1) == NULL);
    CHECK(pbw_message_get(NULL, 1) == NULL);
    CHECK(pbw_value_get_bytes(pbw_message_get(msg, 1), NULL) == NULL);
    {
        /* a linked schema is frozen against new enum values */
        pbw_schema *s2 = pbw_schema_new();
        pbw_descriptor *e2;

        CHECK(s2 != NULL);
        e2 = pbw_schema_add_enum(s2, "E");
        CHECK(e2 != NULL);
        pbw_enum_add_value(e2, "A", 0);
        CHECK_OK(pbw_schema_link(s2));
        CHECK_STATUS(PBW_ERR_INVALID, pbw_enum_add_value(e2, "B", 1));
        pbw_schema_free(s2);
    }
    {
        size_t n = 99;
        const uint8_t *p;

        /* NULL value leaves the length alone; a value fills it in */
        CHECK(pbw_value_get_bytes(NULL, &n) == NULL);
        CHECK(n == 0);
        CHECK_OK(pbw_message_set(msg, 15,
                                 pbw_value_new_bytes("xy", 2)));
        p = pbw_value_get_bytes(pbw_message_get(msg, 15), &n);
        CHECK(p != NULL && n == 2 && memcmp(p, "xy", 2) == 0);
        CHECK(pbw_value_get_bytes(pbw_message_get(msg, 15), NULL) != NULL);
    }
    CHECK(pbw_message_descriptor(NULL) == NULL);
    CHECK(pbw_message_field_count(NULL) == 0);
    CHECK(pbw_message_get(msg, 9999) == NULL);
    CHECK(pbw_schema_link(NULL) != PBW_OK);
    CHECK(pbw_schema_service_find(NULL, "x") == NULL);
    CHECK(pbw_service_method_find(NULL, "x") == NULL);
    CHECK(!pbw_descriptor_is_map_entry(NULL));
    CHECK(pbw_schema_find_extension(schema, "no.such.Ext") == NULL);
    pbw_descriptor_free(NULL);
    /* present fields enumerate in field-index order */
    {
        uint32_t fnums[16];
        const pbw_value *values[16];
        size_t n;
        const pbw_field *fa;
        const pbw_field *fb;

        CHECK_OK(pbw_message_set(msg, 1, pbw_value_new_int(-7)));
        CHECK(pbw_message_field_count(msg) > 0);
        n = pbw_message_present_list(msg, fnums, values, 16);
        CHECK(n == pbw_message_field_count(msg));
        CHECK(pbw_message_present_list(msg, NULL, values, 16) == 0);
        CHECK(pbw_message_present_list(msg, fnums, NULL, 16) == 0);
        CHECK(pbw_message_present_list(NULL, fnums, values, 16) == 0);
        /* field pointers from another message do not resolve here */
        fa = pbw_descriptor_find_field(desc(schema, "Parent"), 1);
        CHECK(fa != NULL);
        CHECK(pbw_message_get_field(msg, fa) == NULL);
        {
            pbw_value *rejected = pbw_value_new_int(1);

            CHECK_STATUS(PBW_ERR_INVALID,
                         pbw_message_set_field(msg, fa, rejected));
            pbw_value_free(rejected);
        }
        fb = pbw_descriptor_find_field(type, 1);
        CHECK(fb != NULL);
        CHECK(pbw_message_get_field(msg, fb) != NULL);
    }
    /* reflection over present fields */
    {
        const pbw_field *f = NULL;
        pbw_value *v = NULL;
        size_t seen = 0;

        CHECK(pbw_message_field_at(NULL, 0, &v) == NULL);
        while ((f = pbw_message_field_at(msg, seen, &v)) != NULL) {
            CHECK(v != NULL);
            seen++;
            if (seen > 64) {
                break;
            }
        }
        CHECK(seen == pbw_message_field_count(msg));
    }
    /* NULL-tolerant mutators and constructors */
    pbw_field_set_type_name(NULL, "x");
    pbw_field_set_full_name(NULL, "x");
    pbw_descriptor_set_map_entry(NULL);
    pbw_ctx_free(NULL);
    pbw_ctx_reset(NULL);
    pbw_pool_free(NULL);
    pbw_istream_free(NULL);
    pbw_istream_reset(NULL);
    pbw_schema_free(NULL);
    {
        pbw_istream *tmp = pbw_istream_new(NULL);

        pbw_istream_free(tmp);
    }
    {
        pbw_pool *none = pbw_pool_new(0, 0);
        pbw_ctx *one;

        CHECK(none != NULL);
        one = pbw_pool_acquire(none);
        CHECK(one != NULL);
        /* max_idle 0 frees on release instead of keeping */
        pbw_pool_release(none, one);
        CHECK(pbw_pool_idle(none) == 0);
        pbw_pool_free(none);
    }
    /* hand-built schemas carry no reserved names */
    CHECK(!pbw_descriptor_is_reserved(type, "ancient"));
    CHECK(!pbw_descriptor_is_reserved(NULL, "ancient"));
    CHECK_OK(pbw_message_set(msg, 1, pbw_value_new_int(-7)));
    CHECK(pbw_message_get_field(msg, f1) != NULL);
    CHECK(pbw_value_get_int(pbw_message_get_field(msg, f1)) == -7);
    CHECK(pbw_message_is_initialized(msg));
    pbw_message_clear(msg, 1);
    CHECK(pbw_message_get(msg, 1) == NULL);
    CHECK(pbw_message_get_field(msg, f1) == NULL);
    pbw_message_clear(msg, 1); /* clearing absent is a no-op */
    pbw_message_clear(NULL, 1);
    /* set_message creates (or returns) the owned submessage */
    {
        const pbw_descriptor *ptype = desc(schema, "Parent");
        pbw_message *p = pbw_message_new(ptype);
        pbw_message *kid;
        pbw_message *kid2;

        CHECK(p != NULL);
        kid = pbw_message_set_message(p, 1);
        CHECK(kid != NULL);
        CHECK_OK(pbw_message_set(kid, 2, pbw_value_new_int(1)));
        CHECK(pbw_value_get_int(pbw_message_get(
                  pbw_value_get_message(pbw_message_get(p, 1)), 2)) ==
              1);
        /* setting the scalar inside again replaces just that value */
        kid2 = pbw_message_set_message(p, 1);
        CHECK(kid2 != NULL);
        CHECK_OK(pbw_message_set(kid2, 2, pbw_value_new_int(2)));
        CHECK(pbw_value_get_int(pbw_message_get(
                  pbw_value_get_message(pbw_message_get(p, 1)), 2)) ==
              2);
        pbw_message_free(p);
    }
    /* delimited framing round-trips; two frames advance by consumed */
    CHECK_OK(pbw_message_set(msg, 2, pbw_value_new_int(INT64_MIN)));
    CHECK_OK(pbw_encode_delimited(msg, &frame, &frame_len));
    {
        uint8_t *two;
        size_t two_len = frame_len * 2;
        pbw_limits lim = {0, 0};

        two = (uint8_t *)malloc(two_len);
        CHECK(two != NULL);
        memcpy(two, frame, frame_len);
        memcpy(two + frame_len, frame, frame_len);
        CHECK_OK(pbw_decode_delimited(type, two, two_len, &decoded,
                                      &consumed));
        CHECK(consumed == frame_len);
        CHECK(pbw_value_get_int(pbw_message_get(decoded, 2)) ==
              INT64_MIN);
        pbw_message_free(decoded);
        decoded = NULL;
        CHECK_OK(pbw_decode_delimited_limits(type, two + consumed,
                                             two_len - consumed, &lim,
                                             &decoded, &consumed));
        CHECK(consumed == frame_len);
        pbw_message_free(decoded);
        decoded = NULL;
        /* a truncated frame fails with nothing out */
        CHECK_STATUS(PBW_ERR_INVALID,
                     pbw_decode_delimited(type, two, frame_len - 1,
                                          &decoded, &consumed));
        CHECK(decoded == NULL);
        free(two);
    }
    pbw_free(frame);
    pbw_message_free(msg);
}

int
main(void)
{
    pbw_schema *schema = make_schema();

    if (schema == NULL || failures != 0) {
        fprintf(stderr, "schema setup failed\n");
        pbw_schema_free(schema);
        return 1;
    }

    test_scalars(schema);
    test_scalars_golden(schema);
    test_golden_bytes(schema);
    test_reflection(schema);
    test_nested(schema);
    test_packed_compat(schema);
    test_map(schema);
    test_oneof(schema);
    test_unknown_fields(schema);
    test_hostile_arguments(schema);
    test_locale_independence(schema);
    test_tag_limits(schema);
    test_field_number_edges(schema);
    test_packed_wide_tag(schema);
    test_pack_all_types(schema);
    test_packed_presize(schema);
    test_packed_hostile(schema);
    test_recursive_type(schema);
    test_print_depth_limit(schema);
    test_group_depth_limit();
    test_bool_and_float_bits(schema);
    test_enum_edges(schema);
    test_explicit_presence(schema);
    test_field_order(schema);
    test_schema_name_index();
    test_duplicate_occurrences(schema);
    test_groups(schema);
    test_unknown_only(schema);
    test_truncation_and_mutation(schema);
    test_large_payloads(schema);
    test_length_patch_boundaries(schema);
    test_ctx(schema);
    test_ctx_build(schema);
    test_ctx_retain_cap(schema);
    test_ctx_pool(schema);
    test_encode_to(schema);
    test_errors(schema);
    test_depth_limit(schema);
    test_enum(schema);
    test_proto2_required();
    test_closed_enum();
    test_group_fields();
    test_hand_built_extensions();
    test_text_format();
    test_json_format();
    test_api_misc(schema);
    test_deterministic();
    test_deterministic_edges();
    test_limits(schema);
    test_visit(schema);
    test_wkt();
    test_json_edges();
    test_text_edges();
    test_streaming(schema);
    test_streaming_edges(schema);
    test_decode_delimited(schema);
    test_schema_errors();

    pbw_schema_free(schema);

    if (failures != 0) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("test_pbwire: all checks passed\n");
    return 0;
}

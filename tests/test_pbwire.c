/* Assert-based self-check for the pbwire runtime: no test framework, no
 * dependencies. */

#include "pbwire.h"
#include "internal.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

/* Nesting limit shared by messages and legacy groups. */
#define PBW_GROUP_DEPTH 200

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
    pbw_message_free(msg);
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
        pbw_free(buf);
        pbw_message_free(decoded);
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
    /* oneof: the second member replaces the first */
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
    CHECK(pbw_ctx_used(ctx) > ((size_t)2 << 20));
    pbw_ctx_reset(ctx);
    CHECK(test_head_cap(ctx) <= PBW_ARENA_LARGE_THRESHOLD);

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

    for (i = 0; i < 150; i++) {
        cursor = pbw_message_set_message(cursor, 1);
        CHECK(cursor != NULL);
    }
    CHECK_STATUS(PBW_ERR_LIMIT, pbw_encode(msg, &buf, &len));
    pbw_message_free(msg);

    /* Build 150 levels of nested length-delimited frames by hand and decode. */
    {
        pbw_message *decoded = NULL;
        uint8_t *wire = NULL;
        size_t wire_len = 0;
        size_t cap = 0;

        for (i = 0; i < 150; i++) {
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
    pbw_schema_free(schema);
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
    /* Accepted spellings. */
    {
        static const char *ok[] = {
            "a: 1, blob: \"x\";", /* separators */
            "a: 1 # comment\n",   /* comments */
            "a:0x10",             /* hex */
            "a:010",              /* octal */
            "nums: [1, 2]",       /* list */
            "nums: []",           /* empty list */
            "kid < a: 1 >",       /* angle brackets */
            "kid: { a: 1 }",      /* colon before message */
            "grp { x: 1 }",       /* lowercase group name */
            "Grp: { x: 1 }",      /* colon with group */
            "blob: \"a\" \"b\"",  /* adjacent strings */
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
            "a: 2147483648", /* range */
            "a: 0x",         /* empty hex */
            "a: 1.5",        /* float for int */
            "blob: \"\\x\"", /* bad escape */
            "blob: \"abc",   /* unterminated */
            "bogus: 1",      /* unknown name */
            "[x_int]: 5",    /* short names never match extensions */
            "color: BOGUS",  /* unknown enum */
            "kid: 1",        /* scalar for message */
            "a: 1 bogus",    /* trailing garbage */
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
    pbw_schema_free(schema);
}

/* JSON mapping: printer goldens, accepted/rejected spellings, WKT forms. */
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
    /* Accepted spellings. */
    {
        static const char *ok[] = {
            "{}",
            "{\"i32\":\"5\"}",
            "{\"i32\":\"5.0\"}",
            "{\"i32\":\"0x10\"}",
            "{\"i32\":5.0}",
            "{\"i32\":1e3}",
            "{\"flag\":\"true\"}",
            "{\"flag\":\"YES\"}",
            "{\"f64\":\"Infinity\"}",
            "{\"f64\":\"-Infinity\"}",
            "{\"color\":1}",
            "{\"color\":\"1\"}",
            "{\"name\":null}",
            "{\"kid\":null}",
            "{\"nums\":null}",
            "{\"nums\":[1,null,2]}",
            "{\"nums\":[[1,2]]}",
            "{'i32':1}",
            "{i32:1}",
            "{\"i32\":1,}",
            "{\"when\":\"1970-01-01T00:00:00Z\"}",
            "{\"when\":{\"seconds\":\"5\"}}",
            "{\"howLong\":\"-3.5s\"}",
            "{\"mask\":\"fooBar,baz\"}",
            "{\"mask\":\"\"}",
            "{\"props\":{\"k\":1}}",
            "{\"payload\":{\"a\":2,\"@type\":\"type.googleapis.com/Sub\"}}",
            "{\"payload\":{}}",
            "{\"count\":7}",
            "{\"count\":null}",
            "{\"scores\":{\"a\":1}}",
            "{\"scores\":{}}",
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
            "{\"i32\":}",
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
            "{\"name\":5}",
            "{\"blob\":\"!\"}",
            "{\"blob\":\"A\"}",
            "{\"color\":\"BOGUS\"}",
            "{\"color\":9999999999}",
            "{\"nums\":1}",
            "{\"kid\":[]}",
            "{\"scores\":{\"a\":1,\"a\":2}}",
            "{\"bogus\":1}",
            "{\"name\":\"x\",\"alt\":1}",
            "{\"when\":\"10000-01-01T00:00:00Z\"}",
            "{\"when\":\"1970-01-01T00:00:01+0800\"}",
            "{\"howLong\":\"3\"}",
            "{\"howLong\":\"3.0000000000s\"}",
            "{\"payload\":{\"a\":1}}",
            "{\"payload\":{\"a\":1,\"@type\":\"type.googleapis.com/Nope\"}}",
            "{\"name\":\"\\ud800\"}",
        };
        size_t i;

        for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
            CHECK_STATUS(PBW_ERR_INVALID,
                         pbw_json_parse(msg, bad[i], strlen(bad[i]),
                                        &decoded));
            CHECK(decoded == NULL);
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
    pbw_message_free(msg);

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

    CHECK_STATUS(PBW_ERR_INVALID,
                 pbw_encode_deterministic(NULL, &det, &det_len));
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
    test_tag_limits(schema);
    test_field_number_edges(schema);
    test_pack_all_types(schema);
    test_recursive_type(schema);
    test_bool_and_float_bits(schema);
    test_enum_edges(schema);
    test_explicit_presence(schema);
    test_field_order(schema);
    test_duplicate_occurrences(schema);
    test_groups(schema);
    test_unknown_only(schema);
    test_truncation_and_mutation(schema);
    test_large_payloads(schema);
    test_length_patch_boundaries(schema);
    test_ctx(schema);
    test_ctx_retain_cap(schema);
    test_ctx_pool(schema);
    test_errors(schema);
    test_depth_limit(schema);
    test_enum(schema);
    test_proto2_required();
    test_closed_enum();
    test_group_fields();
    test_hand_built_extensions();
    test_text_format();
    test_json_format();
    test_deterministic();
    test_deterministic_edges();
    test_limits(schema);
    test_streaming(schema);
    test_streaming_edges(schema);
    test_schema_errors();

    pbw_schema_free(schema);

    if (failures != 0) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("test_pbwire: all checks passed\n");
    return 0;
}

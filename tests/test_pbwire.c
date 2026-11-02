/* Assert-based self-check for the pbwire runtime: no test framework, no
 * dependencies. */

#include "pbwire.h"

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
    /* 5-byte tag varint whose value exceeds 32 bits (bit 35 set): rejected.
     * Truncating it would silently turn it into a valid tag for field 2. */
    {
        static const uint8_t wide_tag[] = {0x90, 0x80, 0x80, 0x80, 0x70, 0x01};
        CHECK_STATUS(PBW_ERR_INVALID,
                     pbw_decode(type, wide_tag, sizeof(wide_tag), &decoded));
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

    /* groups are not supported as fields (they are skipped when decoding) */
    schema = pbw_schema_new();
    msg = pbw_schema_add_message(schema, "M");
    pbw_message_add_field(msg, "g", 1, PBW_TYPE_GROUP, PBW_LABEL_OPTIONAL);
    CHECK_STATUS(PBW_ERR_TYPE, pbw_schema_link(schema));
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
    test_errors(schema);
    test_depth_limit(schema);
    test_enum(schema);
    test_schema_errors();

    pbw_schema_free(schema);

    if (failures != 0) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("test_pbwire: all checks passed\n");
    return 0;
}

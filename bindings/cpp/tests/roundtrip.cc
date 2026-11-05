/* C++ binding self-check: struct -> wire -> struct, plus the failure paths a
 * C++ caller hits. No framework, no dependencies beyond the C runtime.
 *
 * Typed-compat coverage (TESTSPEC T1-T6, dependency-free half: the same
 * matrix3.bin reference bytes and vectors.json values the C self-check
 * pins, written the C++ way). The live libprotobuf half of T4 plus the
 * shared-snippet T5 live in compat_oracle.cc, which needs protoc and stays
 * out of `make test-cpp` like `make bench-c` does.
 *
 * T1  struct encode == matrix3.bin
 * T2  decode of the reference reads every field back (incl. presence)
 * T3  empty/explicit-zero round-trips (see test_struct.c for why
 *     implicit-presence zeros are still emitted)
 * T5  one workflow through the one-shot calls and the compiled codec
 *     gives identical bytes and identical fields
 * T6  truncated/overlong wire and bad arguments fail loudly; unknown
 *     open-enum values are kept like the reference keeps them
 *
 *     make test-cpp
 */
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

/* Relative so the test needs no new -I: the Makefile rule for this binary
 * only carries the runtime and the examples include paths. */
#include "../../c/tests/gen/matrix3.h"

#include "gen/addressbook.h"
#include "pbwire.h"

static int failures;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__,    \
                         __LINE__, #cond);                                \
            failures++;                                                   \
        }                                                                 \
    } while (0)

#define CHECK_OK(expr) CHECK((expr) == PBW_OK)

static bool
read_reference(const char *path, std::vector<uint8_t> &out)
{
    std::FILE *f = std::fopen(path, "rb");

    if (f == nullptr) {
        return false;
    }
    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    if (size < 0) {
        std::fclose(f);
        return false;
    }
    std::rewind(f);
    out.resize((size_t)size > 0 ? (size_t)size : 1);
    out.resize((size_t)size);
    size_t n = std::fread(out.data(), 1, (size_t)size, f);
    std::fclose(f);
    return n == (size_t)size;
}

static uint32_t
float_bits(float f)
{
    uint32_t u;

    std::memcpy(&u, &f, sizeof(u));
    return u;
}

static bool
person_equal(const tutorial_person_t *a, const tutorial_person_t *b)
{
    size_t i;

    if (std::strcmp(a->name, b->name) != 0 || a->id != b->id ||
        a->n_phones != b->n_phones) {
        return false;
    }
    for (i = 0; i < a->n_phones; i++) {
        if (std::strcmp(a->phones[i].number, b->phones[i].number) != 0 ||
            a->phones[i].has_type != b->phones[i].has_type) {
            return false;
        }
    }
    return true;
}

/* The populated matrix3.txt values, mirrored as C++ aggregates. */
static void
fill_scalars3(structtest_scalars3_t &s, structtest_inner3_t &inner,
              int32_t *nums, const char **tags, structtest_inner3_t *inners,
              bool *flags)
{
    s = {};
    s.i32 = INT32_MIN;
    s.i64 = INT64_MAX;
    s.u32 = UINT32_MAX;
    s.u64 = UINT64_MAX;
    s.s32 = INT32_MIN;
    s.s64 = INT64_MIN;
    s.fx32 = 0xdeadbeefu;
    s.fx64 = UINT64_C(72623859790382856);
    s.sfx32 = INT32_MIN;
    s.sfx64 = INT64_MIN;
    s.f32 = std::numeric_limits<float>::infinity();
    s.f64 = -123456.789;
    s.flag = true;
    s.str = "hello";
    s.color = STRUCTTEST_COLOR3_COLOR3_GREEN;
    inner = {};
    inner.name = "kid";
    inner.value = 42;
    s.inner = &inner;
    nums[0] = 1;
    nums[1] = -5;
    nums[2] = 300;
    s.nums = nums;
    s.n_nums = 3;
    tags[0] = "a";
    tags[1] = "bb";
    s.tags = tags;
    s.n_tags = 2;
    inners[0] = {};
    inners[0].name = "x";
    inners[0].value = 1;
    inners[1] = {};
    inners[1].name = "y";
    inners[1].value = -2;
    s.inners = inners;
    s.n_inners = 2;
    s.opt_i32 = -7;
    s.has_opt_i32 = 1;
    s.opt_str = "opt";
    flags[0] = true;
    flags[1] = false;
    flags[2] = true;
    s.flags = flags;
    s.n_flags = 3;
}

/* T1+T2: encode equals the protoc reference bytes; decode recovers every
 * field; re-encode is byte-identical. */
static void
test_matrix3_reference()
{
    structtest_scalars3_t s = {};
    structtest_inner3_t inner = {};
    int32_t nums[3];
    const char *tags[2];
    structtest_inner3_t inners[2] = {};
    bool flags[3];
    structtest_scalars3_t got = {};
    pbw_schema *schema = nullptr;
    std::vector<uint8_t> expect;
    uint8_t *wire = nullptr;
    uint8_t *wire2 = nullptr;
    size_t wire_len = 0;
    size_t wire2_len = 0;

    CHECK(read_reference("bindings/c/tests/data/matrix3.bin", expect));
    CHECK_OK(pbw_struct_schema_load(MATRIX3_DESC_PATH, &schema));
    if (expect.empty() || schema == nullptr) {
        pbw_schema_free(schema);
        return;
    }
    fill_scalars3(s, inner, nums, tags, inners, flags);
    CHECK_OK(structtest_scalars3_encode(schema, &s, &wire, &wire_len));
    CHECK(wire_len == expect.size() &&
          std::memcmp(wire, expect.data(), expect.size()) == 0);

    CHECK_OK(structtest_scalars3_decode(schema, expect.data(), expect.size(),
                                        &got));
    CHECK(got.i32 == INT32_MIN);
    CHECK(got.i64 == INT64_MAX);
    CHECK(got.u32 == UINT32_MAX);
    CHECK(got.u64 == UINT64_MAX);
    CHECK(got.s32 == INT32_MIN);
    CHECK(got.s64 == INT64_MIN);
    CHECK(got.fx32 == 0xdeadbeefu);
    CHECK(got.fx64 == UINT64_C(72623859790382856));
    CHECK(got.sfx32 == INT32_MIN);
    CHECK(got.sfx64 == INT64_MIN);
    CHECK(float_bits(got.f32) == UINT32_C(0x7f800000));
    CHECK(got.f64 == -123456.789);
    CHECK(got.flag == true);
    CHECK(got.str != nullptr && std::strcmp(got.str, "hello") == 0);
    CHECK(got.color == STRUCTTEST_COLOR3_COLOR3_GREEN);
    CHECK(got.inner != nullptr && std::strcmp(got.inner->name, "kid") == 0 &&
          got.inner->value == 42);
    CHECK(got.n_nums == 3 && got.nums[0] == 1 && got.nums[1] == -5 &&
          got.nums[2] == 300);
    CHECK(got.n_tags == 2 && std::strcmp(got.tags[0], "a") == 0 &&
          std::strcmp(got.tags[1], "bb") == 0);
    CHECK(got.n_inners == 2 && std::strcmp(got.inners[0].name, "x") == 0 &&
          got.inners[0].value == 1 && got.inners[1].value == -2);
    CHECK(got.has_opt_i32 == 1 && got.opt_i32 == -7);
    CHECK(got.opt_str != nullptr && std::strcmp(got.opt_str, "opt") == 0);
    CHECK(got.n_flags == 3 && got.flags[0] == true &&
          got.flags[1] == false && got.flags[2] == true);

    CHECK_OK(structtest_scalars3_encode(schema, &got, &wire2, &wire2_len));
    CHECK(wire2_len == expect.size() &&
          std::memcmp(wire2, expect.data(), expect.size()) == 0);
    structtest_scalars3_free(&got);
    pbw_free(wire2);
    pbw_free(wire);
    pbw_schema_free(schema);
}

/* T3: a zeroed struct round-trips to zero; an explicit opt_i32 zero keeps its
 * presence. Field 20 is the last row of the pinned layout, so the explicit
 * zero lands as a 3-byte {0xa0, 0x01, 0x00} suffix on the zero wire. */
static void
test_empty_and_explicit_zero()
{
    structtest_scalars3_t zero = {};
    structtest_scalars3_t back = {};
    structtest_scalars3_t explicit_zero = {};
    structtest_scalars3_t back2 = {};
    pbw_schema *schema = nullptr;
    uint8_t *wire = nullptr;
    uint8_t *wire2 = nullptr;
    size_t wire_len = 0;
    size_t wire2_len = 0;

    CHECK_OK(pbw_struct_schema_load(MATRIX3_DESC_PATH, &schema));
    if (schema == nullptr) {
        return;
    }
    CHECK_OK(structtest_scalars3_encode(schema, &zero, &wire, &wire_len));
    CHECK_OK(structtest_scalars3_decode(schema, wire, wire_len, &back));
    CHECK(back.i32 == 0 && back.u64 == 0 && back.f64 == 0.0);
    CHECK(back.flag == false && back.str == nullptr && back.inner == nullptr);
    CHECK(back.n_nums == 0 && back.n_tags == 0 && back.n_inners == 0);
    CHECK(back.has_opt_i32 == 0 && back.opt_str == nullptr);
    CHECK(back.color == STRUCTTEST_COLOR3_COLOR3_UNSPECIFIED);
    CHECK_OK(structtest_scalars3_encode(schema, &back, &wire2, &wire2_len));
    CHECK(wire2_len == wire_len && std::memcmp(wire2, wire, wire_len) == 0);
    structtest_scalars3_free(&back);
    pbw_free(wire2);
    wire2 = nullptr;

    explicit_zero.opt_i32 = 0;
    explicit_zero.has_opt_i32 = 1;
    CHECK_OK(structtest_scalars3_encode(schema, &explicit_zero, &wire2,
                                        &wire2_len));
    CHECK(wire2_len == wire_len + 3);
    if (wire2_len == wire_len + 3) {
        CHECK(std::memcmp(wire2, wire, wire_len) == 0);
        CHECK(wire2[wire_len] == 0xa0 && wire2[wire_len + 1] == 0x01 &&
              wire2[wire_len + 2] == 0x00);
    }
    CHECK_OK(structtest_scalars3_decode(schema, wire2, wire2_len, &back2));
    CHECK(back2.has_opt_i32 == 1 && back2.opt_i32 == 0);
    CHECK(back2.opt_str == nullptr);
    structtest_scalars3_free(&back2);
    pbw_free(wire2);
    pbw_free(wire);
    pbw_schema_free(schema);
}

/* T5: the vectors.json business values through the one-shot calls and the
 * compiled codec: identical bytes, identical fields. */
static void
test_entry_point_parity()
{
    structtest_scalars3_t s = {};
    structtest_inner3_t inner = {};
    int32_t nums[3];
    const char *tags[2];
    structtest_inner3_t inners[2] = {};
    bool flags[3];
    structtest_scalars3_t via_calls = {};
    structtest_scalars3_t via_codec = {};
    pbw_schema *schema = nullptr;
    pbw_struct_codec *codec = nullptr;
    pbw_struct_buf buf = {nullptr, 0, 0};
    uint8_t *wire = nullptr;
    size_t wire_len = 0;

    CHECK_OK(pbw_struct_schema_load(MATRIX3_DESC_PATH, &schema));
    if (schema == nullptr) {
        return;
    }
    fill_scalars3(s, inner, nums, tags, inners, flags);
    CHECK_OK(pbw_struct_codec_init(&codec, schema, "structtest.Scalars3",
                                   structtest_scalars3_fields, 22));
    if (codec == nullptr) {
        pbw_schema_free(schema);
        return;
    }
    CHECK_OK(structtest_scalars3_encode(schema, &s, &wire, &wire_len));
    CHECK_OK(pbw_struct_codec_encode(codec, &s, &buf));
    CHECK(buf.len == wire_len && std::memcmp(buf.data, wire, wire_len) == 0);

    CHECK_OK(structtest_scalars3_decode(schema, wire, wire_len, &via_calls));
    CHECK_OK(pbw_struct_codec_decode(codec, buf.data, buf.len, nullptr,
                                     &via_codec));
    CHECK(via_calls.i32 == via_codec.i32 && via_calls.i64 == via_codec.i64);
    CHECK(float_bits(via_calls.f32) == float_bits(via_codec.f32));
    CHECK(via_calls.f64 == via_codec.f64);
    CHECK(std::strcmp(via_calls.str, via_codec.str) == 0);
    CHECK(via_calls.color == via_codec.color);
    CHECK(std::strcmp(via_calls.inner->name, via_codec.inner->name) == 0);
    CHECK(via_calls.inner->value == via_codec.inner->value);
    CHECK(via_calls.n_nums == via_codec.n_nums &&
          via_calls.nums[2] == via_codec.nums[2]);
    CHECK(via_calls.n_inners == via_codec.n_inners &&
          via_calls.inners[1].value == via_codec.inners[1].value);
    CHECK(via_calls.has_opt_i32 == via_codec.has_opt_i32 &&
          via_calls.opt_i32 == via_codec.opt_i32);
    CHECK(std::strcmp(via_calls.opt_str, via_codec.opt_str) == 0);
    CHECK(via_calls.n_flags == via_codec.n_flags &&
          via_calls.flags[0] == via_codec.flags[0]);
    structtest_scalars3_free(&via_calls);
    structtest_scalars3_free(&via_codec);
    pbw_struct_codec_free(codec);
    pbw_struct_buf_free(&buf);
    pbw_free(wire);
    pbw_schema_free(schema);
}

/* T6: truncated and overlong varints fail loudly; an unknown open-enum value
 * is kept (the reference parses it into the field too); NULL arguments are
 * rejected instead of crashing. */
static void
test_wire_errors()
{
    structtest_scalars3_t got = {};
    pbw_schema *schema = nullptr;
    static const uint8_t truncated[] = {0x08};
    static const uint8_t overlong[] = {0x08, 0xff, 0xff, 0xff, 0xff, 0xff,
                                       0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    static const uint8_t unknown_enum[] = {0x78, 0x63};
    uint8_t *wire = nullptr;
    size_t wire_len = 0;

    CHECK_OK(pbw_struct_schema_load(MATRIX3_DESC_PATH, &schema));
    if (schema == nullptr) {
        return;
    }
    CHECK(pbw_struct_decode(schema, "structtest.Scalars3",
                            structtest_scalars3_fields, 22, truncated,
                            sizeof(truncated), &got) != PBW_OK);
    got = {};
    CHECK(pbw_struct_decode(schema, "structtest.Scalars3",
                            structtest_scalars3_fields, 22, overlong,
                            sizeof(overlong), &got) != PBW_OK);
    got = {};
    CHECK_OK(structtest_scalars3_decode(schema, unknown_enum,
                                        sizeof(unknown_enum), &got));
    CHECK(got.color == 99);
    CHECK_OK(structtest_scalars3_encode(schema, &got, &wire, &wire_len));
    CHECK_OK(structtest_scalars3_decode(schema, wire, wire_len, &got));
    CHECK(got.color == 99);
    structtest_scalars3_free(&got);
    pbw_free(wire);
    wire = nullptr;
    got = {};
    CHECK(structtest_scalars3_encode(schema, nullptr, &wire, &wire_len) ==
          PBW_ERR_INVALID);
    CHECK(wire == nullptr);
    pbw_schema_free(schema);
}

int
main(void)
{
    pbw_schema *schema = nullptr;
    tutorial_person_phone_number_t phones[2];
    tutorial_person_t in = {}, out = {};
    uint8_t *wire = nullptr;
    size_t wire_len = 0;

    CHECK_OK(pbw_struct_schema_load(ADDRESSBOOK_DESC_PATH, &schema));
    if (schema == nullptr) {
        return 1;
    }

    phones[0] = {};
    phones[0].number = "555-1234";
    phones[0].type = TUTORIAL_PERSON_PHONE_TYPE_MOBILE;
    phones[0].has_type = 1;
    phones[1] = {};
    phones[1].number = "555-9999";

    in.name = "alice";
    in.id = 1234;
    in.email = "alice@example.com";
    in.phones = phones;
    in.n_phones = 2;

    CHECK_OK(tutorial_person_encode(schema, &in, &wire, &wire_len));
    CHECK(wire != nullptr && wire_len > 0);
    memset(&out, 0, sizeof(out));
    CHECK_OK(tutorial_person_decode(schema, wire, wire_len, &out));
    CHECK(person_equal(&in, &out));
    tutorial_person_free(&out);
    pbw_free(wire);

    /* A missing required field must fail, not write a broken message. */
    in.name = nullptr;
    wire = nullptr;
    CHECK(pbw_struct_encode(schema, "tutorial.Person", tutorial_person_fields,
                            4, &in, &wire, &wire_len) == PBW_ERR_REQUIRED);
    CHECK(wire == nullptr);

    pbw_schema_free(schema);

    test_matrix3_reference();
    test_empty_and_explicit_zero();
    test_entry_point_parity();
    test_wire_errors();

    if (failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    return 0;
}

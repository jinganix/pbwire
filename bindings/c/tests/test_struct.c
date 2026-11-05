/* Struct-binding self-check: the generated headers under
 * bindings/c/tests/gen (produced by protoc-gen-pbwire-c, pinned by the
 * plugin golden tests) encode/decode through pbw_struct_* against the
 * reference bytes protoc produced (--encode from the checked-in .txt).
 * No test framework, no dependencies; run from the repo root so the
 * relative fixture paths resolve.
 *
 * Typed-compat coverage (TESTSPEC T1-T6, adapted: the struct codec IS the
 * typed model, so vectors.json lives here as C literals in fill_scalars3;
 * the oracle half runs through the dynamic tree API below plus the live
 * libprotobuf differential in bindings/cpp/tests/compat_oracle.cc):
 *
 *   T1  struct encode == matrix3.bin / matrix2.bin / addressbook.bin
 *   T2  decode of each reference reads every field back (incl. presence)
 *   T3  empty/explicit-zero round-trips (implicit-presence zeros are still
 *       emitted: the struct has no has-flag to omit them by, and the bytes
 *       stay valid protobuf; explicit-presence absence omits exactly)
 *   T4  dynamic-tree cross-decode of matrix3.bin agrees field by field,
 *       and re-encodes to the same bytes
 *   T5  one workflow through the one-shot calls and the compiled codec
 *       gives identical bytes and identical fields
 *   T6  bad wire, overlong varints, wrong kinds and missing required
 *       fields fail loudly; unknown open-enum values are kept like the
 *       reference keeps them. Oneof messages never reach the codec: the
 *       plugin rejects them at generation time (cgen.go).
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pbwire.h"

#include "gen/matrix2.h"
#include "gen/matrix3.h"
#include "gen/addressbook.h"

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

static int
read_file(const char *path, uint8_t **data, size_t *len)
{
    FILE *f = fopen(path, "rb");
    long size;
    size_t n;

    if (f == NULL) {
        return -1;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return -1;
    }
    size = ftell(f);
    if (size < 0) {
        fclose(f);
        return -1;
    }
    rewind(f);
    *data = malloc((size_t)size > 0 ? (size_t)size : 1);
    if (*data == NULL) {
        fclose(f);
        return -1;
    }
    n = fread(*data, 1, (size_t)size, f);
    fclose(f);
    if (n != (size_t)size) {
        free(*data);
        return -1;
    }
    *len = n;
    return 0;
}

/* Float bit pattern without math.h: inf must survive the round trip. */
static uint32_t
float_bits(float f)
{
    uint32_t u;

    memcpy(&u, &f, sizeof(u));
    return u;
}

/* The populated matrix3.txt values, mirrored as C structs. */
static void
fill_scalars3(structtest_scalars3_t *s, structtest_inner3_t *inner,
              int32_t *nums, const char **tags, structtest_inner3_t *inners)
{
    memset(s, 0, sizeof(*s));
    s->i32 = INT32_MIN;
    s->i64 = INT64_MAX;
    s->u32 = UINT32_MAX;
    s->u64 = UINT64_MAX;
    s->s32 = INT32_MIN;
    s->s64 = INT64_MIN;
    s->fx32 = 0xdeadbeefu;
    s->fx64 = UINT64_C(72623859790382856);
    s->sfx32 = INT32_MIN;
    s->sfx64 = INT64_MIN;
    s->f32 = 1.0f / 0.0f; /* inf, matches `f32: inf` in matrix3.txt */
    s->f64 = -123456.789;
    s->flag = 1;
    s->str = "hello";
    s->color = STRUCTTEST_COLOR3_COLOR3_GREEN;
    inner->name = "kid";
    inner->value = 42;
    s->inner = inner;
    nums[0] = 1;
    nums[1] = -5;
    nums[2] = 300;
    s->nums = nums;
    s->n_nums = 3;
    tags[0] = "a";
    tags[1] = "bb";
    s->tags = tags;
    s->n_tags = 2;
    inners[0].name = "x";
    inners[0].value = 1;
    inners[1].name = "y";
    inners[1].value = -2;
    s->inners = inners;
    s->n_inners = 2;
    s->opt_i32 = -7;
    s->has_opt_i32 = 1;
    s->opt_str = "opt";
}

/* flags lives apart: a bool array the fill helper does not own. */
static void
fill_flags(structtest_scalars3_t *s, bool *flags)
{
    flags[0] = 1;
    flags[1] = 0;
    flags[2] = 1;
    s->flags = flags;
    s->n_flags = 3;
}

/* Encode must equal the protoc reference bytes; decode must recover
 * every field; re-encode must be byte-identical. */
static void
test_matrix3_reference(void)
{
    structtest_scalars3_t s;
    structtest_inner3_t inner;
    int32_t nums[3];
    const char *tags[2];
    structtest_inner3_t inners[2];
    bool flags[3];
    structtest_scalars3_t got;
    pbw_schema *schema = NULL;
    uint8_t *expect = NULL;
    uint8_t *wire = NULL;
    uint8_t *wire2 = NULL;
    size_t expect_len = 0;
    size_t wire_len = 0;
    size_t wire2_len = 0;

    CHECK(read_file("bindings/c/tests/data/matrix3.bin", &expect,
                    &expect_len) == 0);
    CHECK_OK(pbw_struct_schema_load(MATRIX3_DESC_PATH, &schema));
    if (schema == NULL || expect == NULL) {
        goto done;
    }
    fill_scalars3(&s, &inner, nums, tags, inners);
    fill_flags(&s, flags);
    CHECK_OK(structtest_scalars3_encode(schema, &s, &wire, &wire_len));
    CHECK(wire_len == expect_len && memcmp(wire, expect, expect_len) == 0);

    CHECK_OK(structtest_scalars3_decode(schema, expect, expect_len, &got));
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
    CHECK(got.flag == 1);
    CHECK(got.str != NULL && strcmp(got.str, "hello") == 0);
    CHECK(got.color == STRUCTTEST_COLOR3_COLOR3_GREEN);
    CHECK(got.inner != NULL);
    if (got.inner != NULL) {
        CHECK(strcmp(got.inner->name, "kid") == 0);
        CHECK(got.inner->value == 42);
    }
    CHECK(got.n_nums == 3);
    if (got.n_nums == 3) {
        CHECK(got.nums[0] == 1 && got.nums[1] == -5 && got.nums[2] == 300);
    }
    CHECK(got.n_tags == 2);
    if (got.n_tags == 2) {
        CHECK(strcmp(got.tags[0], "a") == 0 && strcmp(got.tags[1], "bb") == 0);
    }
    CHECK(got.n_inners == 2);
    if (got.n_inners == 2) {
        CHECK(strcmp(got.inners[0].name, "x") == 0);
        CHECK(got.inners[0].value == 1);
        CHECK(strcmp(got.inners[1].name, "y") == 0);
        CHECK(got.inners[1].value == -2);
    }
    CHECK(got.has_opt_i32 == 1 && got.opt_i32 == -7);
    CHECK(got.opt_str != NULL && strcmp(got.opt_str, "opt") == 0);
    CHECK(got.n_flags == 3);
    if (got.n_flags == 3) {
        CHECK(got.flags[0] == 1 && got.flags[1] == 0 && got.flags[2] == 1);
    }

    CHECK_OK(structtest_scalars3_encode(schema, &got, &wire2, &wire2_len));
    CHECK(wire2_len == expect_len && memcmp(wire2, expect, expect_len) == 0);
    structtest_scalars3_free(&got);
done:
    pbw_free(wire2);
    pbw_free(wire);
    free(expect);
    pbw_schema_free(schema);
}

static void
fill_scalars2(structtest_scalars2_t *s, structtest_inner2_t *inner,
              int32_t *nums, int32_t *packed, structtest_inner2_t *inners)
{
    memset(s, 0, sizeof(*s));
    s->id = 99;
    s->i32 = -5;
    s->has_i32 = 1;
    s->s64 = -1;
    s->has_s64 = 1;
    s->fx32 = 305419896u;
    s->has_fx32 = 1;
    s->dbl = 2.5;
    s->has_dbl = 1;
    s->flag = 1;
    s->has_flag = 1;
    s->str = "hi";
    s->shade = STRUCTTEST_SHADE_SHADE_NEG;
    s->has_shade = 1;
    nums[0] = 4;
    nums[1] = 5;
    s->nums = nums;
    s->n_nums = 2;
    packed[0] = 6;
    packed[1] = 7;
    packed[2] = 8;
    s->packed_nums = packed;
    s->n_packed_nums = 3;
    inner->name = "req";
    inner->value = 9;
    inner->has_value = 1;
    s->inner = inner;
    /* inners[0] leaves value absent: no has-flag, default not materialized. */
    inners[0].name = "a";
    inners[0].value = 0;
    inners[0].has_value = 0;
    inners[1].name = "b";
    inners[1].value = -3;
    inners[1].has_value = 1;
    s->inners = inners;
    s->n_inners = 2;
}

/* The compiled codec is the hot path: same bytes as the one-shot calls,
 * caller-owned buffers reused, and an arena that owns the decoded tree. */
static void
test_codec(void)
{
    structtest_scalars3_t s;
    structtest_scalars3_t a;
    structtest_scalars3_t b;
    structtest_inner3_t inner;
    int32_t nums[3];
    const char *tags[2];
    structtest_inner3_t inners[2];
    bool flags[3];
    pbw_schema *schema = NULL;
    pbw_struct_codec *codec = NULL;
    pbw_struct_codec *wrong = (pbw_struct_codec *)0x1;
    pbw_struct_buf buf = {NULL, 0, 0};
    pbw_struct_arena *arena = NULL;
    uint8_t *expect = NULL;
    uint8_t *wire = NULL;
    size_t expect_len = 0;
    size_t wire_len = 0;
    size_t cap;

    CHECK(read_file("bindings/c/tests/data/matrix3.bin", &expect,
                    &expect_len) == 0);
    CHECK_OK(pbw_struct_schema_load(MATRIX3_DESC_PATH, &schema));
    if (schema == NULL || expect == NULL) {
        goto done;
    }
    fill_scalars3(&s, &inner, nums, tags, inners);
    fill_flags(&s, flags);

    /* Same bytes as the one-shot encoder, into a buffer that keeps its
     * capacity across calls. */
    CHECK_OK(pbw_struct_codec_init(&codec, schema, "structtest.Scalars3",
                                   structtest_scalars3_fields, 22));
    CHECK(codec != NULL);
    if (codec == NULL) {
        goto done;
    }
    CHECK_OK(pbw_struct_codec_encode(codec, &s, &buf));
    CHECK(buf.len == expect_len && memcmp(buf.data, expect, expect_len) == 0);
    cap = buf.cap;
    CHECK_OK(structtest_scalars3_encode(schema, &s, &wire, &wire_len));
    CHECK(wire_len == expect_len && memcmp(wire, expect, expect_len) == 0);
    CHECK_OK(pbw_struct_codec_encode(codec, &s, &buf));
    CHECK(buf.len == expect_len && memcmp(buf.data, expect, expect_len) == 0);
    CHECK(buf.cap == cap); /* steady state reuses the allocation */

    /* Arena decode: no per-value allocation to release, and the arena is
     * reused (the second decode invalidates the first tree). */
    memset(&a, 0, sizeof(a));
    CHECK_OK(pbw_struct_codec_decode(codec, expect, expect_len, &arena, &a));
    CHECK(arena != NULL);
    CHECK(a.n_nums == 3 && a.n_tags == 2 && a.n_inners == 2 && a.n_flags == 3);
    CHECK(a.str != NULL && strcmp(a.str, "hello") == 0);
    CHECK(a.inner != NULL && strcmp(a.inner->name, "kid") == 0);
    CHECK(a.inners != NULL && a.inners[1].value == -2);
    memset(&b, 0, sizeof(b));
    CHECK_OK(pbw_struct_codec_decode(codec, expect, expect_len, &arena, &b));
    CHECK(b.n_nums == 3 && b.str != NULL && strcmp(b.str, "opt") != 0);
    CHECK(b.opt_str != NULL && strcmp(b.opt_str, "opt") == 0);
    /* Re-encode from an arena-decoded tree: still byte identical. */
    CHECK_OK(pbw_struct_codec_encode(codec, &b, &buf));
    CHECK(buf.len == expect_len && memcmp(buf.data, expect, expect_len) == 0);

    /* No arena means per-value allocations, released by pbw_struct_free. */
    memset(&a, 0, sizeof(a));
    CHECK_OK(pbw_struct_codec_decode(codec, expect, expect_len, NULL, &a));
    CHECK(a.str != NULL && strcmp(a.str, "hello") == 0);
    structtest_scalars3_free(&a);

    /* A failed decode cleans up after itself on both allocator paths. */
    memset(&a, 0, sizeof(a));
    CHECK_STATUS(PBW_ERR_INVALID,
                 pbw_struct_codec_decode(codec, expect, expect_len / 2, NULL,
                                         &a));
    memset(&b, 0, sizeof(b));
    CHECK_STATUS(PBW_ERR_INVALID,
                 pbw_struct_codec_decode(codec, expect, expect_len / 2, &arena,
                                         &b));

    /* A repeated numeric field may arrive unpacked: field 17 (nums) as plain
     * varints, field 22 (flags) as single bools. Both must be accepted. */
    {
        static const uint8_t unpacked[] = {
            0x88,
            0x01,
            0x01, /* nums = 1 */
            0x88,
            0x01,
            0x02, /* nums = 2 */
            0xb0,
            0x01,
            0x01, /* flags[0] = true */
            0xb0,
            0x01,
            0x00, /* flags[1] = false */
        };

        memset(&b, 0, sizeof(b));
        CHECK_OK(pbw_struct_codec_decode(codec, unpacked, sizeof(unpacked),
                                         &arena, &b));
        CHECK(b.n_nums == 2);
        if (b.n_nums == 2) {
            CHECK(b.nums[0] == 1 && b.nums[1] == 2);
        }
        CHECK(b.n_flags == 2);
        if (b.n_flags == 2) {
            CHECK(b.flags[0] == 1 && b.flags[1] == 0);
        }
    }

    /* Codec setup reports bad arguments instead of half-building a codec. */
    CHECK_STATUS(PBW_ERR_INVALID, pbw_struct_codec_init(NULL, schema,
                                                        "structtest.Scalars3",
                                                        structtest_scalars3_fields,
                                                        22));
    CHECK_STATUS(PBW_ERR_INVALID,
                 pbw_struct_codec_init(&wrong, NULL, "structtest.Scalars3",
                                       structtest_scalars3_fields, 22));
    CHECK(wrong == NULL);
    CHECK_STATUS(PBW_ERR_INVALID,
                 pbw_struct_codec_init(&wrong, schema, "structtest.Nope",
                                       structtest_scalars3_fields, 22));
    CHECK(wrong == NULL);
    CHECK_STATUS(PBW_ERR_INVALID,
                 pbw_struct_codec_init(&wrong, schema, "structtest.Scalars3",
                                       structtest_scalars3_fields, 21));
    CHECK(wrong == NULL);
    CHECK_STATUS(PBW_ERR_INVALID, pbw_struct_codec_encode(NULL, &s, &buf));
    CHECK_STATUS(PBW_ERR_INVALID, pbw_struct_codec_encode(codec, NULL, &buf));
    CHECK_STATUS(PBW_ERR_INVALID,
                 pbw_struct_codec_encode(codec, &s, NULL));
    structtest_scalars3_free(&a);
done:
    pbw_struct_codec_free(NULL);
    pbw_struct_codec_free(codec);
    pbw_struct_arena_free(arena);
    pbw_struct_arena_free(NULL);
    pbw_struct_buf_free(&buf);
    pbw_struct_buf_free(NULL);
    pbw_free(wire);
    free(expect);
    pbw_schema_free(schema);
}

static void
test_matrix2_reference(void)
{
    structtest_scalars2_t s;
    structtest_inner2_t inner;
    int32_t nums[2];
    int32_t packed[3];
    structtest_inner2_t inners[2];
    structtest_scalars2_t got;
    pbw_schema *schema = NULL;
    uint8_t *expect = NULL;
    uint8_t *wire = NULL;
    uint8_t *wire2 = NULL;
    size_t expect_len = 0;
    size_t wire_len = 0;
    size_t wire2_len = 0;

    CHECK(read_file("bindings/c/tests/data/matrix2.bin", &expect,
                    &expect_len) == 0);
    CHECK_OK(pbw_struct_schema_load(MATRIX2_DESC_PATH, &schema));
    if (schema == NULL || expect == NULL) {
        goto done;
    }
    fill_scalars2(&s, &inner, nums, packed, inners);
    CHECK_OK(structtest_scalars2_encode(schema, &s, &wire, &wire_len));
    CHECK(wire_len == expect_len && memcmp(wire, expect, expect_len) == 0);

    CHECK_OK(structtest_scalars2_decode(schema, expect, expect_len, &got));
    CHECK(got.id == 99);
    CHECK(got.has_i32 == 1 && got.i32 == -5);
    CHECK(got.has_s64 == 1 && got.s64 == -1);
    CHECK(got.has_fx32 == 1 && got.fx32 == 305419896u);
    CHECK(got.has_dbl == 1 && got.dbl == 2.5);
    CHECK(got.has_flag == 1 && got.flag == 1);
    CHECK(got.str != NULL && strcmp(got.str, "hi") == 0);
    CHECK(got.has_shade == 1 && got.shade == STRUCTTEST_SHADE_SHADE_NEG);
    CHECK(got.n_nums == 2);
    if (got.n_nums == 2) {
        CHECK(got.nums[0] == 4 && got.nums[1] == 5);
    }
    CHECK(got.n_packed_nums == 3);
    if (got.n_packed_nums == 3) {
        CHECK(got.packed_nums[0] == 6 && got.packed_nums[1] == 7 &&
              got.packed_nums[2] == 8);
    }
    CHECK(got.inner != NULL);
    if (got.inner != NULL) {
        CHECK(strcmp(got.inner->name, "req") == 0);
        CHECK(got.inner->has_value == 1 && got.inner->value == 9);
    }
    CHECK(got.n_inners == 2);
    if (got.n_inners == 2) {
        CHECK(strcmp(got.inners[0].name, "a") == 0);
        CHECK(got.inners[0].has_value == 0 && got.inners[0].value == 0);
        CHECK(strcmp(got.inners[1].name, "b") == 0);
        CHECK(got.inners[1].has_value == 1 && got.inners[1].value == -3);
    }

    CHECK_OK(structtest_scalars2_encode(schema, &got, &wire2, &wire2_len));
    CHECK(wire2_len == expect_len && memcmp(wire2, expect, expect_len) == 0);
    structtest_scalars2_free(&got);
done:
    pbw_free(wire2);
    pbw_free(wire);
    free(expect);
    pbw_schema_free(schema);
}

/* The addressbook golden: plugin header + descriptor set + pinned bytes.
 * The example demo only checks round-trip self-consistency; here the
 * encode output must equal examples/addressbook.bin. */
static void
test_addressbook_reference(void)
{
    tutorial_address_book_t book;
    tutorial_person_t people[2];
    tutorial_person_phone_number_t phones[2];
    tutorial_address_book_t got;
    pbw_schema *schema = NULL;
    uint8_t *expect = NULL;
    uint8_t *wire = NULL;
    size_t expect_len = 0;
    size_t wire_len = 0;

    memset(&book, 0, sizeof(book));
    memset(people, 0, sizeof(people));
    memset(phones, 0, sizeof(phones));
    people[0].name = "Alice";
    people[0].id = 1;
    phones[0].number = "555-0100";
    phones[0].type = TUTORIAL_PERSON_PHONE_TYPE_MOBILE;
    phones[0].has_type = 1;
    people[0].phones = &phones[0];
    people[0].n_phones = 1;
    people[1].name = "Bob";
    people[1].id = 2;
    people[1].email = "bob@example.com";
    phones[1].number = "555-0200";
    phones[1].type = TUTORIAL_PERSON_PHONE_TYPE_HOME;
    phones[1].has_type = 1;
    people[1].phones = &phones[1];
    people[1].n_phones = 1;
    book.people = people;
    book.n_people = 2;

    CHECK(read_file("examples/addressbook.bin", &expect, &expect_len) == 0);
    CHECK_OK(pbw_struct_schema_load(ADDRESSBOOK_DESC_PATH, &schema));
    if (schema == NULL || expect == NULL) {
        goto done;
    }
    CHECK_OK(tutorial_address_book_encode(schema, &book, &wire, &wire_len));
    CHECK(wire_len == expect_len && memcmp(wire, expect, expect_len) == 0);

    CHECK_OK(tutorial_address_book_decode(schema, expect, expect_len, &got));
    CHECK(got.n_people == 2);
    if (got.n_people == 2) {
        CHECK(strcmp(got.people[0].name, "Alice") == 0);
        CHECK(got.people[0].id == 1);
        CHECK(got.people[0].email == NULL);
        CHECK(got.people[0].n_phones == 1);
        CHECK(got.people[0].phones[0].has_type == 1);
        CHECK(strcmp(got.people[1].email, "bob@example.com") == 0);
        CHECK(got.people[1].phones[0].type ==
              TUTORIAL_PERSON_PHONE_TYPE_HOME);
    }
    tutorial_address_book_free(&got);
done:
    pbw_free(wire);
    free(expect);
    pbw_schema_free(schema);
}

/* Absent singulars stay absent: empty input decodes to a zeroed struct,
 * and an enum without its has-flag decodes to the schema default while
 * the has-flag stays clear. */
static void
test_absent_and_empty(void)
{
    structtest_scalars3_t empty;
    structtest_scalars2_t s2;
    structtest_inner2_t inner;
    structtest_scalars2_t got2;
    pbw_schema *schema3 = NULL;
    pbw_schema *schema2 = NULL;
    uint8_t *wire = NULL;
    size_t wire_len = 0;

    CHECK_OK(pbw_struct_schema_load(MATRIX3_DESC_PATH, &schema3));
    CHECK_OK(pbw_struct_schema_load(MATRIX2_DESC_PATH, &schema2));
    if (schema3 == NULL || schema2 == NULL) {
        goto done;
    }
    memset(&empty, 0, sizeof(empty));
    CHECK_OK(structtest_scalars3_decode(schema3, NULL, 0, &empty));
    CHECK(empty.i32 == 0 && empty.u64 == 0 && empty.f64 == 0.0);
    CHECK(empty.flag == 0 && empty.str == NULL && empty.inner == NULL);
    CHECK(empty.n_nums == 0 && empty.n_tags == 0 && empty.n_inners == 0);
    CHECK(empty.has_opt_i32 == 0 && empty.opt_str == NULL);
    CHECK(empty.color == STRUCTTEST_COLOR3_COLOR3_UNSPECIFIED);
    structtest_scalars3_free(&empty);

    /* Proto2 enum without has-flag: omitted on the wire, schema default
     * (-2) on decode, has-flag still clear. */
    memset(&s2, 0, sizeof(s2));
    memset(&inner, 0, sizeof(inner));
    s2.id = 1;
    inner.name = "only-required-rest-absent";
    s2.inner = &inner;
    CHECK_OK(structtest_scalars2_encode(schema2, &s2, &wire, &wire_len));
    CHECK_OK(structtest_scalars2_decode(schema2, wire, wire_len, &got2));
    CHECK(got2.id == 1);
    CHECK(got2.has_i32 == 0 && got2.i32 == 0);
    CHECK(got2.str == NULL);
    CHECK(got2.has_shade == 0 && got2.shade == STRUCTTEST_SHADE_SHADE_NEG);
    CHECK(got2.inner != NULL);
    if (got2.inner != NULL) {
        CHECK(got2.inner->has_value == 0 && got2.inner->value == 0);
    }
    structtest_scalars2_free(&got2);
done:
    pbw_free(wire);
    pbw_schema_free(schema3);
    pbw_schema_free(schema2);
}

/* Unknown fields are tolerated on decode: known fields still land. */
static void
test_unknown_fields_tolerated(void)
{
    structtest_scalars3_t got;
    pbw_schema *schema = NULL;
    uint8_t *base = NULL;
    uint8_t *wire = NULL;
    size_t base_len = 0;
    /* Field 100, varint 1: tag 0xA0 0x06, value 0x01. */
    static const uint8_t suffix[] = {0xa0, 0x06, 0x01};

    CHECK(read_file("bindings/c/tests/data/matrix3.bin", &base, &base_len) ==
          0);
    CHECK_OK(pbw_struct_schema_load(MATRIX3_DESC_PATH, &schema));
    if (schema == NULL || base == NULL) {
        goto done;
    }
    wire = malloc(base_len + sizeof(suffix));
    CHECK(wire != NULL);
    if (wire == NULL) {
        goto done;
    }
    memcpy(wire, base, base_len);
    memcpy(wire + base_len, suffix, sizeof(suffix));
    CHECK_OK(structtest_scalars3_decode(schema, wire, base_len + sizeof(suffix),
                                        &got));
    CHECK(got.i32 == INT32_MIN);
    CHECK(got.str != NULL && strcmp(got.str, "hello") == 0);
    structtest_scalars3_free(&got);
done:
    free(wire);
    free(base);
    pbw_schema_free(schema);
}

/* A known field carrying the wrong wire type lands in unknown fields:
 * decode succeeds and the struct keeps the zero value. */
static void
test_wrong_wire_type_tolerated(void)
{
    structtest_scalars3_t got;
    pbw_schema *schema = NULL;
    /* Field 1 (i32) as length-delimited "X" instead of a varint. */
    static const uint8_t wire[] = {0x0a, 0x01, 'X'};

    CHECK_OK(pbw_struct_schema_load(MATRIX3_DESC_PATH, &schema));
    if (schema == NULL) {
        return;
    }
    CHECK_OK(structtest_scalars3_decode(schema, wire, sizeof(wire), &got));
    CHECK(got.i32 == 0);
    structtest_scalars3_free(&got);
    pbw_schema_free(schema);
}

/* A proto2 required string left NULL fails at encode time. */
static void
test_missing_required(void)
{
    structtest_inner2_t inner;
    structtest_scalars2_t s;
    pbw_schema *schema = NULL;
    uint8_t *wire = NULL;
    size_t wire_len = 0;

    CHECK_OK(pbw_struct_schema_load(MATRIX2_DESC_PATH, &schema));
    if (schema == NULL) {
        return;
    }
    memset(&inner, 0, sizeof(inner)); /* name stays NULL: required */
    memset(&s, 0, sizeof(s));
    s.id = 1;
    s.inner = &inner;
    CHECK_STATUS(PBW_ERR_REQUIRED,
                 structtest_scalars2_encode(schema, &s, &wire, &wire_len));
    CHECK(wire == NULL);
    pbw_schema_free(schema);
}

/* Truncated and malformed input must not decode. */
static void
test_truncated_and_garbage(void)
{
    structtest_scalars3_t got3;
    structtest_scalars2_t got2;
    pbw_schema *schema3 = NULL;
    pbw_schema *schema2 = NULL;
    uint8_t *bin3 = NULL;
    uint8_t *bin2 = NULL;
    size_t len3 = 0;
    size_t len2 = 0;
    /* Field number 0 is never valid. */
    static const uint8_t bad_tag[] = {0x00};

    CHECK(read_file("bindings/c/tests/data/matrix3.bin", &bin3, &len3) == 0);
    CHECK(read_file("bindings/c/tests/data/matrix2.bin", &bin2, &len2) == 0);
    CHECK_OK(pbw_struct_schema_load(MATRIX3_DESC_PATH, &schema3));
    CHECK_OK(pbw_struct_schema_load(MATRIX2_DESC_PATH, &schema2));
    if (schema3 == NULL || schema2 == NULL || bin3 == NULL || bin2 == NULL) {
        goto done;
    }
    CHECK(len3 > 1 && len2 > 1);
    if (structtest_scalars3_decode(schema3, bin3, len3 - 1, &got3) == PBW_OK) {
        CHECK(0); /* truncated input must fail */
        structtest_scalars3_free(&got3);
    }
    if (structtest_scalars2_decode(schema2, bin2, len2 - 1, &got2) == PBW_OK) {
        CHECK(0);
        structtest_scalars2_free(&got2);
    }
    if (structtest_scalars3_decode(schema3, bad_tag, sizeof(bad_tag), &got3) ==
        PBW_OK) {
        CHECK(0);
        structtest_scalars3_free(&got3);
    }
done:
    free(bin3);
    free(bin2);
    pbw_schema_free(schema3);
    pbw_schema_free(schema2);
}

/* The schema drives: a layout that drops a field, changes a kind or
 * flips repetition must fail instead of silently dropping data. */
static void
test_layout_mismatch(void)
{
    structtest_scalars3_t s;
    structtest_inner3_t inner;
    int32_t nums[3];
    const char *tags[2];
    structtest_inner3_t inners[2];
    bool flags[3];
    structtest_scalars3_t got;
    pbw_schema *schema = NULL;
    pbw_struct_field_t fields[22];
    uint8_t *wire = NULL;
    size_t wire_len = 0;
    uint8_t *bin = NULL;
    size_t bin_len = 0;

    CHECK(read_file("bindings/c/tests/data/matrix3.bin", &bin, &bin_len) ==
          0);
    CHECK_OK(pbw_struct_schema_load(MATRIX3_DESC_PATH, &schema));
    if (schema == NULL || bin == NULL) {
        goto done;
    }
    fill_scalars3(&s, &inner, nums, tags, inners);
    fill_flags(&s, flags);

    /* Dropped row (field 1 missing): encode and decode both fail. */
    memcpy(fields, structtest_scalars3_fields, sizeof(fields));
    memmove(&fields[0], &fields[1], sizeof(fields) - sizeof(fields[0]));
    CHECK_STATUS(PBW_ERR_INVALID,
                 pbw_struct_encode(schema, "structtest.Scalars3", fields, 21,
                                   &s, &wire, &wire_len));
    CHECK(wire == NULL);
    /* Direct calls bypass the wrapper's memset; zero base by hand. */
    memset(&got, 0, sizeof(got));
    CHECK_STATUS(PBW_ERR_INVALID,
                 pbw_struct_decode(schema, "structtest.Scalars3", fields, 21,
                                   bin, bin_len, &got));

    /* Wrong kind (int32 field read as string). */
    memcpy(fields, structtest_scalars3_fields, sizeof(fields));
    fields[0].kind = PBW_SF_STRING;
    CHECK_STATUS(PBW_ERR_INVALID,
                 pbw_struct_encode(schema, "structtest.Scalars3", fields, 22,
                                   &s, &wire, &wire_len));
    CHECK(wire == NULL);

    /* Repetition flipped (repeated field marked singular). */
    memcpy(fields, structtest_scalars3_fields, sizeof(fields));
    fields[16].repeated = 0; /* nums */
    CHECK_STATUS(PBW_ERR_INVALID,
                 pbw_struct_encode(schema, "structtest.Scalars3", fields, 22,
                                   &s, &wire, &wire_len));
    CHECK(wire == NULL);
done:
    pbw_free(wire);
    free(bin);
    pbw_schema_free(schema);
}

/* Boundary argument checks shared by every entry point. */
static void
test_bad_arguments(void)
{
    structtest_scalars3_t s;
    structtest_scalars3_t got;
    pbw_schema *schema = NULL;
    uint8_t *wire = NULL;
    size_t wire_len = 0;

    memset(&s, 0, sizeof(s));
    CHECK_OK(pbw_struct_schema_load(MATRIX3_DESC_PATH, &schema));
    if (schema == NULL) {
        return;
    }
    CHECK_STATUS(PBW_ERR_INVALID,
                 pbw_struct_encode(NULL, "structtest.Scalars3",
                                   structtest_scalars3_fields, 22, &s, &wire,
                                   &wire_len));
    CHECK_STATUS(PBW_ERR_INVALID,
                 pbw_struct_encode(schema, "structtest.Nope",
                                   structtest_scalars3_fields, 22, &s, &wire,
                                   &wire_len));
    CHECK_STATUS(PBW_ERR_INVALID,
                 pbw_struct_encode(schema, "structtest.Scalars3",
                                   structtest_scalars3_fields, 22, NULL, &wire,
                                   &wire_len));
    CHECK_STATUS(PBW_ERR_INVALID,
                 pbw_struct_encode(schema, "structtest.Scalars3",
                                   structtest_scalars3_fields, 22, &s, NULL,
                                   &wire_len));
    CHECK_STATUS(PBW_ERR_INVALID,
                 pbw_struct_decode(schema, "structtest.Nope",
                                   structtest_scalars3_fields, 22, NULL, 0,
                                   &got));
    CHECK_STATUS(PBW_ERR_INVALID,
                 pbw_struct_decode(schema, "structtest.Scalars3",
                                   structtest_scalars3_fields, 22, NULL, 0,
                                   NULL));
    /* Freeing is safe on zeroed structs and NULL. */
    memset(&got, 0, sizeof(got));
    structtest_scalars3_free(&got);
    pbw_struct_free(structtest_scalars3_fields, 21, NULL);
    CHECK(wire == NULL);
    pbw_schema_free(schema);
}

/* Schema loading reports file problems instead of crashing. */
static void
test_schema_load_errors(void)
{
    pbw_schema *schema = (pbw_schema *)0x1; /* must be reset on failure */
    FILE *f;

    CHECK_STATUS(PBW_ERR_INVALID,
                 pbw_struct_schema_load("bindings/c/tests/data/nope.desc",
                                        &schema));
    CHECK(schema == NULL);
    schema = (pbw_schema *)0x1;
    CHECK_STATUS(PBW_ERR_INVALID, pbw_struct_schema_load(NULL, &schema));
    CHECK(schema == NULL);
    CHECK_STATUS(PBW_ERR_INVALID,
                 pbw_struct_schema_load(MATRIX3_DESC_PATH, NULL));
    f = fopen("build/corrupt.desc", "wb");
    CHECK(f != NULL);
    if (f != NULL) {
        static const uint8_t garbage[] = {0xde, 0xad, 0xbe, 0xef};
        CHECK(fwrite(garbage, 1, sizeof(garbage), f) == sizeof(garbage));
        fclose(f);
        /* Garbage propagates descriptor_set_load's contract: a status
         * plus a schema carrying the reason, freed on both paths. */
        schema = (pbw_schema *)0x1;
        if (pbw_struct_schema_load("build/corrupt.desc", &schema) == PBW_OK) {
            CHECK(0); /* garbage must not load */
        }
        CHECK(schema != (pbw_schema *)0x1);
        pbw_schema_free(schema);
        remove("build/corrupt.desc");
    }
}

/* T3: a zeroed struct round-trips to zero and re-encodes identically.
 * Adaptation pin: proto3 implicit-presence scalars have no has-flag, so the
 * encoder still emits them as zeros (valid protobuf: parsers read explicit
 * defaults as zero, and libprotobuf parses these bytes back to the same
 * zeros). Only explicit-presence (optional), repeated, string and message
 * fields vanish when absent. */
static void
test_t3_empty_and_explicit_zero(void)
{
    structtest_scalars3_t zero;
    structtest_scalars3_t back;
    structtest_scalars3_t explicit_zero;
    structtest_scalars3_t back2;
    pbw_schema *schema = NULL;
    uint8_t *wire = NULL;
    uint8_t *wire2 = NULL;
    size_t wire_len = 0;
    size_t wire2_len = 0;

    CHECK_OK(pbw_struct_schema_load(MATRIX3_DESC_PATH, &schema));
    if (schema == NULL) {
        return;
    }
    memset(&zero, 0, sizeof(zero));
    CHECK_OK(structtest_scalars3_encode(schema, &zero, &wire, &wire_len));
    CHECK_OK(structtest_scalars3_decode(schema, wire, wire_len, &back));
    CHECK(back.i32 == 0 && back.i64 == 0 && back.u32 == 0 && back.u64 == 0);
    CHECK(back.s32 == 0 && back.s64 == 0 && back.fx32 == 0 && back.fx64 == 0);
    CHECK(back.sfx32 == 0 && back.sfx64 == 0 && back.f32 == 0.0f);
    CHECK(back.f64 == 0.0 && back.flag == 0 && back.str == NULL);
    CHECK(back.color == STRUCTTEST_COLOR3_COLOR3_UNSPECIFIED);
    CHECK(back.inner == NULL);
    CHECK(back.n_nums == 0 && back.n_tags == 0 && back.n_inners == 0);
    CHECK(back.has_opt_i32 == 0 && back.opt_i32 == 0);
    CHECK(back.opt_str == NULL && back.n_flags == 0);
    CHECK_OK(structtest_scalars3_encode(schema, &back, &wire2, &wire2_len));
    CHECK(wire2_len == wire_len && memcmp(wire2, wire, wire_len) == 0);
    structtest_scalars3_free(&back);
    pbw_free(wire2);
    wire2 = NULL;

    /* Explicit zero with presence: field 20 (tag 0xa0 0x01) carrying 0x00 is
     * the last row of the pinned layout, so it lands as a 3-byte suffix.
     * Absent opt_str stays absent: no field 21 tag anywhere after it. */
    memset(&explicit_zero, 0, sizeof(explicit_zero));
    explicit_zero.opt_i32 = 0;
    explicit_zero.has_opt_i32 = 1;
    CHECK_OK(structtest_scalars3_encode(schema, &explicit_zero, &wire2,
                                        &wire2_len));
    CHECK(wire2_len == wire_len + 3);
    if (wire2_len == wire_len + 3) {
        CHECK(memcmp(wire2, wire, wire_len) == 0);
        CHECK(wire2[wire_len] == 0xa0 && wire2[wire_len + 1] == 0x01 &&
              wire2[wire_len + 2] == 0x00);
    }
    CHECK_OK(structtest_scalars3_decode(schema, wire2, wire2_len, &back2));
    CHECK(back2.has_opt_i32 == 1 && back2.opt_i32 == 0);
    CHECK(back2.opt_str == NULL && back2.n_flags == 0);
    structtest_scalars3_free(&back2);
    pbw_free(wire2);
    pbw_free(wire);
    pbw_schema_free(schema);
}

/* T4 in-gate: the same matrix3.bin through the dynamic tree API. Two
 * independent codecs over one descriptor must agree field by field, and the
 * dynamic tree must re-encode to the same bytes (the live libprotobuf half
 * of T4 is compat_oracle.cc, which also pins the oracle version). */
static void
test_t4_dynamic_crosscheck(void)
{
    pbw_schema *dyn = NULL;
    const pbw_descriptor *desc;
    pbw_message *msg = NULL;
    pbw_value *v;
    pbw_message *inner;
    uint8_t *desc_bytes = NULL;
    uint8_t *expect = NULL;
    uint8_t *wire = NULL;
    size_t desc_len = 0;
    size_t expect_len = 0;
    size_t wire_len = 0;
    size_t i;
    uint32_t color_bits;

    CHECK(read_file("bindings/c/tests/data/matrix3.desc", &desc_bytes,
                    &desc_len) == 0);
    CHECK(read_file("bindings/c/tests/data/matrix3.bin", &expect,
                    &expect_len) == 0);
    if (desc_bytes == NULL || expect == NULL) {
        goto done;
    }
    CHECK_OK(pbw_descriptor_set_load(desc_bytes, desc_len, &dyn));
    if (dyn == NULL) {
        goto done;
    }
    desc = pbw_schema_find(dyn, "structtest.Scalars3");
    CHECK(desc != NULL);
    if (desc == NULL) {
        goto done;
    }
    CHECK_OK(pbw_decode(desc, expect, expect_len, &msg));
    if (msg == NULL) {
        goto done;
    }
    v = pbw_message_get(msg, 1);
    CHECK(v != NULL && pbw_value_get_int32(v) == INT32_MIN);
    v = pbw_message_get(msg, 2);
    CHECK(v != NULL && pbw_value_get_int(v) == INT64_MAX);
    v = pbw_message_get(msg, 3);
    CHECK(v != NULL && pbw_value_get_uint32(v) == UINT32_MAX);
    v = pbw_message_get(msg, 4);
    CHECK(v != NULL && pbw_value_get_uint(v) == UINT64_MAX);
    v = pbw_message_get(msg, 5);
    CHECK(v != NULL && pbw_value_get_int32(v) == INT32_MIN);
    v = pbw_message_get(msg, 6);
    CHECK(v != NULL && pbw_value_get_int(v) == INT64_MIN);
    v = pbw_message_get(msg, 7);
    CHECK(v != NULL && pbw_value_get_uint32(v) == UINT32_C(0xdeadbeef));
    v = pbw_message_get(msg, 8);
    CHECK(v != NULL &&
          pbw_value_get_uint(v) == UINT64_C(72623859790382856));
    v = pbw_message_get(msg, 9);
    CHECK(v != NULL && pbw_value_get_int32(v) == INT32_MIN);
    v = pbw_message_get(msg, 10);
    CHECK(v != NULL && pbw_value_get_int(v) == INT64_MIN);
    v = pbw_message_get(msg, 11);
    CHECK(v != NULL);
    if (v != NULL) {
        float f32 = pbw_value_get_float(v);

        memcpy(&color_bits, &f32, sizeof(color_bits));
        CHECK(color_bits == UINT32_C(0x7f800000)); /* inf */
    }
    v = pbw_message_get(msg, 12);
    CHECK(v != NULL && pbw_value_get_double(v) == -123456.789);
    v = pbw_message_get(msg, 13);
    CHECK(v != NULL && pbw_value_get_bool(v) != 0);
    v = pbw_message_get(msg, 14);
    CHECK(v != NULL && strcmp(pbw_value_get_string(v), "hello") == 0);
    v = pbw_message_get(msg, 15);
    CHECK(v != NULL && pbw_value_get_int32(v) == 2); /* GREEN */
    v = pbw_message_get(msg, 16);
    CHECK(v != NULL);
    inner = v != NULL ? pbw_value_get_message(v) : NULL;
    CHECK(inner != NULL);
    if (inner != NULL) {
        pbw_value *name = pbw_message_get(inner, 1);
        pbw_value *value = pbw_message_get(inner, 2);
        CHECK(name != NULL && strcmp(pbw_value_get_string(name), "kid") == 0);
        CHECK(value != NULL && pbw_value_get_int32(value) == 42);
    }
    v = pbw_message_get(msg, 17);
    CHECK(v != NULL && pbw_value_count(v) == 3);
    if (v != NULL && pbw_value_count(v) == 3) {
        CHECK(pbw_value_get_int32(pbw_value_at(v, 0)) == 1);
        CHECK(pbw_value_get_int32(pbw_value_at(v, 1)) == -5);
        CHECK(pbw_value_get_int32(pbw_value_at(v, 2)) == 300);
    }
    v = pbw_message_get(msg, 18);
    CHECK(v != NULL && pbw_value_count(v) == 2);
    if (v != NULL && pbw_value_count(v) == 2) {
        CHECK(strcmp(pbw_value_get_string(pbw_value_at(v, 0)), "a") == 0);
        CHECK(strcmp(pbw_value_get_string(pbw_value_at(v, 1)), "bb") == 0);
    }
    v = pbw_message_get(msg, 19);
    CHECK(v != NULL && pbw_value_count(v) == 2);
    if (v != NULL && pbw_value_count(v) == 2) {
        pbw_message *first = pbw_value_get_message(pbw_value_at(v, 0));
        pbw_message *second = pbw_value_get_message(pbw_value_at(v, 1));
        CHECK(first != NULL && second != NULL);
        if (first != NULL) {
            CHECK(strcmp(pbw_value_get_string(pbw_message_get(first, 1)),
                         "x") == 0);
        }
        if (second != NULL) {
            CHECK(pbw_value_get_int32(pbw_message_get(second, 2)) == -2);
        }
    }
    v = pbw_message_get(msg, 20);
    CHECK(v != NULL && pbw_value_get_int32(v) == -7);
    v = pbw_message_get(msg, 21);
    CHECK(v != NULL && strcmp(pbw_value_get_string(v), "opt") == 0);
    v = pbw_message_get(msg, 22);
    CHECK(v != NULL && pbw_value_count(v) == 3);
    if (v != NULL && pbw_value_count(v) == 3) {
        CHECK(pbw_value_get_bool(pbw_value_at(v, 0)) != 0);
        CHECK(pbw_value_get_bool(pbw_value_at(v, 1)) == 0);
        CHECK(pbw_value_get_bool(pbw_value_at(v, 2)) != 0);
    }
    /* Same tree back to bytes: byte equality with the protoc reference. */
    CHECK_OK(pbw_encode(msg, &wire, &wire_len));
    CHECK(wire_len == expect_len && memcmp(wire, expect, expect_len) == 0);
    for (i = 0; i < expect_len; i++) {
        if (wire_len != expect_len || wire[i] != expect[i]) {
            break;
        }
    }
done:
    pbw_free(wire);
    pbw_message_free(msg);
    free(expect);
    free(desc_bytes);
    pbw_schema_free(dyn);
}

/* T5: one workflow (the vectors.json business values) through the one-shot
 * calls and through the compiled codec: identical bytes, identical fields.
 * The cross-library half (same steps against libprotobuf generated code) is
 * compat_oracle.cc, where the snippet is shared, not duplicated. */
static void
test_t5_entry_point_parity(void)
{
    structtest_scalars3_t s;
    structtest_inner3_t inner;
    int32_t nums[3];
    const char *tags[2];
    structtest_inner3_t inners[2];
    bool flags[3];
    structtest_scalars3_t via_calls;
    structtest_scalars3_t via_codec;
    pbw_schema *schema = NULL;
    pbw_struct_codec *codec = NULL;
    pbw_struct_buf buf = {NULL, 0, 0};
    uint8_t *wire = NULL;
    size_t wire_len = 0;

    CHECK_OK(pbw_struct_schema_load(MATRIX3_DESC_PATH, &schema));
    if (schema == NULL) {
        return;
    }
    fill_scalars3(&s, &inner, nums, tags, inners);
    fill_flags(&s, flags);
    CHECK_OK(pbw_struct_codec_init(&codec, schema, "structtest.Scalars3",
                                   structtest_scalars3_fields, 22));
    if (codec == NULL) {
        goto done;
    }
    /* Same literals, two entry points, one byte string. */
    CHECK_OK(structtest_scalars3_encode(schema, &s, &wire, &wire_len));
    CHECK_OK(pbw_struct_codec_encode(codec, &s, &buf));
    CHECK(buf.len == wire_len && memcmp(buf.data, wire, wire_len) == 0);
    /* Same bytes back through both decoders: the same observable struct. */
    CHECK_OK(structtest_scalars3_decode(schema, wire, wire_len, &via_calls));
    memset(&via_codec, 0, sizeof(via_codec));
    CHECK_OK(pbw_struct_codec_decode(codec, buf.data, buf.len, NULL,
                                     &via_codec));
    CHECK(via_calls.i32 == via_codec.i32 && via_calls.i64 == via_codec.i64);
    CHECK(via_calls.u32 == via_codec.u32 && via_calls.u64 == via_codec.u64);
    CHECK(float_bits(via_calls.f32) == float_bits(via_codec.f32));
    CHECK(via_calls.f64 == via_codec.f64);
    CHECK(via_calls.flag == via_codec.flag);
    CHECK(strcmp(via_calls.str, via_codec.str) == 0);
    CHECK(via_calls.color == via_codec.color);
    CHECK(strcmp(via_calls.inner->name, via_codec.inner->name) == 0);
    CHECK(via_calls.inner->value == via_codec.inner->value);
    CHECK(via_calls.n_nums == via_codec.n_nums);
    CHECK(via_calls.n_tags == via_codec.n_tags && via_calls.n_tags == 2);
    CHECK(via_calls.nums[2] == via_codec.nums[2]);
    CHECK(via_calls.n_inners == via_codec.n_inners);
    CHECK(via_calls.inners[1].value == via_codec.inners[1].value);
    CHECK(via_calls.has_opt_i32 == via_codec.has_opt_i32);
    CHECK(via_calls.opt_i32 == via_codec.opt_i32);
    CHECK(strcmp(via_calls.opt_str, via_codec.opt_str) == 0);
    CHECK(via_calls.n_flags == via_codec.n_flags);
    CHECK(via_calls.flags[0] == via_codec.flags[0]);
    structtest_scalars3_free(&via_calls);
    structtest_scalars3_free(&via_codec);
done:
    pbw_struct_codec_free(codec);
    pbw_struct_buf_free(&buf);
    pbw_free(wire);
    pbw_schema_free(schema);
}

/* T6, wire level: truncated and overlong varints fail loudly, and an unknown
 * open-enum value is kept (proto3 enums are open; the reference parses 99
 * into the field too, so the codec stores it and re-emits it stably). */
static void
test_t6_wire_errors(void)
{
    structtest_scalars3_t got;
    pbw_schema *schema = NULL;
    uint8_t *wire = NULL;
    size_t wire_len = 0;
    /* Field 1 tag with the payload cut off. */
    static const uint8_t truncated[] = {0x08};
    /* Field 1 tag plus 11 continuation bytes: no varint is that long. */
    static const uint8_t overlong[] = {0x08, 0xff, 0xff, 0xff, 0xff, 0xff,
                                       0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    /* Field 15 (color) carrying unknown value 99. */
    static const uint8_t unknown_enum[] = {0x78, 0x63};

    CHECK_OK(pbw_struct_schema_load(MATRIX3_DESC_PATH, &schema));
    if (schema == NULL) {
        return;
    }
    memset(&got, 0, sizeof(got));
    CHECK(pbw_struct_decode(schema, "structtest.Scalars3",
                            structtest_scalars3_fields, 22, truncated,
                            sizeof(truncated), &got) != PBW_OK);
    memset(&got, 0, sizeof(got));
    CHECK(pbw_struct_decode(schema, "structtest.Scalars3",
                            structtest_scalars3_fields, 22, overlong,
                            sizeof(overlong), &got) != PBW_OK);
    CHECK_OK(structtest_scalars3_decode(schema, unknown_enum,
                                        sizeof(unknown_enum), &got));
    CHECK(got.color == 99);
    CHECK(got.i32 == 0 && got.str == NULL && got.has_opt_i32 == 0);
    /* Re-encode keeps the unknown value: every other implicit scalar rides
     * along as zero, so find field 15's tag/value pair in the bytes. */
    CHECK_OK(structtest_scalars3_encode(schema, &got, &wire, &wire_len));
    {
        size_t at = 0;
        int found = 0;
        size_t k;

        for (k = 0; k + 1 < wire_len; k++) {
            if (wire[k] == 0x78 && wire[k + 1] == 0x63) {
                at = k;
                found = 1;
            }
        }
        CHECK(found != 0);
        if (found) {
            structtest_scalars3_t again;

            CHECK_OK(structtest_scalars3_decode(schema, wire, wire_len,
                                                &again));
            CHECK(again.color == 99);
            CHECK(again.i32 == 0 && again.has_opt_i32 == 0);
            CHECK(at + 2 <= wire_len);
            structtest_scalars3_free(&again);
        }
    }
    structtest_scalars3_free(&got);
    pbw_free(wire);
    pbw_schema_free(schema);
}

int
main(void)
{
    test_matrix3_reference();
    test_codec();
    test_matrix2_reference();
    test_addressbook_reference();
    test_absent_and_empty();
    test_unknown_fields_tolerated();
    test_wrong_wire_type_tolerated();
    test_missing_required();
    test_truncated_and_garbage();
    test_layout_mismatch();
    test_bad_arguments();
    test_schema_load_errors();
    test_t3_empty_and_explicit_zero();
    test_t4_dynamic_crosscheck();
    test_t5_entry_point_parity();
    test_t6_wire_errors();
    if (failures != 0) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("test_struct: all checks passed\n");
    return 0;
}

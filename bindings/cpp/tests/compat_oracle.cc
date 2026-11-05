/* Typed-compat oracle differential for the C/C++ struct codec (TESTSPEC T4
 * live half, plus the shared-snippet T5 and the oracle pin).
 *
 * The struct codec IS the typed model for C and C++, so T1-T3 and the
 * dependency-free halves of T5-T6 live in bindings/c/tests/test_struct.c
 * and bindings/cpp/tests/roundtrip.cc. This file covers what those cannot:
 * the same business values through the official C++ runtime (protoc
 * --cpp_out from the same matrix3.proto), byte against byte.
 *
 * Needs protoc + libprotobuf, so it stays out of `make test` and
 * `make test-cpp` (same status as `make bench-c`); run from the repo root
 * so MATRIX3_DESC_PATH resolves. Wire it with the compat-oracle lines from
 * the task summary (own protoc rule + PB_LIBS link, bench-c pattern).
 *
 * Oracle pin: protoc 36.2 (libprotoc 36.2, which stamped matrix3.bin) with
 * libprotobuf 36.2.0 (GOOGLE_PROTOBUF_VERSION 7036002). The #if below fails
 * the build instead of silently testing a drifted oracle.
 */
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include <google/protobuf/stubs/common.h>

#define PBW_ORACLE_PIN 7036002 /* libprotobuf 36.2.0; see testdata/ORACLES.md */
#if GOOGLE_PROTOBUF_VERSION != PBW_ORACLE_PIN
#error "libprotobuf oracle drifted: update PBW_ORACLE_PIN and ORACLES.md"
#endif

#include "matrix3.pb.h"

#include "gen/matrix3.h"
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
    out.resize((size_t)size);
    size_t n = std::fread(out.data(), 1, (size_t)size, f);
    std::fclose(f);
    return n == (size_t)size;
}

/* The vectors.json business values as struct literals. */
static void
fill_struct(structtest_scalars3_t &s, structtest_inner3_t &inner,
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

/* The same values through the oracle's generated setters. */
static void
fill_oracle(structtest::Scalars3 &m)
{
    m.set_i32(INT32_MIN);
    m.set_i64(INT64_MAX);
    m.set_u32(UINT32_MAX);
    m.set_u64(UINT64_MAX);
    m.set_s32(INT32_MIN);
    m.set_s64(INT64_MIN);
    m.set_fx32(0xdeadbeefu);
    m.set_fx64(UINT64_C(72623859790382856));
    m.set_sfx32(INT32_MIN);
    m.set_sfx64(INT64_MIN);
    m.set_f32(std::numeric_limits<float>::infinity());
    m.set_f64(-123456.789);
    m.set_flag(true);
    m.set_str("hello");
    m.set_color(structtest::COLOR3_GREEN);
    m.mutable_inner()->set_name("kid");
    m.mutable_inner()->set_value(42);
    m.add_nums(1);
    m.add_nums(-5);
    m.add_nums(300);
    m.add_tags("a");
    m.add_tags("bb");
    structtest::Inner3 *x = m.add_inners();
    x->set_name("x");
    x->set_value(1);
    structtest::Inner3 *y = m.add_inners();
    y->set_name("y");
    y->set_value(-2);
    m.set_opt_i32(-7);
    m.set_opt_str("opt");
    m.add_flags(true);
    m.add_flags(false);
    m.add_flags(true);
}

/* T1+T4: both libraries encode the same values to the reference bytes, and
 * each library parses the other's bytes to the same fields. */
static void
test_differential(pbw_schema *schema, const std::vector<uint8_t> &reference)
{
    structtest_scalars3_t s = {};
    structtest_inner3_t inner = {};
    int32_t nums[3];
    const char *tags[2];
    structtest_inner3_t inners[2] = {};
    bool flags[3];
    structtest::Scalars3 gen;
    std::string want;
    uint8_t *got = nullptr;
    size_t got_len = 0;

    fill_struct(s, inner, nums, tags, inners, flags);
    fill_oracle(gen);
    CHECK(gen.SerializeToString(&want));
    CHECK(want.size() == reference.size() &&
          std::memcmp(want.data(), reference.data(), reference.size()) == 0);
    CHECK_OK(structtest_scalars3_encode(schema, &s, &got, &got_len));
    CHECK(got_len == reference.size() &&
          std::memcmp(got, reference.data(), reference.size()) == 0);

    /* Cross-decode: the oracle parses our bytes, we parse the oracle's. */
    {
        structtest::Scalars3 om;
        structtest_scalars3_t fm = {};

        CHECK(om.ParseFromArray(got, (int)got_len));
        CHECK_OK(structtest_scalars3_decode(schema,
                                            (const uint8_t *)want.data(),
                                            want.size(), &fm));
        CHECK(om.i32() == fm.i32 && om.i64() == fm.i64);
        CHECK(om.u32() == fm.u32 && om.u64() == fm.u64);
        CHECK(om.s32() == fm.s32 && om.s64() == fm.s64);
        CHECK(om.fx32() == fm.fx32 && om.fx64() == fm.fx64);
        CHECK(om.sfx32() == fm.sfx32 && om.sfx64() == fm.sfx64);
        CHECK(om.f32() == fm.f32 && om.f64() == fm.f64);
        CHECK(om.flag() == (fm.flag != 0));
        CHECK(om.str() == (fm.str != nullptr ? fm.str : ""));
        CHECK(om.color() == structtest::COLOR3_GREEN &&
              fm.color == STRUCTTEST_COLOR3_COLOR3_GREEN);
        CHECK(om.inner().name() ==
              (fm.inner != nullptr && fm.inner->name != nullptr
                   ? fm.inner->name
                   : ""));
        CHECK(om.inner().value() == (fm.inner != nullptr ? fm.inner->value : 0));
        CHECK(om.nums_size() == (int)fm.n_nums && om.tags_size() == 2 &&
              fm.n_tags == 2);
        CHECK(om.inners_size() == (int)fm.n_inners);
        CHECK(om.has_opt_i32() == (fm.has_opt_i32 != 0));
        CHECK(om.opt_i32() == fm.opt_i32);
        CHECK(om.opt_str() == (fm.opt_str != nullptr ? fm.opt_str : ""));
        CHECK(om.flags_size() == (int)fm.n_flags);
        structtest_scalars3_free(&fm);
    }
    pbw_free(got);
}

/* The comparable business result for T5. */
struct Summary {
    std::string wire;
    std::string str;
    int32_t i32;
    int32_t nums[2];
    size_t n_nums;
    std::string inner;
    bool has_opt;

    /* Fields only: wires differ by design (the struct codec emits
     * implicit-presence zeros, valid protobuf the oracle parses fine,
     * while the oracle omits them), so byte equality is asserted via
     * cross-parsing in test_switch, not here. */
    bool operator==(const Summary &o) const
    {
        return str == o.str && i32 == o.i32 && n_nums == o.n_nums &&
               nums[0] == o.nums[0] && nums[1] == o.nums[1] &&
               inner == o.inner && has_opt == o.has_opt;
    }
};

struct PbAdapter {
    using Msg = structtest::Scalars3;

    static void set(Msg &m)
    {
        m.set_str("kid");
        m.set_i32(-5);
        m.add_nums(1);
        m.add_nums(2);
        m.mutable_inner()->set_name("x");
        m.mutable_inner()->set_value(1);
    }
    static std::string encode(Msg &m)
    {
        std::string w;

        CHECK(m.SerializeToString(&w));
        return w;
    }
    static void decode(const std::string &w, Msg &m)
    {
        CHECK(m.ParseFromString(w));
    }
    static void summarize(const Msg &m, Summary &s)
    {
        s.str = m.str();
        s.i32 = m.i32();
        s.n_nums = (size_t)m.nums_size();
        s.nums[0] = m.nums_size() > 0 ? m.nums(0) : 0;
        s.nums[1] = m.nums_size() > 1 ? m.nums(1) : 0;
        s.inner = m.has_inner() ? m.inner().name() : "";
        s.has_opt = m.has_opt_i32();
    }
    static void release(Msg &m)
    {
        (void)m;
    }
};

struct StructAdapter {
    using Msg = structtest_scalars3_t;

    pbw_schema *schema;
    structtest_inner3_t inner;
    int32_t nums[2];

    void set(Msg &m)
    {
        m.str = "kid";
        m.i32 = -5;
        nums[0] = 1;
        nums[1] = 2;
        m.nums = nums;
        m.n_nums = 2;
        inner = {};
        inner.name = "x";
        inner.value = 1;
        m.inner = &inner;
    }
    std::string encode(Msg &m)
    {
        uint8_t *w = nullptr;
        size_t n = 0;

        CHECK_OK(structtest_scalars3_encode(schema, &m, &w, &n));
        std::string s((const char *)w, n);
        pbw_free(w);
        return s;
    }
    void decode(const std::string &w, Msg &m)
    {
        CHECK_OK(structtest_scalars3_decode(schema, (const uint8_t *)w.data(),
                                            w.size(), &m));
    }
    static void summarize(const Msg &m, Summary &s)
    {
        s.str = m.str != nullptr ? m.str : "";
        s.i32 = m.i32;
        s.n_nums = m.n_nums;
        s.nums[0] = m.n_nums > 0 ? m.nums[0] : 0;
        s.nums[1] = m.n_nums > 1 ? m.nums[1] : 0;
        s.inner = m.inner != nullptr && m.inner->name != nullptr
                      ? m.inner->name
                      : "";
        s.has_opt = m.has_opt_i32 != 0;
    }
    static void release(Msg &m)
    {
        structtest_scalars3_free(&m);
    }
};

/* T5: one business-logic snippet, written once against the adapter, runs
 * unchanged against both libraries with identical observable results. */
template <typename A>
static Summary
run_workflow(A &a)
{
    typename A::Msg m = {};
    typename A::Msg back = {};
    Summary s = {};

    /* Only the decoded tree is owned: m aliases caller storage (the struct
     * adapter points at stack literals), so only back is released. */
    a.set(m);
    s.wire = a.encode(m);
    a.decode(s.wire, back);
    A::summarize(back, s);
    A::release(back);
    return s;
}

static void
test_switch(pbw_schema *schema)
{
    PbAdapter pb;
    StructAdapter st;
    PbAdapter::Msg cross_pb = {};
    StructAdapter::Msg cross_st = {};
    Summary cross_a = {};
    Summary cross_b = {};

    st.schema = schema;
    Summary a = run_workflow(pb);
    Summary b = run_workflow(st);
    CHECK(a == b);
    CHECK(a.str == "kid" && a.i32 == -5 && a.n_nums == 2 && a.nums[0] == 1 &&
          a.nums[1] == 2 && a.inner == "x" && a.has_opt == false);
    /* Each side parses the other's wire to the same fields. */
    PbAdapter::decode(b.wire, cross_pb);
    PbAdapter::summarize(cross_pb, cross_a);
    CHECK(cross_a == a);
    st.decode(a.wire, cross_st);
    StructAdapter::summarize(cross_st, cross_b);
    CHECK(cross_b == b);
    StructAdapter::release(cross_st);
}

/* T6: bad wire fails loudly on both sides; empty parses to the default. */
static void
test_errors(pbw_schema *schema)
{
    static const uint8_t bad[] = {0xff, 0xff, 0xff};
    structtest_scalars3_t got = {};
    structtest::Scalars3 om;
    structtest_scalars3_t empty = {};
    structtest::Scalars3 oe;

    CHECK(pbw_struct_decode(schema, "structtest.Scalars3",
                            structtest_scalars3_fields, 22, bad, sizeof(bad),
                            &got) != PBW_OK);
    CHECK(!om.ParseFromArray(bad, (int)sizeof(bad)));
    CHECK_OK(structtest_scalars3_decode(schema, nullptr, 0, &empty));
    CHECK(empty.i32 == 0 && empty.str == nullptr && empty.has_opt_i32 == 0);
    structtest_scalars3_free(&empty);
    CHECK(oe.ParseFromString(""));
    CHECK(oe.i32() == 0 && !oe.has_opt_i32());
}

int
main(void)
{
    pbw_schema *schema = nullptr;
    std::vector<uint8_t> reference;

    CHECK(read_reference("bindings/c/tests/data/matrix3.bin", reference));
    CHECK_OK(pbw_struct_schema_load(MATRIX3_DESC_PATH, &schema));
    if (reference.empty() || schema == nullptr) {
        pbw_schema_free(schema);
        return 1;
    }
    test_differential(schema, reference);
    test_switch(schema);
    test_errors(schema);
    pbw_schema_free(schema);
    if (failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("compat_oracle: all checks passed (libprotobuf %d)\n",
                GOOGLE_PROTOBUF_VERSION);
    return 0;
}

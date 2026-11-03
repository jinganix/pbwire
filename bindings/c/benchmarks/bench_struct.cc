/* C binding benchmark: generated structs (the compiled `pbw_struct_codec`
 * over the pinned `bindings/c/tests/gen/matrix3.h`) against the official C++
 * generated code (`protoc --cpp_out` from the same `matrix3.proto`) on the
 * same Scalars3 message.
 *
 * The four cases are the standard sizes from BENCHMARKS.md: small / medium /
 * large / xlarge scale the repeated fields x0 / x1 / x100 / x3000, so xlarge
 * lands in the ~80KB tier of the Erlang google_message2.dat case. Tables use
 * the same columns as bench/bench.cc (`impl op ns/op MB/s`, throughput over
 * wire bytes, higher is better), so the root BENCHMARKS.md can aggregate
 * them. Like `make bench` this needs protoc + libprotobuf and stays outside
 * `make test`; run from the repo root so MATRIX3_DESC_PATH resolves.
 */

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "matrix3.pb.h"
#include "pbwire.h"

#include "gen/matrix3.h"

namespace {

int failures = 0;

void
fail(const char *what)
{
    std::printf("FAIL %s\n", what);
    failures++;
}

using Clock = std::chrono::steady_clock;

double
ns_per_op(Clock::time_point start, int iterations)
{
    auto elapsed =
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start);
    return static_cast<double>(elapsed.count()) / iterations;
}

template <typename Fn>
double
time_it(Fn &&fn, int iterations, int warmup)
{
    for (int i = 0; i < warmup; i++)
        fn();
    auto start = Clock::now();
    for (int i = 0; i < iterations; i++)
        fn();
    return ns_per_op(start, iterations);
}

struct CaseData {
    structtest_scalars3_t s;
    structtest_inner3_t inner;
    int32_t *nums = nullptr;
    const char **tags = nullptr;
    structtest_inner3_t *inners = nullptr;
    bool *flags = nullptr;
};

/* Scalars mirror matrix3.txt (all non-zero: proto3 omits zeros on the
 * reference side while the struct encoder always emits them, so zeros would
 * compare different bytes for a reason unrelated to speed). Repeats scale
 * with n. */
void
fill_struct(CaseData &d, int n)
{
    structtest_scalars3_t &s = d.s;

    memset(&s, 0, sizeof(s));
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
    s.f32 = 1.0f / 0.0f; /* inf, matches `f32: inf` in matrix3.txt */
    s.f64 = -123456.789;
    s.flag = 1;
    s.str = "hello";
    s.color = STRUCTTEST_COLOR3_COLOR3_GREEN;
    d.inner.name = "kid";
    d.inner.value = 42;
    s.inner = &d.inner;
    if (n > 0) {
        d.nums = new int32_t[(size_t)n];
        d.tags = new const char *[(size_t)n];
        d.inners = new structtest_inner3_t[(size_t)n];
        d.flags = new bool[(size_t)n];
        for (int i = 0; i < n; i++) {
            d.nums[i] = i - 3;
            d.tags[i] = "tag";
            d.inners[i].name = "grandkid";
            d.inners[i].value = i + 1;
            d.flags[i] = (i % 2 == 0);
        }
    }
    s.nums = d.nums;
    s.n_nums = (size_t)n;
    s.tags = d.tags;
    s.n_tags = (size_t)n;
    s.inners = d.inners;
    s.n_inners = (size_t)n;
    s.opt_i32 = -7;
    s.has_opt_i32 = 1;
    s.opt_str = "opt";
    s.flags = d.flags;
    s.n_flags = (size_t)n;
}

void
free_struct(CaseData &d)
{
    delete[] d.nums;
    delete[] d.tags;
    delete[] d.inners;
    delete[] d.flags;
    d.nums = nullptr;
    d.tags = nullptr;
    d.inners = nullptr;
    d.flags = nullptr;
}

void
fill_gen(structtest::Scalars3 *m, int n)
{
    m->set_i32(INT32_MIN);
    m->set_i64(INT64_MAX);
    m->set_u32(UINT32_MAX);
    m->set_u64(UINT64_MAX);
    m->set_s32(INT32_MIN);
    m->set_s64(INT64_MIN);
    m->set_fx32(0xdeadbeefu);
    m->set_fx64(UINT64_C(72623859790382856));
    m->set_sfx32(INT32_MIN);
    m->set_sfx64(INT64_MIN);
    m->set_f32(HUGE_VALF); /* inf */
    m->set_f64(-123456.789);
    m->set_flag(true);
    m->set_str("hello");
    m->set_color(structtest::COLOR3_GREEN);
    m->mutable_inner()->set_name("kid");
    m->mutable_inner()->set_value(42);
    for (int i = 0; i < n; i++) {
        m->add_nums(i - 3);
        m->add_tags("tag");
        structtest::Inner3 *kid = m->add_inners();
        kid->set_name("grandkid");
        kid->set_value(i + 1);
        m->add_flags(i % 2 == 0);
    }
    m->set_opt_i32(-7);
    m->set_opt_str("opt");
}

/* Correctness first: a speed number from a wrong encoder is meaningless. */
void
check_parity(pbw_schema *schema, int n)
{
    CaseData d;
    structtest::Scalars3 gen;
    std::string want;
    uint8_t *got = nullptr;
    size_t got_len = 0;
    pbw_struct_codec *codec = nullptr;

    fill_struct(d, n);
    fill_gen(&gen, n);
    (void)gen.SerializeToString(&want);
    if (pbw_struct_codec_init(&codec, schema, "structtest.Scalars3",
                              structtest_scalars3_fields, 22) != PBW_OK) {
        fail("struct codec init");
        free_struct(d);
        return;
    }
    {
        pbw_struct_buf buf = {nullptr, 0, 0};

        if (pbw_struct_codec_encode(codec, &d.s, &buf) != PBW_OK) {
            fail("struct encode of Scalars3");
            pbw_struct_codec_free(codec);
            free_struct(d);
            return;
        }
        got = buf.data;
        got_len = buf.len;
    }
    if (got_len != want.size() || std::memcmp(got, want.data(), got_len) != 0) {
        char msg[128];
        std::snprintf(msg, sizeof msg,
                      "struct encoding parity (x%d): libprotobuf %zu bytes, "
                      "pbwire-c %zu bytes",
                      n, want.size(), got_len);
        fail(msg);
        for (size_t i = 0; i < want.size() && i < got_len; i++) {
            if (want[i] != (char)got[i]) {
                std::printf("  first diff at %zu: %02x vs %02x\n", i,
                            (unsigned char)want[i], got[i]);
                break;
            }
        }
    } else {
        std::printf("OK   struct encoding parity, x%d (%zu bytes)\n", n,
                    want.size());
    }
    {
        pbw_struct_buf encoded = {got, got_len, 0};

        pbw_struct_buf_free(&encoded);
    }

    /* Decode the reference bytes into an arena and re-encode: byte identical.
     * Reusing one arena and one buffer is the steady state a hot loop runs. */
    {
        structtest_scalars3_t out;
        pbw_struct_arena *arena = nullptr;
        pbw_struct_buf back = {nullptr, 0, 0};

        std::memset(&out, 0, sizeof(out));
        if (pbw_struct_codec_decode(
                codec, reinterpret_cast<const uint8_t *>(want.data()),
                want.size(), &arena, &out) != PBW_OK) {
            fail("struct decode of libprotobuf output");
        } else if (pbw_struct_codec_encode(codec, &out, &back) != PBW_OK ||
                   back.len != want.size() ||
                   std::memcmp(back.data, want.data(), back.len) != 0) {
            fail("struct decode->encode is not byte identical");
        } else {
            std::printf("OK   struct decode(libprotobuf) -> encode identical, "
                        "x%d\n",
                        n);
        }
        pbw_struct_buf_free(&back);
        pbw_struct_arena_free(arena);
    }
    pbw_struct_codec_free(codec);
    free_struct(d);
}

struct Row {
    const char *impl;
    const char *op;
    double ns;
    size_t bytes;
};

void
print_rows(const std::vector<Row> &rows)
{
    std::printf("  %-9s %-7s %10s %10s\n", "impl", "op", "ns/op", "MB/s");
    for (const Row &r : rows) {
        double mbps = r.ns > 0 ? (static_cast<double>(r.bytes) / r.ns) * 1e3 : 0;
        std::printf("  %-9s %-7s %10.1f %10.1f\n", r.impl, r.op, r.ns, mbps);
    }
}

void
bench_case(pbw_schema *schema, const char *label, int n, int iterations)
{
    CaseData d;
    structtest::Scalars3 gen_msg;
    std::string ref;
    std::string scratch;
    std::vector<Row> rows;
    pbw_struct_codec *codec = nullptr;
    pbw_struct_buf out = {nullptr, 0, 0};
    pbw_struct_arena *arena = nullptr;
    structtest_scalars3_t decoded;
    int warmup = iterations / 10;

    fill_struct(d, n);
    fill_gen(&gen_msg, n);
    (void)gen_msg.SerializeToString(&ref);
    if (pbw_struct_codec_init(&codec, schema, "structtest.Scalars3",
                              structtest_scalars3_fields, 22) != PBW_OK) {
        fail("struct codec init");
        free_struct(d);
        return;
    }

    /* Steady state: one compiled codec, one output buffer, one decode arena,
     * so neither path allocates per message (the generated side reuses its
     * std::string the same way). */
    rows.push_back({"pbwire-c", "encode", time_it([&] {
                        (void)pbw_struct_codec_encode(codec, &d.s, &out);
                    },
                                                  iterations, warmup),
                    ref.size()});
    rows.push_back({"pb gen", "encode", time_it([&] {
                        scratch.clear();
                        (void)gen_msg.SerializeToString(&scratch);
                    },
                                                iterations, warmup),
                    ref.size()});
    rows.push_back({"pbwire-c", "decode", time_it([&] {
                        std::memset(&decoded, 0, sizeof(decoded));
                        if (pbw_struct_codec_decode(
                                codec,
                                reinterpret_cast<const uint8_t *>(ref.data()),
                                ref.size(), &arena, &decoded) != PBW_OK)
                            failures++;
                    },
                                                  iterations, warmup),
                    ref.size()});
    rows.push_back({"pb gen", "decode", time_it([&] {
                        structtest::Scalars3 m;
                        if (!m.ParseFromArray(ref.data(),
                                              static_cast<int>(ref.size())))
                            failures++;
                    },
                                                iterations, warmup),
                    ref.size()});

    std::printf("\n%s: %zu bytes, %d iterations (repeat field x%d)\n", label,
                ref.size(), iterations, n);
    print_rows(rows);

    /* The one-shot API compiles the layout on every call; kept visible so a
     * regression there is not mistaken for a codec regression. */
    rows.clear();
    rows.push_back({"pbwire-c1", "encode", time_it([&] {
                        uint8_t *buf = nullptr;
                        size_t len = 0;
                        (void)structtest_scalars3_encode(schema, &d.s, &buf,
                                                         &len);
                        pbw_free(buf);
                    },
                                                   iterations, warmup),
                    ref.size()});
    rows.push_back({"pbwire-c1", "decode", time_it([&] {
                        structtest_scalars3_t m;
                        if (structtest_scalars3_decode(
                                schema,
                                reinterpret_cast<const uint8_t *>(ref.data()),
                                ref.size(), &m) != PBW_OK)
                            failures++;
                        structtest_scalars3_free(&m);
                    },
                                                   iterations, warmup),
                    ref.size()});
    std::printf("  one-shot API (per-call layout compile + per-message alloc):\n");
    print_rows(rows);

    pbw_struct_buf_free(&out);
    pbw_struct_arena_free(arena);
    pbw_struct_codec_free(codec);
    free_struct(d);
}

} // namespace

int
main(int argc, char **argv)
{
    /* Same CLI as bench/bench.cc: `bench [iterations]`. */
    bool run_bench = argc > 1 && std::strcmp(argv[1], "bench") == 0;
    int iterations = argc > 2 ? std::atoi(argv[2]) : 200000;

    if (!run_bench) {
        std::printf("usage: %s bench [iterations]\n", argv[0]);
        return 2;
    }
    pbw_schema *schema = nullptr;
    const int repeats[] = {0, 1, 100, 3000};
    const char *labels[] = {"small", "medium", "large", "xlarge"};

    if (pbw_struct_schema_load(MATRIX3_DESC_PATH, &schema) != PBW_OK) {
        std::printf("FAIL schema load: %s\n", pbw_schema_error(schema));
        pbw_schema_free(schema);
        return 2;
    }
    for (int n : repeats) {
        if (n <= 1)
            check_parity(schema, n);
    }
    for (size_t i = 0; i < 4; i++) {
        int iters = iterations;
        if (repeats[i] >= 3000) {
            iters = iterations / 100;
            if (iters < 200)
                iters = 200;
        }
        bench_case(schema, labels[i], repeats[i], iters);
    }
    pbw_schema_free(schema);
    return failures ? 1 : 0;
}

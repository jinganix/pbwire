/* C++ binding concurrency check: 8 threads x 200 iters, three phases per
 * worker mirroring the Go binding shape (and test_concurrency.c):
 *
 *   (a) independent-message encode == want bytes,
 *   (b) SHARED read-only message concurrent encode == want bytes,
 *   (c) fresh-target decode of want with field checks (NOT re-encode).
 *
 * The schema and the want bytes are the only cross-thread state, both
 * read-only after startup. Every message a worker encodes/decodes into is
 * thread-owned. Runs from the repo root so the relative fixture paths
 * resolve. No test framework, no dependencies beyond std::thread.
 *
 *     make test-cpp
 */
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <thread>
#include <vector>

/* Relative so the test needs no new -I: the Makefile rule for this binary
 * only carries the runtime and the examples include paths. */
#include "../../c/tests/gen/matrix3.h"

#include "pbwire.h"

using Scalars3 = structtest_scalars3_t;

namespace {

const int kThreads = 8;
const int kIters = 200;
const int kScaleIters = 20000;

pbw_schema *g_schema = nullptr; /* read-only after load, shared */
std::vector<uint8_t> g_want;    /* matrix3.bin oracle, read-only, shared */

/* Backing for the shared read-only fixture: filled once before threads
 * start, never mutated after. */
structtest_inner3_t g_inner = {};
int32_t g_nums[3] = {};
const char *g_tags[2] = {};
structtest_inner3_t g_inners[2] = {};
bool g_flags[3] = {};
Scalars3 g_shared = {};

unsigned g_failures = 0;
unsigned long g_checks = 0;
std::mutex g_mu;

struct Worker {
    int id = 0;
    unsigned failures = 0;
    unsigned long checks = 0;
};

void
fail(Worker &w, const char *phase)
{
    std::fprintf(stderr, "worker %d: %s\n", w.id, phase);
    w.failures++;
}

/* The populated matrix3.txt values (same as roundtrip.cc: the oracle bytes
 * only match this exact content). Writes into caller-provided backing so
 * each thread owns its memory. */
void
fill_into(Scalars3 &s, structtest_inner3_t &inner, int32_t *nums,
          const char **tags, structtest_inner3_t *inners, bool *flags)
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

/* Identical oracle content for all workers; `id` is naming-only. Backing is
 * thread_local, so each thread owns its message -- but the returned Scalars3
 * only borrows that backing (its pointer fields alias the thread_local
 * arrays). The caller must let encode consume it synchronously before the
 * next make_fixture call on the same thread overwrites the backing. Holding
 * two fixtures live simultaneously on one thread would alias -- do not do
 * that. */
Scalars3
make_fixture(int id)
{
    (void)id;
    thread_local structtest_inner3_t inner = {};
    thread_local int32_t nums[3] = {};
    thread_local const char *tags[2] = {};
    thread_local structtest_inner3_t inners[2] = {};
    thread_local bool flags[3] = {};
    thread_local Scalars3 s = {};

    fill_into(s, inner, nums, tags, inners, flags);
    return s;
}

bool
bytes_equal(const uint8_t *wire, size_t wire_len)
{
    return wire_len == g_want.size() &&
           std::memcmp(wire, g_want.data(), g_want.size()) == 0;
}

/* (a) independent-message encode == want bytes. */
void
phase_encode_own(Worker &w)
{
    Scalars3 m = make_fixture(w.id);
    uint8_t *wire = nullptr;
    size_t wire_len = 0;

    if (structtest_scalars3_encode(g_schema, &m, &wire, &wire_len) != PBW_OK) {
        fail(w, "phase (a) encode");
        return;
    }
    w.checks++;
    if (!bytes_equal(wire, wire_len)) {
        fail(w, "phase (a) encode bytes");
    }
    pbw_free(wire);
}

/* (b) SHARED read-only message concurrent encode == want bytes. */
void
phase_encode_shared(Worker &w)
{
    uint8_t *wire = nullptr;
    size_t wire_len = 0;

    if (structtest_scalars3_encode(g_schema, &g_shared, &wire, &wire_len) !=
        PBW_OK) {
        fail(w, "phase (b) encode shared");
        return;
    }
    w.checks++;
    if (!bytes_equal(wire, wire_len)) {
        fail(w, "phase (b) encode shared bytes");
    }
    pbw_free(wire);
}

/* (c) fresh-target decode with field checks (i32/str/nested/repeated/opt
 * presence). NOT re-encode. */
void
phase_decode(Worker &w)
{
    Scalars3 got = {};

    if (structtest_scalars3_decode(g_schema, g_want.data(), g_want.size(),
                                   &got) != PBW_OK) {
        fail(w, "phase (c) decode");
        return;
    }
    w.checks++;
    if (got.i32 != INT32_MIN || got.str == nullptr ||
        std::strcmp(got.str, "hello") != 0 ||
        got.color != STRUCTTEST_COLOR3_COLOR3_GREEN || got.inner == nullptr ||
        std::strcmp(got.inner->name, "kid") != 0 || got.inner->value != 42 ||
        got.n_nums != 3 || got.nums[0] != 1 || got.nums[1] != -5 ||
        got.nums[2] != 300 || got.n_tags != 2 || got.n_inners != 2 ||
        got.has_opt_i32 != 1 || got.opt_i32 != -7 || got.opt_str == nullptr ||
        std::strcmp(got.opt_str, "opt") != 0 || got.n_flags != 3) {
        fail(w, "phase (c) decode fields");
    }
    structtest_scalars3_free(&got);
}

void
run_worker(Worker &w)
{
    for (int i = 0; i < kIters; i++) {
        phase_encode_own(w);
    }
    for (int i = 0; i < kIters; i++) {
        phase_encode_shared(w);
    }
    for (int i = 0; i < kIters; i++) {
        phase_decode(w);
    }
    std::lock_guard<std::mutex> lock(g_mu);
    g_failures += w.failures;
    g_checks += w.checks;
}

/* One encode of the shared read-only message; shared by the serial and
 * parallel timing loops. Returns false on mismatch. */
bool
scale_encode_once()
{
    uint8_t *wire = nullptr;
    size_t wire_len = 0;
    bool ok = structtest_scalars3_encode(g_schema, &g_shared, &wire,
                                         &wire_len) == PBW_OK &&
              bytes_equal(wire, wire_len);

    pbw_free(wire);
    return ok;
}

bool
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

}  // namespace

int
main()
{
    if (!read_reference("bindings/c/tests/data/matrix3.bin", g_want) ||
        g_want.empty()) {
        std::fprintf(stderr, "concurrency: cannot read matrix3.bin\n");
        return 2;
    }
    if (pbw_struct_schema_load(MATRIX3_DESC_PATH, &g_schema) != PBW_OK ||
        g_schema == nullptr) {
        std::fprintf(stderr, "concurrency: cannot load matrix3.desc\n");
        return 2;
    }

    /* The serial want must equal matrix3.bin: encode once up front and
     * assert that before any thread starts. */
    {
        Scalars3 m = make_fixture(0);
        uint8_t *wire = nullptr;
        size_t wire_len = 0;

        if (structtest_scalars3_encode(g_schema, &m, &wire, &wire_len) !=
                PBW_OK ||
            !bytes_equal(wire, wire_len)) {
            std::fprintf(stderr,
                         "concurrency: serial encode != matrix3.bin\n");
            pbw_free(wire);
            pbw_schema_free(g_schema);
            return 2;
        }
        pbw_free(wire);
    }

    fill_into(g_shared, g_inner, g_nums, g_tags, g_inners, g_flags);

    std::vector<std::thread> threads;
    std::vector<Worker> workers((size_t)kThreads);
    for (int i = 0; i < kThreads; i++) {
        workers[(size_t)i].id = i;
    }
    for (int i = 0; i < kThreads; i++) {
        threads.emplace_back(run_worker, std::ref(workers[(size_t)i]));
    }
    for (auto &t : threads) {
        t.join();
    }
    std::printf("%d threads, %lu verifications, %u failures\n", kThreads,
                g_checks, g_failures);

    /* Serial encode loop vs parallel: informational only, never asserted. */
    {
        auto t0 = std::chrono::steady_clock::now();
        bool ok = true;
        for (int i = 0; i < kScaleIters; i++) {
            if (!scale_encode_once()) {
                ok = false;
                break;
            }
        }
        auto t1 = std::chrono::steady_clock::now();
        std::chrono::duration<double> single = t1 - t0;

        t0 = std::chrono::steady_clock::now();
        std::vector<std::thread> st;
        for (int t = 0; t < kThreads; t++) {
            st.emplace_back([&ok]() {
                for (int i = 0; i < kScaleIters / kThreads; i++) {
                    if (!scale_encode_once()) {
                        ok = false;
                        break;
                    }
                }
            });
        }
        for (auto &t : st) {
            t.join();
        }
        t1 = std::chrono::steady_clock::now();
        std::chrono::duration<double> multi = t1 - t0;

        double ns_single = single.count() * 1e9 / kScaleIters;
        double ns_multi =
            multi.count() * 1e9 / (kScaleIters / kThreads) / kThreads;
        std::printf("scale encode 1 thread: %8.1f MB/s (%6.1f ns/op) | "
                    "8 threads: %8.1f MB/s aggregate (%6.1f ns/op)\n",
                    g_want.size() / ns_single * 1e3, ns_single,
                    g_want.size() / ns_multi * 1e3, ns_multi);
        if (!ok) {
            std::fprintf(stderr, "concurrency: scale encode mismatch\n");
            pbw_schema_free(g_schema);
            return 1;
        }
    }

    pbw_schema_free(g_schema);
    if (g_failures != 0) {
        std::fprintf(stderr, "test_cpp_concurrency: %u failure(s)\n",
                     g_failures);
        return 1;
    }
    std::printf("test_cpp_concurrency: all checks passed\n");
    return 0;
}

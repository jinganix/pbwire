/* Struct-codec concurrency check: 8 threads x 200 iters, shared read-only
 * schema state, per-thread messages; encode must equal serial want bytes,
 * decode must keep fields. Runs from the repo root so the relative fixture
 * paths resolve. No test framework, no dependencies beyond pthreads.
 *
 * Later tasks copy this shape per language, not this code.
 */

#define _POSIX_C_SOURCE 200809L

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "pbwire.h"
#include "pbw_struct.h"

#include "gen/matrix3.h"

#define THREADS 8
#define ITERATIONS 200
#define SCALE_ITERATIONS 20000

static pbw_schema *shared_schema; /* read-only after load, shared */
static uint8_t *want;             /* matrix3.bin oracle, read-only, shared */
static size_t want_len;

static unsigned worker_failures;
static unsigned long worker_checks;
static pthread_mutex_t worker_mu = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
    unsigned id;
    unsigned failures;
    unsigned long checks;
} worker;

static void
fail(worker *w, const char *phase)
{
    fprintf(stderr, "worker %u: %s\n", w->id, phase);
    w->failures++;
}

/* The populated matrix3.txt values, mirrored as C structs (same as
 * test_struct.c: the oracle bytes only match this exact content). */
static void
fill_msg(structtest_scalars3_t *s, structtest_inner3_t *inner, int32_t *nums,
         const char **tags, structtest_inner3_t *inners, bool *flags)
{
    uint32_t infbits = 0x7f800000u;
    float inf;

    memcpy(&inf, &infbits, sizeof(inf));
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
    s->f32 = inf; /* matches `f32: inf` in matrix3.txt */
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
    flags[0] = 1;
    flags[1] = 0;
    flags[2] = 1;
    s->flags = flags;
    s->n_flags = 3;
}

/* One encode+decode round trip on thread-owned memory. The schema and the
 * want bytes are the only shared state, both read-only. */
static void
roundtrip(worker *w)
{
    structtest_scalars3_t s;
    structtest_inner3_t inner;
    int32_t nums[3];
    const char *tags[2];
    structtest_inner3_t inners[2];
    bool flags[3];
    structtest_scalars3_t got;
    uint8_t *wire = NULL;
    size_t wire_len = 0;

    fill_msg(&s, &inner, nums, tags, inners, flags);
    if (structtest_scalars3_encode(shared_schema, &s, &wire, &wire_len) !=
        PBW_OK) {
        fail(w, "encode");
        return;
    }
    w->checks++;
    if (wire_len != want_len || memcmp(wire, want, want_len) != 0) {
        fail(w, "encode bytes");
        pbw_free(wire);
        return;
    }
    if (structtest_scalars3_decode(shared_schema, wire, wire_len, &got) !=
        PBW_OK) {
        fail(w, "decode");
        structtest_scalars3_free(&got);
        pbw_free(wire);
        return;
    }
    w->checks++;
    if (got.i32 != INT32_MIN || got.u64 != UINT64_MAX ||
        got.str == NULL || strcmp(got.str, "hello") != 0 ||
        got.color != STRUCTTEST_COLOR3_COLOR3_GREEN || got.inner == NULL ||
        got.n_nums != 3 || got.n_tags != 2 || got.n_inners != 2 ||
        got.has_opt_i32 != 1 || got.opt_i32 != -7 || got.opt_str == NULL ||
        strcmp(got.opt_str, "opt") != 0 || got.n_flags != 3) {
        fail(w, "decode fields");
    }
    structtest_scalars3_free(&got);
    pbw_free(wire);
}

static void *
run_worker(void *arg)
{
    worker *w = (worker *)arg;
    unsigned i;

    for (i = 0; i < ITERATIONS; i++) {
        roundtrip(w);
    }
    pthread_mutex_lock(&worker_mu);
    worker_failures += w->failures;
    worker_checks += w->checks;
    pthread_mutex_unlock(&worker_mu);
    return NULL;
}

static double
now_seconds(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

typedef struct {
    unsigned iterations;
    unsigned failures;
    double elapsed;
} scale_worker;

/* Same work as roundtrip's encode half, on thread-owned memory over the
 * shared read-only schema: this is the shape of a server worker. */
static void *
run_scale_worker(void *arg)
{
    scale_worker *s = (scale_worker *)arg;
    structtest_scalars3_t msg;
    structtest_inner3_t inner;
    int32_t nums[3];
    const char *tags[2];
    structtest_inner3_t inners[2];
    bool flags[3];
    double start;
    unsigned i;

    s->elapsed = 0.0;
    fill_msg(&msg, &inner, nums, tags, inners, flags);
    start = now_seconds();
    for (i = 0; i < s->iterations; i++) {
        uint8_t *wire = NULL;
        size_t wire_len = 0;

        if (structtest_scalars3_encode(shared_schema, &msg, &wire, &wire_len) !=
                PBW_OK ||
            wire_len != want_len ||
            memcmp(wire, want, want_len) != 0) {
            s->failures++;
            pbw_free(wire);
            break;
        }
        pbw_free(wire);
    }
    s->elapsed = now_seconds() - start;
    return NULL;
}

/* Average per-thread time of `threads` threads each doing `iterations`
 * encodes. Each thread times its own loop so thread creation and joining
 * stay out of the number. Reports only; never fails. */
static double
time_scale(unsigned threads, unsigned iterations, unsigned *failures)
{
    pthread_t ids[THREADS];
    scale_worker workers[THREADS];
    double elapsed = 0.0;
    unsigned ran = 0;
    unsigned i;

    *failures = 0;
    for (i = 0; i < threads; i++) {
        workers[i].iterations = iterations;
        workers[i].failures = 0;
        workers[i].elapsed = 0.0;
        if (pthread_create(&ids[i], NULL, run_scale_worker, &workers[i]) != 0) {
            *failures += 1;
            threads = i;
            break;
        }
    }
    for (i = 0; i < threads; i++) {
        pthread_join(ids[i], NULL);
        *failures += workers[i].failures;
        elapsed += workers[i].elapsed;
        ran++;
    }
    return ran == 0 ? 0.0 : elapsed / (double)ran;
}

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

int
main(void)
{
    pthread_t threads[THREADS];
    worker workers[THREADS];
    double single;
    double multi;
    double ns_single;
    double ns_multi;
    unsigned scale_failures = 0;
    unsigned i;

    if (read_file("bindings/c/tests/data/matrix3.bin", &want, &want_len) !=
        0) {
        fprintf(stderr, "concurrency: cannot read matrix3.bin\n");
        return 2;
    }
    if (pbw_struct_schema_load(MATRIX3_DESC_PATH, &shared_schema) != PBW_OK ||
        shared_schema == NULL) {
        fprintf(stderr, "concurrency: cannot load matrix3.desc\n");
        free(want);
        return 2;
    }

    for (i = 0; i < THREADS; i++) {
        workers[i].id = i;
        workers[i].failures = 0;
        workers[i].checks = 0;
        if (pthread_create(&threads[i], NULL, run_worker, &workers[i]) != 0) {
            unsigned j;

            fprintf(stderr, "concurrency: pthread_create failed\n");
            for (j = 0; j < i; j++) {
                pthread_join(threads[j], NULL);
            }
            pbw_schema_free(shared_schema);
            free(want);
            return 2;
        }
    }
    for (i = 0; i < THREADS; i++) {
        pthread_join(threads[i], NULL);
    }
    printf("%u threads, %lu verifications, %u failures\n", THREADS,
           worker_checks, worker_failures);

    /* Warm up, then compare one thread against THREADS threads on the same
     * encode work. Informational only: speed is printed, never asserted. */
    (void)time_scale(1, SCALE_ITERATIONS / 20, &scale_failures);
    (void)time_scale(THREADS, SCALE_ITERATIONS / 20, &scale_failures);
    single = time_scale(1, SCALE_ITERATIONS, &scale_failures);
    multi = time_scale(THREADS, SCALE_ITERATIONS, &scale_failures);
    ns_single = single * 1e9 / SCALE_ITERATIONS;
    ns_multi = multi * 1e9 / SCALE_ITERATIONS;
    printf("scale encode 1 thread: %8.1f MB/s (%6.1f ns/op) | %u threads: "
           "%8.1f MB/s/thread, %8.1f MB/s aggregate\n",
           want_len / ns_single * 1e3, ns_single, THREADS,
           want_len / ns_multi * 1e3, THREADS * want_len / ns_multi * 1e3);

    pbw_schema_free(shared_schema);
    free(want);
    if (worker_failures != 0 || scale_failures != 0) {
        fprintf(stderr, "test_concurrency: %u worker + %u scale failure(s)\n",
                worker_failures, scale_failures);
        return 1;
    }
    printf("test_concurrency: all checks passed\n");
    return 0;
}

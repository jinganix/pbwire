/* Multithreaded self-check. The library keeps no globals and no thread-local
 * state, so the interesting failure modes are:
 *   - one thread reading another thread's recycled arena memory (pbw_ctx)
 *   - a lazily built or mutated cache inside a shared schema (linked or loaded)
 *   - two threads loading descriptors into separate schemas at the same time
 *   - independent trees/payloads being freed while another thread reads its own
 * Every thread therefore works on content derived from its own id, and verifies
 * that nothing of another thread's pattern survived. The last section measures
 * single- vs multi-thread throughput so a serialization regression (a hidden
 * lock) is visible instead of guessed. */

#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__)
/* clock_gettime() is POSIX; _SC_NPROCESSORS_ONLN is a BSD extension. */
#define _DARWIN_C_SOURCE 1
#endif

#include "pbwire.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "desc_set.h"

#define THREADS 8
#define ITERATIONS 2000
/* Descriptor loading is ~20 us, so the load loop runs fewer rounds. */
#define LOAD_ITERATIONS 100
#define SCALE_ITERATIONS 200000

typedef struct {
    pbw_schema *schema;          /* hand-built schema, shared read-only */
    const pbw_descriptor *msg;   /* "Msg" in that schema */
    const pbw_descriptor *outer; /* "pbwire.test.Outer" in a loaded schema */
    unsigned id;
    unsigned failures;
    unsigned long checks;
} worker;

static void
fail(worker *w, const char *what)
{
    fprintf(stderr, "worker %u: %s\n", w->id, what);
    w->failures++;
}

/* Payload content is derived from the worker id, so a stale pointer from
 * another worker shows up as a wrong byte. */
static int
fill(worker *w, unsigned iteration, pbw_message *msg)
{
    const unsigned len = 64 + (w->id * 97) % 4000;
    uint8_t *text = (uint8_t *)malloc(len ? len : 1);
    unsigned i;
    pbw_message *child;

    if (text == NULL) {
        return 0;
    }
    for (i = 0; i < len; i++) {
        text[i] = (uint8_t)(w->id * 31 + i);
    }
    if (pbw_message_set(msg, 1, pbw_value_new_bytes(text, len)) != PBW_OK) {
        free(text);
        return 0;
    }
    free(text);
    /* a few repeated values, one of them large enough to need a 5-byte varint */
    for (i = 0; i < 1 + w->id % 7; i++) {
        if (pbw_message_set(msg, 2, pbw_value_new_int((int64_t)(w->id * 1000) + (int64_t)i)) !=
            PBW_OK) {
            return 0;
        }
    }
    if (pbw_message_set(msg, 2, pbw_value_new_int(-(int64_t)w->id - 1)) != PBW_OK) {
        return 0;
    }
    child = pbw_message_set_message(msg, 3);
    if (child == NULL) {
        return 0;
    }
    if (pbw_message_set(child, 1, pbw_value_new_int((int64_t)(iteration * 7 + w->id))) !=
        PBW_OK) {
        return 0;
    }
    if (pbw_message_set(child, 2, pbw_value_new_string("thread")) != PBW_OK) {
        return 0;
    }
    if (pbw_message_set(msg, 4, pbw_value_new_int((int64_t)(w->id % 3u))) != PBW_OK) {
        return 0;
    }
    return 1;
}

static void
verify(worker *w, unsigned iteration, const pbw_message *msg)
{
    const unsigned len = 64 + (w->id * 97) % 4000;
    pbw_value *value = pbw_message_get(msg, 1);
    size_t got = 0;
    const uint8_t *text = (const uint8_t *)pbw_value_get_bytes(value, &got);
    unsigned i;
    pbw_message *child;

    w->checks++;
    if (text == NULL || got != len) {
        fail(w, "string length");
        return;
    }
    for (i = 0; i < len; i++) {
        if (text[i] != (uint8_t)(w->id * 31 + i)) {
            fail(w, "string content");
            return;
        }
    }
    if (pbw_value_count(pbw_message_get(msg, 2)) != 1 + w->id % 7 + 1) {
        fail(w, "repeated count");
        return;
    }
    if (pbw_value_get_int32(pbw_value_at(pbw_message_get(msg, 2), w->id % 7)) !=
        (int32_t)(w->id * 1000 + w->id % 7)) {
        fail(w, "repeated value");
        return;
    }
    child = pbw_value_get_message(pbw_message_get(msg, 3));
    if (child == NULL) {
        fail(w, "missing child");
        return;
    }
    if (pbw_value_get_int(pbw_message_get(child, 1)) !=
        (int64_t)(iteration * 7 + w->id)) {
        fail(w, "child value");
        return;
    }
    if (pbw_value_get_int32(pbw_message_get(msg, 4)) != (int32_t)(w->id % 3u)) {
        fail(w, "enum value");
        return;
    }
}

/* ------------------------------------------- loaded descriptor-set content */

/* pbwire.test.Outer from tests/data/desc_proto3.proto: scalar, proto3 packed
 * repetition, an explicitly unpacked repeated field, a nested message, a map
 * entry, a oneof case, a cross-file proto2 message and an enum. */
static int
fill_outer(worker *w, unsigned iteration, pbw_message *msg)
{
    int32_t base = (int32_t)(w->id * 1000u + iteration);
    pbw_message *inner;
    pbw_message *entry;
    pbw_message *legacy;
    char text[64];
    unsigned i;
    unsigned count = 1 + w->id % 5;

    if (pbw_message_set(msg, 1, pbw_value_new_int(base)) != PBW_OK) {
        return 0;
    }
    for (i = 0; i < count; i++) {
        if (pbw_message_set(msg, 2, pbw_value_new_int(base + (int32_t)i)) !=
            PBW_OK) {
            return 0;
        }
    }
    for (i = 0; i < 1 + w->id % 3; i++) {
        if (pbw_message_set(msg, 3, pbw_value_new_int(-(int64_t)(w->id + i))) !=
            PBW_OK) {
            return 0;
        }
    }
    inner = pbw_message_set_message(msg, 4);
    if (inner == NULL) {
        return 0;
    }
    snprintf(text, sizeof(text), "inner-%u-%u", w->id, iteration);
    if (pbw_message_set(inner, 1, pbw_value_new_string(text)) != PBW_OK) {
        return 0;
    }
    entry = pbw_message_set_message(msg, 5);
    if (entry == NULL) {
        return 0;
    }
    if (pbw_message_set(entry, 1, pbw_value_new_string("k")) != PBW_OK ||
        pbw_message_set(entry, 2, pbw_value_new_int(base)) != PBW_OK) {
        return 0;
    }
    /* oneof: setting b must not leave a present */
    if (pbw_message_set(msg, 7, pbw_value_new_string("choice")) != PBW_OK) {
        return 0;
    }
    legacy = pbw_message_set_message(msg, 8);
    if (legacy == NULL) {
        return 0;
    }
    if (pbw_message_set(legacy, 1, pbw_value_new_int(base)) != PBW_OK ||
        pbw_message_set(legacy, 4, pbw_value_new_int(-2)) != PBW_OK) {
        return 0;
    }
    if (pbw_message_set(msg, 9, pbw_value_new_int(1)) != PBW_OK) { /* COLOR_RED */
        return 0;
    }
    return 1;
}

static void
verify_outer(worker *w, unsigned iteration, const pbw_message *msg)
{
    int32_t base = (int32_t)(w->id * 1000u + iteration);
    unsigned count = 1 + w->id % 5;
    pbw_message *inner;
    pbw_message *legacy;
    pbw_message *entry;
    char want[64];

    w->checks++;
    if (pbw_value_get_int32(pbw_message_get(msg, 1)) != base) {
        fail(w, "loaded: scalar");
        return;
    }
    if (pbw_value_count(pbw_message_get(msg, 2)) != count ||
        pbw_value_get_int32(pbw_value_at(pbw_message_get(msg, 2), count - 1)) !=
            base + (int32_t)(count - 1)) {
        fail(w, "loaded: packed repeated");
        return;
    }
    if (pbw_value_count(pbw_message_get(msg, 3)) != 1 + w->id % 3 ||
        pbw_value_get_int(pbw_value_at(pbw_message_get(msg, 3), 0)) !=
            -(int64_t)w->id) {
        fail(w, "loaded: unpacked repeated");
        return;
    }
    inner = pbw_value_get_message(pbw_message_get(msg, 4));
    snprintf(want, sizeof(want), "inner-%u-%u", w->id, iteration);
    if (inner == NULL ||
        strcmp(pbw_value_get_string(pbw_message_get(inner, 1)), want) != 0) {
        fail(w, "loaded: nested message");
        return;
    }
    entry = pbw_value_get_message(pbw_value_at(pbw_message_get(msg, 5), 0));
    if (entry == NULL ||
        strcmp(pbw_value_get_string(pbw_message_get(entry, 1)), "k") != 0 ||
        pbw_value_get_int32(pbw_message_get(entry, 2)) != base) {
        fail(w, "loaded: map entry");
        return;
    }
    if (pbw_message_get(msg, 6) != NULL) {
        fail(w, "loaded: stale oneof case");
        return;
    }
    if (strcmp(pbw_value_get_string(pbw_message_get(msg, 7)), "choice") != 0) {
        fail(w, "loaded: oneof case");
        return;
    }
    legacy = pbw_value_get_message(pbw_message_get(msg, 8));
    if (legacy == NULL || pbw_value_get_int32(pbw_message_get(legacy, 1)) != base ||
        pbw_value_get_int32(pbw_message_get(legacy, 4)) != -2) {
        fail(w, "loaded: cross-file proto2");
        return;
    }
    if (pbw_value_get_int32(pbw_message_get(msg, 9)) != 1) {
        fail(w, "loaded: enum");
        return;
    }
}

/* Encode and decode one "Outer" round trip with this thread's own context. */
static int
outer_roundtrip(worker *w, unsigned iteration, pbw_ctx *ctx)
{
    pbw_message *source = pbw_message_new(w->outer);
    pbw_message *decoded = NULL;
    const uint8_t *wire = NULL;
    size_t wire_len = 0;
    int ok = 0;

    if (source == NULL || !fill_outer(w, iteration, source)) {
        fail(w, "loaded: fill");
    } else if (pbw_encode_ctx(ctx, source, &wire, &wire_len) != PBW_OK) {
        fail(w, "loaded: encode");
    } else if (pbw_decode_ctx(ctx, w->outer, wire, wire_len, &decoded) !=
               PBW_OK) {
        fail(w, "loaded: decode");
    } else {
        verify_outer(w, iteration, decoded);
        ok = 1;
    }
    pbw_message_free(source);
    pbw_ctx_reset(ctx);
    return ok;
}

/* --------------------------------------------------------------- workers */

/* Each thread owns a context; a context is never shared. */
static void *
run_ctx(void *arg)
{
    worker *w = (worker *)arg;
    pbw_ctx *ctx = pbw_ctx_new();
    unsigned iteration;

    if (ctx == NULL) {
        fail(w, "pbw_ctx_new");
        return NULL;
    }
    for (iteration = 0; iteration < ITERATIONS; iteration++) {
        pbw_message *source = pbw_message_new(w->msg);
        pbw_message *decoded = NULL;
        const uint8_t *encoded = NULL;
        size_t encoded_len = 0;

        if (source == NULL || !fill(w, iteration, source)) {
            fail(w, "fill");
            pbw_message_free(source);
            break;
        }
        if (pbw_encode_ctx(ctx, source, &encoded, &encoded_len) != PBW_OK) {
            fail(w, "pbw_encode_ctx");
        } else if (pbw_decode_ctx(ctx, w->msg, encoded, encoded_len, &decoded) !=
                   PBW_OK) {
            fail(w, "pbw_decode_ctx");
        } else {
            verify(w, iteration, decoded);
        }
        pbw_message_free(source);
        /* Resetting keeps the buffer and recycles the values; every few rounds
         * drop the context entirely so arenas of different sizes get reused. */
        if (iteration % 64 == 63) {
            pbw_ctx_free(ctx);
            ctx = pbw_ctx_new();
            if (ctx == NULL) {
                fail(w, "pbw_ctx_new after free");
                break;
            }
        } else {
            pbw_ctx_reset(ctx);
        }
    }
    pbw_ctx_free(ctx);
    return NULL;
}

/* The plain API allocates per message, so it must be safe with no context at
 * all: this covers accidental global state anywhere else in the library. */
static void *
run_plain(void *arg)
{
    worker *w = (worker *)arg;
    unsigned iteration;

    for (iteration = 0; iteration < ITERATIONS / 8; iteration++) {
        pbw_message *source = pbw_message_new(w->msg);
        pbw_message *decoded = NULL;
        uint8_t *encoded = NULL;
        size_t encoded_len = 0;

        if (source == NULL || !fill(w, iteration, source)) {
            fail(w, "fill");
            pbw_message_free(source);
            break;
        }
        if (pbw_encode(source, &encoded, &encoded_len) != PBW_OK ||
            pbw_decode(w->msg, encoded, encoded_len, &decoded) != PBW_OK) {
            fail(w, "plain roundtrip");
        } else {
            verify(w, iteration, decoded);
        }
        pbw_free(encoded);
        pbw_message_free(decoded);
        pbw_message_free(source);
    }
    return NULL;
}

/* Reflection over the shared, already linked schema: every lookup table must
 * give the same answer in every thread, which only holds if nothing mutates the
 * descriptors after pbw_schema_link(). */
static void *
run_reflect(void *arg)
{
    worker *w = (worker *)arg;
    const pbw_descriptor *color = pbw_schema_find(w->schema, "Color");
    unsigned iteration;

    if (color == NULL) {
        fail(w, "missing enum");
        return NULL;
    }
    for (iteration = 0; iteration < ITERATIONS * 8; iteration++) {
        size_t i;
        const char *name = NULL;
        int32_t number = 0;

        w->checks++;
        for (i = 0; i < pbw_descriptor_field_count(w->msg); i++) {
            const pbw_field *field = pbw_descriptor_field_at(w->msg, i);
            const pbw_field *by_name;
            const pbw_field *by_number;

            if (field == NULL) {
                fail(w, "field_at");
                return NULL;
            }
            by_number = pbw_descriptor_find_field(w->msg, pbw_field_number(field));
            by_name =
                pbw_descriptor_find_field_name(w->msg, pbw_field_name(field));
            if (by_number != field || by_name != field) {
                fail(w, "shared schema lookup");
                return NULL;
            }
            if (pbw_field_number(by_name) != pbw_field_number(field) ||
                pbw_field_type(by_name) != pbw_field_type(field) ||
                pbw_field_label(by_name) != pbw_field_label(field) ||
                pbw_field_is_packed(by_name) != pbw_field_is_packed(field) ||
                pbw_field_oneof_index(by_name) != pbw_field_oneof_index(field) ||
                pbw_field_message_type(by_name) !=
                    pbw_field_message_type(field)) {
                fail(w, "field accessors disagree");
                return NULL;
            }
        }
        if (pbw_descriptor_find_field(w->msg, 9999) != NULL ||
            pbw_descriptor_find_field_name(w->msg, "nope") != NULL ||
            pbw_descriptor_field_at(w->msg,
                                    pbw_descriptor_field_count(w->msg)) != NULL) {
            fail(w, "lookup returned a field it does not declare");
            return NULL;
        }
        if (!pbw_enum_find_value(color, "GREEN", &number) || number != 2 ||
            !pbw_enum_find_name(color, 2, &name) || name == NULL ||
            strcmp(name, "GREEN") != 0 ||
            pbw_enum_find_value(color, "PURPLE", &number) ||
            pbw_enum_find_name(color, 42, &name)) {
            fail(w, "enum lookup");
            return NULL;
        }
    }
    return NULL;
}

/* Loading a FileDescriptorSet is pure work on caller-owned memory: two threads
 * doing it at the same time must not share anything. Each round loads the set,
 * uses the schema (including the group field) and frees it again; the
 * truncated set exercises the error path concurrently. */
static void *
run_dynamic_load(void *arg)
{
    worker *w = (worker *)arg;
    pbw_ctx *ctx = pbw_ctx_new();
    unsigned iteration;

    if (ctx == NULL) {
        fail(w, "pbw_ctx_new");
        return NULL;
    }
    for (iteration = 0; iteration < LOAD_ITERATIONS; iteration++) {
        pbw_schema *loaded = NULL;
        pbw_schema *broken = NULL;
        pbw_descriptor *outer;
        pbw_descriptor *grouped;
        pbw_descriptor *color;

        if (pbw_descriptor_set_load(desc_set, sizeof(desc_set), &loaded) !=
            PBW_OK) {
            fail(w, "pbw_descriptor_set_load");
            pbw_schema_free(loaded);
            break;
        }
        outer = pbw_schema_find(loaded, "pbwire.test.Outer");
        grouped = pbw_schema_find(loaded, "pbwire.test.Grouped");
        color = pbw_schema_find(loaded, "pbwire.test.Color");
        if (outer == NULL || grouped == NULL || color == NULL) {
            fail(w, "loaded schema is incomplete");
            pbw_schema_free(loaded);
            break;
        }
        w->outer = outer; /* private to this thread for the round trip */
        outer_roundtrip(w, iteration, ctx);

        /* the group field loads like a message field */
        {
            static const uint8_t wire[] = {0x0b, 0x10, 0x07, 0x0c, 0x18, 0x09};
            pbw_message *decoded = NULL;

            if (pbw_decode(grouped, wire, sizeof(wire), &decoded) != PBW_OK) {
                fail(w, "loaded group decode");
            } else {
                pbw_message *group =
                    pbw_value_get_message(pbw_message_get(decoded, 1));
                w->checks++;
                if (pbw_value_get_int32(pbw_message_get(decoded, 3)) != 9 ||
                    group == NULL ||
                    pbw_value_get_int32(pbw_message_get(group, 2)) != 7) {
                    fail(w, "loaded group handling");
                }
            }
            pbw_message_free(decoded);
        }

        /* a truncated set fails with a reason, it does not crash */
        if (pbw_descriptor_set_load(desc_set, sizeof(desc_set) - 1, &broken) ==
            PBW_OK) {
            fail(w, "truncated descriptor set accepted");
        } else if (pbw_schema_error(broken)[0] == '\0') {
            fail(w, "truncated descriptor set without a reason");
        }
        pbw_schema_free(broken);
        pbw_schema_free(loaded);
    }
    pbw_ctx_free(ctx);
    return NULL;
}

/* One loaded schema shared by every thread, read-only, like a schema compiled
 * once at startup. */
static void *
run_shared_loaded(void *arg)
{
    worker *w = (worker *)arg;
    pbw_ctx *ctx = pbw_ctx_new();
    unsigned iteration;

    if (ctx == NULL) {
        fail(w, "pbw_ctx_new");
        return NULL;
    }
    for (iteration = 0; iteration < ITERATIONS / 4; iteration++) {
        outer_roundtrip(w, iteration, ctx);
    }
    pbw_ctx_free(ctx);
    return NULL;
}

/* ------------------------------------------------------------- thread pool */

static int
run(pbw_schema *schema, const char *label, void *(*entry)(void *),
    const pbw_descriptor *outer)
{
    pthread_t threads[THREADS];
    worker workers[THREADS];
    unsigned i;
    unsigned failures = 0;
    unsigned long checks = 0;

    for (i = 0; i < THREADS; i++) {
        memset(&workers[i], 0, sizeof(workers[i]));
        workers[i].schema = schema;
        workers[i].msg = pbw_schema_find(schema, "Msg");
        workers[i].outer = outer;
        workers[i].id = i;
        if (pthread_create(&threads[i], NULL, entry, &workers[i]) != 0) {
            fprintf(stderr, "%s: pthread_create failed\n", label);
            return 1;
        }
    }
    for (i = 0; i < THREADS; i++) {
        pthread_join(threads[i], NULL);
        failures += workers[i].failures;
        checks += workers[i].checks;
    }
    printf("%-13s %u threads, %lu verifications, %u failures\n", label, THREADS,
           checks, failures);
    return failures == 0 ? 0 : 1;
}

/* ------------------------------------------------------- scaling check */

static double
now_seconds(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

typedef struct {
    const pbw_descriptor *msg;
    const pbw_message *source; /* read-only, shared by all encode workers */
    const uint8_t *wire;       /* read-only, shared by all decode workers */
    size_t wire_len;
    unsigned iterations;
    int encode;
    unsigned failures;
    double elapsed; /* seconds, measured inside the thread */
} scale_worker;

/* Every thread decodes the same bytes / encodes the same read-only message with
 * its own context: that is the shape of a server worker, and it must not need
 * any shared mutable state. */
static void *
run_scale_worker(void *arg)
{
    scale_worker *s = (scale_worker *)arg;
    pbw_ctx *ctx = pbw_ctx_new();
    double start;
    unsigned i;

    s->elapsed = 0.0;
    if (ctx == NULL) {
        s->failures++;
        return NULL;
    }
    start = now_seconds();
    for (i = 0; i < s->iterations; i++) {
        if (s->encode) {
            const uint8_t *out = NULL;
            size_t len = 0;

            if (pbw_encode_ctx(ctx, s->source, &out, &len) != PBW_OK ||
                len == 0) {
                s->failures++;
                break;
            }
        } else {
            pbw_message *decoded = NULL;

            if (pbw_decode_ctx(ctx, s->msg, s->wire, s->wire_len, &decoded) !=
                PBW_OK) {
                s->failures++;
                break;
            }
        }
        pbw_ctx_reset(ctx);
    }
    s->elapsed = now_seconds() - start;
    pbw_ctx_free(ctx);
    return NULL;
}

/* Average time of `threads` threads each doing `iterations` operations. Each
 * thread times its own loop, so thread creation and joining stay out of the
 * number; averaging over the threads keeps one straggler on a slow core from
 * dominating the measurement. */
static double
time_scale(unsigned threads, unsigned iterations, const pbw_descriptor *msg,
           const pbw_message *source, const uint8_t *wire, size_t wire_len,
           int encode, unsigned *failures)
{
    pthread_t ids[THREADS];
    scale_worker workers[THREADS];
    double elapsed = 0.0;
    unsigned ran = 0;
    unsigned i;

    *failures = 0;
    for (i = 0; i < threads; i++) {
        memset(&workers[i], 0, sizeof(workers[i]));
        workers[i].msg = msg;
        workers[i].source = source;
        workers[i].wire = wire;
        workers[i].wire_len = wire_len;
        workers[i].iterations = iterations;
        workers[i].encode = encode;
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

static long
cpu_count(void)
{
    long cores = sysconf(_SC_NPROCESSORS_ONLN);

    return cores > 0 ? cores : 1;
}

/* Compare one thread against THREADS threads on the same work. Reports the
 * per-operation latency in both cases plus the aggregate speedup, and fails only
 * if the aggregate throughput collapses: a coarse detector for a hidden lock or
 * false sharing (which would report about x1), loose enough not to trip on a
 * busy machine or a host with fewer available cores than threads. */
static int
run_scale(pbw_schema *schema, const char *label, int encode)
{
    const pbw_descriptor *msg = pbw_schema_find(schema, "Msg");
    const long cores = cpu_count();
    const double required = 0.35 * (double)(cores < THREADS ? cores : THREADS);
    pbw_message *source = pbw_message_new(msg);
    worker filler;
    uint8_t *wire = NULL;
    size_t wire_len = 0;
    unsigned failures = 0;
    double single;
    double multi;
    double ns_single;
    double ns_multi;
    double speedup;
    int result = 0;

    memset(&filler, 0, sizeof(filler));
    filler.id = 1;
    if (source == NULL || msg == NULL || !fill(&filler, 3, source) ||
        pbw_encode(source, &wire, &wire_len) != PBW_OK) {
        fprintf(stderr, "scale %s: setup failed\n", label);
        pbw_message_free(source);
        pbw_free(wire);
        return 1;
    }

    /* Warm up both configurations so page faults and arena growth do not end up
     * in the measured runs. */
    (void)time_scale(1, SCALE_ITERATIONS / 20, msg, source, wire, wire_len,
                     encode, &failures);
    (void)time_scale(THREADS, SCALE_ITERATIONS / 20, msg, source, wire, wire_len,
                     encode, &failures);

    single = time_scale(1, SCALE_ITERATIONS, msg, source, wire, wire_len, encode,
                        &failures);
    multi = time_scale(THREADS, SCALE_ITERATIONS, msg, source, wire, wire_len,
                       encode, &failures);
    if (failures != 0) {
        fprintf(stderr, "scale %s: %u failed operation(s)\n", label, failures);
        result = 1;
    }

    ns_single = single * 1e9 / SCALE_ITERATIONS;
    ns_multi = multi * 1e9 / SCALE_ITERATIONS;
    speedup = THREADS * single / multi;
    printf("scale %-10s 1 thread: %8.1f MB/s (%6.1f ns/op) | %u threads: "
           "%8.1f MB/s/thread, %8.1f MB/s aggregate (per-op %.2fx, aggregate "
           "%.2fx, %ld cores)\n",
           label, wire_len / ns_single * 1e3, ns_single, THREADS,
           wire_len / ns_multi * 1e3, THREADS * wire_len / ns_multi * 1e3,
           ns_single > 0 ? ns_multi / ns_single : 0.0, speedup, cores);
    if (speedup < required) {
        fprintf(stderr,
                "scale %s: aggregate throughput collapsed (x%.2f, expected at "
                "least x%.2f)\n",
                label, speedup, required);
        result = 1;
    }

    pbw_message_free(source);
    pbw_free(wire);
    return result;
}

/* ------------------------------------------------------- stack headroom */

/* The nesting cap bounds recursion, so it also bounds stack: hostile input may
 * legally reach PBW_MAX_DEPTH, and every level costs one frame of the decode /
 * JSON / encode recursion. Measured worst case at depth 255 is ~1.4 KB per
 * level in a release build (~1.6 KB for the printers), ~3.8 KB for the encoder
 * in a -O0 build, and ~1 MB under ASan. A 2 MB worker stack therefore covers
 * every build shape; a future frame bloat that needs more trips this check
 * instead of a host's thread. */
#define STACK_PROBE_BYTES (2u * 1024u * 1024u)
#define STACK_PROBE_DEPTH (PBW_MAX_DEPTH - 1)
#define STACK_PROBE_MAX (4096)

static unsigned stack_probe_failures;

/* Reports the status the failing call actually returned, so an injected OOM
 * inside a probe does not read like a format error. */
static void
stack_fail(const char *what, pbw_status status)
{
    fprintf(stderr, "stack probe: %s: %s\n", what, pbw_strerror(status));
    stack_probe_failures++;
}

/* `depth` nested messages through field 2 (wire type 2), innermost value 1.
 * Alternates between the two buffers, so the caller re-checks which holds the
 * result. */
static size_t
layered_wire(uint8_t **out)
{
    uint8_t *cur = (uint8_t *)malloc(STACK_PROBE_MAX);
    uint8_t *next = (uint8_t *)malloc(STACK_PROBE_MAX);
    uint8_t *swap;
    size_t len = 2;
    size_t i;

    if (cur == NULL || next == NULL) {
        free(cur);
        free(next);
        *out = NULL;
        return 0;
    }
    cur[0] = 0x08; /* field 1: varint */
    cur[1] = 0x01;
    for (i = 0; i < STACK_PROBE_DEPTH; i++) {
        size_t at = 1;
        size_t n = len;

        next[0] = 0x12; /* field 2: nested message */
        while (n >= 0x80) {
            next[at++] = (uint8_t)(n | 0x80);
            n >>= 7;
        }
        next[at++] = (uint8_t)n;
        memcpy(next + at, cur, len);
        len += at;
        swap = cur;
        cur = next;
        next = swap;
    }
    free(next);
    *out = cur;
    return len;
}

static void *
stack_probe(void *arg)
{
    pbw_schema *schema = (pbw_schema *)arg;
    const pbw_descriptor *node = pbw_schema_find(schema, "Node");
    pbw_message *decoded = NULL;
    pbw_message *built = NULL;
    pbw_message *tip = NULL;
    uint8_t *wire = NULL;
    uint8_t *enc = NULL;
    char *json = NULL;
    char *printed = NULL;
    size_t len = 0;
    size_t at = 0;
    size_t elen = 0;
    size_t plen = 0;
    size_t i;

    if (node == NULL) {
        stack_fail("schema: Node missing", PBW_ERR_INVALID);
        return NULL;
    }

    /* wire: decode a cap-deep tree */
    len = layered_wire(&wire);
    if (wire == NULL) {
        stack_fail("wire: allocation", PBW_ERR_NOMEM);
        return NULL;
    }
    {
        pbw_status status = pbw_decode(node, wire, len, &decoded);

        if (status != PBW_OK) {
            stack_fail("decode", status);
        }
    }
    pbw_message_free(decoded);
    free(wire);

    /* JSON: parse and print the same nesting */
    json = (char *)malloc(STACK_PROBE_MAX);
    if (json == NULL) {
        stack_fail("json: allocation", PBW_ERR_NOMEM);
        return NULL;
    }
    for (i = 0; i < STACK_PROBE_DEPTH; i++) {
        memcpy(json + at, "{\"kid\":", 7);
        at += 7;
    }
    json[at++] = '{';
    json[at++] = '}';
    for (i = 0; i < STACK_PROBE_DEPTH; i++) {
        json[at++] = '}';
    }
    decoded = NULL;
    {
        pbw_status status = pbw_json_parse(node, json, at, &decoded);

        if (status != PBW_OK) {
            stack_fail("json parse", status);
        } else if ((status = pbw_json_print(decoded, &printed, &plen)) != PBW_OK ||
                   printed == NULL || plen == 0) {
            stack_fail("json print", status != PBW_OK ? status : PBW_ERR_INVALID);
        }
        pbw_free(printed);
        pbw_message_free(decoded);
    }
    free(json);

    /* encode: build the same tree and serialize it */
    built = pbw_message_new(node);
    if (built == NULL) {
        stack_fail("encode: allocation", PBW_ERR_NOMEM);
        return NULL;
    }
    tip = built;
    for (i = 0; i < STACK_PROBE_DEPTH && tip != NULL; i++) {
        tip = pbw_message_set_message(tip, 2);
    }
    {
        pbw_status status = PBW_OK;

        if (tip == NULL || (status = pbw_message_set(tip, 1, pbw_value_new_int(1))) != PBW_OK ||
            (status = pbw_encode(built, &enc, &elen)) != PBW_OK ||
            enc == NULL) {
            stack_fail("encode", status != PBW_OK ? status : PBW_ERR_NOMEM);
        }
    }
    pbw_free(enc);
    pbw_message_free(built);
    return NULL;
}

/* The other half of the same contract: a host that cannot spare even the
 * release-build stack (64 KB here) lowers the nesting cap per call instead.
 * Without pbw_limits the 255-level input would recurse into a SIGBUS on this
 * thread, which is why the limit probe runs on one. */
#define LIMIT_PROBE_BYTES (64u * 1024u)

static void *
limit_probe(void *arg)
{
    pbw_schema *schema = (pbw_schema *)arg;
    const pbw_descriptor *node = pbw_schema_find(schema, "Node");
    pbw_json_parse_options opts;
    pbw_limits lim;
    pbw_message *out = NULL;
    pbw_status status;
    char *json;
    size_t at = 0;
    size_t i;

    if (node == NULL) {
        stack_fail("limit probe: schema: Node missing", PBW_ERR_INVALID);
        return NULL;
    }
    json = (char *)malloc(STACK_PROBE_MAX);
    if (json == NULL) {
        stack_fail("limit probe: allocation", PBW_ERR_NOMEM);
        return NULL;
    }
    for (i = 0; i < STACK_PROBE_DEPTH; i++) {
        memcpy(json + at, "{\"kid\":", 7);
        at += 7;
    }
    json[at++] = '{';
    json[at++] = '}';
    for (i = 0; i < STACK_PROBE_DEPTH; i++) {
        json[at++] = '}';
    }
    memset(&opts, 0, sizeof(opts));
    lim.max_bytes = 0;
    lim.max_depth = 4;
    status = pbw_json_parse_limits(node, json, at, &lim, &opts, &out);
    free(json);
    if (status != PBW_ERR_LIMIT || out != NULL) {
        stack_fail("limit probe: cap-depth JSON did not report PBW_ERR_LIMIT",
                   status);
        return NULL;
    }
    /* the same call without the cap reaches PBW_MAX_DEPTH, which no 64 KB
     * stack can hold: the test only proves the knob was applied because the
     * capped parse survived */
    printf("limit probe: depth cap honoured on a %u KB stack\n",
           (unsigned)(LIMIT_PROBE_BYTES / 1024));
    return NULL;
}

/* One thread, explicit stack: the probe must finish on it. */
static unsigned
run_stack_bound(void)
{
    pbw_schema *schema = pbw_schema_new();
    pbw_descriptor *node;
    pbw_field *field;
    pthread_attr_t attr;
    pthread_t thread;
    unsigned failures = 0;

    if (schema == NULL) {
        fprintf(stderr, "stack probe: cannot create a schema\n");
        return 1;
    }
    node = pbw_schema_add_message(schema, "Node");
    pbw_message_add_field(node, "value", 1, PBW_TYPE_INT32,
                          PBW_LABEL_OPTIONAL);
    field = pbw_message_add_field(node, "kid", 2, PBW_TYPE_MESSAGE,
                                  PBW_LABEL_OPTIONAL);
    pbw_field_set_type_name(field, "Node");
    if (pbw_schema_link(schema) != PBW_OK) {
        fprintf(stderr, "stack probe: schema: %s\n", pbw_schema_error(schema));
        pbw_schema_free(schema);
        return 1;
    }

    stack_probe_failures = 0;
    pthread_attr_init(&attr);
    if (pthread_attr_setstacksize(&attr, STACK_PROBE_BYTES) != 0) {
        fprintf(stderr, "stack probe: cannot set a %u byte stack\n",
                (unsigned)STACK_PROBE_BYTES);
        failures = 1;
    } else if (pthread_create(&thread, &attr, stack_probe, schema) != 0) {
        fprintf(stderr, "stack probe: cannot start the thread\n");
        failures = 1;
    } else {
        pthread_join(thread, NULL);
        if (stack_probe_failures != 0) {
            fprintf(stderr, "stack probe: %u failure(s) at depth %d\n",
                    stack_probe_failures, (int)STACK_PROBE_DEPTH);
            failures = 1;
        } else {
            printf("stack probe: PBW_MAX_DEPTH fits a %u KB worker stack\n",
                   (unsigned)(STACK_PROBE_BYTES / 1024));
        }
    }
    /* ...and the per-call cap works on a stack PBW_MAX_DEPTH could not use */
    if (failures == 0) {
        if (pthread_attr_setstacksize(&attr, LIMIT_PROBE_BYTES) != 0 ||
            pthread_create(&thread, &attr, limit_probe, schema) != 0) {
            fprintf(stderr, "stack probe: cannot start the limit thread\n");
            failures = 1;
        } else {
            pthread_join(thread, NULL);
            if (stack_probe_failures != 0) {
                fprintf(stderr, "limit probe: %u failure(s)\n",
                        stack_probe_failures);
                failures = 1;
            }
        }
    }
    pthread_attr_destroy(&attr);
    pbw_schema_free(schema);
    return failures;
}

/* ---------------------------------------------------------------- schema */

static pbw_schema *
make_schema(void)
{
    pbw_schema *schema = pbw_schema_new();
    pbw_descriptor *msg = pbw_schema_add_message(schema, "Msg");
    pbw_descriptor *child = pbw_schema_add_message(schema, "Child");
    pbw_descriptor *mode = pbw_schema_add_enum(schema, "Color");
    pbw_field *field;

    pbw_enum_add_value(mode, "OFF", 0);
    pbw_enum_add_value(mode, "ON", 1);
    pbw_enum_add_value(mode, "GREEN", 2);

    pbw_message_add_field(child, "value", 1, PBW_TYPE_INT64, PBW_LABEL_OPTIONAL);
    pbw_message_add_field(child, "note", 2, PBW_TYPE_STRING, PBW_LABEL_OPTIONAL);

    pbw_message_add_field(msg, "text", 1, PBW_TYPE_BYTES, PBW_LABEL_OPTIONAL);
    field = pbw_message_add_field(msg, "nums", 2, PBW_TYPE_INT64,
                                  PBW_LABEL_REPEATED);
    pbw_field_set_packed(field, 1);
    field = pbw_message_add_field(msg, "child", 3, PBW_TYPE_MESSAGE,
                                  PBW_LABEL_OPTIONAL);
    pbw_field_set_type_name(field, "Child");
    field = pbw_message_add_field(msg, "mode", 4, PBW_TYPE_ENUM,
                                  PBW_LABEL_OPTIONAL);
    pbw_field_set_type_name(field, "Color");
    if (pbw_schema_link(schema) != PBW_OK) {
        fprintf(stderr, "schema: %s\n", pbw_schema_error(schema));
        pbw_schema_free(schema);
        return NULL;
    }
    return schema;
}

int
main(void)
{
    pbw_schema *schema = make_schema();
    pbw_schema *loaded = NULL;
    pbw_descriptor *outer = NULL;
    int failures = 0;

    if (schema == NULL) {
        return 2;
    }
    /* One loaded schema, shared by all threads of run_shared_loaded(). */
    if (pbw_descriptor_set_load(desc_set, sizeof(desc_set), &loaded) != PBW_OK) {
        fprintf(stderr, "load: %s\n", pbw_schema_error(loaded));
        pbw_schema_free(loaded);
        pbw_schema_free(schema);
        return 2;
    }
    outer = pbw_schema_find(loaded, "pbwire.test.Outer");
    if (outer == NULL) {
        fprintf(stderr, "load: pbwire.test.Outer missing\n");
        pbw_schema_free(loaded);
        pbw_schema_free(schema);
        return 2;
    }

    /* The linked schema is immutable and shared; every thread reads it. */
    failures += run(schema, "ctx", run_ctx, NULL);
    failures += run(schema, "plain", run_plain, NULL);
    failures += run(schema, "reflect", run_reflect, NULL);
    failures += run(schema, "load", run_dynamic_load, NULL);
    failures += run(schema, "loaded-shared", run_shared_loaded, outer);
    failures += run_stack_bound();
    /* PBW_SKIP_SCALE=1 (used by `make coverage`): gcov edge counters are
     * shared globals, so instrumented threads serialize on them and any
     * throughput ratio is noise. The detector still runs under plain
     * `make test`, where it is meaningful. */
    if (getenv("PBW_SKIP_SCALE") == NULL) {
        failures += run_scale(schema, "ctx decode", 0);
        failures += run_scale(schema, "ctx encode", 1);
    }

    pbw_schema_free(loaded);
    pbw_schema_free(schema);
    if (failures != 0) {
        fprintf(stderr, "test_threads: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_threads: all checks passed\n");
    return 0;
}

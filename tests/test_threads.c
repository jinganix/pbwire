/* Multithreaded self-check: the arena in pbw_ctx is the one piece of state that
 * is reused across calls, so the interesting failure mode is one thread reading
 * another thread's recycled memory. Every thread therefore decodes messages
 * whose content is a function of its own id, resets its context, and verifies
 * that nothing of another thread's pattern survived. */

#include "pbwire.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define THREADS 8
#define ITERATIONS 2000

typedef struct {
    const pbw_schema *schema;
    const pbw_descriptor *msg;
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
}

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

static int
run(pbw_schema *schema, const char *label,
    void *(*entry)(void *))
{
    pthread_t threads[THREADS];
    worker workers[THREADS];
    unsigned i;
    unsigned failures = 0;
    unsigned long checks = 0;

    for (i = 0; i < THREADS; i++) {
        workers[i].schema = schema;
        workers[i].msg = pbw_schema_find(schema, "Msg");
        workers[i].id = i;
        workers[i].failures = 0;
        workers[i].checks = 0;
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
    printf("%-6s %u threads, %lu verifications, %u failures\n", label, THREADS,
           checks, failures);
    return failures == 0 ? 0 : 1;
}

static pbw_schema *
make_schema(void)
{
    pbw_schema *schema = pbw_schema_new();
    pbw_descriptor *msg = pbw_schema_add_message(schema, "Msg");
    pbw_descriptor *child = pbw_schema_add_message(schema, "Child");
    pbw_field *field;

    pbw_message_add_field(child, "value", 1, PBW_TYPE_INT64, PBW_LABEL_OPTIONAL);
    pbw_message_add_field(child, "note", 2, PBW_TYPE_STRING, PBW_LABEL_OPTIONAL);

    pbw_message_add_field(msg, "text", 1, PBW_TYPE_BYTES, PBW_LABEL_OPTIONAL);
    field = pbw_message_add_field(msg, "nums", 2, PBW_TYPE_INT64,
                                  PBW_LABEL_REPEATED);
    pbw_field_set_packed(field, 1);
    field = pbw_message_add_field(msg, "child", 3, PBW_TYPE_MESSAGE,
                                  PBW_LABEL_OPTIONAL);
    pbw_field_set_type_name(field, "Child");
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
    int failures = 0;

    if (schema == NULL) {
        return 2;
    }
    /* The linked schema is immutable and shared; every thread reads it. */
    failures += run(schema, "ctx", run_ctx);
    failures += run(schema, "plain", run_plain);
    pbw_schema_free(schema);
    if (failures != 0) {
        fprintf(stderr, "test_threads: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_threads: all checks passed\n");
    return 0;
}

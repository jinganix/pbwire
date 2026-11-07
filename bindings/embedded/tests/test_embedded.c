/* Assert-based self-check for the embedded facade: no test framework, no
 * dependencies. Runs from the repo root (see the Makefile rule). */

#include "pbwe.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
            fprintf(stderr, "%s:%d: expected PBW_OK, got %d\n", __FILE__, __LINE__, \
                    (int)status_);                                                  \
            failures++;                                                             \
        }                                                                           \
    } while (0)

#define CHECK_STATUS(expr, want)                                             \
    do {                                                                     \
        pbw_status status_ = (expr);                                         \
        if (status_ != (want)) {                                             \
            fprintf(stderr, "%s:%d: expected status %d, got %d\n", __FILE__, \
                    __LINE__, (int)(want), (int)status_);                    \
            failures++;                                                      \
        }                                                                    \
    } while (0)

static uint8_t *
read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    uint8_t *buf;
    long size;
    size_t n;

    CHECK(f != NULL);
    if (f == NULL) {
        return NULL;
    }
    CHECK(fseek(f, 0, SEEK_END) == 0);
    size = ftell(f);
    CHECK(fseek(f, 0, SEEK_SET) == 0);
    CHECK(size >= 0);
    if (size < 0) {
        fclose(f);
        return NULL;
    }
    n = (size_t)size;
    buf = (uint8_t *)malloc(n > 0 ? n : 1);
    CHECK(buf != NULL);
    if (buf == NULL) {
        fclose(f);
        return NULL;
    }
    CHECK(fread(buf, 1, n, f) == n);
    fclose(f);
    *len = n;
    return buf;
}

#ifdef PBWE_HAVE_ALLOC_HOOKS
/* The nofail build links the core with -Dmalloc=pbw_fail_malloc (see the
 * Makefile): these count instead of failing, so the steady-state promise
 * ("no allocation for a message that fits the retained budget") is asserted
 * rather than assumed. The test's own malloc stays unhooked. */
static long hook_calls;

void *
pbw_fail_malloc(size_t n)
{
    hook_calls++;
    return malloc(n);
}

void *
pbw_fail_calloc(size_t count, size_t size)
{
    hook_calls++;
    return calloc(count, size);
}

void *
pbw_fail_realloc(void *ptr, size_t n)
{
    hook_calls++;
    return realloc(ptr, n);
}

void
pbw_fail_free(void *ptr)
{
    free(ptr);
}
#endif

int
main(void)
{
    static uint8_t wire[64 * 1024];
    static uint8_t frame[64 * 1024];
    static const uint8_t broken[] = {0x00}; /* tag with field number 0 */
#ifdef PBWE_HAVE_ALLOC_HOOKS
    const char *label = "test_embedded (alloc-counting core)";
#else
    const char *label = "test_embedded";
#endif
    uint8_t *desc_bytes;
    uint8_t *ref_bytes;
    size_t desc_len = 0, ref_len = 0;
    pbwe_box box;
    const pbw_message *msg = NULL;
    const pbw_descriptor *desc;
    pbw_limits tight;
    pbw_status status;
    size_t len = 0;
    int i;

    memset(&box, 0, sizeof(box));
    desc_bytes = read_file("bindings/c/tests/data/matrix3.desc", &desc_len);
    ref_bytes = read_file("bindings/c/tests/data/matrix3.bin", &ref_len);
    CHECK(desc_bytes != NULL && ref_bytes != NULL);
    if (failures > 0) {
        return 1;
    }
    CHECK(ref_len < sizeof(wire));

    /* Open once; the rest of the test must not allocate per call. */
    CHECK_OK(pbwe_open(&box, desc_bytes, desc_len, 64 * 1024, NULL));
    desc = pbw_schema_find(box.schema, "structtest.Scalars3");
    CHECK(desc != NULL);

    /* Decode the pinned oracle, encode it back into caller memory. */
    CHECK_OK(pbwe_decode(&box, "structtest.Scalars3", ref_bytes, ref_len, &msg));
    CHECK(msg != NULL);
    CHECK_OK(pbwe_encode(msg, wire, sizeof(wire), &len));
    CHECK(len == ref_len);
    CHECK(memcmp(wire, ref_bytes, ref_len) == 0);

    /* The documented sizing recipe holds: exact size plus PBW_ENCODE_SLACK
     * fits, and the exact size is what the encoder then writes. */
    {
        size_t need = 0;

        CHECK_OK(pbw_message_encoded_size(msg, &need));
        CHECK(need == ref_len);
        CHECK_OK(pbwe_encode(msg, wire, need + PBW_ENCODE_SLACK, &len));
        CHECK(len == ref_len && memcmp(wire, ref_bytes, ref_len) == 0);
    }

    /* A buffer that is one byte short fails instead of growing. */
    CHECK_STATUS(pbwe_encode(msg, wire, len - 1, &len), PBW_ERR_LIMIT);
    CHECK(len == 0); /* no stale length survives a failure */
    len = 7;
    CHECK_STATUS(pbwe_encode(NULL, wire, sizeof(wire), &len), PBW_ERR_INVALID);
    CHECK(len == 0);
    CHECK_STATUS(pbwe_encode(msg, NULL, sizeof(wire), &len), PBW_ERR_INVALID);
    CHECK(len == 0);
    CHECK_STATUS(pbwe_encode(msg, wire, sizeof(wire), NULL), PBW_ERR_INVALID);

    /* The length-delimited framing, both directions, against the same oracle:
     * the prefix is 1 byte under 128, 2 above it (192 here). */
    {
        uint8_t scratch[300]; /* the failure cases must not clobber frame */
        size_t framed = 0;
        size_t consumed = 0;
        size_t need = 0;
        size_t cap;

        CHECK_OK(pbw_message_encoded_size(msg, &need));
        CHECK(need == ref_len);
        CHECK_OK(pbwe_encode_delimited(msg, frame, sizeof(frame), &framed));
        CHECK(framed == ref_len + 2);
        CHECK(memcmp(frame + 2, ref_bytes, ref_len) == 0);

        /* The documented cap recipe fits; one byte less does not. */
        cap = need + PBW_ENCODE_SLACK + 2;
        CHECK_OK(pbwe_encode_delimited(msg, frame, cap, &framed));
        CHECK(framed == ref_len + 2);
        CHECK_STATUS(pbwe_encode_delimited(msg, scratch, framed - 1, &consumed), PBW_ERR_LIMIT);
        CHECK(consumed == 0);
        CHECK_STATUS(pbwe_encode_delimited(msg, scratch, sizeof(scratch), NULL), PBW_ERR_INVALID);
        CHECK_STATUS(pbwe_encode_delimited(NULL, scratch, sizeof(scratch), &consumed),
                     PBW_ERR_INVALID);
        CHECK_STATUS(pbwe_encode_delimited(msg, NULL, 0, &consumed), PBW_ERR_LIMIT);
        CHECK(consumed == 0);

        /* Decoding consumes the prefix plus the frame, so a buffer holding two
         * frames leaves the second one for the next call. */
        memcpy(frame + framed, frame, framed);
        CHECK_OK(pbwe_decode_delimited(&box, "structtest.Scalars3", frame, 2 * framed, &msg,
                                       &consumed));
        CHECK(msg != NULL && consumed == framed);
        CHECK_OK(pbwe_encode(msg, wire, sizeof(wire), &len));
        CHECK(len == ref_len && memcmp(wire, ref_bytes, len) == 0);
        CHECK_OK(pbwe_decode_delimited_desc(&box, desc, frame + framed, framed, &msg, &consumed));
        CHECK(msg != NULL && consumed == framed);
        CHECK_OK(pbwe_encode(msg, wire, sizeof(wire), &len));
        CHECK(len == ref_len && memcmp(wire, ref_bytes, len) == 0);

        /* A body that is one byte short is a truncated frame, not a message. */
        consumed = 99;
        CHECK_STATUS(pbwe_decode_delimited(&box, "structtest.Scalars3", frame, framed - 1, &msg,
                                           &consumed),
                     PBW_ERR_INVALID);
        CHECK(msg == NULL && consumed == 0);
        CHECK_STATUS(pbwe_decode_delimited(&box, "structtest.Scalars3", frame, framed, &msg, NULL),
                     PBW_ERR_INVALID);
        CHECK_STATUS(pbwe_decode_delimited(&box, "structtest.NoSuchMessage", frame, framed, &msg,
                                           &consumed),
                     PBW_ERR_NOT_FOUND);
        CHECK(msg == NULL && consumed == 0);

        /* The declared length is checked against the frame cap before the
         * body is decoded, so an oversized frame fails at the prefix. */
        tight.max_bytes = ref_len - 1;
        tight.max_depth = 0;
        pbwe_set_limits(&box, &tight);
        CHECK_STATUS(pbwe_decode_delimited(&box, "structtest.Scalars3", frame, framed, &msg,
                                           &consumed),
                     PBW_ERR_LIMIT);
        CHECK(msg == NULL && consumed == 0);
        pbwe_set_limits(&box, NULL);
        CHECK_OK(pbwe_decode_delimited(&box, "structtest.Scalars3", frame, framed, &msg,
                                       &consumed));
        CHECK(msg != NULL && consumed == framed);
    }

    /* Unknown type names fail loudly. */
    status = pbwe_decode(&box, "structtest.NoSuchMessage", ref_bytes, ref_len, &msg);
    CHECK_STATUS(status, PBW_ERR_NOT_FOUND);
    CHECK(msg == NULL);

    /* A failed decode clears *msg: its predecessor died in the reset, so it
     * must not come back as a dangling pointer. */
    CHECK_OK(pbwe_decode(&box, "structtest.Scalars3", ref_bytes, ref_len, &msg));
    CHECK(msg != NULL);
    status = pbwe_decode(&box, "structtest.Scalars3", broken, sizeof(broken), &msg);
    CHECK(status != PBW_OK);
    CHECK(msg == NULL);

    /* A byte cap rejects an oversized frame before the arena grows; clearing
     * it restores the core defaults. */
    tight.max_bytes = ref_len - 1;
    tight.max_depth = 0;
    pbwe_set_limits(&box, &tight);
    CHECK_STATUS(pbwe_decode(&box, "structtest.Scalars3", ref_bytes, ref_len, &msg),
                 PBW_ERR_LIMIT);
    CHECK(msg == NULL);
    CHECK_STATUS(pbwe_decode_desc(&box, desc, ref_bytes, ref_len, &msg), PBW_ERR_LIMIT);
    pbwe_set_limits(&box, NULL);
    CHECK_OK(pbwe_decode_desc(&box, desc, ref_bytes, ref_len, &msg));
    CHECK_OK(pbwe_encode(msg, wire, sizeof(wire), &len));
    CHECK(len == ref_len && memcmp(wire, ref_bytes, len) == 0);

    /* The nesting cap binds through the box too: Maps.by_msg nests an entry
     * message (depth 1) holding a MapValue (depth 2). */
    {
        pbwe_box deep;
        uint8_t *map_desc;
        uint8_t *map_bin;
        size_t map_desc_len = 0, map_bin_len = 0;

        memset(&deep, 0, sizeof(deep));
        map_desc = read_file("bindings/c/tests/data/maptest.desc", &map_desc_len);
        map_bin = read_file("bindings/c/tests/data/maptest.bin", &map_bin_len);
        CHECK(map_desc != NULL && map_bin != NULL);
        CHECK_OK(pbwe_open(&deep, map_desc, map_desc_len, 16 * 1024, NULL));
        tight.max_bytes = 0;
        tight.max_depth = 1;
        pbwe_set_limits(&deep, &tight);
        CHECK_STATUS(pbwe_decode(&deep, "structtest.Maps", map_bin, map_bin_len, &msg),
                     PBW_ERR_LIMIT);
        CHECK(msg == NULL);
        tight.max_depth = 2;
        pbwe_set_limits(&deep, &tight);
        CHECK_OK(pbwe_decode(&deep, "structtest.Maps", map_bin, map_bin_len, &msg));
        CHECK(msg != NULL);
        pbwe_close(&deep);
        free(map_desc);
        free(map_bin);
    }

    /* Encode-only firmware builds a frame in the same arena. */
    {
        pbw_message *built = pbwe_build(&box, desc);
        const pbw_value *v;

        CHECK(built != NULL);
        CHECK_OK(pbw_message_set(built, 3, pbw_value_new_uint_ctx(box.ctx, 42)));
        CHECK_OK(pbwe_encode(built, wire, sizeof(wire), &len));
        CHECK_OK(pbwe_decode_desc(&box, desc, wire, len, &msg));
        v = pbw_message_get(msg, 3);
        CHECK(v != NULL && pbw_value_get_uint(v) == 42);
    }
    CHECK(pbwe_build(NULL, desc) == NULL);

    /* pbwe_reset drops the live tree and keeps the box ready to use. */
    CHECK_OK(pbwe_decode_desc(&box, desc, ref_bytes, ref_len, &msg));
    CHECK(pbw_ctx_used(box.ctx) > 0);
    pbwe_reset(&box);
    CHECK(pbw_ctx_used(box.ctx) == 0);
    CHECK_OK(pbwe_decode_desc(&box, desc, ref_bytes, ref_len, &msg));
    CHECK_OK(pbwe_encode(msg, wire, sizeof(wire), &len));
    CHECK(len == ref_len);

    /* An empty frame is a valid empty message, not an error. */
    CHECK_OK(pbwe_decode(&box, "structtest.Scalars3", NULL, 0, &msg));
    CHECK(msg != NULL);
    CHECK_OK(pbwe_encode(msg, wire, sizeof(wire), &len));
    CHECK(len == 0);
    CHECK_OK(pbwe_encode(msg, NULL, 0, &len)); /* an empty frame needs no buffer */
    CHECK(len == 0);

    /* Bad arguments fail loudly. */
    CHECK_STATUS(pbwe_open(NULL, desc_bytes, desc_len, 0, NULL), PBW_ERR_INVALID);
    /* Re-opening a live box would leak it; it must fail and change nothing. */
    CHECK_STATUS(pbwe_open(&box, desc_bytes, desc_len, 0, NULL), PBW_ERR_INVALID);
    CHECK_OK(pbwe_decode(&box, "structtest.Scalars3", ref_bytes, ref_len, &msg));
    CHECK_STATUS(pbwe_decode(NULL, "structtest.Scalars3", ref_bytes, ref_len, &msg),
                 PBW_ERR_INVALID);
    CHECK_STATUS(pbwe_decode(&box, NULL, ref_bytes, ref_len, &msg), PBW_ERR_INVALID);
    CHECK(msg == NULL);
    CHECK_STATUS(pbwe_decode_desc(&box, NULL, ref_bytes, ref_len, &msg), PBW_ERR_INVALID);
    CHECK_STATUS(pbwe_decode(&box, "structtest.Scalars3", NULL, 1, &msg), PBW_ERR_INVALID);
    CHECK(msg == NULL);
    pbwe_set_limits(NULL, &tight); /* a no-op, not a crash */
    pbwe_reset(NULL);
    pbwe_close(NULL);

    /* Steady state: a thousand rounds on one box, both lookup paths. */
#ifdef PBWE_HAVE_ALLOC_HOOKS
    /* Warm up (the first decode allocates the arena chunk), then require
     * exactly zero core allocations for the rest of the loop. */
    CHECK_OK(pbwe_decode(&box, "structtest.Scalars3", ref_bytes, ref_len, &msg));
    CHECK_OK(pbwe_encode(msg, wire, sizeof(wire), &len));
    CHECK(hook_calls > 0); /* the hooks are live: the load above allocated */
    hook_calls = 0;
#endif
    for (i = 0; i < 1000; i++) {
        if ((i % 2) == 0) {
            CHECK_OK(pbwe_decode(&box, "structtest.Scalars3", ref_bytes, ref_len, &msg));
        } else {
            CHECK_OK(pbwe_decode_desc(&box, desc, ref_bytes, ref_len, &msg));
        }
        CHECK_OK(pbwe_encode(msg, wire, sizeof(wire), &len));
        CHECK(len == ref_len);
    }
#ifdef PBWE_HAVE_ALLOC_HOOKS
    CHECK(hook_calls == 0);

    /* Framing costs nothing either: prefix read and write stay on the stack. */
    {
        size_t framed = 0, consumed = 0;

        CHECK_OK(pbwe_encode_delimited(msg, frame, sizeof(frame), &framed));
        CHECK_OK(pbwe_decode_delimited(&box, "structtest.Scalars3", frame, framed, &msg,
                                       &consumed));
        CHECK(consumed == framed);
        CHECK(hook_calls == 0);
    }

    /* The zero above is the cap at work: a box that retains one byte keeps
     * nothing across a reset, so each decode allocates again. */
    {
        pbwe_box slim;

        memset(&slim, 0, sizeof(slim));
        CHECK_OK(pbwe_open(&slim, desc_bytes, desc_len, 1, NULL));
        hook_calls = 0;
        CHECK_OK(pbwe_decode(&slim, "structtest.Scalars3", ref_bytes, ref_len, &msg));
        CHECK_OK(pbwe_decode(&slim, "structtest.Scalars3", ref_bytes, ref_len, &msg));
        CHECK(hook_calls > 0);
        pbwe_close(&slim);
    }
#endif

    /* A box that was never opened (the same state a closed one leaves) fails
     * cleanly instead of touching a NULL context, and is openable. */
    {
        pbwe_box closed;

        memset(&closed, 0, sizeof(closed));
        CHECK_STATUS(pbwe_decode(&closed, "structtest.Scalars3", ref_bytes, ref_len, &msg),
                     PBW_ERR_INVALID);
        CHECK(msg == NULL);
        CHECK(pbwe_build(&closed, desc) == NULL);
        pbwe_reset(&closed); /* a no-op on a closed box */
        CHECK_OK(pbwe_open(&closed, desc_bytes, desc_len, 64 * 1024, NULL));
        pbwe_close(&closed);
    }

    pbwe_close(&box);
    CHECK(box.schema == NULL && box.ctx == NULL);
    pbwe_close(&box); /* idempotent: closing a closed box is a no-op */

    /* The closed box refuses work rather than dereferencing freed pointers,
     * and it can be opened again. */
    CHECK_STATUS(pbwe_decode(&box, "structtest.Scalars3", ref_bytes, ref_len, &msg),
                 PBW_ERR_INVALID);
    CHECK(msg == NULL);
    pbwe_reset(&box); /* a no-op on a closed box */
    CHECK_OK(pbwe_open(&box, desc_bytes, desc_len, 64 * 1024, NULL));
    pbwe_close(&box);

    /* A set that cannot be loaded leaves the partial schema behind for
     * pbw_schema_error(), and closing the box frees it. */
    {
        pbwe_box bad_box;

        memset(&bad_box, 0, sizeof(bad_box));
        CHECK_STATUS(pbwe_open(&bad_box, broken, sizeof(broken), 0, NULL), PBW_ERR_INVALID);
        /* The box still owns the partial schema, so reopening it is refused
         * until it is closed (that is what keeps the failure leak-free). */
        CHECK_STATUS(pbwe_decode(&bad_box, "structtest.Scalars3", ref_bytes, ref_len, &msg),
                     PBW_ERR_INVALID);
        CHECK_STATUS(pbwe_open(&bad_box, desc_bytes, desc_len, 0, NULL), PBW_ERR_INVALID);
        pbwe_close(&bad_box);
        CHECK(bad_box.schema == NULL && bad_box.ctx == NULL);
    }

    /* A descriptor set past its startup cap fails before it is parsed, and
     * the box still explains why. */
    {
        pbwe_box capped;
        pbw_limits desc_limits;

        memset(&capped, 0, sizeof(capped));
        desc_limits.max_bytes = desc_len - 1;
        desc_limits.max_depth = 0;
        CHECK_STATUS(pbwe_open(&capped, desc_bytes, desc_len, 0, &desc_limits),
                     PBW_ERR_LIMIT);
        CHECK(capped.schema != NULL && capped.ctx == NULL);
        CHECK(capped.schema != NULL && pbw_schema_error(capped.schema)[0] != '\0');
        pbwe_close(&capped);
    }

    /* A rejected call leaves a zeroed box closed instead of half-open. */
    {
        pbwe_box empty;

        memset(&empty, 0, sizeof(empty));
        CHECK_STATUS(pbwe_open(&empty, desc_bytes, 0, 0, NULL), PBW_ERR_INVALID);
        CHECK(empty.schema == NULL && empty.ctx == NULL);
    }

    free(desc_bytes);
    free(ref_bytes);

    if (failures == 0) {
        printf("%s: all checks passed\n", label);
        return 0;
    }
    printf("%s: %d checks failed\n", label, failures);
    return 1;
}

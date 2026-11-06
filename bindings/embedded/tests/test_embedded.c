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
    size_t n;

    CHECK(f != NULL);
    if (f == NULL) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    n = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
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

int
main(void)
{
    static uint8_t wire[64 * 1024];
    uint8_t *desc_bytes;
    uint8_t *ref_bytes;
    size_t desc_len = 0, ref_len = 0;
    pbwe_box box;
    const pbw_message *msg = NULL;
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
    CHECK_OK(pbwe_open(&box, desc_bytes, desc_len, 64 * 1024));

    /* Decode the pinned oracle, encode it back into caller memory. */
    CHECK_OK(pbwe_decode(&box, "structtest.Scalars3", ref_bytes, ref_len, &msg));
    CHECK(msg != NULL);
    CHECK_OK(pbwe_encode(msg, wire, sizeof(wire), &len));
    CHECK(len == ref_len);
    CHECK(memcmp(wire, ref_bytes, ref_len) == 0);

    /* A buffer that is one byte short fails instead of growing. */
    CHECK_STATUS(pbwe_encode(msg, wire, len - 1, &len), PBW_ERR_LIMIT);

    /* Unknown type names fail loudly. */
    CHECK_STATUS(pbwe_decode(&box, "structtest.NoSuchMessage", ref_bytes, ref_len, &msg),
                 PBW_ERR_NOT_FOUND);

    /* Bad arguments fail loudly. */
    CHECK_STATUS(pbwe_open(NULL, desc_bytes, desc_len, 0), PBW_ERR_INVALID);
    CHECK_STATUS(pbwe_decode(NULL, "structtest.Scalars3", ref_bytes, ref_len, &msg),
                 PBW_ERR_INVALID);
    CHECK_STATUS(pbwe_decode(&box, NULL, ref_bytes, ref_len, &msg), PBW_ERR_INVALID);
    pbwe_reset(NULL);
    pbwe_close(NULL);

    /* Steady state: a thousand decode/encode rounds on one box. */
    for (i = 0; i < 1000; i++) {
        CHECK_OK(pbwe_decode(&box, "structtest.Scalars3", ref_bytes, ref_len, &msg));
        CHECK_OK(pbwe_encode(msg, wire, sizeof(wire), &len));
        CHECK(len == ref_len);
    }

    pbwe_close(&box);
    free(desc_bytes);
    free(ref_bytes);

    if (failures == 0) {
        printf("test_embedded: all checks passed\n");
        return 0;
    }
    printf("test_embedded: %d checks failed\n", failures);
    return 1;
}

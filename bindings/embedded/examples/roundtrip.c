/* Embedded round trip: decode the pinned matrix3 bytes with a static box,
 * print one field, encode the tree back into a caller buffer. */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pbwe.h"

static uint8_t *
read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    uint8_t *buf;
    size_t n;

    if (f == NULL) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    n = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = (uint8_t *)malloc(n > 0 ? n : 1);
    if (buf == NULL || fread(buf, 1, n, f) != n) {
        free(buf);
        fclose(f);
        return NULL;
    }
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
    size_t desc_len = 0, ref_len = 0, len = 0;
    pbwe_box box;
    const pbw_message *msg = NULL;
    const pbw_value *v;

    memset(&box, 0, sizeof(box));
    desc_bytes = read_file("bindings/c/tests/data/matrix3.desc", &desc_len);
    ref_bytes = read_file("bindings/c/tests/data/matrix3.bin", &ref_len);
    if (desc_bytes == NULL || ref_bytes == NULL) {
        fprintf(stderr, "run from the repo root\n");
        return 1;
    }
    if (pbwe_open(&box, desc_bytes, desc_len, 64 * 1024) != PBW_OK ||
        pbwe_decode(&box, "structtest.Scalars3", ref_bytes, ref_len, &msg) != PBW_OK) {
        fprintf(stderr, "decode failed\n");
        return 1;
    }
    v = pbw_message_get(msg, 1);
    printf("i32 = %lld\n", v != NULL ? (long long)pbw_value_get_int(v) : -1);
    if (pbwe_encode(msg, wire, sizeof(wire), &len) != PBW_OK || len != ref_len) {
        fprintf(stderr, "encode failed\n");
        return 1;
    }
    printf("round trip ok: %zu bytes\n", len);
    pbwe_close(&box);
    free(desc_bytes);
    free(ref_bytes);
    return 0;
}

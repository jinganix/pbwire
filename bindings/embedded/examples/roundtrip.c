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
    long size;

    if (f == NULL) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 0 ||
        fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    buf = (uint8_t *)malloc(size > 0 ? (size_t)size : 1);
    if (buf == NULL || fread(buf, 1, (size_t)size, f) != (size_t)size) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *len = (size_t)size;
    return buf;
}

int
main(void)
{
    static uint8_t wire[64 * 1024];
    uint8_t *desc_bytes = NULL;
    uint8_t *ref_bytes = NULL;
    size_t desc_len = 0, ref_len = 0, len = 0;
    pbwe_box box;
    const pbw_message *msg = NULL;
    const pbw_value *v;
    int rc = 1;

    memset(&box, 0, sizeof(box));
    desc_bytes = read_file("bindings/c/tests/data/matrix3.desc", &desc_len);
    ref_bytes = read_file("bindings/c/tests/data/matrix3.bin", &ref_len);
    if (desc_bytes == NULL || ref_bytes == NULL) {
        fprintf(stderr, "run from the repo root\n");
        goto done;
    }
    if (pbwe_open(&box, desc_bytes, desc_len, 64 * 1024, NULL) != PBW_OK) {
        /* The box keeps the partial schema, so it can say why. */
        fprintf(stderr, "open failed: %s\n", pbw_schema_error(box.schema));
        goto done;
    }
    if (pbwe_decode(&box, "structtest.Scalars3", ref_bytes, ref_len, &msg) != PBW_OK) {
        fprintf(stderr, "decode failed\n");
        goto done;
    }
    v = pbw_message_get(msg, 1);
    printf("i32 = %lld\n", v != NULL ? (long long)pbw_value_get_int(v) : -1);
    if (pbwe_encode(msg, wire, sizeof(wire), &len) != PBW_OK || len != ref_len) {
        fprintf(stderr, "encode failed\n");
        goto done;
    }
    printf("round trip ok: %zu bytes\n", len);
    rc = 0;

done:
    pbwe_close(&box);
    free(desc_bytes);
    free(ref_bytes);
    return rc;
}

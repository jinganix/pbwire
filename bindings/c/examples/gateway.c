/* Gateway example: dynamic API -> wire bytes -> JSON -> forward untouched.
 *
 * A protocol gateway inspects a message for a schema it only partly knows:
 * it reads the fields it understands, prints JSON for the log, and forwards
 * the bytes with everything it never taught about preserved. This program
 * does that loop with the dynamic API (no structs, no generated code):
 *
 *   1. load examples/gateway.desc at runtime, find shop.Order;
 *   2. build an Order (id, one Item, one attr, status PLACED);
 *   3. encode, then append one unknown field (99: varint 7), as a newer
 *      producer's bytes would carry;
 *   4. decode: known fields read back, the unknown field is kept as raw
 *      bytes (pbw_message_unknown_len);
 *   5. print JSON (unknowns are inspection-only, never printed);
 *   6. re-encode: the output is the known fields plus the unknown bytes,
 *      so forwarding is lossless.
 *
 * Build and run: `make examples` (from the repo root, also in `make test`).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pbwire.h"

#define CHECK(cond)                                                \
    do {                                                           \
        if (!(cond)) {                                             \
            fprintf(stderr, "gateway: CHECK failed: %s\n", #cond); \
            return 1;                                              \
        }                                                          \
    } while (0)

static uint8_t *
read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    uint8_t *buf;
    long n;

    if (f == NULL) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    n = ftell(f);
    if (n < 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    buf = (uint8_t *)malloc((size_t)n ? (size_t)n : 1);
    if (buf == NULL) {
        fclose(f);
        return NULL;
    }
    if (n > 0 && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(buf);
        return NULL;
    }
    fclose(f);
    *len = (size_t)n;
    return buf;
}

int
main(void)
{
    uint8_t *desc_bytes = NULL;
    size_t desc_len = 0;
    pbw_schema *schema = NULL;
    const pbw_descriptor *order_desc;
    const pbw_descriptor *item_desc;
    pbw_message *order = NULL;
    pbw_message *item = NULL;
    pbw_message *got = NULL;
    uint8_t *wire = NULL;
    size_t wire_len = 0;
    uint8_t *fwd = NULL; /* producer bytes: wire + one unknown field */
    size_t fwd_len = 0;
    uint8_t *out = NULL;
    size_t out_len = 0;
    char *json = NULL;
    size_t json_len = 0;
    /* field 99, varint 7: tag 0x98 0x06 (two-byte varint), value 0x07 */
    static const uint8_t unknown[] = {0x98, 0x06, 0x07};

    desc_bytes = read_file("examples/gateway.desc", &desc_len);
    CHECK(desc_bytes != NULL);
    CHECK(pbw_descriptor_set_load(desc_bytes, desc_len, &schema) == PBW_OK);
    free(desc_bytes);

    order_desc = pbw_schema_find(schema, "shop.Order");
    item_desc = pbw_schema_find(schema, "shop.Item");
    CHECK(order_desc != NULL && item_desc != NULL);

    order = pbw_message_new(order_desc);
    CHECK(order != NULL);
    CHECK(pbw_message_set(order, 1, pbw_value_new_string("ord-1")) == PBW_OK);
    item = pbw_message_new(item_desc);
    CHECK(item != NULL);
    CHECK(pbw_message_set(item, 1, pbw_value_new_string("sku-9")) == PBW_OK);
    CHECK(pbw_message_set(item, 2, pbw_value_new_int(3)) == PBW_OK);
    CHECK(pbw_message_set(order, 2, pbw_value_new_message(item)) == PBW_OK);
    CHECK(pbw_message_set(order, 4, pbw_value_new_int(1)) == PBW_OK);
    CHECK(pbw_encode(order, &wire, &wire_len) == PBW_OK);

    /* a newer producer's bytes: our encoding plus a field we never knew */
    fwd_len = wire_len + sizeof(unknown);
    fwd = (uint8_t *)malloc(fwd_len);
    CHECK(fwd != NULL);
    memcpy(fwd, wire, wire_len);
    memcpy(fwd + wire_len, unknown, sizeof(unknown));

    CHECK(pbw_decode(order_desc, fwd, fwd_len, &got) == PBW_OK);
    /* known fields read back through the old schema */
    CHECK(strcmp(pbw_value_get_string(pbw_message_get(got, 1)), "ord-1") == 0);
    /* the unknown field survived decoding instead of being dropped */
    CHECK(pbw_message_unknown_len(got) == sizeof(unknown));
    CHECK(memcmp(pbw_message_unknown_bytes(got), unknown, sizeof(unknown)) ==
          0);

    /* JSON is the inspection surface: known fields only, never unknowns */
    CHECK(pbw_json_print(got, &json, &json_len) == PBW_OK);
    CHECK(strstr(json, "ord-1") != NULL);

    /* forwarding is lossless: re-encode carries the unknown bytes along */
    CHECK(pbw_encode(got, &out, &out_len) == PBW_OK);
    CHECK(out_len == fwd_len);
    CHECK(memcmp(out + out_len - sizeof(unknown), unknown, sizeof(unknown)) ==
          0);

    printf("gateway ok: %zu bytes in, %zu bytes out, %zu unknown preserved\n",
           fwd_len, out_len, pbw_message_unknown_len(got));

    pbw_free(json);
    pbw_free(out);
    pbw_free(wire);
    free(fwd);
    pbw_message_free(got);
    pbw_message_free(order);
    pbw_schema_free(schema);
    return 0;
}

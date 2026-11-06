/* Stdin fuzz harness: raw wire bytes in, verdicts out. No schema needed for
 * the scan; decoding uses a small built-in proto3 tree (one message with a
 * submessage, strings, packed fields and an enum) so group/message nesting,
 * packed runs and UTF-8-touched paths are all reachable from stdin bytes.
 *
 * Usage:
 *   ./build/fuzz_decode < corpus.bin        # one input
 *   make fuzz                               # fixed corpus + mutation burst
 *
 * Exit 0 unless an ASan/UBSan trap fires: any nonzero status or crash is a
 * bug. Output is a one-line verdict summary for corpus bookkeeping.
 */

#include "pbwire.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static pbw_schema *g_schema;
static const pbw_descriptor *g_msg;

static void
build_schema(void)
{
    pbw_descriptor *msg;
    pbw_descriptor *sub;
    pbw_descriptor *en;
    pbw_field *sf;
    pbw_field *ef;
    pbw_field *rf;
    /* NOTE: set each type name right after its add: the field array may
     * move on the next add, so no field pointer survives another add. */

    g_schema = pbw_schema_new();
    if (g_schema == NULL) {
        exit(1);
    }
    msg = pbw_schema_add_message(g_schema, "Fuzz");
    sub = pbw_schema_add_message(g_schema, "FuzzSub");
    en = pbw_schema_add_enum(g_schema, "FuzzEnum");
    pbw_enum_add_value(en, "ZERO", 0);
    pbw_enum_add_value(en, "ONE", 1);
    pbw_message_add_field(msg, "i", 1, PBW_TYPE_INT32, PBW_LABEL_OPTIONAL);
    pbw_message_add_field(msg, "s", 2, PBW_TYPE_STRING, PBW_LABEL_OPTIONAL);
    pbw_message_add_field(msg, "b", 3, PBW_TYPE_BYTES, PBW_LABEL_OPTIONAL);
    pbw_message_add_field(msg, "rep", 4, PBW_TYPE_INT32, PBW_LABEL_REPEATED);
    sf = pbw_message_add_field(msg, "sub", 5, PBW_TYPE_MESSAGE,
                               PBW_LABEL_OPTIONAL);
    pbw_field_set_type_name(sf, "FuzzSub");
    pbw_message_add_field(sub, "x", 1, PBW_TYPE_SINT64, PBW_LABEL_OPTIONAL);
    pbw_message_add_field(sub, "d", 2, PBW_TYPE_DOUBLE, PBW_LABEL_OPTIONAL);
    ef = pbw_message_add_field(msg, "e", 6, PBW_TYPE_ENUM, PBW_LABEL_OPTIONAL);
    pbw_field_set_type_name(ef, "FuzzEnum");
    rf = pbw_message_add_field(msg, "subs", 7, PBW_TYPE_MESSAGE,
                               PBW_LABEL_REPEATED);
    pbw_field_set_type_name(rf, "FuzzSub");
    pbw_message_add_field(msg, "m", 8, PBW_TYPE_STRING, PBW_LABEL_REPEATED);
    if (pbw_schema_link(g_schema) != PBW_OK) {
        exit(1);
    }
    g_msg = pbw_schema_find(g_schema, "Fuzz");
}

static pbw_status
visit_cb(void *ud, const pbw_field *field, uint32_t number,
         uint32_t wire_type, const pbw_visit_value *value)
{
    (void)ud;
    (void)field;
    (void)number;
    (void)wire_type;
    (void)value;
    return PBW_OK;
}

/* One frame: process a single input through every entry path. */
static void
run_frame(const uint8_t *buf, size_t len, unsigned frame)
{
    pbw_message *msg = NULL;
    pbw_status st;
    size_t fail_off = 0;

    printf("[%u] ", frame);
    /* path 1: validating scan, no allocation */
    st = pbw_visit(g_msg, buf, len, visit_cb, NULL, &fail_off);
    printf("visit=%d ", (int)st);
    /* path 2: full decode + re-encode + JSON/text print */
    st = pbw_decode(g_msg, buf, len, &msg);
    printf("decode=%d ", (int)st);
    if (st == PBW_OK) {
        uint8_t *out = NULL;
        size_t out_len = 0;
        char *text = NULL;
        size_t text_len = 0;
        pbw_limits lim = {0, 0};

        if (pbw_encode(msg, &out, &out_len) == PBW_OK) {
            printf("encode=%zu ", out_len);
            pbw_free(out);
        } else {
            printf("encode=ERR ");
        }
        if (pbw_json_print(msg, &text, &text_len) == PBW_OK) {
            printf("json=%zu ", text_len);
            pbw_free(text);
            text = NULL;
        } else {
            printf("json=ERR ");
        }
        if (pbw_text_print(msg, &text, &text_len) == PBW_OK) {
            printf("text=%zu ", text_len);
            pbw_free(text);
        } else {
            printf("text=ERR ");
        }
        /* path 3: bounded decode agrees on the verdict class */
        {
            pbw_message *m2 = NULL;
            pbw_status st2 =
                pbw_decode_limits(g_msg, buf, len, &lim, &m2);
            if ((st2 == PBW_OK) != 1) {
                printf("LIMIT-MISMATCH ");
            } else {
                pbw_message_free(m2);
            }
        }
        pbw_message_free(msg);
    }
    printf("len=%zu\n", len);
}

/* Stdin is a run of frames: [u16 LE length][payload]*. A bare payload
 * without a length header is one frame, so `... < raw.bin` keeps working. */
int
main(void)
{
    static uint8_t input[1 << 20];
    size_t total = fread(input, 1, sizeof(input), stdin);
    size_t pos = 0;
    unsigned frame = 0;

    build_schema();
    if (total >= 2) {
        size_t probe = (size_t)input[0] | ((size_t)input[1] << 8);
        if (probe + 2 <= total) {
            while (pos + 2 <= total && frame < 4096) {
                size_t len =
                    (size_t)input[pos] | ((size_t)input[pos + 1] << 8);
                pos += 2;
                if (len > total - pos) {
                    break; /* truncated tail: not a bug, stop */
                }
                run_frame(input + pos, len, frame++);
                pos += len;
            }
            printf("frames=%u\n", frame);
            pbw_schema_free(g_schema);
            return 0;
        }
    }
    run_frame(input, total, 0);
    pbw_schema_free(g_schema);
    return 0;
}

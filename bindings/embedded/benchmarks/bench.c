/* Embedded tier vs nanopb: the same message (msg.proto), the same bytes, the
 * same loop shape on both sides. pbwire runs the embedded facade (static
 * schema, caller-memory sized arena); nanopb runs its generated static struct.
 *
 * `make embedded-bench` generates the descriptor set and the nanopb code, then
 * builds and runs this. Not part of `make test`: it needs protoc and a nanopb
 * checkout (`.tmp/nanopb-*`, `make nanopb-setup`).
 *
 * Decode on the nanopb side zeroes the struct first (a decode needs a fresh
 * message) and pbwire's decode resets its arena first; that reset is part of
 * both numbers, because it is work every caller does per message.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "pbwe.h"

#include <pb_encode.h>
#include <pb_decode.h>

#include "msg.pb.h" /* nanopb generated: eb_Reading + eb_Reading_fields */

static volatile long sink;

static double
now_ns(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e9 + (double)t.tv_nsec;
}

static void
report(const char *impl, const char *op, double ns, size_t bytes)
{
    double mbps = ns > 0 ? (double)bytes * 1000.0 / ns : 0.0;

    printf("  %-8s %-6s %9.2f ns/op %9.1f MB/s\n", impl, op, ns, mbps);
}

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

/* Fill both sides with the same non-default values (proto3 implicit presence:
 * a zero would make the two encoders disagree on whether to write the field). */
static const char label[] = "sensor-a";
static const uint8_t raw[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};

static void
fill_tree(pbw_ctx *ctx, pbw_message *msg, int samples)
{
    float temp = 21.5f;
    double pressure = 1013.25;
    uint64_t temp_bits;
    uint64_t pressure_bits;
    int i;

    memcpy(&temp_bits, &temp, sizeof(temp_bits));
    memcpy(&pressure_bits, &pressure, sizeof(pressure_bits));
    pbw_message_set(msg, 1, pbw_value_new_uint_ctx(ctx, 42));
    pbw_message_set(msg, 2, pbw_value_new_uint_ctx(ctx, 1700000000));
    pbw_message_set(msg, 3, pbw_value_new_uint_ctx(ctx, temp_bits));
    pbw_message_set(msg, 4, pbw_value_new_uint_ctx(ctx, pressure_bits));
    pbw_message_set(msg, 5, pbw_value_new_uint_ctx(ctx, 1));
    pbw_message_set(msg, 6, pbw_value_new_bytes_ctx(ctx, label, sizeof(label) - 1));
    pbw_message_set(msg, 7, pbw_value_new_bytes_ctx(ctx, raw, sizeof(raw)));
    for (i = 0; i < samples; i++) {
        pbw_message_set(msg, 8, pbw_value_new_uint_ctx(ctx, (uint64_t)(i + 1)));
    }
}

static void
fill_struct(eb_Reading *s, int samples)
{
    int i;

    memset(s, 0, sizeof(*s));
    s->id = 42;
    s->ts = 1700000000u;
    s->temp = 21.5f;
    s->pressure = 1013.25;
    s->ok = true;
    snprintf(s->label, sizeof(s->label), "%s", label);
    s->raw.size = sizeof(raw);
    memcpy(s->raw.bytes, raw, sizeof(raw));
    s->samples_count = (pb_size_t)samples;
    for (i = 0; i < samples; i++) {
        s->samples[i] = i + 1;
    }
}

static int
run_case(pbwe_box *box, const pbw_descriptor *desc, int samples, int iters)
{
    static uint8_t wire_pb[64 * 1024];
    static uint8_t wire_nb[64 * 1024];
    static eb_Reading got;
    eb_Reading nb;
    pbw_message *built;
    const pbw_message *msg = NULL;
    size_t wire_pb_len = 0, wire_nb_len = 0, len = 0;
    double t0, t1;
    int i;
    int equal;

    built = pbwe_build(box, desc);
    if (built == NULL) {
        fprintf(stderr, "build failed\n");
        return 0;
    }
    fill_tree(box->ctx, built, samples);
    if (pbwe_encode(built, wire_pb, sizeof(wire_pb), &wire_pb_len) != PBW_OK) {
        fprintf(stderr, "pbwire encode failed\n");
        return 0;
    }

    fill_struct(&nb, samples);
    {
        pb_ostream_t os = pb_ostream_from_buffer(wire_nb, sizeof(wire_nb));

        if (!pb_encode(&os, eb_Reading_fields, &nb)) {
            fprintf(stderr, "nanopb encode failed: %s\n", PB_GET_ERROR(&os));
            return 0;
        }
        wire_nb_len = os.bytes_written;
    }
    equal = wire_pb_len == wire_nb_len &&
            memcmp(wire_pb, wire_nb, wire_pb_len) == 0;
    printf("samples x%d: %zu bytes (pbwire == nanopb: %s)\n", samples, wire_pb_len,
           equal ? "yes" : "NO");

    /* ---- encode ---- */
    t0 = now_ns();
    for (i = 0; i < iters; i++) {
        if (pbwe_encode(built, wire_pb, sizeof(wire_pb), &len) != PBW_OK) {
            return 0;
        }
        sink += (long)len;
    }
    t1 = now_ns();
    report("pbwire", "encode", (t1 - t0) / iters, wire_pb_len);

    t0 = now_ns();
    for (i = 0; i < iters; i++) {
        pb_ostream_t os = pb_ostream_from_buffer(wire_nb, sizeof(wire_nb));

        if (!pb_encode(&os, eb_Reading_fields, &nb)) {
            return 0;
        }
        sink += (long)os.bytes_written;
    }
    t1 = now_ns();
    report("nanopb", "encode", (t1 - t0) / iters, wire_nb_len);

    /* ---- decode ---- */
    t0 = now_ns();
    for (i = 0; i < iters; i++) {
        if (pbwe_decode_desc(box, desc, wire_pb, wire_pb_len, &msg) != PBW_OK) {
            return 0;
        }
        sink += (long)pbw_value_count(pbw_message_get(msg, 8));
    }
    t1 = now_ns();
    report("pbwire", "decode", (t1 - t0) / iters, wire_pb_len);

    t0 = now_ns();
    for (i = 0; i < iters; i++) {
        pb_istream_t is = pb_istream_from_buffer(wire_nb, wire_nb_len);

        memset(&got, 0, sizeof(got));
        if (!pb_decode(&is, eb_Reading_fields, &got)) {
            return 0;
        }
        sink += (long)got.samples_count;
    }
    t1 = now_ns();
    report("nanopb", "decode", (t1 - t0) / iters, wire_nb_len);
    return equal;
}

int
main(int argc, char **argv)
{
    static uint8_t pool[512 * 1024 + 8];
    void *block = (void *)(((uintptr_t)pool + 7u) & ~(uintptr_t)7u);
    uint8_t *desc_bytes;
    size_t desc_len = 0;
    pbwe_box box;
    const pbw_descriptor *desc;
    int ok = 1;

    if (argc != 2) {
        fprintf(stderr, "usage: %s msg.desc\n", argv[0]);
        return 2;
    }
    desc_bytes = read_file(argv[1], &desc_len);
    if (desc_bytes == NULL) {
        fprintf(stderr, "cannot read %s\n", argv[1]);
        return 2;
    }
    memset(&box, 0, sizeof(box));
    if (pbwe_open_mem(&box, desc_bytes, desc_len, block, sizeof(pool) - 8, NULL) !=
        PBW_OK) {
        fprintf(stderr, "pbwe_open_mem failed: %s\n", pbw_schema_error(box.schema));
        free(desc_bytes);
        return 2;
    }
    desc = pbw_schema_find(box.schema, "eb.Reading");
    if (desc == NULL) {
        fprintf(stderr, "eb.Reading not in the schema\n");
        pbwe_close(&box);
        free(desc_bytes);
        return 2;
    }

    ok &= run_case(&box, desc, 0, 200000);
    ok &= run_case(&box, desc, 1, 200000);
    ok &= run_case(&box, desc, 100, 50000);
    ok &= run_case(&box, desc, 3000, 2000);

    printf("checksum: %ld\n", sink);
    pbwe_close(&box);
    free(desc_bytes);
    return ok ? 0 : 1;
}

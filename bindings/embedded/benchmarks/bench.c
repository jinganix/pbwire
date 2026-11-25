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
 *
 * Arms: `pbwire` is the facade `tree` arm -- build + fill + encode fresh per
 * iteration inside the timer (the full task). `pbwire-proxy` re-encodes the
 * held tree instead: same-run timing only, never the headline.
 * `pbwire-struct` is the `shim` arm (the struct codec reading caller memory
 * in place through layout tables): fresh struct fill + codec encode per
 * iteration, decode into a caller struct + caller-memory arena
 * (`pbw_struct_arena_new_mem`, zero per-decode allocation).
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "pbwe.h"
#include "pbw_struct.h"

#include <pb_encode.h>
#include <pb_decode.h>

#include "msg.pb.h"  /* nanopb generated: eb_Reading + eb_Reading_fields */
#include "gen/msg.h" /* pbwire generated: eb_reading_t + eb_reading_fields */

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

/* Same values into the pbwire generated struct (struct-codec `shim` arm).
 * Backing lives in static storage: encode borrows the struct, so the
 * pointers only have to outlive the call. */
static int32_t s_samples[3000];
static uint8_t s_raw[sizeof(raw)];

static void
fill_sreading(eb_reading_t *s, int samples)
{
    int i;

    memset(s, 0, sizeof(*s));
    s->id = 42;
    s->ts = 1700000000u;
    s->temp = 21.5f;
    s->pressure = 1013.25;
    s->ok = true;
    s->label = label;
    memcpy(s_raw, raw, sizeof(raw));
    s->raw.data = s_raw;
    s->raw.len = sizeof(raw);
    s->samples = s_samples;
    s->n_samples = (size_t)samples;
    for (i = 0; i < samples; i++) {
        s_samples[i] = i + 1;
    }
}

static int
run_case(pbwe_box *box, const pbw_descriptor *desc, pbw_struct_codec *codec,
         pbw_struct_arena *sarena, int samples, int iters)
{
    static uint8_t wire_pb[64 * 1024];
    static uint8_t wire_nb[64 * 1024];
    static eb_Reading got;
    static pbw_struct_buf sbuf; /* zero-initialized: keeps capacity across tiers */
    eb_Reading nb;
    eb_reading_t ss;
    eb_reading_t ds;
    pbw_message *built;
    const pbw_message *msg = NULL;
    size_t wire_pb_len = 0, wire_nb_len = 0, len = 0;
    size_t want_bytes =
        samples == 0 ? 52 : samples == 1 ? 55
                        : samples == 100 ? 154
                                         : 5928;
    double t0, t1;
    int i;
    int equal;
    int struct_equal;

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
    if (!equal)
        return 0;

    /* Field-by-field gate: identical bytes must also decode to the values
     * both sides filled (byte parity alone would not catch a dropped field
     * that both sides drop). */
    {
        const pbw_message *check = NULL;
        const pbw_value *id = NULL;
        const pbw_value *lbl = NULL;
        const pbw_value *samp = NULL;
        const void *lbl_bytes = NULL;
        size_t lbl_len = 0;
        int ok_fields = 0;

        if (pbwe_decode_desc(box, desc, wire_pb, wire_pb_len, &check) != PBW_OK) {
            fprintf(stderr, "gate decode failed\n");
            return 0;
        }
        id = pbw_message_get(check, 1);
        lbl = pbw_message_get(check, 6);
        samp = pbw_message_get(check, 8);
        if (lbl != NULL)
            lbl_bytes = pbw_value_get_bytes(lbl, &lbl_len);
        ok_fields = id != NULL && pbw_value_get_uint(id) == 42 &&
                    lbl_bytes != NULL && lbl_len == sizeof(label) - 1 &&
                    memcmp(lbl_bytes, label, lbl_len) == 0 &&
                    (samp == NULL ? 0 : (int)pbw_value_count(samp)) == samples;
        if (!ok_fields) {
            fprintf(stderr, "field gate failed\n");
            return 0;
        }
    }

    /* ---- encode (fresh tree per iteration: build + fill + encode) ---- */
    t0 = now_ns();
    for (i = 0; i < iters; i++) {
        built = pbwe_build(box, desc);
        if (built == NULL) {
            return 0;
        }
        fill_tree(box->ctx, built, samples);
        if (pbwe_encode(built, wire_pb, sizeof(wire_pb), &len) != PBW_OK) {
            return 0;
        }
        sink += (long)len;
    }
    t1 = now_ns();
    report("pbwire", "encode", (t1 - t0) / iters, wire_pb_len);

    /* ---- encode (proxy: the old held-tree re-encode, relabelled) ---- */
    t0 = now_ns();
    for (i = 0; i < iters; i++) {
        if (pbwe_encode(built, wire_pb, sizeof(wire_pb), &len) != PBW_OK) {
            return 0;
        }
        sink += (long)len;
    }
    t1 = now_ns();
    report("pbwire-proxy", "encode", (t1 - t0) / iters, wire_pb_len);

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

    /* ---- struct-codec gate: same values via the generated struct ---- */
    fill_sreading(&ss, samples);
    if (pbw_struct_codec_encode(codec, &ss, &sbuf) != PBW_OK) {
        fprintf(stderr, "struct encode failed\n");
        return 0;
    }
    struct_equal = wire_nb_len == want_bytes && sbuf.len == wire_nb_len &&
                   memcmp(sbuf.data, wire_nb, wire_nb_len) == 0;
    printf("samples x%d: %zu bytes (pbwire-struct == nanopb: %s)\n", samples,
           sbuf.len, struct_equal ? "yes" : "NO");
    if (!struct_equal)
        return 0;
    /* Decode into a caller struct + caller-memory arena, then re-encode:
     * the bytes must survive the round trip (no free: the next decode
     * resets the arena). */
    memset(&ds, 0, sizeof(ds));
    {
        pbw_struct_arena *a = sarena;
        const char *lbl = NULL;

        if (pbw_struct_codec_decode(codec, wire_nb, wire_nb_len, &a, &ds) !=
            PBW_OK) {
            fprintf(stderr, "struct decode failed\n");
            return 0;
        }
        lbl = ds.label;
        if (ds.id != 42 || lbl == NULL || strcmp(lbl, label) != 0 ||
            ds.n_samples != (size_t)samples ||
            (samples > 0 && ds.samples[ds.n_samples - 1] != samples)) {
            fprintf(stderr, "struct field gate failed\n");
            return 0;
        }
        if (pbw_struct_codec_encode(codec, &ds, &sbuf) != PBW_OK ||
            sbuf.len != wire_nb_len ||
            memcmp(sbuf.data, wire_nb, wire_nb_len) != 0) {
            fprintf(stderr, "struct re-encode mismatch\n");
            return 0;
        }
    }

    /* ---- encode (fresh struct fill + codec encode per iteration) ---- */
    t0 = now_ns();
    for (i = 0; i < iters; i++) {
        fill_sreading(&ss, samples);
        if (pbw_struct_codec_encode(codec, &ss, &sbuf) != PBW_OK) {
            return 0;
        }
        sink += (long)sbuf.len;
    }
    t1 = now_ns();
    report("pbwire-struct", "encode", (t1 - t0) / iters, sbuf.len);

    /* ---- decode (into a caller struct + caller-memory arena) ---- */
    t0 = now_ns();
    for (i = 0; i < iters; i++) {
        pbw_struct_arena *a = sarena;

        memset(&ds, 0, sizeof(ds));
        /* Same buffer the gate validated (struct bytes == wire_nb, decode
         * of wire_nb re-encodes byte-identical): the parity claim is strict. */
        if (pbw_struct_codec_decode(codec, wire_nb, wire_nb_len, &a, &ds) !=
            PBW_OK) {
            return 0;
        }
        sink += (long)ds.n_samples;
    }
    t1 = now_ns();
    report("pbwire-struct", "decode", (t1 - t0) / iters, wire_nb_len);

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
    /* Peak of one message plus header: the x3000 decode grows its repeated
     * array by doubling (~2x the 12 KB final), so 32 KB does not fit. */
    static uint8_t smem[64 * 1024 + 8];
    void *block = (void *)(((uintptr_t)pool + 7u) & ~(uintptr_t)7u);
    void *sblock = (void *)(((uintptr_t)smem + 7u) & ~(uintptr_t)7u);
    uint8_t *desc_bytes;
    size_t desc_len = 0;
    pbwe_box box;
    const pbw_descriptor *desc;
    pbw_struct_codec *codec = NULL;
    pbw_struct_arena *sarena = NULL;
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

    if (pbw_struct_codec_init(&codec, box.schema, "eb.Reading",
                              eb_reading_fields, 8) != PBW_OK) {
        fprintf(stderr, "struct codec init failed\n");
        pbwe_close(&box);
        free(desc_bytes);
        return 2;
    }
    sarena = pbw_struct_arena_new_mem(sblock, sizeof(smem) - 8);
    if (sarena == NULL) {
        fprintf(stderr, "struct arena init failed\n");
        pbw_struct_codec_free(codec);
        pbwe_close(&box);
        free(desc_bytes);
        return 2;
    }

    ok &= run_case(&box, desc, codec, sarena, 0, 200000);
    ok &= run_case(&box, desc, codec, sarena, 1, 200000);
    ok &= run_case(&box, desc, codec, sarena, 100, 50000);
    ok &= run_case(&box, desc, codec, sarena, 3000, 2000);

    printf("checksum: %ld\n", sink);
    pbwe_close(&box);
    free(desc_bytes);
    return ok ? 0 : 1;
}

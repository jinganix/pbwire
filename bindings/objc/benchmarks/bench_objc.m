/* Objective-C binding benchmark against the official protobuf Objective-C
 * runtime and generated code on the same matrix3.proto and the four standard
 * tiers from BENCHMARKS.md.
 *
 * Every scalar, the nested message and the two proto3 optionals. The wrapper
 * does not wrap repeated fields, so instead of scaling `nums`/`tags`/`inners`/
 * `flags` (x0 / x1 / x100 / x3000) the `str` field is padded until the wire size
 * matches the C/Go/Python rows: 136 / 174 / 2469 / 74913 bytes. Both sides must
 * produce identical bytes before anything is timed.
 *
 *   ./bench.sh          generate, build, run
 */

#import <Foundation/Foundation.h>

#import "PBWMessage.h"
#import "PBWSchema.h"

#import "Matrix3.pbobjc.h"

#include <stdio.h>
#include <time.h>

typedef struct {
    const char *label;
    int pad;
    size_t want;
    int repeats;
    int iterations;
} Case;

static const Case cases[] = {
    {"small", 0, 136, 0, 50000},
    {"medium", 38, 174, 1, 50000},
    {"large", 2332, 2469, 100, 20000},
    {"xlarge", 74775, 74913, 3000, 2000},
};

static int failures;

static uint64_t
now_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

static NSString *
padded(int pad)
{
    return [@"hello" stringByAppendingString:[@"" stringByPaddingToLength:(NSUInteger)pad
                                                               withString:@"x"
                                                          startingAtIndex:0]];
}

static Scalars3 *
fill_official(int pad)
{
    Scalars3 *m = [Scalars3 message];
    Inner3 *inner;

    m.i32 = INT32_MIN;
    m.i64 = INT64_MAX;
    m.u32 = UINT32_MAX;
    m.u64 = UINT64_MAX;
    m.s32 = INT32_MIN;
    m.s64 = INT64_MIN;
    m.fx32 = 0xdeadbeefu;
    m.fx64 = UINT64_C(72623859790382856);
    m.sfx32 = INT32_MIN;
    m.sfx64 = INT64_MIN;
    m.f32 = INFINITY;
    m.f64 = -123456.789;
    m.flag = YES;
    m.str = padded(pad);
    m.color = Color3_Color3Green;
    inner = [Inner3 message];
    inner.name = @"kid";
    inner.value = 42;
    m.inner = inner;
    m.optI32 = -7; /* the setter marks the proto3 optional present */
    m.optStr = @"opt";
    return m;
}

static PBWMessage *
fill_pbwire(PBWSchema *schema, int pad, NSError **error)
{
    PBWMessage *m = [schema messageByName:@"structtest.Scalars3"];
    PBWMessage *inner;

    if (![m setValue:@(INT32_MIN) forField:@"i32" error:error] || ![m setValue:@(INT64_MAX) forField:@"i64" error:error] || ![m setValue:@(UINT32_MAX) forField:@"u32" error:error] || ![m setValue:@(UINT64_MAX) forField:@"u64" error:error] || ![m setValue:@(INT32_MIN) forField:@"s32" error:error] || ![m setValue:@(INT64_MIN) forField:@"s64" error:error] || ![m setValue:@(0xdeadbeefu) forField:@"fx32" error:error] || ![m setValue:@(UINT64_C(72623859790382856)) forField:@"fx64" error:error] || ![m setValue:@(INT32_MIN) forField:@"sfx32" error:error] || ![m setValue:@(INT64_MIN) forField:@"sfx64" error:error] || ![m setValue:@(INFINITY) forField:@"f32" error:error] || ![m setValue:@(-123456.789) forField:@"f64" error:error] || ![m setValue:@YES forField:@"flag" error:error] || ![m setValue:padded(pad) forField:@"str" error:error] || ![m setValue:@(Color3_Color3Green) forField:@"color" error:error] || ![m setValue:@(-7) forField:@"opt_i32" error:error] || ![m setValue:@"opt" forField:@"opt_str" error:error]) {
        return nil;
    }
    inner = [m addMessageForField:@"inner" error:error];
    if (inner == nil || ![inner setValue:@"kid" forField:@"name" error:error] || ![inner setValue:@42 forField:@"value" error:error]) {
        return nil;
    }
    return m;
}

/* One autorelease pool per iteration: a decoded tree and the official parse
 * both hand out autoreleased objects, and a whole tier would not fit in
 * memory otherwise. Both sides pay the same pool. */
static double
run(int iterations, void (^op)(void))
{
    uint64_t start = now_ns();

    for (int i = 0; i < iterations; i++) {
        @autoreleasepool {
            op();
        }
    }
    return (double)(now_ns() - start) / iterations;
}

/* Floor iterations per tier; doubled until one pass takes 0.25 s (shared
 * standard, see BENCHMARKS.md). Calibrated on the binding's own encode;
 * slower ops then take longer than the minimum. */
static const double kMinPassSeconds = 0.25;

static int
calibrate(void (^op)(void), int floor)
{
    int count = floor > 0 ? floor : 1;

    for (;;) {
        uint64_t start = now_ns();
        for (int i = 0; i < count; i++)
            @autoreleasepool {
                op();
            }
        if ((double)(now_ns() - start) / 1e9 >= kMinPassSeconds ||
            count >= 100000000)
            return count;
        count *= 2;
    }
}

/* Every op calibrates independently from the floor (a slow op already exceeds
 * the minimum at the floor and keeps it), then warms up and keeps the best
 * of three runs. */
static double
measure(int floor, void (^op)(void))
{
    int count = calibrate(op, floor);
    double best = INFINITY;

    run(count, op);
    for (int pass = 0; pass < 3; pass++) {
        double ns = run(count, op);

        if (ns < best)
            best = ns;
    }
    return best;
}

static void
row(const char *impl, const char *op, size_t bytes, double ns)
{
    printf("  %-14s %-7s %12.1f %10.1f\n", impl, op, ns, (double)bytes / ns * 1000.0);
}

int
main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: bench_objc <descriptor-set>\n");
        return 2;
    }
    @autoreleasepool {
        NSError *error = nil;
        NSString *path = @(argv[1]);
        PBWSchema *schema = [PBWSchema schemaFromFile:path error:&error];

        if (schema == nil) {
            fprintf(stderr, "load %s: %s\n", argv[1],
                    error.localizedDescription.UTF8String);
            return 1;
        }
        for (size_t tier = 0; tier < sizeof cases / sizeof cases[0]; tier++) {
            const Case *c = &cases[tier];
            NSData *reference = [fill_official(c->pad) data];
            PBWMessage *message;

            if (reference.length != c->want) {
                printf("FAIL %s: official encoder wrote %zu bytes, expected %zu\n",
                       c->label, (size_t)reference.length, c->want);
                failures++;
                continue;
            }
            message = fill_pbwire(schema, c->pad, &error);
            if (message == nil) {
                printf("FAIL %s: %s\n", c->label,
                       error.localizedDescription.UTF8String);
                failures++;
                continue;
            }
            if (![[message encode:&error] isEqualToData:reference]) {
                printf("FAIL %s: pbwire-objc and protobuf-objc encode differently\n",
                       c->label);
                failures++;
                continue;
            }
            /* Decode must round-trip byte-exactly and expose the same values. */
            {
                NSError *gateError = nil;
                PBWMessage *back = [message decode:reference error:&gateError];
                NSData *backBytes = back == nil ? nil : [back encode:&gateError];
                PBWMessage *backInner = back == nil ? nil : [back messageForField:@"inner"];
                BOOL backOk = back != nil && backBytes != nil &&
                              [backBytes isEqualToData:reference] &&
                              [[back valueForField:@"str"] isEqual:@"hello"] &&
                              [[backInner valueForField:@"name"] isEqual:@"kid"] &&
                              [[backInner valueForField:@"value"] isEqual:@42];
                if (!backOk) {
                    printf("FAIL %s: pbwire-objc decode differs\n", c->label);
                    failures++;
                    continue;
                }
            }
            printf("OK   %s encoding parity, x%d (%zu bytes)\n", c->label,
                   c->repeats, (size_t)reference.length);

            {
                Scalars3 *official = fill_official(c->pad);
                PBWMessage *decodeSource = fill_pbwire(schema, c->pad, &error);
                /* Tier line shows the reference op's count; slower ops keep
                 * the floor. */
                int count = calibrate(^{
                  NSError *ignored = nil;

                  (void)[message encode:&ignored];
                },
                                      c->iterations);
                double encode_ns = measure(c->iterations, ^{
                  NSError *ignored = nil;

                  (void)[message encode:&ignored];
                });
                double decode_ns = measure(c->iterations, ^{
                  NSError *ignored = nil;

                  (void)[decodeSource decode:reference error:&ignored];
                });
                double official_encode_ns = measure(c->iterations, ^{
                  (void)[official data];
                });
                double official_decode_ns = measure(c->iterations, ^{
                  NSError *ignored = nil;

                  (void)[Scalars3 parseFromData:reference error:&ignored];
                });

                printf("%s: %zu bytes, %d iterations (str padded x%d)\n", c->label,
                       (size_t)reference.length, count, c->pad);
                printf("  %-14s %-7s %12s %10s\n", "impl", "op", "ns/op", "MB/s");
                row("pbwire-objc", "encode", reference.length, encode_ns);
                row("protobuf-objc", "encode", reference.length, official_encode_ns);
                row("pbwire-objc", "decode", reference.length, decode_ns);
                row("protobuf-objc", "decode", reference.length, official_decode_ns);
                printf("\n");
            }
        }
    }
    if (failures != 0) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    return 0;
}

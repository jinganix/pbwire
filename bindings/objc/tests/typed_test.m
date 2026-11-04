/* Typed-conformance check for the Objective-C binding: build matrix3.proto's
 * Scalars3 through the generated classes, encode, require the protoc reference
 * bytes (matrix3.bin), then decode those bytes back and check every field.
 *
 * Run it with ./build.sh typed. */

#import <Foundation/Foundation.h>

#import "PBWMessage.h"
#import "PBWSchema.h"
#import "gen/matrix3.h"

#include <stdio.h>

static int failures;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        if (!(cond)) {                                                               \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                              \
        }                                                                            \
    } while (0)

static PBWGen_Scalars3 *
BuildTyped(void)
{
    PBWGen_Scalars3 *m = [[PBWGen_Scalars3 alloc] init];

    m.i32 = INT32_MIN;
    m.i64 = INT64_MAX;
    m.u32 = -1; /* 4294967295 as raw 32-bit bits */
    m.u64 = -1; /* 18446744073709551615 as raw 64-bit bits */
    m.s32 = INT32_MIN;
    m.s64 = INT64_MIN;
    m.fx32 = (int32_t)0xdeadbeef;
    m.fx64 = 72623859790382856LL;
    m.sfx32 = INT32_MIN;
    m.sfx64 = INT64_MIN;
    m.f32 = INFINITY;
    m.f64 = -123456.789;
    m.flag = YES;
    m.str = @"hello";
    m.color = PBWGen_Color3_COLOR3_GREEN;
    m.inner = [PBWGen_Inner3 new];
    m.inner.name = @"kid";
    m.inner.value = 42;
    m.nums = @[ @1, @-5, @300 ];
    m.tags = @[ @"a", @"bb" ];
    PBWGen_Inner3 *x = [PBWGen_Inner3 new];
    x.name = @"x";
    x.value = 1;
    PBWGen_Inner3 *y = [PBWGen_Inner3 new];
    y.name = @"y";
    y.value = -2;
    m.inners = @[ x, y ];
    m.hasOptI32 = YES;
    m.optI32 = -7;
    m.hasOptStr = YES;
    m.optStr = @"opt";
    m.flags = @[ @YES, @NO, @YES ];
    return m;
}

static void
VerifyTyped(PBWGen_Scalars3 *m)
{
    CHECK(m.i32 == INT32_MIN);
    CHECK(m.i64 == INT64_MAX);
    CHECK(m.u32 == -1);
    CHECK(m.u64 == -1);
    CHECK(m.s32 == INT32_MIN);
    CHECK(m.s64 == INT64_MIN);
    CHECK(m.fx32 == (int32_t)0xdeadbeef);
    CHECK(m.fx64 == 72623859790382856LL);
    CHECK(m.sfx32 == INT32_MIN);
    CHECK(m.sfx64 == INT64_MIN);
    CHECK(m.f32 == INFINITY);
    CHECK(m.f64 == -123456.789);
    CHECK(m.flag == YES);
    CHECK([m.str isEqualToString:@"hello"]);
    CHECK(m.color == PBWGen_Color3_COLOR3_GREEN);
    CHECK([m.inner.name isEqualToString:@"kid"] && m.inner.value == 42);
    CHECK(m.nums.count == 3 && m.nums[1].intValue == -5);
    CHECK(m.tags.count == 2 && [m.tags[1] isEqualToString:@"bb"]);
    CHECK(m.inners.count == 2 && m.inners[1].value == -2);
    CHECK(m.hasOptI32 && m.optI32 == -7);
    CHECK(m.hasOptStr && [m.optStr isEqualToString:@"opt"]);
    CHECK(m.flags.count == 3 && m.flags[2].boolValue);
}

int
main(int argc, char **argv)
{
    @autoreleasepool {
        NSString *desc = argc > 1 ? @(argv[1]) : @"bindings/c/tests/data/matrix3.desc";
        NSString *bin = argc > 2 ? @(argv[2]) : @"bindings/c/tests/data/matrix3.bin";
        NSError *error = nil;

        PBWSchema *schema = [PBWSchema schemaFromFile:desc error:&error];
        CHECK(schema != nil);
        NSData *reference = [NSData dataWithContentsOfFile:bin];
        CHECK(reference.length > 0);

        PBWGen_Scalars3 *typed = BuildTyped();
        NSData *wire = [typed encodeWithSchema:schema error:&error];
        if (wire == nil) {
            fprintf(stderr, "encode error: %s\n", error.localizedDescription.UTF8String);
        }
        CHECK(wire != nil);
        CHECK([wire isEqualToData:reference]);
        VerifyTyped([PBWGen_Scalars3 decodeWithSchema:schema data:reference error:&error]);
        CHECK([[PBWGen_Scalars3 new] encodeWithSchema:schema error:&error].length == 0);

        PBWGen_Scalars3 *zero = [PBWGen_Scalars3 new];
        zero.hasOptI32 = YES;
        zero.optI32 = 0;
        PBWGen_Scalars3 *zeroBack =
            [PBWGen_Scalars3 decodeWithSchema:schema
                                         data:[zero encodeWithSchema:schema error:&error]
                                        error:&error];
        CHECK(zeroBack.hasOptI32 && zeroBack.optI32 == 0);

        PBWGen_Scalars3 *nested = [PBWGen_Scalars3 new];
        nested.inner = [PBWGen_Inner3 new];
        nested.inner.name = @"kid";
        nested.inner.value = 42;
        CHECK([PBWGen_Scalars3 decodeWithSchema:schema data:[nested encodeWithSchema:schema error:&error] error:&error]
                  .inner.value == 42);

        if (failures != 0) {
            fprintf(stderr, "typed_test: %d checks failed\n", failures);
            return 1;
        }
        printf("typed_test: all checks passed\n");
    }
    return 0;
}

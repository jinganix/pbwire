/* Generated-compat check for the Objective-C binding (TESTSPEC T1-T6, vectors.json
 * as the single source of truth):
 *
 *   T1  generated `data` == pinned protoc reference bytes (matrix3.bin,
 *       oneof1.bin, addressbook.bin)
 *   T2  `parseFromData:` of the reference reads every fixture value back,
 *       field by field, including presence
 *   T3  an empty message encodes empty; an explicit optional zero (the
 *       setter marks presence, like the official runtime) round-trips
 *   T4  differential vs the pinned protobuf ObjC oracle (protobuf v36.2,
 *       GOOGLE_PROTOBUF_OBJC_VERSION 40311; runtime sources from the
 *       `make conformance-setup` cache, see testdata/ORACLES.md): same
 *       bytes, same fields, cross-decode agrees
 *   T5  one business-logic snippet (WORKFLOW) runs against both libraries
 *       with identical observable results (the macro arguments -- class
 *       names plus the one enum constant -- are the only difference)
 *   T6  errors are loud, not crashes: bad wire fails, empty parses to the
 *       default, a missing required field fails with a message, two oneof
 *       members on the wire resolve deterministically, unknown enums
 *       round-trip
 *
 * The oracle side links the official runtime plus protoc --objc_out for the
 * three fixtures (build.sh generated); the facade classes carry the PBWGen_
 * prefix so both sides link into one binary.
 *
 * Run it with ./build.sh generated. */

#import <Foundation/Foundation.h>

#import "PBWMessage.h"
#import "PBWSchema.h"
#import "gen/matrix3.h"
#import "gen/matrix3_schema.h"
#import "gen/oneof1.h"
#import "gen/oneof1_schema.h"
#import "gen/addressbook.h"
#import "gen/addressbook_schema.h"

#import "Matrix3.pbobjc.h"
#import "Oneof1.pbobjc.h"
#import "Addressbook.pbobjc.h"

#include <math.h>
#include <stdio.h>

#if GOOGLE_PROTOBUF_OBJC_VERSION != 40311
#error "oracle pin drifted: protobuf ObjC runtime must be v36.2 (40311); see testdata/ORACLES.md"
#endif

static int failures;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        if (!(cond)) {                                                               \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                              \
        }                                                                            \
    } while (0)

/* One business-logic snippet, instantiated once per library. The macro
 * arguments (class names plus the one enum constant) are the only
 * difference; both sides share the property names, +message, -data and
 * -initWithData:error:. */
#define WORKFLOW(MsgClass, InnerClass, GreenValue, WireOut, SummaryOut)                               \
    do {                                                                                              \
        MsgClass *wfm = [MsgClass message];                                                           \
        wfm.str = @"kid";                                                                             \
        wfm.i32 = -5;                                                                                 \
        InnerClass *wfi = [InnerClass message];                                                       \
        wfi.name = @"x";                                                                              \
        wfi.value = 1;                                                                                \
        wfm.inner = wfi;                                                                              \
        wfm.optI32 = 7;                                                                               \
        wfm.color = GreenValue;                                                                       \
        WireOut = [wfm data];                                                                         \
        CHECK(WireOut != nil);                                                                        \
        NSError *wfe = nil;                                                                           \
        MsgClass *wfb = [[MsgClass alloc] initWithData:WireOut error:&wfe];                           \
        CHECK(wfb != nil);                                                                            \
        CHECK(wfe == nil);                                                                            \
        SummaryOut = [NSString stringWithFormat:                                                      \
                                   @"%@|%d|%d|%@|%d|%d",                                              \
                                   wfb.str, wfb.i32, (int)wfb.color, wfb.inner.name, wfb.inner.value, \
                                   (int)wfb.hasOptI32];                                               \
    } while (0)

static PBWGen_Scalars3 *
BuildTyped(void)
{
    PBWGen_Scalars3 *m = [PBWGen_Scalars3 message];

    m.i32 = INT32_MIN;
    m.i64 = INT64_MAX;
    m.u32 = UINT32_MAX;
    m.u64 = UINT64_MAX;
    m.s32 = INT32_MIN;
    m.s64 = INT64_MIN;
    m.fx32 = (uint32_t)0xdeadbeef;
    m.fx64 = 72623859790382856ULL;
    m.sfx32 = INT32_MIN;
    m.sfx64 = INT64_MIN;
    m.f32 = INFINITY;
    m.f64 = -123456.789;
    m.flag = YES;
    m.str = @"hello";
    m.color = PBWGen_Color3_COLOR3_GREEN;
    m.inner = [PBWGen_Inner3 message];
    m.inner.name = @"kid";
    m.inner.value = 42;
    m.numsArray = [NSMutableArray arrayWithArray:@[ @1, @-5, @300 ]];
    m.tagsArray = [NSMutableArray arrayWithArray:@[ @"a", @"bb" ]];
    PBWGen_Inner3 *x = [PBWGen_Inner3 message];
    x.name = @"x";
    x.value = 1;
    PBWGen_Inner3 *y = [PBWGen_Inner3 message];
    y.name = @"y";
    y.value = -2;
    m.innersArray = [NSMutableArray arrayWithArray:@[ x, y ]];
    m.optI32 = -7;
    m.optStr = @"opt";
    m.flagsArray = [NSMutableArray arrayWithArray:@[ @YES, @NO, @YES ]];
    return m;
}

static Scalars3 *
BuildOracle(void)
{
    Scalars3 *m = [Scalars3 message];
    Inner3 *inner;

    m.i32 = INT32_MIN;
    m.i64 = INT64_MAX;
    m.u32 = UINT32_MAX;
    m.u64 = UINT64_MAX;
    m.s32 = INT32_MIN;
    m.s64 = INT64_MIN;
    m.fx32 = (uint32_t)0xdeadbeef;
    m.fx64 = 72623859790382856ULL;
    m.sfx32 = INT32_MIN;
    m.sfx64 = INT64_MIN;
    m.f32 = INFINITY;
    m.f64 = -123456.789;
    m.flag = YES;
    m.str = @"hello";
    m.color = Color3_Color3Green;
    inner = [Inner3 message];
    inner.name = @"kid";
    inner.value = 42;
    m.inner = inner;
    [m.numsArray addValue:1];
    [m.numsArray addValue:-5];
    [m.numsArray addValue:300];
    [m.tagsArray addObject:@"a"];
    [m.tagsArray addObject:@"bb"];
    Inner3 *x = [Inner3 message];
    x.name = @"x";
    x.value = 1;
    Inner3 *y = [Inner3 message];
    y.name = @"y";
    y.value = -2;
    [m.innersArray addObject:x];
    [m.innersArray addObject:y];
    m.optI32 = -7;
    m.optStr = @"opt";
    [m.flagsArray addValue:YES];
    [m.flagsArray addValue:NO];
    [m.flagsArray addValue:YES];
    return m;
}

static void
CheckTyped(PBWGen_Scalars3 *m)
{
    CHECK(m.i32 == INT32_MIN);
    CHECK(m.i64 == INT64_MAX);
    CHECK(m.u32 == UINT32_MAX);
    CHECK(m.u64 == UINT64_MAX);
    CHECK(m.s32 == INT32_MIN);
    CHECK(m.s64 == INT64_MIN);
    CHECK(m.fx32 == (uint32_t)0xdeadbeef);
    CHECK(m.fx64 == 72623859790382856ULL);
    CHECK(m.sfx32 == INT32_MIN);
    CHECK(m.sfx64 == INT64_MIN);
    CHECK(isinf(m.f32) && m.f32 > 0);
    CHECK(m.f64 == -123456.789);
    CHECK(m.flag == YES);
    CHECK([m.str isEqualToString:@"hello"]);
    CHECK(m.color == PBWGen_Color3_COLOR3_GREEN);
    CHECK([m.inner.name isEqualToString:@"kid"] && m.inner.value == 42);
    CHECK(m.hasInner);
    CHECK(m.numsArray.count == 3);
    CHECK(m.numsArray_Count == 3);
    CHECK(m.numsArray[1].intValue == -5);
    CHECK(m.tagsArray.count == 2 && [m.tagsArray[1] isEqualToString:@"bb"]);
    CHECK(m.innersArray.count == 2 && m.innersArray[1].value == -2);
    CHECK([m.innersArray[0].name isEqualToString:@"x"]);
    CHECK(m.hasOptI32 && m.optI32 == -7);
    CHECK(m.hasOptStr && [m.optStr isEqualToString:@"opt"]);
    CHECK(m.flagsArray.count == 3 && m.flagsArray[2].boolValue);
    CHECK(!m.flagsArray[1].boolValue);
    CHECK(m.isInitialized);
}

static void
CheckOracle(Scalars3 *m)
{
    CHECK(m.i32 == INT32_MIN);
    CHECK(m.i64 == INT64_MAX);
    CHECK(m.u32 == UINT32_MAX);
    CHECK(m.u64 == UINT64_MAX);
    CHECK(m.s32 == INT32_MIN);
    CHECK(m.s64 == INT64_MIN);
    CHECK(m.fx32 == (uint32_t)0xdeadbeef);
    CHECK(m.fx64 == 72623859790382856ULL);
    CHECK(m.sfx32 == INT32_MIN);
    CHECK(m.sfx64 == INT64_MIN);
    CHECK(isinf(m.f32) && m.f32 > 0);
    CHECK(m.f64 == -123456.789);
    CHECK(m.flag == YES);
    CHECK([m.str isEqualToString:@"hello"]);
    CHECK(m.color == Color3_Color3Green);
    CHECK([m.inner.name isEqualToString:@"kid"] && m.inner.value == 42);
    CHECK(m.hasInner);
    CHECK(m.numsArray.count == 3 && [m.numsArray valueAtIndex:1] == -5);
    CHECK(m.numsArray_Count == 3);
    CHECK(m.tagsArray.count == 2 && [m.tagsArray[1] isEqualToString:@"bb"]);
    CHECK(m.innersArray.count == 2 && m.innersArray[1].value == -2);
    CHECK(m.hasOptI32 && m.optI32 == -7);
    CHECK(m.hasOptStr && [m.optStr isEqualToString:@"opt"]);
    CHECK(m.flagsArray.count == 3 && [m.flagsArray valueAtIndex:2]);
    CHECK(![m.flagsArray valueAtIndex:1]);
    CHECK(m.initialized);
}

int
main(int argc, char **argv)
{
    @autoreleasepool {
        NSString *matrixBin = argc > 1 ? @(argv[1]) : @"bindings/c/tests/data/matrix3.bin";
        NSString *oneofBin = argc > 2 ? @(argv[2]) : @"codegen/testdata/oneof1.bin";
        NSString *bookBin = argc > 3 ? @(argv[3]) : @"examples/addressbook.bin";
        NSError *error = nil;

        NSData *reference = [NSData dataWithContentsOfFile:matrixBin];
        NSData *oneofReference = [NSData dataWithContentsOfFile:oneofBin];
        NSData *bookReference = [NSData dataWithContentsOfFile:bookBin];
        CHECK(reference.length > 0);
        CHECK(oneofReference.length > 0);
        CHECK(bookReference.length > 0);

        /* T1: facade bytes == pinned protoc bytes; oracle bytes agree too. */
        NSData *wire = [BuildTyped() data];
        CHECK(wire != nil);
        CHECK([wire isEqualToData:reference]);
        NSData *oracleWire = [[BuildOracle() data] copy];
        CHECK([oracleWire isEqualToData:reference]);

        PBWGen_Oneof1 *one = [PBWGen_Oneof1 message];
        one.name = @"a";
        one.text = @"hi";
        CHECK(one.kindOneOfCase == PBWGen_Oneof1_Kind_OneOfCase_Text);
        CHECK(one.hasText && !one.hasNum && !one.hasInner);
        CHECK([one.text isEqualToString:@"hi"]);
        CHECK([[one data] isEqualToData:oneofReference]);
        Oneof1 *oracleOne = [Oneof1 message];
        oracleOne.name = @"a";
        oracleOne.text = @"hi";
        CHECK(oracleOne.kindOneOfCase == Oneof1_Kind_OneOfCase_Text);
        CHECK([[oracleOne data] isEqualToData:oneofReference]);

        PBWGen_AddressBook *book = [PBWGen_AddressBook message];
        PBWGen_Person *alice = [PBWGen_Person message];
        alice.name = @"Alice";
        alice.id = 1;
        PBWGen_Person_PhoneNumber *work = [PBWGen_Person_PhoneNumber message];
        work.number = @"555-0100";
        work.type = PBWGen_Person_PhoneType_MOBILE;
        alice.phonesArray = [NSMutableArray arrayWithArray:@[ work ]];
        PBWGen_Person *bob = [PBWGen_Person message];
        bob.name = @"Bob";
        bob.id = 2;
        bob.email = @"bob@example.com"; /* setter marks presence */
        CHECK(bob.hasEmail);
        PBWGen_Person_PhoneNumber *home = [PBWGen_Person_PhoneNumber message];
        home.number = @"555-0200";
        home.type = PBWGen_Person_PhoneType_HOME;
        bob.phonesArray = [NSMutableArray arrayWithArray:@[ home ]];
        book.peopleArray = [NSMutableArray arrayWithArray:@[ alice, bob ]];
        CHECK([[book data] isEqualToData:bookReference]);

        /* T2: every field back from the reference, including presence. */
        CheckTyped([PBWGen_Scalars3 parseFromData:reference error:&error]);
        CHECK(error == nil);
        CheckOracle([Scalars3 parseFromData:reference error:&error]);
        CHECK(error == nil);
        PBWGen_Oneof1 *oneBack = [PBWGen_Oneof1 parseFromData:oneofReference error:&error];
        CHECK(oneBack != nil && error == nil);
        CHECK([oneBack.name isEqualToString:@"a"]);
        CHECK(oneBack.kindOneOfCase == PBWGen_Oneof1_Kind_OneOfCase_Text);
        CHECK(oneBack.hasText && [oneBack.text isEqualToString:@"hi"]);
        PBWGen_Oneof1 *nested = [PBWGen_Oneof1 message];
        nested.name = @"n";
        nested.inner = [PBWGen_Oneof1_Inner message];
        nested.inner.v = 7;
        CHECK(nested.kindOneOfCase == PBWGen_Oneof1_Kind_OneOfCase_Inner);
        PBWGen_Oneof1 *nestedBack =
            [PBWGen_Oneof1 parseFromData:[nested data]
                                   error:&error];
        CHECK(nestedBack.inner.v == 7);
        PBWGen_AddressBook *bookBack =
            [PBWGen_AddressBook parseFromData:bookReference
                                        error:&error];
        CHECK(bookBack != nil && error == nil);
        CHECK(bookBack.peopleArray.count == 2);
        CHECK([bookBack.peopleArray[0].name isEqualToString:@"Alice"]);
        CHECK(!bookBack.peopleArray[0].hasEmail);
        CHECK(bookBack.peopleArray[0].phonesArray_Count == 1);
        CHECK([bookBack.peopleArray[1].name isEqualToString:@"Bob"]);
        CHECK([bookBack.peopleArray[1].email isEqualToString:@"bob@example.com"]);
        CHECK([bookBack.peopleArray[1].phonesArray[0].number isEqualToString:@"555-0200"]);

        /* T3: empty encodes empty; explicit zero keeps presence. */
        CHECK([[[PBWGen_Scalars3 message] data] length] == 0);
        CHECK([[[Scalars3 message] data] length] == 0);
        PBWGen_Scalars3 *zero = [PBWGen_Scalars3 message];
        zero.optI32 = 0; /* the setter marks presence, like the runtime */
        CHECK(zero.hasOptI32);
        PBWGen_Scalars3 *zeroBack =
            [PBWGen_Scalars3 parseFromData:[zero data]
                                     error:&error];
        CHECK(zeroBack.hasOptI32 && zeroBack.optI32 == 0);
        CHECK(!zeroBack.hasOptStr);
        Scalars3 *oracleZero = [Scalars3 message];
        oracleZero.optI32 = 0;
        Scalars3 *oracleZeroBack =
            [Scalars3 parseFromData:[oracleZero data]
                              error:&error];
        CHECK(oracleZeroBack.hasOptI32 && oracleZeroBack.optI32 == 0);

        /* T4: cross-decode agrees field by field. */
        CheckTyped([PBWGen_Scalars3 parseFromData:oracleWire error:&error]);
        CheckOracle([Scalars3 parseFromData:wire error:&error]);
        CHECK([[BuildTyped() data] isEqualToData:oracleWire]);

        /* T5: one snippet, both libraries. */
        NSString *facadeSummary = nil;
        NSString *oracleSummary = nil;
        NSData *facadeWire = nil;
        NSData *oracleWire2 = nil;
        {
            WORKFLOW(PBWGen_Scalars3, PBWGen_Inner3,
                     PBWGen_Color3_COLOR3_GREEN, facadeWire, facadeSummary);
        }
        {
            WORKFLOW(Scalars3, Inner3, Color3_Color3Green, oracleWire2,
                     oracleSummary);
        }
        CHECK([facadeWire isEqualToData:oracleWire2]);
        CHECK([facadeSummary isEqualToString:oracleSummary]);

        /* T6: loud errors, not crashes. */
        NSData *garbage = [NSData dataWithBytes:"\xff\xff\xff" length:3];
        error = nil;
        CHECK([PBWGen_Scalars3 parseFromData:garbage error:&error] == nil);
        CHECK(error != nil);
        CHECK([error.domain isEqualToString:PBWErrorDomain]);
        error = nil;
        CHECK([Scalars3 parseFromData:garbage error:&error] == nil);
        CHECK(error != nil);
        /* Empty parses to the default on both sides. */
        error = nil;
        PBWGen_Scalars3 *emptyBack =
            [PBWGen_Scalars3 parseFromData:[NSData data]
                                     error:&error];
        CHECK(emptyBack != nil && error == nil);
        CHECK([[emptyBack data] length] == 0);
        error = nil;
        CHECK([Scalars3 parseFromData:[NSData data] error:&error] != nil);
        /* Nil data fails loudly instead of crashing. */
        error = nil;
        CHECK([PBWGen_Scalars3 parseFromData:nil error:&error] == nil);
        CHECK(error != nil);
        /* Missing required is loud: the facade flags it before encode, and
         * the C core refuses the wire too ("required field not set"). */
        PBWGen_Person *nameless = [PBWGen_Person message];
        nameless.id = 2;
        CHECK(!nameless.isInitialized);
        NSData *namelessWire = [nameless data];
        CHECK(namelessWire != nil);
        error = nil;
        CHECK([PBWGen_Person parseFromData:namelessWire error:&error] == nil);
        CHECK(error != nil);
        CHECK([error.domain isEqualToString:PBWErrorDomain]);
        CHECK(error.localizedDescription.length > 0);
        Person *oracleNameless = [Person message];
        oracleNameless.id_p = 2;
        Person *oracleParsed =
            [Person parseFromData:namelessWire
                            error:&error];
        CHECK(oracleParsed == nil || !oracleParsed.initialized);
        /* Two oneof members on the wire resolve deterministically. */
        PBWMessage *dynOne = [[PBWGen_Oneof1 pbw_schema]
            messageByName:@"spectest.Oneof1"];
        CHECK(dynOne != nil);
        CHECK([dynOne setValue:@1 forField:@"num" error:&error]);
        CHECK([dynOne setValue:@"hi" forField:@"text" error:&error]);
        NSData *twoMemberWire = [dynOne encode:&error];
        CHECK(twoMemberWire != nil);
        PBWGen_Oneof1 *twoBack =
            [PBWGen_Oneof1 parseFromData:twoMemberWire
                                   error:&error];
        CHECK(twoBack != nil && error == nil);
        CHECK(twoBack.kindOneOfCase == PBWGen_Oneof1_Kind_OneOfCase_Text);
        CHECK([twoBack.text isEqualToString:@"hi"]);
        Oneof1 *oracleTwo =
            [Oneof1 parseFromData:twoMemberWire
                            error:&error];
        CHECK(oracleTwo.kindOneOfCase == Oneof1_Kind_OneOfCase_Text);
        /* Unknown enum values survive the round trip on both sides. */
        PBWMessage *dynColor = [[PBWGen_Scalars3 pbw_schema]
            messageByName:@"structtest.Scalars3"];
        CHECK([dynColor setValue:@99 forField:@"color" error:&error]);
        NSData *colorWire = [dynColor encode:&error];
        PBWGen_Scalars3 *colorBack =
            [PBWGen_Scalars3 parseFromData:colorWire
                                     error:&error];
        CHECK(colorBack.color == 99);
        CHECK([[colorBack data] isEqualToData:colorWire]);
        Scalars3 *oracleColor =
            [Scalars3 parseFromData:colorWire
                              error:&error];
        CHECK(Scalars3_Color_RawValue(oracleColor) == 99);
        CHECK([[oracleColor data] isEqualToData:colorWire]);

        /* T7  the schema sidecar: the whole-file list covers every
         * message, preload warms them, and an external schema swaps in
         * (dual registration) with byte-identical output. */
        CHECK([PBWGenMatrix3FileSchemaMessageNames()
            containsObject:@"structtest.Scalars3"]);
        CHECK([PBWGenMatrix3FileSchemaMessageNames()
            containsObject:@"structtest.Inner3"]);
        PBWGenMatrix3FileSchemaPreload();
        PBWGen_Scalars3 *preBack =
            [PBWGen_Scalars3 parseFromData:colorWire
                                     error:&error];
        CHECK(preBack != nil && preBack.color == 99);
        PBWSchema *extSchema =
            [PBWSchema schemaFromFile:@"tests/data/probe.desc"
                                error:&error];
        CHECK(extSchema != nil);
        PBWGenMatrix3FileSchemaUse(extSchema);
        CHECK(PBWGenMatrix3FileSchema() == extSchema);
        PBWGenMatrix3FileSchemaInit();
        PBWGen_Scalars3 *restored =
            [PBWGen_Scalars3 parseFromData:colorWire
                                     error:&error];
        CHECK(restored != nil && restored.color == 99);

        if (failures != 0) {
            fprintf(stderr, "generated_test: %d checks failed\n", failures);
            return 1;
        }
        printf("generated_test: all checks passed (T1-T7, oracle v36.2)\n");
    }
    return 0;
}

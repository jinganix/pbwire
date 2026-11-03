/* Assert-based self-check for the Objective-C binding: no XCTest, no
 * dependencies beyond Foundation. Run it with ./build.sh.
 *
 * Covers: descriptor set load (file + data), unknown names, a full scalar and
 * nested round trip, byte-exact re-encode, presence of absent/zero fields,
 * schema/API errors, decode rejecting garbage, and message lifetime. */

#import <Foundation/Foundation.h>

#import "PBWMessage.h"
#import "PBWSchema.h"

#include <stdio.h>

static int failures;
static NSString *descriptorPath;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        if (!(cond)) {                                                               \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                              \
        }                                                                            \
    } while (0)

#define CHECK_STRING(actual, expected)                                              \
    do {                                                                            \
        NSString *a_ = (actual);                                                    \
        if (![a_ isEqual:(expected)]) {                                             \
            fprintf(stderr, "%s:%d: expected %s == \"%s\", got \"%s\"\n", __FILE__, \
                    __LINE__, #actual, [(expected) UTF8String], [a_ UTF8String]);   \
            failures++;                                                             \
        }                                                                           \
    } while (0)

#define CHECK_NUMBER(actual, expected)                                                \
    do {                                                                              \
        NSNumber *a_ = (actual);                                                      \
        NSNumber *e_ = (expected);                                                    \
        if (![a_ isEqualToNumber:e_]) {                                               \
            fprintf(stderr, "%s:%d: expected %s == %s, got %s\n", __FILE__, __LINE__, \
                    #actual, [[e_ description] UTF8String],                           \
                    [[a_ description] UTF8String]);                                   \
            failures++;                                                               \
        }                                                                             \
    } while (0)

/* __FILE__ is the path this file was compiled from (build.sh passes an absolute
 * one), so the fixture resolves without depending on the working directory. */
static NSString *
defaultDescriptorPath(void)
{
    NSString *source = @(__FILE__);

    return [[source stringByDeletingLastPathComponent]
        stringByAppendingPathComponent:@"data/probe.desc"];
}

static PBWSchema *
loadSchema(void)
{
    NSError *error = nil;
    PBWSchema *schema = [PBWSchema schemaFromFile:descriptorPath error:&error];

    if (schema == nil) {
        fprintf(stderr, "cannot load %s: %s\n", descriptorPath.UTF8String,
                error.localizedDescription.UTF8String);
        failures++;
    }
    return schema;
}

/* Every field the fixture declares, so the round-trip test reads them all. */
static PBWMessage *
filledProbe(PBWSchema *schema)
{
    static const uint8_t raw[] = {0x00, 0xff, 0x10, 0x7f};
    PBWMessage *probe = [schema messageByName:@"pbwobjc.Probe"];
    PBWMessage *inner;
    NSError *error = nil;

    CHECK(probe != nil);
    CHECK([probe setValue:@(-7) forField:@"i32" error:&error]);
    CHECK([probe setValue:@(-3) forField:@"s32" error:&error]);
    CHECK([probe setValue:@(1234567890123LL) forField:@"i64" error:&error]);
    CHECK([probe setValue:@(4000000000U) forField:@"u32" error:&error]);
    CHECK([probe setValue:@(18446744073709551615ULL)
                 forField:@"u64"
                    error:&error]);
    CHECK([probe setValue:@YES forField:@"flag" error:&error]);
    CHECK([probe setValue:@(1.5f) forField:@"f32" error:&error]);
    CHECK([probe setValue:@(-2.25) forField:@"f64" error:&error]);
    CHECK([probe setValue:@"héllo" forField:@"text" error:&error]);
    CHECK([probe setValue:[NSData dataWithBytes:raw length:sizeof(raw)]
                 forField:@"blob"
                    error:&error]);
    CHECK([probe setValue:@(2) forField:@"color" error:&error]);
    inner = [probe addMessageForField:@"inner" error:&error];
    CHECK(inner != nil);
    CHECK([inner setValue:@"nested" forField:@"label" error:&error]);
    CHECK([inner setValue:@9 forField:@"n" error:&error]);
    return probe;
}

static void
checkFilledProbe(PBWMessage *probe)
{
    static const uint8_t raw[] = {0x00, 0xff, 0x10, 0x7f};
    PBWMessage *inner;

    CHECK([probe hasField:@"i32"]);
    CHECK_NUMBER([probe valueForField:@"i32"], @(-7));
    CHECK_NUMBER([probe valueForField:@"s32"], @(-3));
    CHECK_NUMBER([probe valueForField:@"i64"], @(1234567890123LL));
    CHECK_NUMBER([probe valueForField:@"u32"], @(4000000000U));
    CHECK_NUMBER([probe valueForField:@"u64"], @(18446744073709551615ULL));
    CHECK_NUMBER([probe valueForField:@"flag"], @YES);
    CHECK_NUMBER([probe valueForField:@"f32"], @(1.5f));
    CHECK_NUMBER([probe valueForField:@"f64"], @(-2.25));
    CHECK_STRING([probe valueForField:@"text"], @"héllo");
    CHECK([[probe valueForField:@"blob"] isEqualToData:
                                             [NSData dataWithBytes:raw
                                                            length:sizeof(raw)]]);
    CHECK_NUMBER([probe valueForField:@"color"], @(2));

    inner = [probe messageForField:@"inner"];
    CHECK(inner != nil);
    CHECK_STRING([(PBWMessage *)[probe valueForField:@"inner"] messageName],
                 @"pbwobjc.Inner");
    CHECK_STRING([inner valueForField:@"label"], @"nested");
    CHECK_NUMBER([inner valueForField:@"n"], @(9));
}

static void
testLoadSchema(void)
{
    NSError *error = nil;
    NSData *bytes;
    PBWSchema *fromData;
    PBWSchema *schema = loadSchema();

    if (schema == nil) {
        return;
    }
    CHECK([schema messageByName:@"pbwobjc.Probe"].messageName != nil);
    CHECK_STRING([schema messageByName:@"pbwobjc.Inner"].messageName,
                 @"pbwobjc.Inner");
    /* An enum is not a message, and an unknown name is not a message. */
    CHECK([schema messageByName:@"pbwobjc.Color"] == nil);
    CHECK([schema messageByName:@"pbwobjc.Nope"] == nil);
    CHECK([schema messageByName:@""] == nil);

    bytes = [NSData dataWithContentsOfFile:descriptorPath];
    CHECK(bytes.length > 0);
    fromData = [PBWSchema schemaFromData:bytes error:&error];
    CHECK(fromData != nil);
    CHECK(error == nil);
    CHECK([fromData messageByName:@"pbwobjc.Probe"] != nil);
}

static void
testRoundTrip(void)
{
    PBWSchema *schema = loadSchema();
    PBWMessage *decoded;
    NSData *wire;
    NSError *error = nil;

    if (schema == nil) {
        return;
    }
    wire = [filledProbe(schema) encode:&error];
    CHECK(wire != nil);
    CHECK(error == nil);
    CHECK(wire.length > 0);

    decoded = [[schema messageByName:@"pbwobjc.Probe"] decode:wire error:&error];
    CHECK(decoded != nil);
    CHECK(error == nil);
    CHECK_STRING(decoded.messageName, @"pbwobjc.Probe");
    checkFilledProbe(decoded);
}

static void
testByteExactReencode(void)
{
    PBWSchema *schema = loadSchema();
    PBWMessage *decoded;
    NSData *first;
    NSData *second;
    NSError *error = nil;

    if (schema == nil) {
        return;
    }
    first = [filledProbe(schema) encode:&error];
    CHECK(first != nil);
    decoded = [[schema messageByName:@"pbwobjc.Probe"] decode:first error:&error];
    CHECK(decoded != nil);
    second = [decoded encode:&error];
    CHECK(second != nil);
    CHECK([first isEqualToData:second]);
}

static void
testEncodeBufferOwnership(void)
{
    PBWSchema *schema = loadSchema();
    PBWMessage *probe;
    NSString *shortText = @"a";
    NSString *longText = [@"a" stringByPaddingToLength:64 withString:@"b" startingAtIndex:0];
    NSData *first;
    NSData *second;
    NSData *again;
    NSError *error = nil;

    if (schema == nil) {
        return;
    }
    probe = [schema messageByName:@"pbwobjc.Probe"];
    CHECK(probe != nil);
    CHECK([probe setValue:shortText forField:@"text" error:&error]);
    first = [probe encode:&error];
    CHECK(first != nil);
    /* Two live encodings of one message must not share storage: the encoder's
     * buffer is transferred to the NSData, not reused from a shared pool. */
    again = [probe encode:&error];
    CHECK(again != nil);
    CHECK([first isEqualToData:again]);
    CHECK(first.bytes != again.bytes);

    /* Growing the message afterwards must leave the first result intact. */
    CHECK([probe setValue:longText forField:@"text" error:&error]);
    second = [probe encode:&error];
    CHECK(second != nil);
    CHECK(second.length > first.length);
    CHECK(![first isEqualToData:second]);
    CHECK([[[probe decode:first error:&error] valueForField:@"text"] isEqual:shortText]);
    CHECK([[[probe decode:second error:&error] valueForField:@"text"] isEqual:longText]);
}

static void
testAbsentFields(void)
{
    PBWSchema *schema = loadSchema();
    PBWMessage *probe;
    PBWMessage *decoded;
    NSData *wire;
    NSError *error = nil;

    if (schema == nil) {
        return;
    }
    probe = [schema messageByName:@"pbwobjc.Probe"];
    CHECK(probe != nil);
    /* No defaults are filled in: an untouched field is absent. */
    CHECK(![probe hasField:@"i32"]);
    CHECK(![probe hasField:@"text"]);
    CHECK(![probe hasField:@"inner"]);
    CHECK([probe valueForField:@"i32"] == nil);
    CHECK([probe messageForField:@"inner"] == nil);
    /* An unknown field name is just absent here (setValue: reports it). */
    CHECK(![probe hasField:@"nope"]);
    CHECK([probe valueForField:@"nope"] == nil);

    wire = [probe encode:&error];
    CHECK(wire != nil);
    CHECK(wire.length == 0);
    decoded = [probe decode:wire error:&error];
    CHECK(decoded != nil);
    CHECK(![decoded hasField:@"i32"]);

    /* An explicit zero is present: proto3 presence here is the field's own. */
    CHECK([probe setValue:@(0) forField:@"i32" error:&error]);
    CHECK([probe hasField:@"i32"]);
    wire = [probe encode:&error];
    CHECK(wire.length == 2);
    decoded = [probe decode:wire error:&error];
    CHECK([decoded hasField:@"i32"]);
    CHECK_NUMBER([decoded valueForField:@"i32"], @(0));

    /* nil clears it again. */
    CHECK([probe setValue:nil forField:@"i32" error:&error]);
    CHECK(![probe hasField:@"i32"]);
}

static void
testNestedMessage(void)
{
    PBWSchema *schema = loadSchema();
    PBWMessage *first;
    PBWMessage *decoded;
    PBWMessage *inner;
    PBWMessage *probe;
    NSData *wire;
    NSError *error = nil;

    if (schema == nil) {
        return;
    }
    probe = [schema messageByName:@"pbwobjc.Probe"];
    CHECK(probe != nil);
    inner = [probe addMessageForField:@"inner" error:&error];
    CHECK(inner != nil);
    CHECK_STRING(inner.messageName, @"pbwobjc.Inner");
    CHECK([inner setValue:@"label" forField:@"label" error:&error]);
    CHECK([inner setValue:@(5) forField:@"n" error:&error]);
    /* Get-or-create: a second call fills the same nested message. */
    first = [probe addMessageForField:@"inner" error:&error];
    CHECK(first != nil);
    CHECK_NUMBER([first valueForField:@"n"], @(5));
    CHECK([first setValue:@(6) forField:@"n" error:&error]);

    wire = [probe encode:&error];
    CHECK(wire != nil);
    decoded = [probe decode:wire error:&error];
    CHECK(decoded != nil);
    inner = [decoded messageForField:@"inner"];
    CHECK(inner != nil);
    CHECK_STRING([inner valueForField:@"label"], @"label");
    CHECK_NUMBER([inner valueForField:@"n"], @(6));
    CHECK([inner setValue:@(7) forField:@"n" error:&error]);
    /* Editing the view edits the parent's tree. */
    CHECK_NUMBER([(PBWMessage *)[decoded valueForField:@"inner"] valueForField:@"n"],
                 @(7));
}

static void
testErrors(void)
{
    PBWSchema *schema = loadSchema();
    PBWMessage *probe;
    NSError *error = nil;
    NSData *garbage = [@"not a FileDescriptorSet" dataUsingEncoding:NSUTF8StringEncoding];

    if (schema == nil) {
        return;
    }
    /* Schema error: malformed descriptor bytes, with pbw_schema_error() as the
     * reason. */
    CHECK([PBWSchema schemaFromData:garbage error:&error] == nil);
    CHECK(error != nil);
    CHECK_STRING(error.domain, PBWErrorDomain);
    CHECK(error.code == PBW_ERR_INVALID);
    CHECK([error.localizedDescription rangeOfString:@"malformed"].location !=
          NSNotFound);

    /* Schema error: a file that does not exist. */
    error = nil;
    CHECK([PBWSchema schemaFromFile:@"/nonexistent/probe.desc" error:&error] == nil);
    CHECK(error != nil);
    CHECK_STRING(error.domain, NSCocoaErrorDomain);

    probe = [schema messageByName:@"pbwobjc.Probe"];
    CHECK(probe != nil);

    /* API errors: unknown field, wrong class, out-of-range, repeated/message
     * field access through the wrong method, and bad wire bytes. */
    error = nil;
    CHECK(![probe setValue:@1 forField:@"nope" error:&error]);
    CHECK(error.code == PBW_ERR_NOT_FOUND);

    error = nil;
    CHECK(![probe setValue:@"seven" forField:@"i32" error:&error]);
    CHECK(error.code == PBW_ERR_TYPE);

    error = nil;
    CHECK(![probe setValue:@(1LL << 40) forField:@"i32" error:&error]);
    CHECK(error.code == PBW_ERR_TYPE);

    error = nil;
    CHECK(![probe setValue:@1 forField:@"inner" error:&error]);
    CHECK(error.code == PBW_ERR_TYPE);

    error = nil;
    CHECK([probe addMessageForField:@"i32" error:&error] == nil);
    CHECK(error.code == PBW_ERR_TYPE);

    error = nil;
    CHECK([probe decode:[NSData dataWithBytes:"\xff\xff" length:2]
                  error:&error] == nil);
    CHECK(error != nil);
    CHECK(error.code == PBW_ERR_INVALID);
}

static void
testLifetime(void)
{
    PBWSchema *schema = loadSchema();
    NSData *wire;
    PBWMessage *view;
    NSError *error = nil;
    int i;

    if (schema == nil) {
        return;
    }
    wire = [filledProbe(schema) encode:&error];
    CHECK(wire != nil);

    /* A nested view keeps its parent alive: reading it after the root message
     * goes out of scope must stay valid. */
    @autoreleasepool {
        @autoreleasepool {
            PBWMessage *root = [schema messageByName:@"pbwobjc.Probe"];
            PBWMessage *decoded = [root decode:wire error:&error];

            CHECK(decoded != nil);
            view = [decoded messageForField:@"inner"];
        }
        CHECK_STRING([view valueForField:@"label"], @"nested");
    }
    view = nil;

    /* Build, encode, decode and release in a loop: a double free or a missed
     * free of the C trees shows up here (crashes or grows unbounded). */
    for (i = 0; i < 200; i++) {
        @autoreleasepool {
            PBWMessage *message = filledProbe(schema);
            NSData *bytes = [message encode:&error];
            PBWMessage *back = [message decode:bytes error:&error];

            CHECK(back != nil);
            (void)[back messageForField:@"inner"];
        }
    }
}

int
main(int argc, char **argv)
{
    @autoreleasepool {
        descriptorPath = argc > 1 ? @(argv[1]) : defaultDescriptorPath();

        testLoadSchema();
        testRoundTrip();
        testByteExactReencode();
        testEncodeBufferOwnership();
        testAbsentFields();
        testNestedMessage();
        testErrors();
        testLifetime();

        if (failures == 0) {
            printf("test_objc: all checks passed (%s)\n",
                   descriptorPath.UTF8String);
            return 0;
        }
        fprintf(stderr, "test_objc: %d check(s) failed\n", failures);
        return 1;
    }
}

/* Assert-based self-check for the Objective-C binding: no XCTest, no
 * dependencies beyond Foundation. Run it with ./build.sh.
 *
 * Covers: descriptor set load (file + data), unknown names, a full scalar and
 * nested round trip, byte-exact re-encode, presence of absent/zero fields,
 * schema/API errors, decode rejecting garbage, message lifetime, JSON/text
 * mappings (plus parse options and bounds), deterministic encoding, delimited
 * framing, explicit decode limits, and the WKT/service helpers in
 * tests/data/cover.desc. */

#import <Foundation/Foundation.h>

#import "PBWMessage.h"
#import "PBWSchema.h"
#import "PBWContext.h"
#import "PBWPool.h"

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

static void
testRepeatedHelpers(void)
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
    CHECK([[probe valuesForField:@"nums"] count] == 0);
    NSArray *three = @[ @(1), @(-5), @(300) ];
    CHECK([probe setValues:three forField:@"nums" error:&error]);
    CHECK([[probe valuesForField:@"nums"] count] == 3);
    CHECK_NUMBER([probe valueForField:@"nums" atIndex:1], @(-5));
    CHECK([probe setValue:@(9) forField:@"i32" error:&error]);
    /* Singular fields read no array, and arrays reject singular setters. */
    CHECK([probe valuesForField:@"i32"] == nil);
    CHECK(![probe setValue:@(1) forField:@"nums" error:&error]);
    CHECK([probe setValues:nil forField:@"nums" error:&error]);
    CHECK([[probe valuesForField:@"nums"] count] == 0);
    NSArray *two = @[ @(4), @(6) ];
    CHECK([probe setValues:two forField:@"nums" error:&error]);
    wire = [probe encode:&error];
    CHECK(wire != nil);
    decoded = [probe decode:wire error:&error];
    CHECK(decoded != nil);
    CHECK(([[decoded valuesForField:@"nums"] isEqualToArray:@[ @(4), @(6) ]]));
    /* Unknown and singular names read no array. */
    CHECK([decoded valuesForField:@"nope"] == nil);
}

static void
testMapHelpers(void)
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
    NSDictionary *pair = @{@"a" : @(1),
                           @"b" : @(-2)};
    CHECK([probe setEntries:pair forField:@"attrs" error:&error]);
    CHECK([[[probe entriesForField:@"attrs"] allKeys] count] == 2);
    CHECK_NUMBER([probe entriesForField:@"attrs"][@"b"], @(-2));
    /* A repeated message that is not a map reads no dictionary. */
    CHECK([probe entriesForField:@"inner"] == nil);
    NSDictionary *oneEntry = @{@"a" : @(1)};
    CHECK(![probe setEntries:oneEntry forField:@"inner" error:&error]);
    wire = [probe encode:&error];
    CHECK(wire != nil);
    decoded = [probe decode:wire error:&error];
    CHECK(decoded != nil);
    CHECK(([[decoded entriesForField:@"attrs"] isEqualToDictionary:@{@"a" : @(1),
                                                                     @"b" : @(-2)}]));
    CHECK([decoded countForField:@"attrs"] == 2);
    /* Replacing drops the old entries. */
    NSDictionary *solo = @{@"c" : @(3)};
    CHECK([decoded setEntries:solo forField:@"attrs" error:&error]);
    CHECK(([[decoded entriesForField:@"attrs"] isEqualToDictionary:@{@"c" : @(3)}]));
    CHECK([decoded setEntries:nil forField:@"attrs" error:&error]);
    CHECK([[[decoded entriesForField:@"attrs"] allKeys] count] == 0);
}

static void
testJSONText(void)
{
    PBWSchema *schema = loadSchema();
    PBWMessage *probe;
    NSData *wire;
    NSString *json;
    NSString *text;
    PBWMessage *back;
    NSError *error = nil;

    if (schema == nil) {
        return;
    }
    probe = filledProbe(schema);
    wire = [probe encode:&error];
    CHECK(wire != nil);
    json = [probe JSONString:&error];
    CHECK(json != nil);
    CHECK([json containsString:@"\"text\":\"h\u00e9llo\""] || [json containsString:@"\"text\": \"h\u00e9llo\""]);
    back = [probe parseJSONString:json error:&error];
    CHECK(back != nil);
    CHECK([[back encode:&error] isEqualToData:wire]);
    CHECK([probe parseJSONString:@"{oops" error:&error] == nil);
    text = [probe textFormat:&error];
    CHECK(text != nil);
    CHECK([text containsString:@"label: \"nested\""]);
    back = [probe parseTextFormat:text error:&error];
    CHECK(back != nil);
    CHECK([[back encode:&error] isEqualToData:wire]);
    CHECK([probe parseTextFormat:@"text: " error:&error] == nil);
}

static void
testDeterministic(void)
{
    PBWSchema *schema = loadSchema();
    PBWMessage *probe;
    PBWMessage *ordered;
    NSData *sorted;
    NSData *plain;
    NSError *error = nil;

    if (schema == nil) {
        return;
    }
    /* Inserted b-then-a: the plain encoding keeps insertion order, the
     * deterministic one sorts by key. */
    NSDictionary *reversed = @{@"b" : @(2),
                               @"a" : @(1)};
    NSDictionary *sortedPair = @{@"a" : @(1),
                                 @"b" : @(2)};
    /* attrs is field 14: entry {a:1} is 72 05 0A 01 61 10 01, {b:2} swaps in
     * 62/02. Deterministic output pins that order whatever NSDictionary
     * enumeration fed setEntries:. */
    static const uint8_t pinned[] = {0x72, 0x05, 0x0a, 0x01, 0x61, 0x10,
                                     0x01, 0x72, 0x05, 0x0a, 0x01, 0x62,
                                     0x10, 0x02};
    NSData *sortedWire = [NSData dataWithBytes:pinned length:sizeof(pinned)];

    probe = [schema messageByName:@"pbwobjc.Probe"];
    CHECK(probe != nil);
    CHECK([probe setEntries:reversed forField:@"attrs" error:&error]);
    sorted = [probe encodeDeterministic:&error];
    CHECK(sorted != nil);
    CHECK([sorted isEqualToData:sortedWire]);
    plain = [probe encode:&error];
    CHECK(plain != nil);
    /* A plain encode of the same entries decodes to the same map, and the
     * deterministic form is stable across insertion orders. */
    CHECK([[[probe decode:plain error:&error] entriesForField:@"attrs"]
        isEqualToDictionary:sortedPair]);
    ordered = [schema messageByName:@"pbwobjc.Probe"];
    CHECK([ordered setEntries:sortedPair forField:@"attrs" error:&error]);
    CHECK([sorted isEqualToData:[ordered encodeDeterministic:&error]]);
}

static void
testDelimited(void)
{
    PBWSchema *schema = loadSchema();
    PBWMessage *probe;
    PBWMessage *second;
    NSData *frames;
    NSUInteger consumed = 0;
    NSUInteger again = 0;
    PBWMessage *first = nil;
    PBWMessage *back = nil;
    NSError *error = nil;
    pbw_limits tiny = {1, 0};

    if (schema == nil) {
        return;
    }
    probe = filledProbe(schema);
    second = [schema messageByName:@"pbwobjc.Probe"];
    CHECK([second setValue:@"bob" forField:@"text" error:&error]);
    NSData *head = [probe encodeDelimited:&error];
    NSData *tail = [second encodeDelimited:&error];
    CHECK(head != nil && tail != nil);
    NSMutableData *both = [head mutableCopy];
    [both appendData:tail];
    frames = both;

    /* Two frames decode in a loop by advancing past *consumed. */
    first = [probe decodeDelimited:frames consumed:&consumed error:&error];
    CHECK(first != nil);
    CHECK(consumed > 0);
    CHECK_STRING([first valueForField:@"text"], @"h\u00e9llo");
    NSRange restRange = NSMakeRange(consumed, frames.length - consumed);
    back = [probe decodeDelimited:[frames subdataWithRange:restRange]
                         consumed:&again
                            error:&error];
    CHECK(back != nil);
    CHECK(consumed + again == frames.length);
    CHECK_STRING([back valueForField:@"text"], @"bob");
    /* NULL consumed is fine; a truncated frame fails. */
    CHECK([probe decodeDelimited:frames consumed:NULL error:&error] != nil);
    error = nil;
    NSData *truncated = [frames subdataWithRange:NSMakeRange(0, 2)];
    CHECK([probe decodeDelimited:truncated
                        consumed:NULL
                           error:&error] == nil);
    CHECK(error.code == PBW_ERR_INVALID);
    /* A declared frame length past max_bytes fails before the body. */
    error = nil;
    CHECK([probe decodeDelimited:frames
                        consumed:NULL
                          limits:tiny
                           error:&error] == nil);
    CHECK(error.code == PBW_ERR_LIMIT);
}

static void
testLimits(void)
{
    PBWSchema *schema = loadSchema();
    PBWMessage *probe;
    NSData *wire;
    NSError *error = nil;
    pbw_limits tinyBytes = {2, 0};
    pbw_limits hugeDepth = {0, 100000};
    pbw_limits oneDeep = {0, 1};
    pbw_limits defaults = {0, 0};
    NSString *json;
    NSString *text;

    if (schema == nil) {
        return;
    }
    probe = filledProbe(schema);
    wire = [probe encode:&error];
    CHECK(wire != nil);
    /* Defaults behave like the unbounded calls. */
    CHECK([[probe decode:wire limits:defaults error:&error] isKindOfClass:
                                                                [PBWMessage class]]);
    /* A byte cap below the payload fails; a depth past PBW_MAX_DEPTH clamps
     * to it and still decodes. */
    error = nil;
    CHECK([probe decode:wire limits:tinyBytes error:&error] == nil);
    CHECK(error.code == PBW_ERR_LIMIT);
    CHECK([probe decode:wire limits:hugeDepth error:&error] != nil);

    /* Same bounds on the text mappings. */
    json = [probe JSONString:&error];
    CHECK(json != nil);
    CHECK([probe parseJSONString:json
              ignoreUnknownFields:NO
                           limits:defaults
                            error:&error] != nil);
    error = nil;
    CHECK([probe parseJSONString:@"{\"nope\":1}"
              ignoreUnknownFields:NO
                           limits:defaults
                            error:&error] == nil);
    CHECK([probe parseJSONString:@"{\"nope\":1}"
              ignoreUnknownFields:YES
                           limits:defaults
                            error:&error] != nil);
    error = nil;
    CHECK([probe parseJSONString:json
              ignoreUnknownFields:NO
                           limits:tinyBytes
                            error:&error] == nil);
    CHECK(error.code == PBW_ERR_LIMIT);
    /* The nested message costs depth: one level is not enough. */
    error = nil;
    CHECK([probe parseJSONString:json
              ignoreUnknownFields:NO
                           limits:oneDeep
                            error:&error] == nil);
    CHECK(error.code == PBW_ERR_LIMIT);
    text = [probe textFormat:&error];
    CHECK(text != nil);
    CHECK([probe parseTextFormat:text limits:defaults error:&error] != nil);
    error = nil;
    CHECK([probe parseTextFormat:text limits:tinyBytes error:&error] == nil);
    CHECK(error.code == PBW_ERR_LIMIT);
}

static void
testCoverSchema(void)
{
    NSString *path = [[@(__FILE__) stringByDeletingLastPathComponent]
        stringByAppendingPathComponent:@"data/cover.desc"];
    NSData *bytes = [NSData dataWithContentsOfFile:path];
    NSError *error = nil;
    PBWSchema *schema;
    PBWMessage *box;
    PBWMessage *packed;
    PBWMessage *stamp;
    PBWMessage *back;
    PBWMessage *mask;
    NSData *payload;
    static const uint8_t raw[] = {0x0a, 0x03, 'a', 'b', 'c'};

    CHECK(bytes.length > 0);
    schema = [PBWSchema schemaFromData:bytes error:&error];
    CHECK(schema != nil);
    if (schema == nil) {
        return;
    }
    /* Services: one service, one method, Stamp in and out. */
    CHECK([[schema serviceNames] isEqualToArray:@[ @"pbwobjc.Lookup" ]]);
    CHECK([[schema methodNamesForService:@"pbwobjc.Lookup"]
        isEqualToArray:@[ @"Get" ]]);
    CHECK([schema methodNamesForService:@"pbwobjc.Nope"] == nil);
    NSString *inType = [schema inputTypeForService:@"pbwobjc.Lookup"
                                            method:@"Get"];
    NSString *outType = [schema outputTypeForService:@"pbwobjc.Lookup"
                                              method:@"Get"];
    CHECK_STRING(inType, @"pbwobjc.Stamp");
    CHECK_STRING(outType, @"pbwobjc.Stamp");
    CHECK([schema inputTypeForService:@"pbwobjc.Lookup" method:@"Nope"] ==
          nil);
    CHECK([schema inputTypeForService:@"pbwobjc.Nope" method:@"Get"] == nil);
    /* probe.desc carries no services: empty, not nil. */
    CHECK([loadSchema().serviceNames count] == 0);

    /* Any: pack, read back, wire round trip. */
    box = [schema messageByName:@"pbwobjc.AnyBox"];
    CHECK(box != nil);
    CHECK([box anyTypeURL] == nil);
    CHECK([box anyPayload] == nil);
    payload = [NSData dataWithBytes:raw length:sizeof(raw)];
    packed = [box packAnyWithTypeURL:@"type.googleapis.com/pkg.Msg"
                             payload:payload
                               error:&error];
    CHECK(packed != nil);
    CHECK_STRING([packed anyTypeURL], @"type.googleapis.com/pkg.Msg");
    CHECK([[packed anyPayload] isEqualToData:payload]);
    back = [box decode:[packed encode:&error] error:&error];
    CHECK(back != nil);
    CHECK_STRING([back anyTypeURL], @"type.googleapis.com/pkg.Msg");
    CHECK([[back anyPayload] isEqualToData:payload]);

    /* Timestamp / Duration millis round trip (1500 ms = 1 s + 500 ms). */
    stamp = [schema messageByName:@"pbwobjc.Stamp"];
    CHECK(stamp != nil);
    CHECK([stamp timestampMillis:&error] == nil);
    back = [stamp messageWithTimestampMillis:1500 error:&error];
    CHECK(back != nil);
    CHECK([[back timestampMillis:&error] isEqualToNumber:@(1500)]);
    CHECK([[[stamp decode:[back encode:&error] error:&error]
        timestampMillis:&error] isEqualToNumber:@(1500)]);
    back = [stamp messageWithDurationMillis:-2500 error:&error];
    CHECK(back != nil);
    CHECK([[back durationMillis:&error] isEqualToNumber:@(-2500)]);

    /* FieldMask paths without materializing the list. */
    mask = [schema messageByName:@"pbwobjc.Mask"];
    CHECK(mask != nil);
    CHECK([[mask fieldMaskPaths] count] == 0);
    NSArray *paths = @[ @"a/b", @"c" ];
    CHECK([mask setValues:paths forField:@"paths" error:&error]);
    CHECK([[mask fieldMaskPaths] isEqualToArray:paths]);
}

static void
testContextPool(void)
{
    PBWSchema *schema = loadSchema();
    PBWMessage *probe;
    NSData *wire;
    NSError *error = nil;
    PBWContext *ctx;
    PBWMessage *held;
    PBWPool *pool;
    PBWContext *pooled;
    PBWMessage *pooledBack;

    if (schema == nil) {
        return;
    }
    probe = filledProbe(schema);
    wire = [probe encode:&error];
    CHECK(wire != nil);
    ctx = [[PBWContext alloc] init];
    CHECK(ctx != nil);
    ctx.retainCap = 1 << 20;
    CHECK(ctx.retainCap == 1 << 20);
    held = [probe decode:wire withContext:ctx error:&error];
    CHECK(held != nil);
    CHECK_STRING([held valueForField:@"text"], @"h\u00e9llo");
    CHECK([[[held encodeWithContext:ctx error:&error] copy] isEqualToData:wire]);
    /* A second decode through the same workspace agrees as well. */
    CHECK([[[[probe decode:wire withContext:ctx error:&error] encode:&error] copy] isEqualToData:wire]);
    CHECK(ctx.usedBytes > 0);
    [ctx reset];
    /* A pooled workspace round-trips the same bytes. */
    pool = [[PBWPool alloc] initWithMaxIdle:2 retainCap:0];
    CHECK(pool != nil);
    CHECK([pool idleCount] == 0);
    pooled = [pool acquire];
    CHECK(pooled != nil);
    pooledBack = [probe decode:wire withContext:pooled error:&error];
    CHECK(pooledBack != nil);
    CHECK([[[pooledBack encode:&error] copy] isEqualToData:wire]);
    pooledBack = nil;
    [pool releaseContext:pooled];
    CHECK([pool idleCount] == 1);
    pooled = nil;
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
        testRepeatedHelpers();
        testMapHelpers();
        testJSONText();
        testDeterministic();
        testDelimited();
        testLimits();
        testCoverSchema();
        testContextPool();

        if (failures == 0) {
            printf("test_objc: all checks passed (%s)\n",
                   descriptorPath.UTF8String);
            return 0;
        }
        fprintf(stderr, "test_objc: %d check(s) failed\n", failures);
        return 1;
    }
}

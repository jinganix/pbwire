/* Objective-C binding example: load a descriptor set at runtime, build a
 * message field by field, encode, decode, assert, print.
 *
 *   ./build.sh          (builds and runs this after the self-check)
 *   ./build/roundtrip   (after the first build; optional .desc path argument)
 *
 * No generated code and no protoc at build time: the .desc file is the schema.
 */

#import <Foundation/Foundation.h>

#import "PBWMessage.h"
#import "PBWSchema.h"

#include <stdio.h>

static NSString *
defaultDescriptorPath(void)
{
    NSString *source = @(__FILE__);

    return [[[source stringByDeletingLastPathComponent]
        stringByDeletingLastPathComponent]
        stringByAppendingPathComponent:@"tests/data/probe.desc"];
}

int
main(int argc, char **argv)
{
    @autoreleasepool {
        NSString *path = argc > 1 ? @(argv[1]) : defaultDescriptorPath();
        NSError *error = nil;
        PBWSchema *schema = [PBWSchema schemaFromFile:path error:&error];
        PBWMessage *message;
        PBWMessage *inner;
        PBWMessage *decoded;
        NSData *wire;

        if (schema == nil) {
            fprintf(stderr, "load %s: %s\n", path.UTF8String,
                    error.localizedDescription.UTF8String);
            return 1;
        }

        message = [schema messageByName:@"pbwobjc.Probe"];
        if (message == nil) {
            fprintf(stderr, "no pbwobjc.Probe in %s\n", path.UTF8String);
            return 1;
        }
        [message setValue:@(1234) forField:@"i32" error:&error];
        [message setValue:@"alice" forField:@"text" error:&error];
        [message setValue:@YES forField:@"flag" error:&error];
        [message setValue:@(1) forField:@"color" error:&error];
        inner = [message addMessageForField:@"inner" error:&error];
        [inner setValue:@"child" forField:@"label" error:&error];
        [inner setValue:@(7) forField:@"n" error:&error];

        wire = [message encode:&error];
        if (wire == nil) {
            fprintf(stderr, "encode: %s\n", error.localizedDescription.UTF8String);
            return 1;
        }

        decoded = [message decode:wire error:&error];
        if (decoded == nil) {
            fprintf(stderr, "decode: %s\n", error.localizedDescription.UTF8String);
            return 1;
        }

        /* The round trip is byte-exact: unknown fields and field order survive. */
        NSData *again = [decoded encode:&error];

        assert([[decoded valueForField:@"text"] isEqual:@"alice"]);
        assert([[decoded valueForField:@"i32"] isEqualToNumber:@(1234)]);
        assert([[decoded valueForField:@"flag"] isEqualToNumber:@YES]);
        assert([[[decoded messageForField:@"inner"] valueForField:@"label"]
            isEqual:@"child"]);
        assert([[[decoded messageForField:@"inner"] valueForField:@"n"]
            isEqualToNumber:@(7)]);
        assert([wire isEqualToData:again]);

        printf("%s: %lu byte(s), text=%s, inner.label=%s, color=%d\n",
               decoded.messageName.UTF8String, (unsigned long)wire.length,
               [[decoded valueForField:@"text"] UTF8String],
               [[[decoded messageForField:@"inner"] valueForField:@"label"]
                   UTF8String],
               [(NSNumber *)[decoded valueForField:@"color"] intValue]);
        return 0;
    }
}

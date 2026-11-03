/* pbwire Objective-C binding: a FileDescriptorSet becomes a schema at runtime.

   No code generation: load the bytes `protoc --descriptor_set_out` writes
   (use --include_imports) and ask the schema for a message by its full name.
   Messages are PBWMessage, a thin ARC wrapper over the C ABI.

     NSError *error = nil;
     PBWSchema *schema = [PBWSchema schemaFromFile:@"probe.desc" error:&error];
     PBWMessage *message = [schema messageByName:@"pbwobjc.Probe"];

   Hold on to the schema for as long as its messages are alive (they retain it,
   so keeping a message is enough). */

#import <Foundation/Foundation.h>

#include "pbwire.h"

@class PBWMessage;

NS_ASSUME_NONNULL_BEGIN

/* Domain of every NSError the binding produces. The error code is the
 * pbw_status of the failing call (PBW_ERR_INVALID, PBW_ERR_NOT_FOUND, ...), so
 * the C enum doubles as the ObjC error code. Foundation file errors
 * (NSCocoaErrorDomain) are passed through untouched by schemaFromFile:. */
extern NSString *const PBWErrorDomain;

@interface PBWSchema : NSObject

/* Load a schema from a serialized FileDescriptorSet. On failure returns nil
 * with *error set, including the reason from pbw_schema_error(). */
+ (nullable instancetype)schemaFromFile:(NSString *)path
                                  error:(NSError **)error;
+ (nullable instancetype)schemaFromData:(NSData *)data
                                  error:(NSError **)error;

/* An empty message of that type, or nil when the name is unknown or names an
 * enum. The returned message retains this schema. */
- (nullable PBWMessage *)messageByName:(NSString *)name;

@end

NS_ASSUME_NONNULL_END

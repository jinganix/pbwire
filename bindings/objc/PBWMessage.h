/* pbwire Objective-C binding: a message tree wrapped for ARC.

   Field access is by name and takes/returns Foundation objects (NSNumber,
   NSString, NSData, PBWMessage); the field's type in the schema decides the
   wire type, so an NSNumber is written as whatever the .proto declared.

     PBWMessage *m = [schema messageByName:@"pbwobjc.Probe"];
     [m setValue:@42 forField:@"i32" error:&error];
     [m setValue:@"hi" forField:@"text" error:&error];
     NSData *wire = [m encode:&error];
     PBWMessage *back = [m decode:wire error:&error];
     NSNumber *i32 = [back valueForField:@"i32"];   // @42

   A message owns the C tree it was created from (messageByName:, decode:) and
   frees it in -dealloc. A nested message is a view into its parent's tree: it
   does not free anything, and it retains the parent so the view cannot outlive
   the storage. */

#import <Foundation/Foundation.h>

#include "pbwire.h"

@class PBWSchema;

NS_ASSUME_NONNULL_BEGIN

@interface PBWMessage : NSObject

/* Full name of the message type ("pkg.Outer.Inner"). */
@property(nonatomic, readonly) NSString *messageName;

/* Internal: PBWSchema's -messageByName: uses this to make an empty message.
 * It retains schema, which owns the descriptor. */
+ (nullable instancetype)messageWithDescriptor:(const pbw_descriptor *)descriptor
                                        schema:(PBWSchema *)schema
                                         error:(NSError **)error;

/* YES when the field has a value (it was set here or arrived on the wire). */
- (BOOL)hasField:(NSString *)name;

/* Present value of a singular field: NSNumber for every numeric type, bool and
 * enum; NSString for string; NSData for bytes; PBWMessage for a message field;
 * nil when the field is absent, unknown or repeated. A string field whose
 * bytes are not valid UTF-8 also reads as nil. */
- (nullable id)valueForField:(NSString *)name;

/* Store a value of a singular field, replacing the previous one; nil clears
 * it. Accepted classes: NSNumber for numeric/bool/enum fields, NSString for
 * string (written as UTF-8) and NSData for bytes. Fails with PBW_ERR_TYPE for
 * another class, an out-of-range 32-bit value, or a message field (use
 * addMessageForField:); PBW_ERR_NOT_FOUND for an unknown field; and
 * PBW_ERR_INVALID for a repeated field, which this wrapper does not wrap. */
- (BOOL)setValue:(nullable id)value
        forField:(NSString *)name
           error:(NSError **)error;

/* The nested message of a singular message field, or nil when it is absent. */
- (nullable PBWMessage *)messageForField:(NSString *)name;

/* Get-or-create the nested message of a singular message field, filling it in
 * place. The C API owns the returned tree; it encodes with the parent. */
- (nullable PBWMessage *)addMessageForField:(NSString *)name
                                      error:(NSError **)error;

/* Wire bytes of this message. */
- (nullable NSData *)encode:(NSError **)error;

/* Decode data as a message of the same type. */
- (nullable PBWMessage *)decode:(NSData *)data error:(NSError **)error;

@end

NS_ASSUME_NONNULL_END

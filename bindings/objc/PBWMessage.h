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
@class PBWContext;

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
 * PBW_ERR_INVALID for a repeated field (use addValue:forField:error:). */
- (BOOL)setValue:(nullable id)value
        forField:(NSString *)name
           error:(NSError **)error;

/* Appends one item to a repeated field (or sets a singular one). Same value
 * classes as setValue:forField:error:; a message item uses
 * addMessageForField: instead. */
- (BOOL)addValue:(id)value forField:(NSString *)name error:(NSError **)error;

/* Items in a repeated field, 0 when the field is absent or singular. */
- (NSUInteger)countForField:(NSString *)name;

/* Every item of a repeated field as an NSArray (same objects as
 * valueForField:atIndex:); nil when the field is not repeated. */
- (nullable NSArray *)valuesForField:(NSString *)name;

/* Replaces a repeated field with the items of an array (nil clears it).
 * Same value classes as addValue:forField:error:. */
- (BOOL)setValues:(nullable NSArray *)values
         forField:(NSString *)name
            error:(NSError **)error;

/* A map field as an NSDictionary: entry keys (NSString for string keys,
 * NSNumber otherwise) to entry values (same objects as valueForField:).
 * nil when the field is not a map. */
- (nullable NSDictionary *)entriesForField:(NSString *)name;

/* Replaces a map field with the entries of a dictionary (nil clears it).
 * Keys are NSString for string keys and NSNumber otherwise; values follow
 * setValue:forField:error:. */
- (BOOL)setEntries:(nullable NSDictionary *)entries
          forField:(NSString *)name
             error:(NSError **)error;

/* One item of a repeated field: NSNumber for numeric/bool/enum, NSString for
 * string, NSData for bytes, PBWMessage for a message field; nil when the field
 * is not repeated or the index is out of range. */
- (nullable id)valueForField:(NSString *)name atIndex:(NSUInteger)index;

/* The nested message of a singular message field, or nil when it is absent. */
- (nullable PBWMessage *)messageForField:(NSString *)name;

/* Get-or-create the nested message of a message field, filling it in place: a
 * singular field replaces its value, a repeated one appends. The C API owns
 * the returned tree; it encodes with the parent. */
- (nullable PBWMessage *)addMessageForField:(NSString *)name
                                      error:(NSError **)error;

/* Wire bytes of this message. */
- (nullable NSData *)encode:(NSError **)error;

/* Wire bytes through a workspace's reusable buffer (copied out, so the
 * result outlives the next use of the context). */
- (nullable NSData *)encodeWithContext:(PBWContext *)context
                                 error:(NSError **)error;

/* JSON mapping text of this message. */
- (nullable NSString *)JSONString:(NSError **)error;

/* Parses JSON mapping text into a message of the same type. */
- (nullable PBWMessage *)parseJSONString:(NSString *)text
                                   error:(NSError **)error;

/* Protobuf text format of this message. */
- (nullable NSString *)textFormat:(NSError **)error;

/* Parses text format into a message of the same type. */
- (nullable PBWMessage *)parseTextFormat:(NSString *)text
                                   error:(NSError **)error;

/* Decode data as a message of the same type. */
- (nullable PBWMessage *)decode:(NSData *)data error:(NSError **)error;

/* Decode into a workspace: the message lives in its allocator (no per-value
 * malloc after warmup), so it retains the context, frees nothing itself, and
 * becomes invalid on the context's next reset. It is read-only: setters
 * fail the way the C API fails them. */
- (nullable PBWMessage *)decode:(NSData *)data
                    withContext:(PBWContext *)context
                          error:(NSError **)error;

@end

NS_ASSUME_NONNULL_END

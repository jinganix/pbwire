/* pbwire Objective-C binding: a reusable encode/decode workspace.

   A context owns a bump allocator and a reusable encode buffer, so a program
   that decodes or encodes in a loop stops calling malloc after the first few
   messages. One context per thread; never share one concurrently (the C core
   is dependency-free and unsynchronized).

   Messages decoded with a context live in its allocator: -reset invalidates
   them, and they cannot be freed individually (PBWMessage handles that: a
   context message frees nothing and retains its context).

     PBWContext *ctx = [[PBWContext alloc] init];
     ctx.retainCap = 1 << 20;
     PBWMessage *m = [probe decode:wire withContext:ctx error:&error];
     [ctx reset]; // m is now invalid */

#import <Foundation/Foundation.h>

#include "pbwire.h"

NS_ASSUME_NONNULL_BEGIN

@interface PBWContext : NSObject

/* A fresh workspace (1 MiB default retain cap, like the C API). */
- (instancetype)init;

/* Max bytes kept resident across resets (decode arena head + encode
 * buffer); 0 keeps nothing. Applies lazily at the next reset. */
@property(nonatomic) NSUInteger retainCap;

/* Bytes currently held (sizing/debugging, the printf side). */
@property(nonatomic, readonly) NSUInteger usedBytes;

/* Releases every allocation made since the last reset, keeping the buffers.
 * Every message decoded with this context becomes invalid. */
- (void)reset;

/* Internal: the wrapped workspace. A context from -[PBWPool acquire] shares
 * its workspace with the pool: do not free it, release it instead. */
@property(nonatomic, readonly) pbw_ctx *ctx;

/* Internal: wrap an existing workspace (PBWPool's acquire path). */
- (nullable instancetype)initWithCtx:(pbw_ctx *)ctx owned:(BOOL)owned;

@end

NS_ASSUME_NONNULL_END

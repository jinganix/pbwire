/* pbwire Objective-C binding: a bounded freelist of workspaces.

   Acquire instead of PBWContext.new, release instead of reset/free. A
   released context is already reset, so the next acquire can use it
   directly; releases past maxIdle are freed. One pool per thread, never
   shared concurrently (like the C core: unsynchronized).

   A context is for exclusive use until released; release it before freeing
   the pool, and never use one context from two threads at once.

     PBWPool *pool = [[PBWPool alloc] initWithMaxIdle:4 retainCap:0];
     PBWContext *ctx = [pool acquire];\n     PBWMessage *m = [probe decode:wire withContext:ctx error:&error];\n     [pool releaseContext:ctx]; */

#import <Foundation/Foundation.h>

#include "pbwire.h"

@class PBWContext;

NS_ASSUME_NONNULL_BEGIN

@interface PBWPool : NSObject

/* maxIdle bounds the freelist (0 keeps nothing: acquire degenerates to\n * new/free); retainCap is stamped on contexts the pool creates (0 is the\n * 1 MiB default). */
- (nullable instancetype)initWithMaxIdle:(NSUInteger)maxIdle
                               retainCap:(NSUInteger)retainCap;

/* Exclusive use until released; nil when out of memory. */
- (nullable PBWContext *)acquire;

/* Releases a context from -acquire back to the freelist (already reset). */
- (void)releaseContext:(PBWContext *)context;

/* Idle contexts held now (sizing/debugging, the printf side). */
- (NSUInteger)idleCount;

@end

NS_ASSUME_NONNULL_END

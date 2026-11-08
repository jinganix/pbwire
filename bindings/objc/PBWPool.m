#import "PBWPool.h"

#import "PBWContext.h"

@implementation PBWPool {
    pbw_pool *_pool;
}

- (nullable instancetype)initWithMaxIdle:(NSUInteger)maxIdle
                               retainCap:(NSUInteger)retainCap
{
    self = [super init];
    if (self == nil) {
        return nil;
    }
    _pool = pbw_pool_new(maxIdle, retainCap);
    if (_pool == NULL) {
        return nil;
    }
    return self;
}

- (void)dealloc
{
    /* Outstanding contexts keep working until released; releasing into a
     * freed pool is undefined, so release first (same rule as the C API). */
    pbw_pool_free(_pool);
    _pool = NULL;
}

- (nullable PBWContext *)acquire
{
    pbw_ctx *ctx = pbw_pool_acquire(_pool);

    if (ctx == NULL) {
        return nil;
    }
    return [[PBWContext alloc] initWithCtx:ctx owned:NO];
}

- (void)releaseContext:(PBWContext *)context
{
    if (![context markReleased]) {
        return; /* second release of one wrapper: nothing to do */
    }
    pbw_pool_release(_pool, context.ctx);
}

- (NSUInteger)idleCount
{
    return pbw_pool_idle(_pool);
}

@end

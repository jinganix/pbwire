#import "PBWContext.h"

@implementation PBWContext {
    pbw_ctx *_ctx;
    BOOL _ownsCtx;
    BOOL _released;
}

@synthesize retainCap = _retainCap;

- (instancetype)init
{
    return [self initWithCtx:pbw_ctx_new() owned:YES];
}

- (instancetype)initWithCtx:(pbw_ctx *)ctx owned:(BOOL)owned
{
    self = [super init];
    if (self == nil) {
        return nil;
    }
    if (ctx == NULL) {
        return nil;
    }
    _ctx = ctx;
    _ownsCtx = owned;
    return self;
}

- (void)dealloc
{
    /* A pooled workspace belongs to its pool: dropping the wrapper without
     * -[PBWPool releaseContext:] leaks it back nowhere, so release first. */
    if (_ownsCtx) {
        pbw_ctx_free(_ctx);
    }
    _ctx = NULL;
}

/* A wrapper the pool took back must not touch the workspace: the pool may
 * have handed it to another owner. */
- (void)requireLive
{
    if (_released) {
        [NSException raise:NSInternalInconsistencyException
                    format:@"pbwire: context was released to its pool"];
    }
}

- (pbw_ctx *)ctx
{
    [self requireLive];
    return _ctx;
}

- (NSUInteger)retainCap
{
    /* No getter in the C API; the wrapper tracks the last set value. */
    return _retainCap;
}

- (void)setRetainCap:(NSUInteger)retainCap
{
    [self requireLive];
    _retainCap = retainCap;
    pbw_ctx_set_retain_cap(_ctx, retainCap);
}

- (NSUInteger)usedBytes
{
    [self requireLive];
    return pbw_ctx_used(_ctx);
}

- (void)reset
{
    [self requireLive];
    pbw_ctx_reset(_ctx);
}

/* One-shot claim for -[PBWPool releaseContext:]: a second release of the same
 * wrapper must not push the same workspace into the freelist twice (that is a
 * double free the pool cannot see). */
- (pbw_ctx *)markReleased
{
    if (_released) {
        return NULL;
    }
    _released = YES;
    return _ctx;
}

@end

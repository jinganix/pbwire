/* Reusable per-caller workspace: one bump allocator for decoded messages and one
 * growable buffer for encoded output. This is the explicit, thread-safe-by-
 * construction version of pbwire's per-scheduler thread data. */

#include "internal.h"

struct pbw_ctx {
    pbw_arena arena;
    pbw_buf out;
    /* Opaque binding slot (Erlang NIF thread data, Go state, ...). Set once
     * by the binding; the core never reads it, and reset/pool keep it. */
    void *user;
};

/* Bounded freelist of contexts. Unsynchronized by design (the core stays
 * C99 with no dependencies): one pool per worker thread, never shared
 * concurrently. Preallocation is lazy: the first acquire allocates, steady
 * use then stops calling malloc. */
struct pbw_pool {
    pbw_ctx **idle;
    size_t count;
    size_t cap;        /* max_idle: how many a release keeps */
    size_t idle_cap;   /* allocated slots in idle[] */
    size_t retain_cap; /* budget stamped on contexts this pool creates */
};

int
pbw_abi_version(void)
{
    return PBW_ABI_VERSION;
}

pbw_ctx *
pbw_ctx_new(void)
{
    pbw_ctx *ctx = (pbw_ctx *)calloc(1, sizeof(*ctx));

    if (ctx == NULL) {
        return NULL;
    }
    ctx->arena.chunk_size = 4096;
    /* Writable: an empty context holds nothing to protect. Decoding seals
     * it read-only; resetting returns it to this state. */
    ctx->arena.readonly = 0;
    ctx->arena.retain_cap = PBW_ARENA_LARGE_THRESHOLD;
    pbw_buf_init(&ctx->out);
    return ctx;
}

pbw_ctx *
pbw_ctx_new_mem(void *mem, size_t len)
{
    pbw_ctx *ctx;
    size_t hdr;

    if (mem == NULL || ((uintptr_t)mem & 7u) != 0u) {
        /* 8-byte alignment keeps every value slot naturally aligned. */
        return NULL;
    }
    hdr = (sizeof(*ctx) + 7u) & ~(size_t)7u;
    if (len < hdr + PBW_CHUNK_HDR + 8u) {
        return NULL;
    }
    ctx = (pbw_ctx *)mem;
    memset(ctx, 0, sizeof(*ctx));
    pbw_buf_init(&ctx->out);
    /* No heap here: pbw_encode_ctx() reports NOMEM instead of allocating; the
     * encoder for this kind of context is pbw_encode_to(caller memory). */
    ctx->out.fixed = 1;
    if (!pbw_arena_init_mem(&ctx->arena, (uint8_t *)mem + hdr, len - hdr)) {
        return NULL;
    }
    return ctx;
}

void
pbw_ctx_free(pbw_ctx *ctx)
{
    if (ctx == NULL) {
        return;
    }
    if (ctx->arena.no_grow) {
        /* Caller memory (pbw_ctx_new_mem): the handle and the arena belong to
         * the caller, so closing releases nothing and the block stays valid. */
        return;
    }
    pbw_arena_free(&ctx->arena);
    pbw_buf_free(&ctx->out);
    free(ctx);
}

void
pbw_ctx_set_user(pbw_ctx *ctx, void *user)
{
    if (ctx != NULL) {
        ctx->user = user;
    }
}

void *
pbw_ctx_user(const pbw_ctx *ctx)
{
    return ctx != NULL ? ctx->user : NULL;
}

void
pbw_ctx_set_retain_cap(pbw_ctx *ctx, size_t cap)
{
    if (ctx == NULL) {
        return;
    }
    ctx->arena.retain_cap = cap;
}

void
pbw_ctx_reset(pbw_ctx *ctx)
{
    if (ctx == NULL) {
        return;
    }
    pbw_arena_reset(&ctx->arena);
    if (ctx->out.cap > ctx->arena.retain_cap) {
        /* Same rule as the arena head: an oversized encode buffer is freed
         * instead of staying resident. A steady workload under budget keeps
         * reusing it with no malloc. */
        pbw_buf_free(&ctx->out);
    }
    ctx->out.len = 0;
    ctx->out.oom = 0;
}

size_t
pbw_ctx_used(const pbw_ctx *ctx)
{
    return ctx == NULL ? 0 : pbw_arena_used(&ctx->arena);
}

pbw_pool *
pbw_pool_new(size_t max_idle, size_t retain_cap)
{
    pbw_pool *pool = (pbw_pool *)calloc(1, sizeof(*pool));

    if (pool == NULL) {
        return NULL;
    }
    pool->retain_cap = retain_cap > 0 ? retain_cap
                                      : PBW_ARENA_LARGE_THRESHOLD;
    pool->cap = max_idle;
    return pool;
}

void
pbw_pool_free(pbw_pool *pool)
{
    size_t i;

    if (pool == NULL) {
        return;
    }
    for (i = 0; i < pool->count; i++) {
        pbw_ctx_free(pool->idle[i]);
    }
    free(pool->idle);
    free(pool);
}

pbw_ctx *
pbw_pool_acquire(pbw_pool *pool)
{
    pbw_ctx *ctx;

    if (pool == NULL) {
        return NULL;
    }
    if (pool->count > 0) {
        return pool->idle[--pool->count];
    }
    ctx = pbw_ctx_new();
    if (ctx == NULL) {
        return NULL;
    }
    pbw_ctx_set_retain_cap(ctx, pool->retain_cap);
    return ctx;
}

void
pbw_pool_release(pbw_pool *pool, pbw_ctx *ctx)
{
    pbw_ctx **slot;

    if (pool == NULL || ctx == NULL) {
        return;
    }
    /* A released context is always clean: the next acquire can use it
     * without resetting. The retain cap bounds what the reset keeps, so
     * an outlier never pins the pool. */
    pbw_ctx_reset(ctx);
    if (pool->count >= pool->cap) {
        pbw_ctx_free(ctx);
        return;
    }
    /* Grow the slot array by doubling: one realloc per doubling instead of
     * one per release, which matters for a pool that fills up. */
    if (pool->count == pool->idle_cap) {
        size_t want;

        if (pool->idle_cap > SIZE_MAX / 2 / sizeof(*slot)) {
            pbw_ctx_free(ctx);
            return;
        }
        want = pool->idle_cap != 0 ? pool->idle_cap * 2 : 8;
        if (want > pool->cap) {
            want = pool->cap;
        }
        if (want <= pool->idle_cap || want > SIZE_MAX / sizeof(*slot)) {
            pbw_ctx_free(ctx);
            return;
        }
        slot = (pbw_ctx **)realloc(pool->idle, want * sizeof(*slot));
        if (slot == NULL) {
            pbw_ctx_free(ctx);
            return;
        }
        pool->idle = slot;
        pool->idle_cap = want;
    }
    pool->idle[pool->count++] = ctx;
}

size_t
pbw_pool_idle(const pbw_pool *pool)
{
    return pool == NULL ? 0 : pool->count;
}

pbw_status
pbw_decode_ctx(pbw_ctx *ctx, const pbw_descriptor *desc, const void *buf,
               size_t len, pbw_message **out)
{
    return pbw_decode_ctx_limits(ctx, desc, buf, len, NULL, out);
}

pbw_status
pbw_decode_ctx_limits(pbw_ctx *ctx, const pbw_descriptor *desc,
                      const void *buf, size_t len, const pbw_limits *limits,
                      pbw_message **out)
{
    if (out != NULL) {
        *out = NULL;
    }
    if (ctx == NULL || out == NULL || (buf == NULL && len > 0)) {
        return PBW_ERR_INVALID;
    }
    /* Every failure below leaves NULL: callers can rely on *out alone. */
    if (limits != NULL && limits->max_bytes > 0 && len > limits->max_bytes) {
        return PBW_ERR_LIMIT;
    }
    {
        pbw_status status =
            pbw_decode_arena_lim(&ctx->arena, desc, buf, len, pbw_effective_depth(limits), out);
        if (status == PBW_OK) {
            /* Seal the arena: decoded trees are read-only, later builds
             * need a reset first. */
            ctx->arena.readonly = 1;
        }
        return status;
    }
}

pbw_status
pbw_encode_ctx(pbw_ctx *ctx, const pbw_message *msg, const uint8_t **out,
               size_t *len)
{
    pbw_status status;

    if (ctx == NULL || out == NULL || len == NULL) {
        return PBW_ERR_INVALID;
    }
    *out = NULL;
    *len = 0;
    status = pbw_encode_buf(&ctx->out, msg);
    if (status != PBW_OK) {
        *out = NULL;
        *len = 0;
        return status;
    }
    *out = ctx->out.data != NULL ? ctx->out.data : (const uint8_t *)"";
    *len = ctx->out.len;
    return PBW_OK;
}

pbw_message *
pbw_message_new_ctx(pbw_ctx *ctx, const pbw_descriptor *desc)
{
    if (ctx == NULL) {
        return NULL;
    }
    return pbw_message_new_a(&ctx->arena, desc);
}

pbw_value *
pbw_value_new_uint_ctx(pbw_ctx *ctx, uint64_t bits)
{
    if (ctx == NULL) {
        return NULL;
    }
    return pbw_value_new_uint_a(&ctx->arena, bits);
}

pbw_value *
pbw_value_new_bytes_ctx(pbw_ctx *ctx, const void *data, size_t len)
{
    if (ctx == NULL) {
        return NULL;
    }
    return pbw_value_new_bytes_a(&ctx->arena, data, len);
}

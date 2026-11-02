/* Reusable per-caller workspace: one bump allocator for decoded messages and one
 * growable buffer for encoded output. This is the explicit, thread-safe-by-
 * construction version of enif_protobuf's per-scheduler thread data. */

#include "internal.h"

struct pbw_ctx {
    pbw_arena arena;
    pbw_buf out;
};

pbw_ctx *
pbw_ctx_new(void)
{
    pbw_ctx *ctx = (pbw_ctx *)calloc(1, sizeof(*ctx));

    if (ctx == NULL) {
        return NULL;
    }
    ctx->arena.chunk_size = 4096;
    pbw_buf_init(&ctx->out);
    return ctx;
}

void
pbw_ctx_free(pbw_ctx *ctx)
{
    if (ctx == NULL) {
        return;
    }
    pbw_arena_free(&ctx->arena);
    pbw_buf_free(&ctx->out);
    free(ctx);
}

void
pbw_ctx_reset(pbw_ctx *ctx)
{
    if (ctx == NULL) {
        return;
    }
    pbw_arena_reset(&ctx->arena);
    ctx->out.len = 0;
}

size_t
pbw_ctx_used(const pbw_ctx *ctx)
{
    return ctx == NULL ? 0 : pbw_arena_used(&ctx->arena);
}

pbw_status
pbw_decode_ctx(pbw_ctx *ctx, const pbw_descriptor *desc, const void *buf,
               size_t len, pbw_message **out)
{
    if (ctx == NULL) {
        return PBW_ERR_INVALID;
    }
    return pbw_decode_arena(&ctx->arena, desc, buf, len, out);
}

pbw_status
pbw_encode_ctx(pbw_ctx *ctx, const pbw_message *msg, const uint8_t **out,
               size_t *len)
{
    pbw_status status;

    if (ctx == NULL || out == NULL || len == NULL) {
        return PBW_ERR_INVALID;
    }
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

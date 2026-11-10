/* Static-memory facade over the pbwire C core: see pbwe.h. */

#include "pbwe.h"

/* Arg checks plus the startup load, shared by the heap and caller-memory
 * opens. On failure box->schema holds the partial set so the box still
 * explains itself; the caller closes it either way. */
static pbw_status
box_load(pbwe_box *box, const void *desc, size_t len, const pbw_limits *desc_limits)
{
    if (box == NULL || desc == NULL || len == 0) {
        return PBW_ERR_INVALID;
    }
    if (box->schema != NULL || box->ctx != NULL) {
        /* Opening an open box would leak the schema and the arena. */
        return PBW_ERR_INVALID;
    }
    box->limits.max_bytes = 0;
    box->limits.max_depth = 0;
    return pbw_descriptor_set_load_limits(desc, len, desc_limits, &box->schema);
}

pbw_status
pbwe_open(pbwe_box *box, const void *desc, size_t len, size_t retain_cap,
          const pbw_limits *desc_limits)
{
    pbw_status st;

    st = box_load(box, desc, len, desc_limits);
    if (st != PBW_OK) {
        /* box->schema holds the partial set, so pbw_schema_error() explains. */
        return st;
    }
    box->ctx = pbw_ctx_new();
    if (box->ctx == NULL) {
        pbw_schema_free(box->schema);
        box->schema = NULL;
        return PBW_ERR_NOMEM;
    }
    if (retain_cap > 0) {
        pbw_ctx_set_retain_cap(box->ctx, retain_cap);
    }
    return PBW_OK;
}

pbw_status
pbwe_open_mem(pbwe_box *box, const void *desc, size_t len, void *mem, size_t mem_len,
              const pbw_limits *desc_limits)
{
    pbw_ctx *ctx;
    pbw_status st;

    /* The context is checked before anything is loaded: a block too small for
     * a context leaves the box closed rather than half-open. A misaligned or
     * NULL block is a caller error, not a size one, so it says INVALID. */
    if (mem == NULL || ((uintptr_t)mem & 7u) != 0u) {
        return PBW_ERR_INVALID;
    }
    ctx = pbw_ctx_new_mem(mem, mem_len);
    if (ctx == NULL) {
        return PBW_ERR_NOMEM;
    }
    st = box_load(box, desc, len, desc_limits);
    if (st != PBW_OK) {
        return st;
    }
    box->ctx = ctx;
    return PBW_OK;
}

void
pbwe_close(pbwe_box *box)
{
    if (box == NULL) {
        return;
    }
    pbw_ctx_free(box->ctx);
    pbw_schema_free(box->schema);
    box->ctx = NULL;
    box->schema = NULL;
}

void
pbwe_set_limits(pbwe_box *box, const pbw_limits *limits)
{
    if (box == NULL) {
        return;
    }
    if (limits == NULL) {
        box->limits.max_bytes = 0;
        box->limits.max_depth = 0;
        return;
    }
    box->limits = *limits;
}

pbw_status
pbwe_encode(const pbw_message *msg, void *out, size_t cap, size_t *len)
{
    return pbw_encode_to(msg, out, cap, len);
}

pbw_status
pbwe_encode_delimited(const pbw_message *msg, void *out, size_t cap, size_t *len)
{
    return pbw_encode_delimited_to(msg, out, cap, len);
}

pbw_status
pbwe_decode(pbwe_box *box, const char *type, const void *buf, size_t len,
            const pbw_message **msg)
{
    const pbw_descriptor *desc;

    if (msg == NULL) {
        return PBW_ERR_INVALID;
    }
    *msg = NULL;
    if (box == NULL || box->ctx == NULL || box->schema == NULL || type == NULL) {
        return PBW_ERR_INVALID;
    }
    desc = pbw_schema_find(box->schema, type);
    if (desc == NULL) {
        return PBW_ERR_NOT_FOUND;
    }
    return pbwe_decode_desc(box, desc, buf, len, msg);
}

pbw_status
pbwe_decode_desc(pbwe_box *box, const pbw_descriptor *desc, const void *buf, size_t len,
                 const pbw_message **msg)
{
    pbw_message *m = NULL;
    pbw_status st;

    if (msg == NULL) {
        return PBW_ERR_INVALID;
    }
    /* The reset below invalidates the previous tree; a failure leaves the
     * caller with NULL, never with a pointer into freed arena memory. */
    *msg = NULL;
    if (box == NULL || box->ctx == NULL || box->schema == NULL || desc == NULL) {
        return PBW_ERR_INVALID;
    }
    if (buf == NULL && len > 0) {
        return PBW_ERR_INVALID;
    }
    pbw_ctx_reset(box->ctx);
    st = pbw_decode_ctx_limits(box->ctx, desc, buf, len, &box->limits, &m);
    if (st != PBW_OK) {
        return st;
    }
    *msg = m;
    return PBW_OK;
}

pbw_message *
pbwe_build(pbwe_box *box, const pbw_descriptor *desc)
{
    if (box == NULL || box->ctx == NULL || box->schema == NULL || desc == NULL) {
        return NULL;
    }
    /* A decode seals the arena read-only; the reset makes the build writable. */
    pbw_ctx_reset(box->ctx);
    return pbw_message_new_ctx(box->ctx, desc);
}

void
pbwe_reset(pbwe_box *box)
{
    if (box == NULL || box->ctx == NULL) {
        return;
    }
    pbw_ctx_reset(box->ctx);
}

pbw_status
pbwe_decode_delimited(pbwe_box *box, const char *type, const void *buf, size_t len,
                      const pbw_message **msg, size_t *consumed)
{
    const pbw_descriptor *desc;

    if (msg == NULL || consumed == NULL) {
        return PBW_ERR_INVALID;
    }
    *msg = NULL;
    *consumed = 0;
    if (box == NULL || box->ctx == NULL || box->schema == NULL || type == NULL) {
        return PBW_ERR_INVALID;
    }
    desc = pbw_schema_find(box->schema, type);
    if (desc == NULL) {
        return PBW_ERR_NOT_FOUND;
    }
    return pbwe_decode_delimited_desc(box, desc, buf, len, msg, consumed);
}

pbw_status
pbwe_decode_delimited_desc(pbwe_box *box, const pbw_descriptor *desc, const void *buf,
                           size_t len, const pbw_message **msg, size_t *consumed)
{
    pbw_message *m = NULL;
    pbw_status st;

    if (msg == NULL || consumed == NULL) {
        return PBW_ERR_INVALID;
    }
    /* Same rule as pbwe_decode_desc: the reset kills the previous tree, so an
     * error returns NULL and a zeroed length, never a pointer into arena
     * memory that has been reused. */
    *msg = NULL;
    *consumed = 0;
    if (box == NULL || box->ctx == NULL || box->schema == NULL || desc == NULL) {
        return PBW_ERR_INVALID;
    }
    if (buf == NULL && len > 0) {
        return PBW_ERR_INVALID;
    }
    pbw_ctx_reset(box->ctx);
    st = pbw_decode_delimited_ctx_limits(box->ctx, desc, buf, len, &box->limits, &m, consumed);
    if (st != PBW_OK) {
        return st;
    }
    *msg = m;
    return PBW_OK;
}

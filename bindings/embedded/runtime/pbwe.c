/* Static-memory facade over the pbwire C core: see pbwe.h. */

#include "pbwe.h"

#include <stddef.h>

pbw_status
pbwe_open(pbwe_box *box, const void *desc, size_t len, size_t retain_cap)
{
    pbw_status st;

    if (box == NULL || desc == NULL || len == 0) {
        return PBW_ERR_INVALID;
    }
    box->schema = NULL;
    box->ctx = NULL;
    st = pbw_descriptor_set_load(desc, len, &box->schema);
    if (st != PBW_OK) {
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

pbw_status
pbwe_encode(const pbw_message *msg, void *out, size_t cap, size_t *len)
{
    if (msg == NULL || (out == NULL && cap > 0)) {
        return PBW_ERR_INVALID;
    }
    return pbw_encode_to(msg, out, cap, len);
}

pbw_status
pbwe_decode(pbwe_box *box, const char *type, const void *buf, size_t len,
            const pbw_message **msg)
{
    const pbw_descriptor *desc;
    pbw_message *m;
    pbw_status st;

    if (box == NULL || box->ctx == NULL || box->schema == NULL || type == NULL || msg == NULL) {
        return PBW_ERR_INVALID;
    }
    if (buf == NULL && len > 0) {
        return PBW_ERR_INVALID;
    }
    desc = pbw_schema_find(box->schema, type);
    if (desc == NULL) {
        return PBW_ERR_NOT_FOUND;
    }
    pbw_ctx_reset(box->ctx);
    st = pbw_decode_ctx(box->ctx, desc, buf, len, &m);
    if (st != PBW_OK) {
        return st;
    }
    *msg = m;
    return PBW_OK;
}

void
pbwe_reset(pbwe_box *box)
{
    if (box == NULL || box->ctx == NULL) {
        return;
    }
    pbw_ctx_reset(box->ctx);
}

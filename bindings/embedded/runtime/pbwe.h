/* Static-memory facade over the pbwire C core, for embedded targets.
 *
 * One malloc phase (pbwe_open), then a bounded steady state: decode reuses a
 * single context arena capped by retain_cap, encode writes straight into
 * caller memory and fails with PBW_ERR_LIMIT instead of growing it. No
 * globals, no thread-locals; one box per thread, like the core.
 *
 * Steady state performs no allocation for messages that fit the retained
 * budget: the cap bounds resident memory, the input length bounds the peak.
 * Oversized input fails instead of growing without bound. */

#ifndef PBWIRE_EMBEDDED_H
#define PBWIRE_EMBEDDED_H

#include <stddef.h>

#include "pbwire.h"

typedef struct {
    pbw_schema *schema;
    pbw_ctx *ctx;
} pbwe_box;

/* Load a FileDescriptorSet and create the reusable context. retain_cap
 * bounds the bytes the context keeps resident across resets (decode arena
 * head + encode buffer); pass 0 for the core default. */
pbw_status pbwe_open(pbwe_box *box, const void *desc, size_t len, size_t retain_cap);
void pbwe_close(pbwe_box *box);

/* Encode msg into out[0:cap]: no allocation, cap is the hard limit
 * (PBW_ERR_LIMIT when the message does not fit). Thin wrapper over
 * pbw_encode_to, here so callers include one header. */
pbw_status pbwe_encode(const pbw_message *msg, void *out, size_t cap, size_t *len);

/* Decode buf into the box context (reset first, so one live message at a
 * time). *msg stays valid until the next decode, pbwe_reset or pbwe_close.
 * PBW_ERR_NOT_FOUND for an unknown type name. */
pbw_status pbwe_decode(pbwe_box *box, const char *type, const void *buf, size_t len,
                       const pbw_message **msg);
void pbwe_reset(pbwe_box *box);

#endif

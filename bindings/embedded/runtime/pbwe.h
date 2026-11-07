/* Static-memory facade over the pbwire C core, for embedded targets.
 *
 * Allocation happens at pbwe_open() and again when the first decode grows the
 * arena; after that decode reuses one context arena capped by retain_cap and
 * encode writes straight into caller memory, failing with PBW_ERR_LIMIT
 * instead of growing it. No globals, no thread-locals; one box per thread,
 * like the core.
 *
 * The cap bounds resident memory and pbwe_set_limits() bounds the input that
 * may reach it, so oversized input fails before it allocates and a message
 * that fits the retained budget costs no malloc at all. The startup phase is
 * bounded too: pbwe_open() can refuse a descriptor set larger than the budget
 * it is given. */

#ifndef PBWE_H
#define PBWE_H

#include <stddef.h>

#include "pbwire.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    pbw_schema *schema;
    pbw_ctx *ctx;
    pbw_limits limits; /* zeroed = core defaults, set by pbwe_set_limits() */
} pbwe_box;
/* Both pointers are for reading (pbw_schema_find(box.schema, ...),
 * pbw_value_new_*_ctx(box.ctx, ...)); the box owns them and pbwe_close()
 * releases them. */

/* Load a FileDescriptorSet (len > 0) and create the reusable context.
 * retain_cap bounds the bytes the context keeps resident across resets
 * (decode arena head + encode buffer); 0 means the core default (1 MiB), so
 * pass a few KiB on a target whose RAM is the limit. desc_limits caps the load
 * itself, the other startup allocation: max_bytes refuses an oversized set
 * before it is parsed, max_depth bounds descriptor nesting. NULL loads it
 * unbounded, like pbw_descriptor_set_load().
 *
 * Zero the box before the first call and pbwe_close() it before opening it
 * again: a second pbwe_open() on a box that is still open (including one left
 * behind by a failed load) fails with PBW_ERR_INVALID instead of leaking. On
 * failure the box holds the partially built schema, so
 * pbw_schema_error(box.schema) says why and pbwe_close() frees it. */
pbw_status pbwe_open(pbwe_box *box, const void *desc, size_t len, size_t retain_cap,
                     const pbw_limits *desc_limits);
void pbwe_close(pbwe_box *box);

/* Cap every later decode (see pbw_limits): max_bytes rejects a frame before
 * anything is allocated, max_depth bounds nesting. NULL restores the core
 * defaults; pbwe_open() starts from them. */
void pbwe_set_limits(pbwe_box *box, const pbw_limits *limits);

/* Encode msg into out[0:cap]: no allocation, cap is the hard limit
 * (PBW_ERR_LIMIT when the message does not fit). Thin wrapper over
 * pbw_encode_to, here so *len is zeroed on every failure and a caller never
 * reads a stale length. */
pbw_status pbwe_encode(const pbw_message *msg, void *out, size_t cap, size_t *len);

/* pbwe_encode() with the varint length prefix in front: writes the whole frame
 * into out[0:cap], no allocation, PBW_ERR_LIMIT when it does not fit. The
 * writer over-reserves, so size the buffer with
 * pbw_message_encoded_size(msg) + the prefix + PBW_ENCODE_SLACK. *len is 0
 * after any failure. */
pbw_status pbwe_encode_delimited(const pbw_message *msg, void *out, size_t cap, size_t *len);

/* Decode buf into the box context (reset first, so one live message at a
 * time). *msg stays valid until the next decode, pbwe_reset or pbwe_close; on
 * any failure it is NULL, so a stale pointer never survives an error. It is
 * const because the core seals the decoded arena read-only: decode is for
 * reading, pbwe_build() is the way to write a tree. PBW_ERR_NOT_FOUND for an
 * unknown type name. */
pbw_status pbwe_decode(pbwe_box *box, const char *type, const void *buf, size_t len,
                       const pbw_message **msg);

/* Same for a descriptor resolved once with pbw_schema_find(box->schema, name):
 * no name scan per call, which is the hot path of firmware that decodes one
 * fixed type. The descriptor must come from this box's schema. */
pbw_status pbwe_decode_desc(pbwe_box *box, const pbw_descriptor *desc, const void *buf, size_t len,
                            const pbw_message **msg);

/* The length-delimited form (varint length + message, the framing
 * pbwe_encode_delimited() writes): decode one frame out of buf, resetting the
 * box first like pbwe_decode(). *consumed is the prefix plus the frame, so a
 * caller holding several frames advances by it and decodes again; it is 0
 * whenever *msg is NULL (PBW_ERR_NOT_FOUND included, for an unknown type
 * name). A declared frame length past the box limits (max_bytes) fails before
 * the body is decoded. */
pbw_status pbwe_decode_delimited(pbwe_box *box, const char *type, const void *buf, size_t len,
                                 const pbw_message **msg, size_t *consumed);

/* Same for a descriptor resolved once, without the per-call name scan. */
pbw_status pbwe_decode_delimited_desc(pbwe_box *box, const pbw_descriptor *desc, const void *buf,
                                      size_t len, const pbw_message **msg, size_t *consumed);

/* Reset the box and return a mutable root message of desc in the box arena, for
 * encode-only firmware that builds a frame from scratch: fill it with the core's
 * pbw_value_new_*_ctx(box.ctx, ...) constructors, then pbwe_encode() it. NULL
 * for bad arguments or a failed allocation (the box stays usable). */
pbw_message *pbwe_build(pbwe_box *box, const pbw_descriptor *desc);

/* Drop the live tree (and every value built into the box arena) without
 * closing the box: it stays ready for the next decode or build. A no-op on a
 * closed box. */
void pbwe_reset(pbwe_box *box);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif

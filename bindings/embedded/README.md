# pbwire embedded binding

A static-memory facade over the C core for resource-constrained targets
(microcontrollers, bootloaders, anything without a heap to spare). Allocation
happens at `pbwe_open` (descriptor set + context) and again when the first
decode grows the arena; after that the steady state allocates nothing for
messages that fit the retained budget, encode writes into caller memory, and
oversized input fails instead of growing.

This is not a second codec: `pbwe_decode`/`pbwe_encode` are thin wrappers
over `pbw_decode_ctx_limits`/`pbw_encode_to`, so the wire format stays the C
core's and the core conformance suite covers it.

```
bindings/embedded/
    runtime/        pbwe.h + pbwe.c (the facade, C99, no dependencies)
    tests/          assert-based self-check against pinned protoc bytes
    examples/       decode-print-encode round trip on matrix3
```

## Use

```c
#include "pbwe.h"

static uint8_t wire[4096];
static pbwe_box box; /* zero-initialized */

/* 4096 = resident cap; the last argument bounds the descriptor-set load. */
if (pbwe_open(&box, desc_bytes, desc_len, 4096, NULL) != PBW_OK) { /* ... */ }

/* Firmware that decodes one fixed type resolves it once and skips the per-call
 * name scan (pbw_schema_find walks every descriptor in the schema). */
const pbw_descriptor *desc = pbw_schema_find(box.schema, "myproto.MyMsg");

const pbw_message *msg;
if (pbwe_decode(&box, "myproto.MyMsg", in, in_len, &msg) != PBW_OK) { /* ... */ }
/* or the same decode without the lookup: */
if (pbwe_decode_desc(&box, desc, in, in_len, &msg) != PBW_OK) { /* ... */ }
/* msg lives in the box: valid until the next decode, pbwe_reset or pbwe_close;
 * NULL on any failure, so an error never hands back a stale pointer. */

size_t len;
/* A fixed buffer is fine, cap is the hard limit. To size it exactly use
 * pbw_message_encoded_size(msg, &need) and add PBW_ENCODE_SLACK. */
if (pbwe_encode(msg, wire, sizeof(wire), &len) != PBW_OK) { /* ... */ }

/* Length-prefixed (varint length + message) frames are one call each way;
 * *consumed says where the next frame starts, so one buffer can hold many. */
size_t consumed;
if (pbwe_decode_delimited(&box, "myproto.MyMsg", in, in_len, &msg, &consumed) != PBW_OK) { /* ... */ }
if (pbwe_encode_delimited(msg, wire, sizeof(wire), &len) != PBW_OK) { /* ... */ }

/* Encode-only firmware (telemetry, replies) builds in the same arena and never
 * decodes. pbwe_build resets the box first, so the tree is writable. */
pbw_message *built = pbwe_build(&box, desc);
if (built != NULL) {
    /* Both can fail (PBW_ERR_NOMEM / NULL on a full arena): check, don't assume. */
    if (pbw_message_set(built, 1, pbw_value_new_uint_ctx(box.ctx, 42)) != PBW_OK ||
        pbw_message_set(built, 2, pbw_value_new_bytes_ctx(box.ctx, name, name_len)) != PBW_OK) {
        /* ... */
    }
    if (pbwe_encode(built, wire, sizeof(wire), &len) != PBW_OK) { /* ... */ }
}

pbwe_close(&box);
```

Rules the facade enforces:

- One live message per box: each `pbwe_decode` (and `pbwe_build`) resets the
  context first, so the previous tree dies with it. Copy fields out before
  decoding again.
- Decoding hands back a `const pbw_message *`: the core seals the decoded
  arena, so decode is for reading. Write a tree with `pbwe_build` instead.
- `retain_cap` (bytes) bounds what the context keeps resident across
  resets; `0` keeps the core default (1 MiB), which is far too much for a
  small MCU. Only the arena half of that budget is live here: the reusable
  encode buffer it also covers belongs to `pbw_encode_ctx`, which the facade
  never calls, because `pbwe_encode` writes into your buffer instead of
  keeping a second copy in RAM. Steady state allocates nothing for messages
  that fit the retained budget.
- The last `pbwe_open` argument bounds the *startup* allocation: a
  `pbw_limits` whose `max_bytes` refuses an oversized `FileDescriptorSet`
  before it is parsed (`NULL` loads it unbounded, like the core).
- `pbwe_set_limits(&box, &limits)` puts the core's `pbw_limits` on every
  later decode: `max_bytes` rejects a frame *before* the arena grows,
  `max_depth` bounds nesting. `NULL` restores the core defaults. On a target
  whose RAM is the limit, set both.
- `pbwe_encode` (and `pbwe_encode_delimited`) never grows the buffer:
  `PBW_ERR_LIMIT` when `cap` is short, and `*len` is 0 after any failure. Size
  the buffer with `pbw_message_encoded_size(msg)` plus slack
  (`PBW_ENCODE_SLACK`), plus the length prefix for the framed form, or a fixed
  MTU-style constant.
- A descriptor set that fails to load leaves the partial schema in the box,
  so `pbw_schema_error(box.schema)` says why; `pbwe_close` frees it.
- Schemas come from a `FileDescriptorSet` loaded once at startup (sidecar
  file, `xxd -i` bytes, or whatever the firmware image carries) — the
  ahead-of-time rule in `../README.md` applies: no hand-built schema.

## Tests

`make test` builds and runs `build/test_embedded`: decode of the pinned
`matrix3.bin` oracle, byte-equal re-encode into a caller buffer (also through
the documented `pbw_message_encoded_size` + `PBW_ENCODE_SLACK` sizing), the
length-delimited framing both ways (prefix, one buffer carrying two frames,
`*consumed`, a truncated frame and a frame over the byte cap), both lookup
paths, a build-then-encode round trip in the box arena, a reset that
empties the arena and keeps the box usable, a thousand-round stability loop on
one box, the short-buffer, byte-cap, nesting-cap, unknown-type and
malformed-input error paths (`*msg` NULL and `*len` 0 after every failure), a
failed descriptor load, a descriptor set past its startup cap, a zero-length
set, and a closed box that refuses work and opens again.
`build/test_embedded_nofail` runs the same source against the counting-hook
core and asserts the loop allocates exactly zero bytes, then that a one-byte
retain cap makes the same loop allocate again (the cap is what the zero relies
on); `build/example_embedded`
runs the decode/encode flow and prints.

## Cost

Decode is one arena reuse; encode is one `pbw_encode_to` into your buffer.
The only per-call work the facade could add is `pbw_schema_find`, which is a
linear `strcmp` walk over the schema's descriptors — `pbwe_decode_desc` skips
it for a type fixed at build time. `build/test_embedded` (and its
counting-hook twin, which proves the steady loop allocates nothing) is the
whole overhead proof.

Footprint, in the two numbers a linker script cares about:

- The box is a handle, not a buffer: two pointers plus the `pbw_limits` pair,
  so `sizeof(pbwe_box)` covers it and the memory you size is the schema plus
  the retained arena, each bounded by the argument you pass.
- Frames arrive either as a bare message or length-prefixed, and both are
  one call: `pbwe_encode`/`pbwe_decode` for a bare message, and
  `pbwe_encode_delimited`/`pbwe_decode_delimited` (plus `_desc`) for the
  `varint length + message` framing the rest of pbwire uses. The prefix is
  read and written in the core, so a binding never carries varint code; a
  declared frame length past `max_bytes` fails at the prefix, before the body
  is decoded, and `*consumed` lets one buffer carry several frames.
  `pbw_istream` is the incremental reader for bytes that arrive in pieces,
  and it buffers, so it is the one piece of framing this facade leaves out.

<!-- shortcut: no binding-level benchmarks or conformance entry yet; the
     facade is a pass-through, so the core bench and conformance suite
     cover the wire. Add them when the facade grows logic of its own. -->

# pbwire embedded binding

A static-memory facade over the C core for resource-constrained targets
(microcontrollers, bootloaders, anything without a heap to spare). One
malloc phase at startup, then a bounded steady state: no per-message
allocation, caller-owned buffers, oversized input fails instead of growing.

This is not a second codec: `pbwe_decode`/`pbwe_encode` are thin wrappers
over `pbw_decode_ctx`/`pbw_encode_to`, so the wire format stays the C
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

if (pbwe_open(&box, desc_bytes, desc_len, 4096) != PBW_OK) { /* ... */ }

const pbw_message *msg;
if (pbwe_decode(&box, "myproto.MyMsg", in, in_len, &msg) != PBW_OK) { /* ... */ }
/* msg lives in the box: valid until the next decode, pbwe_reset or pbwe_close */

size_t len;
if (pbwe_encode(msg, wire, sizeof(wire), &len) != PBW_OK) { /* ... */ }

pbwe_close(&box);
```

Rules the facade enforces:

- One live message per box: each `pbwe_decode` resets the context first,
  so the previous tree dies with it. Copy fields out before decoding again.
- `retain_cap` (bytes) bounds what the context keeps resident across
  resets; `0` keeps the core default. Steady state allocates nothing for
  messages that fit the retained budget.
- `pbwe_encode` never grows the buffer: `PBW_ERR_LIMIT` when `cap` is
  short. Size the buffer with `pbw_message_encoded_size(msg)` plus slack,
  or a fixed MTU-style constant.
- Schemas come from a `FileDescriptorSet` loaded once at startup (sidecar
  file, `xxd -i` bytes, or whatever the firmware image carries) — the
  ahead-of-time rule in `../README.md` applies: no hand-built schema.

## Tests

`make test` builds and runs `build/test_embedded`: decode of the pinned
`matrix3.bin` oracle, byte-equal re-encode into a caller buffer, the
short-buffer and unknown-type error paths, and a thousand-round stability
loop on one box. `build/example_embedded` runs the same flow and prints.

## Cost

The facade adds no hot path: decode is one arena reuse, encode is one
`pbw_encode_to` into your buffer. `build/test_embedded` is the whole
overhead proof.

<!-- shortcut: no binding-level benchmarks or conformance entry yet; the
     facade is a pass-through, so the core bench and conformance suite
     cover the wire. Add them when the facade grows logic of its own. -->

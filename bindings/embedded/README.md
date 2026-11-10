# pbwire embedded binding

A static-memory facade over the C core for resource-constrained targets
(microcontrollers, bootloaders, anything without a heap to spare). Allocation
happens at `pbwe_open` (descriptor set + context) and again when the first
decode grows the arena; after that the steady state allocates nothing for
messages that fit the retained budget, encode writes into caller memory, and
oversized input fails instead of growing. `pbwe_open_mem` goes one step
further and puts the whole message workspace in a caller-owned block, so
**decode never allocates at all** — an oversized message is a `PBW_ERR_NOMEM`.
The startup path (loading the descriptor set) still uses the heap; a firmware
with no heap at all supplies the allocator the way `tests/test_nofail.c`
redirects it (`-Dmalloc=...`), or uses the frozen-schema path once it exists.

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

/* Or heap-free messages: mem backs the whole context (arena + handle), so
 * decode allocates nothing and a message that does not fit is PBW_ERR_NOMEM.
 * 8-byte aligned; leave room for the context header (a few dozen bytes) on
 * top of the message budget you size. */
static uint8_t pool[8192 + 8];
void *block = (void *)(((uintptr_t)pool + 7u) & ~(uintptr_t)7u);
if (pbwe_open_mem(&box, desc_bytes, desc_len, block, sizeof(pool) - 8, NULL) != PBW_OK) {
    /* PBW_ERR_NOMEM means the block is too small for a context */
}

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
- `pbwe_open_mem` replaces `retain_cap` with the block itself: the context and
  its arena live in the caller's memory, nothing is ever allocated for a
  message, and `pbwe_reset`/`pbwe_close` leave the block untouched (it stays
  the caller's, so the same memory reopens the box). The trade is that an
  oversized message fails with `PBW_ERR_NOMEM` at the point it runs out
  instead of a bigger arena absorbing it: set `pbwe_set_limits(&box, ...)`
  with `max_bytes` so the rejection happens before the decode, not halfway
  through it.

## Footprint and the nanopb comparison

Two numbers matter on a target: flash and RAM. Both are measured for the same
comparison message (`benchmarks/msg.proto`, four payload tiers), and both are
reproducible:

```bash
make nanopb-setup    # once: shallow clone of nanopb into .tmp/
make embedded-bench  # host throughput, both sides, byte equality check
make embedded-size   # Cortex-M flash, both sides
```

### Flash (Cortex-M, `-Os -mthumb`, no libc, no LTO)

pbwire, the generic codec (every type the descriptor set declares):

| object | text (bytes) |
|---|---|
| `wire` | 1284 |
| `arena` + `ctx` | 1643 |
| `schema` | 4707 |
| `message` | 3310 |
| `descriptor_set` | 5879 |
| `encode` | 12906 |
| `decode` | 8798 |
| `pbwe` | 684 |
| **codec total** | **39211** |
| optional: `json` / `text` / `stream` / `wkt` | 18765 / 10228 / 1180 / 754 |

nanopb, the same message (its runtime plus its generated code):

| object | text (bytes) |
|---|---|
| `pb_common` | 708 |
| `pb_encode` | 2367 |
| `pb_decode` | 3786 |
| `msg.pb` (generated) | 160 |
| **total** | **7021** |

Read the totals for what they are: nanopb's 7 KB covers *one* message type
(6.9 KB runtime + 160 B generated code), pbwire's 39 KB covers every type in
the schema. Add a message on nanopb and flash grows by its generated code; on
pbwire it grows by the descriptor bytes only (about 40 B per field: the pinned
fixtures are 733 B for `matrix3`, 475 B for `maptest`, 423 B for `edge`), and
the same bytes drive a runtime-swappable schema. A firmware with one fixed
message and a 32 KB part is still nanopb's win; a firmware with several
messages, or one whose schema changes in the field, is where the fixed codec
starts paying for itself.

A decode-only firmware can drop `encode` when it links with
`-ffunction-sections -Wl,--gc-sections` and nothing calls `pbwe_encode` (those
wrappers and `pbw_ctx`'s encode entry points are `encode.o`'s only entry
path). JSON, text, `stream` and `wkt` are separate objects and are only linked
when a firmware calls them; `descriptor_set` is the price of loading a schema
from bytes at startup.

RAM per decoded message is the tree (`pbw_value` per present field, 24 bytes on
a 64-bit target, ~20 on a 32-bit one, plus payload copies and 8-byte
alignment) in the arena. `pbwe_open_mem`'s block is therefore the number to
size: it is the peak of one message plus the context header, not a doubling
arena. The `bindings/c` struct codec (`pbw_struct_*` + `pbw_struct_arena_new_mem`)
is the lower-RAM option when the message types are fixed: decode goes straight
into a caller struct with no tree, and its caller-memory arena allocates
nothing. nanopb's per-message struct is the same idea with the caps compiled
into the struct type.

### Host throughput (Apple M4 Pro, same message, identical bytes)

`make embedded-bench`: pbwire through the embedded facade (`pbwe_open_mem`,
caller-sized arena) against nanopb's generated static struct, same loop shape.
Both encoders produce byte-identical wire on every tier (the harness asserts
it). ns/op, lower is better:

| tier | bytes | pbwire enc | nanopb enc | ratio | pbwire dec | nanopb dec | ratio |
|---|---|---|---|---|---|---|---|
| x0 | 52 | 23.3 | 131.0 | 5.6x | 35.7 | 264.6 | 7.4x |
| x1 | 55 | 26.1 | 141.6 | 5.4x | 45.3 | 280.1 | 6.2x |
| x100 | 154 | 87.9 | 753.0 | 8.6x | 338.4 | 723.9 | 2.1x |
| x3000 | 5928 | 3100.0 | 23506.0 | 7.6x | 9924.5 | 18768.0 | 1.9x |

Two costs are inside both decode columns on purpose, because every caller pays
them: pbwire resets its arena before a decode, nanopb zeroes its struct (a
12 KB memset at x3000). pbwire's decode then builds a tree, which is the RAM
trade in the paragraph above; nanopb writes straight into its struct.

### Static schema: considered, not done

The 5.9 KB `descriptor_set` object and the startup parse are the remaining
fixed cost for a schema that never changes. Two ways to remove them were
evaluated and rejected for now, each for a structural reason:

- A **binary image** of the parsed schema would be generated on the build host
  and mapped on the device, but the image contains struct bytes and pointer
  offsets, so it is only valid for the ABI it was generated for: a 64-bit host
  cannot emit an image for a 32-bit Cortex-M.
- A **generated C initializer** of the same structures would let the target
  compiler place them in `.rodata` (zero RAM, no loader, no parse), but the
  generated file has to include `src/internal.h` and the generator has to
  re-print every derived table (`tag_map`, `name_map`, `fnum_map`, `name_index`,
  the `add_*` indexes) exactly; that publishes the private schema layout to a
  second artifact and adds a divergence surface the project's "one schema
  source" rule exists to prevent.

What keeps the startup heap-free today is redirecting the core's allocator
(`-Dmalloc=...`, the pattern `tests/test_nofail.c` uses), which is one function
in the firmware. Revisit the generated-image option if a firmware's flash
budget makes 5.9 KB decisive *and* the schema is genuinely frozen forever.

<!-- shortcut: throughput here is host-only (no MCU cycle counting), and the
     paired harness covers one message shape. The bytes are checked against
     nanopb's encoder in the same run, and the core bench plus the conformance
     suite cover the format itself. A Cortex-M cycle run is the missing
     measurement. -->

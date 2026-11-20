# Julia pbench: three arms, one contract

The arms, the gate and the timing loop from
[`bindings/README.md`](../../README.md#pbench-three-arms-one-contract), applied
to `structtest.Scalars3` from `bindings/c/tests/data/matrix3.proto` and the four
standard tiers. Every arm does the same user-visible task: a native object in
and wire bytes out; wire bytes in and a fully readable native object out.

| arm | file | mechanism | what it answers |
|---|---|---|---|
| `tree` | `a-tree.jl` | the shipped `Dict`/`Message` handle: `to_dict`, `encode_dict`; `decode_dict`, `from_dict` | what the codec costs once the tree exists |
| `shim` | `b-shim.jl` | the generated `mutable struct <Message>Layout` image is the native object; `Pbwire.encode_layout` / `decode_layout` cross once and `bindings/julia/native/pbw_julia_shim.c` walks that memory in place through `pbw_struct_codec_*` | is a boundary worth paying for at all |
| `core` | `n/a` | a pure-Julia port of the C core | plan 19 (`issue41-19-core-julia.md`) builds it |

`core` prints `n/a` until plan 19 lands; there is no second wire
implementation in this binding.

## Layout table

`codegen/internal/gen/julia.go` emits, beside each message, a
`mutable struct <Message>Layout` and the `<message>LayoutRows` row list. Field
slots follow the C codec's struct ABI: scalars use their exact width, a string
is a `Ptr{UInt8}` to NUL-terminated UTF-8, bytes are the core's inline
`_PbwBytes` (pointer + length), a sub-message is a pointer to its child image,
and a repeated field is a pointer to a contiguous array plus a `Csize_t` count.
Presence survives as the 4-byte has flag, and a oneof keeps its case word.
Offsets are emitted as `fieldoffset(<Message>Layout, :field)`, so the Julia
struct and the C row table cannot disagree; the generated zero constructor
makes one image allocation possible.

`pbw_julia_shim.c` builds one `pbw_struct_field_t` row per layout row, resolves
sub-layouts by proto full name (children register first), fills proto2 enum
defaults from the schema and compiles the codec once per type. Encode and
decode are each one ccall; the type owns the reusable encode buffer, the caller
owns the decode image and arena (allocated once in the plan, so the steady
state has no `malloc`/`free`).

## Gate

```
bindings/julia/pbench/setup.sh
cd bindings/julia/pbench && julia --project=../runtime gate.jl   # PASS / exit 1
```

The gate asserts, per tier, that `tree` (dynamic handle) bytes and `shim`
(layout image) bytes equal the official ProtoBuf.jl encoder's bytes, that both
decode paths expose every field `payload.jl` reads (including `str`,
`inner.name`, the repeated last elements, the proto3 optionals and `u64`), and
that the shim's decoded image re-encodes to the same bytes.

## Provisional table (Stage 3)

`cd pbench && julia --project=../runtime compare.jl`: interleaved rounds (all
three arms back to back), median of five. `tree` is the whole user task (build
with one `set` per field, then encode; decode, then read every field back);
`shim` is one `encode_layout` / `decode_layout` call on the flat image, with the
image and arena allocated once. MB/s over wire bytes, 2026-11-21, recorded
mid-session:

| tier | `tree` enc | `shim` enc | pb-jl enc | `tree` dec | `shim` dec | pb-jl dec |
|---|---|---|---|---|---|---|
| 136 B | 168.9 | 1353.3 | 412.6 | 193.7 | 1095.2 | 1289.2 |
| 174 B | 152.5 | 1325.1 | 341.6 | 187.4 | 993.9 | 726.7 |
| 2469 B | 99.3 | 1077.9 | 140.2 | 424.9 | 688.3 | 635.8 |
| 74913 B | 97.5 | 1115.3 | 11.9 | 522.1 | 746.2 | 720.7 |

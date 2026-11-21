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
| `core` | `core_arm.jl` | the pure-Julia core (`runtime/src/core/`): `encode` / `decode` + field reads, no `ccall`, no native library | what the whole task costs when the wire walk is an interpreter loop |

`core` is the no-native arm (plan 19): stdlib-only Julia, no `ccall`, no
generated code. It is an interpreter loop by construction and is never the
fast path; `tree` and `shim` keep the shipped C core.

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

The gate asserts, per tier, that `tree` (dynamic handle), `shim` (layout image)
and `core` (pure-Julia tree) bytes equal the official ProtoBuf.jl encoder's
bytes, that every decode path exposes every field `payload.jl` reads (including
`str`, `inner.name`, the repeated last elements, the proto3 optionals and
`u64`), that the shim's decoded image re-encodes to the same bytes, and that the
core's decoded dict equals the input object and re-encodes byte-exactly.

## Provisional table (Stage 3)

`cd pbench && julia --project=../runtime compare.jl`: interleaved rounds (all
three arms back to back), median of five. `tree` is the whole user task (build
with one `set` per field, then encode; decode, then read every field back);
`shim` is one `encode_layout` / `decode_layout` call on the flat image, with the
image and arena allocated once; `core` is the pure-Julia arm (a plain `Dict`
in, `encode_dict` out; wire bytes in, fully materialized `decode_dict` out). MB/s
over wire bytes, 2026-11-21, recorded mid-session:

| tier | `tree` enc | `shim` enc | `core` enc | pb-jl enc | `tree` dec | `shim` dec | `core` dec | pb-jl dec |
|---|---|---|---|---|---|---|---|---|
| 136 B | 138.6 | 1334.6 | 48.8 | 390.6 | 183.7 | 1145.2 | 172.6 | 1279.5 |
| 174 B | 132.3 | 1235.5 | 26.6 | 327.2 | 180.4 | 1003.5 | 48.9 | 699.1 |
| 2469 B | 85.3 | 1030.5 | 29.4 | 110.0 | 420.2 | 684.7 | 25.9 | 610.0 |
| 74913 B | 84.8 | 1117.4 | 30.0 | 11.9 | 514.0 | 748.5 | 25.1 | 728.1 |

## Verdict

**Publish the flat-image `shim` row as the native-object path beside the
shipped handle.** With one `ccall` per op, the shim is above ProtoBuf.jl on all
four encode tiers (3.3x at 136 B, 9.3x at 74 KB) and above it on medium, large
and xlarge decode; the 136 B decode is 0.85x, so the small-size decode still
pays the C struct decode plus the ccall. The whole-task `tree` row (build a
`Message` with one setter per field, then read one getter per field) is 0.4-1.6x
the baseline encode because each field crosses the boundary; that is the
per-field walk issue 41 corrects, not the codec.

It is not (a) -- "replace the shipped fast path": the formal
`make julia-bench` row `pbwire-julia-tree` times a *prebuilt* `Message`, which
stays the shipping runtime for applications that already hold a tree. It is not
(c) -- "a boundary feature is missing": the shim removes the per-field crossing
from 136 B to 74 KB; only the small decode stays below the pure-Julia generated
baseline, and Stage 6 tests that one cell.

For the review's 136 B decode question: the shim does remove the per-call
`malloc`/`free` pair (the plan owns the image and the caller-memory arena, so
steady-state decode allocates nothing). The remaining gap is the C struct
decode plus the one `ccall`; 124 ns against ProtoBuf.jl's 105 ns, not an arena
cost.

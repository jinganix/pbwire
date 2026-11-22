# Julia pbench: four arms, one contract

The arms, the gate and the timing loop from
[`bindings/README.md`](../../README.md#pbench-three-arms-one-contract), applied
to `structtest.Scalars3` from `bindings/c/tests/data/matrix3.proto` and the four
standard tiers. Every arm does the same user-visible task: a native object in
and wire bytes out; wire bytes in and a fully readable native object out.

| arm | file | mechanism | what it answers |
|---|---|---|---|
| `tree` | `a-tree.jl` | the shipped `Dict`/`Message` handle through the per-field API, Go-parity full task: fresh `Message` per iteration (one `set` per field), `encode`, `close`; `decode`, one `get` per field folded into a live sink, `close` | what an application pays for the native object in and out |
| `proxy` | `benchmarks/bench.jl` (held `probe`) | the prebuilt task: a held handle's `encode`, bare `decode` + `close` with no materialization, and the reusable-`Ctx` workspace decode (`dec+ctx`) | what the transcoder/proxy task costs; published, never the headline |
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

`tree`, `shim` and `core` all time the same native-in/native-out task on the
four standard tiers. `shim`/`core` time the one-downcall op on the app-owned
native input (layout image / `Dict`) held across iterations -- the harness's
steady-state carve-out, said here and in the `bench.jl` header; only `tree`
rebuilds its native input per iteration.

## Formal table (`make julia-bench`, Go-parity re-time)

`make julia-bench` (`bindings/julia/benchmarks/bench.jl` via the recorder
into `results/bench-julia-20261121T190400Z.json`): interleaved rounds, median
of five. `tree` is the Go-parity full task (fresh `Message` per iteration,
one `set` per field, encode, `close`; decode, every field folded into a live
sink, `close`); `proxy` is the old prebuilt shape, relabelled not
re-measured; `shim` is one `encode_layout` / `decode_layout` call on the
app-owned flat image; `core` is the pure-Julia arm (plain `Dict` in,
`encode_dict` out; wire bytes in, fully materialized `decode_dict` out).
MB/s over wire bytes; x = THROUGHPUT ratio vs ProtoBuf.jl (higher = faster):

| tier | `tree` enc | `shim` enc | `core` enc | pb-jl enc | `tree` dec | `shim` dec | `core` dec | pb-jl dec |
|---|---|---|---|---|---|---|---|---|
| 136 B | 160.5 (0.42x) | 1307.0 (3.39x) | 39.1 (0.10x) | 385.6 | 212.5 (0.18x) | 1102.5 (0.93x) | 162.7 (0.14x) | 1181.2 |
| 174 B | 144.4 (0.45x) | 1243.1 (3.90x) | 22.3 (0.07x) | 319.1 | 213.0 (0.30x) | 980.5 (1.39x) | 47.9 (0.07x) | 706.0 |
| 2469 B | 87.3 (0.70x) | 1040.5 (8.30x) | 28.6 (0.23x) | 125.3 | 576.1 (0.93x) | 693.3 (1.12x) | 25.9 (0.04x) | 617.0 |
| 74913 B | 87.9 (7.26x) | 1122.2 (92.74x) | 29.3 (2.42x) | 12.1 | 739.8 (1.04x) | 738.9 (1.04x) | 24.9 (0.04x) | 709.1 |

The prebuilt `proxy` row beside it (same run, MB/s): encode
1757.0 / 1644.6 / 1648.0 / 1694.4; decode 1037.8 / 886.2 / 689.9 / 747.2
(`dec+ctx` 1140.5 / 958.5 / 698.7 / 740.5). Proxy/tree encode gap:
11.0x / 11.4x / 18.9x / 19.3x -- the gap the old prebuilt `tree` row was
hiding. The xlarge baseline encoder collapses (12.1 MB/s: the generated
encoder re-derives sizes per field), which is why every pbwire encoder is
above it there; on the three smaller tiers the ratios read normally.

## Verdict

**Ship the flat-image `shim` row as the native-object path; publish the
full-task `tree`, the `core`, and the `proxy` rows beside it, never headline
`proxy`.** With one `ccall` per op, the shim is above ProtoBuf.jl on all four
encode tiers (3.39x at 136 B, 92.74x at 74 KB -- the xlarge baseline encoder
collapses) and above it on medium, large and xlarge decode; only the 136 B
decode is below at 0.93x, where the C struct decode plus the one `ccall`
(123 ns against ProtoBuf.jl's 115 ns) still costs more than the generated
one-shot parse. The whole-task `tree` row (fresh `Message` per iteration, one
setter per field, then one getter per field into a live sink) is 0.42-0.70x
the baseline encode on the three smaller tiers because each field crosses the
boundary -- that is the per-field walk issue 41 corrects, not the codec -- and
7.26x at 74 KB where the generated encoder falls over. At 74 KB the tree
decode (1.04x) and the shim decode (1.04x) both match the baseline because the
wire walk, not the boundary, dominates.

It is not (a) -- "replace the shipped fast path": the `ccall` runtime stays
the shipping codec, and the prebuilt shape applications that already hold a
tree pay is still published as `pbwire-julia-proxy` (11-19x the tree encode
across tiers). It is not (c) -- "a boundary feature is missing": the shim
removes the per-field crossing from 136 B to 74 KB; only the small decode
stays below the generated baseline.

Below-baseline native cells, flagged for Task 15 (no optimization here): tree
encode 136/174/2469 B, tree decode 136/174/2469 B, shim decode 136 B, core
encode 136/174/2469 B, core decode all four tiers.

## Verdict (core, plan 19)

**The `ccall` runtime stays the shipped codec; the pure core ships as the
no-native arm.** `make conformance-julia-core` passes the official binary suite
(1412 successes, 1403 skipped, 2 expected, 0 unexpected -- the same set as the
C core, the TypeScript, C#, Python and Dart ports), so the arm is a proven
second mechanism, and it needs neither a C toolchain nor a native library:
stdlib-only Julia, no `ccall`, no generated code.

Its numbers (provisional table above) are the honest price of that portability:
at 74 KB the pure encoder is 30.0 MB/s against ProtoBuf.jl's 11.9 (2.5x,
because the generated encoder re-derives sizes per field while this one writes
fused tag+value pairs into one buffer), but it is 0.12-0.27x the generated
baseline on the three smaller encode tiers and 0.03-0.13x on every decode tier,
where the `Dict` <-> tree conversion, not the wire walk, is what an interpreter
pays. The `shim` row is 2.4-4.8x the core encode and 2.2-4.8x the core decode,
so this is published as the arm for hosts without a C toolchain, never as the
fast one.

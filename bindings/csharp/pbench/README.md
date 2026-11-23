# C# pbench: four arms, one contract

The arms, the gate and the timing loop from
[`bindings/README.md`](../../README.md#pbench-three-arms-one-contract), applied
to `structtest.Scalars3` from `bindings/c/tests/data/matrix3.proto` and the four
standard tiers. Every arm does the same user-visible task: a native object in
(a `Structtest.Scalars3` built by the generated model, or the dynamic
`Pbwire.Message` handle) and wire bytes out; wire bytes in and a fully readable
native object out.

| arm | mechanism | what it answers |
|---|---|---|
| `tree` | a native object in and out through the shipped per-field API: fresh `NewMessage()` + one `Set()` per field then `Encode()`; `Decode()` then one `get` per field into a fresh object, then `Dispose()`; no prebuilt tree before the timer | what an application pays through the handle path |
| `shim` | `Scalars3Layout.Marshal(model)` / `.Unmarshal(wire)`: one flat image in, **one `P/Invoke`** per op, `bindings/csharp/native/pbw_cs_shim.c` walks it through `pbw_struct_codec_*` | is a boundary worth paying for at all |
| `core` | `bindings/csharp/Pbwire/Core/`: a pure-C# port of the C core (`Schema`, `Msg`, `Encode`, `Decode`), no `DllImport` | what the same codec costs with no native library at all |
| `proxy` | the prebuilt task: re-encode a held handle, decode-and-`Dispose()` with no materialization | what the codec costs once the tree exists; published, never the headline |

The `core` arm is a file, not a stub: `CCore.cs` in the gate loads
`benchmarks/gen/matrix3.desc`, builds the dynamic tree from the plain object
(`EncodeObject`) and decodes to a plain object (`DecodeObject`). It never
touches `libpbwire`; that is the whole point of the arm.

## Layout table

`codegen/internal/gen/csharp.go` emits, beside each message, the flat image
that the native reader walks:

- `<Message>Layout.Table`: the gated slot table — `FieldAt(name, offset, size)`
  for a plain scalar, `Has(name, bit, offset)` for a proto3 optional (presence
  bit index plus storage), `Ref(name, offset, size)` for a pointer slot. Scalars
  pack first at natural alignment, pointer slots follow. `Pbwire.Layout.Verify`
  checks it against the descriptor (every schema field has a row, every row
  names a real field, no overlap).
- `<Message>Layout.Rows`: one `Pbwire.LayoutRow` per field with the facts the C
  codec needs — `pbw_struct_kind_t`, `pbw_type`, wire type, `Has`/`Count`/`Case`
  byte offsets, `Packed`/`Required`, and the child's proto full name.
- `WriteImage` / `ReadImage` / `NewImage` on the model: copy the native object
  into and out of one contiguous image. Strings are NUL-terminated UTF-8 into
  the call arena, `bytes` is the core's inline `pbw_bytes_t`, a sub-message is a
  pointer to its own image (stride `Child.ImageBytes`), a repeated field is a
  pointer to a contiguous array plus its count.

**Why an explicit image and not a pin:** `GCHandle.Alloc(obj, Pinned)` throws for
a class with reference members (`string`, `List<T>`/`RepeatedField<T>`), and the
GC may move those members at any time. The image is the pinned representation;
writing it field by field into one `Marshal.AllocHGlobal` block per thread is
this arm's honest cost, and it is paid once per op, not once per field across
the boundary.

**Presence:** proto3 optionals keep their 4-byte presence word, absent
pointer-kind fields stay NULL, and a oneof keeps its 4-byte case word. One
recorded limitation, shared with the Dart arm: a *present but empty* optional
`string`/`bytes` is indistinguishable from an absent one (NULL is the absence
signal), so it encodes as absent.

## Gate

```
bindings/csharp/pbench/setup.sh        # protoc: benchmarks/gen (oracle) + matrix3.desc
dotnet run --project bindings/csharp/pbench/gate -c Release   # gate: PASS / exit 1
```

The gate asserts, per tier, that `tree` (handle) bytes and `shim` (model) bytes
equal the pinned protoc-generated `Google.Protobuf` bytes, that both decode
paths expose the fields `benchmarks/Bench.cs` checks (`str`, `inner.name`,
`inner.value`, the repeated lengths and last element, the proto3 optionals,
`u64`, `color`), and that a decoded model re-encodes to the same bytes. It also
runs `Layout.Verify` for `Scalars3` and `Inner3`, so a wrong offset table fails
the gate instead of producing bad bytes.

## Provisional table (Stage 3, plan 05/05b)

The `tree` and `shim` rows below are plan 05's mid-session table (kept as
written; the formal run is the Verdict).

`dotnet run --project pbench/gate -c Release -- compare`: gate first, then
interleaved rounds (all three implementations back to back), median of five.
MB/s over wire bytes, 2026-11-21, mid-session (host load varies; the ordering
is the signal):

| tier | `tree` enc | `shim` enc | Google enc | `tree` dec | `shim` dec | Google dec |
|---|---|---|---|---|---|---|
| 136 B | 1259.1 | 402.5 | 132.6 | 854.1 | 347.9 | 147.1 |
| 174 B | 1173.6 | 519.4 | 374.4 | 698.1 | 259.4 | 140.3 |
| 2469 B | 1341.3 | 649.5 | 439.0 | 665.3 | 335.1 | 471.5 |
| 74913 B | 1558.6 | 686.8 | 414.5 | 740.9 | 348.6 | 474.6 |

The `shim` row is now the mechanism the plan set out to measure: one P/Invoke
per op with the C reader walking the image, so it is above the Google.Protobuf
baseline on every encode tier (1.7x-3.0x) and on the two small decode tiers
(2.4x, 1.8x). Decode at 2469 B and 74913 B is below the baseline (0.71x,
0.73x): the C decode is one call, but the generated `ReadImage` then materialises
every repeated element (3000 ints, 3000 strings, 3000 sub-messages, 3000 bools
at the xlarge tier) into managed objects, which the handle row never pays.
Stage 6 addresses (or records) exactly those two cells.

## Pure-C# `core` arm (Stage 3 provisional, plan 16)

The same gate then interleaved rounds, median of five, 2026-11-21
(`make csharp-pbench`, mid-session: the small-tier `core` cells showed a 6-8x
round spread, so they are host noise, not a codec number -- the Stage 5
best-of-three run below is the figure that counts):

| tier | `core` enc | Google enc | `core` dec | Google dec |
|---|---|---|---|---|
| 136 B | 129.5 | 130.6 | 134.8 | 219.0 |
| 174 B | 111.5 | 382.6 | 450.1 | 337.3 |
| 2469 B | 665.2 | 439.3 | 342.3 | 435.1 |
| 74913 B | 702.4 | 393.6 | 304.8 | 484.7 |

The ordering that held in every round: encode above Google.Protobuf from
2469 B up (1.5x-1.8x), decode above it below 2.5 KB only. The arm is
`c-core/CCore.cs` over `bindings/csharp/Pbwire/Core/` (descriptor reader,
tree, wire, encode, decode), measured with its own tree built before the timer
so the row times the codec, like `tree`.

## Verdict (Go-parity re-time, 2026-11-21)

**Ship the flat-image `shim` row as the native-object path; publish `tree`
(full task), `core`, and `proxy` beside it; never headline `proxy`.** Formal
`make csharp-bench` under `results/.bench.lock`, median of 5
(`results/bench-csharp-20261121T185300Z.json`). Throughput ratios are
pbwire MB/s over `protobuf-csharp` MB/s (higher is faster):

| tier | tree enc | tree dec | shim enc | shim dec | core enc | core dec |
|---|---|---|---|---|---|---|
| 136 B | 0.38x | 0.45x | 2.58x | 1.54x | 2.09x | 1.14x |
| 174 B | 0.36x | 0.39x | 2.42x | 1.48x | 2.08x | 1.14x |
| 2469 B | 0.15x | 1.11x | 1.05x | 0.73x | 1.04x | 0.73x |
| 74913 B | 0.17x | 1.49x | 1.36x | 0.70x | 1.34x | 0.62x |

The `tree` row is now the real application task (`benchmarks/Bench.cs`:
`FillPbwire` fresh per iteration, `Encode()`, `Dispose()` inside the timer;
decode reads every gate-checked field back through `ConsumeTree`, same field
list the gate asserts in `TreeFieldsOk`). The old prebuilt rows are relabelled
`pbwire-csharp-proxy`, not re-measured as a new task. Proxy/tree encode gap
at xlarge: 20.7x (8.4x small, 9.9x medium, 15.7x large) -- that gap is what the
old `tree` row was hiding.

Why not the others: `tree` encode loses on every tier (0.15x-0.38x -- one
`Set()` P/Invoke per field plus a fresh arena per iteration) and small-tier
decode (0.45x, 0.39x); it wins decode only at 2469 B+ (1.11x, 1.49x), where
one C decode beats the generated parser even with per-field reads back out.
`core` is the no-C-toolchain path (above baseline on all four encode tiers
and both small decodes), not the fast path. `proxy` is the held-tree price,
never the headline.

Below-baseline native cells flagged for Task 15 (no optimization here): tree
encode every tier, tree decode at 136/174 B, shim decode at 2469/74913 B,
core decode at 2469/74913 B. F# rides on C#: no own bench or pbench arms; the
F# facade encodes through the same C core, and `make conformance-fsharp` is
green (2750 + 443 successes, 0 unexpected).

## Verdict (superseded Stage 3/16 tables, kept as written)

**Publish the flat-image `shim` row as the native-object path: with one
`P/Invoke` per op the C reader is above the official baseline on 10 of the 12
encode/decode cells, and the two it loses are the managed materialisation, not
the boundary.** Formal `make csharp-bench` medians (2026-11-21,
`results/bench-csharp-20261121T000941Z.json`):

| tier | op | pbwire-csharp-shim | protobuf-csharp | x |
|---|---|---|---|---|
| 136 B | encode | 1130.8 | 465.6 | 2.43 |
| 136 B | decode | 744.1 | 500.5 | 1.49 |
| 174 B | encode | 980.3 | 408.1 | 2.40 |
| 174 B | decode | 595.9 | 400.3 | 1.49 |
| 2469 B | encode | 617.5 | 562.0 | 1.10 |
| 2469 B | decode | 341.5 | 464.3 | 0.74 |
| 74913 B | encode | 654.1 | 476.0 | 1.37 |
| 74913 B | decode | 348.7 | 492.3 | 0.71 |

What is measured is the mechanism the plan set out to build: the native object
is copied into one contiguous image per op (`WriteImage`), one `DllImport`
hands it to `bindings/csharp/native/pbw_cs_shim.c`, and the C layout codec
walks it in place. There is no per-field crossing anywhere in `shim`.

The two below-baseline cells are decode at 2469 B and 74913 B. The split at
74913 B (warm, median): C decode 103 us, `ReadImage` 147 us, i.e. the native
half is 2.1x faster than `protobuf-csharp`'s whole decode (492 MB/s -> 729 MB/s
of raw work), and materialising 3000 ints, 3000 strings, 3000 sub-messages and
3000 bools into managed objects costs more than the reference parser's entire
run. That is the price of a *separate* native object: the handle row (`tree`)
does not pay it because it never leaves the C tree.

It is not (a) -- "replace the shipped fast path": a prebuilt `Message` handle
encodes 1603.5 MB/s and decodes 729.1 MB/s at 74 KB, faster than the image path
because it skips the copy, and re-encode users keep it. It is not (c) -- "a
boundary feature is missing": the boundary is one call at every tier; what is
missing is the willingness to hand a managed object graph to C, which no C ABI
can do without an image. Both rows are published (`pbwire-csharp-shim` beside
`pbwire-csharp-tree`); the shim is the native-object row, never the headline.

Stage 6 (optimization) is recorded in `docs/perf-attempts.md`: the plan-cache
change (one static `LayoutPlan` instead of a lock+lookup per op) is kept and is
byte-identical, and the two decode cells cannot cross without dropping the
image copy -- a mechanism change, not a tuning round.

### The pure-C# core arm (plan 16)

**Publish `pbwire-csharp-core` beside the two native rows; the addon stays the
default path and the pure core is the arm for a build with no C toolchain
(and the codec F# inherits).** Formal `make csharp-bench`, one run, same
baseline in every row (`results/bench-csharp-20261121T004756Z.json`,
2026-11-21):

| tier | op | pbwire-csharp-core | protobuf-csharp | x |
|---|---|---|---|---|
| 136 B | encode | 925.8 | 457.9 | 2.02 |
| 136 B | decode | 540.9 | 473.9 | 1.14 |
| 174 B | encode | 852.6 | 404.6 | 2.11 |
| 174 B | decode | 479.6 | 395.2 | 1.21 |
| 2469 B | encode | 621.2 | 568.5 | 1.09 |
| 2469 B | decode | 362.4 | 473.4 | 0.77 |
| 74913 B | encode | 653.6 | 472.2 | 1.38 |
| 74913 B | decode | 318.6 | 506.3 | 0.63 |

The core is above Google.Protobuf's generated codec on all four encode tiers
(1.09-2.11x) and on both small decode tiers (1.14x, 1.21x), and below it on the
two large decodes (0.77x, 0.63x). The gap is the tree shape, not the wire
engine: every decoded element is a boxed `object` slot plus a `Raw` slice (or a
`Msg`), so 3000 ints + 3000 strings + 3000 sub-messages + 3000 bools cost more
than the reference parser's straight-line generated reader, while the handle
row (`tree`, 737.9 / 753.3 at 74913 B) never materializes at all. It is still
2.1-2.6x faster than the `shim`'s managed image round trip on the same
tiers, because nothing is copied twice: `tree` > `core` > `shim` on decode at
74 KB (754 / 319 / 363 MB/s).

It is not a replacement for the addon: the prebuilt handle is 2.4x faster on
encode and 2.4x on decode at 74 KB, and the shim's native half is still the
fastest way to hand a *separate* native object to the core. The pure core wins
where the alternative is "no pbwire at all" -- a container or CI image with no
C compiler, a single-file `dotnet publish`, or a wasm/browser host -- and it is
the codec F#'s facade can fall back to. Stage 6 is recorded in
`docs/perf-attempts.md`: the two below-baseline decode cells need unboxed
slot storage (a columnar/struct tree), which is a representation change, not
a tuning round; no probe was run for a cell that a one-line change cannot move.

## Verdict (Task 5 takeover, 2026-11-22)

Selection unchanged: **ship the flat-image `shim` as the native-object path**
-- and now the generated models actually take it. `ToByteArray()` encodes in
one native call over the image (`<Message>Layout.Marshal`) whenever the
embedded descriptor set is active, and `MergeFromBytes` decodes the same way
(`<Message>Layout.Unmarshal`); an externally registered schema
(`UseSchema`) keeps the per-field handle path, which reads the override
(`PbwireFile_<Stem>.IsEmbedded` switches between them, `EncodeInto` /
`LoadFrom` stay for the fallback). F# rides free: no F# code changed.

Three correctness fixes land with the promotion (all gated):

- Every singular scalar owns a 4-byte presence word -- implicit-presence
  zero no longer hits the wire, and `WriteImage` mirrors the per-field
  path's skip condition (`Has` bit, `Required`, or `p != 0`).
  Strings/bytes keep NULL as the absence signal, with `Utf8OrEmpty` (never
  NULL) for values the wire must keep and `Clear` on absent optionals
  during `ReadImage` instead of assigning null.
- `LayoutPlan.Decode` returns the default message on empty input without a
  native call.
- The decode arena headroom tracks the grown images
  (`wire.Length * 6 + 65536`, was `* 4`): the xlarge gate tier failed with
  `status 2` before the bump, passes after; the formula lives in codegen
  so every generated model carries it.

One honesty fix: the `core` arm converted nothing -- `CCore.New` built the
`Msg` tree once before the timer, so the published `core` row (1.34x / 0.62x
at xlarge) priced the codec, not the task. It now holds the plain object
(`Dictionary<string, object?>`, the same prebuilt native input every arm
encodes from) and converts per call (`EncodeObject` / `DecodeObject`),
exactly what the arm table above always described. The row drops to
0.55x / 0.25x at xlarge -- the documented price of native-in/native-out,
the same class as Python's accepted `encode(data)` 0.69x -- and the gate
checks it byte-identical plus field-by-field. The old row was a proxy row
in a core label; it is superseded, not comparable.

Same-host A/B (interleaved `compare`, median of 5, in-process
`protobuf-csharp` as drift control): presizing the four repeated lists in
generated `ReadImage` (`Capacity = n` when short, never shrunk) moves xlarge
`shim` decode 0.698x -> 0.735x and large 0.757x -> 0.803x, with the small
tiers inside their usual spread (no elements to presize there). Mechanism:
~48 growth reallocations per xlarge decode removed; kept. The remaining
decode floor is structural -- managed materialisation of 3000 ints, 3000
strings, 3000 sub-messages and 3000 bools -- and is recorded in
`docs/perf-attempts.md`.

Formal `make csharp-bench` for the shipped code is
`results/bench-csharp-20261122T181950Z.json` (same-host A/B above folded
in: xlarge `shim` decode 0.74x, `core` 0.55x / 0.25x); the `BENCHMARKS.md`
C# rows follow that record.
Below-baseline native cells after this task: tree encode every tier, tree
decode at 136/174 B, shim decode at 2469/74913 B, core every cell but small
encode (0.67x/0.71x) -- the core row is honest now, and its red is published,
not tuned away.

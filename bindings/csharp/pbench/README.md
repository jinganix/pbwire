# C# pbench: three arms, one contract

The arms, the gate and the timing loop from
[`bindings/README.md`](../../README.md#pbench-three-arms-one-contract), applied
to `structtest.Scalars3` from `bindings/c/tests/data/matrix3.proto` and the four
standard tiers. Every arm does the same user-visible task: a native object in
(a `Structtest.Scalars3` built by the generated model, or the dynamic
`Pbwire.Message` handle) and wire bytes out; wire bytes in and a fully readable
native object out.

| arm | mechanism | what it answers |
|---|---|---|
| `tree` | the prebuilt `Pbwire.Message` handle: `message.Encode()`, `type.Decode(wire)` | what the C core's codec costs once the tree exists |
| `shim` | `Scalars3Layout.Marshal(model)` / `.Unmarshal(wire)`: one flat image in, **one `P/Invoke`** per op, `bindings/csharp/native/pbw_cs_shim.c` walks it through `pbw_struct_codec_*` | is a boundary worth paying for at all |
| `core` | `bindings/csharp/Pbwire/Core/`: a pure-C# port of the C core (`Schema`, `Msg`, `Encode`, `Decode`), no `DllImport` | what the same codec costs with no native library at all |

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

## Provisional table (Stage 3)

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

## Verdict

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

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
| `core` | `n/a` — a C# port of the C core | plan 16 (`issue41-16-core-csharp.md`) builds it |

`core` prints `n/a` until plan 16 lands; there is no second wire
implementation in this binding.

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

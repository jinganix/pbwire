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
MB/s over wire bytes, 2026-11-21, recorded mid-session (load ~15-30; absolute
numbers move with the host, the ordering does not):

| tier | `tree` enc | `shim` enc | Google enc | `tree` dec | `shim` dec | Google dec |
|---|---|---|---|---|---|---|
| 136 B | 1365.9 | 91.1 | 130.8 | 829.6 | 100.9 | 277.8 |
| 174 B | 1490.8 | 116.4 | 378.5 | 821.7 | 128.6 | 273.0 |
| 2469 B | 1359.2 | 60.4 | 386.7 | 676.5 | 51.1 | 381.7 |
| 74913 B | 1573.5 | 61.3 | 387.9 | 745.7 | 51.8 | 476.4 |
## Verdict

**Keep the prebuilt handle as the runtime and publish the native-object path
beside it; the pinned layout does not pay on .NET as this plan can build it.**
The table is emitted and gated (name/offset/width/presence), but the C core has
no entry that reads a pinned managed object through it, so every present field
still costs one `P/Invoke` in the managed walk: the `shim` row is 0.70x
Google.Protobuf at 136 B encode and 0.16x at 74 KB (0.11x decode), i.e. the
boundary is what the native-object task pays, not the codec. The `tree` row
(the shipped `Message` handle) is 3-4x the baseline and stays the binding's
headline.

It is not (a) -- "replace the shipped fast path": re-encoding the same message
through the prebuilt handle is 30x the native-object row (74 KB encode 44.3 µs
against 1267.8 µs), and those users keep it. It is not (c) -- "a boundary
feature is missing": no payload-size crossover exists, the native-object row is
slower at every tier including 74 KB. So the arm is published as measured
(`pbwire-csharp-shim`), never as the headline.

Stage 6 is a recorded rejection, not a tuning round: the only change that
removes the per-field crossing is a native offset reader over these rows (a
`src/` + `include/` change outside this plan), and a managed walk variant is
the same converter with a different loop, which cannot beat the per-field
`P/Invoke` floor. Ledger entry: `docs/perf-attempts.md`.

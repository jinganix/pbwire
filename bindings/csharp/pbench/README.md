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
| `shim` | `Scalars3Layout.Marshal(model)` / `.Unmarshal(wire)`: the generated model is the native object, one call per op | is a boundary worth paying for at all |
| `core` | `n/a` — a C# port of the C core | plan 16 (`issue41-16-core-csharp.md`) builds it |

`core` prints `n/a` until plan 16 lands; there is no second wire
implementation in this binding.

## Layout table

`codegen/internal/gen/csharp.go` emits a `<Message>Layout` row table beside
each message: `FieldAt(name, offset, size)` for a plain scalar slot,
`Has(name, bit, offset)` for a proto3 optional (presence bit index plus its
storage), `Ref(name, offset, size)` for a reference-shaped slot (a pointer
indirection: `string`, `bytes`, submessage, repeated field). Scalars pack first
at natural alignment, pointer slots follow; the offsets describe the flat image
`pbw` would read if the core grew an offset reader. `Pbwire.Layout` validates
the table against the descriptor (`Verify`): every schema field must have a row,
every row must name a real field, and no slot may overlap another.

**Deviation (recorded):** the C core has no entry that reads a pinned managed
object through an offset table, and adding one needs `src/` + `include/`, which
this plan does not touch. The walk is therefore managed: `Pbwire.Run.Encode`
allocates the tree once per op (`ty.NewMessage()`), the generated model's own
shipped converter (`EncodeInto`) crosses once per present field, and one
`Encode` call writes the bytes; decode is one `Decode` call plus the model's
`LoadFrom`. One call per op from the caller's view, native object in and out,
but the per-field crossing is not removed. The table is emitted and gated as
the contract a native reader would consume (`FieldAt`/`Has` offsets), not as
the walk's data source.

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

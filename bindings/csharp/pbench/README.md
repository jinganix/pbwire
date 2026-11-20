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

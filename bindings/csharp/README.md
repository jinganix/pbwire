# pbwire C# binding

`bindings/csharp/Pbwire` is the C# binding: `DllImport` over the C ABI in
[`include/pbwire.h`](../../include/pbwire.h), with a `DllImportResolver` that
loads `PBWIRE_LIB` (else `build/libpbwire.{dylib,so}` found above the working
directory). A `FileDescriptorSet` (`protoc --descriptor_set_out`) becomes the
schema at run time; there is no generated code and no C glue. The wire format
lives in the C core, so a C# message and a C message are the same bytes by
construction.

```csharp
using Pbwire;

using var schema = Schema.FromFile("tests/data/matrix3.desc");
var ty = schema.Message("structtest.Scalars3");
var innerTy = schema.Message("structtest.Inner3");

using var msg = ty.NewMessage()
    .Set("i32", -42L)
    .Set("str", "alice");
foreach (var tag in new[] { "a", "bb" })
{
    msg.Set("tags", tag);                       // repeated: appends
}
msg.SetMessage("inner")                         // nested: created in place
   .Set(innerTy.Field("name"), "kid")
   .Set(innerTy.Field("value"), 42L);

var wire = msg.Encode();
using var decoded = ty.Decode(wire);            // unknown fields preserved
if (!decoded.Encode().SequenceEqual(wire))
{
    throw new InvalidOperationException("round trip differs");
}
```

Fields are addressed by name (`"i32"`) or by a `Field` handle from
`ty.Field("i32")`; the handle skips the name lookup and is what a hot loop should
reuse.

## Requirements

The .NET SDK 8+ (built and tested with 10), `make shared`, and nothing else: the
binding project has no package references. `PBWIRE_LIB` overrides the library
path.

## Build and run

```shell
dotnet run --project tests -- ../c/tests/data/matrix3.desc      # self-check
dotnet run --project examples -- ../c/tests/data/matrix3.desc   # example
```

## Benchmarks

```shell
./benchmarks/bench.sh    # vs Google.Protobuf C# + protobuf-net, four tiers
```

Needs `protoc` and NuGet access (`dotnet restore` fetches `Google.Protobuf`
and `protobuf-net`).
The four tiers are small / medium / large / xlarge (the repeated fields scaled
x0 / x1 / x100 / x3000). Copied from [BENCHMARKS.md](../../BENCHMARKS.md),
which is the single source for all performance numbers; higher is better, MB/s
over wire bytes (protobuf-net's medium rows are absent: it singular-encodes
single-element packed fields, so there is nothing byte-identical to time):

| payload | op | pbwire-csharp (MB/s, x protobuf-csharp) | protobuf-csharp (MB/s) | protobuf-net (MB/s) |
|---|---|---|---|---|
| 136 B | encode | 1470.5 (3.1x) | 479.4 | 587.7 |
| 136 B | decode | 1164.5 (2.4x) | 480.1 | 463.1 |
| 174 B | encode | 1377.2 (3.3x) | 416.0 | — |
| 174 B | decode | 983.4 (2.5x) | 399.1 | — |
| 2469 B | encode | 1326.1 (2.3x) | 582.1 | 409.5 |
| 2469 B | decode | 660.2 (1.4x) | 471.3 | 306.7 |
| 74913 B | encode | 1532.7 (2.9x) | 526.7 | 414.2 |
| 74913 B | decode | 745.6 (1.4x) | 526.6 | 344.9 |

Strings cross as NUL-terminated C strings, so `Set(field, string)` stops at the
first NUL byte: use the `ReadOnlySpan<byte>` overload for binary payloads (a
`string` field accepts them too, which is how protobuf's wire format works).

## Ownership and threads

A `Schema` and a root `Message` own native memory: `Dispose()` frees it. A
`MessageType` keeps its `Schema` (and a `Message` its `MessageType`) reachable,
so the GC cannot free the schema under a live type or message; `Dispose()` is
still the caller's promise that nothing else uses it. A nested message is a view
of its parent, holds it alive through a `Parent` reference, and never frees
anything. Nothing is shared, so one tree belongs to one thread, like the C core.

## Generated models

`protoc-gen-pbwire` (in `../../codegen`) generates Google.Protobuf-shaped
sealed classes: the same property names, `RepeatedField<T>` for repeated,
`KindOneofCase` for oneofs, `Has`+`Clear` for optional, and
`Parser.ParseFrom` / `ToByteArray` / `WriteTo` / `CalculateSize` /
`Clone` / `Equals` over the embedded schema — no schema argument at use
sites. Each file splits in two: `<base>.cs` (models) and
`PbwireFile_<Stem>.cs` (the holder: embedded set plus its loader).
`--pbwire_opt=lang=csharp[,model=bool][,register=bool][,registry=bool]`
(all default `true`): `model=false` leaves the holder for dynamic-only
users; `register=false` drops the eager static constructor so `Init()`
(embedded) or `UseSchema()` (external `Schema`, dual registration) must run
first; `registry=false` drops `MessageNames` + `Preload()`.
Deliberate deltas from protoc output: no `IMessage` (a streaming
`MergeFrom` would need a second wire reader) and no `Descriptor`; use the
dynamic `Schema` for reflection. The committed output is `generated/gen/` (tests)
and `examples/gen/` (example); `oracle/gen_oracle/` holds the pinned
protoc output (`extern alias Oracle` in `GeneratedTest.cs` proves byte and field
parity, T1–T6). See `../../codegen/README.md`.

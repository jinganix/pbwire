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
| 136 B | encode | 1365.4 (3.0x) | 450.7 | 583.9 |
| 136 B | decode | 922.1 (1.9x) | 487.5 | 467.5 |
| 174 B | encode | 1410.7 (3.4x) | 417.7 | — |
| 174 B | decode | 825.1 (2.0x) | 411.6 | — |
| 2469 B | encode | 1206.7 (2.2x) | 546.8 | 384.0 |
| 2469 B | decode | 519.8 (1.18x) | 441.8 | 303.7 |
| 74913 B | encode | 1393.5 (2.9x) | 480.6 | 392.5 |
| 74913 B | decode | 547.1 (1.07x) | 513.0 | 337.2 |

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

## Typed models

`protoc-gen-pbwire-typed` (in `../typed`) generates Google.Protobuf-shaped
sealed classes: the same property names, `RepeatedField<T>` for repeated,
`KindOneofCase` for oneofs, `Has`+`Clear` for optional, and
`Parser.ParseFrom` / `ToByteArray` / `WriteTo` / `CalculateSize` /
`Clone` / `Equals` over the embedded schema — no schema argument at use
sites. Deliberate deltas from protoc output: no `IMessage` (a streaming
`MergeFrom` would need a second wire reader) and no `Descriptor`; use the
dynamic `Schema` for reflection. The committed output is `typed/gen/` (tests)
and `examples/gen/` (example); `oracle/gen_oracle/` holds the pinned
protoc output (`extern alias Oracle` in `TypedTest.cs` proves byte and field
parity, T1–T6). See `../typed/README.md`.

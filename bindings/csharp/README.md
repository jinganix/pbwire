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
over wire bytes. `pbwire-csharp-tree` is the prebuilt `Pbwire.Message` handle;
`pbwire-csharp-shim` is the generated model's native-object entry, one call per
op (see `pbench/README.md` for the recorded deviation: the C core has no
offset reader, so every field still crosses once). protobuf-net's medium rows
are absent: it singular-encodes single-element packed fields, so there is
nothing byte-identical to time:

| payload | op | pbwire-csharp-tree (MB/s, x protobuf-csharp) | pbwire-csharp-shim (MB/s, x) | protobuf-csharp (MB/s) | protobuf-net (MB/s) |
|---|---|---|---|---|
| 136 B | encode | 1538.9 (3.32x) | 146.4 (0.32x) | 463.6 | 567.4 |
| 136 B | decode | 870.4 (1.80x) | 174.6 (0.36x) | 482.7 | 491.1 |
| 174 B | encode | 1478.9 (3.63x) | 121.2 (0.30x) | 407.5 | — |
| 174 B | decode | 789.0 (2.05x) | 126.8 (0.33x) | 384.8 | — |
| 2469 B | encode | 1191.3 (2.09x) | 56.0 (0.10x) | 569.7 | 392.3 |
| 2469 B | decode | 670.6 (1.43x) | 46.8 (0.10x) | 469.7 | 296.0 |
| 74913 B | encode | 1689.7 (3.46x) | 59.1 (0.12x) | 488.7 | 381.9 |
| 74913 B | decode | 750.7 (1.48x) | 51.5 (0.10x) | 506.9 | 328.0 |

Strings cross as NUL-terminated C strings, so `Set(field, string)` stops at the
first NUL byte: use the `ReadOnlySpan<byte>` overload for binary payloads (a
`string` field accepts them too, which is how protobuf's wire format works).

## Pure-C# core (no native library)

`bindings/csharp/Pbwire/Core/` is a second, self-contained engine with no
`DllImport`: it reads the same `FileDescriptorSet`, builds its own schema, and
encodes/decodes the same bytes, so a deployment with no C toolchain and no
`libpbwire` still uses pbwire. Nothing in `Pbwire.cs` changes: the P/Invoke
binding stays the default. The F# facade still rides the P/Invoke binding;
wiring it to this core is a separate change.

```csharp
using Pbwire.Core;

var schema = Schema.FromFile("../c/tests/data/matrix3.desc");
var ty = schema.Message("structtest.Scalars3");
var msg = ty.NewMessage().Set("i32", -42).Set("str", "alice");
msg.Add("tags", "a").Add("tags", "bb");        // repeated: appends (converted)
msg.NewMessage("inner").Set("name", "kid");

byte[] wire = ty.Encode(msg);                  // byte-identical with the C core
Msg back = ty.Decode(wire);                    // unknown fields retained
byte[] again = ty.Encode(back);                // byte-exact round trip
```

`ty.DecodeObject(wire)` / `ty.EncodeObject(dict)` are the plain-object forms the
pbench `core` arm times. Scope is the binary codec and the same subset the C
core owns: descriptor reader, encode, decode with unknown retention, groups,
packed runs, oneof, presence, proto2 required fields, depth/size limits, strict
tags and lengths, UTF-8 validation on proto3 strings. There is no JSON or text
printer and no deterministic/delimited/WKT helper in this core (use the P/Invoke
binding for those). `long`/`ulong` edges round-trip exactly, and no field takes
a `byte[]` on the hot path: repeated elements are boxed slots, strings/bytes
decoded off the wire stay slices of the input until read.

`make conformance-csharp` runs the official binary suite through it: 1412
successes, 1403 skipped (JSON/text input and output), 2 expected failures, 0
unexpected, same failure set as the C core. Numbers: `BENCHMARKS.md` and
`pbench/README.md`; it beats Google.Protobuf's generated codec on every encode
tier and loses the two large decodes to the boxed tree, so it is the
no-toolchain arm, not the shipped fast path.

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

## Core coverage

P/Invoke reaches the whole core through the binding; nothing is re-implemented:

| member | what it does |
|---|---|
| `MessageType.ParseJson(text, ignoreUnknown, maxBytes, maxDepth)` / `Message.PrintJson()` | JSON mapping, bounded |
| `MessageType.ParseText(text, maxBytes, maxDepth)` / `Message.PrintText(printUnknown)` | text format, bounded |
| `Message.EncodeDeterministic()` | map entries sorted by key |
| `Message.EncodeDelimited()` / `MessageType.DecodeDelimited(wire)` | framing, returns `consumed` |
| `Message.EncodedSize()` | exact size without writing |
| `MessageType.Visit(wire, callback)` | single-level scan; `false` stops it without throwing |
| `MessageType.AnyPack(url, payload)`, `Message.AnyTypeUrl()`/`AnyPayload()`, `MessageType.TimestampFromMillis(ms)` (+ duration), `Message.TimestampMillis()`/`DurationMillis()`, `Message.FieldMaskPaths()` | WKT helpers |
| `Schema.ServiceCount()`/`ServiceAt(i)`/`Service(name)`, `Schema.ResolveMethod(service, method)` | routing metadata |

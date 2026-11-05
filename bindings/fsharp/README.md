# pbwire F# binding

`bindings/fsharp/runtime` is the F# binding: an idiomatic facade over the C#
binding's dynamic API (`../csharp/Pbwire`), which is P/Invoke over the C ABI in
[`include/pbwire.h`](../../include/pbwire.h). Like Kotlin over Java, this adds
no wire code and no second schema path: a `FileDescriptorSet`
(`protoc --descriptor_set_out`) becomes the schema at run time, and the wire
format lives in the C core, so an F# message and a C message are the same bytes
by construction.

```fsharp
open Pbwire.FSharp

use schema = Pbwire.schema "tests/data/matrix3.desc"
let ty = Pbwire.messageType schema "structtest.Scalars3"
let innerTy = Pbwire.messageType schema "structtest.Inner3"

use msg =
    Pbwire.build ty (fun msg ->
        Pbwire.setAll msg [ "i32", Int -42L; "str", Text "alice" ] |> ignore
        msg.Set("tags", "a") |> ignore
        Pbwire.nested msg "inner" (fun inner ->
            inner.Set(innerTy.Field "name", "kid") |> ignore)
        |> ignore)

let wire = Pbwire.encode msg
use decoded = Pbwire.parse ty wire            // unknown fields preserved
if decoded.Encode() <> wire then
    failwith "round trip differs"
```

Fields are addressed by name (`"i32"`) or by a `Field` handle from
`ty.Field("i32")`; the handle skips the name lookup and is what a hot loop
should reuse. `FieldValue` (`Int`/`Bool`/`Float`/`Double`/`Text`/`Bytes`) is the
record-friendly write path for `setAll`; singular reads come back as options
(`tryString`, `tryBytes`, `tryMessage`), repeated items via `count`/`at`, and
refused input as `Error` through `tryParse` instead of an exception.

Strings cross as NUL-terminated C strings, so `Text` stops at the first NUL
byte: use `Bytes` for binary payloads (a `string` field accepts them too, which
is how protobuf's wire format works).

## Requirements

The .NET SDK 8+ (built and tested with 10), `make shared`, and nothing else:
the runtime project has no package references (only a `ProjectReference` to the
C# binding). `PBWIRE_LIB` overrides the library path. The benchmarks and the
generated-code baseline need NuGet access (`dotnet restore` fetches
`Google.Protobuf`).

## Build and run

```shell
dotnet run --project tests -- ../c/tests/data/matrix3.desc        # self-check
dotnet run --project examples -- ../c/tests/data/matrix3.desc     # example
```

## Benchmarks

```shell
./benchmarks/bench.sh    # vs Google.Protobuf C#, four tiers
```

The four tiers are small / medium / large / xlarge (the repeated fields scaled
x0 / x1 / x100 / x3000). Measured on Apple M4 Pro, .NET 10.0.403; higher is
better, MB/s over wire bytes (the official baseline itself moves ±10% between
runs, so the ratios are in-run):

| payload | op | pbwire-fsharp (MB/s, x protobuf-csharp) | protobuf-csharp (MB/s) |
|---|---|---|---|
| 136 B | encode | 1363.7 (3.0x) | 452.8 |
| 136 B | decode | 911.6 (1.9x) | 482.1 |
| 174 B | encode | 1353.3 (4.0x) | 338.5 |
| 174 B | decode | 811.6 (2.1x) | 390.9 |
| 2469 B | encode | 1237.7 (2.2x) | 560.8 |
| 2469 B | decode | 649.5 (1.5x) | 431.2 |
| 74913 B | encode | 1480.3 (3.1x) | 473.3 |
| 74913 B | decode | 673.7 (1.9x) | 364.0 |

Every tier is gated before timing: the official bytes must land on the pinned
size, `pbwire-fsharp` must encode byte-identical bytes, and decoding the
reference and re-encoding must return the same bytes. Unlike the C# harness
this one skips protobuf-net: it singular-encodes single-element packed fields,
so two of the four tiers carry nothing MB/s-fair to time.

## Conformance

```shell
dotnet run --project conformance -- ../../build/conformance.desc   # needs `make conformance-setup`
```

`conformance/Testee.fs` speaks the runner's wire protocol through the facade
only (the request/response envelopes are dynamic messages too, exactly like the
Java testee). Binary in → binary out is fully covered (1389 successes, 0
unexpected failures — the same 2 expected failures the C testee lists in
`conformance/failure_list_pbwire.txt`); JSON and text answer `skipped`, because
the binding exposes no printers. The testee binary needs the .NET runtime on
the runner's path (`DOTNET_ROOT_ARM64` when `dotnet` lives outside
`/usr/local/share/dotnet`, e.g. Homebrew).

## Ownership and threads

A `Schema` and a root `Message` own native memory: `Dispose()` (`use`) frees
it. A nested message is a view of its parent and never frees anything. Nothing
is shared, so one tree belongs to one thread, like the C core.

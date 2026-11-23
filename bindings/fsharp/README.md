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

The F# facade has no separate port: `make fsharp-bench` prints
`pbwire-fsharp-tree` (the `Message` handle the facade exposes, fresh build
per iteration with a full field read-back on decode),
`pbwire-fsharp-shim` (the C# generated model's native-object entry) and a
same-run `pbwire-fsharp-proxy` (held handle, published, never the headline)
through the C# runtime, so all three rows measure the same shipped code the
C# binding publishes.

## Benchmarks

```shell
./benchmarks/bench.sh    # vs Google.Protobuf C#, four tiers
```

The four tiers are small / medium / large / xlarge (the repeated fields scaled
x0 / x1 / x100 / x3000). Measured on Apple M4 Pro, .NET 10.0.403; higher is
better, MB/s over wire bytes (the official baseline itself moves ±10% between
runs, so the ratios are in-run):

| payload | op | pbwire-fsharp-tree (MB/s, x protobuf-csharp) | pbwire-fsharp-shim (MB/s, x) | protobuf-csharp (MB/s) |
|---|---|---|---|---|
| 136 B | encode | 110.0 (0.24x) | 1149.1 (2.52x) | 455.6 |
| 136 B | decode | 181.9 (0.40x) | 705.3 (1.56x) | 450.7 |
| 174 B | encode | 94.3 (0.23x) | 933.5 (2.29x) | 407.0 |
| 174 B | decode | 132.8 (0.35x) | 579.1 (1.52x) | 379.8 |
| 2469 B | encode | 76.6 (0.13x) | 628.8 (1.07x) | 588.2 |
| 2469 B | decode | 533.1 (1.13x) | 351.2 (0.75x) | 470.3 |
| 74913 B | encode | 72.3 (0.15x) | 661.4 (1.33x) | 495.8 |
| 74913 B | decode | 713.1 (1.47x) | 354.7 (0.73x) | 486.6 |

Same-run `proxy` rows (held handle, never the headline): encode
1443.2 / 1400.8 / 1353.5 / 1583.6 MB/s, decode
862.5 / 796.1 / 673.0 / 714.0 MB/s. Formal run
`results/bench-fsharp-20261123T005908Z.json`.

Every tier is gated before timing: the official bytes must land on the pinned
size, the fresh-build `tree` arm must encode byte-identical bytes, the decode
must expose every gate-checked field, and re-encoding must return the same
bytes. Unlike the C# harness
this one skips protobuf-net: it singular-encodes single-element packed fields,
so two of the four tiers carry nothing MB/s-fair to time.

## Conformance

```shell
dotnet run --project conformance -- ../../build/conformance.desc   # needs `make conformance-setup`
```

`conformance/Testee.fs` speaks the runner's wire protocol through the facade
only (the request/response envelopes are dynamic messages too, exactly like the
Java testee). Binary in → binary out is fully covered (2750 successes, 0
unexpected failures — the same 2 expected failures the C testee lists in
`conformance/failure_list_pbwire.txt`); JSON and text in both directions go
through the C# binding's printers and parsers, so only JSPB answers `skipped`. The testee binary needs the .NET runtime on
the runner's path (`DOTNET_ROOT_ARM64` when `dotnet` lives outside
`/usr/local/share/dotnet`, e.g. Homebrew).

## Ownership and threads

A `Schema` and a root `Message` own native memory: `Dispose()` (`use`) frees
it. A nested message is a view of its parent and never frees anything. Nothing
is shared, so one tree belongs to one thread, like the C core.

## Generated models

`protoc-gen-pbwire` (in `../../codegen`) emits `<base>.fs` (records) and
`<base>_meta.fs` (the embedded descriptor set plus its loader).
`--pbwire_opt=lang=fsharp[,model=bool][,register=bool][,registry=bool]`
(all default `true`): `model=false` leaves the schema module for
dynamic-only users; `register=false` drops the eager load so `initSchema`
(embedded bytes) or `useSchema` (an external `Schema`, dual registration)
must run first; `registry=false` drops `messageNames` + `preload`.

## Core coverage

The facade passes every call to the C# binding, so the whole core is reachable
without a second implementation:

| member | what it does |
|---|---|
| `printJson`/`parseJson` and `printText`/`parseText` | JSON mapping and text format |
| `encodeDeterministic` | map entries sorted by key |
| `encodeDelimited`/`decodeDelimited` | framing, with `consumed` |
| `encodedSize` | exact size without writing |
| `visit` | single-level scan; `false` stops it without throwing |
| `anyPack`, `anyTypeUrl`/`anyPayload`, `timestampFromMillis`/`timestampMillis` (+ duration), `fieldMaskPaths` | WKT helpers |
| `serviceCount`/`serviceAt`/`service`, `resolveMethod` | routing metadata |
| `decodeLimits(ty, wire, maxBytes, maxDepth)` | input bounds |

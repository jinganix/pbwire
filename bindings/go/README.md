# pbwire Go binding

cgo dynamic binding over the shared C core: it loads a `FileDescriptorSet`
at run time (`protoc --descriptor_set_out` bytes), builds dynamic message
trees and encodes/decodes them through the C ABI. Schemas hot-swap without
a restart — `Registry.Swap` loads the next version while in-flight messages
keep the one they were built from.

```go
reg, _ := dynamic.NewRegistry(descSet)   // descSet: FileDescriptorSet bytes
typ, _ := reg.Current().Message("structtest.Scalars3")

m := typ.New()
m.SetInt("i32", -7)
sub, _ := m.NewMessage("inner")
sub.SetString("name", "kid")
wire, _ := m.Encode()                     // one cgo downcall
m.Close()                                // explicit: no finalizer on the hot path

back, _ := typ.Decode(wire)
back.Close()
```

Most code never touches that surface: generate protoc-gen-go-shaped structs
once (`make -C codegen gen-go`) and use them like the library they
mirror — same field names, same getters, same observable behavior, but the
wire format stays in the C core:

```go
import gen "pbwire/tests/gen/matrix3"

m := &gen.Scalars3{I32: -7, Str: "kid", Inner: &gen.Inner3{Name: "x", Value: 1}}
wire, _ := m.Marshal()                // no schema argument; embedded set
back := &gen.Scalars3{}
back.Unmarshal(wire)
back.GetStr()                          // "kid" (nil-safe, like protoc-gen-go)
back.OptI32                            // nil when absent: optionals are pointers
```

Optional fields are pointers (`nil` = absent), oneofs are interface
wrappers (`isOneof1_Kind`), repeated fields are slices — the same shapes
`protoc-gen-go` emits. Unknown wire fields are preserved through the C
core; a `proto.Message` interface is deliberately not implemented (maps
and cross-file references stay on the dynamic API, as in every binding).

## Codegen options

`--pbwire_opt=lang=go[,model=bool][,register=bool][,registry=bool]`
(all default `true`):

- Each file generates two halves: `<base>_pbwire.go` (structs) and
  `<base>_pbwire_meta.go` (embedded descriptor set plus its loader).
  `model=false` leaves the meta half for dynamic-only users.
- `register=false` drops the implicit once-load: call `Init()` (embedded)
  or `UseSchema()` (external) first, failing loudly otherwise.
- `registry=false` drops `MessageNames` + `Preload()`; the loader stays.

Two registrations, one model. By default structs load the embedded set
on first use. `UseSchema(dynamic.Load(descBytes))` points them at an
external descriptor file instead (nil restores embedded); `Preload()`
warms every generated type so later calls pay no lookup.

| Piece | Path | Role |
|---|---|---|
| dynamic | [`dynamic/`](dynamic/) | the binding: `Registry` (hot-swap), `Schema`, `MessageType`, `Message` |
| dynamic tests | [`dynamic/dynamic_test.go`](dynamic/dynamic_test.go) | tier parity (136/174/2469/74913 B) + hot-swap, desc built by `protoc` |
| bench | [`benchmarks/bench_test.go`](benchmarks/bench_test.go) (`bench.sh` generates `gen/`) | four standard tiers vs `protobuf-go` generated code, gated on byte parity before timing |

```shell
make go-test    # dynamic binding self-check (parity + hot-swap, needs protoc)
make go-bench   # four standard tiers vs protobuf-go (needs Go + protoc + protoc-gen-go)
```

Explicit `Close` discipline: roots own their C tree and must be freed with
`Close`; nested views from `NewMessage` read into the root and are never
freed separately. `Decode` passes the input slice straight into C
(zero-copy in); `Encode` copies the bytes out.

## Core coverage

The dynamic API mirrors the core one for one; each call is a single cgo
downcall:

| member | what it does |
|---|---|
| `Message.ToJSON()` / `MessageType.ParseJSON(text, ignoreUnknown)` | JSON mapping |
| `Message.ToText(printUnknown)` / `MessageType.ParseText(text)` | text format |
| `Message.EncodeDeterministic()` | map entries sorted by key |
| `Message.EncodeDelimited()` / `MessageType.DecodeDelimited(wire)` | framing, returns `consumed` |
| `Message.EncodedSize()` | exact size without writing |
| `MessageType.DecodeLimits(wire, maxBytes, maxDepth)`, `ParseJSONLimits(...)`, `ParseTextLimits(...)` | input bounds on every decode/parse |
| `MessageType.Visit(wire, maxBytes, maxDepth, cb)` | single-level scan (callback via `cgo.Handle`) |
| `Message.AnyTypeURL()`/`AnyPayload()`, `Message.TimestampMillis()`/`DurationMillis()`, `Message.FieldMaskPathCount()`/`FieldMaskPathAt(i)`, `Schema.ResolveMethod(service, method)` | WKT helpers and routing |

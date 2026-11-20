# pbwire Go binding

Two dynamic cores, one wire implementation:

- **C core** ([`dynamic/`](dynamic/)): cgo over the shared `libpbwire.a`. It
  loads a `FileDescriptorSet` at run time, builds message trees in C, and
  hot-swaps schemas without a restart (`Registry.Swap`); in-flight messages
  keep the schema they were built from.
- **Go core** ([`codec/`](codec/)): the same runtime schema, decoded into a
  pure-Go value tree (`codec.DMsg`) and encoded by the shared table engine.
  No cgo, no generated code, no second wire implementation.

Generated models are thin wrappers over the Go core: a `protoc-gen-pbwire`
struct plus one metadata file of field tables. Nothing in generated code
decides a tag, a length, presence or skipping.

## Dynamic C core

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

Roots own their C tree and must be freed with `Close`; nested views from
`NewMessage` read into the root and are never freed separately. `Decode`
passes the input slice straight into C (zero-copy in); `Encode` writes into
a Go slice (one downcall, nothing to free).

## Dynamic Go core

Same schema bytes, no cgo: `codec.Load` turns a `FileDescriptorSet` into
per-message plans, `Schema.Type` binds one to a runtime table, and `DMsg`
holds the values.

```go
s, _ := codec.Load(descSet)               // protoc --descriptor_set_out bytes
typ, _ := s.Type("structtest.Scalars3")

m := typ.New()
m.SetInt("i32", -7)                       // semantic values: sint32 -7 reads back -7
sub, _ := m.NewMessage("inner")
sub.SetString("name", "kid")
wire, _ := m.Encode()                      // table engine, no cgo

back, _ := typ.Decode(wire)                // retains unknown fields, tag included
nums, _ := back.GetIntSlice("nums")
```

Wire-first scope: encode, decode, field access and unknown-field retention.
JSON, text, delimited framing, limits, `Visit`, the WKT helpers and
`Registry` hot swap stay C-core-only for now. Maps ride as repeated entry
messages in first-seen order with no sort: byte equality across runs is not
promised for multi-entry maps until a deterministic pass exists.

## Thin generated model

```go
import gen "pbwire/tests/gen/matrix3"

m := &gen.Scalars3{I32: -7, Str: "kid", Inner: &gen.Inner3{Name: "x", Value: 1}}
wire, _ := m.Marshal()                // codec.EncodeModel: no schema argument
back := &gen.Scalars3{}
back.Unmarshal(wire)
back.GetStr()                          // "kid" (nil-safe, like protoc-gen-go)
back.OptI32                            // nil when absent: optionals are pointers
```

Optional fields are pointers (`nil` = absent), oneofs are interface
wrappers (`isOneof1_kind`), repeated fields are slices — the same shapes
`protoc-gen-go` emits. A `proto.Message` interface is deliberately not
implemented (maps ride as repeated entry structs, as in every binding), and
unknown wire fields are dropped on decode, as before: the struct has nowhere
to keep them. Reach for the two dynamic cores when that matters.

| Piece | Path | Role |
|---|---|---|
| model | `tests/gen/<pkg>/<base>_pbwire.go` | structs, enums, getters, `Reset`, one-line `Marshal`/`Unmarshal` |
| metadata | `tests/gen/<pkg>/<base>_pbwire_meta.go` | one field table per message: facts and trivial accessors, no wire logic |
| dynamic Go core | [`codec/`](codec/) | `Schema`/`DType`/`DMsg`, the one `EncodeTable`/`DecodeTable` engine, descriptor-set plans |
| dynamic C core | [`dynamic/`](dynamic/) | `Registry` (hot-swap), `Schema`, `MessageType`, `Message` |
| bench | [`benchmarks/bench_test.go`](benchmarks/bench_test.go) (`bench.sh` writes `gen/matrix3.desc` with `protoc`) | four standard tiers: `pbwire-go dync`, `pbwire-go dyngo`, `protobuf-go`, gated on byte parity and a field-by-field decode before timing |
| arms | [`pbench/`](pbench/) (`go test -tags pbench ./pbench/`) | the three dynamic arms over the same tiers, native struct in and out (issue #41) |

```shell
make go-test    # dynamic C core checks + Go core parity + generated-model compat (needs protoc)
make go-bench   # four standard tiers, both cores, against protobuf-go
```

Generated models are split like Java's: the message file holds the data types
and the forwarding, the metadata file holds the field tables, so a
hand-written model can replace the generated one without touching the codec.

## Codegen

`--pbwire_opt=lang=go` needs no further options: there is no descriptor to
embed and no schema registry to warm, so a generated model never consults a
`FileDescriptorSet` at run time.

## Core coverage (C core)

The `dynamic` API mirrors the core one for one; each call is a single cgo
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

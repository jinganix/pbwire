# pbwire Go binding

Shipped: thin generated models over the pure-Go table engine
[`runtime/`](runtime/): no cgo, no layout measurement,
no schema argument. The generated metadata file holds one field table per
message; the one shared engine interprets every table. The same package also
carries the descriptor-set schema (`Schema`/`DType`/`DMsg`) and the
model<->tree converters the dynamic arms share.

A generated model is a `protoc-gen-pbwire` struct plus one metadata file of
field tables. Nothing in generated code decides a tag, a length, presence or
skipping. The dynamic C candidate keeps its engine in [`pbench/`](pbench/)
next to its arm; the pure-Go table engine ships as [`runtime/`](runtime/).

## Dynamic C core (candidate, not shipped)

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

## Pure-Go engine (`runtime/`)

What the generated model falls back to, and the value-tree API on top of it:
`runtime.Load` turns a `FileDescriptorSet` into per-message plans,
`Schema.Type` binds one to a runtime table, and `DMsg` holds the values.

```go
s, _ := runtime.Load(descSet)             // protoc --descriptor_set_out bytes
typ, _ := s.Type("structtest.Scalars3")

m := typ.New()
m.SetInt("i32", -7)                       // semantic values: sint32 -7 reads back -7
sub, _ := m.NewMessage("inner")
sub.SetString("name", "kid")
wire, _ := m.Encode()                      // table engine, no cgo

back, _ := typ.Decode(wire)                // retains unknown fields, tag included
nums, _ := back.GetIntSlice("nums")
```

Full-runtime scope: encode, decode, field access and unknown-field retention,
plus JSON, text, deterministic encode, delimited framing, encoded size,
limits, `Visit`, the WKT helpers and `Registry` hot swap -- all on `DMsg`,
all in this package. The candidate in [`pbench/dync/dynamic/`](pbench/dync/dynamic/)
keeps the C-core arm for comparison only. Maps ride as repeated entry
messages in first-seen order; `EncodeDeterministic` sorts native map-entry
rows by key, so equal trees encode byte-identical bytes across runs.

## Thin generated model

```go
import gen "pbwire/tests/gen/matrix3"

m := &gen.Scalars3{I32: -7, Str: "kid", Inner: &gen.Inner3{Name: "x", Value: 1}}
wire, _ := m.Marshal()                // table engine: no schema argument
back := &gen.Scalars3{}
back.Unmarshal(wire)
back.GetStr()                          // "kid" (nil-safe, like protoc-gen-go)
back.OptI32                            // nil when absent: optionals are pointers
```

Optional fields are pointers (`nil` = absent), oneofs are interface
wrappers (`isOneof1_kind`), repeated fields are slices — the same shapes
`protoc-gen-go` emits. A `proto.Message` interface is deliberately not
implemented (maps ride as repeated entry structs, as in every binding), and
unknown wire fields are retained on decode in an `unknownFields` buffer and
replayed after the known fields on encode, byte for byte like the
reference; `Reset` clears them with the rest. The pbench gate pins the
round trip (`TestGateSemantics`), including nested messages. Reach for
`runtime.DMsg (decode retains unknowns and binary
re-encode preserves them) when that matters.

| Piece | Path | Role |
|---|---|---|
| model | `tests/gen/<pkg>/<base>_pbwire.go` | structs, enums, getters, `Reset`, `Marshal`/`Unmarshal` via the table engine |
| metadata | `tests/gen/<pkg>/<base>_pbwire_meta.go` | one field table per message: facts and trivial accessors, no wire logic |
| engine | [`runtime/`](runtime/) | the shipped pure-Go engine: `Schema`/`DType`/`DMsg`, the one `EncodeTable`/`DecodeTable` engine, descriptor-set plans, model<->tree converters |
| C core candidate | [`pbench/dync/dynamic/`](pbench/dync/dynamic/) | `Registry` (hot-swap), `Schema`, `MessageType`, `Message`, model<->tree converter |
| bench | [`benchmarks/bench_test.go`](benchmarks/bench_test.go) | four standard tiers: thin model vs `protobuf-go`, gated on byte parity and a field-by-field decode before timing |
| arms | [`pbench/`](pbench/) (`go test -tags pbench ./pbench/`) | the C value tree and pure-Go value tree arms over the same tiers, native struct in and out, converter-only glue (issue #41) |

```shell
make go-test    # engine + converter + generated-model compat checks (needs protoc)
make go-bench   # four standard tiers, thin model against protobuf-go (needs Go only)
```

Generated models are split like Java's: the message file holds the data types
and the forwarding, the metadata file holds the field tables, so a
hand-written model can replace the generated one without touching the codec.

## Codegen

`--pbwire_opt=lang=go` needs no further options: there is no descriptor to
embed and no schema registry to warm, so a generated model never consults a
`FileDescriptorSet` at run time.

## Runtime coverage (shipped `runtime/` surface)

Everything below is the shipped pure-Go engine: no cgo, no schema argument at
call time. JSON, text, deterministic encode, delimited framing, encoded size,
limits, the WKT helpers, `Visit`, service routing and hot-swap `Registry` all
operate on `DMsg` (the dynamic message). Thin-model convenience wrappers stay
out of scope by design: a struct composes them from public API in two lines:

```go
d, _ := typ.ImportModel(m, gen.Scalars3Meta)
js, _ := typ.MarshalJSON(d)
```

| member | what it does |
|---|---|
| `Schema.Load(set)` / `Schema.Type(full)` / `Schema.Message/Enum/Extension/Service(full)` | descriptor-set plans: message, enum, extension and service routing |
| `Registry{Load, Swap, Current}` | hot-swap schemas; retired schemas stay valid for in-flight use |
| `DType.New/Decode/DecodeLimits` | build empty, parse wire bytes, parse with byte/depth bounds |
| `DType.Encode(m)` / `DType.EncodeDeterministic(m)` | serialize a tree; deterministic sorts native map-entry rows by key |
| `DType.DecodeDelimited/DecodeDelimitedLimits` | framing, returns `consumed`, trailing bytes left unread |
| `DMsg.EncodedSize()` / `DMsg.IsInitialized()` | exact size without writing (`-1` when depth-capped; Encode errors on the same tree); proto2 required-field check |
| `DType.MarshalJSON/UnmarshalJSON(+Limits)` | JSON mapping, `ignoreUnknown` flag, byte/depth bounds |
| `DType.MarshalText/UnmarshalText(+Limits)` | text format, `printUnknown` flag, byte/depth bounds |
| `DType.Visit(wire, lim, fn)` | single-level scan, raw bits, non-nil visitor aborts |
| `AnyPack/AnyUnpack`, `AnyTypeURL/AnyPayload`, `TimestampMillis/TimestampFromMillis`, `DurationMillis/DurationFromMillis`, `FieldMaskPaths/NewFieldMask` | WKT helpers |
| `DType.ImportModel/ExportModel` | struct tree converters shared with the thin generated models |
| `Encoder/Decoder` | wire primitives (tags, varints, fixed widths, skips) for hand-written codecs |

Known limitations, each needing an emitter change plus regen so parked, not
built: generated-model tables predate the schema UTF-8 flag, so proto3 strings
skip UTF-8 validation on the struct path; deterministic encode keeps foreign
(`ForeignOps`) rows in first-seen order while native map-entry rows sort by key.

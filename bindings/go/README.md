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

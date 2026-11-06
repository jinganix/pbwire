# pbwire Julia binding

`runtime/src/Pbwire.jl` is the Julia binding: `ccall` over the C ABI in
[`include/pbwire.h`](../../include/pbwire.h), loaded with `Libdl.dlopen` (no
C glue, no packages). A `FileDescriptorSet`
(`protoc --descriptor_set_out`) becomes the schema at run time; there is no
generated code and no `.proto` parser. Messages are `Dict`s at the boundary,
converted through the C ABI. The wire format lives in the C core, so a Julia
message and a C message are the same bytes by construction.

```julia
push!(LOAD_PATH, "bindings/julia/runtime")
using Pbwire

schema = from_file("bindings/c/tests/data/matrix3.desc")
ty = msgtype(schema, "structtest.Scalars3")

msg = new_message(ty)
set_int!(msg, "i32", -42)
set_string!(msg, "str", "alice")
for tag in ("a", "bb")
    set_string!(msg, "tags", tag)                        # repeated: appends
end
inner = set_message!(msg, "inner")                       # nested: in place
set_string!(inner, "name", "kid")
set_int!(inner, "value", 42)

wire = encode(msg)
decoded = decode(ty, wire)                               # unknown fields preserved
@assert as_string(at(decoded, "tags", 2)) == "bb"
@assert encode(decoded) == wire                          # byte-exact round trip

d = to_dict(decoded)          # plain Dicts: d["inners"][2]["value"] == 42
@assert encode_dict(ty, d) == wire
```

Fields are addressed by name (`"i32"`) or by a `Field` handle from
`field(ty, "i32")`; the handle skips the name lookup and is what a hot loop
should reuse. `set_*` returns the message, so calls chain; `at` is 1-based.

## Requirements

Julia 1.x (only `Libdl`, `Test` and `Printf` from the standard library) and
`make shared` (`build/libpbwire.dylib` / `.so`). `PBWIRE_LIB` points at the
library; otherwise it is found by walking up from the working directory or
from the binding file.

## Build and run

```shell
julia --project=runtime tests/runtests.jl   # self-check vs pinned protoc bytes
julia --project=runtime examples/roundtrip.jl
```

## Benchmarks

```shell
julia --project=runtime benchmarks/bench.jl # vs JuliaIO/ProtoBuf.jl, four tiers
```

Without `benchmarks/oracle/matrix3_pb.jl` (generated with `protoc
--julia_out`) the script times pbwire only and says so. The four tiers are
small / medium / large / xlarge (the repeated fields scaled x0 / x1 / x100 /
x3000). Copied from [BENCHMARKS.md](../../BENCHMARKS.md), which is the single
source for all performance numbers; higher is better, MB/s over wire bytes.

Strings cross as NUL-terminated C strings, so `set_string!` rejects embedded
NUL bytes: use `set_bytes!` for binary payloads (a `string` field accepts them
too, which is how protobuf's wire format works).

## Ownership and threads

A `Schema` and a root `Message` own native memory: `close` frees it, and a
finalizer catches the ones that are not closed. A `MsgType` keeps its
`Schema` (and a `Message` its `MsgType`, a nested message its parent)
reachable, so garbage collection cannot free the schema under a live handle;
an explicit `close` is still the caller's promise that nothing else uses it.
A nested message is a view of its parent, holds it alive, and never frees
anything. One message tree belongs to one thread, like the C core.

## Generated models

`protoc-gen-pbwire` (in `../../codegen`) emits `<base>_pbwire.jl`
(structs over this dynamic API) and `<base>_meta.jl` (the embedded
descriptor set plus its loader, included by path). `--pbwire_opt=lang=julia`
`[,model=bool][,register=bool][,registry=bool]` (all default `true`):
`model=false` leaves the schema file for dynamic-only users;
`register=false` drops the lazy load so `init_schema()` (embedded bytes)
or `use_schema()` (an external schema, dual registration) must run first;
`registry=false` drops `MESSAGE_NAMES` + `preload()`.

## Conformance

```shell
julia --project=runtime conformance/testee.jl build/conformance.desc
```

Speaks the runner's framing (little-endian length prefix) and answers binary
in → binary out from the schema; JSON/text requests answer `skipped`.

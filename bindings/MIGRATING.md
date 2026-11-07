# Migrating from the official runtimes

Three mental-model shifts, then one row per language. Bytes on the wire are
identical (see `../BENCHMARKS.md`), so migration is client-by-client: a pbwire
service talks to an official-runtime service with no flag day.

## The three shifts

1. **Schema comes from bytes, not from build output.** Replace the generated
   module (`*_pb2.py`, `*.pb.go`, `FooProto.java`, ...) with a
   `FileDescriptorSet` built once (`protoc --descriptor_set_out`,
   `buf build -o`) and loaded at startup. The `.proto` still exists; the
   per-language build step does not.
2. **Messages are data, not classes.** There is no `Foo.newBuilder()` /
   `Foo()` constructor: create a message from its descriptor, set fields by
   number or name, read them back the same way. `codegen/` emits typed
   wrappers where you want them, but they call the same dynamic API.
3. **Unknown fields survive.** Anything the schema does not declare is kept
   as raw bytes and re-emitted in order. Gateways and mixed-version fleets
   get passthrough for free; generated-code users had to opt into retention.

## Per-language notes

| from (official) | to (pbwire) | what changes in your code |
|---|---|---|
| protoc C++ generated / `DynamicMessage` | `bindings/c/`, `bindings/cpp/` | link `libpbwire.a`, load the set, use the struct codec (C) or the dynamic API (C++); drop libprotobuf from the link |
| `google.protobuf` (upb, Python) | `bindings/python/` | replace `*_pb2` imports with `schema.message(name)`; dicts stay dicts, `decode()` returns a `Message` (call `.to_dict()` only for fields you read) |
| `google-protobuf` gem (Ruby) | `bindings/ruby/` | replace generated `*_pb.rb` requires with schema load; builder for writes, lazy view for reads |
| `protobuf-java` generated | `bindings/java/` | replace generated classes with `DMsg` + workspace; no JNI, no native lib to ship |
| `protobuf-kotlin` | `bindings/kotlin/` | same runtime as the Java row, DSL syntax replaces builders |
| jspb / protobufjs (Node) | `bindings/node/` | replace `require('*_pb.js')` with the N-API addon + set; decoded values materialize on read |
| `protobuf-objc` (Objective-C) | `bindings/objc/` | replace generated `*.pbobjc` with the ARC wrapper; `NSData` I/O unchanged |
| `protobuf/php` pure or C ext (PHP) | `bindings/php/` | replace generated classes with the C extension (or the `ext-ffi` wrapper where no compiler exists) |
| `protobuf` pub package (Dart) | `bindings/dart/` | replace generated `.pb.dart` with the `dart:ffi` host + set |
| rust-protobuf / prost (Rust) | `bindings/rust/` | replace `build.rs` codegen with a static link + set loaded at startup |
| Google.Protobuf / protobuf-net (C#) | `bindings/csharp/` | replace generated `.cs` with P/Invoke calls; no NuGet runtime |
| `google.golang.org/protobuf` (Go) | `bindings/go/` | replace generated `.pb.go` + registry with `dynamic` + a set file; schemas hot-swap without restart |
| gpb / gpb NIF (Erlang) | `bindings/erlang/` | keep `gpb_compile` output, point `pbwire:load_cache/1` at it; records unchanged |
| swift-protobuf (Swift) | `bindings/swift/` | replace generated `.pb.swift` with the SwiftPM FFI host + set |
| ScalaPB (Scala) | `bindings/scala/` | replace generated case classes with the DSL over the shared Java runtime |
| elixir-protobuf (Elixir) | `bindings/elixir/` | keep generated structs, swap the codec for the shared NIF |
| zig-protobuf (Zig) | `bindings/zig/` | replace per-message codegen with the static link + set |
| Google.Protobuf via C# (F#) | `bindings/fsharp/` | facade over the C# binding; computation expressions replace builders |
| protobuf-java interop (Clojure) | `bindings/clojure/` | persistent maps in/out over the shared Java runtime |
| ProtoBuf.jl (Julia) | `bindings/julia/` | replace `@proto` modules with `dlopen`/`ccall` + set; `Dict` boundary |
| RProtoBuf (R) | `bindings/r/` | replace generated descriptors with the C extension; named lists in/out |
| lua-protobuf / pbc (Lua) | `bindings/lua/` | replace `protobuf.register` blobs with the C module + set; tables unchanged |
| nanopb (generated C, compile-time schema) | `bindings/embedded/` | replace the generated structs and accessors with `pbwe_decode`/`pbwe_encode` over one box per thread; the schema becomes a `FileDescriptorSet` at startup, `pbwe_set_limits` replaces the compile-time size caps |

## What intentionally has no equivalent

- **RPC/stubs.** Neither the core nor any binding generates service stubs;
  bring your own transport (gRPC, Twirp, plain HTTP) and use pbwire for the
  payload. Service descriptors load and are queryable for routing.
- **Canonical bytes.** Deterministic mode sorts map keys only, like the
  reference — do not hash the wire for cross-version identity.
- **`DynamicMessage` reflection idioms.** `ListFields()`-style walks become
  `pbw_message_present_list()` / `pbw_message_field_at()`; cheaper, but the
  call shapes differ per binding — read that binding's README, not this file.

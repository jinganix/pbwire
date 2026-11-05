# pbwire FAQ

- [Is pbwire a Protobuf code generator?](#is-pbwire-a-protobuf-code-generator)
- [Does it need `protoc` at runtime?](#does-it-need-protoc-at-runtime)
- [Which Protobuf syntaxes are supported?](#which-protobuf-syntaxes-are-supported)
- [Does pbwire support JSON or text format?](#does-pbwire-support-json-or-text-format)
- [Can I use it from another language?](#can-i-use-it-from-another-language)
- [When should I *not* use pbwire?](#when-should-i-not-use-pbwire)
- [Is it thread-safe?](#is-it-thread-safe)
- [What happens to fields the schema does not declare?](#what-happens-to-fields-the-schema-does-not-declare)
- [Why does it walk a descriptor instead of compiling field offsets in?](#why-does-it-walk-a-descriptor-instead-of-compiling-field-offsets-in)
- [Can I decode untrusted bytes with it?](#can-i-decode-untrusted-bytes-with-it)
- [How do I load a `.proto` file at runtime?](#how-do-i-load-a-proto-file-at-runtime)
- [Do I have to recompile when a `.proto` changes?](#do-i-have-to-recompile-when-a-proto-changes)
- [Where do I report a bug?](#where-do-i-report-a-bug)


### Is pbwire a Protobuf code generator?

No. It is a runtime: descriptors in, dynamic messages out. Nothing is generated,
nothing is compiled per schema, and adding a message type does not require a
rebuild — unless you embedded the descriptor set in the binary (see
[Do I have to recompile when a `.proto` changes?](#do-i-have-to-recompile-when-a-proto-changes)).

### Does it need `protoc` at runtime?

No. `protoc` (or `buf`, or the optional `pbwire-protoc` library) produces a
`FileDescriptorSet` once; pbwire reads those bytes at runtime.

### Which Protobuf syntaxes are supported?

proto2 and proto3, including edition-2023 descriptor sets (treated as proto3
semantics). Groups decode into their submessage like message fields, including
`required` checks inside them. Extensions attach to their extendee
(`pbw_field_is_extension()` tells them apart; MessageSet members keep their wire
bytes as unknown fields). A proto2 closed enum's unknown number is kept as an
unknown field, and `pbw_decode` refuses a message missing a `required` field.

### Does pbwire support JSON or text format?

Text format yes (`pbw_text_print` / `pbw_text_parse`, byte-identical printer
output to the reference, strict parser parity including extensions, groups,
`Any` and unknown fields). JSON yes (`pbw_json_print` / `pbw_json_parse`, same
verdicts as the reference parser on 40,000 mutated inputs, all well-known types,
extensions as `[full.name]`, strict base64 and number rules). See
[guide.md](guide.md).

### Can I use it from another language?

Yes — that is the intended use. All twenty-three bindings live under [`bindings/`](../bindings/README.md), each with a round-trip
test, a benchmark against its official runtime and, where it applies, the
conformance testee. The per-language pain point each one removes is the
[table in bindings/README.md](../bindings/README.md#what-each-binding-solves);
the short version:

| language | how it reaches the core | schema |
|---|---|---|
| C | direct call | `FileDescriptorSet`, plus `plugin/` layout tables |
| C++ | direct call over the C runtime | `FileDescriptorSet` |
| Python | CPython extension, dict↔wire in C | `FileDescriptorSet` |
| Ruby | C extension linked against the core | `FileDescriptorSet` |
| Java | own pure-Java codec (no JNI, no native library) | `FileDescriptorSet` |
| Kotlin | the Java runtime + a DSL | `FileDescriptorSet` |
| Node | hand-written N-API addon (no npm) | `FileDescriptorSet` |
| Objective-C | ARC wrapper (Foundation only) | `FileDescriptorSet` |
| PHP | native C extension (`ext-ffi` wrapper without a compiler) | `FileDescriptorSet` |
| Dart | `dart:ffi` | `FileDescriptorSet` |
| C# | `DllImport` + a resolver | `FileDescriptorSet` |
| Rust | links `libpbwire.a` statically | `FileDescriptorSet` |
| Go | cgo dynamic binding over the C core | `FileDescriptorSet` (hot-swappable) |
| Erlang | NIF; gpb records via `pbwire:load_cache/1` | gpb's `get_msg_defs/0` |
| Lua | C module over the core, tables in/out | `FileDescriptorSet` |
| Swift | SwiftPM package, FFI host over the C ABI | `FileDescriptorSet` |
| Scala | the Java runtime + a DSL | `FileDescriptorSet` |
| Elixir | struct translation over the shared NIF | `elixir-protobuf` structs |
| Zig | static link, core sources compiled in | `FileDescriptorSet` |
| F# | facade over the C# project | `FileDescriptorSet` |
| Clojure | persistent maps over the Java runtime | `FileDescriptorSet` |
| Julia | `dlopen` + `ccall` FFI host | `FileDescriptorSet` |
| R | C extension, named lists in/out | `FileDescriptorSet` |

The C bindings check `PBW_ABI_VERSION` at load time, so a library built from a
newer, incompatible tree fails fast instead of corrupting memory. The C ABI is
the product; richer host bindings live on top of it. `codegen` adds generated
message models per language over the same dynamic API.

### When should I *not* use pbwire?

When the schema is fixed at build time, you never touch a descriptor, and
libprotobuf is already in the process: then generated code is the simpler
choice, with fewer moving parts and no runtime schema to ship. pbwire still wins
the throughput comparison even there (the in-process core is 1.2-1.7x the
generated C++ codec), but throughput is not the only axis — a straight-line
codec has no schema to load and no reflection work to do. pbwire also does not
do RPC. See [../README.md](../README.md#comparison-with-generated-code-dynamicmessage-and-upb) and
[design.md](design.md).

### Is it thread-safe?

There is no global state. A schema or message must be confined to one thread by
the caller, and one `pbw_ctx` per thread keeps arena reuse local. No locks, no
thread-local buffers.

### What happens to fields the schema does not declare?

They are preserved as raw wire bytes and re-emitted after the known fields, in
input order. A gateway can forward messages for a schema it only partially knows.
Unknown fields are why 7 conformance tests pass that would fail otherwise.

### Why does it walk a descriptor instead of compiling field offsets in?

Generated code compiles field offsets into straight-line C++; pbwire walks a
descriptor and a value tree at runtime, which costs roughly 40 ns per encode of a
132-byte message. That cost is mostly paid back by writing the message in one
pass (the C++ serializer sizes first, then writes) and by precomputed tag bytes,
so the in-process `pbw_ctx` API is 1.1-1.7x the generated codec on every tier.
The gap only shows up in two places: the plain `pbw_encode` API, which allocates
a tree per call (about parity with generated code at the small tiers, either way
within run noise), and a dynamic codec meeting generated straight-line writes on
a tiny message (Java/Kotlin, 0.69x/0.82x at 136 B). Numbers
and the per-language
reasons are in [BENCHMARKS.md](../BENCHMARKS.md).

### Can I decode untrusted bytes with it?

Yes: lengths are bounds-checked, field number 0 and wire types above 5 are
rejected, nesting is capped at 256 (`PBW_MAX_DEPTH`), and every failure returns a
`pbw_status` instead of aborting. `pbw_limits` lowers the caps per call
(`max_bytes`, `max_depth`) for stricter hosts. Descriptors, by contrast, are
trusted input — see [design.md](design.md).

### How do I load a `.proto` file at runtime?

Convert it to a `FileDescriptorSet` once (`protoc --include_imports
--descriptor_set_out`, `buf build -o`, or the optional `pbwire-protoc` library),
then call `pbw_descriptor_set_load()` and `pbw_schema_link()` is already done for
you. All imported files must be in the set. See [guide.md](guide.md).

### Do I have to recompile when a `.proto` changes?

Only if you baked the schema into the binary. The loader takes bytes
(`pbw_descriptor_set_load`), so a sidecar file and embedded bytes are the same
call — the difference is deployment, not API:

- **External file (default).** Ship the new `FileDescriptorSet` next to the
  binary and reload the schema in the running process; no rebuild. Messages of
  the old schema stay valid until you free that schema.
- **Embedded bytes (single binary).** Compile the set in (`xxd -i`, Rust
  `include_bytes!`, app resources) and load it from memory
  (`from_bytes` / `fromBytes` / `load(bytes)`, depending on the binding). No
  sidecar file to lose, but a `.proto` change means rebuilding and redeploying.

Exceptions: Erlang uses gpb's compiled definitions and recompiles by
construction. The C struct binding still validates its generated layout against
a descriptor set at run time, so embed that set too rather than dropping it.

### Where do I report a bug?

A minimal reproduction: the bytes (or `.proto`), the expected behavior, and
whether it came from `make difftest` / `make conformance`. See
[CONTRIBUTING.md](../CONTRIBUTING.md).

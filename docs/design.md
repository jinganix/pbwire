# Design and architecture

How pbwire is put together, what a dynamic message means, and where the
boundaries are. The API walk-through is [guide.md](guide.md), the numbers are in
[BENCHMARKS.md](../BENCHMARKS.md), the verification layers in
[testing.md](testing.md); the landing page is [../README.md](../README.md).

- [The problem it solves](#the-problem-it-solves)
- [How it works](#how-it-works)
- [Dynamic message semantics](#dynamic-message-semantics)
- [Two allocation strategies](#two-allocation-strategies)
- [Scope and boundaries](#scope-and-boundaries)
- [Target use cases](#target-use-cases)
- [Status and limitations](#status-and-limitations)
- [Repository layout](#repository-layout)

## The problem it solves

Traditional Protobuf usage binds the schema at compile time:

```
.proto -> protoc -> generated C++/Java code -> compiled into the app -> encode / decode
```

The schema must be known up front, changing a `.proto` usually means regenerating
code and recompiling, loading a schema dynamically is awkward, and sharing
dynamic message handling across languages is hard.

pbwire moves Protobuf message handling from compile-time code generation to
runtime dynamic processing. Two libraries live in this repository:

| Target | Role | Stack | External dependency |
|---------|------|-------|---------------------|
| `pbwire` | Protobuf runtime | Pure C | none |
| `pbwire-protoc` | `.proto` -> `FileDescriptorSet` | C++17 | Google `libprotobuf` |

They meet at the standard `FileDescriptorSet` and share nothing else — no
header, no symbol, no link edge. `pbwire-protoc` is optional and never built by
`make`, so the core stays buildable with nothing but a C99 compiler. It uses
libprotobuf's own parser (`compiler::Importer`) rather than re-implementing
`.proto` text parsing, and it does not need `libprotoc`.

## How it works

```
.proto ----------------------+
                             |  pbwire-protoc (optional, C++, needs libprotobuf)
FileDescriptorSet <----------+  protoc / buf / other tools
      |
Descriptor runtime           <- parse schema, resolve type relationships
      |
Dynamic message              <- create messages, read/write fields, walk nested structures
      |
Encode (message -> bytes) / Decode (bytes -> message)
```

Descriptors are the foundation of the whole system: reflection, dynamic
messages and dynamic RPC all build on them. Names are stored fully qualified
without the leading dot (`pkg.Outer.Inner`).

`src/` implements that pipeline:

```
src/wire.c           varint / zigzag / tag / little-endian primitives, output buffer
src/schema.c         descriptor registry, name resolution, lookup tables
src/message.c        dynamic message and value tree, ownership, oneof handling
src/arena.c          bump allocator behind pbw_ctx (reset = pointer move)
src/ctx.c            pbw_ctx: reusable arena + encode buffer, decode/encode entry points
src/stream.c         delimited framing reader, encode sink, deterministic map sort
src/encode.c         message tree -> wire bytes
src/decode.c         wire bytes -> message tree, unknown field preservation
src/descriptor_set.c FileDescriptorSet -> schema; the only reader of descriptor.proto
src/json.c           JSON mapping printer + parser, WKT special forms
src/text.c           text format printer + parser
src/internal.h       internal structs and helpers; never included by consumers
```

## Dynamic message semantics

- **Explicit presence for every field.** Nothing is filled with defaults; a
  field absent on the wire is absent in the decoded message
  (`pbw_message_get` returns `NULL`).
- **Numbers are raw bit patterns.** Use the typed accessors
  (`pbw_value_get_int32`, `pbw_value_get_float`, ...); the descriptor type says
  which one applies.
- **Repeated numeric fields** decode from both packed and unpacked encodings;
  the descriptor only controls what the encoder emits. Packing is never
  implied: proto3 defaults to packed, so call `pbw_field_set_packed(field, 1)`
  when the schema says so (a `FileDescriptorSet` loader applies that default).
- **Maps are repeated entry messages;** oneofs are tracked per value; groups
  decode into their submessage like message fields.
- **The dialect decides enum and `required` behavior.** A message descriptor is
  proto2 or proto3 as declared by the file it came from; hand-built schemas
  default to proto3 and `pbw_schema_set_proto3(schema, 0)` switches them. A
  proto2 closed enum's unknown number does not land in the field: it is kept as
  an unknown field, in packed repetition too. A proto2 `required` field is
  checked by `pbw_decode()` (which then fails with `PBW_ERR_REQUIRED`) and by
  `pbw_message_is_initialized()`; `pbw_encode()` writes the tree as it stands,
  matching the reference serializer.
- **Unknown fields are data.** Anything the schema does not declare — including
  a known field carrying the wrong wire type, and legacy groups — is kept as
  raw wire bytes and written back after the known fields, in the order they
  were read. A proxy or gateway can therefore pass through fields it was never
  taught about. The cost is one buffer per message that actually contains
  unknowns.
- **Nesting is capped at 256 levels** in both directions (`PBW_MAX_DEPTH`).
- **Ownership is explicit.** `pbw_message_set()` takes ownership of the value on
  success only (a rejected value stays the caller's), values owned by another
  message are refused so one payload never has two owners, a decoded plain tree
  owns everything reachable from it via its arena, and the schema owns the
  descriptors every message points into. [guide.md](guide.md#ownership-rules)
  has the one-page version.

## Two allocation strategies

The same encoder and decoder, and the same bytes, with two memory and ownership
models. Pick per call site, not per program.

| | `pbwire ctx` (`pbw_ctx` API) | `pbwire plain` (`pbw_encode` / `pbw_decode`) |
|---|---|---|
| Entry points | `pbw_decode_ctx`, `pbw_encode_ctx` | `pbw_decode`, `pbw_encode` |
| Allocation | one bump arena + one output buffer per context; no `malloc` after warm-up | one tree allocation + one output buffer per call |
| Release | `pbw_ctx_reset()` moves a pointer back and recycles the whole tree; `pbw_ctx_free()` releases the context | caller frees: `pbw_message_free(tree)` / `pbw_free(buf)` |
| Lifetime | valid until the next `reset`/`encode`/`free`; a reset invalidates every pointer into the tree | owned by the caller, valid for as long as it is kept |
| Mutability | read-only: `pbw_message_set`/`set_message` fail, `clear` does nothing | mutable: `set`/`clear`/`set_message` work; values and submessages inside the tree still belong to it and are released with it |
| Concurrency | one context per thread/goroutine, never shared | no shared state: each message is independent, threads need no workspace |
| Output buffer | `pbw_encode_ctx` output valid until the next `encode`/`reset`/`free` | `*out` is `malloc`'d and handed to the caller (`pbw_free`) |

Use the context in a hot loop or per server worker; use the plain API for
one-off work, or when the result has to be kept and modified. The difference the
two columns show is exactly one `malloc`/`free` pair per call: it costs ~20% of
the context's throughput at 132-169 B, ~13% at 2755 B, and is gone by 86 KB
where the payload dominates — see the `ctx` vs `plain` columns in
[BENCHMARKS.md](../BENCHMARKS.md).

A context keeps at most `pbw_ctx_set_retain_cap` bytes resident across resets
(1 MiB by default; single allocations above it bypass the reusable chunk and are
freed on reset, so one outlier never pins the context). `0` keeps nothing.
`pbw_pool` is a bounded freelist of contexts for short-lived workers.

## Scope and boundaries

The core library provides: pure C with no external runtime dependencies, no
`protoc` requirement, runtime loading of standard `FileDescriptorSet`, dynamic
messages and reflection, compliance with the standard Protobuf wire format, the
JSON mapping and the text format, and a stable, easy-to-bind C ABI.

Out of scope for the core library:

- `.proto` text parsing (the optional `pbwire-protoc` library does it, and only
  writes bytes: core and importer share no type and no link dependency)
- Code generation
- Networking / RPC / HTTP / gRPC
- Business-level validation frameworks

**Security premise:** descriptors are trusted input. Wire data is untrusted and
validated (bounds-checked, field number 0 and wire types above 5 rejected,
nesting capped). Do not load descriptors from untrusted sources without
validation — dynamic message handling carries resource-consumption and
denial-of-service risk.

## Target use cases

Scenarios, in the shape of the pain they remove:

- **One runtime, many hosts.** A service written in Erlang, Python, Java, Node
  or PHP that has to speak the same Protobuf dialect as its neighbours: one C
  core, one set of bytes, no per-language reimplementation. The per-language
  pain points and what each binding changes are in
  [bindings/README.md](../bindings/README.md#what-each-binding-solves).
- **Protocol gateways and proxies.** Inspect, transform and forward messages for
  a schema that is only partly known — unknown fields pass through untouched.
- **Dynamic protocol tools.** Debug, diff and edit payloads of types you never
  compiled in, using JSON and text format as the inspection surface.
- **Policy and multi-tenancy.** Load a tenant's `FileDescriptorSet` when it
  connects and drop it when it leaves; nothing is compiled or generated.
- **Embedding.** The Erlang NIF is the extreme case: a crash takes the host VM
  down, so every failure path returns a status, everything is bounds-checked,
  and ownership is explicit.
- **Typed ergonomics where wanted.** `bindings/typed` generates typed message
  models per language from the same descriptor; the compiler checks host code,
  the wire format still comes from this core.

## Status and limitations

The runtime core is implemented: schema registry, dynamic messages, wire
encode/decode, reflection, JSON and text format. Encoding is byte-identical to
`protoc 36.2` for every scalar type, packed and unpacked repeated fields and
nested messages; the self-check pins those bytes so parity cannot regress
silently.

`pbw_descriptor_set_load()` accepts a standard `FileDescriptorSet`, so a schema
can come from a `.proto` at build time (`protoc --descriptor_set_out`) or from
`pbwire-protoc` at runtime.

Documented limits, pinned by tests rather than silently ignored: MessageSet
encoding stays byte-identical through the unknown fields instead of populating
the extensions. Proto2 semantics are followed where the reference
implementations agree: a closed enum's unknown number leaves the field and comes
back as an unknown field, and `pbw_decode()` rejects a message whose `required`
field is missing while `pbw_encode()` writes the tree as it stands (the
reference parser refuses, the reference serializer does not check;
`pbw_message_is_initialized()` is the opt-in check).
`pbw_schema_set_proto3()` selects the dialect for hand-built schemas; a
descriptor set chooses it per file.

## Repository layout

```
include/pbwire.h            public C ABI (schemas, descriptors, messages, values, wire)
src/                        the runtime (see How it works above)
include/pbwire-protoc/      importer / descriptor set / diagnostic headers (no protobuf types)
src/protoc/importer.cc      .proto -> FileDescriptorSet, via compiler::Importer
src/protoc/descriptor_set.cc, error.cc

bindings/README.md          binding layout + where each one's schema comes from
bindings/typed/             protoc-gen-pbwire-typed: typed models for every language
bindings/c/                 C binding: runtime/ (struct codec), plugin/, examples/, tests/, benchmarks/, conformance/
bindings/cpp/               C++ binding: examples/, tests/, benchmarks/ over the shared C runtime (no plugin)
bindings/python/            CPython extension: runtime/, examples/, tests/, benchmarks/, conformance/
bindings/go/                Go: cgo dynamic over the C ABI, dynamic/ with hot-swap registry (no plugin)
bindings/erlang/            Erlang/OTP NIF: c_src/ (runtime), examples/, test/, benchmarks/, conformance/
bindings/java/              Java: pure-Java codec, runtime/, examples/, tests/ (no JNI, no native library)
bindings/kotlin/            Kotlin: the Java runtime + a DSL
bindings/ruby/              Ruby: a C extension over the core, ext/, lib/, examples/, test/, benchmarks/, conformance/
bindings/node/              Node: N-API addon over the C ABI, lib/, examples/, test/ (no npm deps)
bindings/objc/              Objective-C: ARC wrapper over the C ABI, examples/, tests/ (Foundation only)
bindings/php/, dart/, rust/, csharp/   FFI/static-link hosts, same pieces

examples/                   one tutorial schema (proto2 address book) + its .desc/.json/.bin data
tests/                      assert-based self-checks (round-trip, golden bytes, limits)
tests/test_descriptor.c     pinned FileDescriptorSet -> schema -> wire
tests/protoc/               importer self-check and its .proto fixtures
bench/                      differential test and benchmark against libprotobuf C++
conformance/testee.c        testee for the official conformance suite
docs/                       this document, guide.md, testing.md, faq.md
BENCHMARKS.md               the performance document: all bindings, one set of tiers
.githooks/, scripts/        git hooks and gate scripts (check / validate-commit-msg / setup-git-hooks)
.github/workflows/ci.yml    CI: make test (Linux + macOS), CMake/ctest, clang-format
CMakeLists.txt, cmake/      two targets, install + pbwire::pbwire / pbwire::protoc
```

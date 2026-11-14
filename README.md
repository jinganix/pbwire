[![CI](https://github.com/jinganix/pbwire/actions/workflows/ci.yml/badge.svg)](https://github.com/jinganix/pbwire/actions/workflows/ci.yml)
[![License](https://img.shields.io/badge/license-Apache--2.0-blue.svg)](LICENSE)
[![Language](https://img.shields.io/badge/C-C99-blue.svg)](#requirements-and-build)
[![Dependencies](https://img.shields.io/badge/runtime%20dependencies-none-brightgreen.svg)](#at-a-glance)

> **Release [`v0.1.16`](https://github.com/jinganix/pbwire/releases/tag/v0.1.16)** — every binding ships this one version; [`VERSION`](VERSION) is the source of truth, `scripts/sync-versions.sh` propagates it.

# pbwire — a dynamic Protobuf runtime for C, no code generation

**pbwire** is a dependency-free Protobuf runtime written in C. It loads a schema
at runtime — a hand-built descriptor tree or a standard `FileDescriptorSet` —
then builds, reads, encodes and decodes messages dynamically through reflection.
No `protoc` step, no generated message code, no libprotobuf in the process. It
speaks proto2, proto3 and edition-2023 descriptor sets, the JSON mapping and the
text format, and drives twenty-three host-language bindings from that one core.

The point is not to re-implement Protobuf encoding. It is to let every language
with a C ABI share one dynamic Protobuf schema and one message-processing core
instead of re-implementing Protobuf per host language.

```c
uint8_t *buf;
size_t len;
pbw_encode(msg, &buf, &len);   /* 0a 03 61 62 63 10 ff ff ff ff ff ff ff ff ff 01 */
pbw_free(buf);
```

## Documentation

| Document | Contents |
|---|---|
| [C API guide](docs/guide.md) | the tutorial, hand-built schema, `FileDescriptorSet` load, `pbw_ctx` loop, ownership rules, streaming, unknown fields, JSON/text, ABI |
| [Design and architecture](docs/design.md) | how it works, dynamic message semantics, the two allocation strategies, scope, limits, use cases, repository layout |
| [Performance benchmarks](BENCHMARKS.md) | **the performance document**: the four standard tiers, every binding against its official runtime, and the cross-binding comparison |
| [Performance attempts ledger](docs/perf-attempts.md) | every optimization tried against a still-below-baseline cell, its A/B number, and why it was kept or rejected |
| [Examples index](docs/examples.md) | one runnable round trip per binding, with the exact command |
| [Testing and conformance](docs/testing.md) | the self-check, pinned reference bytes, `difftest`, the official conformance suite |
| [FAQ](docs/faq.md) | syntax support, thread safety, unknown fields, untrusted input, `.proto` at runtime |
| [Protobuf pain points](docs/protobuf-pain-points.md) | which official-library complaints pbwire solves, eases or leaves alone — the searchable index |
| [Bindings](bindings/README.md) | every host binding, where its schema comes from, and the per-language pain point each one removes |
| [include/pbwire.h](include/pbwire.h) | the whole public API with per-function contracts |
| [AGENTS.md](AGENTS.md) · [CONTRIBUTING.md](CONTRIBUTING.md) · [llms.txt](llms.txt) | working on this repo, commit rules, a one-page factual summary |
| [ROADMAP.md](ROADMAP.md) | what is done, what is never, what is next |

## Why use pbwire

- **Schema at runtime.** Load a `FileDescriptorSet` produced by `protoc`, `buf`
  or `pbwire-protoc`, and handle message types you did not compile in.
- **No runtime dependencies.** C99, one public header, ~13k lines of C (about
  7.5k of it the binary codec, the rest the JSON mapping and text format).
  `make` and `make test` need nothing but a compiler — no libprotobuf, no
  `protoc`, no JSON/C++ runtime underneath.
- **Dynamic messages and reflection.** Create messages from a descriptor,
  get/set fields by number or name, walk nested structures, oneofs and maps.
- **Byte-identical to the reference implementation.** Encodings match
  `protoc 36.2` for every scalar type, packed/unpacked repetition and nested
  messages; pinned bytes in the test suite keep parity from regressing silently.
- **Built for embedding.** Stable C ABI, no globals, caller-confined
  schema/message objects, arena-backed `pbw_ctx` for per-thread hot loops, wire
  input validated at the boundary. [`bindings/embedded`](bindings/embedded/README.md)
  packages that as one `pbwe_box`: one arena (on the heap, or in caller memory
  via `pbwe_open_mem`/`pbw_ctx_new_mem`, which never allocates), caller-owned
  encode buffers (bare or length-prefixed frames), and both inputs (frames and
  the startup descriptor set) capped by `pbw_limits`; `make embedded-bench`
  measures it against nanopb, byte for byte and ns for ns.
- **One core, twenty-three host languages.** [C](bindings/c/), [C++](bindings/cpp/), [Python](bindings/python/), [Ruby](bindings/ruby/), [Java](bindings/java/), [Kotlin](bindings/kotlin/), [Node](bindings/node/), [Objective-C](bindings/objc/), [PHP](bindings/php/), [Dart](bindings/dart/), [C#](bindings/csharp/), [Rust](bindings/rust/), [Go](bindings/go/), [Erlang](bindings/erlang/), [Lua](bindings/lua/), [Swift](bindings/swift/), [Scala](bindings/scala/), [Elixir](bindings/elixir/), [Zig](bindings/zig/), [F#](bindings/fsharp/), [Clojure](bindings/clojure/), [Julia](bindings/julia/) and [R](bindings/r/) all speak the same wire
  format, each with a measured answer to its runtime's own pain point — see
  [what each binding solves](bindings/README.md#what-each-binding-solves).

## At a glance

| | |
|---|---|
| Language | C99 (C++ consumers include the same header) |
| Wire format | Google Protobuf binary, proto2 + proto3 (edition-2023 descriptor sets load too) |
| Schema input | hand-built descriptors, or a standard `FileDescriptorSet` |
| Message handling | dynamic value tree with reflection; no generated code |
| Runtime dependencies | none (optional importer `pbwire-protoc` needs libprotobuf, C++17) |
| Not implemented | `.proto` text parsing in the core (deliberately never: build the set once with `protoc`/`buf`/`pbwire-protoc`); RPC transport and stubs (services load, are queryable, and resolve to descriptors with `pbw_service_resolve` for routing — the transport itself stays official gRPC/Twirp/plain HTTP); no CLI (inspect with `protoc --decode/--encode`) |
| Memory model | one allocation per decoded tree, or one reusable arena `pbw_ctx` per thread |
| Limits | nesting depth 256, field numbers up to 2^29-1 |
| Platforms | Linux, macOS, Windows (MSVC through CMake/ctest); any C99 compiler; 32-bit (ILP32) and big-endian are CI-verified |
| License | Apache-2.0 |

## Requirements and build

A C99 compiler for the core. CMake plus a C++17 compiler and `libprotobuf` only
for `pbwire-protoc`.

```bash
make          # build build/libpbwire.a
make test     # self-checks: core, threads, descriptor set, C structs, examples round trip
make shared   # libpbwire.so.1 / libpbwire.1.dylib, for FFI hosts that dlopen it
make format   # clang-format (>= 19); make format-check
make clean
```

CMake is the packaging path and the only build that knows about
`pbwire-protoc`:

```bash
cmake -B build-cmake                        # core only, no protobuf needed
cmake -B build-cmake -DPBW_BUILD_PROTOC=ON   # core + .proto importer (needs libprotobuf)
cmake --build build-cmake
(cd build-cmake && ctest)                    # same checks as `make test`, plus test_importer

# installed targets: pbwire::pbwire and pbwire::protoc
cmake --install build-cmake --prefix /usr/local

# then, in a consumer:
# find_package(pbwire REQUIRED)
# target_link_libraries(app PRIVATE pbwire::pbwire)
```

Everything else is optional and outside the commit gate: `make difftest`,
`make conformance` and the per-binding `*-test` / `*-bench` targets. See
[docs/testing.md](docs/testing.md) for what they verify and when they are worth
running; binding targets are listed in each binding's README and in
[BENCHMARKS.md](BENCHMARKS.md).

## Quick start

```c
#include "pbwire.h"

pbw_schema *schema = pbw_schema_new();
pbw_descriptor *person = pbw_schema_add_message(schema, "Person");
pbw_message_add_field(person, "name", 1, PBW_TYPE_STRING, PBW_LABEL_OPTIONAL);
pbw_message_add_field(person, "id", 2, PBW_TYPE_INT32, PBW_LABEL_OPTIONAL);
pbw_schema_link(schema);

pbw_message *msg = pbw_message_new(person);
pbw_message_set(msg, 1, pbw_value_new_string("abc"));
pbw_message_set(msg, 2, pbw_value_new_int(-1));

uint8_t *buf;
size_t len;
pbw_encode(msg, &buf, &len);   /* 0a 03 61 62 63 10 ff ff ff ff ff ff ff ff ff 01 */

pbw_message *decoded;
pbw_decode(person, buf, len, &decoded);
const char *name = pbw_value_get_string(pbw_message_get(decoded, 1));

pbw_free(buf);
pbw_message_free(decoded);
pbw_message_free(msg);
pbw_schema_free(schema);
```

Real programs rarely build a schema by hand — they load a `FileDescriptorSet`
at runtime and process types they were never compiled against. The walk-through
for that, the arena-backed hot loop, streaming framing and the JSON/text entry
points is [docs/guide.md](docs/guide.md); a runnable tutorial schema (proto2
address book, loaded from `addressbook.desc` at runtime) is in
[`examples/`](examples/) and runs with `make examples`.

## Performance

`make bench` verifies byte-for-byte equality with libprotobuf before timing
anything, so a speed number can never come from a wrong encoder. On an Apple
M4 Pro with `-O2` and `protoc 36.2`, the in-process dynamic core with a
`pbw_ctx` runs 1.12-1.47x the throughput of the generated C++ codec on encode
and 1.52-1.82x on decode, on every tier
(132 B to 86 KB), and 11-15x the speed of C++
`DynamicMessage` on encode and 3.0-4.6x on decode. Generated code only wins where
a dynamic codec meets generated straight-line writes on a tiny message
(Java/Kotlin/Clojure, 136 B, 0.71x/0.78x/0.75x); the arena API wins every tier, and the plain
API trails only on the 169 B encode. `make bench-docker` runs the
same harnesses in a Linux container and `make bench-compare` pairs the two runs
row by row, so the numbers above hold off Apple silicon as well
([BENCHMARKS.md](BENCHMARKS.md#linux-docker-the-same-harnesses-one-container-away)).

The per-language tables — the same four payload tiers for every binding against
its official runtime (Python, Java, Kotlin, Node, Ruby, Objective-C, PHP,
Dart, Rust, C#, Erlang, Swift, Scala, Elixir, F#, Clojure, Julia), plus the fastest third-party runtime wherever one
meaningfully exists (Node, Rust, C#, PHP), the C struct codec, 1-vs-8-thread
scaling, and the reason behind each gap — live in [BENCHMARKS.md](BENCHMARKS.md).
The one-row summary is here; the detail is there.

| Language | How it talks to the core | Best encode / decode vs official | Baseline |
|---|---|---|---|
| [C](bindings/c/) | static link, struct codec | 1.34x (small) / 2.00x (xlarge) | [protoc C++](https://github.com/protocolbuffers/protobuf) |
| [C++](bindings/cpp/) | same C runtime, same header | 1.34x (small) / 2.00x (xlarge)¹ | [protoc C++](https://github.com/protocolbuffers/protobuf) |
| [Python](bindings/python/) | CPython extension | 64.5x (xlarge) / 1.22x (xlarge) | [google.protobuf (upb)](https://pypi.org/project/protobuf/) |
| [Ruby](bindings/ruby/) | C extension, linked | 1.30x (small) / 1.89x (large) | [google-protobuf gem](https://rubygems.org/gems/google-protobuf) |
| [Java](bindings/java/) | pure-Java codec, no native code | 1.15x (medium) / 1.89x (xlarge) | [protobuf-java](https://github.com/protocolbuffers/protobuf/tree/main/java) |
| [Kotlin](bindings/kotlin/) | Java runtime + DSL | 1.20x (medium) / 1.81x (xlarge) | [protobuf-kotlin](https://github.com/protocolbuffers/protobuf/tree/main/java/kotlin) |
| [Node](bindings/node/) | N-API addon | 3.21x (large) / 2.40x (xlarge) vs protobufjs | [jspb](https://www.npmjs.com/package/google-protobuf) + [protobufjs](https://www.npmjs.com/package/protobufjs) |
| [Objective-C](bindings/objc/) | ARC wrapper | 6.61x (small) / 2.91x (medium) | [protobuf-objc](https://github.com/protocolbuffers/protobuf/tree/main/objectivec) |
| [PHP](bindings/php/) | C extension | 1.59x (small) / 1.88x (small) vs C ext (362x / 140x vs pure PHP) | [protobuf-php](https://github.com/protocolbuffers/protobuf/tree/main/php) |
| [Dart](bindings/dart/) | `dart:ffi` | 15.4x (xlarge) / 4.4x (large) | [protobuf-dart](https://pub.dev/packages/protobuf) |
| [Rust](bindings/rust/) | static link | 1.25x (small) / 1.83x (large) vs prost | [rust-protobuf 3.x](https://github.com/stepancheg/rust-protobuf) + [prost](https://github.com/tokio-rs/prost) |
| [C#](bindings/csharp/) | P/Invoke | 3.34x (medium) / 2.04x (medium) | [Google.Protobuf](https://www.nuget.org/packages/Google.Protobuf) + [protobuf-net](https://github.com/protobuf-net/protobuf-net) |
| [Go](bindings/go/) | cgo dynamic (hot-swappable registry) | 2.45x (xlarge) / 2.28x (large) | [google.golang.org/protobuf](https://pkg.go.dev/google.golang.org/protobuf) |
| [Erlang](bindings/erlang/) | NIF | 3.91x (xlarge) / 2.47x (medium) | [gpb](https://github.com/tomas-abrahamsson/gpb) |
| [Lua](bindings/lua/) | C module, tables + builder | 7.65x (xlarge, builder) / 1.73x (small, tables) | [lua-protobuf](https://github.com/starwing/lua-protobuf) |
| [Swift](bindings/swift/) | SwiftPM, static link | 4.96x (large) / 4.28x (medium) | [swift-protobuf](https://github.com/apple/swift-protobuf) |
| [Scala](bindings/scala/) | Java runtime + DSL | 1.44x (medium) / 1.25x (xlarge) | [ScalaPB](https://github.com/scalapb/ScalaPB) |
| [Elixir](bindings/elixir/) | NIF (shared core) | 2.92x (medium) / 1.82x (large) | [elixir-protobuf](https://github.com/elixir-protobuf/protobuf) |
| [Zig](bindings/zig/) | static link | 1.81x (xlarge) / 47.2x (small) | [zig-protobuf](https://github.com/Arwalk/zig-protobuf) |
| [F#](bindings/fsharp/) | C# runtime + facade | 3.29x (medium) / 2.04x (medium) | [Google.Protobuf](https://www.nuget.org/packages/Google.Protobuf) |
| [Clojure](bindings/clojure/) | Java runtime + interop | 1.51x (large) / 1.77x (xlarge) | [protobuf-java](https://github.com/protocolbuffers/protobuf/tree/main/java) |
| [Julia](bindings/julia/) | `dlopen` + `ccall` | 135.6x (xlarge) / 1.22x (medium) | [ProtoBuf.jl](https://github.com/JuliaIO/ProtoBuf.jl) |
| [R](bindings/r/) | C extension, named lists | 61.5x (xlarge) / 66.6x (xlarge) | [RProtoBuf](https://github.com/eddelbuettel/rprotobuf) |

¹ C++ has no separate bench; same engine as the C struct codec. Best columns take the best ratio across the four tiers for encode/decode separately, tier in parentheses (Apple M4 Pro, `protoc 36.2`); tiers and absolute MB/s are in [BENCHMARKS.md](BENCHMARKS.md).

## Comparison with generated code, `DynamicMessage` and upb

| | Schema at runtime | Generated code | Runtime dependency | Dynamic messages + reflection |
|---|---|---|---|---|
| **pbwire** | yes | none | none (C99) | yes |
| `protoc` generated C++ | no | yes | libprotobuf | via `DynamicMessage`, slower |
| `DynamicMessage` (C++) | yes | no | libprotobuf | yes |
| upb | yes | optional | bundled C runtime | yes |
| nanopb | no | yes | none (embedded C) | no |

Pick pbwire when the schema is only known at runtime, when one C core serves
several host languages, or when libprotobuf must not be in the process. Pick
generated code only when the schema never changes *and* you accept a codegen
step: even then generated straight-line writes win only the smallest encode
cells against dynamic dispatch — the Java, Kotlin and Clojure bindings at 136 B
(0.71x, 0.78x and 0.75x) — while the in-process `pbw_ctx` wins every tier of the
core benchmark and the C struct codec wins every tier of its own.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md). Enable hooks once per clone:

```bash
./scripts/setup-git-hooks.sh
```

## License

[Apache-2.0](LICENSE).

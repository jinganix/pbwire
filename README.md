[![CI](https://github.com/jinganix/pbwire/actions/workflows/ci.yml/badge.svg)](https://github.com/jinganix/pbwire/actions/workflows/ci.yml)
[![License](https://img.shields.io/badge/license-Apache--2.0-blue.svg)](LICENSE)
[![Language](https://img.shields.io/badge/C-C99-blue.svg)](#requirements-and-build)
[![Dependencies](https://img.shields.io/badge/runtime%20dependencies-none-brightgreen.svg)](#at-a-glance)

# pbwire — a dynamic Protobuf runtime for C, no code generation

**pbwire** is a dependency-free Protobuf runtime written in C. It loads a schema
at runtime — a hand-built descriptor tree or a standard `FileDescriptorSet` —
then builds, reads, encodes and decodes messages dynamically through reflection.
No `protoc` step, no generated message code, no libprotobuf in the process. It
speaks proto2, proto3 and edition-2023 descriptor sets, the JSON mapping and the
text format, and drives fourteen host-language bindings from that one core.

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
| [Testing and conformance](docs/testing.md) | the self-check, pinned reference bytes, `difftest`, the official conformance suite |
| [FAQ](docs/faq.md) | syntax support, thread safety, unknown fields, untrusted input, `.proto` at runtime |
| [Bindings](bindings/README.md) | every host binding, where its schema comes from, and the per-language pain point each one removes |
| [include/pbwire.h](include/pbwire.h) | the whole public API with per-function contracts |
| [AGENTS.md](AGENTS.md) · [CONTRIBUTING.md](CONTRIBUTING.md) · [llms.txt](llms.txt) | working on this repo, commit rules, a one-page factual summary |

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
  input validated at the boundary.
- **One core, fourteen host languages.** [C](bindings/c/), [C++](bindings/cpp/), [Python](bindings/python/), [Ruby](bindings/ruby/), [Java](bindings/java/), [Kotlin](bindings/kotlin/), [Node](bindings/node/), [Objective-C](bindings/objc/), [PHP](bindings/php/), [Dart](bindings/dart/), [C#](bindings/csharp/), [Rust](bindings/rust/), [Go](bindings/go/) and [Erlang](bindings/erlang/) all speak the same wire
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
| Not implemented | RPC, MessageSet decoding into extensions |
| Memory model | one allocation per decoded tree, or one reusable arena `pbw_ctx` per thread |
| Limits | nesting depth 256, field numbers up to 2^29-1 |
| Platforms | Linux, macOS, anywhere with a C99 compiler |
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
`pbw_ctx` runs 1.1-1.7x the throughput of the generated C++ codec on every tier
(132 B to 86 KB, encode and decode), and 8-15x the speed of C++
`DynamicMessage` on encode and 2-4x on decode. Generated code only wins where
the comparison includes an FFI boundary (Java, 136 B, 0.69x) or where pbwire's
*plain* API pays one `malloc`/`free` pair per call (0.97x at 2755 B); the arena
API wins every tier.

The per-language tables — the same four payload tiers for every binding against
its official runtime (Python, Java, Kotlin, Node, Ruby, Objective-C, PHP,
Dart, Rust, C#, Erlang), plus the fastest third-party runtime wherever one
meaningfully exists (Node, Rust, C#, PHP), the C struct codec, 1-vs-8-thread
scaling, and the reason behind each gap — live in [BENCHMARKS.md](BENCHMARKS.md).
The one-row summary is here; the detail is there.

| Language | How it talks to the core | Best encode / decode vs official | Baseline |
|---|---|---|---|
| [C](bindings/c/) | static link, struct codec | 1.42x (xlarge) / 2.25x (xlarge) | [protoc C++](https://github.com/protocolbuffers/protobuf) |
| [C++](bindings/cpp/) | same C runtime, same header | 1.42x (xlarge) / 2.25x (xlarge)¹ | [protoc C++](https://github.com/protocolbuffers/protobuf) |
| [Python](bindings/python/) | CPython extension | 6.5x (xlarge) / 1.69x (xlarge) | [google.protobuf (upb)](https://pypi.org/project/protobuf/) |
| [Ruby](bindings/ruby/) | C extension, linked | 1.42x (small) / 1.93x (large) | [google-protobuf gem](https://rubygems.org/gems/google-protobuf) |
| [Java](bindings/java/) | FFI `java.lang.foreign` | 1.59x (large) / 1.22x (small) | [protobuf-java](https://github.com/protocolbuffers/protobuf/tree/main/java) |
| [Kotlin](bindings/kotlin/) | Java FFM layer + DSL | 1.60x (xlarge) / 1.20x (medium) | [protobuf-kotlin](https://github.com/protocolbuffers/protobuf/tree/main/java/kotlin) |
| [Node](bindings/node/) | N-API addon | 22.3x (xlarge) / 20.0x (xlarge) | [jspb](https://www.npmjs.com/package/google-protobuf) + protobufjs |
| [Objective-C](bindings/objc/) | ARC wrapper | 6.6x (medium) / 2.9x (large) | [protobuf-objc](https://github.com/protocolbuffers/protobuf/tree/main/objectivec) |
| [PHP](bindings/php/) | `ext-ffi` | 0.99x (xlarge) / 0.99x (xlarge) vs C ext (337x / 103x vs pure PHP) | [protobuf-php](https://github.com/protocolbuffers/protobuf/tree/main/php) |
| [Dart](bindings/dart/) | `dart:ffi` | 12.2x (xlarge) / 3.3x (xlarge) | [protobuf-dart](https://pub.dev/packages/protobuf) |
| [Rust](bindings/rust/) | static link | 1.57x (xlarge) / 1.45x (xlarge) | [rust-protobuf 3.x](https://github.com/stepancheg/rust-protobuf) + prost |
| [C#](bindings/csharp/) | P/Invoke | 3.4x (medium) / 2.0x (medium) | [Google.Protobuf](https://www.nuget.org/packages/Google.Protobuf) + protobuf-net |
| [Go](bindings/go/) | cgo dynamic (hot-swappable registry) | — (rebench pending) | [google.golang.org/protobuf](https://pkg.go.dev/google.golang.org/protobuf) |
| [Erlang](bindings/erlang/) | NIF | 2.35x (medium) / 2.33x (medium) | [gpb](https://github.com/tomas-abrahamsson/gpb) |

¹ C++ has no separate bench; same engine as the C struct codec. Best columns take the highest tier each for encode/decode separately (Apple M4 Pro, `protoc 36.2`); tiers and absolute MB/s are in [BENCHMARKS.md](BENCHMARKS.md).

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
step: even then it wins just two cells of the benchmark set — the C struct codec
at 136 B (0.98x) and the Java binding at 136 B (0.69x, where the FFI downcall
dominates) — while the in-process `pbw_ctx` wins every tier of the core
benchmark.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md). Enable hooks once per clone:

```bash
./scripts/setup-git-hooks.sh
```

## License

[Apache-2.0](LICENSE).

[![CI](https://github.com/jinganix/pbwire/actions/workflows/ci.yml/badge.svg)](https://github.com/jinganix/pbwire/actions/workflows/ci.yml)
[![License](https://img.shields.io/badge/license-Apache--2.0-blue.svg)](LICENSE)
[![Language](https://img.shields.io/badge/C-C99-blue.svg)](#requirements-and-build)
[![Dependencies](https://img.shields.io/badge/runtime%20dependencies-none-brightgreen.svg)](#at-a-glance)

# pbwire — a dynamic Protobuf runtime for C, no code generation

**pbwire** is a dependency-free Protobuf runtime written in C. It loads a schema at runtime — a
hand-built descriptor tree or a standard `FileDescriptorSet` — then builds, reads, encodes and
decodes messages dynamically through reflection. No `protoc` step, no generated message code, no
libprotobuf in the process.

The point is not to re-implement Protobuf encoding. It is to let every language with a C ABI share
one dynamic Protobuf schema and one message-processing core instead of re-implementing Protobuf per
host language.

```c
uint8_t *buf;
size_t len;
pbw_encode(msg, &buf, &len);   /* 0a 03 61 62 63 10 ff ff ff ff ff ff ff ff ff 01 */
pbw_free(buf);
```

## Contents

- [Why use pbwire](#why-use-pbwire)
- [At a glance](#at-a-glance)
- [The problem it solves](#the-problem-it-solves)
- [How it works](#how-it-works)
- [Requirements and build](#requirements-and-build)
- [Quick start](#quick-start)
- [Dynamic message semantics](#dynamic-message-semantics)
- [Scope and boundaries](#scope-and-boundaries)
- [Target use cases](#target-use-cases)
- [Repository layout](#repository-layout)
- [Status and limitations](#status-and-limitations)
- [Testing against the official implementation](#testing-against-the-official-implementation)
- [Performance](#performance)
- [Comparison with alternatives](#comparison-with-alternatives)
- [Using pbwire with AI coding assistants](#using-pbwire-with-ai-coding-assistants)
- [FAQ](#faq)
- [Contributing](#contributing)
- [License](#license)

## Why use pbwire

- **Schema at runtime.** Load a `FileDescriptorSet` produced by `protoc`, `buf` or
  `pbwire-protoc`, and handle message types you did not compile in.
- **No runtime dependencies.** C99, one public header, about 3.4k lines of C. `make` and `make test`
  need nothing but a compiler — no libprotobuf, no `protoc`, no JSON/C++ runtime underneath.
- **Dynamic messages and reflection.** Create messages from a descriptor, get/set fields by number or
  name, walk nested structures, oneofs and maps.
- **Byte-identical to the reference implementation.** Encodings match `protoc 36.2` for every scalar
  type, packed/unpacked repetition and nested messages; pinned bytes in the test suite keep parity
  from regressing silently.
- **Built for embedding.** Stable C ABI, no globals, caller-confined schema/message objects,
  arena-backed `pbw_ctx` for per-thread hot loops, wire input validated at the boundary.

## At a glance

| | |
|---|---|
| Language | C99 (C++ consumers include the same header) |
| Wire format | Google Protobuf binary, proto2 + proto3 (edition-2023 descriptor sets load too) |
| Schema input | hand-built descriptors, or a standard `FileDescriptorSet` |
| Message handling | dynamic value tree with reflection; no generated code |
| Runtime dependencies | none (optional importer `pbwire-protoc` needs libprotobuf, C++17) |
| Not implemented | JSON / text format mapping, RPC, groups as fields, extension resolution |
| Memory model | per-message ownership by default, or one arena `pbw_ctx` per thread |
| Limits | nesting depth 100, field numbers up to 2^29-1 |
| Platforms | Linux, macOS, anywhere with a C99 compiler |
| License | Apache-2.0 |

## The problem it solves

Traditional Protobuf usage binds the schema at compile time:

```
.proto -> protoc -> generated C++/Java/Go code -> compiled into the app -> encode / decode
```

The schema must be known up front, changing a `.proto` usually means regenerating code and
recompiling, loading a schema dynamically is awkward, and sharing dynamic message handling across
languages is hard.

pbwire moves Protobuf message handling from compile-time code generation to runtime dynamic
processing. Two libraries live in this repository:

| Target | Role | Stack | External dependency |
|---------|------|-------|---------------------|
| `pbwire` | Protobuf runtime | Pure C | none |
| `pbwire-protoc` | `.proto` -> `FileDescriptorSet` | C++17 | Google `libprotobuf` |

They meet at the standard `FileDescriptorSet` and share nothing else — no header, no symbol, no link
edge. `pbwire-protoc` is optional and never built by `make`, so the core stays buildable with
nothing but a C99 compiler. It uses libprotobuf's own parser (`compiler::Importer`) rather than
re-implementing `.proto` text parsing, and it does not need `libprotoc`.

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

Descriptors are the foundation of the whole system: reflection, dynamic messages and dynamic RPC
all build on them. Names are stored fully qualified without the leading dot (`pkg.Outer.Inner`).

## Requirements and build

A C99 compiler for the core. CMake plus a C++17 compiler and `libprotobuf` only for
`pbwire-protoc`.

```bash
make          # build build/libpbwire.a
make test     # build and run the self-checks (core + threads + descriptor set)
make format   # clang-format (>= 19) over include/, src/, tests/, bench/
make format-check
make clean
```

CMake is the packaging path and the only build that knows about `pbwire-protoc`:

```bash
cmake -B build-cmake                        # core only, no protobuf needed
cmake -B build-cmake -DPBW_BUILD_PROTOC=ON   # core + .proto importer (needs libprotobuf)
cmake --build build-cmake
(cd build-cmake && ctest)                    # same checks as `make test`, plus test_importer

# installed targets: pbwire::pbwire and pbwire::protoc
cmake --install build-cmake --prefix /usr/local
```

Optional, needs `protoc` + libprotobuf C++ (`brew install protobuf`). These are never part of the
commit gate:

```bash
make difftest     # differential test against the official C++ runtime
make bench        # encode/decode benchmark vs generated code and DynamicMessage
make conformance  # official binary conformance suite (needs a protobuf v36.2 source checkout)
```

## Quick start

### Build a schema by hand

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

### Load a schema at runtime

This is the feature the library exists for: the message types do not have to be known when the
program is compiled. Any process that can read bytes can pick up a schema later — after a config
reload, a tenant connect, or a plugin install — and immediately read, write and re-emit messages of
that type.

```c
uint8_t *set;
size_t set_len;
read_file("schema.pb", &set, &set_len);     /* protoc --include_imports --descriptor_set_out */

pbw_schema *schema;
if (pbw_descriptor_set_load(set, set_len, &schema) != PBW_OK) {
    log("%s", pbw_schema_error(schema));    /* *out is set even on failure */
    pbw_schema_free(schema);
    return;
}
/* names are fully qualified without the leading dot */
const pbw_descriptor *person = pbw_schema_find(schema, "example.Person");
```

- **Three producers, one format.** `protoc --include_imports --descriptor_set_out=schema.pb` at
  build time, `buf build -o schema.pb` (imports included), or `pbwire-protoc` at runtime. pbwire only
  ever reads the resulting bytes.
- **Ship every imported file.** Types are resolved by name inside the set, so a field typed
  `other.Thing` needs `other.Thing` in the same set: pass `--include_imports` (`buf build` already
  does) or the load fails with `unknown type ... for field ...` in `pbw_schema_error()`.
- **What the loader reads:** names, field numbers, types, labels, oneofs, the `packed` option and the
  file syntax (proto2, proto3, or an edition). Services, extensions, reserved ranges and source code
  info are ignored.
- **Dialect defaults are applied:** proto3 and edition-2023 sets get packed repetition by default,
  proto2 sets keep explicit `packed`. Presence is explicit either way — nothing is defaulted in.
- **Failure is not fatal and not silent:** `*out` is always written, so free it on both paths; the
  reason lands in `pbw_schema_error()`.
- **The schema outlives its messages.** Messages, values and context trees keep pointers into the
  schema's descriptors; free the schema last, and treat a linked schema as read-only while other
  threads use it.

`pbwire-protoc` turns `.proto` text into exactly those bytes at runtime
(`#include "pbwire-protoc/importer.h"`):

```cpp
pbwp::Options options;
options.import_paths = {"./proto", "./third_party"};

pbwp::Result result = pbwp::Importer(options).import({"user.proto", "order.proto"});
if (result.ok()) {
    result.value().save("schema.pb");            /* or hand data()/size() to pbw_descriptor_set_load */
} else {
    for (const pbwp::Diagnostic &d : result.diagnostics()) log("%s", d.ToString().c_str());
}
```

### Reuse one arena in a loop

Hand the library a workspace instead of relying on per-message ownership:

```c
pbw_ctx *ctx = pbw_ctx_new();          /* one per thread / goroutine */

for (;;) {
    pbw_message *msg;
    const uint8_t *out;
    size_t len;

    pbw_decode_ctx(ctx, person, in, in_len, &msg);   /* no allocation per field */
    /* ... read msg ... */
    pbw_encode_ctx(ctx, msg, &out, &len);            /* buffer reused */
    send(out, len);
    pbw_ctx_reset(ctx);                              /* everything freed at once */
}
pbw_ctx_free(ctx);
```

Messages decoded through a context belong to it: they are read-only, `pbw_message_free` on them is a
no-op, and `pbw_ctx_reset` invalidates them. The plain functions keep the per-message ownership model
and share all the code below; they just allocate one `pbw_ctx` per call.

## Dynamic message semantics

- **Explicit presence for every field.** Nothing is filled with defaults; a field absent on the wire
  is absent in the decoded message (`pbw_message_get` returns `NULL`).
- **Numbers are raw bit patterns.** Use the typed accessors (`pbw_value_get_int32`,
  `pbw_value_get_float`, ...); the descriptor type says which one applies.
- **Repeated numeric fields** decode from both packed and unpacked encodings; the descriptor only
  controls what the encoder emits. Packing is never implied: proto3 defaults to packed, so call
  `pbw_field_set_packed(field, 1)` when the schema says so (a `FileDescriptorSet` loader applies
  that default).
- **Maps are repeated entry messages;** oneofs are tracked per value; groups are not supported as
  fields (legacy groups are skipped when decoding).
- **Unknown fields are data.** Anything the schema does not declare — including a known field
  carrying the wrong wire type, and legacy groups — is kept as raw wire bytes and written back after
  the known fields, in the order they were read. A proxy or gateway can therefore pass through
  fields it was never taught about. The cost is one buffer per message that actually contains
  unknowns.
- **Nesting is capped at 100 levels** in both directions.

## Scope and boundaries

The core library provides: pure C with no external runtime dependencies, no `protoc` requirement,
runtime loading of standard `FileDescriptorSet`, dynamic messages and reflection, compliance with the
standard Protobuf wire format, and a stable, easy-to-bind C ABI.

Out of scope for the core library:

- JSON / text-format mapping
- `.proto` text parsing (the optional `pbwire-protoc` library does it, and only writes bytes:
  core and importer share no type and no link dependency)
- Code generation
- Networking / RPC / HTTP / gRPC
- Business-level validation frameworks

**Security premise:** descriptors are trusted input. Wire data is untrusted and validated
(bounds-checked, field number 0 and wire types above 5 rejected, nesting capped). Do not load
descriptors from untrusted sources without validation — dynamic message handling carries
resource-consumption and denial-of-service risk.

## Target use cases

- **Erlang NIF**: handle Protobuf messages dynamically without generating Erlang code per message
- **Python**: load schemas at runtime, read and write messages dynamically
- **C / C++**: operate on dynamic messages without generated business message classes
- **Go / Rust**: reuse one C runtime through FFI
- **Protocol gateways**: transform, inspect and forward Protobuf messages driven by a dynamic schema
- **Dynamic protocol tools**: debug, inspect and modify Protobuf payloads of unknown types

## Repository layout

```
include/pbwire.h            public C ABI (schemas, descriptors, messages, values, wire)
src/wire.c                  varint / zigzag / tag / little-endian primitives, output buffer
src/schema.c                descriptor registry, name resolution, lookup tables
src/message.c               dynamic message and value tree, oneof handling
src/encode.c                message tree -> wire bytes
src/decode.c                wire bytes -> message tree, unknown field preservation
src/descriptor_set.c        FileDescriptorSet -> schema (the only reader of descriptor.proto)
src/ctx.c                   pbw_ctx: arena + encode buffer reuse

include/pbwire-protoc/      importer / descriptor set / diagnostic headers (no protobuf types)
src/protoc/importer.cc      .proto -> FileDescriptorSet, via compiler::Importer
src/protoc/descriptor_set.cc, error.cc

tests/                      assert-based self-checks (round-trip, golden bytes, limits)
tests/test_descriptor.c     pinned FileDescriptorSet -> schema -> wire
tests/protoc/               importer self-check and its .proto fixtures
bench/                      differential test and benchmark against libprotobuf C++
conformance/testee.c        testee for the official conformance suite (binary wire format)
.githooks/                  git hooks (pre-commit / commit-msg / pre-push)
scripts/                    gate scripts (check / validate-commit-msg / setup-git-hooks)
.github/workflows/ci.yml    CI: make test (Linux + macOS), CMake/ctest, clang-format
CMakeLists.txt, cmake/      two targets, install + pbwire::pbwire / pbwire::protoc
```

## Status and limitations

The runtime core is implemented: schema registry, dynamic messages, wire encode/decode, reflection.
Encoding is byte-identical to `protoc 36.2` for every scalar type, packed and unpacked repeated
fields and nested messages; the self-check pins those bytes so parity cannot regress silently.

`pbw_descriptor_set_load()` accepts a standard `FileDescriptorSet`, so a schema can come from a
`.proto` at build time (`protoc --descriptor_set_out`) or from `pbwire-protoc` at runtime.

Documented limits, pinned by tests rather than silently ignored: no JSON mapping, groups are skipped
rather than represented, `required` fields are not enforced, and extension fields are not resolved
(they round-trip as unknown fields).

## Testing against the official implementation

Four layers, in order of how much they can catch:

1. **Self-check** (`make test`, three binaries). Dependency-free, assert-based. `test_pbwire` covers
   every scalar type at its edges (varint width boundaries, zigzag/fixed/NaN/Inf/-0 bit patterns,
   open enums, any-non-zero `bool`), field numbers up to 2^29-1 and the tag widths in between,
   packed and unpacked repetition of all 14 packable types, recursive message types, maps, oneof,
   duplicate occurrences (scalars last-one-wins, singular messages *merge*), legacy groups, unknown
   fields, truncation and single-byte mutation sweeps, payloads up to 70 KB, schema validation, the
   workspace API, and the length-patch boundaries (127/128, 16383/16384). `test_descriptor` loads a
   pinned `protoc` descriptor set and checks the metadata it produces (packed defaults per syntax,
   oneofs, maps, nested and cross-file types, required fields) plus the bytes encoded through it.
   `test_threads` runs 8 threads that decode and encode per-thread contexts, resets and recreates
   arenas, and verify that no other thread's payload ever shows up in their memory; it is also run
   under ThreadSanitizer during development. This is what the commit gate runs.
2. **Pinned reference bytes.** `tests/test_pbwire.c` carries the exact bytes `protoc 36.2` produces
   for a 16-field message, so cross-implementation parity cannot regress without a `protoc` on the
   machine.
3. **Differential test** (`make difftest`). Parses random and mutated byte streams with both this
   library and the official C++ runtime (generated code plus `DynamicMessage`) and requires the same
   verdict: if one accepts, the other must accept. Disagreements are shrunk to a minimal reproducer
   before being reported. The context path is checked the same way: bytes encoded through a
   `pbw_ctx` must be identical to the reference's. Both syntaxes take part: `bench/msg.proto`
   (proto3) for implicit presence and packed-by-default, `bench/msg2.proto` (proto2) for explicit
   presence — where byte-for-byte parity holds even for fields set to their default value —
   unpacked repetition, groups (skipped by pbwire, pinned as a documented limit) and required fields
   (not enforced, also pinned). The same run loads `bench/gen/msg.pb` (produced by protoc) through
   `pbw_descriptor_set_load()` and compares every field with libprotobuf's own view of that
   `FileDescriptorSet`, then encodes through the loaded schema and requires identical bytes: the
   descriptor loader is verified against the reference, not against itself.
4. **Official conformance suite** (`make conformance`). `conformance/testee.c` is a testee for
   Google's own `conformance_test_runner`: it speaks the length-delimited request/response protocol,
   loads the test messages from a `FileDescriptorSet` built at run time (that is what
   `pbw_descriptor_set_load()` is for — the conformance messages have hundreds of fields, so no
   hand-built mirror is possible), and round-trips every request through `pbw_decode`/`pbw_encode`.
   It answers only the binary wire format; JSON and text-format requests are reported as *skipped*,
   since pbwire has no such layer.

   ```
   # needs a protobuf v36.2 checkout with the runner built (see the Makefile comment)
   PBWIRE_PROTOBUF_SRC=$PWD/protobuf make conformance
   CONFORMANCE SUITE PASSED: 1391 successes, 1403 skipped, 0 unexpected failures
   ```

   The suite runs against `TestAllTypesProto3`, `TestAllTypesProto2` (groups, required fields,
   MessageSet encodings, extensions) and the two edition-2023 variants. Every binary test passes;
   the 1403 skipped ones are the JSON/text cases. This is the run that found the tag-truncation bug
   (`BadTag_FieldNumberSlightlyTooHigh`) and the one that motivated keeping unknown fields instead of
   dropping them: with them dropped, 7 tests fail (unknown varints, unknown ordering, MessageSet).

   Like `difftest` it is outside `make test`: it needs a Protobuf source tree and a built runner,
   which are not repository dependencies.

The differential test is what found the two real bugs in this library so far: tags were truncated to
32 bits *after* shifting (reference implementations truncate first, so tags wider than 32 bits
disagreed), and a skipped legacy group accepted field number 0 inside it. 1.2 million random and
mutated inputs across three seeds now agree on every verdict.

## Performance

`make bench` encodes and decodes the same message with three implementations and prints ns/op and
throughput. It verifies byte-for-byte equality first, so a speed number can never come from a wrong
encoder. Apple M4 Pro, `-O2`, `protoc 36.2`, 500k iterations:

| payload | op | pbwire (ctx) | pbwire (plain) | protoc generated | DynamicMessage |
|---|---|---|---|---|---|
| 132 B | encode | 92 ns | 145 ns | 77 ns | 1177 ns |
| 132 B | decode | 181 ns | 406 ns | 155 ns | 1113 ns |
| 169 B | encode | 124 ns | 184 ns | 101 ns | 1558 ns |
| 169 B | decode | 252 ns | 627 ns | 221 ns | 1475 ns |
| 2755 B | encode | 2.32 us | 2.79 us | 2.44 us | 21.2 us |
| 2755 B | decode | 6.20 us | 14.8 us | 6.35 us | 21.8 us |

Two columns for pbwire, because allocation strategy is the whole story: the plain API allocates one
context per call (per-message ownership), the `pbw_ctx` API reuses one. Same encoder, same decoder,
same bytes.

Read it as:

- **Against generated code**: slightly faster on large payloads (encode 0.95x, decode 0.98x) and
  within 1.2x on a 132-byte message. Generated code wins the small case because the schema is
  compiled into straight-line C++ with constant field offsets; pbwire still walks a descriptor and a
  value tree. That is the price of a runtime schema, and it is a small one.
- **Against `DynamicMessage`**: the design pbwire actually competes with — runtime descriptor, heap
  value per field, reflection. pbwire is 3.5-13x faster there, and 1.2-2.5x faster than its own
  plain-API self.

How the gap was closed, since `make bench` is the reason the numbers are trustworthy:

1. Decoding allocates from a bump arena per context instead of one `malloc` per field value, and a
   `pbw_ctx_reset` is a pointer move.
2. Encoding computes no size pass: it writes into a growable buffer, puts a one-byte placeholder
   where a length goes and patches it afterwards (shifting the payload when the length grows) --
   the same trick the original Erlang NIF codec used.
3. Varints are never written twice either: the buffer reserves the maximum ten bytes and a single
   loop emits the value, so nothing is measured before it is written.
4. Hot helpers (`varint_size`, the field-number lookup, the arena bump) are inlined, and the common
   "singular scalar, expected wire type, first occurrence" case skips the general decode path.

## Comparison with alternatives

| | Schema at runtime | Generated code | Runtime dependency | Dynamic messages + reflection |
|---|---|---|---|---|
| **pbwire** | yes | none | none (C99) | yes |
| `protoc` generated C++ | no | yes | libprotobuf | via `DynamicMessage`, slower |
| `DynamicMessage` (C++) | yes | no | libprotobuf | yes |
| upb | yes | optional | bundled C runtime | yes |
| nanopb | no | yes | none (embedded C) | no |

Pick pbwire when the schema is only known at runtime, when one C core serves several host languages,
or when libprotobuf must not be in the process. Pick generated code when payloads are tiny and the
schema never changes — it is still ~1.2x faster on a 132-byte message and needs no descriptor walk.

## Using pbwire with AI coding assistants

- [AGENTS.md](AGENTS.md): commands, layout and the hard invariants an agent must not break (error
  handling, ownership, bounded recursion, decoupling of core and importer).
- [llms.txt](llms.txt): a one-page factual summary for retrieval-style assistants.

## FAQ

### Is pbwire a Protobuf code generator?

No. It is a runtime: descriptors in, dynamic messages out. Nothing is generated, nothing is compiled
per schema, and adding a message type does not require a rebuild.

### Does it need `protoc` at runtime?

No. `protoc` (or `buf`, or the optional `pbwire-protoc` library) produces a `FileDescriptorSet` once;
pbwire reads those bytes at runtime.

### Which Protobuf syntaxes are supported?

proto2 and proto3, including edition-2023 descriptor sets (treated as proto3 semantics). Legacy
groups decode as skipped/unknown data rather than as message fields; `required` is not enforced.

### Does pbwire support JSON or text format?

No. Only the binary wire format. JSON mapping is out of scope for the core library.

### Can I use it from Python, Erlang, Go or Rust?

Yes — that is the intended use. The C ABI is the product; language bindings live outside this
repository.

### Is it thread-safe?

There is no global state. A schema or message must be confined to one thread by the caller, and one
`pbw_ctx` per thread keeps arena reuse local. No locks, no thread-local buffers.

### What happens to fields the schema does not declare?

They are preserved as raw wire bytes and re-emitted after the known fields, in input order. A gateway
can forward messages for a schema it only partially knows. Unknown fields are why 7 conformance
tests pass that would fail otherwise.

### Why is pbwire slower than generated code on small messages?

Generated code compiles field offsets into straight-line C++; pbwire walks a descriptor and a value
tree at runtime. On a 132-byte message that costs roughly 40 ns per encode. On large payloads the
descriptor walk is amortized and pbwire is at parity or slightly ahead.

### Can I decode untrusted bytes with it?

Yes: lengths are bounds-checked, field number 0 and wire types above 5 are rejected, nesting is
capped at 100, and every failure returns a `pbw_status` instead of aborting. Descriptors, by
contrast, are trusted input — see [Scope and boundaries](#scope-and-boundaries).

### How do I load a `.proto` file at runtime?

Convert it to a `FileDescriptorSet` once (`protoc --include_imports --descriptor_set_out`,
`buf build -o`, or the optional `pbwire-protoc` library), then call `pbw_descriptor_set_load()` and
`pbw_schema_link()` is already done for you. All imported files must be in the set.

### Do I have to recompile when a `.proto` changes?

No. Ship the new descriptor set and reload the schema in the running process; nothing is generated or
compiled per schema. Messages of the old schema stay valid until you free that schema.

### Where do I report a bug?

A minimal reproduction: the bytes (or `.proto`), the expected behavior, and whether it came from
`make difftest` / `make conformance`. See [CONTRIBUTING.md](CONTRIBUTING.md).

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md). Enable hooks once per clone:

```bash
./scripts/setup-git-hooks.sh
```

## License

[Apache-2.0](LICENSE).

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
- **No runtime dependencies.** C99, one public header, about 10k lines of C. `make` and `make test`
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
| Not implemented | RPC, MessageSet decoding into extensions |
| Memory model | one allocation per decoded tree, or one reusable arena `pbw_ctx` per thread |
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
make conformance-setup  # once: shallow-clone pinned protobuf + build runner (cached in ~/.cache/pbwire)
make conformance  # official binary/text/JSON conformance suite (reuses the cached checkout)
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
- **What the loader reads:** names, field numbers, types, labels, oneofs, the `packed` option, proto2
  defaults and the file syntax (proto2, proto3, or an edition), plus extensions with their extendee.
  Services, reserved ranges and source code info are ignored.
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
no-op, and `pbw_ctx_reset` invalidates them. The plain functions share all the code below and give
every decoded tree its own arena, so a decode is still one `malloc`; `pbw_message_free` releases the
whole tree and everything reachable from it.

### Stream, bound and sort

Length-delimited framing (`varint length + message`) is the standard way to put messages on a
stream. Feed bytes as they arrive; each `next` call decodes one message, or reports nothing yet:

```c
pbw_istream *s = pbw_istream_new(NULL);   /* optional pbw_limits */

pbw_istream_feed(s, chunk, chunk_len);     /* append, any split */
for (;;) {
    pbw_message *msg = NULL;
    pbw_istream_next(s, person, &msg);     /* NULL + PBW_OK = need more data */
    if (msg == NULL)
        break;
    /* ... read msg (owned, pbw_message_free) ... */
    pbw_message_free(msg);
}
```

`pbw_encode_delimited` writes one frame; `pbw_encode_sink(msg, sink, opaque)` hands the bytes to a
caller callback in 8 KiB chunks instead of a `malloc`'d buffer. Untrusted input takes explicit
bounds (`0` means the default):

```c
pbw_limits limits = {.max_bytes = 1 << 20, .max_depth = 32};

pbw_decode_limits(person, buf, len, &limits, &msg);      /* PBW_ERR_LIMIT past them */
pbw_decode_ctx_limits(ctx, person, buf, len, &limits, &msg);
```

`pbw_encode_deterministic` sorts map entries by key so equal messages encode to equal bytes;
everything else encodes exactly as `pbw_encode` writes it.

## Dynamic message semantics

- **Explicit presence for every field.** Nothing is filled with defaults; a field absent on the wire
  is absent in the decoded message (`pbw_message_get` returns `NULL`).
- **Numbers are raw bit patterns.** Use the typed accessors (`pbw_value_get_int32`,
  `pbw_value_get_float`, ...); the descriptor type says which one applies.
- **Repeated numeric fields** decode from both packed and unpacked encodings; the descriptor only
  controls what the encoder emits. Packing is never implied: proto3 defaults to packed, so call
  `pbw_field_set_packed(field, 1)` when the schema says so (a `FileDescriptorSet` loader applies
  that default).
- **Maps are repeated entry messages;** oneofs are tracked per value; groups decode into their
  submessage like message fields.
- **The dialect decides enum and `required` behavior.** A message descriptor is proto2 or proto3 as
  declared by the file it came from; hand-built schemas default to proto3 and
  `pbw_schema_set_proto3(schema, 0)` switches them. A proto2 closed enum's unknown number does not
  land in the field: it is kept as an unknown field, in packed repetition too. A proto2 `required`
  field is checked by `pbw_decode()` (which then fails with `PBW_ERR_REQUIRED`) and by
  `pbw_message_is_initialized()`; `pbw_encode()` writes the tree as it stands, matching the
  reference serializer.
- **Unknown fields are data.** Anything the schema does not declare — including a known field
  carrying the wrong wire type, and legacy groups — is kept as raw wire bytes and written back after
  the known fields, in the order they were read. A proxy or gateway can therefore pass through
  fields it was never taught about. The cost is one buffer per message that actually contains
  unknowns.
- **Nesting is capped at 100 levels** in both directions.

## Scope and boundaries

The core library provides: pure C with no external runtime dependencies, no `protoc` requirement,
runtime loading of standard `FileDescriptorSet`, dynamic messages and reflection, compliance with the
standard Protobuf wire format, the JSON mapping and the text format, and a stable, easy-to-bind C ABI.

Out of scope for the core library:

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
src/stream.c                delimited framing reader, encode sink, deterministic encode
src/ctx.c                   pbw_ctx: arena + encode buffer reuse
bindings/python/            cffi binding (ABI mode, stdlib-free smoke cover)
bindings/go/pbwire/        cgo binding with a round-trip test

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

Documented limits, pinned by tests rather than silently ignored: MessageSet
encoding stays byte-identical through the unknown fields instead of populating the extensions.
Proto2 semantics are followed where the reference implementations agree: a closed enum's unknown
number leaves the field and comes back as an unknown field, and `pbw_decode()` rejects a message
whose `required` field is missing while `pbw_encode()` writes the tree as it stands (the reference
parser refuses, the reference serializer does not check; `pbw_message_is_initialized()` is the
opt-in check). `pbw_schema_set_proto3()` selects the dialect for hand-built schemas; a descriptor
set chooses it per file.

## Testing against the official implementation

Five layers, in order of how much they can catch:

1. **Self-check** (`make test`, three binaries). Dependency-free, assert-based. `test_pbwire` covers
   every scalar type at its edges (varint width boundaries, zigzag/fixed/NaN/Inf/-0 bit patterns,
   open enums, any-non-zero `bool`), field numbers up to 2^29-1 and the tag widths in between,
   packed and unpacked repetition of all 14 packable types, recursive message types, maps, oneof,
   duplicate occurrences (scalars last-one-wins, singular messages *merge*), legacy groups, unknown
   fields, truncation and single-byte mutation sweeps, payloads up to 70 KB, schema validation, the
   workspace API, the text format (printing goldens, parse goldens, separator/escape/range/nesting
   failures, unknown-field show/hide), the JSON mapping (print goldens, accept/reject spellings,
   well-known-type forms, extensions, maps, strict base64 and number rules), and the length-patch
   boundaries (127/128, 16383/16384).
   `test_descriptor` loads a pinned `protoc` descriptor set and checks the metadata it produces
   (packed defaults per syntax, oneofs, maps, nested and cross-file types, required fields,
   extension full names, reserved names, Any) plus the bytes encoded through it and text and JSON
   parsing through it (extensions, reserved skipping, Any round trip, unknown-field policy,
   required).
   `test_threads` runs 8 threads through five shapes that must all stay independent: per-thread
   contexts (resets and recreated arenas, verifying that no other thread's payload ever shows up in
   their memory), the plain API, reflection over one shared linked schema, descriptor-set loading
   (each thread loads and frees its own schema from the same bytes, plus the truncated-set error
   path), and decode/encode against one shared loaded schema. It also prints a 1-thread vs 8-thread
   throughput comparison and fails if the aggregate collapses, which a hidden lock would cause; the
   suite is run under ThreadSanitizer and ASan/UBSan during development. This is what the commit gate
   runs.
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
   unpacked repetition, groups (decoded into their submessage), extensions, closed enums (an
   unknown number moves to the unknown fields, singular and packed) and `required` fields (the parser
   refuses, the serializer writes). The same run loads `bench/gen/msg.pb` (produced by protoc) through
   `pbw_descriptor_set_load()` and compares every field with libprotobuf's own view of that
   `FileDescriptorSet`, then encodes through the loaded schema and requires identical bytes: the
   descriptor loader is verified against the reference, not against itself. The same run checks the
   text format both ways (C++-printed text parses to the same message, pbw-printed text parses the
   same under C++, unknown fields print byte-identically) and fuzzes text verdicts: 60,000 mutated
   texts must be accepted/rejected exactly like the reference parser. The same run checks the JSON
   mapping both ways (C++-printed JSON parses to the same message, pbw-printed JSON parses the
   same under C++) and fuzzes JSON verdicts: 40,000 mutated JSON texts must be accepted/rejected
   exactly like the reference parser.
4. **Official conformance suite** (`make conformance`). `conformance/testee.c` is a testee for
   Google's own `conformance_test_runner`: it speaks the length-delimited request/response protocol,
   loads the test messages from a `FileDescriptorSet` built at run time (that is what
   `pbw_descriptor_set_load()` is for — the conformance messages have hundreds of fields, so no
   hand-built mirror is possible), and round-trips every request through `pbw_decode`/`pbw_encode`
   (binary), `pbw_json_parse`/`pbw_json_print` (JSON) or `pbw_text_parse`/`pbw_text_print` (text).
   It answers the binary wire format, the JSON mapping and the text format.

   ```
   make conformance-setup   # once: shallow-clone protobuf v36.2 + build the runner
   make conformance         # reuses the cached checkout, rebuilds incrementally
   CONFORMANCE SUITE PASSED: 2745 successes, 0 skipped, 2 expected failures, 0 unexpected failures.
   CONFORMANCE SUITE PASSED: 445 successes, 0 skipped, 0 expected failures, 0 unexpected failures.
   ```

   The first line is the binary + JSON suite, where every test passes — including four JSON tests
   the reference C++ testee itself fails (`SingleValueForRepeatedFieldInt32/Message`, proto2 and
   proto3: C++ still accepts a bare value for a repeated field, pbwire rejects it per spec);
   the second is the text
   suite, where every test passes — including four the reference C++ testee itself fails
   (bad-UTF-8 strings and out-of-range `\\U` escapes, which pbwire rejects and C++ accepts).

   The two failures are `Required.Proto2.ProtobufInput.BadTag_FieldNumberSlightlyTooHigh` and the
   proto3 variant, and they are not pbwire's: the official C++ testee (`conformance_cpp`) built from
   the same checkout fails them with the same response — both truncate a 5-byte tag varint whose
   value exceeds `UINT32_MAX` to `field = 1, wire type = 0` instead of rejecting it, which is what
   `CodedInputStream::ReadVarint32FromArray` does. Its run reports
   `2765 successes, 0 skipped, 0 expected failures, 6 unexpected failures`: the same two binary
   tests plus four JSON tests that pbwire skips. So the binary verdicts agree with the reference
   test by test, including the two the reference itself does not satisfy.

   The suite runs against `TestAllTypesProto3`, `TestAllTypesProto2` (groups, required fields,
   MessageSet encodings, extensions) and the two edition-2023 variants. Every other binary and JSON
   test passes. This is the run that found the
   tag-truncation bug (`BadTag_FieldNumberSlightlyTooHigh`) and the one that motivated keeping
   unknown fields instead of dropping them: with them dropped, 7 tests fail (unknown varints,
   unknown ordering, MessageSet).

   Like `difftest` it is outside `make test`: it needs a Protobuf source tree and a built runner,
   which are not repository dependencies.

The differential test is what found the two real bugs in this library so far: tags were truncated to
32 bits *after* shifting (reference implementations truncate first, so tags wider than 32 bits
disagreed), and a skipped legacy group accepted field number 0 inside it. 1.2 million random and
mutated inputs across three seeds now agree on every verdict.

## Performance

`make bench` encodes and decodes the same message with three implementations and prints ns/op and
throughput. It verifies byte-for-byte equality first, so a speed number can never come from a wrong
encoder. Apple M4 Pro, `-O2`, `protoc 36.2`, 500k iterations, throughput in MB/s (higher is better):

| payload | op | pbwire ctx (MB/s) | pbwire plain (MB/s) | protoc gen (MB/s) | DynamicMessage (MB/s) |
|---|---|---|---|---|---|
| 132 B | encode | 1611.0 | 1233.2 | 1588.0 | 109.6 |
| 132 B | decode | 1058.1 | 803.5 | 874.4 | 119.3 |
| 169 B | encode | 1595.8 | 1175.1 | 1663.5 | 108.7 |
| 169 B | decode | 939.0 | 744.5 | 766.1 | 116.2 |
| 2755 B | encode | 1428.8 | 1375.5 | 1109.8 | 131.5 |
| 2755 B | decode | 583.6 | 559.8 | 431.4 | 126.2 |

Throughput moves by a few percent between runs; the numbers above are one run.

`make bench` also runs the 169-byte payload with 1 vs 8 threads (one context and one message per
thread, the same read-only wire input) and prints MB/s per thread, aggregate MB/s, the per-operation
degradation and the aggregate speedup. Same machine (14 cores), from the same run:

| op | pbwire 1thr (MB/s) | pbwire 8thr agg (MB/s) | gen 1thr (MB/s) | gen 8thr agg (MB/s) |
|---|---|---|---|---|
| encode | 1665.2 | 11574.9 | 1761.6 | 9113.3 |
| decode | 963.8 | 7135.2 | 815.9 | 6436.6 |

`8thr agg` is the total throughput of all eight threads. The schema, the descriptors and the wire
input are shared by all threads; each thread only adds its own `pbw_ctx` and message. pbwire scales
6.9x (encode) and 7.4x (decode) from 1 to 8 threads in that run; generated code reaches 5.2x and
7.9x. Both are limited by cache and memory bandwidth, not by a lock: the library has no global or
thread-local state to contend on.

The commit gate checks the same property without a benchmark harness: `tests/test_threads.c` prints
1-thread vs 8-thread MB/s for the context API (its own, smaller message, one run on the same
machine):

```
scale ctx decode 1 thread: 3157.0 MB/s ( 61.4 ns/op) | 8 threads: 2544.8 MB/s/thread, 20358.3 MB/s aggregate (per-op 1.24x, aggregate 6.45x, 14 cores)
scale ctx encode 1 thread: 5901.1 MB/s ( 32.9 ns/op) | 8 threads: 4127.8 MB/s/thread, 33022.2 MB/s aggregate (per-op 1.43x, aggregate 5.60x, 14 cores)
```

It fails when the aggregate throughput collapses below 0.35x of `min(8, cores)`, which is what a
hidden lock or false sharing would cause.

`make bench` also prints a `stream` table for the same payload: deterministic encode ties plain
encode (no maps in that message, so it is one branch), the sink costs one pass over the bytes, and
a delimited feed/next round-trip costs about one encode plus one decode. Ratios only — absolute
numbers move between machines.

### `pbwire ctx` vs `pbwire plain`

The two pbwire columns are the same encoder and decoder with a different memory and ownership
strategy; the bytes they produce are identical (the benchmark checks that before timing anything).

| | `pbwire ctx` (`pbw_ctx` API) | `pbwire plain` (`pbw_encode` / `pbw_decode`) |
|---|---|---|
| Entry points | `pbw_decode_ctx`, `pbw_encode_ctx` | `pbw_decode`, `pbw_encode` |
| Allocation | one bump arena + one output buffer per context; no `malloc` after warm-up | one tree allocation + one output buffer per call |
| Release | `pbw_ctx_reset()` moves a pointer back and recycles the whole tree; `pbw_ctx_free()` releases the context | caller frees: `pbw_message_free(tree)` / `pbw_free(buf)` |
| Lifetime | valid until the next `reset`/`encode`/`free`; a reset invalidates every pointer into the tree | owned by the caller, valid for as long as it is kept |
| Mutability | read-only: `pbw_message_set`/`set_message` fail, `clear` does nothing | mutable: `set`/`clear`/`set_message` work; values and submessages inside the tree still belong to it and are released with it |
| Concurrency | one context per thread/goroutine, never shared | no shared state: each message is independent, threads need no workspace |
| Output buffer | `pbw_encode_ctx` output valid until the next `encode`/`reset`/`free` | `*out` is `malloc`'d and handed to the caller (`pbw_free`) |

Use the context in a hot loop or per server worker; use the plain API for one-off work, or when the
result has to be kept and modified. The difference the two columns show is exactly that `malloc`/
`free` pair per call: it dominates at 132-169 B (the plain API is at 0.7-0.9x of the context
throughput) and disappears as payloads grow (both are equal at 2755 B).

Read it as:

- **Against generated code**: decoding a runtime schema is 1.21x-1.35x faster than generated C++ on
  every payload tested, and the large encode is 1.29x. On the small encodes the context API is at
  parity (1.01x, 0.96x): generated code still wins a hair because the schema is compiled into
  straight-line C++ with constant field offsets.
- **Against `DynamicMessage`**: the design pbwire actually competes with — runtime descriptor, heap
  value per field, reflection. pbwire is 9-14x faster there.
- **The plain column** pays exactly one `malloc`/`free` pair per call (decoded tree, output buffer),
  which is why it trails generated code on small payloads (1233 vs 1588 MB/s on the 132-byte encode)
  while the context API is at parity or ahead; both pbwire columns win at 2755 B (1429/1376 vs
  1110 MB/s encode, 584/560 vs 431 MB/s decode). See the comparison above for when to use which.

How the gap was closed, since `make bench` is the reason the numbers are trustworthy:

1. A decoded message is one allocation: struct, value slots, presence bitmap and oneof cases share
   one block, and values are stored inline (a bitmap says which slots hold a value) instead of one
   `malloc` per field. The tree's arena struct, its first chunk and the tree itself are one block
   too, so plain `pbw_decode` is a single `malloc`.
2. The `pbw_ctx` API reuses those arenas and the output buffer: `pbw_ctx_reset` is a pointer move.
3. Encoding computes no size pass: it writes into a growable buffer, puts a one-byte placeholder
   where a nested length goes and patches it afterwards (strings and bytes write their known length
   up front). Per-field tag bytes and the packed variant are precomputed at `pbw_schema_link()` and
   stored with one 32-bit store.
4. Varint and tag readers/writers are inlined with one- and two-byte fast paths and an unrolled
   ten-byte path for long (negative int32/int64) values; the common "singular scalar, expected wire
   type, first occurrence" case writes the value slot directly.
5. `pbw_schema_link()` also builds a dense field-number table of field pointers, so decoding a field
   is one indexed load, and packed repeated payloads are read in a tight loop.

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
schema never changes — it still wins the 132-byte encode by a few percent (1595.8 vs 1663.5 MB/s at
169 B) and needs no descriptor walk.

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

proto2 and proto3, including edition-2023 descriptor sets (treated as proto3 semantics). Groups
decode into their submessage like message fields, including `required` checks inside them.
Extensions attach to their extendee (`pbw_field_is_extension()` tells them apart; MessageSet
members keep their wire bytes as unknown fields). A proto2 closed enum's unknown
number is kept as an unknown field, and `pbw_decode` refuses a message missing a `required` field.

### Does pbwire support JSON or text format?

Text format yes (`pbw_text_print` / `pbw_text_parse`, byte-identical printer output to the
reference, strict parser parity including extensions, groups, `Any` and unknown fields).
JSON yes (`pbw_json_print` / `pbw_json_parse`, same verdicts as the reference parser on 40,000
mutated inputs, all well-known types, extensions as `[full.name]`, strict base64 and number rules).

### Can I use it from Python, Erlang, Go or Rust?

Yes — that is the intended use. `bindings/` holds a minimal cffi cover (Python, ABI mode, no
compiler needed) and a minimal cgo package (Go), each with a round-trip smoke test. They pin
`PBW_ABI_VERSION` at load time, so a library built from a newer, incompatible tree fails fast
instead of corrupting memory. The C ABI is the product; richer host bindings live on top of it.

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
capped at 100, and every failure returns a `pbw_status` instead of aborting. `pbw_limits` lowers
the caps per call (`max_bytes`, `max_depth`) for stricter hosts. Descriptors, by
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

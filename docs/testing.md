# Testing and conformance

Fifteen layers verify the wire format, the JSON mapping and the text format
against Google's own implementations. They differ in what they need installed
and how much they can catch; the last few cover environments a laptop cannot
provide (docker, a 32-bit or big-endian host, every binding's toolchain).

| layer | command | needs | part of the commit gate |
|---|---|---|---|
| Self-check | `make test` | a C99 compiler | yes (`scripts/check.sh`) |
| Pinned reference bytes | `make test` | a C99 compiler | yes |
| Sanitizers + fuzz corpus | `make fuzz` under ASan/UBSan | clang/gcc | yes (CI `sanitizers` + `fuzz-short` jobs) |
| Differential test (short) | `make difftest-short` | `protoc` + libprotobuf C++ | yes (CI, fixed seed) |
| Differential test (full) | `make difftest` | `protoc` + libprotobuf C++ | nightly |
| Official conformance suite | `make conformance` | cached protobuf checkout | yes (CI, ubuntu) + nightly |
| Core coverage floor (floor lives in the Makefile) | `make coverage` | gcov | yes (CI) |
| Linux bench node (docker) | `make bench-docker` | docker | no (nightly `bench-baseline` records core + C structs) |
| Binding tests | `make <lang>-test` (plus `make erlang-test`, `make test-cpp`, `make go-test`) | that language's toolchain | no (nightly `bindings` job) |
| Extension sanitizers | CI `binding-asan` job | Python/Ruby/Node + `libasan` | yes (CI) |
| Thread sanitizer | CI `tsan` job (`test_threads`) | `libtsan` | yes (CI) |
| 32-bit (ILP32) | `make port-test-32` | an x86_64 host + `gcc-multilib` | yes (CI `ports` job) |
| Big-endian | `./scripts/check-ports.sh` | `gcc-s390x-linux-gnu` + `qemu-user-static` | yes (CI `ports` job) |
| Generated models | `make codegen-test` (`codegen-test-ci-linux` / `-macos` in CI) | every binding's toolchain | nightly (`codegen-linux` + `codegen-apple`) |
| API reference | `make api-docs` | doxygen | yes (CI `api-docs` job, uploaded as an artifact) |

## 1. Self-check (`make test`)

`make test` builds and runs the assert-based binaries plus the runnable
tutorials (address-book, gateway, embedded). Dependency-free.

`test_pbwire` covers every scalar type at its edges (varint width boundaries,
zigzag/fixed/NaN/Inf/-0 bit patterns, open enums, any-non-zero `bool`), field
numbers up to 2^29-1 and the tag widths in between, packed and unpacked
repetition of all 14 packable types, recursive message types, maps, oneof,
duplicate occurrences (scalars last-one-wins, singular messages *merge*), legacy
groups, unknown fields, truncation and single-byte mutation sweeps, payloads up
to 70 KB, schema validation, the workspace API, the text format (printing
goldens, parse goldens, separator/escape/range/nesting failures,
unknown-field show/hide), the JSON mapping (print goldens, accept/reject
spellings, well-known-type forms, extensions, maps, strict base64 and number
rules), and the length-patch boundaries (127/128, 16383/16384).

`test_descriptor` loads a pinned `protoc` descriptor set and checks the metadata
it produces (packed defaults per syntax, oneofs, maps, nested and cross-file
types, required fields, extension full names, reserved names, Any) plus the
bytes encoded through it and text and JSON parsing through it (extensions,
reserved skipping, Any round trip, unknown-field policy, required).
Hand-built descriptor sets beside it pin services (`shop.Orders`:
`pbw_schema_service_*`), editions FeatureSets (`tests/data/editions_feat.proto`:
packed override, `LEGACY_REQUIRED`, closed/open enums, delimited messages) and
MessageSets (`tests/data/messageset.proto`: items decode into extensions and
encode back byte-identically, unknown type ids stay unknown).

`test_struct` pins the C binding: plugin-generated structs encode to the exact
bytes `protoc --encode` produces (a proto3 and a proto2 matrix covering every
scalar kind the struct engine supports, plus string/enum/message, repetition,
packed forms, defaults and the addressbook golden), decode recovers every
field, and layout mismatches, missing required fields, truncated input and bad
arguments all fail loudly.

`test_threads` runs 8 threads through five shapes that must all stay
independent: per-thread contexts (resets and recreated arenas, verifying that no
other thread's payload ever shows up in their memory), the plain API,
reflection over one shared linked schema, descriptor-set loading (each thread
loads and frees its own schema from the same bytes, plus the truncated-set error
path), and decode/encode against one shared loaded schema. It also prints a
1-thread vs 8-thread throughput comparison and fails if the aggregate collapses,
which a hidden lock would cause; the suite is run under ThreadSanitizer and
ASan/UBSan during development. This is what the commit gate runs.

`test_embedded` pins the embedded facade (and `test_embedded_nofail` runs the
same source against the core built with counting allocation hooks): the pinned
`matrix3` decode, byte-equal re-encode into a caller buffer, the
length-delimited framing both ways (prefix, `*consumed` through two frames in
one buffer, a truncated frame, a frame past the byte cap), both lookup paths,
a build-then-encode round trip in the box arena, a reset that empties the arena,
the short-buffer, byte-cap and nesting-cap, unknown-type and malformed-input
error paths with `*msg`/`*len` cleared, a refused descriptor load and a
descriptor set past its startup cap, plus a thousand-round loop that must
allocate nothing once the arena is warm — and must allocate again under a
one-byte retain cap.

`make test` then builds and runs the C address-book example
(`bindings/c/examples/addressbook.c`), so the tutorial program stays working
too; `make examples` additionally builds the C++ one.

## 2. Pinned reference bytes

`tests/test_pbwire.c` carries the exact bytes `protoc 36.2` produces for a
16-field message, so cross-implementation parity cannot regress without a
`protoc` on the machine.

## 3. Differential test (`make difftest`)

Parses random and mutated byte streams with both this library and the official
C++ runtime (generated code plus `DynamicMessage`) and requires the same
verdict: if one accepts, the other must accept. Disagreements are shrunk to a
minimal reproducer before being reported. The context path is checked the same
way: bytes encoded through a `pbw_ctx` must be identical to the reference's.

Both syntaxes take part: `bench/msg.proto` (proto3) for implicit presence and
packed-by-default, `bench/msg2.proto` (proto2) for explicit presence — where
byte-for-byte parity holds even for fields set to their default value —
unpacked repetition, groups (decoded into their submessage), extensions, closed
enums (an unknown number moves to the unknown fields, singular and packed) and
`required` fields (the parser refuses, the serializer writes).

The same run loads `bench/gen/msg.pb` (produced by protoc) through
`pbw_descriptor_set_load()` and compares every field with libprotobuf's own view
of that `FileDescriptorSet`, then encodes through the loaded schema and requires
identical bytes: the descriptor loader is verified against the reference, not
against itself. It also checks the text format both ways (C++-printed text
parses to the same message, pbw-printed text parses the same under C++, unknown
fields print byte-identically) and fuzzes text verdicts: 60,000 mutated texts
must be accepted/rejected exactly like the reference parser. The JSON mapping is
checked the same way (C++-printed JSON parses to the same message, pbw-printed
JSON parses the same under C++) with 40,000 mutated JSON texts.

```bash
make difftest                           # default case count and seed
./build/bench difftest <cases> <seed>   # longer fuzz run
```

## 4. Official conformance suite (`make conformance`)

`conformance/testee.c` is a testee for Google's own `conformance_test_runner`:
it speaks the length-delimited request/response protocol, loads the test
messages from a `FileDescriptorSet` built at run time (that is what
`pbw_descriptor_set_load()` is for — the conformance messages have hundreds of
fields, so no hand-built mirror is possible), and round-trips every request
through `pbw_decode`/`pbw_encode` (binary), `pbw_json_parse`/`pbw_json_print`
(JSON) or `pbw_text_parse`/`pbw_text_print` (text). It answers the binary wire
format, the JSON mapping and the text format.

```bash
make conformance-setup   # once: shallow-clone protobuf v36.2 + build the runner (.tmp/, git-ignored)
make conformance         # reuses the cached checkout, rebuilds incrementally
PBWIRE_PROTOBUF_SRC=/path/to/protobuf make conformance   # use an existing checkout
```

```
CONFORMANCE SUITE PASSED: 2745 successes, 0 skipped, 2 expected failures, 0 unexpected failures.
CONFORMANCE SUITE PASSED: 443 successes, 0 skipped, 0 expected failures, 0 unexpected failures.
```

The first line is the binary + JSON suite, where every test passes — including
four JSON tests the reference C++ testee itself fails
(`SingleValueForRepeatedFieldInt32/Message`, proto2 and proto3: C++ still accepts
a bare value for a repeated field, pbwire rejects it per spec); the second is
the text suite, where every required test passes. Two recommended text cases
(`StringLiteralShortUnicodeEscapeSurrogatePair{String,Bytes}`: pbwire accepts a
`\ud8xx\udcxx` pair and decodes it to the character it stands for, the strict
suite wants the escapes rejected) are reported as warnings, not failures.

The two failures are `Required.Proto2.ProtobufInput.BadTag_FieldNumberSlightlyTooHigh`
and the proto3 variant, and they are not pbwire's: the official C++ testee
(`conformance_cpp`) built from the same checkout fails them with the same
response — both truncate a 5-byte tag varint whose value exceeds `UINT32_MAX` to
`field = 1, wire type = 0` instead of rejecting it, which is what
`CodedInputStream::ReadVarint32FromArray` does. Its run reports
`2765 successes, 0 skipped, 0 expected failures, 6 unexpected failures`: the same
two binary tests plus the four JSON tests pbwire passes and the reference does
not. So the verdicts agree with the reference test by test, including the two
the reference itself does not satisfy.

The suite runs against `TestAllTypesProto3`, `TestAllTypesProto2` (groups,
required fields, MessageSet encodings, extensions) and the two edition-2023
variants. Only `conformance/failure_list_pbwire.txt` may declare expected
failures (the two tag-truncation cases above), so any new failure is a
regression to fix, not a line to add; compare against `conformance_cpp` before
acting on one.

This is the run that found the tag-truncation bug
(`BadTag_FieldNumberSlightlyTooHigh`) and the one that motivated keeping unknown
fields instead of dropping them: with them dropped, 7 tests fail (unknown
varints, unknown ordering, MessageSet).

Like `difftest` it is outside `make test`: it needs a Protobuf source tree and a
built runner, which are not repository dependencies.

## 5. Bindings

Each binding carries its own self-check and, where the official runtime is
available, the same four benchmark tiers — see
[bindings/README.md](../bindings/README.md) for the per-language commands and
[BENCHMARKS.md](../BENCHMARKS.md) for the numbers.

```bash
make python-test ruby-test node-test objc-test java-test kotlin-test php-test dart-test rust-test csharp-test \
     lua-test swift-test scala-test elixir-test zig-test fsharp-test clojure-test julia-test r-test
make erlang-test                 # EUnit + NIF C tests
make go-test                     # cgo dynamic binding (parity + hot-swap)
make test-cpp                    # C++ binding self-check
make codegen-test                  # the generated model test of every binding
make conformance-python          # official suite through the Python binding
make conformance-ruby            # official binary suite through the Ruby binding
make conformance-java            # official binary suite through the Java binding
make conformance-lua conformance-r conformance-node  # same suite through those bindings
```

## 6. Environments a laptop cannot cover

Four PR jobs and two nightly jobs exist for shapes that need another machine:

| job | what it runs | what it catches |
|---|---|---|
| `binding-asan` | the Python, Ruby and Node extensions built with `-fsanitize=address`, under their own suites | heap bugs in the embedding boundary, where a crash takes the host process down. Python keeps leak detection on (its interpreter is clean) and caught a `Schema`/`MessageType` reference cycle; Ruby and Node leak on their own, so those two run ASan-only, and Ruby's run found a use-after-free reading `pbw_schema_error()` after the schema was freed. |
| `tsan` | `test_threads` under ThreadSanitizer | a data race in the shared-schema / per-thread-context model |
| `ports` (i386) | `make port-test-32`, the whole gate with `-m32` | ILP32 size/alignment assumptions, and x87 float comparison traps in the tests |
| `ports` (s390x) | `scripts/check-ports.sh` under `qemu-user` | byte-order assumptions. This job found three real big-endian bugs: tag words written with a 32-bit store, floats read from the wrong half of the 64-bit bit pattern (JSON and text), and fixed-width struct members copied through a `uint64_t`. |
| `codegen-linux` / `codegen-apple` | every binding's generated-model test (the Apple pair on macOS) | a generated emitter that drifts from the binding's codec on a platform whose toolchain PR CI does not install |
| `api-docs` | `make api-docs` (Doxygen over `include/`) | an undocumented or misparsed public header |

The 32-bit and s390x jobs are the reason `scripts/check-ports.sh` exists: it
compiles the dependency-free suites with a cross compiler and runs them under
`qemu-user`, so the same script works for any target
(`CROSS=... RUN=... ./scripts/check-ports.sh`).

## What the differential test found

Two real bugs in this library so far: tags were truncated to 32 bits *after*
shifting (reference implementations truncate first, so tags wider than 32 bits
disagreed), and a skipped legacy group accepted field number 0 inside it. 1.2
million random and mutated inputs across three seeds now agree on every verdict.

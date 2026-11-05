# Testing and conformance

Five layers verify the wire format, the JSON mapping and the text format
against Google's own implementations. They differ in what they need installed
and how much they can catch.

| layer | command | needs | part of the commit gate |
|---|---|---|---|
| Self-check | `make test` | a C99 compiler | yes (`scripts/check.sh`) |
| Pinned reference bytes | `make test` | a C99 compiler | yes |
| Differential test | `make difftest` | `protoc` + libprotobuf C++ | no |
| Official conformance suite | `make conformance` | cached protobuf checkout | yes (CI, ubuntu) |
| Binding tests | `make <lang>-test` (plus `make erlang-test`, `make test-cpp`, `make go-test`) | that language's toolchain | no |

## 1. Self-check (`make test`)

`make test` builds and runs five assert-based binaries plus the C address-book
example. Dependency-free.

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
CONFORMANCE SUITE PASSED: 445 successes, 0 skipped, 0 expected failures, 0 unexpected failures.
```

The first line is the binary + JSON suite, where every test passes — including
four JSON tests the reference C++ testee itself fails
(`SingleValueForRepeatedFieldInt32/Message`, proto2 and proto3: C++ still accepts
a bare value for a repeated field, pbwire rejects it per spec); the second is
the text suite, where every test passes — including four the reference C++
testee itself fails (bad-UTF-8 strings and out-of-range `\U` escapes, which
pbwire rejects and C++ accepts).

The two failures are `Required.Proto2.ProtobufInput.BadTag_FieldNumberSlightlyTooHigh`
and the proto3 variant, and they are not pbwire's: the official C++ testee
(`conformance_cpp`) built from the same checkout fails them with the same
response — both truncate a 5-byte tag varint whose value exceeds `UINT32_MAX` to
`field = 1, wire type = 0` instead of rejecting it, which is what
`CodedInputStream::ReadVarint32FromArray` does. Its run reports
`2765 successes, 0 skipped, 0 expected failures, 6 unexpected failures`: the same
two binary tests plus four JSON tests that pbwire skips. So the binary verdicts
agree with the reference test by test, including the two the reference itself
does not satisfy.

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

## What the differential test found

Two real bugs in this library so far: tags were truncated to 32 bits *after*
shifting (reference implementations truncate first, so tags wider than 32 bits
disagreed), and a skipped legacy group accepted field number 0 inside it. 1.2
million random and mutated inputs across three seeds now agree on every verdict.

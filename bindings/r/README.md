# pbwire R binding

An R extension (`runtime/src/pbwire_r.c`, loaded with `dyn.load`) that links the
pbwire C core statically. No CRAN dependency, no `protoc` plugin, no
generated code: a `FileDescriptorSet` (`protoc --descriptor_set_out`) is loaded
at runtime and drives every field lookup, so the field numbers, types and
presence rules are the descriptor's, not a hand-written copy.

Messages are named lists, which is what R code already passes around:

```r
dyn.load("pbwire.so")
source("runtime/R/pbwire.R")
schema <- pbwire_load("tests/data/sample.desc")

wire <- pbwire_encode(schema, "pbwire.lua.Sample",
                      list(i32 = -7L, name = "alice",
                           inner = list(s = "nested"), nums = list(1L, 2L, 3L)))
back <- pbwire_decode(schema, "pbwire.lua.Sample", wire)

back$name       # "alice"
back$inner$s    # "nested"
identical(pbwire_encode(schema, "pbwire.lua.Sample", back), wire)  # TRUE

# Hot path: build once, encode per send.
m <- pbwire_message(schema, "pbwire.lua.Sample")
pbwire_message_set(m, list(i32 = -7L, name = "alice"))
w1 <- pbwire_message_encode(m)  # byte-identical to pbwire_encode of the list
```

## Build and run

```shell
./build.sh            # builds ../../build/libpbwire.a, then pbwire.so (needs R)
Rscript tests/test_pbwire.R
Rscript examples/roundtrip.R
```

The test prints one `ok`/`FAIL` line per check and exits non-zero on a failure;
`tests/data/sample.desc` is checked in, so no `protoc` is needed at test time.
`runtime/` is also an installable R package skeleton (`DESCRIPTION`,
`NAMESPACE`, `R/`, `src/Makevars` with `PKG_LIBS` pointing at the core
archive); `build.sh` compiles the same sources straight to a `dyn.load`-able
`pbwire.so` for development without a package install.

## Design: named lists, one arena per schema, builders for hot loops

The schema owns one `pbw_ctx` (arena + encode buffer, reset per call), so
steady use stops calling `malloc` after warmup: list encode/decode converts
through one transient tree per call, builders own plain trees. A builder's
external pointer keeps the schema alive in its protected slot, so collecting
the schema never dangles it.

Fields are read in whatever order the list carries them: the core emits
fields in descriptor order no matter the insertion order, so equal lists encode
to equal bytes and match `protoc --encode` byte for byte (verified on the
`matrix3` small tier). Unknown keys fail instead of being silently dropped;
`NULL` means absent.

## API

| member | meaning |
|---|---|
| `pbwire_load(path)` | load a `FileDescriptorSet` file; errors with the core's `pbw_schema_error()` text when the set is rejected |
| `pbwire_load_raw(bytes)` | same from a raw vector holding the set |
| `pbwire_encode(schema, "pkg.Name", list)` | wire bytes as a raw vector |
| `pbwire_decode(schema, "pkg.Name", wire)` | a named list; unknown fields are skipped, so a re-encode is canonical rather than byte-exact |
| `pbwire_roundtrip(schema, "pkg.Name", wire)` | gateway passthrough: decode and re-encode in the core, unknown fields survive |
| `pbwire_message(schema, "pkg.Name")` | an empty builder message (anchors the schema until collected) |
| `pbwire_message_set(msg, list)` | merge a list into the builder (returns the builder; repeated fields replace) |
| `pbwire_message_encode(msg)` | wire bytes of the builder |

Every `pbw_status` is checked and raised as an R error; a wrong R type for a
value raises; malformed wire data is rejected by `decode`. Nothing aborts or
segfaults on a bad call. Nesting is capped at `PBW_MAX_DEPTH` (256) both ways.

### Value mapping

| protobuf | R |
|---|---|
| int32/sint32/sfixed32/enum | integer; enum also accepts `"NAME"` strings, resolved through the enum |
| int64/sint64/sfixed64, uint32/uint64/fixed* | numeric (double); `u64` max is passed as `-1` (same bit pattern) |
| bool | logical |
| float/double | numeric |
| string | string |
| bytes | raw (string accepted on encode) |
| message | named list |
| repeated field | list (atomic vectors accepted on encode); `{}`/`list()` clears, absent reads as an empty list |

An absent singular field decodes as `NULL`, and a field set to `FALSE`/`0` is
present and round trips as such (explicit presence, like the C API and the Lua
binding: the reference encoder would omit an explicit proto3 zero, both parsers
read it back the same). Oneof: the last member in descriptor order wins on
encode; decode carries the live member only. Groups map like messages
(START_GROUP framing on the wire). Maps are lists of
`list(key=, value=)` entries. `NA` is rejected: protobuf has no null number.

## Deliberately skipped

- **No `protoc` plugin, no codegen, no second wire implementation.** The
  descriptor set is the schema; lists convert through the C core's message
  tree, builders encode it directly.
- **Formats beyond binary/JSON/text lists.** `pbwire_to_json`/`from_json`
  (with an optional ignore-unknown flag), `pbwire_to_text`/`from_text`,
  deterministic encoding (`pbwire_encode_deterministic`, map entries sorted
  by key) and delimited framing (`pbwire_encode_delimited`,
  `pbwire_decode_delimited` returning `list(message, read)`) wrap the C API.
  Lists carry doubles past 2^53 approximately (see below); builders
  (`pbwire_parse_json`/`parse_text`/`parse_binary` plus `pbwire_msg_to_json`/
  `msg_to_text`) skip lists and stay exact.

## Conformance

`conformance/testee.R` drives the official `conformance_test_runner`
through this binding (`make conformance-r`):

```
CONFORMANCE SUITE PASSED: 2742 successes, 0 skipped, 2 expected failures,
0 unexpected failures.
CONFORMANCE SUITE PASSED: 445 successes, 0 skipped, 0 expected failures,
0 unexpected failures.
```

Binary in -> binary out goes through `pbwire_roundtrip()`, so unknown fields
survive and a request is accepted exactly when the core accepts it; JSON/text
travel through builder trees (exact, including 64-bit integers). The 2
expected failures are the shared `conformance/failure_list_pbwire.txt` (tag
varints the reference C++ testee truncates identically); only JSPB answers
`skipped`.

## Benchmark

`benchmarks/bench.R` runs the four standard tiers from
[`BENCHMARKS.md`](../../BENCHMARKS.md) against RProtoBuf when it is installed
(any load failure skips the baseline columns instead of failing the run).
The gate is tier size (the bytes are canonical), byte equality with the
baseline, plus a full-field expectation check.

## Layout

| Piece | Path |
|---|---|
| the binding | [`runtime/src/pbwire_r.c`](runtime/src/pbwire_r.c) (+ [`runtime/R/pbwire.R`](runtime/R/pbwire.R) wrapper) |
| package skeleton | [`runtime/DESCRIPTION`](runtime/DESCRIPTION), [`runtime/NAMESPACE`](runtime/NAMESPACE), [`runtime/src/Makevars`](runtime/src/Makevars) |
| build | [`build.sh`](build.sh) |
| example | [`examples/roundtrip.R`](examples/roundtrip.R) |
| self-check | [`tests/test_pbwire.R`](tests/test_pbwire.R) |
| benchmark | [`benchmarks/bench.R`](benchmarks/bench.R) | the four standard tiers against RProtoBuf |
| conformance | [`conformance/testee.R`](conformance/testee.R) | binary, JSON and text through the official runner |
| fixture | [`tests/data/sample.proto`](tests/data/sample.proto) and the checked-in `sample.desc` |

Regenerate the descriptor set after editing the fixture (it is checked in, so the
tests need no `protoc`):

```shell
(cd tests/data && protoc --descriptor_set_out=sample.desc sample.proto)
```

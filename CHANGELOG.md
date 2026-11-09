# Changelog

All notable changes to pbwire. Versions follow SemVer; `VERSION` is the
source of truth and `scripts/sync-versions.sh` propagates it to every
binding manifest.

## [Unreleased]

### Fixed

- JSON `Timestamp` parsing rejects a date or time a real calendar cannot spell
  (month 13, hour 25, minute or second 61, February 30, and a leap day in a
  common year). The reference implementations reject them too; before this the
  epoch-day arithmetic rolled the value over silently.
- The C struct codec enforces a proto2 `required` scalar on decode: the
  generated layout now carries a `has_` flag for such a field, so a wire that
  omits one fails with `PBW_ERR_REQUIRED` instead of decoding as zero. A layout
  with a required scalar and no flag is rejected at codec init.
- A legacy group whose END_GROUP never arrives, or closes a different field,
  fails the decode (both in the dynamic decoder and in the direct struct
  codec) instead of being accepted.
- The PHP FFI runtime builds nested values in the root's arena (a
  context-built tree could not set a nested field at all), and closing a nested
  view no longer frees the context the root owns.
- The Python and Ruby binding caches drop a half-built message type when an
  allocation fails mid-construction; a later lookup used to hand that type out
  and the codec dereferenced a NULL sub-type.
- The Java encode buffer grows with long arithmetic: past 1 GiB the int
  doubling wrapped to zero and looped forever, and a wrapped size request now
  fails with `PbwireException` instead of writing past the array.
- Generated C# models hash a repeated `float`/`double` field as a collection
  (the singular bitwise-comparer overload did not even compile), and the
  generated models are warning-free.
- Rust `encode_to()` tries the caller's buffer first and only walks the tree a
  second time to size it when it does not fit: the unconditional sizing walk
  had halved encode throughput (0.52x prost on the largest tier).
- Node's lazy slice list refuses a capacity that would wrap to zero.
- `conformance/testee.c` reads a JSON/text payload at its exact field length
  instead of `strlen()`, matching the reference testee when the payload carries
  a NUL byte.
- JSON legacy bare keys compare the literal words at their own length (a field
  named `nulls` was rejected as if it were `null`), and the name buffers hold
  the 512/1024-byte names the descriptor loader accepts.
- `bindings/erlang/c_src/core` is a byte-identical copy of the core again
  (`make erlang-core-sync` refreshes it; the NIF build compares).

### Changed

- `pbw_message_add_field()` indexes field numbers and names as it goes, so
  building a very wide message is linear: 8000 fields went from 61 ms of
  comparing to 0.5 ms.
- `test_nofail` compiles the direct struct codec with the same allocation hooks
  and sweeps its error paths too, and `CMakeLists.txt` builds that variant.
- Generated models validate against the official conformance proto
  (`make codegen-conformance`) in all twenty languages.

## [0.1.14] - 2026-11-07

### Added

- `pbw_encode_delimited_to()` and `pbw_decode_delimited_ctx[_limits]()`: the
  caller-buffer and `pbw_ctx` twins of the length-delimited framing, so a
  framed stream decodes into a workspace arena with no per-frame `malloc` and
  a frame is written without one either. The declared frame length is now
  checked against `max_bytes` in `pbw_decode_delimited_limits()` too, before
  the body is decoded, matching `pbw_istream_next()`.
- `bindings/embedded`: `pbwe_encode_delimited`, `pbwe_decode_delimited` and
  `pbwe_decode_delimited_desc`, so the facade covers both the bare and the
  length-prefixed form without carrying varint code of its own.
- `SECURITY.md`: vulnerability reporting policy and supported versions.
- `NOTICE`: third-party attributions (`descriptor.proto` wire shape, test
  baselines).
- `scripts/release-check.sh` (`make release-check`): release dry-run — version
  consistency, tag-vs-`VERSION` match, changelog entry, artifact presence.
- `tests/fuzz_decode.c` (`make fuzz`): stdin-fed decode/encode/visit fuzz
  harness; CI runs it on a fixed corpus plus a short deterministic mutation
  burst.
- Editions `FeatureSet` support in `pbw_descriptor_set_load()`: file-level
  and per-field `field_presence` / `repeated_field_encoding` are honored
  instead of treating every edition as proto3 defaults.
- Service descriptors: `FileDescriptorSet` services are loaded and queryable
  (`pbw_schema_service_*`, `pbw_service_*`); the core still does no RPC.
- MessageSet wire format: messages with `message_set_wire_format` decode
  group items into their extension fields and re-encode them byte-identically.
- `bindings/MIGRATING.md`: per-language migration notes from the official
  runtimes.
- `examples/gateway/`: proto3 + JSON + unknown-field passthrough tutorial.
- `pbw_descriptor_set_load_limits()`: the `pbw_limits` byte/nesting caps applied
  to the descriptor set itself (`PBW_ERR_LIMIT` on oversize/hostile sets), plus
  a `tests/data/fuzz-desc/` corpus replayed by `make fuzz` through it.
- `pbw_service_resolve()`: service + method name to input/output descriptors,
  so a transport can route without the core doing RPC.
- WKT pack direction and FieldMask paths: `pbw_timestamp/duration_from_millis`,
  `pbw_fieldmask_path_count/at` (the `to_millis` side already existed).
- `tests/data/fuzz-json/` + `tests/data/fuzz-text/` corpora: `make fuzz` replays
  the JSON mapping and text format through the same harness as the wire bytes.
- `scripts/check-llms.sh` (+ CI `docs-sync` job): `llms.txt` must name every
  public `pbw_*` function, so the AI-retrieval summary cannot drift from the API.
- CI: `test_threads` under ThreadSanitizer; nightly runs every ubuntu-capable
  binding self-check; release artifacts carry SLSA provenance attestations.
  New `binding-asan` job: the Python/Ruby/Node extensions build instrumented
  and run their suites under ASan (LEAK detection stays on for Python; the
  Ruby/Node interpreters leak on their own, so they run ASan-only).
- `PBW_EXTRA_CFLAGS` hook in the Ruby/Node `build.sh` drivers (empty by
  default; the ASan job passes `-fsanitize=address` through it).
- Python binding: cyclic `Schema` <-> `MessageType` references are GC-tracked
  now (dropping a schema in a loop holds RSS flat; ASan/LSan clean).
- Ruby binding: fixed a use-after-free reading `pbw_schema_error()` after
  `pbw_schema_free()` on the load-failure path (found by the ASan job).
- Java: `GenericCodec`/`PbCodec.setRetainCap(bytes)` caps the per-thread
  encode buffer (1 MiB default, 0 keeps nothing); Kotlin/Scala/Clojure
  inherit it through the shared runtime.
- Erlang: `pbwire:set_opts([{retain_cap, Bytes}])` caps every scheduler
  context (existing and future, under the cache lock); Elixir passes
  `retain_cap:` through `Pbwire.load_schema/2` (needs Hex `:pbwire` past
  this release to take effect).
- CI gates for the environments a laptop cannot cover:
  - `binding-asan` (PR): Python/Ruby/Node extensions built with
    `-fsanitize=address`, run under their own suites.
  - `ports` (PR): 32-bit (`-m32` ILP32) and big-endian (s390x under
    qemu-user) through `scripts/check-ports.sh`.
  - `api-docs` (PR): Doxygen renders the public headers, uploaded as an
    artifact.
  - `codegen-linux` / `codegen-apple` (nightly): every binding's
    generated-model test, split by toolchain family.
- `make api-docs` (`Doxyfile`): API reference from `include/`, the protobuf
  importer headers included; output lands in `build/doxygen/html`.
- `ROADMAP.md`: what is done, what is never, what is next.
- `docs/examples.md`: one runnable round trip per binding, with the exact command.
- Well-known-type helpers in `src/wkt.c` (`pbw_any_pack`,
  `pbw_any_type_url/payload`, `pbw_timestamp/duration_to_millis`) over field
  numbers only; the core still special-cases no WKT.
- `bindings/embedded/`: static-memory facade over the C core for
  microcontrollers — `pbwe_open/build/decode/encode` over one `pbw_ctx`,
  caller-owned encode buffers, `pbwe_set_limits` for bounded input,
  `pbwe_open`'s descriptor limit for the startup allocation and
  `pbwe_decode_desc` for a type fixed at build time. Covered by `make test`
  and `ctest`, formatted and gated with the rest of the C sources.

### Fixed

- Generated models: a `.proto` file or message named after a host keyword
  produced code that did not compile. Go, Rust and Zig now sanitize the
  derived package/module identifiers (`struct.proto` becomes package
  `struct_pbwire`, module `r#struct`, alias `struct_pbwire`), Dart imports
  sibling models under an `_imp_` prefix (a dep message named `Value` was
  ambiguous with the runtime), Lua requires siblings under `_imp_<stem>` (a dep
  file named `meta` shadowed the file's own schema module and a dep named after
  a Lua keyword was a syntax error), and a oneof enum member in the Java model
  emitted `Kind.KindCase` instead of `Kind<Prop>Case`.
- `src/wkt.c`: `pbw_timestamp/duration_to_millis()` could overflow `int64`
  when the nanosecond remainder pushed a seconds value at the edge of the
  representable millisecond range over the bound (signed-integer overflow is
  undefined behaviour). The range check now leaves room for the sub-second
  part and reports `PBW_ERR_LIMIT`.
- `src/wire.c`: the non-'.' decimal-separator fallback could return an end
  pointer before the start of the number when the host locale's separator is
  multi-byte and the parse stopped inside it.
- `src/json.c`: the JSON printer recursed over nested messages without the
  `PBW_MAX_DEPTH` cap the text printer and both codecs apply, so a hand-built
  tree deep enough to pass `pbw_message_new`'s checks could overflow the stack
  in `pbw_json_print()`. It now reports `PBW_ERR_LIMIT` at the same depth as
  every other formatter.
- Elixir: `pbwire_elixir` never reached Hex in any release, because
  `mix hex.publish` runs `mix docs` first and the `docs` task aborts when
  ExDoc is not a dependency. ExDoc is a `only: :dev, runtime: false`
  dependency now, so the published package carries HexDocs without costing
  consumers a dependency. The nested `else:` inside `Pbwire.Schema.nif_opts/1`
  is parenthesized (Elixir 1.20 warns about the keyword ambiguity).
- Python: `Schema.load()` / `Schema.from_file()` leaked the partially built
  schema `pbw_descriptor_set_load()` hands back on failure, and reported only
  "invalid data" instead of the loader's reason (every other binding already
  frees and surfaces it).
- Binary wire format: a packed (`LEN`) run for a repeated *closed* (proto2)
  enum whose field number is 1..15 was swallowed whole into the unknown bytes,
  because the link-time one-byte tag map omitted the packed-form entry for
  closed enums. The reference parser keeps known values in the field and moves
  each unknown value into the unknown set as one unpacked item; the map now
  routes that form to `read_packed()`, which does the same. (Multi-byte tags,
  resolved through `fnum_map`, always took this path.) An all-unknown run now
  leaves the field absent, like an empty packed run, instead of presenting an
  empty list.

Big-endian correctness, found by the new s390x gate:

- `encode.c`: precomputed tag words were written with a 32-bit store, which
  byte-reverses on a big-endian host (every tag was wrong). They now go
  through `w_tag_word_le`, which keeps the one-store fast path where the host
  byte order matches the wire and spells it byte by byte elsewhere.
- JSON and text printers read a float as the first four memory bytes of the
  64-bit bit pattern; on big-endian that is the high half. They mask to
  `uint32_t` first (same for the parser direction).
- `bindings/c/runtime/pbw_struct.c`: fixed-width member reads and writes
  copied 4 bytes into/out of a `uint64_t`, which lands in the wrong half on
  big-endian. `ld_member`/`st_member` go through the matching integer width.
- 32-bit (`-m32`) correctness: the struct self-check compared doubles with
  `==`, which x87 excess precision makes unreliable; it compares bit patterns
  now (`double_bits`).

### Changed

- Core coverage floor 65% -> 85% (hostile JSON/text/wire/descriptor edge tests;
  two reference-parity fixes found along the way: text `\\u` surrogate-pair
  combining and Any store-error normalization).
- CI: added `sanitizers` (ASan+UBSan `make test`), `difftest-short`
  (fixed-seed differential run), `fuzz-short`, and `coverage` (core-only
  `gcov` floor) jobs; conformance also runs on a nightly schedule.

## [0.1.13] - 2026-11-07

- `bindings/embedded`: static-schema facade over the C core, including the
  length-delimited encode/decode entry points.
- Framing and limits: `pbw_encode_delimited_to()`,
  `pbw_decode_delimited_ctx[_limits]()`, and
  `pbw_descriptor_set_load_limits()` for hostile descriptor sets.
- Descriptor sets: services, editions `FeatureSet` and MessageSet wire format
  are loaded and queryable; the core still does no RPC.
- Core fixes: `pbw_limits` depth propagation through JSON and text parsing,
  list form accepted in text format, unused internal helpers removed.
- Gates: allocation-failure sweep, merged coverage floor, wider malformed and
  descriptor tests.

## [0.1.12] - 2026-11-06

- Erlang Hex package vendors the C core.
- Full bench sweep refresh (R/Zig baselines).

## [0.1.11] - 2026-11-06

- Core fast paths; C# holder fix; all-green baselines.

## [0.1.8] - 2026-11

- Core fast paths (link-time decode actions, packed encode); first release
  line where every binding beats its baseline on xlarge encode.

<!--
Release process: bump VERSION, run scripts/sync-versions.sh, add the entry
above (date = tag date), run ./scripts/release-check.sh, then tag vX.Y.Z.
-->

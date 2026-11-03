# Agents

## Commands

```bash
make          # build build/libpbwire.a
make test     # build and run the self-checks (core + threads + descriptor set)
make erlang-test # Erlang binding: EUnit + NIF C tests (needs OTP, rebar3 bootstrapped)
make format   # clang-format (>= 19) over include/, src/, tests/, bench/
make format-check

# Optional, requires protoc + libprotobuf C++ (brew install protobuf).
# Never part of the commit gate; run them after wire-format changes.
make difftest     # differential test against the official C++ runtime
make bench        # encode/decode benchmark vs generated code and DynamicMessage
make bench-c      # C struct binding benchmark vs protoc C++ on the same message
./build/bench difftest <cases> <seed>   # longer fuzz run
make desc-fixture # regenerate the pinned descriptor set in tests/test_descriptor.c

# Bindings: Python needs the Python headers + a C compiler (protobuf only for
# the benchmark); Erlang needs OTP + gpb. Neither is part of the commit gate.
make python-bind   # build the CPython extension next to its sources
make python-test   # Python binding self-check (pinned protoc bytes as oracle)
make python-bench  # Python binding vs google.protobuf on the four standard tiers
make erlang-bench  # gpb, gpb NIF and enif_protobuf
make test-cpp      # C++ binding self-check (C++17, no protobuf needed)
make conformance-python  # official suite through the Python binding

# Official conformance suite (binary wire format). Needs the cached reference
# checkout: `make conformance-setup` clones it once, `make conformance`
# reuses it. Override with PBWIRE_PROTOBUF_SRC=/path/to/protobuf.
make conformance-setup   # once (kept in .tmp/, git-ignored)
make conformance

# The Makefile is the development gate. CMake is the packaging path and the only
# build that knows about the optional pbwire-protoc target (needs libprotobuf).
cmake -B build-cmake -DPBW_BUILD_PROTOC=ON && cmake --build build-cmake
(cd build-cmake && ctest)   # same checks as `make test`, plus test_importer
```

Run `make test` after any C change; run `make format` after touching `include/`, `src/`, `tests/`,
`bench/` or `conformance/`. Run `make python-test` after touching `bindings/python`, and refresh
`BENCHMARKS.md` after changing anything on the encode/decode hot path.

## Layout

| Path | Contents |
|------|----------|
| `include/pbwire.h` | Public C ABI: schema, descriptor, message, value, wire |
| `src/wire.c` | varint / zigzag / tag / little-endian primitives, output buffer |
| `src/schema.c` | Descriptor registry, name resolution, field lookup tables |
| `src/message.c` | Dynamic message and value tree, ownership, oneof handling |
| `src/arena.c` | Bump allocator behind `pbw_ctx` (reset = pointer move) |
| `src/ctx.c` | `pbw_ctx`: reusable arena + encode buffer, decode/encode entry points |
| `src/stream.c` | delimited framing reader (`pbw_istream`), encode sink, deterministic map sort |
| `src/encode.c` | Message tree -> wire bytes |
| `src/decode.c` | Wire bytes -> message tree, unknown field preservation |
| `src/descriptor_set.c` | `FileDescriptorSet` (protoc/buf output) -> schema; the only reader of descriptor.proto |
| `src/json.c` | JSON mapping printer + parser (no dependencies); extensions as `[full.name]`, WKT special forms |
| `src/text.c` | Text format printer + parser (no dependencies); unknown fields print as numeric lines |
| `src/internal.h` | Internal structs and helpers; never included by consumers |
| `include/pbwire-protoc/` | Optional C++ importer headers: `importer.h`, `descriptor_set.h`, `error.h`; no protobuf types in the API |
| `src/protoc/` | `.proto` -> `FileDescriptorSet` via Google `compiler::Importer` |
| `CMakeLists.txt`, `cmake/` | Two targets (`pbwire`, `pbwire-protoc`), install + `pbwire::pbwire` / `pbwire::protoc` export |
| `tests/test_pbwire.c` | Assert-based self-check (no framework, no dependencies) |
| `tests/test_threads.c` | 8-thread stress test (pthread): shared schema, per-thread contexts, per-thread descriptor-set loading, one shared loaded schema, and a 1-vs-8-thread scaling check |
| `tests/desc_set.h` | Pinned `FileDescriptorSet` shared by `test_descriptor.c` and `test_threads.c` |
| `conformance/testee.c` | Testee for Google's `conformance_test_runner`, binary + JSON + text (JSPB skipped) |
| `tests/test_descriptor.c` | Pinned `protoc` descriptor set -> schema -> wire bytes |
| `tests/data/*.proto` | Fixtures the pinned descriptor set was generated from |
| `tests/protoc/` | Importer self-check and its `.proto` fixtures (CMake only) |
| `bench/bench.cc` | Differential test and benchmark against libprotobuf C++ |
| `bindings/erlang/` | Erlang/OTP NIF binding (`enif_protobuf` app, own rebar3 build, see its AGENTS.md) |
| `bench/msg.proto` | proto3 message used by both; `bench/gen/` is generated, never committed |
| `bench/msg2.proto` | proto2 counterpart: explicit presence, groups, required fields, closed enums, extensions |
| `BENCHMARKS.md` | the four standard payload tiers, every binding vs its baseline, cross-binding table |
| `bindings/c/` | C binding: direct struct codec (`runtime/pbw_struct.c`, layout tables carry the wire facts), `protoc-gen-pbwire-c` (`plugin/`), `examples/`, `tests/`, `benchmarks/`, `conformance/` |
| `bindings/python/` | Python binding: CPython extension (`runtime/pbwire.c`, dict<->wire in C), `examples/`, `tests/`, `benchmarks/`, `conformance/` (official runner testee) |
| `bindings/go/` | cgo binding: `runtime/` (package pbwire), `examples/`, `benchmarks/`, `conformance/` |
| `bindings/cpp/` | C++ binding over the shared C runtime: `examples/`, `tests/`, `benchmarks/`, `conformance/` |
| `bindings/erlang/` | Erlang/OTP NIF binding with its own AGENTS.md |
| `scripts/` | Gate scripts: `check.sh` (hooks), `validate-commit-msg.sh`, `setup-git-hooks.sh` |
| `.github/workflows/ci.yml` | CI: `make test` on Linux + macOS, CMake/ctest, clang-format |
| `README.md`, `llms.txt` | User-facing docs and the AI-retrieval summary; keep them in sync with behavior |

## Binding schema source

This decides how a binding gets its schema, and it is not negotiable:

1. **Ahead-of-time compiled languages load the descriptor set.** C, C++ and Go
   have no way to bake a dynamic schema into the binary, so every binding loads
   a `FileDescriptorSet` (`protoc --descriptor_set_out`) at runtime and drives
   field lookup, wire types and presence from it. A hand-built schema in a
   compiled binding is a bug: it cannot express what a real `.proto` can, and
   it silently diverges from the descriptor every other binding uses.
2. **Languages that load code at runtime get a plugin.** The plugin turns
   `.proto` into that host's own schema definition, which the binding loads once
   at startup. Generated *layout* (C structs and offset tables, Erlang records)
   is part of this, but the field numbers and types still come from the schema.
3. **Erlang needs neither.** gpb already is the plugin: `gpb_compile` /
   `protoc-erl` emits the record definitions plus `get_msg_defs/0`, and
   `enif_protobuf:load_cache/1` consumes exactly that. Do not add a second
   generator or a descriptor loader to the Erlang binding.

`bindings/README.md` tracks each binding against these rules.

## Hard invariants

This library is meant to be embedded in foreign runtimes (Erlang NIF, Python, Go), where a crash
takes down the whole host process. Anything is better than a segfault.

1. **No uncaught errors.** Every failure path returns a `pbw_status`; never `abort`, never
   dereference NULL after a failed allocation, never leave a caller-owned pointer dangling.
2. **Validate at the boundary.** Wire data and public API arguments are untrusted: bounds-check
   every length, reject field number 0 and wire types above 5, cap nesting at `PBW_MAX_DEPTH`.
3. **Ownership is explicit.** Functions documented as taking ownership do so only on success.
   Values rejected by `pbw_message_set` stay owned by the caller.
4. **Bounded recursion and sizes.** Recursion happens only over nested messages and legacy groups,
   both capped by `PBW_MAX_DEPTH`. Buffer growth is bounded by the data being encoded.
5. **Allocation strategy is part of the contract.** `pbw_message_new` owns one message,
   `pbw_decode` owns one arena per decoded tree (a single `malloc` for the whole tree, released by
   `pbw_message_free`), and `pbw_decode_ctx` puts the tree in the context's arena and makes it
   read-only. Anything allocated from an arena must never be freed individually (`value_free`/
   `pbw_message_free` check `->arena` and become no-ops); a value's `PBW_VF_ARENA` flag is what keeps
   a caller-supplied payload freeable. Performance work goes through `make bench`: it also verifies
   byte equality with libprotobuf, so a change that wins ns by changing bytes fails.
6. **The wire format is defined by the reference implementation.** Where the spec is silent
   (varint width, truncation, group skipping), match libprotobuf C++/Java, and prove it with
   `make difftest` rather than reasoning about it. Wire changes that also touch wire.c/decode.c need a
   difftest run, not only `make test`.
   `make conformance` is the authority for the whole binary format, the JSON mapping and the text
   format. Only `conformance/failure_list_pbwire.txt` may declare expected failures -- two binary
   tag-truncation cases that the reference C++ testee fails identically -- so any new failure is a
   regression to fix, not a line to add. Compare against `conformance_cpp` before acting on any
   failure; the binary + JSON suite currently passes 2745/2745 runnable including 4 JSON tests the
   reference itself fails, and the text suite passes 445/445 including 4 the reference fails.
7. **The two libraries stay decoupled.** `pbwire` never includes a protobuf header and never links
   against `pbwire-protoc`; `pbwire-protoc` never includes `src/internal.h` and links nothing from
   the core. They meet only at the serialized `FileDescriptorSet`. Nothing in `src/` may learn to
   parse `.proto` text: that is what the optional importer is for. `make` and `make test` must stay
   buildable with a C99 compiler and no external library.
8. **Unknown fields are data, not noise.** Anything the schema does not declare (including a known
   field carrying the wrong wire type, and legacy groups) is stored as raw wire bytes and written
   back after the known fields in input order. Do not "simplify" this away: it is what lets a
   gateway pass through a schema it only partially knows, and 7 conformance tests depend on it.
9. **No hidden state.** No globals, no thread-local buffers: a schema or message must be safely
   confined to one thread by the caller, and nothing else.

## Commits

- Commit only when the user explicitly asks; follow `CONTRIBUTING.md`
- Run `./scripts/check.sh` before committing or pushing. Use `SKIP_CHECKS=1` only with a real
  reason, and say so in the summary
- Commit messages are English-only (ASCII): `<type>(<scope>): <subject>`, header <= 100 characters

## Repository conventions

- Gates are shell hooks (`.githooks/` + `scripts/`); never add husky / commitlint as a second
  validation path
- `.githooks/pre-commit` and `pre-push` refuse direct commits/pushes on `master`: work on a feature
  branch
- A human enables the hooks once with `./scripts/setup-git-hooks.sh`; agents do not run it

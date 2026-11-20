# Bindings

Every binding ships the same pieces (`plugin/` only where the language needs
it): a runtime, examples, tests, benchmarks and a conformance entry.

```
bindings/<lang>/
    runtime/        the binding's codec (rebar3 layout: c_src/ + src/ + priv/)
    plugin/         optional: schema/code generator
    examples/       a runnable round trip, through the generated model
    tests/          self-checks
    benchmarks/     the four standard tiers from ../BENCHMARKS.md
    conformance/    how the official suite covers this binding
```

`bindings/embedded` is the one exception: it is a pass-through facade over the C
core, so it ships `runtime/`, `tests/` and `examples/` only. The core's bench and
conformance suite already cover the wire it speaks.

The top-level `codegen/` holds one protoc plugin (`protoc-gen-pbwire`) that
emits a **generated message model** for every language from the same descriptor:
fields with the host language's types, names and presence flags, plus
`encode`/`decode` that call the binding's dynamic API. It is pure ergonomics on
top of one wire implementation — see `../codegen/README.md` for the per-language
shape, the `make codegen` / `codegen-test` / `codegen-conformance` targets, and
the measured cost.

## Where the schema comes from

The rule is the same for all of them, and it decides the design more than
anything else:

- **Compiled languages bake the schema in at build time.** A protoc plugin
  turns the `.proto` into the language's own structs and wire facts, so no
  binding hand-builds a schema. C does that for layout/kind/type columns on
  top of the dynamic core.
- **A `FileDescriptorSet` is the runtime schema for the dynamic core and for
  interpreted bindings.** C, C++, the Python extension and the JSON/text paths
  load `protoc --descriptor_set_out` bytes at run time; the C struct entry
  points also validate the generated layout against that schema. Whether
  the set ships as a sidecar file or is embedded in the binary is a deployment
  choice — the loader takes bytes either way (see the
  [FAQ](../docs/faq.md#do-i-have-to-recompile-when-a-proto-changes)).
- **FFI hosts get the descriptor set and nothing else.** Node and
  Objective-C call the C ABI directly and load a `FileDescriptorSet` at run
  time, so neither of them carries a plugin, generated code or a second wire
  implementation. The C core owns the format; the binding owns the ergonomics.
  This is also the cheapest way to add a language.
- **Java is a pure-Java codec, not an FFI host.** It parses the same
  `FileDescriptorSet` with its own reader and owns its wire code in Java
  (columnar values, lazy decode slices, `Unsafe` stores with a checked
  fallback), so there is no native library and no downcall. Kotlin reuses
  that runtime and adds only its DSL.
- **Ruby is a C extension, not an FFI host.** It links the core statically and
  loads a `FileDescriptorSet` at run time like Node/Objective-C (Java parses
  it with its own reader instead of the C core), but the
  message conversion happens in C: a builder tree for mutation and encoding, and
  a validating wire scan that materializes fields lazily for decoding. That is
  what removes the per-call boundary cost that made the Fiddle version lose.
- **Lua is the same shape as Ruby, minus the wire view.** It links the core
  statically and loads a `FileDescriptorSet` at run time, but messages are
  plain tables converted through one transient tree per call instead of a
  builder/wire-view pair.
- **Languages whose toolchain can call C get the descriptor set, not a plugin.**
  Dart (`dart:ffi`) and C# (P/Invoke) are one class with Node
  and Objective-C: load the `FileDescriptorSet`, call the C ABI, own no
  wire code. PHP offers the same FFI wrapper (`ext-ffi`) plus a native C
  extension over the same core for hosts with a compiler; Rust links
  `libpbwire.a` in statically instead of `dlopen`-ing it,
  which makes the result a single self-contained binary; Go compiles its own
  cshim codec into the binary and crosses cgo once per call.
- **Kotlin needs no separate layer.** Kotlin runs on the JVM, so it uses the
  Java binding's runtime and adds only the DSL that official Kotlin support
  is (protoc `--kotlin_out` generates Kotlin over protobuf-java). Scala and
  Clojure are the same move: Scala adds an idiomatic DSL over the Java
  runtime, Clojure adds persistent-map in/out over plain interop — one Java
  runtime for three languages, no second codec anywhere.
- **Embedded is the C core behind a smaller surface.** `bindings/embedded`
  ships no codec and no generated code: `pbwe_open/encode/decode` are thin
  wrappers over `pbw_ctx`/`pbw_encode_to`, with `pbwe_encode_delimited` and
  `pbwe_decode_delimited` covering length-prefixed frames the same way, so a
  firmware image links the same static `libpbwire.a` and loads the same
  `FileDescriptorSet`. Its budget is the box, not the language: `retain_cap`
  plus `pbwe_set_limits` bound what a decode may allocate, and
  `pbwe_open_mem` puts the whole message workspace in a caller block so a
  decode allocates nothing at all (`pbw_ctx_new_mem` in the core, and
  `pbw_struct_arena_new_mem` for the C struct codec). `make embedded-bench`
  and `make embedded-size` compare it against nanopb on one message: 5-8x
  throughput on encode/decode with byte-identical wire, 39 KB of codec flash
  (every message in the schema) against nanopb's 7 KB (one message), and no
  per-message generated code.
- **Erlang needs neither.** gpb already is that plugin: `gpb_compile` /
  `protoc-erl` generates the record definitions plus `get_msg_defs/0`, and
  `pbwire:load_cache/1` consumes exactly that. Adding a second
  generator or a descriptor loader would duplicate gpb. Elixir rides the same
  NIF: `elixir-protobuf` generates the structs plus `__message_props__/0`,
  and the binding translates those to the NIF's tuple form — it owns no C
  of its own (it reuses the published `:pbwire` Hex NIF, the same C core).
- **F# needs no separate layer either.** It references the C# binding's
  project and adds only an F#-idiomatic facade over the same P/Invoke
  runtime, exactly like Kotlin reuses Java.

## pbench: three arms, one contract

Every binding's `pbench/` compares the same three arms over the same `matrix3`
payload and the four standard tiers. Every arm does the same user-visible
task: a native object in, wire bytes out; wire bytes in, a fully readable
native object out. No arm may hold a prebuilt tree before the timer starts,
and no arm may use test-only glue: the native <-> tree move goes through a
shipped converter or a shipped model, so the timer covers what a user of that
path pays.

| arm | mechanism | what it answers |
|---|---|---|
| `tree` | the data already lives in the binding's own message/tree object (`Message`, `DMsg`, `MessageType` handle) | what the codec costs once the tree exists |
| `shim` | C reads and writes the host's own memory through a layout/offset table; one crossing per op, no intermediate tree | is a boundary worth paying for at all |
| `core` | the C core ported to the host language: value tree plus wire engine plus skip/group/unknown/validation, native object in and out | can the host language replace the C core for this binding |

The uniform contract, per binding:

```
New(descriptorSet) -> Arm
Arm.Marshal(nativeMessage) -> wire bytes
Arm.Unmarshal(wire) -> nativeMessage
Arm.Tree(nativeMessage) -> binding tree        // tree arm only, already built
```

Gate before any timing: every arm's bytes equal the official runtime's bytes on
all four tiers, and every arm's decode compares field by field against an
oracle decode. Timing: interleaved rounds (all arms plus the baseline back to
back), median of five, printed in the `BENCHMARKS.md` "Output format" block so
`scripts/bench-record.py` reads it unchanged.

The layered rule replaces "no second wire implementation" for the third arm:
the C core stays the default for every host binding that can reach it, a
host-language `core` port is the one allowed second mechanism, and the official
binary conformance suite is the single contract both obey. `core` numbers enter
`BENCHMARKS.md` only after the arm passes the official binary conformance
suite; until then it prints `n/a`. pbench keeps one copy of the shipped codec,
as an adapter over `runtime/`, never a duplicate engine.

A binding states which of the three it has, including the two-mechanism
bindings and the deferred ports with the reason: the status table in
[BENCHMARKS.md](../BENCHMARKS.md#bindings-vs-their-baselines), with the per-arm
detail in `bindings/<lang>/pbench/README.md`.

## What each binding solves

Every row is also the reason that binding exists: its host runtime's status quo
has a specific cost, and the binding removes it *without* a second wire
implementation (the allowed third mechanism is the
[`core` arm](#pbench-three-arms-one-contract)). The ratios are xlarge-tier
throughput against the official runtime, copied from
[BENCHMARKS.md](../BENCHMARKS.md) where the reasoning and the full tables are.

| binding | the pain it removes | what it does instead | encode / decode vs official |
|---|---|---|---|
| C | dynamic messages cost a libprotobuf dependency, or hand-written wire code per message | struct codec compiled from layout tables over the shared core, schema loaded at run time | 1.33x / 2.00x (vs protoc C++) |
| C++ | generated classes pin the schema at compile time; `DynamicMessage` is 10x slower | the same C runtime and header, no extra layer | 1.33x / 2.00x (vs protoc C++) |
| Embedded | generated C codecs pin the schema at compile time and need a struct per message; a full runtime drags a heap in | static-memory facade over the same core: one box, one arena, caller-owned encode buffers, input bounded by `pbw_limits` | no separate bench — the facade is a pass-through, so the C row and the core bench measure the wire it speaks |
| Python | `google.protobuf` needs a generated module per `.proto`, and dict conversion is seconds at 75 KB | CPython extension: dict↔wire in C, `decode()` returns a parsed message that keeps one slice per wire field | 6.85x / 3.69x (vs upb; 1.19x decode handle, 64x via `encode(parsed)`, data already in pbwire) |
| Ruby | the `google-protobuf` gem materializes and re-encodes every field; a Fiddle bridge costs ~3 µs per downcall | C extension over the core: builder tree for writes, validating wire view for reads | 1.20x / 1.64x (vs the gem) |
| Java | JNI glue, or protobuf-java re-deriving sizes before every write | pure-Java codec: columnar values, lazy decode, `Unsafe` stores (no JNI, no native lib, no generator) | 1.15x / 1.89x (vs protobuf-java; 0.95x encode at 74 KB) |
| Kotlin | official Kotlin is Kotlin over protobuf-java | reuses the Java runtime, adds a DSL | 1.20x / 1.81x (vs protobuf-kotlin; 0.78x encode at 136 B) |
| Node | jspb resolves a code/type table per field and hands every repeated element to a closure | N-API addon (node headers only), reusable buffer, wire view for decoded messages | 22.9x / 12.6x (vs jspb; 2.55x / 2.40x vs [protobufjs](https://www.npmjs.com/package/protobufjs)) |
| Objective-C | the ObjC runtime allocates and copies per field | ARC wrapper: the encoder's buffer is handed to `NSData` with no copy | 1.92x / 1.16x (vs protobuf-objc; 6.6x / 2.7x at 136 B) |
| PHP | the official runtime is pure PHP, all per-field bookkeeping in the interpreter | native C extension over the core (`ext-ffi` wrapper without a compiler) | 1.11x / 1.29x (vs C ext; 362x / 140x vs pure PHP) |
| Dart | a pure-Dart field loop dominates, and FFI adds copies | `dart:ffi` plus the core's arena | 15.4x / 4.27x (vs protobuf-dart) |
| Rust | rust-protobuf generated codecs | links `libpbwire.a` statically behind a safe wrapper | 1.69x / 1.86x (vs rust-protobuf; 0.98x / 1.59x vs prost) |
| C# | Google.Protobuf is already a fast tree codec — the closest race of the set | P/Invoke plus a resolver, no packages | 3.07x / 1.47x (vs Google.Protobuf; 3.93x / 2.27x vs protobuf-net) |
| Go | the official runtime walks `protoreflect`/`protowire` per field | thin generated structs over one cgo crossing per op (`runtime/`: struct offsets as layout tables); the models the cshim cannot drive stay on the pure-Go table engine (`pbench/dyngo/codec/`), which also holds the descriptor-set value tree ([issue 41](https://github.com/jinganix/pbwire/issues/41)) | 1.73x / 1.47x (vs protobuf-go; thin model, struct in and struct out) |
| Erlang | gpb is pure Erlang; gpb NIF binds libprotobuf per message | NIF over the core, consuming gpb's own definitions — no second generator | 3.91x / 1.60x (vs gpb) |
| Swift | Apple moved to Swift while the binding set still spoke ObjC; `swift-protobuf` generates a struct per message | SwiftPM package: FFI host over the C ABI (`FileDescriptorSet` at run time, static `libpbwire.a`, no wire code) | 4.43x / 2.84x (vs swift-protobuf) |
| Scala | data teams write Scala against Java protobuf | same Java runtime as Kotlin plus an idiomatic DSL — one runtime, three JVM languages | 1.21x / 1.25x (vs ScalaPB) |
| Elixir | `elixir-protobuf` is pure Elixir, every field through the interpreter | struct↔tuple translation over the shared NIF (no C of its own) | 2.41x / 0.99x (vs elixir-protobuf; xlarge decode at parity) |
| Zig | `zig-protobuf` codegen per message, fragmented across forks | `@cImport`-free static link (`translate-c` + `libpbwire.a` compiled in) | 1.81x / 2.13x (vs zig-protobuf) |
| F# | .NET protobuf means C# SDK style even in F# code | facade over the C# project (`ProjectReference`, same P/Invoke core) | 3.05x / 1.44x (vs Google.Protobuf) |
| Clojure | protobuf-java interop per message, maps materialized per field | mutable-message fast path over the Java runtime (maps stay for convenience) | 1.16x / 1.77x (vs protobuf-java; mutable-message fast path; 0.75x encode at 136 B) |
| Julia | `ProtoBuf.jl` is pure Julia, per-field interpreter overhead | `dlopen` + `ccall` FFI host (`FileDescriptorSet` at run time, `Dict` boundary) | 135.6x / 1.00x (vs ProtoBuf.jl; baseline encode collapses with size) |
| R | `RProtoBuf` binds the C++ runtime per message | C extension over the core (`.Call`, named lists in/out, statically linked) | 61.5x / 66.6x (vs RProtoBuf) |
| Lua | game servers hand tables to `pbc`/`lua-protobuf`, each with its own schema story | C module over the core: tables convert through the schema's arena, a builder tree is the write fast path | 1.08x / 1.33x (vs lua-protobuf; 7.65x via builder encode — data already in pbwire) |

Read the ratios as "what this binding buys its host language", not as a
cross-language speed ranking: the payload and the boundary cost differ per row,
which is why the at-a-glance table in
[BENCHMARKS.md](../BENCHMARKS.md#at-a-glance-every-binding-one-tier) puts them
side by side with the absolute MB/s.

Header ratios quote the native-object row (host data in, host data out). The
`tree` row further down is the same codec with the tree already built: a proxy
or transcoder task, not an application's encode.

## Status

Migrating from an official runtime? Start at [MIGRATING.md](MIGRATING.md):
the three mental-model shifts plus one row per language.

| binding | schema source | compliant | note |
|---|---|---|---|
| C | `pbw_struct_schema_load(path)` on a `FileDescriptorSet`; `plugin/` additionally emits structs + layout tables for the compile-time side | ✅ | struct layout is codegen, the schema itself is loaded |
| C++ | the C runtime and its generated headers, unchanged | ✅ | inherits C |
| Python | `pbwire.Schema.from_file/load` on a `FileDescriptorSet` | ✅ | the extension is compiled but the schema is always loaded; `decode()` returns a parsed message, `to_dict()`/`encode(dict)` are the dict path; binary + JSON + text testee |
| Go | `protoc-gen-pbwire` generated models (layout tables, no descriptor at run time); the `pbench` Go engine adds `codec.Load` on a `FileDescriptorSet` for a value tree | ✅ | struct path needs no schema; both descriptor-set engines (the C core with the hot-swap registry and the pure-Go one) are `pbench` candidates until [issue 41](https://github.com/jinganix/pbwire/issues/41) closes |
| Erlang | `gpb_compile` → records + `get_msg_defs/0` → `pbwire:load_cache/1` | ✅ | gpb is the plugin, epb needs no descriptor path |
| Lua | `pbwire.loadfile` on a `FileDescriptorSet` | ✅ | tables in, tables out; arena per schema, builders anchor it |
| Java | `FileDescriptorSet` at run time, own pure-Java reader (JDK 17+) | ✅ | no JNI, no C glue, no generator, no native library; binary + JSON + text conformance testee |
| Ruby | `FileDescriptorSet` at run time, a C extension linked against the core | ✅ | no gems, no generator; binary + JSON + text conformance testee |
| Node | `FileDescriptorSet` at run time, hand-written N-API addon | ✅ | no npm deps, node headers only |
| Objective-C | `FileDescriptorSet` at run time, ARC wrapper over the C API | ✅ | Foundation only, no codegen |
| Kotlin | the Java binding's runtime: `FileDescriptorSet` at run time, one Java runtime for both | ✅ | four DSL helpers, no second codec; JSON/text/delimited arrive with the Java runtime |
| PHP | `Pbwire\Schema::fromFile` on a `FileDescriptorSet`, native C extension or `ext-ffi` | ✅ | no composer; the C extension needs only its `.so` |
| Dart | `Schema.load` on a `FileDescriptorSet`, `dart:ffi` | ✅ | `package:ffi` only (malloc, strings) |
| Rust | `Schema::from_file` on a `FileDescriptorSet`, core linked in statically | ✅ | no bindgen, no external crate |
| C# | `Schema.FromFile` on a `FileDescriptorSet`, P/Invoke + `DllImportResolver` | ✅ | no packages in the binding project |
| Swift | `Schema(contentsOfFile:)` on a `FileDescriptorSet`, SwiftPM + C ABI | ✅ | links `build/libpbwire.a` via `unsafeFlags`; baseline is a separate package, no checked-in generated code |
| Scala | the Java binding's runtime: `FileDescriptorSet` at run time | ✅ | DSL over the same runtime as Java/Kotlin; binary + JSON + text testee; baseline needs `scalapbc` once |
| Elixir | `elixir-protobuf` structs + `__message_props__/0` → the shared `:pbwire` NIF | ✅ | no C of its own; Hex `:pbwire` tracks `bindings/erlang` (mix.lock pins the current release) |
| Zig | `Schema.fromFile` on a `FileDescriptorSet`, core sources compiled in statically | ✅ | `build.zig` API targets Zig 0.17; binary + JSON + text testee (`make conformance-zig`); baseline not wired (needs network) |
| F# | the C# binding's project: `FileDescriptorSet` at run time | ✅ | `ProjectReference`, no second runtime; JSON/text/visit come from the C# binding; binary + JSON + text testee |
| Clojure | the Java binding's runtime via plain interop, `FileDescriptorSet` at run time | ✅ | persistent maps in/out, plus `to-json`/`from-json`/`to-text`/`from-text`; binary + JSON + text testee; `benchmarks/deps` jars are fetched, not committed |
| Embedded | `pbwe_open` on a `FileDescriptorSet`, static facade over the C core | ✅ | one malloc phase, then bounded steady state; caller-owned buffers, no per-message allocation |
| Julia | `Libdl.dlopen` + `ccall`, `FileDescriptorSet` at run time | ✅ | stdlib-only package; binary + JSON + text testee; baseline oracle is generated with `protojl`, not committed |
| R | `.Call` C extension on a `FileDescriptorSet`, core linked in statically | ✅ | named lists in/out; INT32_MIN reads back as `NA`, doubles clamp at the 64-bit limits |

Every row above also has a generated model from `codegen`: typed
fields plus `encode`/`decode` over that binding's codec. C keeps its own
generator (it is ahead-of-time compiled); Go's models are refreshed by
`make codegen` like every other language; Erlang keeps gpb; Elixir keeps
`elixir-protobuf` (which is also what its `codegen` emitter emits). Every
other binding — including Swift, Zig, Scala, F#, Clojure, Julia and R — has a
`codegen` emitter whose output is committed under `tests/gen` and
`examples/gen`. Embedded needs no emitter: it encodes and decodes through
the existing dynamic API (`pbw_encode_to`/`pbw_decode_ctx`) with no generated
types of its own.

## Memory: what each binding keeps, and the one rule per row

A context's resident ceiling is `retain_cap` (1 MiB default) for the arena
head plus the encode buffer; anything above it is freed on reset instead of
kept (see `../docs/guide.md#reuse-one-arena`). Java, Erlang and Elixir expose
it as a setting (`GenericCodec`/`PbCodec.setRetainCap`,
`pbwire:set_opts([{retain_cap, Bytes}])`, `Pbwire.load_schema(mods,
retain_cap: Bytes)`), and Ruby (`Pbwire::Context`/`Pbwire::Pool`), Objective-C
(`PBWContext`/`PBWPool`), Lua (`new_context`/`new_pool`) and R
(`pbwire_context_*`/`pbwire_pool_*`) expose the context and pool APIs
themselves. Every other `pbw_ctx` below runs at the default cap; the table
gives the ceiling and the workaround.

| binding | what stays resident | ceiling | rule |
|---|---|---|---|
| C / C++ | caller-owned `pbw_struct_buf` (keeps capacity) + `pbw_struct_arena` (each decode resets it) | largest message handled | an arena decode invalidates the previous tree; never `pbw_struct_free` an arena tree (`NULL` arena = per-message malloc + `pbw_struct_free`) |
| Embedded | one `pbw_ctx` inside the box (arena + encode buffer, reset per decode, `retain_cap` caps it) + the loaded schema | `retain_cap` | the next decode invalidates the previous tree; `pbwe_set_limits` rejects oversized input before it allocates; `pbwe_open`'s descriptor limit bounds the startup load; `pbwe_close` releases both |
| Python | own C codec, no `pbw_ctx`; a decoded `Message` pins the whole input buffer | retained `Message`s | drop the `Message` after materializing (`to_dict()` output does not free it) |
| Ruby | builder = C tree, `encode` writes straight into the returned `String`; a wire view pins the bytes; optional `Context`/`Pool` workspace | retained views, plus the context's `retain_cap` when one is used | same as Python: `set` on a view raises, and holding a view holds its bytes; a context borrow dies with the next reset |
| Java / Kotlin | one `ThreadLocal` workspace: a reusable encode buffer (capped by `setRetainCap`, 1 MiB default) + a fixed 8-slot string-image cache | live + dead threads x capped buffer each | lower the cap for thread-per-request pools, or reuse a fixed pool |
| Node | single-threaded: one reusable encode `pbw_ctx` per message type (freed with the type) + decoded message = JS `Buffer` + slice array | types x largest encoding each + retained messages | drop decoded messages; a type's buffer stays at its largest encoding |
| Objective-C | `PBWContext` (with `retainCap`) backs `-decode:withContext:error:`, `PBWPool` hands contexts out | the context's capped buffer | an arena decode invalidates the previous tree; a pool `release` resets it |
| Dart / Rust / C# / Swift / Zig / F# / Julia | plain `pbw_encode`/`pbw_decode` per call (`NSData` takes the malloc'd buffer over with no copy; Julia holds input bytes via finalizers) | none, except retained Julia messages | nothing to tune; Julia messages need a `close()` |
| Go | one cached C codec per message type (`sync.OnceValue`: layout plan, decode arena, per-field accumulators); the Go string arena is per `Unmarshal` call and lives as long as the decoded message | the largest message that type has decoded | no `Close` on the model: the plan and its scratch buffers live for the process, so per-type memory settles at the largest decode; encode and decode of one type share that scratch under one mutex, so they run one at a time |
| PHP | built trees live in one `pbw_ctx` each (freed with the message, never decoded into so always mutable); decoded trees are single-malloc arenas | retained messages | drop the message; nested views hold the parent, `close()` frees either kind |
| Erlang | one `pbw_ctx` per scheduler + a geometrically grown encode buffer, capped by `set_opts([{retain_cap, Bytes}])` (1 MiB default) | schedulers x capped buffer each | bounded by the scheduler count; lower the cap when messages vary wildly in size |
| Lua | one `pbw_ctx` per schema (arena + encode buffer, reset per call; builders own plain trees); `new_context`/`new_pool` expose the arena API | largest message handled, plus the context's `retain_cap` | nothing to tune; idle schemas keep up to the retain cap; a context borrow dies with the next reset |
| Scala / Clojure | one `ThreadLocal` workspace inherited from the Java runtime (reusable buffer, never freed until thread exit) | live + dead threads x largest message each | same rule as Java: fixed pool, not thread-per-request |
| Elixir | one `pbw_ctx` per scheduler via the shared NIF (BEAM-global schema cache, like Erlang) | schedulers x capped buffer each | same rule as Erlang (`load_schema(mods, retain_cap: Bytes)`) |
| Julia | `ccall` per call; decoded messages hold the input bytes via finalizers | retained messages | drop the message; `close()` frees either kind |
| R | built trees live in one `pbw_ctx` each; decoded trees are single-malloc arenas; `pbwire_context_*`/`pbwire_pool_*` expose the arena API | retained messages, plus the context's `retain_cap` | drop the message; nested views hold the parent, builders anchor the schema; a context borrow dies with the next reset |

Compiler-level code generation is never hand-written: C's layout tables come
from the same descriptor `protoc` parsed, so field numbers, wire types and
proto2/proto3 semantics are the schema's. C loads the descriptor set at run
time as well (the dynamic core needs it and the struct entry points validate
against it), and Erlang's records are a layout over gpb's definitions.

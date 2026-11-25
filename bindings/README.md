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
  plain tables: the one-entry `schema:encode`/`schema:decode` walks them
  straight through to the wire, while a builder tree stays for mutation,
  exact-64-bit and JSON/text paths.
- **Languages whose toolchain can call C get the descriptor set, not a plugin.**
  Dart (`dart:ffi`) and C# (P/Invoke) are one class with Node
  and Objective-C: load the `FileDescriptorSet`, call the C ABI, own no
  wire code. PHP offers the same FFI wrapper (`ext-ffi`) plus a native C
  extension over the same core for hosts with a compiler; Rust links
  `libpbwire.a` in statically instead of `dlopen`-ing it,
  which makes the result a single self-contained binary; Go ships pure-Go
  generated models with no cgo and no C toolchain.
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

## pbench: four arms, one contract

<a id="pbench-three-arms-one-contract"></a>

Every binding's `pbench/` compares the same four arms over the same `matrix3`
payload and the four standard tiers. The `tree`, `shim` and `core` arms do the
same user-visible task: a native object in, wire bytes out; wire bytes in, a
fully readable native object out. None of the three may hold a prebuilt tree
before the timer starts, and none may use test-only glue: the native <-> tree
move goes through a shipped converter or a shipped model, so the timer covers
what a user of that path pays. The `tree` arm builds its native input fresh
per iteration; the `shim`/`core` encode arms may reuse app-owned native input
across iterations (the steady-state carve-out: the boundary price is their
question, not the build price), disclosed per binding in its pbench README.
The fourth arm, `proxy`, times the prebuilt task instead: re-encode a held
handle, decode-and-free with no materialization. It is published, never the
headline.

| arm | mechanism | what it answers |
|---|---|---|
| `tree` | a native object in and out through the shipped per-field API (build per field, encode; decode, read back per field, free); no prebuilt tree before the timer | what a user of that path pays |
| `shim` | C reads and writes the host's own memory through a layout/offset table; one crossing per op, no intermediate tree | is a boundary worth paying for at all |
| `core` | the C core ported to the host language: value tree plus wire engine plus skip/group/unknown/validation, native object in and out | can the host language replace the C core for this binding |
| `proxy` | the prebuilt task: re-encode a held handle, decode-and-free with no materialization | what the proxy/transcoder task costs; published, never the headline |

The uniform contract, per binding:

```
New(descriptorSet) -> Arm
Arm.Marshal(nativeMessage) -> wire bytes
Arm.Unmarshal(wire) -> nativeMessage
Arm.Proxy() -> wire bytes                       // proxy arm only, prebuilt held before the timer
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

C# has all three plus the prebuilt row: `tree` is the per-field full task
(fresh handle per iteration, one shipped `Set()` per field, `Encode()` +
`Dispose()` inside the timer; decode reads every gate-checked field back),
`shim` is the generated model's native-object entry (`pbwire-csharp-shim`), and `core`
is the pure-C# port in `bindings/csharp/Pbwire/Core/` (`pbwire-csharp-core`,
`make conformance-csharp`). F# rides the C# P/Invoke runtime and prints the
same `tree`/`shim` rows plus a same-run `proxy` through it; the pure core is C#-only today (no F#
facade is wired to it, no separate F# port exists). The `core` row is the
honest full task (plain object in, conversion plus codec per call): it trails
the generated codec on every cell (0.68x/0.73x small encode is its best,
0.55x / 0.25x at 74 KB) -- the no-toolchain arm, never the headline.
The C# `shim` row copies the
native object into one contiguous image per op and hands it to a C-side reader
(`bindings/csharp/native/pbw_cs_shim.c`, one `P/Invoke` per op): it is above
Google.Protobuf on every encode tier (1.10x-2.60x) and on the small decode
tiers
(1.55x-1.58x), and below it at 2469 B / 74913 B decode (0.78x / 0.74x) because
the
image-to-object copy costs more than the reference parser's whole run (see
`bindings/csharp/pbench/README.md`). The `proxy` row holds the prebuilt-handle
price (3.59x / 1.47x at 74 KB), published beside the native rows and never
headlined. A managed
class with reference members cannot be pinned, so the image -- not
`GCHandle.Alloc(obj, Pinned)` -- is what C walks.

Python has all three plus the prebuilt row: `tree` is the per-field full task
(fresh dict in, `encode(dict)`; `decode(bytes)` plus `to_dict()` with every
field read back), `shim` is the same task in one CPython downcall per op, and `core` is the pure-Python
port in `bindings/python/pure/` (`pbwire-py-core`, `make
conformance-python-pure`). The core is the no-extension mode (pure-Python
interpreter, PyPy, sandbox), published, never the headline: at 74 KB it is 6.5
MB/s decode / 10.3 MB/s `encode(data)` against the extension's 911.2 / 570.2
(`proxy decode(bytes)` / `tree encode(data)`),
and against `google.protobuf`'s own pure-Python runtime -- the comparable
baseline -- it is 1.05x on decode and 1.25x on `encode(parsed)`. See
`bindings/python/pbench/README.md`.

Dart has all three plus the prebuilt row: `tree` is the per-field full task
(one `set` per field, `encode()`, `close()` per iteration; decode plus a full
read-back), `shim` is the
generated flat `<Message>Layout` read in place by the C-side reader
(`pbwire-dart-shim`), and `core` is the pure-Dart codec of
`bindings/dart/lib/core/` (`pbwire-dart-core`, no `dart:ffi` and no `dart:io`,
so it is also the row that exists on dart2js/dart2wasm).

Julia has all three plus the prebuilt row: `tree` is the per-field full task
(fresh `Message` per iteration, one `set` per field, `close()` inside the
timer; decode plus a full read-back), `shim` is the
generated mutable `<Message>Layout` read in place by
`bindings/julia/native/pbw_julia_shim.c` (`pbwire-julia-shim`), and `core` is
the pure-Julia codec of `bindings/julia/runtime/src/core/`
(`pbwire-julia-core`: stdlib only, no `ccall`, no native library; 2.42x
ProtoBuf.jl on the 74 KB encode -- on the same collapsed baseline the native
rows quote -- and below it everywhere else, the price of the
`Dict` <-> tree conversion in an interpreter). The `proxy` row holds the
prebuilt-handle price, published beside the native rows and never headlined.

Rust has all three plus the prebuilt row: `tree` is the per-field full task
(fresh `fill` build per iteration, `encode`; decode plus a full read-back),
`shim` is the
generated `#[repr(C)]` `<Message>Flat` read in place by the C struct codec
compiled into the crate (`pbwire-rust-shim`, one call per op), and `core` is the
pure-Rust codec of `bindings/rust/src/core/` behind the `core` cargo feature
(`pbwire-rust-core`, no C library, no `unsafe`, no generated code;
`make conformance-rust`). It is above `rust-protobuf` on the two large encodes
(1.12x / 1.08x) and below prost everywhere (0.62-0.70x encode, 0.47-0.65x
decode),
because prost's generated code has no value tree in the way: published as the
no-C-toolchain mode, never as the shipped fast path.

Swift has all three plus the prebuilt row: `tree` is the per-field full task
(fresh message plus `fillTree` build per iteration; decode plus `consumeTree`),
`shim` is the
generated `@frozen` `<Message>Repr` mirror (no references, so no ARC per field)
read in place by `bindings/swift/Sources/CPbwire/shim.c` over the same C struct
codec (`pbwire-swift-shim`, one call per op), and `core` is the `PbwireCore`
product (`bindings/swift/Sources/PbwireCore/`): a pure-Swift descriptor reader,
value tree, encoder and decoder with no C module, no `libpbwire.a` and no
`unsafeFlags`, so it can be a normal SwiftPM dependency and `make
conformance-swift` can drive the official suite through it.

Zig has all three plus the prebuilt row: `tree` is the per-field full task
(fresh handle per iteration, one `set` per field; decode plus a full
read-back), `shim` is the
generated `extern struct` mirror read in place by the C struct codec
(`pbwire-zig-shim`, one call per op), and `core` is the pure-Zig codec of
`bindings/zig/core/` selected by `-Dcore=true` (`pbwire-zig-core`: descriptor
reader, value tree and binary encoder/decoder with no C source, no translate-c
and no `libpbwire.a`; `make conformance-zig-core`). It is below the generated
baseline on every cell (0.42-0.88x encode, 0.54-0.93x decode), so the
static link stays the default and the core option exists for a build that
cannot compile `src/*.c`.

Ruby has two native arms plus the prebuilt row: `tree` is the per-field facade
(fresh model per iteration, full read-back on decode), `shim` is the
generated model read by one C-extension entry (`pbwire-ruby-shim`); `core` is
deferred (`shortcut: no pure-Ruby core, reopen when a native-less Ruby target
appears`).

R has two native arms plus the prebuilt row: `tree` is the per-field-API
reference (fresh builder, full read-back on decode), `shim` is
the named list in and wire bytes out through one `.Call` with the walk inside C
(`pbwire-r-shim`, the generated model's `Type_encode(list)` shape); `core` is
deferred (`shortcut: no pure-R core, reopen when a native-less R target
appears`).

PHP has two native arms plus the FFI deployment and the prebuilt row: `tree`
is the per-field full task (fresh `Message` per call, one `set`/`get` per
field, `close` inside the timer), `shim` is the native array in and wire bytes
out through one
C-extension entry that walks it in C (`pbwire-php-shim`), and `shim-ffi` is the
same API on the shipped `ext-ffi` wrapper (`pbwire-php-shim-ffi`, no compiler);
`core` is deferred (`shortcut: no pure-PHP core, reopen when a no-compiler
deployment needs it`) because the official pure-PHP runtime is already the
baseline.

A binding states which of the four it has, including the two-mechanism
bindings and the deferred ports with the reason: the status table in
[BENCHMARKS.md](../BENCHMARKS.md#at-a-glance-every-binding-one-tier), with the per-arm
detail in `bindings/<lang>/pbench/README.md`.

## What each binding solves

Every row is also the reason that binding exists: its host runtime's status quo
has a specific cost, and the binding removes it *without* a second wire
implementation (the allowed third mechanism is the
[`core` arm](#pbench-four-arms-one-contract)). The ratios are xlarge-tier
throughput against the official runtime, copied from
[BENCHMARKS.md](../BENCHMARKS.md) where the reasoning and the full tables are.

| binding | the pain it removes | what it does instead | encode / decode vs official |
|---|---|---|---|
| C | dynamic messages cost a libprotobuf dependency, or hand-written wire code per message | struct codec compiled from layout tables over the shared core, schema loaded at run time | 4.17x / 2.03x (vs protoc C++) |
| C++ | generated classes pin the schema at compile time; `DynamicMessage` is 10x slower | the same C runtime and header, no extra layer | 4.17x / 2.03x (vs protoc C++) |
| Embedded | generated C codecs pin the schema at compile time and need a struct per message; a full runtime drags a heap in | static-memory facade over the same core: one box, one arena, caller-owned encode buffers, input bounded by `pbw_limits` | no separate bench — the facade is a pass-through, so the C row and the core bench measure the wire it speaks |
| Python | `google.protobuf` needs a generated module per `.proto`, and dict conversion is seconds at 75 KB | CPython extension: dict↔wire in C, `decode()` returns a parsed message that keeps one slice per wire field | 2.75x / 3.58x (vs upb; `shim encode(data)` / `shim to_dict`) |
| Ruby | the `google-protobuf` gem materializes and re-encodes every field; a Fiddle bridge costs ~3 µs per downcall | C extension over the core: per-field facade plus a one-entry native-object path over the generated model (prebuilt builder/wire view stays the holder fast path) | 5.03x / 6.11x `shim` (vs the gem) |
| Java | JNI glue, or protobuf-java re-deriving sizes before every write | pure-Java codec: columnar values, lazy decode, `Unsafe` stores (no JNI, no native lib, no generator) | 1.00x / 0.84x `core` (generated model, vs protobuf-java) |
| Kotlin | official Kotlin is Kotlin over protobuf-java | reuses the Java runtime, adds a DSL | 27.74x / 0.77x `core` (vs protobuf-kotlin) |
| Node | jspb resolves a code/type table per field and hands every repeated element to a closure | N-API addon (node headers only), reusable buffer, wire view for decoded messages; the native-object path is one addon call that walks the JS object in C | 0.19x / 0.13x `shim` (vs protobufjs) |
| Objective-C | the ObjC runtime allocates and copies per field | ARC wrapper: the encoder's buffer is handed to `NSData` with no copy | 0.97x / 1.16x (vs protobuf-objc; single native path, fresh build per iteration) |
| PHP | the official runtime is pure PHP, all per-field bookkeeping in the interpreter | native C extension over the core (`ext-ffi` wrapper without a compiler); the native-object path is one extension entry that walks the PHP array in C | 2.94x / 2.81x `shim` (vs the C ext) |
| Dart | a pure-Dart field loop dominates, and the per-field walk is what the native task pays | `dart:ffi` plus the core's arena; the native-object path is one FFI call that reads the flat `<Message>Layout` in place (`bindings/dart/native/`); the pure-Dart `core` is the portable path for hosts without a C library | 1.82x / 4.84x `shim` (vs protobuf-dart) |
| Rust | rust-protobuf generated codecs | links `libpbwire.a` statically behind a safe wrapper | 2.02x / 1.86x `shim` vs prost |
| C# | Google.Protobuf is already a fast tree codec — the closest race of the set | P/Invoke plus a resolver, no packages; the native-object path is one call into `bindings/csharp/native/pbw_cs_shim.c` over a per-op flat image | 1.32x / 0.77x `shim` (vs Google.Protobuf) |
| Go | the official runtime walks `protoreflect`/`protowire` per field | thin generated structs over the pure-Go table engine (`runtime/`), which also holds the descriptor-set value tree | 1.36x / 1.33x (vs protobuf-go; thin model, struct in and struct out, xlarge tier) |

| Erlang | gpb is pure Erlang; gpb NIF binds libprotobuf per message | NIF over the core, consuming gpb's own definitions — no second generator | 2.95x / 1.59x (vs gpb) |
| Swift | Apple moved to Swift while the binding set still spoke ObjC; `swift-protobuf` generates a struct per message | SwiftPM package: FFI host over the C ABI (`FileDescriptorSet` at run time, static `libpbwire.a`, no wire code), plus a pure-Swift `PbwireCore` product for consumers the `unsafeFlags` static link cannot reach | 3.24x / 2.54x `shim` (vs swift-protobuf) |
| Scala | data teams write Scala against Java protobuf | same Java runtime as Kotlin plus an idiomatic DSL — one runtime, three JVM languages | 211.33x / 0.30x `core` (case class, vs ScalaPB) |
| Elixir | `elixir-protobuf` is pure Elixir, every field through the interpreter | struct↔tuple translation over the shared NIF (no C of its own) | 2.98x / 1.91x (vs elixir-protobuf) |
| Zig | `zig-protobuf` codegen per message, fragmented across forks | `@cImport`-free static link (`translate-c` + `libpbwire.a` compiled in); the generated `extern struct` mirror is read in place by the C struct codec, one call per op; a pure-Zig `core` port behind `-Dcore=true` is the no-C-source build | 1.84x / 1.47x `shim` (vs zig-protobuf) |
| F# | .NET protobuf means C# SDK style even in F# code | facade over the C# project (`ProjectReference`, same P/Invoke core and the same one-call C reader) | 1.37x / 0.74x `shim` (vs Google.Protobuf) |
| Clojure | protobuf-java interop per message, maps materialized per field | mutable-message fast path over the Java runtime (maps stay for convenience) | 0.26x / 0.74x (vs protobuf-java; mutable-message fast path; 0.05x encode at 136 B) |
| Julia | `ProtoBuf.jl` is pure Julia, per-field interpreter overhead | `dlopen` + `ccall` FFI host (`FileDescriptorSet` at run time, `Dict` boundary); the generated layout image is read in place by a C-side reader (`bindings/julia/native/`), one `ccall` per op; the pure-Julia `core` (`runtime/src/core/`) is the no-native arm | 22.35x / 1.08x `shim` (vs ProtoBuf.jl) |
| R | `RProtoBuf` binds the C++ runtime per message | C extension over the core (`.Call`, named lists in/out, statically linked) | 19.28x / 57.00x `shim` (vs RProtoBuf) |
| Lua | game servers hand tables to `pbc`/`lua-protobuf`, each with its own schema story | C module over the core: one-entry table encode/decode plus a builder tree for mutation | 0.98x (tie) / 1.47x `shim` (vs lua-protobuf; native-object, one C entry per op) |

Read the ratios as "what this binding buys its host language", not as a
cross-language speed ranking: the payload and the boundary cost differ per row,
which is why the at-a-glance table in
[BENCHMARKS.md](../BENCHMARKS.md#at-a-glance-every-binding-one-tier) puts them
side by side with the absolute MB/s.

Header ratios quote the native-object row (host data in, host data out): what a user of the shipped path pays.

Go-parity close (2026-11-22, issue 41 plan): every binding with a `pbench/`
re-times its `tree` arm as the full task -- fresh native object per iteration,
alloc and free inside the timer, deep materialization of every gate-checked
field -- and every published row stays green behind the official conformance
suite.
Below-baseline native cells are parked with reasons in `docs/perf-attempts.md`
(no untried one-change mechanism); Task 15's JVM slice is closed there,
other tracks still parked.

## Status

Migrating from an official runtime? Start at [MIGRATING.md](MIGRATING.md):
the three mental-model shifts plus one row per language.

| binding | schema source | compliant | note |
|---|---|---|---|
| C | `pbw_struct_schema_load(path)` on a `FileDescriptorSet`; `plugin/` additionally emits structs + layout tables for the compile-time side | ✅ | struct layout is codegen, the schema itself is loaded |
| C++ | the C runtime and its generated headers, unchanged | ✅ | inherits C |
| Python | `pbwire.Schema.from_file/load` on a `FileDescriptorSet` | ✅ | the extension is compiled but the schema is always loaded; `decode()` returns a parsed message, `to_dict()`/`encode(dict)` are the dict path; binary + JSON + text testee |
| Go | `protoc-gen-pbwire` generated models (field tables, no descriptor at run time); the `pbench` Go engines add `Load` on a `FileDescriptorSet` for a value tree | ✅ | struct path needs no schema; the dynamic engines stay `pbench` arms for the value-tree task ([issue 41](https://github.com/jinganix/pbwire/issues/41) closed 2026-11-22; the thin model is the shipped path) |
| Erlang | `gpb_compile` → records + `get_msg_defs/0` → `pbwire:load_cache/1` | ✅ | gpb is the plugin, epb needs no descriptor path |
| Lua | `pbwire.loadfile` on a `FileDescriptorSet` | ✅ | tables in, tables out; arena per schema, builders anchor it |
| Java | `FileDescriptorSet` at run time, own pure-Java reader (JDK 17+) | ✅ | no JNI, no C glue, no generator, no native library; binary + JSON + text conformance testee |
| Ruby | `FileDescriptorSet` at run time, a C extension linked against the core | ✅ | no gems, no generator; binary + JSON + text conformance testee |
| Node | `FileDescriptorSet` at run time, hand-written N-API addon | ✅ | no npm deps, node headers only |
| Objective-C | `FileDescriptorSet` at run time, ARC wrapper over the C API | ✅ | Foundation only, no codegen |
| Kotlin | the Java binding's runtime: `FileDescriptorSet` at run time, one Java runtime for both | ✅ | four DSL helpers, no second codec; JSON/text/delimited arrive with the Java runtime |
| PHP | `Pbwire\Schema::fromFile` on a `FileDescriptorSet`, native C extension or `ext-ffi` | ✅ | no composer; the C extension needs only its `.so`; `Type::encodeArray`/`decodeArray` are the native-array entry |
| Dart | `Schema.load` on a `FileDescriptorSet`, `dart:ffi` | ✅ | `package:ffi` only (malloc, strings) |
| Rust | `Schema::from_file` on a `FileDescriptorSet`, core linked in statically | ✅ | no bindgen, no external crate |
| C# | `Schema.FromFile` on a `FileDescriptorSet`, P/Invoke + `DllImportResolver` | ✅ | no packages in the binding project |
| Swift | `Schema(contentsOfFile:)` on a `FileDescriptorSet`, SwiftPM + C ABI | ✅ | links `build/libpbwire.a` via `unsafeFlags`; baseline is a separate package, no checked-in generated code |
| Scala | the Java binding's runtime: `FileDescriptorSet` at run time | ✅ | DSL over the same runtime as Java/Kotlin; binary + JSON + text testee; baseline needs `scalapbc` once |
| Elixir | `elixir-protobuf` structs + `__message_props__/0` → the shared `:pbwire` NIF | ✅ | no C of its own; Hex `:pbwire` tracks `bindings/erlang` (mix.lock pins the current release) |
| Zig | `Schema.fromFile` on a `FileDescriptorSet`, core sources compiled in statically; `-Dcore=true` uses the pure-Zig reader | ✅ | `build.zig` API targets Zig 0.17; binary + JSON + text testee (`make conformance-zig`), binary pure-core testee (`make conformance-zig-core`); `make zig-bench` wires the pinned Arwalk/zig-protobuf baseline (fetched on first run) |
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
| Go | no per-type retained state: each `Marshal` allocates its own buffer (sized by the table's shared atomic size hint) and each `Unmarshal` its own string arena | one buffer + one arena per call | nothing to tune |
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

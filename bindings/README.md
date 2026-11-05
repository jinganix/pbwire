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
  which makes the result a single self-contained binary; Go does the same over
  cgo and adds a hot-swappable schema registry on top.
- **Kotlin needs no separate layer.** Kotlin runs on the JVM, so it uses the
  Java binding's runtime and adds only the DSL that official Kotlin support
  is (protoc `--kotlin_out` generates Kotlin over protobuf-java).
- **Erlang needs neither.** gpb already is that plugin: `gpb_compile` /
  `protoc-erl` generates the record definitions plus `get_msg_defs/0`, and
  `pbwire:load_cache/1` consumes exactly that. Adding a second
  generator or a descriptor loader would duplicate gpb.

## What each binding solves

Every row is also the reason that binding exists: its host runtime's status quo
has a specific cost, and the binding removes it *without* a second wire
implementation. The ratios are xlarge-tier throughput against the official
runtime, copied from [BENCHMARKS.md](../BENCHMARKS.md) where the reasoning and
the full tables are.

| binding | the pain it removes | what it does instead | encode / decode vs official |
|---|---|---|---|
| C | dynamic messages cost a libprotobuf dependency, or hand-written wire code per message | struct codec compiled from layout tables over the shared core, schema loaded at run time | 1.58x / 2.26x (vs protoc C++) |
| C++ | generated classes pin the schema at compile time; `DynamicMessage` is 10x slower | the same C runtime and header, no extra layer | 1.58x / 2.26x (vs protoc C++) |
| Python | `google.protobuf` needs a generated module per `.proto`, and dict conversion is seconds at 75 KB | CPython extension: dict↔wire in C, `decode()` returns a parsed message that keeps one slice per wire field | 6.5x / 1.69x (vs upb) |
| Ruby | the `google-protobuf` gem materializes and re-encodes every field; a Fiddle bridge costs ~3 µs per downcall | C extension over the core: builder tree for writes, validating wire view for reads | 1.22x / 1.67x (vs the gem) |
| Java | JNI glue, or protobuf-java re-deriving sizes before every write | pure-Java codec: columnar values, lazy decode, `Unsafe` stores (no JNI, no native lib, no generator) | 1.21x / 2.40x (vs protobuf-java) |
| Kotlin | official Kotlin is Kotlin over protobuf-java | reuses the Java runtime, adds a DSL | 1.60x / 1.19x (vs protobuf-kotlin) |
| Node | jspb resolves a code/type table per field and hands every repeated element to a closure | N-API addon (node headers only), reusable buffer, wire view for decoded messages | 22.3x / 20.0x (vs jspb; 2.7x / 4.1x vs [protobufjs](https://www.npmjs.com/package/protobufjs)) |
| Objective-C | the ObjC runtime allocates and copies per field | ARC wrapper: the encoder's buffer is handed to `NSData` with no copy | 1.8x / 2.7x (vs protobuf-objc) |
| PHP | the official runtime is pure PHP, all per-field bookkeeping in the interpreter | native C extension over the core (`ext-ffi` wrapper without a compiler) | 1.11x / 1.15x (vs C ext; 374x / 127x vs pure PHP) |
| Dart | a pure-Dart field loop dominates, and FFI adds copies | `dart:ffi` plus the core's arena | 12.2x / 3.3x (vs protobuf-dart) |
| Rust | rust-protobuf generated codecs | links `libpbwire.a` statically behind a safe wrapper | 1.84x / 1.69x (vs rust-protobuf; 1.09x / 1.64x vs prost) |
| C# | Google.Protobuf is already a fast tree codec — the closest race of the set | P/Invoke plus a resolver, no packages | 2.90x / 1.07x (vs Google.Protobuf; 3.6x / 1.62x vs protobuf-net) |
| Go | the official runtime walks `protoreflect`/`protowire` per field | cgo dynamic binding over the C core: one downcall per op, zero-copy decode in, hot-swappable registry | 2.6x / 2.1x (vs official, xlarge; 1.0x / 1.1x small) |
| Erlang | gpb is pure Erlang; gpb NIF binds libprotobuf per message | NIF over the core, consuming gpb's own definitions — no second generator | 4.21x / 1.98x (vs gpb) |
| Lua | game servers hand tables to `pbc`/`lua-protobuf`, each with its own schema story | C module over the core: tables convert through the schema's arena, builders encode hot | 0.79x / 1.20x (vs lua-protobuf; 8.17x via builder encode) |

Read the ratios as "what this binding buys its host language", not as a
cross-language speed ranking: the payload and the boundary cost differ per row,
which is why the at-a-glance table in
[BENCHMARKS.md](../BENCHMARKS.md#at-a-glance-every-binding-one-tier) puts them
side by side with the absolute MB/s.

## Status

| binding | schema source | compliant | note |
|---|---|---|---|
| C | `pbw_struct_schema_load(path)` on a `FileDescriptorSet`; `plugin/` additionally emits structs + layout tables for the compile-time side | ✅ | struct layout is codegen, the schema itself is loaded |
| C++ | the C runtime and its generated headers, unchanged | ✅ | inherits C |
| Python | `pbwire.Schema.from_file/load` on a `FileDescriptorSet` | ✅ | the extension is compiled but the schema is always loaded; `decode()` returns a parsed message, `to_dict()`/`encode(dict)` are the dict path |
| Go | `dynamic.Registry` on a `FileDescriptorSet`, cgo downcalls into the C core | ✅ | explicit `Close` per root (no finalizer); failed loads never disturb the live version |
| Erlang | `gpb_compile` → records + `get_msg_defs/0` → `pbwire:load_cache/1` | ✅ | gpb is the plugin, epb needs no descriptor path |
| Lua | `pbwire.loadfile` on a `FileDescriptorSet` | ✅ | tables in, tables out; arena per schema, builders anchor it |
| Java | `FileDescriptorSet` at run time, own pure-Java reader (JDK 17+) | ✅ | no JNI, no C glue, no generator, no native library; binary-only conformance testee |
| Ruby | `FileDescriptorSet` at run time, a C extension linked against the core | ✅ | no gems, no generator; binary-only conformance testee |
| Node | `FileDescriptorSet` at run time, hand-written N-API addon | ✅ | no npm deps, node headers only |
| Objective-C | `FileDescriptorSet` at run time, ARC wrapper over the C API | ✅ | Foundation only, no codegen |
| Kotlin | the Java binding's runtime: `FileDescriptorSet` at run time, one Java runtime for both | ✅ | four DSL helpers, no second codec |
| PHP | `Pbwire\Schema::fromFile` on a `FileDescriptorSet`, native C extension or `ext-ffi` | ✅ | no composer; the C extension needs only its `.so` |
| Dart | `Schema.load` on a `FileDescriptorSet`, `dart:ffi` | ✅ | `package:ffi` only (malloc, strings) |
| Rust | `Schema::from_file` on a `FileDescriptorSet`, core linked in statically | ✅ | no bindgen, no external crate |
| C# | `Schema.FromFile` on a `FileDescriptorSet`, P/Invoke + `DllImportResolver` | ✅ | no packages in the binding project |

Every row above also has a generated model from `codegen`: typed
fields plus `encode`/`decode` over that binding's dynamic API. C keeps
its own generator (it is ahead-of-time compiled); Go is dynamic-API only
for now; Erlang keeps gpb.

## Memory: what each binding keeps, and the one rule per row

A context's resident ceiling is `retain_cap` (1 MiB default) for the arena
head plus the encode buffer; anything above it is freed on reset instead of
kept (see `../docs/guide.md#reuse-one-arena`). No binding currently exposes
`pbw_ctx_set_retain_cap` or `pbw_pool_*`, so every `pbw_ctx` below runs at
the default cap and the table gives the ceiling plus the workaround.

| binding | what stays resident | ceiling | rule |
|---|---|---|---|
| C / C++ | caller-owned `pbw_struct_buf` (keeps capacity) + `pbw_struct_arena` (each decode resets it) | largest message handled | an arena decode invalidates the previous tree; never `pbw_struct_free` an arena tree (`NULL` arena = per-message malloc + `pbw_struct_free`) |
| Python | own C codec, no `pbw_ctx`; a decoded `Message` pins the whole input buffer | retained `Message`s | drop the `Message` after materializing (`to_dict()` output does not free it) |
| Ruby | builder = C tree, `encode` writes straight into the returned `String`; a wire view pins the bytes | retained views | same as Python: `set` on a view raises, and holding a view holds its bytes |
| Java / Kotlin | one `ThreadLocal` workspace: a reusable encode buffer + a string-image cache, never freed until thread exit | live + dead threads x largest message each | thread-per-request plus one xlarge message pins its buffer per dead thread; keep request threads off the binding or reuse a fixed pool |
| Node | single-threaded: one reusable encode `pbw_ctx` per message type (freed with the type) + decoded message = JS `Buffer` + slice array | types x largest encoding each + retained messages | drop decoded messages; a type's buffer stays at its largest encoding |
| Objective-C / Dart / Rust / C# / Go | plain `pbw_encode`/`pbw_decode` per call (`NSData` takes the malloc'd buffer over with no copy; Go passes the input slice straight in) | none, except retained Go roots | nothing to tune; Go roots need an explicit `Close` |
| PHP | built trees live in one `pbw_ctx` each (freed with the message, never decoded into so always mutable); decoded trees are single-malloc arenas | retained messages | drop the message; nested views hold the parent, `close()` frees either kind |
| Erlang | one `pbw_ctx` per scheduler + a geometrically grown encode buffer, never shrunk (see its README FAQ) | schedulers x largest message | bounded by the scheduler count; keep individual messages reasonably sized |
| Lua | one `pbw_ctx` per schema (arena + encode buffer, reset per call; builders own plain trees) | largest message handled | nothing to tune; idle schemas keep up to the retain cap |

Compiler-level code generation is never hand-written: C's layout tables come
from the same descriptor `protoc` parsed, so field numbers, wire types and
proto2/proto3 semantics are the schema's. C loads the descriptor set at run
time as well (the dynamic core needs it and the struct entry points validate
against it), and Erlang's records are a layout over gpb's definitions.

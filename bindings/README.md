# Bindings

Every binding ships the same pieces (`plugin/` only where the language needs
it): a runtime, examples, tests, benchmarks and a conformance entry.

```
bindings/<lang>/
    runtime/        the binding's codec (rebar3 layout: c_src/ + src/ + priv/)
    plugin/         optional: schema/code generator
    examples/       a runnable round trip, through the typed model
    tests/          self-checks
    benchmarks/     the four standard tiers from ../BENCHMARKS.md
    conformance/    how the official suite covers this binding
```

`bindings/typed/` holds one extra protoc plugin (`protoc-gen-pbwire-typed`) that
emits a **typed message model** for every language from the same descriptor:
fields with the host language's types, names and presence flags, plus
`encode`/`decode` that call the binding's dynamic API. It is pure ergonomics on
top of one wire implementation — see `typed/README.md` for the per-language
shape, the `make typed-gen` / `typed-test` / `typed-conformance` targets, and
the measured cost.

## Where the schema comes from

The rule is the same for all of them, and it decides the design more than
anything else:

- **Compiled languages bake the schema in at build time.** A protoc plugin
  turns the `.proto` into the language's own structs and wire facts, so no
  binding hand-builds a schema. C does that for layout/kind/type columns on
  top of the dynamic core; Go generates straight-line codecs that carry every
  wire fact themselves (`bindings/go/plugin`), which is also where the
  performance comes from.
- **A `FileDescriptorSet` is the runtime schema for the dynamic core and for
  interpreted bindings.** C, C++, the Python extension and the JSON/text paths
  load `protoc --descriptor_set_out` bytes at run time; the C struct entry
  points also validate the generated layout against that schema. Go needs no
  descriptor at run time: its generated code is the complete schema.
- **FFI hosts get the descriptor set and nothing else.** Java, Node and
  Objective-C call the C ABI directly and load a `FileDescriptorSet` at run
  time, so none of them carries a plugin, generated code or a second wire
  implementation. The C core owns the format; the binding owns the ergonomics.
  This is also the cheapest way to add a language.
- **Ruby is a C extension, not an FFI host.** It links the core statically and
  loads a `FileDescriptorSet` at run time like Java/Node/Objective-C, but the
  message conversion happens in C: a builder tree for mutation and encoding, and
  a validating wire scan that materializes fields lazily for decoding. That is
  what removes the per-call boundary cost that made the Fiddle version lose.
- **Languages whose toolchain can call C get the descriptor set, not a plugin.**
  PHP (`ext-ffi`), Dart (`dart:ffi`) and C# (P/Invoke) are one class with Java,
  Node and Objective-C: load the `FileDescriptorSet`, call the C ABI, own no
  wire code. Rust links `libpbwire.a` in statically instead of `dlopen`-ing it,
  which makes the result a single self-contained binary.
- **Kotlin needs no separate layer.** Kotlin runs on the JVM, so it uses the
  Java binding's FFM downcalls and adds only the DSL that official Kotlin support
  is (protoc `--kotlin_out` generates Kotlin over protobuf-java).
- **Erlang needs neither.** gpb already is that plugin: `gpb_compile` /
  `protoc-erl` generates the record definitions plus `get_msg_defs/0`, and
  `enif_protobuf:load_cache/1` consumes exactly that. Adding a second
  generator or a descriptor loader would duplicate gpb.

## What each binding solves

Every row is also the reason that binding exists: its host runtime's status quo
has a specific cost, and the binding removes it *without* a second wire
implementation. The ratios are xlarge-tier throughput against the official
runtime, copied from [BENCHMARKS.md](../BENCHMARKS.md) where the reasoning and
the full tables are.

| binding | the pain it removes | what it does instead | encode / decode vs official |
|---|---|---|---|
| C | dynamic messages cost a libprotobuf dependency, or hand-written wire code per message | struct codec compiled from layout tables over the shared core, schema loaded at run time | 1.47x / 2.42x (vs protoc C++) |
| C++ | generated classes pin the schema at compile time; `DynamicMessage` is 10x slower | the same C runtime and header, no extra layer | 1.46x / 1.43x (vs protoc C++) |
| Python | `google.protobuf` needs a generated module per `.proto`, and dict conversion is seconds at 75 KB | CPython extension: dict↔wire in C, `decode()` returns a parsed message that keeps one slice per wire field | 6.7x / 1.65x (vs upb) |
| Ruby | the `google-protobuf` gem materializes and re-encodes every field; a Fiddle bridge costs ~3 µs per downcall | C extension over the core: builder tree for writes, validating wire view for reads | 1.12x / 1.67x (vs the gem) |
| Java | JNI glue, or protobuf-java re-deriving sizes before every write | `java.lang.foreign` downcalls with one workspace per thread (no JNI, no generator) | 1.68x / 1.31x (vs protobuf-java) |
| Kotlin | official Kotlin is Kotlin over protobuf-java | reuses the Java FFM layer, adds a DSL | 1.61x / 1.26x (vs protobuf-kotlin) |
| Node | jspb resolves a code/type table per field and hands every repeated element to a closure | N-API addon (node headers only), reusable buffer, wire view for decoded messages | 23.8x / 19.0x (vs jspb) |
| Objective-C | the ObjC runtime allocates and copies per field | ARC wrapper: the encoder's buffer is handed to `NSData` with no copy | 1.9x / 2.7x (vs protobuf-objc) |
| PHP | the official runtime is pure PHP, all per-field bookkeeping in the interpreter | `ext-ffi`, with whole buffers crossing the boundary for encode and decode | 392x / 101x (vs protobuf-php) |
| Dart | a pure-Dart field loop dominates, and FFI adds copies | `dart:ffi` plus the core's arena | 14.0x / 3.1x (vs protobuf-dart) |
| Rust | rust-protobuf generated codecs | links `libpbwire.a` statically behind a safe wrapper | 1.60x / 1.47x (vs rust-protobuf) |
| C# | Google.Protobuf is already a fast tree codec — the closest race of the set | P/Invoke plus a resolver, no packages | 2.9x / 1.07x (vs Google.Protobuf) |
| Go | the official runtime walks `protoreflect`/`protowire` per field | `protoc-gen-pbwire-go` emits straight-line codecs: no cgo, no descriptor at run time | 2.85x / 1.69x (vs google.golang.org/protobuf) |
| Erlang | gpb is pure Erlang; gpb NIF binds libprotobuf per message | NIF over the core, consuming gpb's own definitions — no second generator | 2.26x / 2.15x (vs gpb) |

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
| Go | `protoc-gen-pbwire-go` at build time: structs + straight-line size/encode/decode (`bindings/go/plugin`) | ✅ | no cgo, no descriptor at run time; maps/oneofs/groups/extensions rejected |
| Erlang | `gpb_compile` → records + `get_msg_defs/0` → `enif_protobuf:load_cache/1` | ✅ | gpb is the plugin, epb needs no descriptor path |
| Java | `FileDescriptorSet` at run time, `java.lang.foreign` downcalls (JDK 22+) | ✅ | no JNI, no C glue, no generator |
| Ruby | `FileDescriptorSet` at run time, a C extension linked against the core | ✅ | no gems, no generator; binary-only conformance testee |
| Node | `FileDescriptorSet` at run time, hand-written N-API addon | ✅ | no npm deps, node headers only |
| Objective-C | `FileDescriptorSet` at run time, ARC wrapper over the C API | ✅ | Foundation only, no codegen |
| Kotlin | the Java binding's runtime: `FileDescriptorSet` at run time, one FFM layer for both | ✅ | four DSL helpers, no second FFI layer |
| PHP | `Pbwire\Schema::fromFile` on a `FileDescriptorSet`, `ext-ffi` | ✅ | no C extension, no composer |
| Dart | `Schema.load` on a `FileDescriptorSet`, `dart:ffi` | ✅ | `package:ffi` only (malloc, strings) |
| Rust | `Schema::from_file` on a `FileDescriptorSet`, core linked in statically | ✅ | no bindgen, no external crate |
| C# | `Schema.FromFile` on a `FileDescriptorSet`, P/Invoke + `DllImportResolver` | ✅ | no packages in the binding project |

Every row above also has a typed model from `bindings/typed`: generated typed
fields plus `encode`/`decode` over that binding's dynamic API. C and Go keep
their own generators (they are ahead-of-time compiled); Erlang keeps gpb.

Compiler-level code generation is never hand-written: C's layout tables and
Go's codecs both come from the same descriptor `protoc` parsed, so field
numbers, wire types and proto2/proto3 semantics are the schema's. The
difference is where they live -- C loads the descriptor set at run time as
well (the dynamic core needs it and the struct entry points validate against
it), Go bakes the facts into the generated code, and Erlang's records are a
layout over gpb's definitions.

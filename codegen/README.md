# Generated message models

One protoc plugin, one generated model per language. The generated code gives the
host language's compiler (or IDE) something to check — field names, types,
presence and repetition — while every byte still comes from the binding's
existing dynamic codec and, under it, the C core. This layer never implements a
second wire format.

```sh
make codegen            # regenerate the committed models (needs protoc)
make codegen-test           # run every binding's generated test
make codegen-conformance    # generate models for the real conformance proto
```

## What a generated model looks like

Every language gets the same shape, spelled the way that language spells things:

| C (`bindings/c`) | struct | `structtest_scalars3_t`, `structtest_scalars3_encode/decode` |
|---|---|---|
| Java | annotated POJO, your own class shape | `@PbField(1) int i32` / `toByteArray()` / `parseFrom()` / `Map` maps + nullable oneof members + `getKindCase()`, direct Wire codec, no schema; Lombok and validation annotations ride along |
| Kotlin | thin DSL over the Java POJOs | `scalars3 { i32 = 1 }` constructors; types and codec are the Java POJOs |
| C# | `sealed class`, Google.Protobuf shape | `Parser.ParseFrom` / `ToByteArray` / `RepeatedField<T>` / `KindOneofCase` / `Has`+`Clear` for optional, over the embedded schema (no `IMessage`, no `Descriptor`) |
| Dart | class, protobuf-dart shape | `create()..i32 = 1` / `writeToBuffer()` / `fromBuffer()` / `hasX()` / `whichX()` oneof, over the embedded schema |
| PHP | class, protobuf-PHP shape | `getX()/setX()/hasX()/clearX()` + `RepeatedField` + `serializeToString()/mergeFromString()`, over the embedded schema |
| Ruby | class + kwargs, gem-shaped | `Klass.new` / `.encode(msg)` / `.decode(bytes)` / `#to_proto` / `#to_h` / `#has_x?`; schema embedded, zero arguments |
| Python | dataclass, official `_pb2` shape | `i32: int = 0` + `.SerializeToString()` / `.FromString()` / `.CopyFrom()` / `.HasField()`; schema embedded, zero arguments |
| Node | protobufjs-static shape | `Cls.create()` / `.encode(m).finish()` / `.decode()` / `.verify()` / `.fromObject()` / `.toObject()`; schema embedded, `.d.ts` included |
| Rust | struct, prost shape | `pub i32: i32` + `pub opt_i32: Option<i32>` + oneof `Option<Kind>` + `i32` enums + `encode_to_vec` / `decode` over the embedded schema |
| Lua | table + constructor (no dominant lib; T1–T4 only) | `M.Scalars3.new{i32 = 1}` + `encode(t)` / `decode(wire)` with embedded schema (legacy `(schema, …)` form kept) |
| Go | struct, protoc-gen-go shape | `I32 int32` + `OptI32 *int32` + oneof wrappers + nil-safe getters + `Marshal`/`Unmarshal` over the embedded schema |
| Objective-C | GPBMessage-shaped | `@property` fields + `hasX` + `XArray` repeated + `Kind_OneOfCase` + `-data` / `+parseFromData:error:`, over the embedded schema |
| Swift | struct, Swift-idiomatic | `var i32: Int64` + `optI32: Int64?` + `Int64` enums + `encode()` / `decode(_:)` over the embedded schema |
| Zig | struct over the static-link runtime | `i32`/`u64` fields + `?T` presence + `enum(i32)` enums + `encode` / `decode` / `load` over the embedded schema |
| Scala | case class over the Java runtime | `copy(name = ...)`, `Option[T]` presence, `Int` enums, `encode` / `decode` over the embedded schema |
| Clojure | map + constructor fns over the interop runtime | `(tutorial-person :name "x")` / `encode-tutorial-person` / `decode-tutorial-person` over the embedded schema |
| F# | record over the C# runtime | `{ name = ... }`, `T option` presence, `int` enums, `encode` / `decode` over the embedded schema |
| Elixir | `elixir-protobuf` modules (the binding's schema source) | `field(:name, 1, ...)` / `%Tutorial.Person{}` + `Pbwire.encode` / `Pbwire.decode`; no embedded schema — the modules are the schema |
| Julia | mutable struct over the ccall runtime | `field::Int64` + `Union{Nothing,T}` presence + `to_dict` / `from_dict` + `encode` / `decode` over the embedded schema |
| R | named-list constructors over the C-extension runtime | `Tutorial_Person_new(...)` + `Tutorial_Person_encode` / `Tutorial_Person_decode` over the embedded schema |

C is not in this table twice: the C plugin already generates the
struct layer.
Erlang gets records from gpb (`get_msg_defs/0`), which is its existing
schema source; adding a second generator there would duplicate gpb.
Go is dynamic-API only for now (its emitter exists and the addressbook
example uses it, but `make codegen` does not yet refresh its matrix3
models).

Where a pinned oracle exists (every row with a shape above), the model follows
that library's conventions instead — including its presence spelling, enum
storage and oneof API. The rules below are the default for languages without
a dominant library, which every facade still honors where its oracle agrees:

Rules the model follows, per field:

- **proto3 implicit fields** default to the zero value and are left off the wire
  when zero (an empty string, an empty list, a zero enum).
- **Explicit presence** (proto2 optional, proto3 `optional`, oneof members) gets
  a `has<Field>` boolean next to the value, exactly like the C struct binding.
  A present zero stays on the wire; an absent field stays absent.
- **`required`** fields get no `has` flag: they are always written.
- **repeated** fields are the language's list type; empty means absent.
- **messages** are nullable references (`None`/`null`/`nil`/`Option`), so a
  cycle is expressible in every language that has pointers.
- **enums** are integer constants plus a constant holder. The field stays
  an integer so an unknown value from the wire round-trips unchanged.
- **maps** ride as repeated entry structs (one array of `{key, value}` per
  map field, the same shape the dynamic API reads).
- **message fields whose type lives in another file** import the sibling
  generated file (same directory; Go takes an explicit `M<path>=<importpath>`
  mapping, protoc-gen-go style) and qualify the reference with the defining
  file's prefix. Unresolvable types still skip loudly (`// skipped <field>:`).
  This keeps each generated file to one import per referenced file.

## Where the models live

The models are committed next to the tests that use them, so a test run needs
no protoc. Each file's schema travels beside its models — either a second
generated file or a per-file registry module — never a `.desc` file:

```
bindings/java/tests/gen/        Structtest_Scalars3.java, ... + per-file *_Registry.java
bindings/kotlin/tests/gen/      matrix3.kt (consumes the Java runtime)
bindings/csharp/generated/gen/      matrix3.cs + PbwireFile_Matrix3.cs
bindings/dart/tests/gen/        matrix3.dart + matrix3_meta.dart
bindings/php/tests/gen/         matrix3.php + PbwireFile_Matrix3.php
bindings/ruby/test/gen/         matrix3.rb + matrix3_meta.rb
bindings/python/tests/gen/      matrix3_pbwire.py + matrix3_pbwire_meta.py
bindings/node/tests/gen/        matrix3.d.ts, matrix3.js + matrix3_meta.js
bindings/go/tests/gen/matrix3/  matrix3_pbwire.go + matrix3_pbwire_meta.go (package matrix3)
bindings/rust/tests/gen/        matrix3_pbwire.rs + matrix3_pbwire_meta.rs
bindings/objc/tests/gen/        matrix3.h, matrix3.m + matrix3_schema.h, matrix3_schema.m
bindings/lua/tests/gen/         matrix3.lua + matrix3_meta.lua
bindings/swift/Tests/PbwireTests/gen/  matrix3.swift + matrix3_meta.swift (SPM layout: inside the test target)
bindings/zig/tests/gen/        matrix3.zig + matrix3_meta.zig
bindings/scala/tests/gen/      matrix3.scala + matrix3_meta.scala
bindings/clojure/tests/gen/    matrix3.clj + matrix3_meta.clj
bindings/fsharp/tests/gen/     matrix3.fs + matrix3_meta.fs
bindings/elixir/test/gen/      matrix3.ex (Registry module appended in-file)
bindings/julia/tests/gen/      matrix3_pbwire.jl + matrix3_meta.jl
bindings/r/tests/gen/          matrix3_pbwire.R + matrix3_meta.R
bindings/*/examples/gen/        addressbook.* (the C example's schema;
                                Swift's lives in Sources/Roundtrip/gen: SPM
                                compiles only its target dirs)
```

`make codegen` rewrites all of them from `bindings/c/tests/data/matrix3.proto`
and `examples/addressbook.proto`.

## Codegen options

`--pbwire_opt=model=bool,register=bool,registry=bool` (all default `true`)
means the same thing in every language:

- `model=false` leaves the schema sidecar for dynamic-only users — the
  message files are not emitted.
- `register=false` drops the lazy load: an explicit init (embedded bytes)
  or `use_schema` (an external schema, dual registration) must run first,
  failing loudly otherwise.
- `registry=false` drops the whole-file message list + preload. The list is
  pure data, so `registry=true` is the default: open-ended registration
  with no runtime cost.

Two languages deviate, and only there: elixir embeds no descriptor set
(the modules are the schema), so there is no second file and `register` is
a no-op; r keeps one dotted holder per file instead of the old single
shared global.

## The tests

`make codegen-test` runs one test per binding. Each does the same thing, which is
the integration test this layer exists for:

1. assign the `matrix3.txt` values through the generated model only,
2. encode, and require the bytes to equal `matrix3.bin` (the pinned `protoc`
   output every binding already uses as its oracle),
3. decode that reference and check every field, including presence flags,
4. check that an empty generated message encodes to nothing and that an explicit
   zero with presence survives.

Node and Python go further: `tests/compat_test.js` and
`tests/test_compat.py` implement the shared modes T1–T6 from
`testdata/TESTSPEC.md` against `testdata/vectors.json` — wire equality,
decode checks, differential tests against the pinned对照 libraries
(`testdata/ORACLES.md`), a zero-change business snippet that runs
against both the对照 library and our facade, and loud error paths.
New languages copy that pattern instead of inventing their own.
The eight newer bindings (Swift, Zig, Scala, Clojure, F#, Elixir, Julia,
R) start with the byte-equality generated test above; the T1–T6
compat expansion is future work (R documents its own `shortcut:` —
doubles hold integers exactly only to 2^53, so its test uses moderate
values instead of the extreme `matrix3.bin` bytes).

`bindings/*/examples/` run the same flow on the C example's `addressbook.proto`
and print the address book; every language produces the same 65 bytes
(`examples/addressbook.bin`).

`make codegen-conformance` generates models for
`google/protobuf/test_messages_proto3.proto` — the schema the official
conformance suite uses: maps, oneofs, deep nesting and every scalar kind. The
generated code must compile in every binding (it is the hardest schema the
generator meets); the value flow is covered by the matrix3 tests.
Swift/Zig/R/Julia/Elixir check in with a parse (their compilers need a
package build the suite does not do); Scala and F# do a real compile;
Clojure loads its namespace (needs the fetched jars, like `test-clojure`).

## Cost

The generated layer is ergonomics, not a second hot path: encode/decode call the
binding's dynamic API, except Java, whose POJOs write straight into a Wire
buffer through reflection over the annotated fields (no Message tree, no
schema at use time). Positions measured on this machine with `matrix3`
x3000 (~33 KB) and the same values through both paths:

| language | generated encode vs dynamic | generated decode vs dynamic |
|---|---|---|
| C (struct binding) | 1.00x (it *is* the struct path) | 1.00x |
| Java (reflective POJOs, direct Wire codec) | 1.7x slower | 1.0x |
| Python (dataclass ⇄ dict) | 3.8x slower | 3.2x slower |

Like Python, Java's typed path trades encode speed for ergonomics (your own
classes, Lombok, validation); the dynamic `Message` path stays the fast path.

Python pays for the dataclass ⇄ dict conversion; the extension's dict path is
the fast path and stays the one the Python benchmark measures. For hot loops in
the other bindings, keep a `Message`/`Type` and reuse it (`schema.message(...)`
is resolved once per call otherwise).

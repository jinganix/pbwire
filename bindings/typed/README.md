# Typed message models

One protoc plugin, one typed model per language. The generated code gives the
host language's compiler (or IDE) something to check — field names, types,
presence and repetition — while every byte still comes from the binding's
existing dynamic codec and, under it, the C core. This layer never implements a
second wire format.

```sh
make typed-gen            # regenerate the committed models (needs protoc)
make typed-test           # run every binding's typed test
make typed-conformance    # generate models for the real conformance proto
```

## What a generated model looks like

Every language gets the same shape, spelled the way that language spells things:

| C (`bindings/c`) | typed struct | `structtest_scalars3_t`, `structtest_scalars3_encode/decode` |
|---|---|---|
| Java | class per message | `public int i32; public String str; public List<Inner3> inners;` + `encode(Schema)` / `static decode(Schema, byte[])` |
| Kotlin | `data class` | same fields as `var`, `fun encode(schema)`, `companion object { fun decode(...) }` |
| C# | `sealed class` | `public int I32;` `public List<Inner3> Inners;` + `Encode(Schema)` / `static Decode` |
| Dart | class | `int i32 = 0;` `List<Inner3> inners = [];` + `encode(schema)` / `static decode` |
| PHP | `final class` | typed properties `public int $i32;` + `encode(Schema)` / `static decode` |
| Ruby | class + kwargs | `attr_accessor :i32` + `#encode(schema)` / `.decode(schema, wire)` |
| Python | dataclass | `i32: int = 0` + `#encode(msg_type)` / `.decode(msg_type, wire)` |
| Node | `.d.ts` + JSDoc | interfaces for the plain objects `MessageType.encode()` accepts |
| Rust | struct | `pub i32: i32` + `encode(&Schema)` / `decode(&Schema, &[u8])` |
| Objective-C | NSObject subclass | `@property int32_t i32;` + `-encodeWithSchema:error:` / `+decodeWithSchema:data:error:` |

C is not in this table twice: the C plugin already generates the typed
struct layer. Go is dynamic-API only and has no typed model for now.
Erlang gets typed records from gpb (`get_msg_defs/0`), which is its existing
schema source; adding a second generator there would duplicate gpb.

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
- **enums** are integer constants plus a typed constant holder. The field stays
  an integer so an unknown value from the wire round-trips unchanged.
- **maps** and **message fields whose type lives in another file** are left out
  of the typed model (`// skipped <field>: ...`); the dynamic API still handles
  them. This keeps the generated code dependency-free.

## Where the models live

The models are committed next to the tests that use them, so a test run needs
no protoc:

```
bindings/java/tests/gen/        Structtest_Scalars3.java, ...
bindings/kotlin/tests/gen/      matrix3.kt
bindings/csharp/typed/gen/      matrix3.cs
bindings/dart/tests/gen/        matrix3.dart
bindings/php/tests/gen/         matrix3.php
bindings/ruby/test/gen/         matrix3.rb
bindings/python/tests/gen/      matrix3_pbwire.py
bindings/node/tests/gen/        matrix3.d.ts, matrix3.js
bindings/rust/tests/gen/        matrix3_pbwire.rs
bindings/objc/tests/gen/        matrix3.h, matrix3.m
bindings/*/examples/gen/        addressbook.* (the C example's schema)
```

`make typed-gen` rewrites all of them from `bindings/c/tests/data/matrix3.proto`
and `examples/addressbook.proto`.

## The tests

`make typed-test` runs one test per binding. Each does the same thing, which is
the integration test this layer exists for:

1. assign the `matrix3.txt` values through the typed model only,
2. encode, and require the bytes to equal `matrix3.bin` (the pinned `protoc`
   output every binding already uses as its oracle),
3. decode that reference and check every field, including presence flags,
4. check that an empty typed message encodes to nothing and that an explicit
   zero with presence survives.

`bindings/*/examples/` run the same flow on the C example's `addressbook.proto`
and print the address book; every language produces the same 65 bytes
(`examples/addressbook.bin`).

`make typed-conformance` generates models for
`google/protobuf/test_messages_proto3.proto` — the schema the official
conformance suite uses: maps, oneofs, deep nesting and every scalar kind. The
generated code must compile in every binding (it is the hardest schema the
generator meets); the value flow is covered by the matrix3 tests.

## Cost

The typed layer is ergonomics, not a second hot path: encode/decode call the
binding's dynamic API. Positions measured on this machine with `matrix3`
x3000 (30 KB) and the same values through both paths:

| language | typed encode vs dynamic | typed decode vs dynamic |
|---|---|---|
| C (struct binding) | 1.00x (it *is* the struct path) | 1.00x |
| Java | 1.00x | 1.09x |
| Python (dataclass ⇄ dict) | 3.8x slower | 3.2x slower |

Python pays for the dataclass ⇄ dict conversion; the extension's dict path is
the fast path and stays the one the Python benchmark measures. For hot loops in
the other bindings, keep a `Message`/`Type` and reuse it (`schema.message(...)`
is resolved once per call otherwise).

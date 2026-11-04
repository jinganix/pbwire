# pbwire Dart binding

`bindings/dart/lib/pbwire.dart` is the Dart binding: `dart:ffi` over the C ABI in
[`include/pbwire.h`](../../include/pbwire.h). A `FileDescriptorSet`
(`protoc --descriptor_set_out`) becomes the schema at run time; there is no
generated code and no C glue. The wire format lives in the C core, so a Dart
message and a C message are the same bytes by construction.

```dart
import 'package:pbwire/pbwire.dart';

final schema = Schema.load('tests/data/matrix3.desc');
final ty = schema.message('structtest.Scalars3');
final innerTy = schema.message('structtest.Inner3');

final msg = ty.newMessage()
  ..setInt('i32', -42)
  ..setString('str', 'alice');
for (final tag in ['a', 'bb']) {
  msg.setString('tags', tag);                       // repeated: appends
}
msg.setMessage('inner')                             // nested: created in place
  ..setString(innerTy.field('name'), 'kid')
  ..setInt('value', 42);

final wire = msg.encode();
final decoded = ty.decode(wire);                    // unknown fields preserved
assert(decoded.at('tags', 1)?.asString() == 'bb');
assert(_equals(decoded.encode(), wire));            // byte-exact round trip
```

Fields are addressed by name (`'i32'`) or by a `Field` handle from
`ty.field('i32')`; the handle skips the name lookup and is what a hot loop should
reuse.

## Requirements

The Dart SDK (3.x) and `make shared` (`build/libpbwire.dylib` / `.so`).
`PBWIRE_LIB` points at the library; otherwise it is found by walking up from the
working directory or the entry script. The only package dependency is
`package:ffi`, for `malloc` and `toNativeUtf8`/`toDartString`.

## Build and run

```shell
dart pub get
dart run tests/selftest.dart                        # self-check vs pinned protoc bytes
dart run examples/roundtrip.dart                    # example
```

## Benchmarks

```shell
cd benchmarks && dart pub get && ./bench.sh         # vs the protobuf Dart runtime, four tiers
```

Needs `dart pub global activate protoc_plugin` (it provides `protoc-gen-dart`;
add `~/.pub-cache/bin` to `PATH`). Numbers live in
[BENCHMARKS.md](../../BENCHMARKS.md).

Strings cross as NUL-terminated C strings, so `setString()` stops at the first
NUL byte: use `setBytes()` for binary payloads (a `string` field accepts them
too, which is how protobuf's wire format works).

## Ownership and threads

A `Schema` and a root `Message` own native memory: `close()` frees it, and a
`Finalizer` catches the ones that are not closed. A `Type` keeps its `Schema`
(and a `Message` its `Type`) reachable, so garbage collection cannot free the
schema under a type or message; an explicit `close()` is still the caller's
promise that nothing else uses it. A nested message is a view of its parent,
holds it alive, and never frees anything. `dart:ffi` state is per-isolate, and a
message tree is not shared: one tree belongs to one isolate, like the C core.

## Typed models

`protoc-gen-pbwire-typed` (in `../typed`) generates Dart classes with typed
fields, `Uint8List encode(schema)` and `static decode`. The committed output is
`tests/gen/` and `examples/gen/`; `examples/roundtrip.dart` assigns typed fields
only. See `../typed/README.md`.

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
add `~/.pub-cache/bin` to `PATH`). The four tiers are small / medium / large /
xlarge (the repeated fields scaled x0 / x1 / x100 / x3000). Copied from
[BENCHMARKS.md](../../BENCHMARKS.md), which is the single source for all
performance numbers; higher is better, MB/s over wire bytes:

| payload | op | pbwire-dart (MB/s, x protobuf-dart) | protobuf-dart (MB/s) |
|---|---|---|---|
| 136 B | encode | 783.4 (3.6x) | 217.5 |
| 136 B | decode | 401.6 (1.9x) | 206.6 |
| 174 B | encode | 689.2 (3.8x) | 179.2 |
| 174 B | decode | 411.7 (2.2x) | 187.3 |
| 2469 B | encode | 1237.0 (11.5x) | 107.3 |
| 2469 B | decode | 624.1 (4.4x) | 141.3 |
| 74913 B | encode | 1430.7 (15.4x) | 92.8 |
| 74913 B | decode | 694.0 (4.3x) | 162.4 |

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

## Generated models

`protoc-gen-pbwire` (in `../../codegen`) generates Dart classes with typed
fields in protobuf-dart shape (`create`, `writeToBuffer`/`fromBuffer`,
`hasX`/`clearX`/`ensureX`, `whichX` for oneofs, `ProtobufEnum` enums, `Int64`
for 64-bit fields) over the dynamic API, with the `FileDescriptorSet`
embedded: no `.desc` file at use time, and the wire stays in the C core. The
committed output is `tests/gen/` (plus the protoc-gen-dart oracle in
`tests/gen_oracle/`, pinned in `pubspec.lock`) and `examples/gen/`;
`examples/roundtrip.dart` assigns typed fields only. Each file splits in two:
`<base>.dart` (facades) and `<base>_meta.dart` (the embedded descriptor set
plus its loader, imported by the models file).
`--pbwire_opt=lang=dart[,model=bool][,register=bool][,registry=bool]`
(all default `true`): `model=false` leaves the schema library for
dynamic-only users; `register=false` drops the lazy per-isolate load so
`initSchema()` (embedded bytes) or `useSchema()` (an external `Schema`,
dual registration) must run first; `registry=false` drops `messageNames` +
`preload()`. See `../../codegen/README.md`
and `../../codegen/testdata/TESTSPEC.md` (T1–T6, run by `tests/generated_test.dart`).

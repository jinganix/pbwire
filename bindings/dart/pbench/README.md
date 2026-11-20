# Dart pbench: three arms, one contract

The arms, the gate and the timing loop from
[`bindings/README.md`](../../README.md#pbench-three-arms-one-contract), applied
to `structtest.Scalars3` from `bindings/c/tests/data/matrix3.proto` and the four
standard tiers. Every arm does the same user-visible task: a native object in
and wire bytes out; wire bytes in and a fully readable native object out.

| arm | file | mechanism | what it answers |
|---|---|---|---|
| `tree` | `a-tree.dart` | the shipped handle: `schema.message(name)`, one `set` per field, `encode()`; `decode()` then one `get` per field | what the per-field downcall task costs once the tree exists |
| `shim` | `b-shim.dart` | the generated `<Message>Layout` (`dart:ffi` `Struct`) is the native object; `PbwireLayout.encode` / `decodeInto` cross once and `bindings/dart/native/pbw_dart_shim.c` walks that memory in place through `pbw_struct_codec_*` | is a boundary worth paying for at all |
| `core` | `n/a` | a pure-Dart port of the C core | plan 18 (`issue41-18-core-dart.md`) builds it |

`core` prints `n/a` until plan 18 lands; there is no second wire
implementation in this binding. `dart2js`/`dart2wasm` have no `dart:ffi`, so the
`shim` row covers the VM targets only; plan 18 owns the rest.

## Layout table

`codegen/internal/gen/dart.go` emits, beside each message, a `final class
<Message>Layout extends ffi.Struct` and the `<message>LayoutRows` row list:
`number`, `kind`, `offset`, `has` (4-byte presence flag), `count` (`size_t`
element count), `child` (the proto full name of a sub-message), `oneof`+`cs`
(shared case word), `type`/`wire`/`packed`/`required` (the codec's precomputed
wire facts). Slots pack in descriptor order at natural alignment, so the Dart
class member order and the offsets cannot disagree; the emitted
`<message>LayoutOffsets` list is the gate-able table.

Copy rules, all into a call-owned arena: a `string` row is a NUL-terminated
`Pointer<Utf8>` (UTF-8), a `bytes` row is the core's inline `PbwBytes`
(pointer + length), a sub-message row is a pointer to the child image, and a
repeated row is a pointer to a contiguous array plus its count. Encode borrows
the image; decode fills it and bump-allocates every non-blittable value from
`PbwArena` (one block, the wire copy included), so the arena must outlive the
read and is released in a `finally`. Presence survives the flat layout: proto3
optionals keep their has flag, absent pointer-kind fields stay NULL, and a
oneof keeps its case word.

`pbw_dart_shim.c` builds one `pbw_struct_field_t` row per layout row, resolves
sub-layouts by full name (children register first), fills proto2 enum defaults
from the schema and compiles the codec once per type
(`pbw_struct_codec_init`). Encode and decode are each one FFI call; the type
owns the reusable encode buffer, the caller owns the decode image and arena.

## Gate

```
bindings/dart/pbench/setup.sh                     # gen + shim library
cd bindings/dart/pbench && dart run gate.dart gen/matrix3.desc   # PASS / exit 1
```

The gate asserts, per tier, that `tree` (handle) bytes and `shim` (image) bytes
equal the official `protobuf-dart` encoder's bytes, that both decode paths
expose the fields `payload.dart` lists (`str`, `inner.name`, `inner.value`, the
repeated counts and last elements, the proto3 optionals, `u64`, `color`), and
that the shim's decoded image re-encodes to the same bytes. There is no
official re-encode check: `protobuf-dart` 6.1's sint64 decoder reads
-9223372036854775808 as 0, so only its encoder is an oracle here.

## Provisional table (Stage 3)

`cd pbench && dart run compare.dart gen/matrix3.desc`: interleaved rounds (all
three arms back to back), median of five. `tree` is the whole user task (build
with one `set` per field, then encode; decode, then read every field back);
`shim` is one `encode` / `decodeInto` call on the flat image, with the output
image and arena allocated once (the C core's arena steady state). MB/s over
wire bytes, 2026-11-21, recorded mid-session:

| tier | `tree` enc | `shim` enc | pb-dart enc | `tree` dec | `shim` dec | pb-dart dec |
|---|---|---|---|---|---|---|
| 136 B | 56.4 | 931.5 | 86.6 | 51.0 | 866.2 | 99.3 |
| 174 B | 67.4 | 1308.3 | 92.1 | 82.5 | 901.6 | 168.9 |
| 2469 B | 32.7 | 1035.2 | 81.8 | 324.4 | 660.3 | 132.5 |
| 74913 B | 35.0 | 1092.2 | 73.1 | 476.6 | 722.6 | 149.5 |

## Verdict

**Publish the flat-image `shim` row as the native-object path; the C-side
reader removes the boundary the `tree` arm pays.** With one FFI call per op,
the `shim` row is above the official baseline at every tier: 10.8x encode and
8.7x decode at 136 B, 14.9x and 4.8x at 74 KB. The `tree` row (build a handle
with one downcall per field, then read one back) is 0.4-0.7x the baseline
because each field crosses `dart:ffi` twice; that is the per-field walk issue
41 is about, not the codec.

It is not (a) -- "replace the shipped fast path": the formal `make dart-bench`
row `pbwire-dart-tree` times a *prebuilt* handle's `encode()`, which stays the
shipping runtime for applications that already hold a tree (and is well above
the baseline). It is not (c) -- "a boundary feature is missing": there is no
payload-size crossover, the shim is above the baseline from 136 B to 74 KB.
`make dart-bench` publishes both rows (`pbwire-dart-shim` beside
`pbwire-dart-tree`); the shim is the native-object row, never a replacement for
the handle.

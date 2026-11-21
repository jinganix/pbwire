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
| `core` | `c-core.dart` | the pure-Dart core (`package:pbwire/core/`): one `set` per field on a `Msg` tree, then `encode()`; `decode()` then one `get` per field | what the per-field boundary costs when there is none, and the only arm that runs where `dart:ffi` does not exist |

The `core` arm is `bindings/dart/lib/core/`, a pure-Dart port of the C core
(plan 18, `issue41-18-core-dart.md`). `dart2js`/`dart2wasm` have no `dart:ffi`,
so the `shim` row covers the VM targets only and the `core` row covers the
rest. There is still no second wire implementation in this binding: `tree` and
`shim` reach the same C core, and `core` is the portable Dart one.

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
four arms back to back), median of five. `tree` and `core` are the whole user
task (build with one entry per field, then encode; decode, then read every
field back); `shim` is one `encode` / `decodeInto` call on the flat image, with
the output image and arena allocated once (the C core's arena steady state).
MB/s over wire bytes, 2026-11-21, recorded mid-session:

| tier | `tree` enc | `shim` enc | `core` enc | pb-dart enc | `tree` dec | `shim` dec | `core` dec | pb-dart dec |
|---|---|---|---|---|---|---|---|---|
| 136 B | 80.5 | 1462.4 | 35.0 | 99.6 | 82.8 | 1022.6 | 18.1 | 197.4 |
| 174 B | 72.5 | 1370.1 | 39.7 | 96.0 | 87.5 | 925.5 | 20.7 | 178.1 |
| 2469 B | 35.4 | 1075.8 | 111.6 | 90.8 | 355.0 | 686.0 | 66.1 | 139.8 |
| 74913 B | 36.9 | 1099.7 | 149.4 | 82.5 | 509.7 | 748.0 | 91.8 | 156.2 |

The `core` arm beats protobuf-dart's generated codec on encode from 2469 B up
and loses the small tiers and every decode cell to per-call allocation: one
`Msg` with a 22-slot list, a `fromObject` walk and a workspace copy per encode,
one `Msg` per nested message and one growable list per repeated field per
decode. It is the only arm that runs without a C library, which is the point.

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

Stage 6 (optimization) is a recorded no-op: no published Dart cell is below
its baseline (the prebuilt handle and the flat image both beat `protobuf-dart`
at every tier, see `make dart-bench`), so no A/B round was run and the
attempts ledger gains no entry.

### The `core` arm: portable, not a replacement

**The FFI runtime stays the VM's path; the pure core is the arm for the
targets that have no C library at all (dart2js, dart2wasm, a host that cannot
ship a dynamic library).** `make dart-bench` publishes it as
`pbwire-dart-core` beside the two FFI rows, same payload, same tiers: 0.34x /
0.33x the baseline at 136 B, 0.37x / 0.42x at 174 B, 3.11x / 1.19x at 2469 B
and 4.82x / 1.32x at 74 KB (encode / decode, against `protobuf-dart`). The
codec itself is fast where there is work to amortize -- it beats the generated
encoder from 2469 B up -- and pays per message on the small tiers: one `Msg`
with a 22-slot list per message, a growable `List` per repeated field, and a
dynamic slot lookup per field where the generated reader writes a compiled
field. The whole-task row in the provisional table above (`core` against
`tree`, both building the object and reading every field back) is 0.35x / 0.09x
at 136 B and 1.81x / 0.59x at 74 KB: the same codec, with the build and the
field reads inside the timer.

So it is (b) in the pbench contract -- a host without a native library -- and
never (a): the README's install path keeps the handle, and the shim stays the
native-object row. Stage 6 for this arm is recorded, not chased: the
below-baseline cells are the small tiers and are per-message allocation plus
the dynamic slot loop, and closing them means a columnar/lazy tree or code
generated for the concrete message -- a rewrite of the tree and both codecs,
which is what the FFI runtime already is.

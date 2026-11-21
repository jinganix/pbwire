# Dart pbench: three arms, one contract

The arms, the gate and the timing loop from
[`bindings/README.md`](../../README.md#pbench-three-arms-one-contract), applied
to `structtest.Scalars3` from `bindings/c/tests/data/matrix3.proto` and the four
standard tiers. Every arm does the same user-visible task: a native object in
and wire bytes out; wire bytes in and a fully readable native object out.

| arm | file | mechanism | what it answers |
|---|---|---|---|
| `tree` | `a-tree.dart` | the shipped handle full task: a fresh `Message` per iteration, one `set` per field, `encode()`, `close()`; a fresh `decode()`, one `get` per field into a live sink, `close()` | what the per-field downcall task costs end to end |
| `proxy` | `benchmarks/bench.dart` | the prebuilt task only: a held handle's `encode()`, one-shot decode-and-close with no materialization (the old `pbwire-dart-tree` shape, relabelled not re-measured) | the transcoder task, published, never the headline |
| `shim` | `b-shim.dart` | the generated `<Message>Layout` (`dart:ffi` `Struct`) is the native object; `PbwireLayout.encode` / `decodeInto` cross once and `bindings/dart/native/pbw_dart_shim.c` walks that memory in place through `pbw_struct_codec_*` | is a boundary worth paying for at all |
| `core` | `c-core.dart` | the pure-Dart core (`package:pbwire/core/`): one `set` per field on a `Msg` tree, then `encode()`; `decode()` then one `get` per field | what the per-field boundary costs when there is none, and the only arm that runs where `dart:ffi` does not exist |

The `shim` and `core` timers reuse an app-owned native input across timed
iterations (the flat image / the `Msg` tree, built once per tier): harness
allocator steady state is not codec cost, so only the `tree` row rebuilds its
native object inside the timer. The reuse is disclosed here and in the
`bench.dart` header; every row still frees what the timed op allocates (the
decode image and arena are released in a `finally`).

The `core` arm is `bindings/dart/lib/core/` a pure-Dart port of the C core
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

## Verdict (Go parity, 2026-11-21)

**Ship the flat-image `shim` row as the native-object path; publish `tree`
(full task), `core`, and `proxy` beside it, never headline `proxy`.** Formal
`make dart-bench` (`results/bench-dart-20261121T185927Z.json`), xlarge
(74913 B) THROUGHPUT ratios (pbwire MB/s over protobuf-dart MB/s, higher is
faster): `tree` 0.50x encode / 4.57x decode, `shim` 10.98x / 4.81x, `core`
4.78x / 1.42x, `proxy` (prebuilt) 15.41x / 4.66x. Across tiers vs
protobuf-dart, `tree` encode 0.46x/0.49x/0.39x/0.50x and `tree` decode
0.45x/0.52x/3.19x/4.57x (small/medium/large/xlarge): the per-field FFI walk
costs more than the codec on encode at every tier, while decode wins once the
payload amortizes the crossings. The proxy/tree encode gap is 8.8x/10.4x/
22.5x/30.8x -- the gap the old prebuilt `tree` row was hiding.

It is not the prebuilt handle: applications that already hold a tree keep the
`proxy` shape (well above the baseline), but a native object in and out pays
the `tree` row. It is not a replacement for the handle: the `shim` row is the
native-object path because one FFI call per op removes the per-field boundary
the `tree` arm pays, above the baseline at every tier (encode 6.6x-11.0x,
decode 4.8x-4.9x). The `core` arm stays the path for targets with no C
library (dart2js, dart2wasm): above the baseline from 2469 B up (encode
2.51x/4.78x, decode 1.29x/1.42x), below it on the small tiers (encode
0.32x/0.40x, decode 0.37x/0.45x) from per-message allocation plus the dynamic
slot loop.

Below-baseline native cells flagged for the optimization pass (no optimization
here): `tree` encode every tier, `tree` decode 136/174 B, `core` encode/decode
136/174 B. Gate: `pbench/gate.dart` 36/36 plus the bench full-task gate 4/4
tiers (fresh-build bytes + field compare + re-encode on 136/174/2469/74913 B);
`make dart-test` PASS, `make conformance-dart` 439 successes 0 unexpected,
`make conformance-dart-core` 0 unexpected, `./scripts/check.sh` OK.

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

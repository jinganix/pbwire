# Dart pbench: three arms, one contract

The arms, the gate and the timing loop from
[`bindings/README.md`](../../README.md#pbench-three-arms-one-contract), applied
to `structtest.Scalars3` from `bindings/c/tests/data/matrix3.proto` and the four
standard tiers. Every arm does the same user-visible task: a native object in
and wire bytes out; wire bytes in and a fully readable native object out.

| arm | mechanism | what it answers |
|---|---|---|
| `tree` | the prebuilt `Message` handle: `schema.message(name)`, one `set` per field, `encode()`; `decode()` then one `get` per field | what the codec costs once the tree exists |
| `shim` | the generated `<Message>Layout` (`dart:ffi` `Struct`) is the native object; `PbwireLayout.encode` / `decodeInto` cross once and `bindings/dart/native/pbw_dart_shim.c` walks that memory in place through `pbw_struct_codec_*` | is a boundary worth paying for at all |
| `core` | `n/a` — a pure-Dart port of the C core | plan 18 (`issue41-18-core-dart.md`) builds it |

`core` prints `n/a` until plan 18 lands; there is no second wire
implementation in this binding. `dart2js`/`dart2wasm` have no `dart:ffi`, so the
`shim` row covers the VM targets only; plan 18 owns the rest.

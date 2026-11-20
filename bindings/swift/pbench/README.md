# Swift pbench: three arms, one contract

The arms, the gate and the timing loop from
[`bindings/README.md`](../../README.md#pbench-three-arms-one-contract), applied
to `structtest.Scalars3` from `bindings/c/tests/data/matrix3.proto` and the four
standard tiers. Every arm does the same user-visible task: a native object in
and wire bytes out; wire bytes in and a fully readable native object out.

| arm | file | mechanism | what it answers |
|---|---|---|---|
| `tree` | `Sources/Pbench/Arms.swift` | the shipped `Message` handle: one `set` per field, then `encode()`; `decode()` then one `get` per field | what the per-field downcall costs once the tree exists |
| `shim` | `Sources/Pbench/Arms.swift` | the generated `@frozen <Message>Repr` is the native object; `Pbwire.InPlace.encodeInPlace` / `decodeInPlace` cross once and `bindings/swift/Sources/CPbwire/shim.c` walks that memory in place through `pbw_struct_codec_*` | is a boundary worth paying for at all |
| `core` | `n/a` | a pure-Swift port of the C core | plan 21 (`issue41-21-core-swift.md`) builds it |

`core` prints `n/a` until plan 21 lands; there is no second wire
implementation in this binding.

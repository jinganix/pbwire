# Rust pbench: three arms, one contract

The arms, the gate and the timing loop from
[`bindings/README.md`](../../README.md#pbench-three-arms-one-contract), applied
to `structtest.Scalars3` from `bindings/c/tests/data/matrix3.proto` and the four
standard tiers. Every arm does the same user-visible task: a native object in
and wire bytes out; wire bytes in and a fully readable native object out.

| arm | file | mechanism | what it answers |
|---|---|---|---|
| `tree` | `src/arms.rs` | the shipped handle: `schema.message(name)`, one `set_*` per field, `encode_to()` into a reused buffer; `decode()` then one `get_*` per field | what the per-field native call costs once the tree exists |
| `shim` | `src/arms.rs` | the generated `<Message>Flat` (`#[repr(C)]`) is the native object; `Type::encode_in_place` / `Type::decode_in_place` cross once into `bindings/c/runtime/pbw_struct.c`, which walks that memory in place through `pbw_struct_codec_*` | is a boundary worth paying for at all |
| `core` | `n/a` | a pure-Rust port of the C core | plan 20 (`issue41-20-core-rust.md`) builds it |

`core` prints `n/a` until plan 20 lands; there is no second wire implementation
in this binding.

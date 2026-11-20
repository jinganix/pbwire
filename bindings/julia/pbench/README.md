# Julia pbench: three arms, one contract

The arms, the gate and the timing loop from
[`bindings/README.md`](../../README.md#pbench-three-arms-one-contract), applied
to `structtest.Scalars3` from `bindings/c/tests/data/matrix3.proto` and the four
standard tiers. Every arm does the same user-visible task: a native object in
and wire bytes out; wire bytes in and a fully readable native object out.

| arm | file | mechanism | what it answers |
|---|---|---|---|
| `tree` | `a-tree.jl` | the shipped `Dict`/`Message` handle: `to_dict`, `encode_dict`; `decode_dict`, `from_dict` | what the codec costs once the tree exists |
| `shim` | `b-shim.jl` | the generated `mutable struct <Message>Layout` image is the native object; `Pbwire.encode_layout` / `decode_layout` cross once and `bindings/julia/native/pbw_julia_shim.c` walks that memory in place through `pbw_struct_codec_*` | is a boundary worth paying for at all |
| `core` | `n/a` | a pure-Julia port of the C core | plan 19 (`issue41-19-core-julia.md`) builds it |

`core` prints `n/a` until plan 19 lands; there is no second wire
implementation in this binding.

# Ruby pbench: two arms, one contract

The arms, the gate and the timing loop from
[`bindings/README.md`](../../README.md#pbench-three-arms-one-contract), applied
to `structtest.Scalars3` from `bindings/c/tests/data/matrix3.proto` and the four
standard tiers. Every arm does the same user-visible task: a native object (a
generated model instance) in and wire bytes out; wire bytes in and a fully
readable generated model out.

| arm | file | mechanism | what it answers |
|---|---|---|---|
| `tree` | `a-tree.rb` | the shipped model facade: `Structtest_Scalars3.encode(obj)` builds a `Message` with one `set` per field, then encodes; `.decode(wire)` reads one `get` per field back | what the per-field downcall task costs once the model exists |
| `shim` | `b-shim.rb` | `type.encode_object(obj)` / `type.decode_object(wire, classes)`: one C-extension entry walks the Ruby object's instance variables in C and fills the C tree (encode) or materializes the generated models (decode) | is a boundary worth paying for at all |
| `core` | `n/a` | a pure-Ruby port of the C core | deferred: no pure-Ruby precedent, no native-less deployment |

`core` prints `n/a` until a native-less Ruby target appears; there is no second
wire implementation in this binding.

TODO in `lib/pbwire.rb`: `shortcut: no pure-Ruby core, reopen when a
native-less Ruby target appears`.

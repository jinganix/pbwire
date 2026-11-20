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

## Gate

```
make ruby-bind                              # the extension
ruby bindings/ruby/pbench/gate.rb           # PASS / exit 1
```

The gate asserts, per tier, that `tree` (model facade) bytes and `shim`
(`encode_object`) bytes equal the google-protobuf gem's bytes, that each arm's
decode reads the fields `benchmarks/bench.rb` checks (`str`, `inner.name`,
`inner.value`, the repeated lengths and last element, the proto3 optionals,
`u64`, `color`), and that each decoded object re-encodes to the same bytes.

## Timing

`compare.rb` interleaves `a-tree`, `b-shim` and the google-protobuf gem in one
loop per tier and reports the median of five rounds
(`ruby bindings/ruby/pbench/compare.rb`). The formal, recorded run is
`make ruby-bench`, which prints `pbwire-ruby-tree` (the prebuilt handle row)
beside `pbwire-ruby-shim` (the native-object row).

## Run table

Provisional (Stage 3, `ruby pbench/compare.rb`, median of five interleaved
rounds, 2026-11-21): `tree` is the whole user task (build the generated model,
then encode; decode, then read every field back), `shim` is one `encode_object`
/ `decode_object` call. The prebuilt-handle row `make ruby-bench` publishes as
`pbwire-ruby-tree` is a different op and is not in this table.

| tier | arm | encode ns/op | encode MB/s | decode ns/op | decode MB/s |
|---|---|---|---|---|---|
| 136 B | `pbwire-ruby-tree` | 2965.2 | 45.9 | 3890.8 | 35.0 |
| 136 B | `pbwire-ruby-shim` | 1005.2 | 135.3 | 978.2 | 139.0 |
| 136 B | `google-protobuf` | 3092.7 | 44.0 | 536.0 | 253.7 |
| 174 B | `pbwire-ruby-tree` | 3986.7 | 43.6 | 4517.7 | 38.5 |
| 174 B | `pbwire-ruby-shim` | 1330.5 | 130.8 | 1153.7 | 150.8 |
| 174 B | `google-protobuf` | 5554.3 | 31.3 | 677.9 | 256.7 |
| 2469 B | `pbwire-ruby-tree` | 63202.0 | 39.1 | 62872.2 | 39.3 |
| 2469 B | `pbwire-ruby-shim` | 19727.9 | 125.2 | 17928.7 | 137.7 |
| 2469 B | `google-protobuf` | 137183.6 | 18.0 | 5160.1 | 478.5 |
| 74913 B | `pbwire-ruby-tree` | 1832896.0 | 40.9 | 1631700.0 | 45.9 |
| 74913 B | `pbwire-ruby-shim` | 571371.5 | 131.1 | 487625.0 | 153.6 |
| 74913 B | `google-protobuf` | 4145257.5 | 18.1 | 111526.0 | 671.7 |

The reading: the per-field facade (`tree`) is ~3x slower than one object walk
at every tier, and it loses to the gem on encode at 136 B-174 B because it pays
a Ruby-level `set` plus a C `pbw_message_set` per field. The shim removes that
Ruby-side loop: encode is 3.0x the gem at 136 B and 7.3x at 74 KB. Decode is
the opposite story: the shim materializes every generated model and array in
C, while the gem's decode leaves fields lazy, so `shim` decode is 0.5x the gem
at 136 B and 0.23x at 74 KB. Stage 6 looks at that cell.

## Verdict

**Keep the builder/wire-view handle as the shipped runtime; the shim is the
native-object row and a re-runnable candidate.** One C-extension entry removes
the Ruby-side per-field loop that the `tree` arm pays: `shim` encode is above
the google-protobuf gem at every tier (3.0x at 136 B, 7.3x at 74 KB). Decode
does not win: the contract asks for a fully materialized generated model, while
the gem's decoder leaves fields lazy, so the shim's per-element Ruby object and
array allocation costs 2.0x-4.4x more than the baseline. Publishing both rows
keeps the codec number and the native-object number visible instead of quoting
one for the other.

It is not (a) -- "replace the shipped fast path": the runtime's fast path is the
prebuilt `Message` builder (encode) and wire view (decode), which this arm does
not replace; replacing it with a full object materialization would slow every
decode. It is not (c) -- "split by payload size": there is no crossover. The
shim wins encode at 136 B and at 74 KB, and loses decode at both ends.

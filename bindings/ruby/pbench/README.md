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
| 136 B | `pbwire-ruby-tree` | 4340.2 | 31.3 | 4131.1 | 32.9 |
| 136 B | `pbwire-ruby-shim` | 1704.4 | 79.8 | 1028.7 | 132.2 |
| 136 B | `google-protobuf` | 283.8 | 479.3 | 564.1 | 241.1 |
| 174 B | `pbwire-ruby-tree` | 4059.7 | 42.9 | 4700.7 | 37.0 |
| 174 B | `pbwire-ruby-shim` | 1332.3 | 130.6 | 1192.0 | 146.0 |
| 174 B | `google-protobuf` | 222.5 | 782.0 | 713.6 | 243.8 |
| 2469 B | `pbwire-ruby-tree` | 64830.9 | 38.1 | 59692.1 | 41.4 |
| 2469 B | `pbwire-ruby-shim` | 19002.1 | 129.9 | 17205.6 | 143.5 |
| 2469 B | `google-protobuf` | 2065.6 | 1195.3 | 4994.0 | 494.4 |
| 74913 B | `pbwire-ruby-tree` | 1779491.0 | 42.1 | 1601363.0 | 46.8 |
| 74913 B | `pbwire-ruby-shim` | 562203.5 | 133.2 | 488371.0 | 153.4 |
| 74913 B | `google-protobuf` | 57537.7 | 1302.0 | 111440.0 | 672.2 |

The reading: the per-field facade (`tree`) loses to the gem on both ops because
it pays a Ruby-level `set`/`get` plus a C tree call per field. One C-extension
entry removes the Ruby-side loop: `shim` is 2.5x-3.1x the `tree` arm on encode
and 2.5x-4.0x on decode. It still loses to the gem, whose generated object is
already a C-backed value: the shim must build a pbw tree from Ruby ivars
(encode) or materialize every generated model and array (decode), while the gem
writes/reads its own storage and leaves decode lazy. Stage 6's arena change cut
the xlarge shim encode from 133.2 to 253.5 MB/s, but did not close the gap.

## Verdict

**Keep the builder/wire-view handle as the shipped runtime; the shim is the
native-object row and a re-runnable candidate.** The formal `make ruby-bench`
row `pbwire-ruby-tree` (prebuilt `Message`/wire view) is 903.7 / 789.7 / 1531.2
/ 1804.9 MB/s encode and 356.7 / 457.7 / 933.3 / 1207.8 MB/s decode, above the
gem at every tier. The native-object `shim` row is 181.0 / 197.2 / 220.7 /
253.5 encode and 138.5 / 153.4 / 142.6 / 158.6 decode: one extension call
removes the Ruby per-field loop (2.5x-3.1x the `tree` arm), but the object walk
still builds a C tree / materializes Ruby objects where the gem reads and
writes its own C-backed storage. Publishing both rows keeps the codec number
and the native-object number visible instead of quoting one for the other.

It is not (a) -- "replace the shipped fast path": the runtime's fast path is the
prebuilt `Message` builder (encode) and wire view (decode), which this arm does
not replace; replacing it with a full object materialization would slow every
decode. It is not (c) -- "split by payload size": there is no crossover. The
shim is below the gem on both ops at 136 B and at 74 KB.

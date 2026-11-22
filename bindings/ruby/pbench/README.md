# Ruby pbench: three arms plus proxy, one contract

The arms, the gate and the timing loop from
[`bindings/README.md`](../../README.md#pbench-three-arms-one-contract), applied
to `structtest.Scalars3` from `bindings/c/tests/data/matrix3.proto` and the four
standard tiers. Every arm does the same user-visible task: a native object (a
generated model instance) in and wire bytes out; wire bytes in and a fully
readable generated model out.

| arm | file | mechanism | what it answers |
|---|---|---|---|
| `tree` | `a-tree.rb` | the shipped model facade as the full task: `Structtest_Scalars3.encode(obj)` builds a `Message` with one `set` per field, then encodes; `.decode(wire)` reads one `get` per field back into a fresh model. The model is built fresh per iteration and every gate-checked field is read back inside the timer; no prebuilt handle before it. Ruby's GC owns the model (no manual free exists on this path) | what an application pays through the per-field API |
| `shim` | `b-shim.rb` | `type.encode_object(obj)` / `type.decode_object(wire, classes)`: one C-extension entry walks the Ruby object's instance variables in C and fills the C tree (encode) or materializes the generated models (decode). Encode reuses the app-owned model across iterations (steady-state carve-out, said here and in the harness header) | is a boundary worth paying for at all |
| `core` | `n/a` | a pure-Ruby port of the C core | deferred: no pure-Ruby precedent, no native-less deployment |
| `proxy` | the prebuilt `Message` in `benchmarks/bench.rb` | re-encode a held handle, decode-and-free with no materialization | the codec's own cost once the tree exists -- published, never the headline |

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
(`ruby bindings/ruby/pbench/compare.rb`): the `tree` side builds a fresh model
per iteration and reads every gate-checked field back. The formal, recorded
run is `make ruby-bench`, which prints `pbwire-ruby-tree` (the per-field full
task), `pbwire-ruby-shim` (the native-object row) and `pbwire-ruby-proxy`
(the prebuilt hold-and-recode row, relabelled not re-measured as a new task).

## Run table

Formal (`make ruby-bench`, `results/bench-ruby-20261122T015616Z.json`,
interleaved rounds, best wins; throughput ratios = pbwire MB/s over
google-protobuf MB/s):

| tier | arm | encode ns/op | vs gem enc | decode ns/op | vs gem dec |
|---|---|---|---|---|---|
| 136 B | `tree` (full task) | 3501.5 | 0.057x | 5104.8 | 0.103x |
| 136 B | `shim` (one downcall) | 709.6 | 0.279x | 933.2 | 0.562x |
| 136 B | `proxy` (prebuilt) | 149.1 | 1.33x | 371.9 | 1.41x |
| 174 B | `tree` (full task) | 4803.6 | 0.045x | 5762.7 | 0.111x |
| 174 B | `shim` (one downcall) | 868.5 | 0.251x | 1107.0 | 0.580x |
| 174 B | `proxy` (prebuilt) | 206.2 | 1.06x | 353.7 | 1.82x |
| 2469 B | `tree` (full task) | 76881.4 | 0.026x | 55704.3 | 0.089x |
| 2469 B | `shim` (one downcall) | 10846.1 | 0.184x | 16420.5 | 0.302x |
| 2469 B | `proxy` (prebuilt) | 1550.2 | 1.29x | 2557.3 | 1.94x |
| 74913 B | `tree` (full task) | 2087081.0 | 0.027x | 1447584.0 | 0.076x |
| 74913 B | `shim` (one downcall) | 300491.5 | 0.186x | 470472.0 | 0.232x |
| 74913 B | `proxy` (prebuilt) | 43312.9 | 1.29x | 62833.5 | 1.74x |

The reading: the per-field facade (`tree`) loses to the gem on both ops at
every tier because it pays a Ruby-level `set`/`get` plus a C tree call per
field, plus a fresh model build per iteration. One C-extension entry removes
the Ruby-side loop: `shim` is 4.9x-7.1x the `tree` arm on encode and
3.1x-5.5x on decode. It still loses to the gem, whose generated object is
already a C-backed value: the shim must build a pbw tree from Ruby ivars
(encode) or materialize every generated model and array (decode), while the gem
writes/reads its own storage and leaves decode lazy. Proxy/tree gap at
xlarge: 48.2x encode, 23.0x decode -- the gap the old prebuilt `tree` row was
hiding. Proxy decode stays near gem decode shape (1.4x-1.9x): decode was
faster than the gem even before materialization; the materialization is what
costs.

## Verdict

**Keep the builder/wire-view handle as the shipped runtime; the shim is the
recommended native-object path.** The `proxy` row (prebuilt `Message`/wire
view) is 1.06x-1.33x the gem on encode and 1.41x-1.94x on decode, above the
gem at every tier: that stays the fast path for holders of a tree. Of the
native-in/native-out arms, `shim` is the fastest on every tier on both ops
(0.184x-0.279x / 0.232x-0.580x vs gem) and the simplest -- one extension call
removes the Ruby per-field loop. `tree` stays published beside it as the
per-field-API reference (0.026x-0.057x / 0.076x-0.111x), never replaced by
`proxy`. `core` stays `n/a`: no pure-Ruby port, no native-less target.
Below-baseline native cells flagged for Task 15 (`tree` encode/decode all
tiers, `shim` encode/decode all tiers); no optimization here.

It is not (a) -- "replace the shipped fast path": the runtime's fast path is
the prebuilt `Message` builder (encode) and wire view (decode), which neither
native arm replaces; replacing it with a full object materialization would
slow every decode. It is not (c) -- "split by payload size": there is no
crossover. Both native arms are below the gem on both ops at 136 B and at
74 KB.

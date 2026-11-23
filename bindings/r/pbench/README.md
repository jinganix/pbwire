# R pbench: three arms plus proxy, one contract

The arms, the gate and the timing loop from
[`bindings/README.md`](../../README.md#pbench-three-arms-one-contract), applied
to `structtest.Scalars3` from `bindings/c/tests/data/matrix3.proto` and the four
standard tiers. Every arm does the same user-visible task: a native object in
and wire bytes out; wire bytes in and a fully readable native object out.

| arm | file | mechanism | what it answers |
|---|---|---|---|
| `tree` | `a-tree.R` | the shipped builder handle as the full task: `tree_encode_full(list)` builds a fresh `pbwire_message` + `pbwire_message_set` per encode (R has no per-field setter, so `set()` walks every named field in C, see `pbwr_fill` in `runtime/src/pbwire_r.c`), then `pbwire_message_encode()`; `tree_decode_full(wire)` parses with `pbwire_parse_binary()` and reads fully back with `pbwire_msg_to_json()` (R builders have no per-field getters by design, so the JSON/text print IS the shipped read-back path; the decode folds its `nchar` into a sink). The list feeding the encode is app-owned steady-state input (Task 5 ledger ruling, Python worker D3 merged as 99ac9ac, whose review accepted plain-data dict reuse as "standard harness practice"); the fresh builder, the whole C field walk and the encode all sit inside the timer. R's GC owns the builder (no manual free exists on this path) | what an application pays through the per-field API |
| `shim` | `b-shim.R` | the generated model's shape, `Structtest_Scalars3_encode(list)` / `_decode(wire)`: one `.Call` hands the named list (or the wire bytes) to C, which walks it and fills the tree inside C and returns the raw vector (or the fully materialized named list). The type is resolved once into a schema-owned message number (`pbwire_type_number`), so the per-call work is a bounds check, not a name lookup. Encode reuses the app-owned list across iterations (steady-state carve-out, said here and in the harness header). Decode carries no R-side folding sink by design: `pbwr_decode_number` runs `pbwr_decode_desc`, which returns `pbwr_message_sexp(msg, s, 0)` -- that walk builds every repeated R vector element by element and converts every scalar before returning (`runtime/src/pbwire_r.c`), so the full R list already exists when the timer drops the reference | is a boundary worth paying for at all |
| `core` | `n/a` | a pure-R port of the C core | deferred: a pure-R engine loops per field in the interpreter, orders of magnitude off, and no user needs it |
| `proxy` | the prebuilt builder in `benchmarks/bench.R` | re-encode a held handle (`pbwire_message_encode`), decode-and-drop with no materialization (`pbwire_parse_binary`) | the codec's own cost once the tree exists -- published, never the headline |

`core` prints `n/a` until a native-less R target appears; there is no second
wire implementation in this binding.

TODO in `runtime/R/pbwire.R`: `shortcut: no pure-R core, reopen when a
native-less R target appears`.

## Gate

```
make r-bind                                    # the extension
Rscript bindings/r/pbench/gate.R               # PASS / exit 1
```

The gate asserts, per tier, that `tree` (fresh builder per encode) bytes and
`shim` (one-`.Call` list path) bytes equal RProtoBuf's bytes, that `tree`
decodes into a builder tree that prints exactly like the source tree (every
field, the shipped read-back) and re-encodes byte for byte, that `shim`
decodes
into the named list `benchmarks/bench.R` reads field by field (`i32`, `i64`,
`u32`, `f32`, `f64`, `str`, `color`, `inner.name`, `inner.value`, the repeated
lengths and last elements, the proto3 optionals, `u64`), that it re-encodes to
the same bytes, and that it equals RProtoBuf's own materialization (the same
plain-list read the baseline is timed on) field for field.

Two R spellings the gate keeps instead of hiding: `INT32_MIN` reads back as `NA`
(it *is* `NA_INTEGER`), so the payload carries `INT32_MIN + 1` like
`benchmarks/bench.R`; and an R double cannot spell `uint64`/`int64` past 2^53, so
`u64` is compared through the byte-exact round trip, not by value.

## Timing

`compare.R` interleaves `a-tree`, `b-shim` and RProtoBuf in one loop per tier and
reports the median of five rounds (`Rscript bindings/r/pbench/compare.R`): the
`tree` side builds a fresh builder per encode and folds the full JSON
read-back into a sink per decode. The formal, recorded run is `make r-bench`,
which prints `pbwire-r-tree` (the per-field full task), `pbwire-r-shim` (the
native-object row) and `pbwire-r-proxy` (the prebuilt hold-and-recode row,
relabelled not re-measured as a new task).

## Run table

Formal (`make r-bench`, `results/bench-r-20261122T020549Z.json`, best of 3;
throughput ratios = pbwire MB/s over RProtoBuf MB/s; all rows same run --
`proxy` is same-run timing only, never the headline):

| tier | arm | encode ns/op | vs RProtoBuf enc | decode ns/op | vs RProtoBuf dec |
|---|---|---|---|---|---|
| 136 B | `tree` (full task) | 4695.0 | 11.53x | 6245.0 | 8.01x |
| 136 B | `shim` (one downcall) | 2415.0 | 22.42x | 1820.0 | 27.47x |
| 136 B | `proxy` (prebuilt) | 496.3 | 109.11x | 1515.0 | 33.00x |
| 174 B | `tree` (full task) | 4455.0 | 12.76x | 5835.0 | 9.74x |
| 174 B | `shim` (one downcall) | 1610.0 | 35.30x | 1720.0 | 33.04x |
| 174 B | `proxy` (prebuilt) | 473.7 | 119.97x | 1380.0 | 41.18x |
| 2469 B | `tree` (full task) | 27450.0 | 34.82x | 37050.0 | 22.79x |
| 2469 B | `shim` (one downcall) | 17700.0 | 54.00x | 18050.0 | 46.77x |
| 2469 B | `proxy` (prebuilt) | 2362.5 | 404.57x | 6075.0 | 138.96x |
| 74913 B | `tree` (full task) | 691000.0 | 38.32x | 1099000.0 | 25.63x |
| 74913 B | `shim` (one downcall) | 479500.0 | 55.23x | 472000.0 | 59.67x |
| 74913 B | `proxy` (prebuilt) | 46250.0 | 572.59x | 150000.0 | 187.77x |

The reading: R's native object is the named list, so the `shim` row is the
task an application actually runs (list in, wire out, one `.Call`, the whole
walk in C) -- 22-60x RProtoBuf on every tier, both ops. The `tree` full task
pays the fresh builder plus the `set()` walk on encode and the parse plus the
JSON read-back on decode, so it trails `shim` by 1.4x-2.8x -- but it still
beats RProtoBuf by 8-38x everywhere, because RProtoBuf pays R-level per-field
work on top of its own C++ call. Proxy/tree gap at xlarge: 14.9x encode,
7.3x decode -- the gap the old prebuilt `tree` row was hiding (same-run timing only). Proxy decode
stays far above RProtoBuf (33x-188x): decode was faster than RProtoBuf even
before materialization; the materialization is what costs.

## Verdict

**Keep the shipped runtime and publish all three rows; `shim` is the
recommended native-object path.** `pbwire-r-shim` is the native-object row R
users actually run -- a named list in, wire bytes out, the whole walk in C
behind one `.Call` -- and it is the fastest native-in/native-out arm on every
tier, both ops (22.42x-55.23x / 27.47x-59.67x vs RProtoBuf). `pbwire-r-tree`
is the per-field-API reference (fresh builder per encode, parse plus full
read-back per decode), published beside it, 8.01x-38.32x RProtoBuf -- never
replaced by `proxy`. The `proxy` row (prebuilt builder) stays the form a hot
loop should hold (109x-573x / 33x-188x vs RProtoBuf, same-run timing only), published never the
headline.

It is not (a) -- "replace the shipped fast path": the list path is 1/1.4 to
1/2.8 of the full-task builder path, and the prebuilt builder is another
4x-15x above that, because it skips the list walk and the read-back entirely;
replacing the builder with either would slow every `pbwire_msg_encode` caller.

It is not (c) -- "split by payload size": the native arms are above
RProtoBuf at all four tiers on both ops (8x at 136 B, 26x-38x at 74913 B),
and there is no crossover between the two native arms at any size -- `shim`
wins by the same 1.4x-2.8x everywhere.

`core` stays `n/a`: a pure-R port would loop per field in the interpreter,
orders of magnitude off, and no native-less R target exists (TODO in
`runtime/R/pbwire.R`). No Task 15 cell: every native arm is above the
baseline on every tier, so there is nothing to optimize here.

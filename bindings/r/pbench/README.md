# R pbench: two arms, one contract

The arms, the gate and the timing loop from
[`bindings/README.md`](../../README.md#pbench-three-arms-one-contract), applied
to `structtest.Scalars3` from `bindings/c/tests/data/matrix3.proto` and the four
standard tiers. Every arm does the same user-visible task: a native object in
and wire bytes out; wire bytes in and a fully readable native object out.

| arm | file | mechanism | what it answers |
|---|---|---|---|
| `tree` | `a-tree.R` | the shipped builder handle: `pbwire_message()` + one `pbwire_message_set()` outside the timer, then `pbwire_message_encode()`; `pbwire_parse_binary()` for wire -> tree, readable with `pbwire_msg_to_json`/`_to_text` | what the codec costs once the tree exists |
| `shim` | `b-shim.R` | the generated model's shape, `Structtest_Scalars3_encode(list)` / `_decode(wire)`: one `.Call` hands the named list (or the wire bytes) to C, which walks it and fills the tree inside C and returns the raw vector (or the fully materialized named list). The type is resolved once into a schema-owned message number (`pbwire_type_number`), so the per-call work is a bounds check, not a name lookup | is a boundary worth paying for at all |
| `core` | `n/a` | a pure-R port of the C core | deferred: a pure-R engine loops per field in the interpreter, orders of magnitude off, and no user needs it |

`core` prints `n/a` until a native-less R target appears; there is no second
wire implementation in this binding.

TODO in `runtime/R/pbwire.R`: `shortcut: no pure-R core, reopen when a
native-less R target appears`.

## Gate

```
make r-bind                                    # the extension
Rscript bindings/r/pbench/gate.R               # PASS / exit 1
```

The gate asserts, per tier, that `tree` (builder handle) bytes and `shim`
(one-`.Call` list path) bytes equal RProtoBuf's bytes, that `tree` decodes into
a builder tree that re-encodes byte for byte and prints, that `shim` decodes
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
reports the median of five rounds (`Rscript bindings/r/pbench/compare.R`). The
formal, recorded run is `make r-bench`, which prints `pbwire-r-shim` (the
one-`.Call` list path) beside `pbwire-r-tree` (the prebuilt builder).

## Provisional table (Stage 3)

`Rscript bindings/r/pbench/compare.R`, median of five interleaved rounds,
2026-11-21, MB/s over wire bytes. `tree` encode is the prebuilt builder handle
and `tree` decode is `pbwire_parse_binary` (wire -> builder tree); `shim` is one
`.Call` with the named list in or out; RProtoBuf is the baseline, materialized
into the same plain list on decode.

| tier | arm | encode | decode |
|---|---|---|---|
| 136 B | `pbwire-r-tree` | 226.4 | 81.4 |
| 136 B | `pbwire-r-shim` | 63.7 | 52.1 |
| 136 B | RProtoBuf | 1.6 | 2.5 |
| 174 B | `pbwire-r-tree` | 265.2 | 97.9 |
| 174 B | `pbwire-r-shim` | 71.0 | 60.8 |
| 174 B | RProtoBuf | 1.9 | 2.8 |
| 2469 B | `pbwire-r-tree` | 949.0 | 437.3 |
| 2469 B | `pbwire-r-shim` | 133.5 | 118.3 |
| 2469 B | RProtoBuf | 2.4 | 3.8 |
| 74913 B | `pbwire-r-tree` | 1606.5 | 485.5 |
| 74913 B | `pbwire-r-shim` | 155.3 | 121.4 |
| 74913 B | RProtoBuf | 2.1 | 2.9 |

The reading: R's native object is the named list, so the `shim` row is the task
an application actually runs (list in, wire out), while `tree` needs a builder
the application had to keep. One `.Call` keeps that task 60-90x RProtoBuf at
74913 B; the builder row is 3.5-10x the list row because it skips the list walk
and the named-list materialization entirely -- that is the builder's price, not
something the shim can recover without a tree of its own.

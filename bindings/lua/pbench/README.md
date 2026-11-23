# Lua pbench: three arms, one contract

The arms, the gate and the timing loop from
[`bindings/README.md`](../../README.md#pbench-three-arms-one-contract), applied
to `structtest.Scalars3` from `bindings/c/tests/data/matrix3.proto` and the four
standard tiers. Every arm does the same user-visible task: a native object (a
plain Lua table, proto field names, Lua integers for 64-bit fields) in and wire
bytes out; wire bytes in and a fully readable native object out. Both timed
encode arms build the input table fresh per iteration (no prebuilt table before
the timer), so construction is shared, not headlined, on both sides -- stricter
than the steady-state carve-out the contract allows, disclosed here.

| arm | file | mechanism | what it answers |
|---|---|---|---|
| `tree` | `a-tree.lua` | the shipped per-field full task through the builder handle: fresh `message()` + `set()` + `encode()` per call; decode is `parse_binary()` plus the full read-back as JSON text (`to_json()`, the shipped read-back path -- builders have no per-field getters by design, the same split the R arm documents) | what an application pays through the per-field API |
| `shim` | `b-shim.lua` | `schema:encode(tbl)` / `schema:decode(wire)`: one C entry per op, the addon walks the Lua table against the schema's prepared field table in C and writes straight into the wire buffer (encode) or materializes the fresh table (decode, documented-eager: the C walk builds every field before it returns) | is a boundary worth paying for at all |
| `core` | `n/a` | a pure-Lua port of the C core | deferred: no pure-Lua precedent, no native-less Lua target |
| `proxy` | the prebuilt builder in `benchmarks/bench.lua` | re-encode a held handle (`built:encode()`), `roundtrip()` with no materialization | the codec's own cost once the tree exists -- published, never the headline |

`core` prints `n/a` until a native-less Lua target appears; there is no second
wire implementation in this binding.

`Payload` is `payload.lua` (one table shared by the gate and the timing loop),
the oracle is lua-protobuf 0.6.0 on the same `.proto`. lua-protobuf writes
nested messages in table hash order, so raw baseline bytes are not comparable:
the baseline only has to agree on size and survive a decode spot-check, like
`benchmarks/bench.lua`.

## Gate

```
sh bindings/lua/build.sh            # the module (or: make lua-bind)
lua bindings/lua/pbench/gate.lua    # gate: PASS / exit 1
```

The gate asserts, per tier, that `tree` and `shim` bytes equal each other and
the tier size (136/174/2469/74913 B, canonical and protoc-identical), that the
shim decode reads every field `benchmarks/bench.lua` checks and re-encodes to
the same bytes, that the tree decode (JSON) prints like the source tree and
re-encodes to the same bytes, and the unknown-field vectors: field 999
top-level and nested inside `inner` round-trip byte-identical through the
builder path (`parse_binary` + `encode`), re-encode canonically through the
table path (unknowns skipped by design), and survive `roundtrip()`.

One documented approximation the gate pins rather than hides: the `u64` max
decodes to the nearest double (`2^64`) on every path -- the value mapping the
binding README documents -- so the re-encode compares normalize it back to the
`-1` bit pattern first.

Known gap (pinned, not gated): invalid UTF-8 in a proto3 string field encodes
without error on both paths (probed) -- out of scope here, conformance is green
regardless, a core-validation task should be filed separately.

## Timing

`compare.lua` interleaves `a-tree`, `b-shim` and lua-protobuf in one loop per
tier and reports the median of five rounds
(`lua bindings/lua/pbench/compare.lua`): both encode arms build a fresh table
per iteration. The formal, recorded run is `make lua-bench`, which prints
`pbwire-lua-tree` (the per-field full task), `pbwire-lua-shim` (the one-entry
native-object row) and `pbwire-lua-proxy` (the prebuilt hold-and-recode row).

## Run table

Formal (`make lua-bench`, `results/bench-lua-20261122T160929Z.json`,
interleaved, best of 3 passes wins; throughput ratios = pbwire MB/s over
lua-protobuf MB/s, same run):

| tier | arm | encode MB/s (x baseline) | decode MB/s (x baseline) |
|---|---|---|---|
| 136 B | `tree` (full task) | 73.3 (0.80x) | 66.4 (0.73x) |
| 136 B | `shim` (one entry) | 102.5 (1.13x) | 147.0 (1.61x) |
| 136 B | `proxy` (prebuilt) | 985.2 (10.82x) | 480.8 (5.26x) |
| 174 B | `tree` (full task) | 72.0 (0.78x) | 70.5 (0.77x) |
| 174 B | `shim` (one entry) | 102.2 (1.11x) | 142.8 (1.56x) |
| 174 B | `proxy` (prebuilt) | 1017.7 (11.01x) | 459.5 (5.01x) |
| 2469 B | `tree` (full task) | 64.0 (0.67x) | 85.1 (0.98x) |
| 2469 B | `shim` (one entry) | 99.8 (1.04x) | 131.6 (1.52x) |
| 2469 B | `proxy` (prebuilt) | 1480.3 (15.50x) | 456.8 (5.26x) |
| 74913 B | `tree` (full task) | 65.3 (0.61x) | 97.0 (1.03x) |
| 74913 B | `shim` (one entry) | 104.1 (0.97x) | 134.6 (1.43x) |
| 74913 B | `proxy` (prebuilt) | 1768.1 (16.49x) | 524.2 (5.57x) |

Shim over tree, same run: 1.40x/1.42x/1.56x/1.59x encode and
2.21x/2.03x/1.55x/1.39x decode. The provisional `compare.lua` runs agree
(shim 1.4-1.6x encode, 1.4-2.2x decode over tree on both runs). The reading:
the tree arm pays a malloc builder plus the per-field table->tree convert on
top of the same core walk the shim does straight from the table (encode), and a
second JSON print on top of the same parse the shim materializes straight into
tables (decode). Proxy/tree at xlarge: 27.1x encode, 5.4x decode -- the gap the
old prebuilt-`encode_msg` headline was hiding.

The xlarge `shim` encode cell (0.97x) is a tie, not a finding: it flips between
runs (1.04x-1.13x at the other tiers, 1.05x-1.06x for xlarge in the provisional
runs) inside the host's ±10% run-to-run spread, and both sides share the same
fresh-table construction inside the timer.

## Verdict

**Ships: the shim. Its C entry (`schema:encode` / `schema:decode` direct
table paths in `runtime/pbwire.c`) is already the shipped path, so this
selection places nothing new.** The one-entry surface beats the per-field
builder assembly on every tier on both ops (1.40x-1.59x encode,
1.39x-2.21x decode) and beats lua-protobuf on every cell but the xlarge-encode
tie (1.04x-1.13x / 1.43x-1.61x), so one-shot encode/decode of a fresh table
goes through the direct paths; they stay the default because there is nothing
to promote -- the comparison only confirms the shipped shape. `tree` stays
published beside it as the per-field-API reference (0.61x-0.80x /
0.73x-1.03x): it is the path holders of a builder tree already pay (game
broadcasts: build once, `encode()` per send -- the `proxy` row, 985-1768 MB/s),
the exact-64-bit path (`parse_binary` skips tables), and the JSON/text surface.
`core` stays `n/a`: no pure-Lua port, no native-less target.

It is not (b) -- "promote the builder": the `proxy` row (prebuilt handle
re-encode, no materialization on the `roundtrip` decode) is 10.8x-16.5x the
baseline on encode and 5.0x-5.6x on decode, but it times data already in
pbwire, not what a fresh-table application pays. It is not (c) -- "split by
payload size": there is no crossover, the shim leads at 136 B and at 74 KB.
The one perf round (decode single-lookup-loop, `docs/perf-attempts.md`) was
rejected at +0.6-1.7%, inside the noise floor: both hot paths sit at the Lua
C-API floor (one `rawget` per known field plus one irreducible `lua_next`
unknown scan per message on encode).

# Benchmark fairness: full-flow native in/out

This page is the fairness contract behind [BENCHMARKS.md](../BENCHMARKS.md).
Read BENCHMARKS.md first for the standard cases, the harness names and the
current numbers; this page only states what makes a row comparable. It extends
(does not replace) the Output format / Timer boundary / Correctness-first
rules there, and applies to every `make *-bench` harness and every row pasted
into BENCHMARKS.md.

## 1. Why

A binding exists to move host objects across the wire. A number is only
meaningful if it times that whole trip: host-native value in, codec call,
wire bytes out (encode) / wire bytes in, codec call, host-native value out
(decode). Timing a held core handle (`Message`/tree/`Decoder` built before
the timer, re-encode or bare decode-and-drop) skips the host<->core
crossing and measures the C core against itself. That is the `proxy` task:
published, never the headline.

## 2. The full-flow rule (non-negotiable)

No preset scenarios. The harness must not assume a use case that favors
either side (no "pbwire builds, the baseline reuses", no "schema arrives
at runtime so the tree is the input"). Both sides run the same task, from
the same logical values, with the same prep location and the same output
shape. If the two APIs cannot take the same shape, each side uses its own
idiomatic API on the same values — and the row is labelled with exactly
what each side did, so the difference is visible instead of hidden in a
scenario.

- Encode times `native -> bytes`. The input at the timer boundary is a
  host-native value (dict / model / struct / table / message object of the
  binding's public API), built by public API calls. Never a core `Message`
  handle, never a pre-encoded buffer.
- Decode times `bytes -> native`. The output is a host-native value of the
  same shape the binding ships (same struct / dict / table / model class).
  `bytes -> handle` with no materialization is the `proxy` task, not a
  headline.
- Both sides cross the same boundary. `struct` vs `struct`, `dict` vs
  `dict`, `table` vs `table`; `lazy handle` vs `lazy handle` only when both
  sides expose that as their shipped API. Never `table` vs `JSON string`,
  never `full object` vs `.str only`.
- No core-handle cache across iterations. Reusing an app-owned *native*
  input across iterations is the documented steady-state carve-out (the
  boundary price is the question, not the build price). Reusing a core
  `Message`/tree/decoder is `proxy` and must not share a cell with a
  native-in/out baseline.

## 3. Freshness must match

Two valid modes, both full-flow, never mixed in one cell:

- `fresh-task`: a fresh native object per iteration (`fill_*` inside the
  timer, alloc/free inside). Measures build + codec. Baseline does the same
  (`fillOfficial` / `fromObject` inside the timer).
- `steady-state`: one app-owned native object built once, re-encoded /
  re-decoded every iteration. Measures codec + boundary. Baseline reuses its
  own native message the same way.

Name which mode the cell is. `core bench.cc` is `fresh-task` both sides;
`bench_struct.cc` / most `shim` rows are `steady-state` both sides. Both
are honest once labelled.

## 3.1 Data preparation must be symmetric

- Same logical values from the same source data on both sides (`fill_gen` /
  `fill_pbw` / `fill_dyn` field for field, same constants, same repeat
  counts); a side never gets cheaper constants or fewer fields.
- Same prep location: `fresh-task` builds inside the timer on both sides
  (alloc/free inside); `steady-state` builds once outside the timer on both
  sides (timer covers codec + boundary only). Never one side fresh inside
  while the other reuses a held object.
- Prep cost that comes from the API itself (dynamic `set` + value alloc per
  field vs generated direct stores) is the measured subject in `fresh-task`
  mode, not a violation — it is disclosed as build price, and the
  codec-only question gets its own `steady-state`/`proxy` cell. What is
  forbidden is extra prep asymmetry on top: different values, different
  field counts, or build-inside vs reuse-outside mixed in one cell.

## 4. Materialization depth must match

- Deep decode means every gate-checked field is read into the host value
  before the timer stops (or the binding proves its C/FFI entry already
  materialized every field before return, with a source pointer).
- `bare parse` vs `deep` is never a headline pair. Either add an explicit
  `consume_*` fold on both sides (with a live `$sink`/`sink` so DCE cannot
  skip it) or demote the bare side to `proxy`.
- Sinks must fold the same fields on both sides so the same work is kept
  alive.

## 5. The rest (unchanged, restated)

- Same schema, same values, same `want` bytes (`136/174/2469/74913`);
  payload change (ObjC pad-`str`) is a declared exception, not silently
  comparable.
- Byte equality + field read-back + re-encode round-trip gate before timing;
  baseline gets the same gate (size-only is not a gate).
- Per-op independent calibration to `>=0.25s`, one warm-up, best-of-3,
  interleaved; never reuse one op's count for another except display-only.
- Same run, same process for both columns of a row. A `shim` number from
  `Bench` vs a baseline from `BenchBoth` is not a row.
- Baseline is the most-popular official package, same `.proto`, pinned
  version; a collapsed baseline (e.g. Julia xlarge `12.4 MB/s`) is labelled
  collapse, not a win.
- `proxy` / pure-host `core` ports / `dync` / `cshim` / JNI-ceiling arms stay
  in `bindings/<lang>/pbench/README.md`, never in `BENCHMARKS.md` tier tables.

## 6. Headline arms

Headline encode rows use the shipped thin/generated model: new a native
message, set its values, encode — never the raw tree API (`new` +
per-field generic `set` with one allocation per value). The tree API stays
fully tested for correctness (`make test`, `make difftest`, conformance)
but is not the published speed number: no shipped binding builds its
headline through it, and no baseline does either.

`shim` (one binding entry per op over a native value), JVM `core`
(generated model), Go thin model, single NIF/ARC arm. A `tree` row is only
a headline when both sides build a tree (or equivalent dynamic structure)
from the same values in the same mode — never tree-vs-struct as the
headline pair. Every headline cell names its mode
(`fresh` / `steady-state`) and its task (`encode(data)` / `to_dict` /
`shim` / `core`).

## 7. Status

Done in-tree (numbers refresh on the next full `make *-bench` run):

- Core: headline arms now go through the generated C struct
  (`bench/gen/msg_struct.h`, regenerated by the plugin at build time) and the
  compiled struct codec — new + set + encode per iteration on both sides.
  Plain decode uses a fresh arena per call (same batched profile as the
  reference parse); ctx reuses one. Table re-pasted from
  `results/bench-core-20261124T073808Z.json`; all eight cells above the line.
- Lua decode: `tree`/`shim`/baseline now fold the same field list, baseline
  gets a byte-equality gate and independent calibration.
- Clojure `core` decode: mutable decode plus the same gate-checked read-back
  as `consume-tree`, vs `protobuf-java` full parse.
- Swift `BenchBoth`: held-handle rows relabelled `proxy`, never the
  headline; the same-run `shim` vs baseline row is still open.
- ObjC encode: baseline builds fresh per iteration, matching the tree arm.

Not headline pairs by design (stay in `pbench/`): Ruby/Node tree rows
(fresh vs held; the headline `shim` rows are held vs held).

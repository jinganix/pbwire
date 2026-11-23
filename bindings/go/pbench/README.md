# pbench: the dynamic arms, one harness

Every arm does the same user-visible task over the same payload (the
`matrix3` tiers from `../benchmarks/bench_test.go`): a native Go struct in,
wire bytes out, and wire bytes in, a fully readable native struct out. No arm
may hold a prebuilt tree before the timer starts, and no arm may use
test-only glue: the struct<->tree moves go through shipped converters
(`ImportModel`/`ExportModel` in each arm's engine, tested as public API),
so the timer covers exactly what a user of that path pays.

The normative contract is in
[`../../README.md#pbench-three-arms-one-contract`](../../README.md#pbench-three-arms-one-contract).

There is no generated-code arm here on purpose: generated models are ergonomics
over the shared engine, not a third wire implementation, and the binding's
dynamic paths are what this harness compares.

## Arms

| dir | arm | what it does |
|---|---|---|
| `dync/dynamic/` | `tree` | dynamic C core engine: `Schema`/`Type`/`Message`, a value tree in C over cgo, hot-swappable registry, plus the shipped model<->tree converter |
| `dync/dync.go` | `tree` | struct -> C tree -> wire -> C tree -> struct, all through the shipped converter |
| `dyngo/dyngo.go` + `../runtime/` | `core` (tree) | struct -> `DMsg` -> wire -> `DMsg` -> struct through the shipped pure-Go engine, no cgo |
| `dyngo/dyngo.go` (`Direct*`) + `../runtime/` | `core` (direct) | struct -> wire -> struct straight through the shipped table engine, no `DMsg` tree in between |
| `dyngo/codec/` | `core` (candidate engine) | a second pure-Go value-tree + table engine, kept next to its tests; the candidate the `core` comparison runs against |
| `cshim/` (`engine/`, `layout/`) | `shim` (candidate) | Erlang-style C: measures the struct layout once, then C reads Go memory directly, one crossing per call; its engine is vendored here, not shipped |

The shipped engine is the pure-Go table engine in `../runtime/`: every
generated model marshals through it, so the formal bench (`../benchmarks/`,
`make go-bench`) times the thin model there. The `dync` arm keeps the C core
runnable for proxies and transcoders; the `cshim` arm keeps the fastest
candidate runnable with its engine vendored beside it (`engine/`,
`layout/`); the `dyngo/codec/` engine keeps the alternate Go core runnable
with its own tests. Nothing here is shared scaffolding.

## Harness

`compare_test.go` (build tag `pbench`, so `go test ./...` stays fast) times
all four paths plus `protobuf-go` in the same process, interleaved round by
round, median of 5 rounds, on all four tiers. `gate_test.go` gates every arm
before timing: byte-identical output to `protobuf-go` for all four tiers, a
field-by-field struct compare against the oracle decode, and the
`TestGateSemantics` vectors (invalid UTF-8 fails on every path; unknown
fields round-trip byte-identical).

```shell
go test -tags pbench -run TestCompare -v -count=1 ./pbench/
```

Last compare run (Apple M4 Pro, `go test -tags pbench -run TestCompare
-count=1 ./pbench/`; absolute numbers move with the host, the ordering does
not): `shim` 1.36-1.81x encode and 1.13-1.98x decode against `protobuf-go`,
`core` direct 1.46-1.58x encode and 0.84-1.13x decode, `tree` 0.05-0.09x
encode and 0.12-0.13x decode. On the xlarge tier (74913 B, median of 5
interleaved rounds):

| arm | encode ns/op | encode MB/s | decode ns/op | decode MB/s |
|---|---|---|---|---|
| `tree` (`dync/`) | 2640940.0 | 28.4 | 2127628.8 | 35.2 |
| `core` (`dyngo/` direct) | 89289.6 | 838.9 | 232320.4 | 322.5 |
| `shim` (`cshim/`) | 77714.8 | 963.9 | 132236.7 | 566.5 |
| `protobuf-go` | 135930.4 | 551.1 | 261572.3 | 286.4 |

That ordering is the reason the arms exist: a per-field copy into any tree
(the C core with cgo crossings worst, the Go core better but still
per-field) costs far more than reading the struct in place. The `shim` is
the shape to copy when a binding must be both native-object-end-to-end and
fast; the shipped `core` direct path is within a few percent of it with no
cgo and no layout measurement.

## Verdict

Shipped: the pure-Go table engine (`../runtime/`), no cgo, no layout
tables. The thin generated model over it is the default application path;
its numbers are the `pbwire-go` row in the formal bench (see
`BENCHMARKS.md`, Go section). `cshim` stays the fastest candidate arm with
its engine vendored beside it (`engine/`, `layout/`); `dyngo/codec/` stays
the alternate Go core with its own tests. `dync` stays the `tree` arm for
proxies and transcoders: the per-field copy into a tree is what separates
it, and it is never a headline.

## Candidate-engine tests

`dyngo/codec/` carries its own `*_test.go` plus `zz_scalars3_table_test.go`:
a codec-typed copy of the committed
`tests/gen/matrix3/matrix3_pbwire_meta.go` rows (same fields, same closures,
no `Unknown` hooks — the generated hooks close over the model's unexported
`unknownFields`, which an external test package cannot reach). It is kept by
hand in sync with the committed generated file; if the generator changes the
rows, this copy follows.

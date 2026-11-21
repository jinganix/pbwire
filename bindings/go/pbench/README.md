# pbench: the three dynamic arms, one harness

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
| `dyngo/codec/` | `core` | dynamic Go core engine: `Schema`/`DType`/`DMsg`, a pure-Go value tree and the shared table engine, plus the model<->tree converter |
| `dyngo/dyngo.go` | `core` | struct -> `DMsg` -> wire -> `DMsg` -> struct, all through the shipped converter, no cgo |
| `cshim/` | `shim` | Erlang-style C: measures the struct layout once, then C reads Go memory directly, one crossing per call |
| `../benchmarks/bench_test.go` (`pbwire-go-proxy`) | `proxy` | one `dync` C tree held before the timer through the shipped `ImportModel` converter: re-encode the held handle per iteration, decode-and-free per iteration with no `ExportModel` back out; published, never the headline |

The arm names are the ones in the normative contract (`tree`, `core`, `shim`);
the directory names stay as they are so the reference arm and its engine keep
one identity.

Both candidates keep their engine next to their arm; nothing here is shared
scaffolding. The harness selected the `shim` arm: the shipped engine is
`../runtime/` (layout in `../runtime/layout/`), and `cshim/` keeps the matrix3
reference arm whose hand-written plan and fixup mirror what
`protoc-gen-pbwire` emits. The `core` arm's engine is what generated code still
reaches: a model the shim cannot drive keeps the table engine's
`Marshal`/`Unmarshal`.

## Harness

`compare_test.go` (build tag `pbench`, so `go test ./...` stays fast) times all
three arms plus `protobuf-go` in the same process, interleaved round by round,
median of 5 rounds, on all four tiers. `gate_test.go` gates every arm before
timing: byte-identical output to `protobuf-go` for all four tiers, and a
field-by-field struct compare against the oracle decode.

```shell
go test -tags pbench -run TestCompare -v -count=1 ./pbench/
```

The formal bench (`../benchmarks/bench_test.go`, `make go-bench`, recorded
into `results/`) times the thin-model `core` row (`pbwire-go-core`: struct in
and out through the shared table engine, no prebuilt tree anywhere) beside
the new `proxy` row (`pbwire-go-proxy`: the held-tree task above) and the
`protobuf-go` baseline. The `tree` full task already has the Go-parity shape
(struct -> C tree -> wire -> C tree -> struct through `ImportModel` /
`ExportModel` per op, tree built fresh and freed inside the timer), so it is
verified, not re-timed: its numbers below stand from the interleaved harness.

Last compare run (Apple M4 Pro, `go test -tags pbench -run TestCompare
-count=1 ./pbench/`; absolute numbers move with the host, the ordering does
not): `shim` 1.36-1.81x encode and 1.13-1.98x decode against `protobuf-go`,
`core` direct 1.46-1.58x encode and 0.84-1.13x decode, `tree` 0.05-0.09x
encode and 0.12-0.13x decode. On the xlarge tier (74913 B, median of 5
interleaved rounds):

| arm | encode ns/op | encode MB/s | decode ns/op | decode MB/s |
|---|---|---|---|---|
| `tree` (`dync/`) | 2640940.0 | 28.4 | 2127628.8 | 35.2 |
| `core` (`dyngo/`) | 89289.6 | 838.9 | 232320.4 | 322.5 |
| `shim` (`cshim/`) | 77714.8 | 963.9 | 132236.7 | 566.5 |
| `protobuf-go` | 135930.4 | 551.1 | 261572.3 | 286.4 |

That ordering is the reason the arms exist: a per-field copy into any tree
(the C core with cgo crossings worst, the Go core better but still
per-field) costs far more than reading the struct in place. The `shim` is
the shape to copy when a binding must be both native-object-end-to-end and
fast.

## Verdict

Selected: `cshim` -> the shipped runtime (`../runtime/`, layout in
`../runtime/layout/`): one crossing per op, native struct in and out, above
`protobuf-go` on every tier in the interleaved harness (1.36-1.81x encode,
1.13-1.98x decode). `dyngo` stays the engine for models the shim cannot
drive, and the thin generated model over it (`pbwire-go-core` in the formal
bench) stays the default application path: xlarge 75829.0 ns/op encode
(1.92x `protobuf-go`) and 168831.1 ns/op decode (1.60x). `dync` stays the
`tree` arm for proxies and transcoders, xlarge 2640940.0 ns/op encode (0.05x)
and 2127628.8 ns/op decode (0.12x): the per-field copy into a tree (worst
through cgo crossings) is what separates it, and it is never a headline.
The new `proxy` row prices the held-tree task on that same arm, xlarge
51252.7 ns/op encode (2.84x) and 109982.8 ns/op decode (2.46x): 51.5x the
tree encode and 19.3x the tree decode at xlarge, the cost of the per-field
copy made visible. Proxy is published, never the headline.

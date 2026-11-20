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
| `dync/dynamic/` | dynamic C core engine | `Schema`/`Type`/`Message`: value tree in C over cgo, hot-swappable registry, plus the shipped model<->tree converter |
| `dync/dync.go` | dynamic C core arm | struct -> C tree -> wire -> C tree -> struct, all through the shipped converter |
| `dyngo/codec/` | dynamic Go core engine | `Schema`/`DType`/`DMsg`: pure-Go value tree and the shared table engine, plus the model<->tree converter |
| `dyngo/dyngo.go` | dynamic Go core arm | struct -> `DMsg` -> wire -> `DMsg` -> struct, all through the shipped converter, no cgo |
| `cshim/` | Erlang-style C | measures the struct layout once, then C reads Go memory directly: one crossing per call |

Both candidates keep their engine next to their arm; nothing here is shared
scaffolding. The harness selected `cshim`: the shipped engine is `../runtime/`
(layout in `../runtime/layout/`), and `cshim/` keeps the matrix3 reference arm
whose hand-written plan and fixup mirror what `protoc-gen-pbwire` emits.
`dyngo/codec/` is a candidate that generated code still reaches: a model the
cshim cannot drive keeps the table engine's `Marshal`/`Unmarshal`.

## Harness

`compare_test.go` (build tag `pbench`, so `go test ./...` stays fast) times all
three arms plus `protobuf-go` in the same process, interleaved round by round,
median of 5 rounds, on all four tiers. `gate_test.go` gates every arm before
timing: byte-identical output to `protobuf-go` for all four tiers, and a
field-by-field struct compare against the oracle decode.

```shell
go test -tags pbench -run TestCompare -v -count=1 ./pbench/
```

Last recorded run (Apple M4 Pro; absolute numbers move with the host, the
ordering does not): `cshim` 1.35-1.80x encode and 1.21-1.96x decode against
`protobuf-go`, `dyngo` direct 1.42-1.76x encode and 0.88-1.13x decode, `dync`
0.05-0.10x encode and 0.11-0.13x decode.

That ordering is the reason the arms exist: a per-field copy into any tree
(the C core with cgo crossings worst, the Go core better but still
per-field) costs far more than reading the struct in place. The C shim is
the shape to copy when a binding must be both native-object-end-to-end and
fast.

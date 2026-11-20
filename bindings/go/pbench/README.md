# pbench: the three dynamic arms, one harness

Every arm does the same user-visible task over the same payload (the
`matrix3` tiers from `../benchmarks/bench_test.go`): a native Go struct in,
wire bytes out, and wire bytes in, a fully readable native struct out. No arm
may hold a prebuilt tree before the timer starts, and no arm may use
test-only glue: the struct<->tree moves go through shipped converters
(`ImportModel`/`ExportModel` in each arm's engine, tested as public API),
so the timer covers exactly what a user of that path pays.

There is no generated-code arm here on purpose: generated models are ergonomics
over the shared engine, not a third wire implementation, and the binding's
dynamic paths are what this harness compares.

## Arms

| dir | arm | what it does |
|---|---|---|
| `dync/dynamic/` | dynamic C core engine | `Schema`/`Type`/`Message`: value tree in C over cgo, hot-swappable registry, plus the shipped model<->tree converter |
| `dync/dync.go` | dynamic C core arm | struct -> C tree -> wire -> C tree -> struct, all through the shipped converter |
| `dyngo/codec/` | dynamic Go core engine | `Schema`/`DType`/`DMsg`: pure-Go value tree and the shared table engine, plus the shipped model<->tree converter |
| `dyngo/dyngo.go` | dynamic Go core arm | struct -> `DMsg` -> wire -> `DMsg` -> struct, all through the shipped converter, no cgo |
| `cshim/` | Erlang-style C | measures the struct layout once, then C reads Go memory directly: one crossing per call |

Each solution lives in its own directory with its engine next to its arm;
nothing here is shared scaffolding. `runtime/` stays empty until this
harness selects the final solution.

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
ordering does not): `cshim` 1.61-2.16x encode and 1.41-2.03x decode against
`protobuf-go`, `dyngo` 0.27-0.35x encode and 0.31-0.35x decode, `dync`
0.05-0.10x encode and 0.11-0.13x decode.

That ordering is the reason the arms exist: a per-field copy into any tree
(the C core with cgo crossings worst, the Go core better but still
per-field) costs far more than reading the struct in place. The C shim is
the shape to copy when a binding must be both native-object-end-to-end and
fast.

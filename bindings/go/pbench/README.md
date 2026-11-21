# pbench: the two dynamic arms, one harness

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
| `dync/dynamic/` | C value tree (engine) | `Schema`/`Type`/`Message`: value tree in C over cgo, hot-swappable registry, plus the shipped model<->tree converter |
| `dync/dync.go` | C value tree (arm) | struct -> C tree -> wire -> C tree -> struct, all through the shipped converter |
| `dyngo/dyngo.go` + `../runtime/` | pure-Go value tree (engine) | `Schema`/`DType`/`DMsg`: pure-Go value tree and the shared table engine in the shipped `runtime/`, plus the model<->tree converter |
| `dyngo/dyngo.go` | pure-Go value tree (arm) | struct -> `DMsg` -> wire -> `DMsg` -> struct, all through the shipped converter, no cgo |

The C arm keeps its engine next to it; the Go arm drives the shipped
`../runtime/` engine. Nothing here is extra scaffolding. The shipped engine is the pure-Go table engine in
`../runtime/`: every generated model marshals through it, so the
harness now compares the two dynamic cores (C tree vs Go tree) with the
shipped struct path measured separately in `../benchmarks/`.

## Harness

`compare_test.go` (build tag `pbench`, so `go test ./...` stays fast) times both
arms plus `protobuf-go` in the same process, interleaved round by round,
median of 5 rounds, on all four tiers. `gate_test.go` gates both arms before
timing: byte-identical output to `protobuf-go` for all four tiers, and a
field-by-field struct compare against the oracle decode.

```shell
go test -tags pbench -run TestCompare -v -count=1 ./pbench/
```

Last recorded run (Apple M4 Pro; absolute numbers move with the host, the
ordering does not): pure-Go value tree 1.34-1.49x encode and 0.93-1.19x
decode against `protobuf-go`, C value tree 0.05-0.09x encode and 0.12-0.13x
decode. (The shipped struct path is timed in `../benchmarks/`.)

That ordering is the reason the arms exist: a per-field copy into any tree
(the C core with cgo crossings worst, the Go core better but still
per-field) costs far more than reading the struct in place, which is what
the shipped table engine's fast path does.

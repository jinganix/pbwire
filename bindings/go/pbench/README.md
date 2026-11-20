# pbench: the three dynamic arms, one harness

Every arm does the same user-visible task over the same payload (the
`matrix3` tiers from `../benchmarks/bench_test.go`): a native Go struct in,
wire bytes out, and wire bytes in, a fully readable native struct out. No arm
may hold a prebuilt tree before the timer starts, so the numbers include the
copy between the native object and whatever the wire codec works on.

There is no generated-code arm here on purpose: generated models are ergonomics
over the same codec, not a third wire implementation, and the binding's dynamic
paths are what this harness compares.

## Arms

| dir | arm | what it does |
|---|---|---|
| `dync/` | dynamic C core | copies the struct field by field into a `pbwire` C tree over cgo, encodes and decodes in the core |
| `dyngo/` | dynamic Go core | copies the struct field by field into a `codec.DMsg` value tree, no cgo |
| `cshim/` | Erlang-style C | measures the struct layout once, then C reads Go memory directly: one crossing per call |

## Harness

`compare_test.go` (build tag `pbench`, so `go test ./...` stays fast) times all
three arms plus `protobuf-go` in the same process, interleaved round by round,
median of 5 rounds, on all four tiers. `gate_test.go` gates every arm before
timing: byte-identical output to `protobuf-go` for all four tiers, and a
field-by-field struct compare against the oracle decode.

```shell
go test -tags pbench -run TestCompare -v -count=1 ./pbench/
```

Last recorded run (loaded host, so absolute numbers are inflated; the ratios
are the point): `cshim` 1.71-2.20x encode and 1.32-2.07x decode against
`protobuf-go`, `dyngo` 0.16-0.27x encode and 0.27-0.37x decode, `dync`
0.05-0.11x encode and 0.12-0.13x decode.

That ordering is the reason the arms exist: a per-field copy into any tree
(the dynamic C core with cgo crossings worst, the Go core better but still
per-field) costs far more than reading the struct in place. The C shim is the
shape to copy when a binding must be both native-object-end-to-end and fast.

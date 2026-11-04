# Benchmarks

`pbwire` (the Erlang/OTP NIF) against **gpb** (pure Erlang) and the
**gpb NIF** (libprotobuf behind gpb's own NIF generator), on the same schema and
payload as every other binding in this repo: `bindings/c/tests/data/matrix3.proto`
with the four standard tiers from [BENCHMARKS.md](../../../BENCHMARKS.md).

## Standard cases

`Scalars3` carries every scalar type, an enum, a nested message, two proto3
optionals, and the repeated fields that scale with the tier
(`nums` / `tags` / `inners` / `flags`). Both backends build the same gpb record,
so the timer measures the codec, not the tree.

| case | repeats | wire size |
|---|---|---|
| small | x0 | 136 B |
| medium | x1 | 174 B |
| large | x100 | 2469 B |
| xlarge | x3000 | 74913 B |

The wire size is pinned in `proto-bench`: gpb's bytes must come out at exactly
that size, and every backend must match gpb byte for byte (encoding the record
and re-encoding a decoded reference) before anything is timed. `matrix3.txt` in
`bindings/c/tests/data` holds the same payload in text format.

## Output format

One block per case, the same as every other binding's harness
(`bench/bench.cc`, `bindings/c/benchmarks/bench_struct.cc`):

```
small: 136 bytes, 1048576 iterations (repeated fields x0)
  impl           op             ns/op       MB/s
  pbwire-erlang  encode         451.1      301.5
  gpb            encode         918.2      148.1
  gpb-nif        encode        5419.5       25.1
  pbwire-erlang  decode         316.6      429.5
  gpb            decode         656.1      207.3
  gpb-nif        decode        4031.9       33.7
```

`ns/op` and `MB/s` are two views of the same measurement
(`MB/s = bytes / ns/op * 1000`), so the `impl op ns/op MB/s` tables in
BENCHMARKS.md hold for this suite too.

## Prerequisites

1. Install the Erlang version from the repo [`.tool-versions`](../.tool-versions) (currently via [asdf](https://asdf-vm.com/) or [mise](https://mise.jdx.dev/)):

   ```shell
   asdf install   # from repo root
   ```

   All benchmark `make` targets run Erlang through `scripts/with_tool_versions.sh` and fail if the active OTP major version does not match `.tool-versions`.

2. Build the parent project (NIF + `pbwire`):

   ```shell
   make build
   ```

   (`make benchmarks` does this automatically with the correct Erlang.)

3. Optional: point `GPB_PATH` at a local gpb checkout if you do not use the test profile deps:

   ```shell
   make GPB_PATH=/path/to/gpb build
   ```

4. Optional: the gpb NIF baseline needs `protoc` and the protobuf C++ runtime.
   `make benchmarks` builds and runs it when `pkg-config --exists protobuf`
   succeeds; otherwise the table simply has no `gpb-nif` rows.

## Running

From this directory:

| Target | Description |
|--------|-------------|
| `make build` | Compile the parent NIF and the gpb record modules into `tmp/` |
| `make benchmarks` | Run every available backend (epb, gpb, gpb NIF) |
| `make benchmarks-quick` | Same suite with `BENCH_TARGET_TIME=0.05` |
| `make update-readme` | Run the suite and refresh the result tables here and in `../README.md` |
| `make show-hw-sw-info` | Print the machine / OTP details recorded with a run |

Each operation is measured with the best of three timed samples; iterations
double until the slowest operation of a case needs `BENCH_TARGET_TIME` seconds
(default 0.25, `make benchmarks-quick` uses 0.05) after one warm-up pass — the
same unified measurement standard as every other binding (see the run-them
section in `BENCHMARKS.md`).

### Manual invocation

`proto-bench` runs every backend whose modules are in `tmp/`; restrict it with
repeated `--backend` flags. gpb (pure Erlang) is always the reference: its bytes
must match the pinned wire size and every other backend must match gpb, whether
or not `--backend gpb` is selected.

```shell
make build benchmarks-nif-code
../scripts/with_tool_versions.sh ./proto-bench --backend epb --backend gpb
BENCH_TARGET_TIME=0.05 ../scripts/with_tool_versions.sh ./proto-bench
```

The gpb modules are generated with gpb's `-introspect-get_proto_defs` option:
`get_proto_defs/0` carries the proto3 markers and the defs-format-3 enum values
that `pbwire:load_cache/1` consumes.

## Results

<!-- BENCHMARK_RESULTS_START -->
Last run: 2026-11-04 10:29:33 UTC (matrix3.proto, the four standard tiers; Erlang 29.0.1 per `.tool-versions`; machine Apple M4 Pro)

| payload | op | pbwire-erlang (MB/s, x gpb) | gpb (MB/s) | gpb-nif (MB/s) |
|---|---|---|---|---|
| 136 B | encode | 491.2 (2.47x) | 198.9 | 56.5 |
| 136 B | decode | 512.0 (2.18x) | 234.9 | 60.9 |
| 174 B | encode | 472.0 (2.44x) | 193.5 | 63.7 |
| 174 B | decode | 525.6 (2.21x) | 238.2 | 80.3 |
| 2469 B | encode | 407.9 (3.60x) | 113.4 | 43.9 |
| 2469 B | decode | 326.1 (1.81x) | 180.0 | 135.5 |
| 74913 B | encode | 441.7 (4.21x) | 104.9 | 109.6 |
| 74913 B | decode | 354.1 (1.98x) | 178.8 | 182.8 |

Higher is better. Throughput in MB/s on serialized wire size. Case names are the
four standard tiers at the repo root ([BENCHMARKS.md](../../../BENCHMARKS.md)).

<details>
<summary>Hardware / OTP (last run)</summary>

```
tool-versions erlang: 29.0.1
erl: /Users/jingan/.asdf/installs/erlang/29.0.1/bin/erl
System
Darwin 27.0.0 Darwin Kernel Version 27.0.0: Tue Aug 11 21:05:48 PDT 2026; root:xnu-13432.1.9~1/RELEASE_ARM64_T6041 arm64
CPU info
Apple M4 Pro
physical cpus  : 14
logical cpus   : 14

OTP release 29
Erlang/OTP 29 [erts-17.0.1] [source] [64-bit] [smp:14:14] [ds:14:14:10] [async-threads:1] [jit]
```
</details>

<details>
<summary>Full benchmark log (last run)</summary>

```
OK   small: pbwire-erlang encode/decode parity (136 bytes)
OK   small: gpb encode/decode parity (136 bytes)
OK   small: gpb-nif encode/decode parity (136 bytes)
small: 136 bytes, 131072 iterations (repeated fields x0)
  impl           op             ns/op       MB/s
  pbwire-erlang  encode         276.9      491.2
  gpb            encode         683.9      198.9
  gpb-nif        encode        2406.5       56.5
  pbwire-erlang  decode         265.6      512.0
  gpb            decode         579.1      234.9
  gpb-nif        decode        2234.9       60.9

OK   medium: pbwire-erlang encode/decode parity (174 bytes)
OK   medium: gpb encode/decode parity (174 bytes)
OK   medium: gpb-nif encode/decode parity (174 bytes)
medium: 174 bytes, 8192 iterations (repeated fields x1)
  impl           op             ns/op       MB/s
  pbwire-erlang  encode         368.7      472.0
  gpb            encode         899.4      193.5
  gpb-nif        encode        2729.9       63.7
  pbwire-erlang  decode         331.1      525.6
  gpb            decode         730.3      238.2
  gpb-nif        decode        2168.0       80.3

OK   large: pbwire-erlang encode/decode parity (2469 bytes)
OK   large: gpb encode/decode parity (2469 bytes)
OK   large: gpb-nif encode/decode parity (2469 bytes)
large: 2469 bytes, 8192 iterations (repeated fields x100)
  impl           op             ns/op       MB/s
  pbwire-erlang  encode        6053.1      407.9
  gpb            encode       21777.1      113.4
  gpb-nif        encode       56263.9       43.9
  pbwire-erlang  decode        7572.1      326.1
  gpb            decode       13718.9      180.0
  gpb-nif        decode       18221.4      135.5

OK   xlarge: pbwire-erlang encode/decode parity (74913 bytes)
OK   xlarge: gpb encode/decode parity (74913 bytes)
OK   xlarge: gpb-nif encode/decode parity (74913 bytes)
xlarge: 74913 bytes, 256 iterations (repeated fields x3000)
  impl           op             ns/op       MB/s
  pbwire-erlang  encode      169617.2      441.7
  gpb            encode      713875.0      104.9
  gpb-nif        encode      683507.8      109.6
  pbwire-erlang  decode      211539.1      354.1
  gpb            decode      418914.1      178.8
  gpb-nif        decode      409769.5      182.8
```
</details>
<!-- BENCHMARK_RESULTS_END -->

## Notes

- `matrix3.proto` is shared with the C, Go, Python, Java, Kotlin, Node, Ruby,
  Objective-C, PHP, Dart, Rust and C# benchmarks, so the wire sizes and the
  payload are comparable across bindings.
- Artifacts go under `tmp/` (gitignored); `make clean` removes them.

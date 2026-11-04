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
Last run: 2026-11-04 05:00:02 UTC (matrix3.proto, the four standard tiers; Erlang 29.0.1 per `.tool-versions`; machine Apple M4 Pro)

| payload | op | pbwire-erlang (MB/s, x gpb) | gpb (MB/s) | gpb-nif (MB/s) |
|---|---|---|---|---|
| 136 B | encode | 362.1 (2.01x) | 180.1 | 81.5 |
| 136 B | decode | 494.7 (2.20x) | 224.6 | 92.6 |
| 174 B | encode | 347.7 (2.46x) | 141.5 | 72.6 |
| 174 B | decode | 477.9 (2.17x) | 219.8 | 84.6 |
| 2469 B | encode | 233.8 (1.84x) | 127.0 | 96.0 |
| 2469 B | decode | 327.9 (1.71x) | 192.3 | 173.7 |
| 74913 B | encode | 253.9 (2.06x) | 123.2 | 123.0 |
| 74913 B | decode | 369.4 (1.69x) | 218.6 | 210.4 |

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
small: 136 bytes, 262144 iterations (repeated fields x0)
  impl           op             ns/op       MB/s
  pbwire-erlang  encode         375.6      362.1
  gpb            encode         755.0      180.1
  gpb-nif        encode        1669.7       81.5
  pbwire-erlang  decode         274.9      494.7
  gpb            decode         605.6      224.6
  gpb-nif        decode        1468.9       92.6

OK   medium: pbwire-erlang encode/decode parity (174 bytes)
OK   medium: gpb encode/decode parity (174 bytes)
OK   medium: gpb-nif encode/decode parity (174 bytes)
medium: 174 bytes, 131072 iterations (repeated fields x1)
  impl           op             ns/op       MB/s
  pbwire-erlang  encode         500.4      347.7
  gpb            encode        1229.6      141.5
  gpb-nif        encode        2396.0       72.6
  pbwire-erlang  decode         364.1      477.9
  gpb            decode         791.6      219.8
  gpb-nif        decode        2056.3       84.6

OK   large: pbwire-erlang encode/decode parity (2469 bytes)
OK   large: gpb encode/decode parity (2469 bytes)
OK   large: gpb-nif encode/decode parity (2469 bytes)
large: 2469 bytes, 16384 iterations (repeated fields x100)
  impl           op             ns/op       MB/s
  pbwire-erlang  encode       10559.2      233.8
  gpb            encode       19443.9      127.0
  gpb-nif        encode       25718.8       96.0
  pbwire-erlang  decode        7530.5      327.9
  gpb            decode       12841.2      192.3
  gpb-nif        decode       14210.3      173.7

OK   xlarge: pbwire-erlang encode/decode parity (74913 bytes)
OK   xlarge: gpb encode/decode parity (74913 bytes)
OK   xlarge: gpb-nif encode/decode parity (74913 bytes)
xlarge: 74913 bytes, 512 iterations (repeated fields x3000)
  impl           op             ns/op       MB/s
  pbwire-erlang  encode      295095.7      253.9
  gpb            encode      607851.6      123.2
  gpb-nif        encode      609236.3      123.0
  pbwire-erlang  decode      202789.1      369.4
  gpb            decode      342712.9      218.6
  gpb-nif        decode      356033.2      210.4
```
</details>
<!-- BENCHMARK_RESULTS_END -->

## Notes

- `matrix3.proto` is shared with the C, Go, Python, Java, Kotlin, Node, Ruby,
  Objective-C, PHP, Dart, Rust and C# benchmarks, so the wire sizes and the
  payload are comparable across bindings.
- Artifacts go under `tmp/` (gitignored); `make clean` removes them.

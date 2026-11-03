# Benchmarks

`enif_protobuf` (the Erlang/OTP NIF) against its two baselines, on the standard
Google wire messages from [gpb/benchmarks](https://github.com/tomas-abrahamsson/gpb/tree/master/benchmarks):
pure-Erlang **gpb** (records) and **gpb NIF** (libprotobuf). Optional `d.proto`
stress cases cover merges.

## Standard cases

Case names follow the four tiers defined at the repo root
([BENCHMARKS.md](../../../BENCHMARKS.md)); numbers from every binding land in the
same cross-binding table there.

| case | file | wire size | notes |
|---|---|---|---|
| medium | `google_message1.dat` | 228 B | between the core medium (169 B) and large (2755 B) cases |
| xlarge | `google_message2.dat` | 84584 B | the tier the core `xlarge` case (85933 B) was sized after |

The other two tiers (small, large) have no `.dat` fixture in this suite: the two
Google messages are fixed, generated-code performance shapes, not synthesizable
sizes. gpb's `d.proto` merge cases are extra and stay Erlang-only.

## Output format

One line per operation, carrying the same fields as every other binding's
harness (`bench/bench.cc`, `bindings/c/benchmarks/bench_struct.cc`):

```
Serialize to binary: 228 bytes, 21025345 iterations in 29.093s; 157.14MB/s; 1384.6 ns/op
Deserialize from binary: 228 bytes, 38342080 iterations in 30.596s; 272.49MB/s; 797.8 ns/op
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

2. Build the parent project (NIF + `enif_protobuf`):

   ```shell
   cd ..
   ../scripts/with_tool_versions.sh ./rebar3 compile
   ../scripts/with_tool_versions.sh ./rebar3 as test compile   # gpb for code generation
   ```

   (`make build` in this directory does this automatically with the correct Erlang.)

3. Optional: point `GPB_PATH` at a local gpb checkout if you do not use the test profile deps:

   ```shell
   make GPB_PATH=/path/to/gpb build
   ```

## Running

From this directory:

| Target | Description |
|--------|-------------|
| `make build` | Compile benchmark modules (`tmp/msg_r.beam`, …) |
| `make gpb-benchmarks` | Pure Erlang **gpb** (records, `msg_r`) |
| `make gpb-nif-benchmarks` | **gpb NIF** (libprotobuf, `msg`) |
| `make epb-benchmarks` | **enif_protobuf** NIF (same generated modules) |
| `make benchmarks` | Run all three backends: `gpb`, `gpb nif`, `epb` |
| `make benchmarks-quick` | Same suite with `BENCH_TARGET_TIME=3` (debug/dev) |
| `make update-readme` | Run standard suite and refresh result tables in this file and `../README.md` |
| `make erl-benchmarks` | Records + maps variants for both backends |
| `make d-benchmarks` | `d.proto` merge stress (both backends) |
| `make nif-benchmarks` | gpb’s libprotobuf NIF (requires C++ protobuf) |

Shorter runs while developing:

```shell
BENCH_TARGET_TIME=3 make benchmarks
```

Or use the shortcut target:

```shell
make benchmarks-quick
```

HiPE (if supported on your OTP):

```shell
make HIPE=1 gpb-benchmarks
```

### Manual invocation

`proto-bench` takes `--backend gpb` or `--backend epb`, then triples: `MODULE MESSAGE FILE`:

```shell
make build
../scripts/with_tool_versions.sh ./proto-bench \
  --backend gpb msg_r Message1 google_message1.dat \
  --backend gpb msg Message1 google_message1.dat \
  --backend epb msg_r Message1 google_message1.dat
```

Each case runs serialize and deserialize for ~30 seconds (configurable via `BENCH_TARGET_TIME`).

## Results

<!-- BENCHMARK_RESULTS_START -->
Last run: 2026-11-03 11:10:46 UTC (standard Google benchmark messages; Erlang 29.0.1 per `.tool-versions`; machine Apple M4 Pro)

| Test case | epb | gpb | gpb nif |
|-----------|-----|-----|---------|
| medium · 228 B · serialize | 300.10 | 165.01 | 59.14 |
| medium · 228 B · deserialize | 739.08 | 278.99 | 74.74 |
| xlarge · 84584 B · serialize | 304.98 | 135.02 | 102.86 |
| xlarge · 84584 B · deserialize | 548.23 | 255.49 | 189.66 |

Higher is better. Throughput in MB/s on serialized wire size. Case names follow
the four standard tiers at the repo root ([BENCHMARKS.md](../../../BENCHMARKS.md)).

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
=== gpb ===
Benchmarking gpb msg_r (Message1) with file google_message1.dat
Serialize to binary: 228 bytes, 7677668 iterations in 10.117s; 165.01MB/s; 1317.7 ns/op
Deserialize from binary: 228 bytes, 12836707 iterations in 10.005s; 278.99MB/s; 779.4 ns/op

Benchmarking gpb msg_r (Message2) with file google_message2.dat
Serialize to binary: 84584 bytes, 16940 iterations in 10.120s; 135.02MB/s; 597424.8 ns/op
Deserialize from binary: 84584 bytes, 23809 iterations in 7.517s; 255.49MB/s; 315722.8 ns/op

=== gpb nif ===
Benchmarking gpb msg (Message1) with file google_message1.dat
Serialize to binary: 228 bytes, 2497487 iterations in 9.183s; 59.14MB/s; 3676.9 ns/op
Deserialize from binary: 228 bytes, 3985135 iterations in 11.593s; 74.74MB/s; 2909.2 ns/op

Benchmarking gpb msg (Message2) with file google_message2.dat
Serialize to binary: 84584 bytes, 12550 iterations in 9.842s; 102.86MB/s; 784236.7 ns/op
Deserialize from binary: 84584 bytes, 24297 iterations in 10.334s; 189.66MB/s; 425326.7 ns/op

=== enif_protobuf ===
Benchmarking enif_protobuf msg_r (Message1) with file google_message1.dat
Serialize to binary: 228 bytes, 13551288 iterations in 9.819s; 300.10MB/s; 724.6 ns/op
Deserialize from binary: 228 bytes, 33545402 iterations in 9.869s; 739.08MB/s; 294.2 ns/op

Benchmarking enif_protobuf msg_r (Message2) with file google_message2.dat
Serialize to binary: 84584 bytes, 37065 iterations in 9.803s; 304.98MB/s; 264490.6 ns/op
Deserialize from binary: 84584 bytes, 68506 iterations in 10.080s; 548.23MB/s; 147137.4 ns/op
```
</details>
<!-- BENCHMARK_RESULTS_END -->

The log above predates the `bytes` and `ns/op` fields; `make update-readme`
regenerates it in the current format.

## Notes

- Small/large messages are 228 and 84584 bytes on the wire (`google_message1.dat`, `google_message2.dat`);
  in the shared four-tier naming they are the **medium** and **xlarge** cases.
- Compared to upstream gpb benchmarks, groups were converted to repeated sub-messages; see `msg.proto` comments.
- Artifacts go under `tmp/` (gitignored).

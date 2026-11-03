# Benchmarks

Every binding benchmarks the same four payload tiers and prints the same
columns, so numbers are comparable across bindings. Each binding compares
against its official counterpart; this file also compares the bindings with
each other.

## Standard cases

`small` / `medium` / `large` come from `make bench`; `xlarge` is the tier of
the Erlang `google_message2.dat` case (84584 B). Each binding scales the
repeated fields of its own representative message (x0 / x1 / x100 / x3000);
schemas differ per binding, so tiers match by wire size, not by bytes.

| case | repeats | core `bench.Msg` | C `Scalars3` | origin |
|---|---|---|---|---|
| small | x0 | 132 B | 136 B | pbwire small |
| medium | x1 | 169 B | 174 B | pbwire full (~ Erlang Message1 228 B tier) |
| large | x100 | 2755 B | 2469 B | pbwire large |
| xlarge | x3000 | 85933 B | 74913 B | Erlang Message2 84584 B tier |

## Output format

Every harness prints one block per case (throughput over wire bytes, higher
is better):

```
<case>: <bytes> bytes, <iterations> iterations (repeat field x<repeats>)
  impl      op           ns/op       MB/s
  <impl>    encode        ...        ...
  <base>    encode        ...        ...
  <impl>    decode        ...        ...
  <base>    decode        ...        ...
```

`ns/op` and `MB/s` are two views of the same measurement
(`MB/s = bytes / ns/op * 1000`). The Erlang `proto-bench` prints one line per
op (`<op>: <iterations> iterations in <seconds>s; <MB/s>MB/s`) carrying the
same fields; `ns/op = seconds * 1e9 / iterations`.

## Bindings vs their baselines

| binding | harness | baseline | cases | status |
|---|---|---|---|---|
| core (dynamic) | `make bench` (`bench/bench.cc`) | `protoc` C++ generated + DynamicMessage | all 4 | ✅ |
| C structs | `make bench-c` (`bindings/c/benchmarks/bench_struct.cc`) | `protoc` C++ generated from the same `matrix3.proto` | all 4 | ✅ |
| Erlang/OTP NIF | `make benchmarks` in `bindings/erlang/benchmarks` | gpb (pure Erlang) and gpb NIF (libprotobuf) | medium, xlarge (`google_message1/2.dat`) | ✅ |
| C++ | same engine as C (`bindings/cpp` mirrors the C structs) | — | — | planned |
| Go / Python | smoke covers only, no perf harness yet | — | — | planned |

Correctness first everywhere: the core bench verifies byte-for-byte equality
before timing, the C bench checks struct encode == `protoc` bytes plus a
decode→encode round trip, and the Erlang suite asserts gpb byte parity.

## Cross-binding comparison (last run)

Apple M4 Pro, `-O2`, `protoc 36.2`, 200k iterations
(xlarge: 2000). Throughput in MB/s on serialized wire size. Per-run noise is
a few percent; regenerate below for the current machine.

Core (`make bench`): pbwire ctx / plain vs `protoc` generated:

| payload | op | pbwire ctx (MB/s, x gen) | pbwire plain (MB/s, x gen) | protoc gen (MB/s) | DynamicMessage (MB/s) |
|---|---|---|---|---|---|
| 132 B | encode | 2067.3 (1.27x) | 1635.4 (1.00x) | 1631.1 | 112.4 |
| 132 B | decode | 1393.8 (1.55x) | 1014.7 (1.13x) | 898.5 | 285.1 |
| 169 B | encode | 1902.9 (1.15x) | 1640.9 (0.99x) | 1652.3 | 108.3 |
| 169 B | decode | 1104.5 (1.40x) | 831.3 (1.06x) | 787.3 | 272.3 |
| 2755 B | encode | 1430.1 (1.25x) | 1325.0 (1.16x) | 1144.4 | 127.2 |
| 2755 B | decode | 553.1 (1.24x) | 533.0 (1.19x) | 447.5 | 248.4 |
| 85933 B | encode | 1653.0 (1.42x) | 1689.3 (1.45x) | 1163.7 | 146.7 |
| 85933 B | decode | 614.8 (1.38x) | 597.1 (1.34x) | 446.1 | 256.8 |

C structs (`make bench-c`): generated structs vs `protoc` generated from the
same `matrix3.proto`:

| payload | op | pbwire-c (MB/s, x gen) | protoc gen (MB/s) |
|---|---|---|---|
| 136 B | encode | 193.6 (0.14x) | 1396.8 |
| 136 B | decode | 283.7 (0.35x) | 816.1 |
| 174 B | encode | 191.6 (0.15x) | 1317.2 |
| 174 B | decode | 268.0 (0.36x) | 751.6 |
| 2469 B | encode | 146.5 (0.17x) | 883.5 |
| 2469 B | decode | 245.4 (0.64x) | 380.9 |
| 74913 B | encode | 143.4 (0.17x) | 868.9 |
| 74913 B | decode | 257.9 (0.69x) | 372.5 |

The struct layer pays for walking twice (structs → message tree → wire) plus
one allocation per value, so it trails generated code; decode stays closer
because parsing dominates on both sides. The struct decode time includes the
mandatory `free` (the decoder allocates into the output struct); the core
bench likewise frees inside its decode rows.

Erlang (`bindings/erlang/benchmarks`, 2026-11-02, Erlang 29.0.1, same machine
class): enif_protobuf vs gpb vs gpb NIF, throughput in MB/s:

| Test case | epb | gpb | gpb nif |
|-----------|-----|-----|---------|
| Small msgs · serialize | 286.91 | 157.14 | 56.79 |
| Small msgs · deserialize | 716.65 | 272.49 | 76.29 |
| Large msgs · serialize | 299.14 | 135.76 | 103.76 |
| Large msgs · deserialize | 516.21 | 255.91 | 196.61 |

Small/large here are the 228 B and 84584 B Google messages, i.e. the medium
and xlarge tiers above.

## Run them

```bash
make bench        # core: 4 cases + text/json/stream/threads (needs protoc + libprotobuf)
make bench-c      # C structs vs protoc C++ on matrix3.proto (same deps)
make erlang-bench # Erlang binding: gpb, gpb NIF, enif_protobuf
./build/bench bench 2000          # shorter core run (default 200000; xlarge scales /100)
./build/bench_struct_c bench 2000 # shorter C run (same scaling)
```

From `bindings/erlang/benchmarks` (needs Erlang/OTP + gpb, ~30 s per op by
default, `BENCH_TARGET_TIME=3` for a quick run):

```bash
make benchmarks        # gpb, gpb NIF and enif_protobuf on both .dat files
make benchmarks-quick  # same suite, 3 s target per op
```

## Regenerate this file

The core table is `scripts/perf.sh` output (same layout, MB/s only); the C
table is `make bench-c` output reformatted the same way; the Erlang table is
copied from `bindings/erlang/benchmarks/README.md`
(`make update-readme` there). Paste all three runs from the same machine and
note the date, CPU, `protoc` version and iteration counts above.

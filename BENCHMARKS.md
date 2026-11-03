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

| case | repeats | core `bench.Msg` | C `Scalars3` | Python `Scalars3` | Erlang Google messages |
|---|---|---|---|---|---|
| small | x0 | 132 B | 136 B | 136 B | — |
| medium | x1 | 169 B | 174 B | 174 B | 228 B (`google_message1.dat`) |
| large | x100 | 2755 B | 2469 B | 2469 B | — |
| xlarge | x3000 | 85933 B | 74913 B | 74913 B | 84584 B (`google_message2.dat`) |

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
op (`<op>: <bytes> bytes, <iterations> iterations in <seconds>s; <MB/s>MB/s;
<ns/op> ns/op`), carrying the same four fields.

## Bindings vs their baselines

| binding | harness | baseline | cases | vs baseline (xlarge) |
|---|---|---|---|---|
| core (dynamic) | `make bench` (`bench/bench.cc`) | `protoc` C++ generated + DynamicMessage | all 4 | 1.5x / 1.4x ✅ |
| C structs | `make bench-c` (`bindings/c/benchmarks/bench_struct.cc`) | `protoc` C++ generated from the same `matrix3.proto` | all 4 | 1.11x / 1.20x ✅ |
| Erlang/OTP NIF | `make benchmarks` in `bindings/erlang/benchmarks` | gpb (pure Erlang) and gpb NIF (libprotobuf) | medium, xlarge | 1.9x / 1.2x ✅ |
| Python (CPython ext) | `make python-bench` (`bindings/python/benchmarks/bench.py`) | `google.protobuf` (upb C extension) on the same `matrix3.proto` | all 4 | 0.38x / 0.24x ❌ |
| C++ | same engine as C (`bindings/cpp` mirrors the C structs) | — | — | planned |
| Go | smoke cover only, no perf harness yet | — | — | planned |

"vs baseline" is encode / decode throughput relative to the baseline on the
largest case the binding runs. The Python row is the one binding still behind
its baseline: upb parses into its own arena and materializes Python objects
lazily, while the binding's contract is plain dicts, so every dict, string and
int is built eagerly. It went from 0.02x (cffi bridge) to 0.25-0.4x with a
CPython extension; going further needs lazy message views or per-message
generated C.

Correctness first everywhere: the core bench verifies byte-for-byte equality
before timing, the C and Python benches check their bytes against the
`protoc`-produced reference (plus a decode→encode round trip) before timing,
and the Erlang suite asserts gpb byte parity.

## Cross-binding comparison (last run)

Apple M4 Pro, `-O2`, `protoc 36.2`, protobuf Python 7.36.2 (upb), Erlang
29.0.1. Core/C/Python: 200k iterations (xlarge: 2000); Erlang: 10 s per op.
Throughput in MB/s on serialized wire size. Per-run noise is a few percent;
regenerate below for the current machine.

### Core: pbwire ctx / plain vs `protoc` generated

| payload | op | pbwire ctx (MB/s, x gen) | pbwire plain (MB/s, x gen) | protoc gen (MB/s) | DynamicMessage (MB/s) |
|---|---|---|---|---|---|
| 132 B | encode | 2115.0 (1.31x) | 1681.1 (1.04x) | 1617.2 | 110.0 |
| 132 B | decode | 1504.3 (1.70x) | 973.5 (1.10x) | 883.1 | 293.3 |
| 169 B | encode | 1897.8 (1.21x) | 1498.2 (0.96x) | 1568.2 | 107.9 |
| 169 B | decode | 1109.5 (1.39x) | 879.8 (1.10x) | 797.8 | 294.6 |
| 2755 B | encode | 1494.6 (1.29x) | 1295.1 (1.12x) | 1154.5 | 127.7 |
| 2755 B | decode | 566.4 (1.26x) | 543.1 (1.21x) | 448.2 | 255.5 |
| 85933 B | encode | 1699.1 (1.46x) | 1696.9 (1.46x) | 1164.5 | 146.2 |
| 85933 B | decode | 626.4 (1.43x) | 602.0 (1.37x) | 438.6 | 259.2 |

Threads (169 B, 1 vs 8):

| op | pbwire 1thr (MB/s, x gen) | pbwire 8thr agg (MB/s, x gen) | gen 1thr (MB/s) | gen 8thr agg (MB/s) |
|---|---|---|---|---|
| encode | 1689.5 (0.97x) | 13918.3 (1.11x) | 1745.5 | 12503.7 |
| decode | 1109.0 (1.40x) | 8186.5 (1.39x) | 792.1 | 5869.1 |

### C structs vs `protoc` generated (same `matrix3.proto`)

| payload | op | pbwire-c (MB/s, x gen) | protoc gen (MB/s) |
|---|---|---|---|
| 136 B | encode | 807.2 (0.55x) | 1455.6 |
| 136 B | decode | 517.0 (0.58x) | 892.3 |
| 174 B | encode | 875.3 (0.63x) | 1391.6 |
| 174 B | decode | 441.1 (0.59x) | 750.3 |
| 2469 B | encode | 845.3 (0.97x) | 869.3 |
| 2469 B | decode | 409.7 (1.12x) | 366.7 |
| 74913 B | encode | 971.9 (1.11x) | 879.1 |
| 74913 B | decode | 434.2 (1.20x) | 361.4 |

The struct codec writes and reads wire bytes directly from the layout tables;
there is no dynamic message tree in between. That is what changed here: it used
to build a `pbw_message` first, which cost 146 MB/s encode and 268 MB/s decode
on the 75 KB tier (0.17x / 0.32x of generated code). From ~2.5 KB up it now
matches or beats generated code; the 136 B / 174 B tiers stay ~1.7x behind
because `protoc --cpp_out` emits straight-line code with no per-field dispatch.

### Python vs `google.protobuf` (upb)

| payload | op | pbwire-py (MB/s, x upb) | upb (MB/s) |
|---|---|---|---|
| 136 B | encode | 291.4 (0.39x) | 741.9 |
| 136 B | decode | 254.6 (0.44x) | 579.3 |
| 174 B | encode | 335.2 (0.42x) | 803.6 |
| 174 B | decode | 219.9 (0.39x) | 561.9 |
| 2469 B | encode | 483.0 (0.33x) | 1443.0 |
| 2469 B | decode | 230.9 (0.33x) | 704.0 |
| 74913 B | encode | 570.8 (0.38x) | 1497.5 |
| 74913 B | decode | 188.8 (0.24x) | 787.0 |

The binding is a CPython extension that walks the schema and converts
dict <-> wire in C (`bindings/python/csrc/pbwire.c`), the same shape as the
Erlang NIF. It used to be a cffi ABI bridge at 0.02x; the extension is 12-15x
faster than that. The remaining gap is representation, not wire work: upb
parses into its arena and builds Python objects lazily, while this API promises
plain dicts and therefore allocates one dict, string and int per value.

#### What the optimization pass changed

| version | xlarge encode | xlarge decode |
|---|---|---|
| cffi bridge (one call per value) | 9.3 MB/s | 8.6 MB/s |
| cffi bridge + bulk helpers | 25.1 | 15.6 |
| CPython extension (this) | 570.8 | 188.8 |

The extension caches per-field facts once per message type (name, number,
type, wire type, packed/required, sub-type), skips the descriptor entirely in
the hot loops, uses the list fast path for repeated fields, and writes tag +
value with one capacity reservation per field.

### Erlang: enif_protobuf vs gpb vs gpb NIF

| Test case | epb | gpb | gpb nif |
|-----------|-----|-----|---------|
| medium · 228 B · serialize | 300.10 | 165.01 | 59.14 |
| medium · 228 B · deserialize | 739.08 | 278.99 | 74.74 |
| xlarge · 84584 B · serialize | 304.98 | 135.02 | 102.86 |
| xlarge · 84584 B · deserialize | 548.23 | 255.49 | 189.66 |

## Run them

```bash
make bench        # core: 4 cases + text/json/stream/threads (needs protoc + libprotobuf)
make bench-c      # C structs vs protoc C++ on matrix3.proto (same deps)
make python-bench # Python extension vs google.protobuf (needs protobuf + protoc)
make erlang-bench # Erlang binding: gpb, gpb NIF, enif_protobuf (needs OTP + gpb)
./build/bench bench 2000          # shorter core run (default 200000; xlarge scales /100)
./build/bench_struct_c bench 2000 # shorter C run (same scaling)
```

The Python benchmark calibrates its own iteration count per case, so it needs
no iteration argument.

From `bindings/erlang/benchmarks` (needs Erlang/OTP + gpb, ~30 s per op by
default, `BENCH_TARGET_TIME=3` for a quick run):

```bash
make benchmarks        # gpb, gpb NIF and enif_protobuf on both .dat files
make benchmarks-quick  # same suite, 3 s target per op
```

## Regenerate this file

The core table is `scripts/perf.sh` output (same layout, MB/s only); the C and
Python tables are `make bench-c` / `make python-bench` output reformatted the
same way; the Erlang table is copied from
`bindings/erlang/benchmarks/README.md` (`make update-readme` there). Paste all
four runs from the same machine and note the date, CPU, `protoc` version and
iteration counts above.

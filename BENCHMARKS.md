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

| case | repeats | core `bench.Msg` | C `Scalars3` | Go `Scalars3` | Python `Scalars3` | Erlang Google messages |
|---|---|---|---|---|---|---|
| small | x0 | 132 B | 136 B | 136 B | 136 B | — |
| medium | x1 | 169 B | 174 B | 174 B | 174 B | 228 B (`google_message1.dat`) |
| large | x100 | 2755 B | 2469 B | 2469 B | 2469 B | — |
| xlarge | x3000 | 85933 B | 74913 B | 74913 B | 74913 B | 84584 B (`google_message2.dat`) |

Go reuses the C `matrix3.proto` schema and the same generated payload, so its
wire sizes match the C column exactly.

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
(`MB/s = bytes / ns/op * 1000`). `make bench-c` adds one more block per case
(`pbwire-c1`: the C binding's one-shot API, which compiles the layout on every
call) after the standard four rows; only the standard block is aggregated
below. The Erlang `proto-bench` prints one line per
op (`<op>: <bytes> bytes, <iterations> iterations in <seconds>s; <MB/s>MB/s;
<ns/op> ns/op`), carrying the same four fields. The Python harness names its
ops by the task instead (`encode(data)`, `decode(bytes)`, `to_dict(bytes)`,
`encode(parsed)`), because a dict-based binding has two representations and
only comparing like with like is fair.

## Bindings vs their baselines

| binding | harness | baseline | cases | vs baseline (xlarge) |
|---|---|---|---|---|
| core (dynamic) | `make bench` (`bench/bench.cc`) | `protoc` C++ generated + DynamicMessage | all 4 | 1.5x / 1.4x ✅ |
| C structs | `make bench-c` (`bindings/c/benchmarks/bench_struct.cc`) | `protoc` C++ generated from the same `matrix3.proto` | all 4 | 1.47x / 2.42x ✅ |
| Erlang/OTP NIF | `make benchmarks` in `bindings/erlang/benchmarks` | gpb (pure Erlang) and gpb NIF (libprotobuf) | medium, xlarge | 1.9x / 1.2x ✅ |
| Python (CPython ext) | `make python-bench` (`bindings/python/benchmarks/bench.py`) | `google.protobuf` (upb C extension) on the same `matrix3.proto` | all 4 | 6.7x / 1.7x ✅ |
| C++ | same engine as C (`bindings/cpp` mirrors the C structs) | — | — | planned |
| Go (generated) | `make go-bench` (`bindings/go/benchmarks`) | `google.golang.org/protobuf` generated code from the same `matrix3.proto` | all 4 | 2.85x / 1.69x ✅ |

"vs baseline" is encode / decode throughput relative to the baseline on the
largest case the binding runs. `encode` is Python data to wire bytes and
`decode` is wire bytes to an in-memory message, so both sides start and end in
the same place; for the Python row that means `encode(dict)` against upb
filling a message from the same dict and serializing it, and `decode(bytes)`
against `FromString`. Every binding is ahead of its baseline now.

Correctness first everywhere: the core bench verifies byte-for-byte equality
before timing, the C, Go and Python benches check their bytes against the
`protoc`-produced reference (plus a decode→encode round trip) before timing,
and the Erlang suite asserts gpb byte parity.

## Cross-binding comparison (last run)

Apple M4 Pro, `-O2`, `protoc 36.2`, protobuf Python 7.36.2 (upb), Erlang
29.0.1. Core/C: 200k iterations (xlarge: 2000); Python and Go calibrate their
own iteration count per case (Go row: Go 1.27.3, `google.golang.org/protobuf`
v1.36.12); Erlang: 10 s per op. Throughput in MB/s on serialized
wire size. Per-run noise is a few percent; regenerate below for the current
machine.

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
| 136 B | encode | 1431.7 (0.98x) | 1464.3 |
| 136 B | decode | 1138.7 (1.28x) | 888.0 |
| 174 B | encode | 1466.4 (1.05x) | 1392.8 |
| 174 B | decode | 1056.0 (1.40x) | 755.8 |
| 2469 B | encode | 1083.6 (1.22x) | 884.7 |
| 2469 B | decode | 810.6 (2.10x) | 386.5 |
| 74913 B | encode | 1318.8 (1.47x) | 898.2 |
| 74913 B | decode | 910.3 (2.42x) | 375.5 |

The struct codec writes and reads wire bytes directly from the layout tables;
there is no dynamic message tree in between. Three changes got it ahead of
generated straight-line code instead of ~1.7x behind it:

- a compiled codec (`pbw_struct_codec`) that resolves the schema once and
  precomputes an opcode and the tag bytes per field, so the per-field work is
  one small dispatch instead of a schema/type lookup;
- an fnum -> field map plus a precomputed free list on the decode side, so a
  field tag is one indexed load and releasing a tree only visits the rows that
  own memory;
- caller-owned buffers: one reused `pbw_struct_buf` for the output and one
  reused `pbw_struct_arena` for decoded memory, so a steady-state loop does not
  touch the allocator at all (an arena block never moves, so decoded pointers
  stay valid).

Generated code still wins the per-field dispatch argument at 136 B — it has
none — so that tier is parity. Everywhere else the struct encoder's single pass
(where `SerializeToString` walks the message twice: size, then write) and the
decode arena (no per-value malloc, no free walk) dominate.

The harness also prints a `pbwire-c1` block: the one-shot API, which compiles
the layout per call and mallocs per value (2.8x-5x slower). It stays in the
output so that trade-off is visible rather than hidden.

### Go generated codecs vs `google.golang.org/protobuf`

Same `matrix3.proto`, same values, both sides encode identically before timing
(the harness fails otherwise). Go numbers are `make go-bench`, encode and decode
into a fresh message per iteration.

| payload | op | pbwire-go (MB/s, x official) | official generated (MB/s) |
|---|---|---|---|
| 136 B | encode | 1387.8 (2.22x) | 623.9 |
| 136 B | decode | 1088.0 (3.92x) | 277.6 |
| 174 B | encode | 1242.9 (2.59x) | 480.7 |
| 174 B | decode | 801.8 (2.01x) | 399.1 |
| 2469 B | encode | 1522.2 (2.95x) | 516.1 |
| 2469 B | decode | 448.4 (1.59x) | 282.1 |
| 74913 B | encode | 1559.3 (2.85x) | 546.9 |
| 74913 B | decode | 515.3 (1.69x) | 304.1 |

The generated code writes each field as a constant tag byte plus an inline
value append from a pre-sized buffer; the official runtime walks a codec table
(`protoreflect`/`protowire`) with presence and size bookkeeping per field. On
decode the difference is allocation count: the binding appends values into one
slice per repeated field and copies strings once, where the official runtime
allocates per element (and per `[]*T` element pointer).

### Python vs `google.protobuf` (upb)

| payload | op | pbwire-py (MB/s, x upb) | upb (MB/s) |
|---|---|---|---|
| 136 B | encode(data) | 286.4 (5.7x) | 50.1 |
| 136 B | decode(bytes) | 647.5 (1.13x) | 572.3 |
| 136 B | to_dict(bytes) | 163.6 (5.0x) | 32.6 |
| 136 B | encode(parsed) | 2508.5 (4.2x) | 594.5 |
| 174 B | encode(data) | 257.9 (4.9x) | 52.6 |
| 174 B | decode(bytes) | 709.7 (1.38x) | 515.0 |
| 174 B | to_dict(bytes) | 148.8 (5.6x) | 26.5 |
| 174 B | encode(parsed) | 3701.6 (4.6x) | 796.3 |
| 2469 B | encode(data) | 469.3 (6.2x) | 75.3 |
| 2469 B | decode(bytes) | 1101.8 (1.59x) | 693.4 |
| 2469 B | to_dict(bytes) | 187.4 (4.7x) | 39.7 |
| 2469 B | encode(parsed) | 32212.8 (23x) | 1395.3 |
| 74913 B | encode(data) | 554.6 (6.7x) | 83.2 |
| 74913 B | decode(bytes) | 1284.7 (1.65x) | 776.8 |
| 74913 B | to_dict(bytes) | 161.0 (3.9x) | 41.4 |
| 74913 B | encode(parsed) | 90542.6 (63x) | 1438.7 |

Rows are the same user-visible task on both sides:

- **encode(data)** starts from the same dict. upb has no dict entry point, so
  its side is filling a generated message field by field and then serializing;
  that population is 890 µs of the 900 µs on the 75 KB tier. pbwire walks the
  dict once and writes the wire directly.
- **decode(bytes)** ends with an in-memory message. Both sides parse lazily:
  pbwire records one slice per wire field (and validates the whole payload),
  upb parses into its arena.
- **to_dict(bytes)** ends with plain Python data on both sides: `decode(bytes)`
  plus `to_dict()`, against `FromString` plus reading every field through
  `ListFields()`. This is the `malloc`-bound row — 3000 sub-messages mean 3000
  dicts, 12000 ints and 3000 strings on either side, and nobody escapes it.
  (`json_format.MessageToDict` is ~4x slower than reading the fields, so the
  harness does not use it.)
- **encode(parsed)** starts from an in-memory message on both sides. A pbwire
  `Message` keeps the bytes it was parsed from and `encode()` hands them back,
  so the round trip is byte-exact and preserves unknown fields; upb re-encodes
  every field from its own structs.

#### What the optimization pass changed

| version | xlarge encode | xlarge decode |
|---|---|---|
| cffi bridge (one call per value) | 9.3 MB/s | 8.6 MB/s |
| CPython extension, dict-in/dict-out | 586.7 | 202.5 (dict) |
| CPython extension, parsed messages | 554.6 | 1284.7 (message) |

The dict codec was already at the floor for its contract: decoding into plain
dicts costs one dict, one list and one int/str object per value, and the old
`decode()` was within 10% of that floor. That is why it stayed at 0.26-0.4x of
upb — upb never builds those objects. The way out was not a faster loop but a
different representation:

- `decode()` now returns a `Message` that records one 16-byte slice per wire
  field instead of materializing a dict tree: one tag scan, one allocation per
  message (the slices live inside the object up to 16 fields), and the whole
  payload validated while walking it.
- Materialization moved to `Message.to_dict()` / `Message.getattr`, so it only
  happens for fields that are actually read.
- `encode()` accepts a `Message` and returns the retained bytes, which is what
  makes the parse/encode round trip exact (and why the Python conformance
  testee no longer needs a failure list).
- The per-field facts (number, type, wire type, packed/required/oneof, sub-type)
  are resolved once per message type and cached per schema, so
  `schema.message(name)` is a dict lookup and the hot loops never see a
  descriptor.
- Decode pre-sizes the list for a packed run (element count from the payload:
  varints end on a byte without the continuation bit, fixed runs divide) and
  fills it with `PyList_SET_ITEM`; the per-field list table lives on the stack
  for messages with up to 8 fields, which is every sub-message on these tiers.

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
make go-bench     # Go generated codecs vs google.golang.org/protobuf (needs protoc + protoc-gen-go)
make python-bench # Python extension vs google.protobuf (needs protobuf + protoc)
make erlang-bench # Erlang binding: gpb, gpb NIF, enif_protobuf (needs OTP + gpb)
./build/bench bench 2000          # shorter core run (default 200000; xlarge scales /100)
./build/bench_struct_c bench 2000 # shorter C run (same scaling)
```

The Python benchmark calibrates its own iteration count per case, so it needs
no iteration argument; the Go harness uses Go's `testing.Benchmark` calibration.

From `bindings/erlang/benchmarks` (needs Erlang/OTP + gpb, ~30 s per op by
default, `BENCH_TARGET_TIME=3` for a quick run):

```bash
make benchmarks        # gpb, gpb NIF and enif_protobuf on both .dat files
make benchmarks-quick  # same suite, 3 s target per op
```

## Regenerate this file

The core table is `scripts/perf.sh` output (same layout, MB/s only); the C, Go
and Python tables are `make bench-c` / `make go-bench` / `make python-bench`
output reformatted the same way; the Erlang table is copied from
`bindings/erlang/benchmarks/README.md` (`make update-readme` there). Paste all
runs from the same machine and note the date, CPU, `protoc` version and
iteration counts above.

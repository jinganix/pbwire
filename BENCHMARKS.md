# pbwire performance benchmarks

This is the performance document for the whole project: the pbwire core, the C
struct codec and every host binding, all measured on the same four payload
tiers and each against its official counterpart, so the numbers can be read
across languages. The single-glance comparison is the
[at-a-glance table](#at-a-glance-every-binding-one-tier); the rest of the file
is the detail behind it.

- [Standard cases](#standard-cases) — what the four tiers are
- [At a glance](#at-a-glance-every-binding-one-tier) — every binding, one tier
- [Bindings vs their baselines](#bindings-vs-their-baselines) — who is ahead and by how much
- [Core](#core-pbw_ctx-vs-plain-vs-generated) — ctx vs plain vs generated vs `DynamicMessage`, threads, streaming
- [Measurement notes](#measurement-notes-the-core-fast-path) — A/B numbers behind the core's fast paths
- [Typed models](#typed-models-bindingstyped) — what the typed layer costs
- [Per-binding tables](#c-structs-vs-protoc-generated-same-matrix3proto) — one section per language
- [Run them](#run-them) / [Regenerate this file](#regenerate-this-file)

## Standard cases

`small` / `medium` / `large` / `xlarge` scale the repeated fields of each
binding's representative message x0 / x1 / x100 / x3000. Every binding except
the core uses the same `matrix3.proto` schema and the same payload, so their
wire sizes are identical; the core has its own `bench.Msg` because it measures
`DynamicMessage` against generated C++ on the same tree.

| case | repeats | core `bench.Msg` | matrix3 `Scalars3` (every other binding) |
|---|---|---|---|
| small | x0 | 132 B | 136 B |
| medium | x1 | 169 B | 174 B |
| large | x100 | 2755 B | 2469 B |
| xlarge | x3000 | 85933 B | 74913 B |

Objective-C lands on the same four wire sizes by padding the `str` field
instead of wrapping repeated fields, so its xlarge tier is one 74 KB string
rather than 3000 sub-messages. Erlang uses `matrix3.proto` like the others:
the gpb record module is generated from `bindings/c/tests/data/matrix3.proto`
and the payload is built in `proto-bench` from the same values as `matrix3.txt`.

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
below. The Python harness names its ops by the task instead (`encode(data)`,
`decode(bytes)`, `to_dict(bytes)`, `encode(parsed)`), because a dict-based
binding has two representations and only comparing like with like is fair.

## Bindings vs their baselines

| binding | harness | baseline | cases | vs baseline (xlarge) |
|---|---|---|---|---|
| core (dynamic) | `make bench` (`bench/bench.cc`) | `protoc` C++ generated + DynamicMessage | all 4 | 1.5x / 1.4x ✅ |
| C structs | `make bench-c` (`bindings/c/benchmarks/bench_struct.cc`) | `protoc` C++ generated from the same `matrix3.proto` | all 4 | 1.47x / 2.42x ✅ |
| Erlang/OTP NIF | `make benchmarks` in `bindings/erlang/benchmarks` | gpb (pure Erlang), with gpb NIF (libprotobuf) as the second baseline, same `matrix3.proto` | all 4 | 2.16x / 1.57x vs gpb ✅ |
| Python (CPython ext) | `make python-bench` (`bindings/python/benchmarks/bench.py`) | `google.protobuf` (upb C extension) on the same `matrix3.proto` | all 4 | 6.7x / 1.7x ✅ |
| C++ | same engine as C (`bindings/cpp` mirrors the C structs) | — | — | planned |
| Go (generated) | `make go-bench` (`bindings/go/benchmarks`) | `google.golang.org/protobuf` generated code from the same `matrix3.proto` | all 4 | 2.85x / 1.69x ✅ |
| Java (`java.lang.foreign`) | `make java-bench` (`bindings/java/benchmarks`) | `protobuf-java` generated code from the same `matrix3.proto` | all 4 | 1.68x / 1.31x ✅ (0.69x at 136 B) |
| Node (N-API addon) | `make node-bench` (`bindings/node/benchmarks`) | `google-protobuf` (jspb) generated code from the same `matrix3.proto` | all 4 | 23.8x / 19.0x ✅ |
| Ruby (C extension) | `make ruby-bench` (`bindings/ruby/benchmarks`) | the `google-protobuf` gem (C extension) | all 4 | 1.12x / 1.67x ✅ |
| Objective-C (ARC) | `make objc-bench` (`bindings/objc/benchmarks`) | the `protobuf` ObjC runtime + generated code from the same `matrix3.proto` | all 4 | 1.90x / 2.72x ✅ |
| Kotlin (same FFM layer as Java) | `make kotlin-bench` (`bindings/kotlin/benchmarks`) | `protoc --kotlin_out` over `protobuf-java`, same `matrix3.proto` | all 4 | 1.61x / 1.26x ✅ (0.95x at 136 B) |
| PHP (`ext-ffi`) | `make php-bench` (`bindings/php/benchmarks`) | the official PHP runtime (`protobuf/php/src`) + generated code | all 4 | 392x / 101x ✅ |
| Dart (`dart:ffi`) | `make dart-bench` (`bindings/dart/benchmarks`) | the `protobuf` Dart package + generated code | all 4 | 14.0x / 3.1x ✅ |
| Rust (static link) | `make rust-bench` (`bindings/rust/benchmarks`) | rust-protobuf 3.x generated code | all 4 | 1.60x / 1.47x ✅ |
| C# (P/Invoke) | `make csharp-bench` (`bindings/csharp/benchmarks`) | Google.Protobuf C# generated code | all 4 | 2.90x / 1.07x ✅ |

"vs baseline" is encode / decode throughput relative to the baseline on the
largest case the binding runs. `encode` is Python data to wire bytes and
`decode` is wire bytes to an in-memory message, so both sides start and end in
the same place; for the Python row that means `encode(dict)` against upb
filling a message from the same dict and serializing it, and `decode(bytes)`
against `FromString`. Every binding is ahead of its baseline now.

Correctness first everywhere: the core bench verifies byte-for-byte equality
before timing, the C, Go and Python benches check their bytes against the
`protoc`-produced reference (plus a decode→encode round trip) before timing,
the Java, Kotlin, Node, Ruby, Objective-C, PHP, Dart, Rust and C# harnesses
require their bytes to equal the official encoder's bytes before timing
anything, and the Erlang harness requires gpb's pinned wire size and byte
equality (encode, plus re-encoding a decoded reference) from every backend
before timing.

## At a glance: every binding, one tier

One row per implementation, on the largest tier that binding runs, from the
per-binding tables further down (same machine, same run). `x baseline` is the
column to compare: absolute MB/s is not comparable across languages because the
payload, the boundary cost and the baseline all differ.

| binding | reaches the core | xlarge wire size | encode MB/s (x baseline) | decode MB/s (x baseline) |
|---|---|---|---|---|
| core dynamic (`pbw_ctx`) | direct call | 85933 B | 1699.1 (1.46x) | 626.4 (1.43x) |
| core dynamic (plain API) | direct call | 85933 B | 1696.9 (1.46x) | 602.0 (1.37x) |
| C structs | direct call, layout tables | 74913 B | 1318.8 (1.47x) | 910.3 (2.42x) |
| Go | generated straight-line codecs | 74913 B | 1559.3 (2.85x) | 515.3 (1.69x) |
| Python (dict path) | CPython extension | 74913 B | 554.6 (6.7x) | 1284.7 (1.65x) |
| Java | `java.lang.foreign` | 74913 B | 1490.0 (1.68x) | 536.1 (1.31x) |
| Kotlin | the Java FFM layer + DSL | 74913 B | 1405.9 (1.61x) | 534.3 (1.26x) |
| Node | N-API addon | 74913 B | 1366.6 (23.8x) | 1596.7 (19.0x) |
| Ruby | C extension over the core | 74913 B | 1496.8 (1.12x) | 1180.3 (1.67x) |
| Objective-C | ARC wrapper | 74913 B (one 74 KB string) | 75784.5 (1.9x) | 76755.1 (2.7x) |
| PHP | `ext-ffi` | 74913 B | 1412.3 (392.3x) | 533.8 (100.7x) |
| Dart | `dart:ffi` | 74913 B | 1298.9 (14.0x) | 528.5 (3.1x) |
| Rust | static link | 74913 B | 1411.3 (1.60x) | 555.1 (1.47x) |
| C# | P/Invoke | 74913 B | 1345.7 (2.9x) | 533.9 (1.07x) |
| Erlang | NIF (`enif_protobuf`) | 74913 B | 250.2 (2.16x) | 346.2 (1.57x) |

Read it as: the in-process dynamic core sets the pace (1.4-1.5x the C++
generated code at this size), a binding that links the core in (Ruby, Rust)
keeps within a few percent of it, and an FFI binding pays its boundary only
until the payload is large — Java, Kotlin, Ruby, Rust, C# and PHP all land at
~1.3-1.6 GB/s encode and ~535 MB/s decode on the xlarge tier, which is the
core's own number. The outliers are baseline or unit effects, not pbwire
effects: Python's encode column is the dict path (3000 sub-messages, 12000
ints), Node and PHP win by orders of magnitude because jspb and the PHP runtime
are an order of magnitude slower per field, and the Objective-C tier is a
memcpy race over one 74 KB string.

## Measurement notes: the core fast path

The decoder's generic length reader (`pbw_len_read`, in `wire.c`) is an
out-of-line call. Strings and bytes shorter than 128 bytes — most of them —
now take a one-byte inline path in `read_length()` instead. A/B measured with
two bench binaries built from `HEAD` and the change, 8 alternating runs of
200k iterations, median ns/op:

| case | HEAD decode | now decode | delta |
|---|---|---|---|
| 136 B | 131.6 | 127.0 | -3.5% |
| 174 B | 196.2 | 193.0 | -1.6% |
| 2469 B | 4998.9 | 4862.9 | -2.7% |
| 74913 B | 139641.8 | 140036.4 | +0.3% (noise) |

Encode paths are untouched by this change (they moved by the same +/-3% noise
floor, which is how the run was calibrated). Same session, the same change was
rejected for `skip_value()` (unknown fields): no measurable effect, so that
call stays out of line.

Formatting/parsing doubles used `strtod`/`snprintf`, which follow `LC_NUMERIC`:
a host that calls `setlocale(LC_NUMERIC, "de_DE.UTF-8")` made the JSON parser
read `1.5` as `1` and the printers emit `1,5` (invalid JSON). `wire.c` now owns
both conversions (`pbw_strtod`, `pbw_format_g`): the print/parse entry points ask
`pbw_locale_dot()` once, the C-locale path is the same libc call as before and
is inlined, and only a non-'.' separator takes the translating slow path.
Measured with the same A/B method (14 alternating runs, 400k iterations): JSON
parse +0.4%, JSON print +0.7%, text parse +0.3%, text print +0.1%, with the
unmodified reference rows drifting -0.7..+0.5% in the same runs, i.e. inside the
noise floor. The same pass removed the JSON parser's 64-byte number buffer (long
numbers were rejected; the reference has no such limit).

The JSON printer built a per-message `order` array (malloc + insertion sort)
before printing fields, although a linked schema's fields are already sorted by
number (as `link_message()` leaves them). The linked path now prints in place:
one pass, no allocation. Measured two ways, medians of alternating runs:

| measurement | before | after | delta |
|---|---|---|---|
| `make bench` json print (one message) | 1815.3 ns | 1746.5 ns | -3.8% |
| `pbw_json_print` on matrix3 x3000 (3000 messages) | 13.3 ms | 12.6 ms | -5.2% |

The text parser also stopped malloc-ing a buffer per numeric token (stack
buffer first); measured neutral on the bench schema (text parse -0.2%), kept
because it removes one allocation per number.

`pbw_pool_release` grew its idle array with one `realloc` per release, so filling
a pool of N contexts cost O(N^2) copying. It now doubles the slot array. Filling
300 pools x 2048 contexts (acquire all, release all, free the pool):

| build | run 1 | run 2 | run 3 |
|---|---|---|---|
| HEAD | 19.7 ms | 20.0 ms | 19.6 ms |
| now | 13.5 ms | 11.9 ms | 11.8 ms |

The rest of the review (overflow guards in the schema, stream and binding
growth paths, and freeing a nested decoded root's arena) only touches failure
and teardown paths. Re-measured with the same A/B method afterwards: every
row inside the +/-3% noise floor (stream `fed`/`detenc` -1..-4%, `sink`
-0.7%, `framed` -0.9%), so nothing was kept that costs throughput.

## Typed models (bindings/typed)

The generated typed models are ergonomics, not a second hot path: their
`encode`/`decode` call the same dynamic API the tables below measure. The cost
is the per-call type lookup plus, in Python, the dataclass ⇄ dict conversion.
Measured on this machine with `matrix3` x3000 (30 KB) and identical values
through both paths:

| language | encode, typed / dynamic | decode, typed / dynamic |
|---|---|---|
| C (struct binding) | 1.00x (the struct path itself) | 1.00x |
| Go (generated codecs) | 1.00x (no dynamic layer) | 1.00x |
| Java | 1.00x | 1.09x |
| Python (dataclass ⇄ dict) | 3.8x slower | 3.2x slower |

So the numbers below still describe what a typed user pays on the wire; for
Python hot loops the dict path (which the Python row measures) is the fast path.
No binding gained a slower encode/decode path from this layer.

## Cross-binding comparison (last run)

Apple M4 Pro, `-O2`, `protoc 36.2`, protobuf Python 7.36.2 (upb), Erlang
29.0.1. Core/C: 200k iterations (xlarge: 2000); Python and Go calibrate their
own iteration count per case (Go row: Go 1.27.3, `google.golang.org/protobuf`
v1.36.12); Erlang: best of three samples per op, 2 s each. Throughput in MB/s
on serialized wire size. Per-run noise is a few percent; regenerate below for
the current machine.

### Core: `pbw_ctx` vs plain vs generated

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

`8thr agg` is the total throughput of all eight threads. The schema, the
descriptors and the wire input are shared by all threads; each thread only adds
its own `pbw_ctx` and message. pbwire scales 8.2x (encode) and 7.4x (decode)
from 1 to 8 threads in that run; generated code reaches 7.2x and 7.4x. Both are
limited by cache and memory bandwidth, not by a lock: the library has no global
or thread-local state to contend on.

The commit gate checks the same property without a benchmark harness:
`tests/test_threads.c` prints 1-thread vs 8-thread MB/s for the context API (its
own, smaller message, one run on the same machine):

```
scale ctx decode 1 thread: 3453.2 MB/s ( 56.2 ns/op) | 8 threads: 1975.2 MB/s/thread, 15801.3 MB/s aggregate (per-op 1.75x, aggregate 4.58x, 14 cores)
scale ctx encode 1 thread: 6206.0 MB/s ( 31.3 ns/op) | 8 threads: 4528.8 MB/s/thread, 36230.5 MB/s aggregate (per-op 1.37x, aggregate 5.84x, 14 cores)
```

It fails when the aggregate throughput collapses below 0.35x of
`min(8, cores)`, which is what a hidden lock or false sharing would cause.

#### `pbwire ctx` vs `pbwire plain`

The two pbwire columns are the same encoder and decoder with a different memory
and ownership strategy; the benchmark checks that they produce identical bytes
before timing anything. The ownership table is in
[docs/design.md](docs/design.md#two-allocation-strategies); on the wire it shows
up as the `malloc`/`free` pair per call, which dominates at 132-169 B (plain is
0.96x-1.04x of generated code there) and disappears by 2755 B, where both
columns are within a few percent of each other.

Read it as:

- **Against generated code**: the context API wins every binary cell — encode
  by 1.21x-1.46x, decode by 1.26x-1.70x. There is no tier where a compiled
  straight-line writer beats it outright, because the C++ serializer walks the
  message twice (size, then write) while pbwire writes one pass.
- **Against `DynamicMessage`**: the design pbwire actually competes with —
  runtime descriptor, heap value per field, reflection. pbwire is 2.2-5.1x
  faster there (decode) and 11.6-19.2x on encode.
- **The plain column** pays exactly one `malloc`/`free` pair per call (decoded
  tree, output buffer), which is why it trails generated code on the two small
  encodes and wins everything at 2755 B and up. Use the plain API for one-off
  work and long-lived results.

#### How the core gap was closed

Since `make bench` is the reason these numbers are trustworthy, the changes
that produced them, each verified byte-for-byte against libprotobuf:

1. A decoded message is one allocation: struct, value slots, presence bitmap and
   oneof cases share one block, and values are stored inline (a bitmap says
   which slots hold a value) instead of one `malloc` per field. The tree's arena
   struct, its first chunk and the tree itself are one block too, so plain
   `pbw_decode` is a single `malloc`.
2. The `pbw_ctx` API reuses those arenas and the output buffer: `pbw_ctx_reset`
   is a pointer move. Oversized single allocations bypass the reusable chunk
   (freed on reset), and `pbw_ctx_set_retain_cap` bounds what a reset keeps, so
   steady workloads stop calling `malloc` without pinning outliers.
3. Encoding computes no size pass: it writes into a growable buffer, puts a
   one-byte placeholder where a nested length goes and patches it afterwards
   (strings and bytes write their known length up front). Per-field tag bytes
   and the packed variant are precomputed at `pbw_schema_link()` and stored with
   one 32-bit store.
4. Varint and tag readers/writers are inlined with one- and two-byte fast paths
   and an unrolled ten-byte path for long (negative int32/int64) values; the
   common "singular scalar, expected wire type, first occurrence" case writes
   the value slot directly.
5. `pbw_schema_link()` also builds a dense field-number table of field pointers,
   a 128-entry one-byte tag table, and a per-tag decode action, so the hot loop
   dispatches a field with two indexed loads and runs it without re-checking its
   type, label, oneof membership or enum openness.

#### Streaming and framing

`make bench` also prints a `stream` table for the same payload: deterministic
encode ties plain encode (no maps in that message, so it is one branch), `size`
(`pbw_message_encoded_size`) costs about 0.75 of an encode because it walks the
same tree without writing, the sink costs one pass over the bytes, and a
delimited feed/next round-trip costs about one encode plus one decode. Ratios
only — absolute numbers move between machines.

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

### Java, Node, Ruby and Objective-C vs their official runtimes

All four load a `FileDescriptorSet` at run time. Java, Node and Objective-C call
the C ABI from an FFI runtime (Java through `java.lang.foreign`, Node through an
N-API addon, Objective-C through an ARC wrapper), so those tables measure the
cost of that boundary on top of the same C core. Ruby is a C extension that links
the core statically, so it pays no boundary cost at all. Java, Node and Ruby run
the repeated-field payload; Objective-C pads `str` to the same wire sizes (see
Standard cases), which makes its xlarge tier a memcpy.

#### Java vs `protobuf-java`

| payload | op | pbwire-java (MB/s, x protobuf-java) | protobuf-java (MB/s) |
|---|---|---|---|
| 136 B | encode | 1579.8 (0.69x) | 2304.8 |
| 136 B | decode | 799.0 (1.39x) | 573.8 |
| 174 B | encode | 1552.5 (1.26x) | 1228.6 |
| 174 B | decode | 715.5 (1.24x) | 575.8 |
| 2469 B | encode | 1548.7 (1.78x) | 871.2 |
| 2469 B | decode | 484.7 (1.20x) | 402.7 |
| 74913 B | encode | 1490.0 (1.68x) | 886.9 |
| 74913 B | decode | 536.1 (1.31x) | 409.5 |

The wrapper calls every C function through `invokeExact` on a `static final`
`MethodHandle` and keeps one workspace per thread (a `pbw_ctx`, whose buffer
`pbw_encode_ctx` reuses, plus the C out-parameters and the decode input copy),
so an encode or decode is one downcall plus one `byte[]` copy: ~90 ns and
~180 ns at 136 B, against ~320 ns and ~340 ns before that workspace existed.
Only the 136 B encode still loses: generated `protobuf-java` writes that message
as one straight-line block of known tags and values (~62 ns), while the dynamic
tree walk pays a presence test, a type dispatch and a re-derived varint size per
field. That is not dispatch overhead that could be tuned away: a prototype
encoder that walks the same tree with the wire type preloaded, no per-field
capacity check and no `switch` on the field type still needs 66.5 ns against
70.4 ns for `src/encode.c` on the same 136-byte payload, so the remaining ~25 ns
is the boundary (~15-19 ns for the downcall, the two out-parameter reads and the
`byte[]` copy) plus the ~10 ns a generated writer saves by knowing its tags. From 174 B up the C core's single pass wins, and by 2.5 KB the size pass
`protobuf-java` runs before writing (3000 nested messages at the xlarge tier)
costs more than the boundary does.

Measured with the machine otherwise busy (load average ~10), so the absolute
MB/s are conservative; the ratios are the point of the table.

#### Node vs `google-protobuf` (jspb)

| payload | op | pbwire-node (MB/s, x jspb) | jspb (MB/s) |
|---|---|---|---|
| 136 B | encode | 579.6 (10.4x) | 55.8 |
| 136 B | decode | 371.0 (3.3x) | 113.4 |
| 174 B | encode | 531.9 (9.5x) | 55.9 |
| 174 B | decode | 419.1 (3.9x) | 107.1 |
| 2469 B | encode | 1222.6 (22.8x) | 53.6 |
| 2469 B | decode | 1102.8 (14.0x) | 78.8 |
| 74913 B | encode | 1366.6 (23.8x) | 57.4 |
| 74913 B | decode | 1596.7 (19.0x) | 84.1 |

jspb's throughput does not move with payload size: its writer resolves a
code/type table per field and hands every repeated element to a closure, where
the C core writes one tag byte and one inline value, or memcpys an already
validated slice.

What the gap is made of, and what the optimization pass changed:

| version | xlarge encode | xlarge decode |
|---|---|---|
| N-API addon, tree per message | 1300.4 | 565.2 |
| encode into a reusable buffer, wire view for decoded messages | 1366.6 | 1596.7 |

- `decode()` returns the bytes plus one 12-byte slice per wire field instead of
  materializing a tree: one validating tag scan, one allocation for the slice
  array, and the bytes copied once with `Buffer.from`. A value materializes only
  when it is read. Byte-for-byte re-encoding falls out of it, so no unknown-field
  bookkeeping is needed either.
- The wire form keeps no native state (JS `Buffer` + `Uint32Array`), because an
  N-API finalizer costs ~250 ns per object — more than the whole decode of a
  136 B message. Only the built form, which has no JS-visible storage, is
  wrapped.
- `encode()` writes into the type's reusable core buffer and copies into a
  JS-allocated (pooled) `Buffer` of the exact size: N-API's own `Buffer`
  creation is ~200 ns, versus ~26 ns for a pooled allocation.
- Field facts are resolved once per descriptor into a cached table (name hash,
  direct number index, wire type, label, oneof, closed enum), so the scan and
  the accessors never walk the descriptor.

#### Ruby (C extension) vs the `google-protobuf` gem

| payload | op | pbwire-ruby (MB/s, x gem) | gem (MB/s) |
|---|---|---|---|
| 136 B | encode | 891.2 (1.25x) | 715.8 |
| 136 B | decode | 414.0 (1.66x) | 250.1 |
| 174 B | encode | 888.2 (1.16x) | 764.2 |
| 174 B | decode | 443.9 (1.77x) | 251.1 |
| 2469 B | encode | 1278.2 (1.04x) | 1228.7 |
| 2469 B | decode | 943.9 (1.93x) | 489.8 |
| 74913 B | encode | 1496.8 (1.12x) | 1333.3 |
| 74913 B | decode | 1180.3 (1.67x) | 707.1 |

Ruby is a C extension over the same core, so this is C extension against C
extension. The first version drove the same C ABI through stdlib Fiddle and lost
every row by an order of magnitude: a downcall costs ~3 µs, so a 136 B encode was
3.3 µs against the core's ~65 ns, and the tiers could only be compared by padding
`str` because the wrapper had no repeated fields. The rewrite removed the
boundary, added the repeated-field payload (so the xlarge tier is 3000 nested
messages, not one 74 KB string), and split the message into two representations:

- **Encoding** writes one pass from the C message tree straight into the String
  that is returned: the builder keeps the last encoded length as the next
  buffer-size hint, `pbw_encode_to` writes into a Ruby string's bytes (no
  intermediate buffer, no second copy), and a message that needs more room retries
  with a larger string. Packed runs compute their exact payload size first
  instead of reserving a 10-bytes-per-element worst case, so the write fits the
  hint and no `memmove` patches the length prefix.
- **Decoding** is one validating tag scan that records, per wire field, where its
  value lives (a 32-byte slice, 32 of them inline). The gem materializes every
  field; this binding materializes on `get`, and `encode` on an unmodified wire
  view hands the bytes back byte for byte, which is also what preserves unknown
  fields and non-canonical encodings. That is where the 1.7-1.9x decode numbers
  come from at 174 B and up, where sub-message tree building used to dominate.
- Field facts (number, kind, wire type, packed/required/oneof, sub type, a
  name -> index Hash) are resolved once per message type into a per-schema cache,
  so the scan and the accessors never walk the descriptor.

The 2469 B encode is the closest row (1.04x): the gem's encoder is also a single
pass, and at that size the tree walk and the generated straight-line writes are
even. Above it the C core's pack-aware writer pulls ahead again (1.12x).

#### Objective-C vs the `protobuf` ObjC runtime

| payload | op | pbwire-objc (MB/s, x protobuf-objc) | protobuf-objc (MB/s) |
|---|---|---|---|
| 136 B | encode | 976.0 (5.9x) | 164.8 |
| 136 B | decode | 647.2 (2.4x) | 267.1 |
| 174 B | encode | 1373.5 (6.4x) | 214.3 |
| 174 B | decode | 832.6 (2.8x) | 299.1 |
| 2469 B | encode | 13543.6 (4.7x) | 2890.1 |
| 2469 B | decode | 10337.0 (2.7x) | 3857.8 |
| 74913 B | encode | 75784.5 (1.9x) | 39783.9 |
| 74913 B | decode | 76755.1 (2.7x) | 28242.4 |

The ARC wrapper adds one Objective-C object per call and no copy: `encode` hands
the encoder's own malloc'd buffer to `NSData` with
`dataWithBytesNoCopy:length:freeWhenDone:`, and `decode` wraps the decoder's
arena in one `PBWMessage`. That stays ahead of the generated runtime on all four
tiers and both ops (5.9x / 2.4x at 136 B down to 1.9x / 2.7x at 74913 B). The
xlarge tier is one 74 KB string, so it is a memcpy race: the generated encoder
writes it into an `NSMutableData` of the final size, and the wrapper writes it
into its own buffer, which `NSData` then takes over.

#### Kotlin vs `protoc --kotlin_out` over `protobuf-java`

| payload | op | pbwire-kotlin (MB/s, x protobuf-kotlin) | protobuf-kotlin (MB/s) |
|---|---|---|---|
| 136 B | encode | 1492.7 (0.95x) | 1577.9 |
| 136 B | decode | 713.2 (1.25x) | 572.4 |
| 174 B | encode | 1411.5 (1.11x) | 1269.6 |
| 174 B | decode | 627.5 (1.20x) | 523.5 |
| 2469 B | encode | 1375.8 (1.62x) | 849.8 |
| 2469 B | decode | 477.4 (1.06x) | 451.5 |
| 74913 B | encode | 1405.9 (1.61x) | 872.3 |
| 74913 B | decode | 534.3 (1.26x) | 425.4 |

Kotlin reuses the Java FFM layer (`bindings/java/runtime`), so these numbers are
`pbwire-java`'s: one downcall plus one `byte[]` copy per call. The 136 B encode
is the case the Java section explains (a generated writer knows its tags, the
dynamic tree walk derives them), here 0.95x rather than 0.69x because the
Kotlin baseline reaches the same protobuf-java encoder through its builder/DSL
path. From 174 B up both ops are ahead at every tier.

#### PHP vs the official PHP runtime

| payload | op | pbwire-php (MB/s, x protobuf-php) | protobuf-php (MB/s) |
|---|---|---|---|
| 136 B | encode | 190.2 (51.4x) | 3.7 |
| 136 B | decode | 186.3 (29.6x) | 6.3 |
| 174 B | encode | 231.3 (62.5x) | 3.7 |
| 174 B | decode | 221.8 (37.0x) | 6.0 |
| 2469 B | encode | 909.1 (259.7x) | 3.5 |
| 2469 B | decode | 438.7 (87.7x) | 5.0 |
| 74913 B | encode | 1412.3 (392.3x) | 3.6 |
| 74913 B | decode | 533.8 (100.7x) | 5.3 |

`ext-ffi` puts the C core behind one PHP call per set/get and hands whole
buffers across, so the per-field PHP loop disappears: the official runtime is
pure PHP and spends its time on per-field bookkeeping. The gap widens with size
because the PHP runtime's cost is per field and value, not per byte.

#### Dart vs the `protobuf` Dart package

| payload | op | pbwire-dart (MB/s, x protobuf-dart) | protobuf-dart (MB/s) |
|---|---|---|---|
| 136 B | encode | 761.3 (3.8x) | 201.4 |
| 136 B | decode | 350.0 (1.6x) | 215.6 |
| 174 B | encode | 700.4 (4.0x) | 173.9 |
| 174 B | decode | 358.6 (2.0x) | 182.6 |
| 2469 B | encode | 996.0 (8.8x) | 113.0 |
| 2469 B | decode | 434.1 (3.0x) | 144.3 |
| 74913 B | encode | 1298.9 (14.0x) | 93.1 |
| 74913 B | decode | 528.5 (3.1x) | 169.3 |

Both sides allocate per call. `dart:ffi` adds a `malloc`/copy for the input
bytes and a `Uint8List` copy for the output, which is why the gain grows with
payload size: the Dart runtime's pure-Dart field loop dominates it by 2.5 KB,
and at 136 B the boundary is still visible in a 1.6x decode.

#### Rust vs `rust-protobuf` 3.x

| payload | op | pbwire-rust (MB/s, x protobuf-rust) | protobuf-rust (MB/s) |
|---|---|---|---|
| 136 B | encode | 1387.8 (1.14x) | 1212.9 |
| 136 B | decode | 886.3 (1.20x) | 737.1 |
| 174 B | encode | 1336.7 (1.21x) | 1107.2 |
| 174 B | decode | 751.9 (1.28x) | 588.7 |
| 2469 B | encode | 1277.9 (1.54x) | 827.7 |
| 2469 B | decode | 490.6 (1.34x) | 365.2 |
| 74913 B | encode | 1411.3 (1.60x) | 879.5 |
| 74913 B | decode | 555.1 (1.47x) | 377.2 |

The archive is linked in, so a call is a direct C call with no `dlopen` lookup:
the smallest tier is already 1.14x, which is the C core's own lead over
rust-protobuf's generated codecs plus the `Vec` copy `encode()` makes.

#### C# vs Google.Protobuf

| payload | op | pbwire-csharp (MB/s, x protobuf-csharp) | protobuf-csharp (MB/s) |
|---|---|---|---|
| 136 B | encode | 1217.3 (2.8x) | 437.4 |
| 136 B | decode | 796.6 (2.8x) | 287.4 |
| 174 B | encode | 1253.9 (3.1x) | 401.5 |
| 174 B | decode | 703.3 (2.4x) | 291.0 |
| 2469 B | encode | 1177.6 (2.1x) | 553.1 |
| 2469 B | decode | 474.5 (0.98x) | 484.0 |
| 74913 B | encode | 1345.7 (2.9x) | 464.7 |
| 74913 B | decode | 533.9 (1.07x) | 499.6 |

Google.Protobuf's C# runtime is already a fast tree codec, so this is the
closest race of the five. The one cell under parity (2469 B decode, 0.98x) is
inside run-to-run noise -- the xlarge decode is 1.07x on the same code path --
and the encode side stays ~2-3x ahead at every tier.

All five land at ~1.4 GB/s encode and ~535 MB/s decode on the xlarge tier, which
is the C core's own number: by 2.5 KB the per-call boundary is invisible and only
the baseline's own codec is being measured. That is the point of the schema-at-
runtime design -- the binding owns ergonomics, the core owns the format.

### Erlang: enif_protobuf vs gpb vs gpb NIF

`matrix3.proto`, all four tiers, best of three samples per op:

| Test case | epb | gpb | gpb nif |
|-----------|-----|-----|---------|
| small · 136 B · encode | 328.4 | 169.9 | 61.0 |
| small · 136 B · decode | 483.6 | 221.9 | 66.4 |
| medium · 174 B · encode | 298.3 | 148.5 | 71.3 |
| medium · 174 B · decode | 500.4 | 210.1 | 82.1 |
| large · 2469 B · encode | 235.3 | 124.5 | 94.5 |
| large · 2469 B · decode | 337.6 | 196.8 | 177.0 |
| xlarge · 74913 B · encode | 250.2 | 115.9 | 118.3 |
| xlarge · 74913 B · decode | 346.2 | 220.8 | 197.1 |

## Run them

```bash
make bench        # core: 4 cases + text/json/stream/threads (needs protoc + libprotobuf)
make bench-c      # C structs vs protoc C++ on matrix3.proto (same deps)
make go-bench     # Go generated codecs vs google.golang.org/protobuf (needs protoc + protoc-gen-go)
make python-bench # Python extension vs google.protobuf (needs protobuf + protoc)
make erlang-bench # Erlang binding: epb, gpb, gpb NIF on matrix3.proto (needs OTP + gpb)
make java-bench   # Java FFM vs protobuf-java (fetches the jar from Maven Central once)
make node-bench   # N-API addon vs google-protobuf/jspb (npm installs into the harness)
make ruby-bench   # Ruby C extension vs the google-protobuf gem (`RUBY=...` picks the interpreter)
make objc-bench   # ARC wrapper vs the protobuf ObjC runtime (needs `make conformance-setup`)
make kotlin-bench # Kotlin DSL vs protoc --kotlin_out over protobuf-java (fetches two jars once)
make php-bench    # ext-ffi binding vs the official PHP runtime (needs `make conformance-setup`)
make dart-bench   # dart:ffi binding vs the protobuf Dart package (needs `dart pub global activate protoc_plugin`)
make rust-bench   # static-link binding vs rust-protobuf (needs `cargo install protobuf-codegen`)
make csharp-bench # P/Invoke binding vs Google.Protobuf (NuGet restore on first run)
./build/bench bench 2000          # shorter core run (default 200000; xlarge scales /100)
./build/bench_struct_c bench 2000 # shorter C run (same scaling)
```

The Python benchmark calibrates its own iteration count per case, so it needs
no iteration argument; the Go harness uses Go's `testing.Benchmark` calibration.
The four FFI harnesses use a fixed iteration count per tier (printed in each
block) and report the best of a warm-up pass plus five interleaved timed passes,
so the JIT has settled and a load spike hits both implementations equally.

From `bindings/erlang/benchmarks` (needs Erlang/OTP + gpb; the gpb NIF baseline
additionally needs protoc + libprotobuf C++, and is skipped without them).
Each op is the best of three samples, each `BENCH_TARGET_TIME` seconds long
(default 2):

```bash
make benchmarks        # epb, gpb and (when built) gpb NIF on matrix3.proto
make benchmarks-quick  # same suite with BENCH_TARGET_TIME=0.2
```

## Regenerate this file

The core tables (single-thread and threads) are `scripts/perf.sh` output (same
layout, MB/s only); the C, Go,
Python, Java, Kotlin, Node, Ruby, Objective-C, PHP, Dart, Rust and C# tables are
`make bench-c` / `make go-bench` / `make python-bench` / `make java-bench` /
`make kotlin-bench` / `make node-bench` / `make ruby-bench` / `make objc-bench` /
`make php-bench` / `make dart-bench` / `make rust-bench` / `make csharp-bench`
output reformatted the same way; the
Erlang table is copied from
`bindings/erlang/benchmarks/README.md` (`make update-readme` there). Rebuild the
at-a-glance table from those rows afterwards — it only copies the xlarge line of
each. Paste all runs from the same machine and note the date, CPU, `protoc`
version and iteration counts above.

`make bench` and `make difftest` are correctness-checked before they report a
number, so a stale or hand-edited row is worse than a missing one: paste the
harness output, do not transcribe by hand.

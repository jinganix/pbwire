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
- [Per-binding tables](#cross-binding-comparison-last-run) — one section per language (✅ exceeds baseline ≥1.0x, ❌ below):
  - [Core](#core-pbw_ctx-vs-plain-vs-generated) · [C structs](#c-structs-vs-protoc-generated-same-matrix3proto) · [Go](#go-cgo-dynamic) · [Python](#python-vs-googleprotobuf-upb)
  - [Java](#java-vs-protobuf-java) · [Node](#node-vs-google-protobuf-jspb) · [Ruby](#ruby-c-extension-vs-the-google-protobuf-gem) · [Objective-C](#objective-c-vs-the-protobuf-objc-runtime)
  - [Kotlin](#kotlin-vs-protoc---kotlin_out-over-protobuf-java) · [PHP](#php-vs-the-official-php-runtime-pure--c-extension) · [Dart](#dart-vs-the-protobuf-dart-package) · [Rust](#rust-vs-rust-protobuf-3x) · [C#](#c-vs-googleprotobuf) · [Erlang](#erlang-pbwire-vs-gpb-vs-gpb-nif)
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
| core (dynamic) | `make bench` (`bench/bench.cc`) | `protoc` C++ generated + DynamicMessage | all 4 | 1.3x / 1.4x ✅ |
| C structs | `make bench-c` (`bindings/c/benchmarks/bench_struct.cc`) | `protoc` C++ generated from the same `matrix3.proto` | all 4 | 1.42x / 2.25x ✅ |
| Erlang/OTP NIF | `make benchmarks` in `bindings/erlang/benchmarks` | gpb (pure Erlang), with gpb NIF (libprotobuf) as the second baseline, same `matrix3.proto` | all 4 | 2.04x / 1.68x vs gpb ✅ |
| Python (CPython ext) | `make python-bench` (`bindings/python/benchmarks/bench.py`) | `google.protobuf` (upb C extension) on the same `matrix3.proto` | all 4 | 6.5x / 1.69x ✅ |
| C++ | same engine as C (`bindings/cpp` mirrors the C structs) | — | — | planned |
| Go (`cgo` dynamic) | `go test ./...` in `bindings/go` (parity + hot-swap; no standard bench yet) | — | — | rebench pending |
| Java (pure-Java codec) | `make java-bench` (`bindings/java/benchmarks`) | `protobuf-java` generated code from the same `matrix3.proto` | all 4 | 1.21x / 2.40x ✅ (0.75x at 136 B) |
| Node (N-API addon) | `make node-bench` (`bindings/node/benchmarks`) | `google-protobuf` (jspb) + protobufjs (fastest) from the same `matrix3.proto` | all 4 | 22.3x / 20.0x vs jspb (2.7x / 4.1x vs protobufjs) ✅ |
| Ruby (C extension) | `make ruby-bench` (`bindings/ruby/benchmarks`) | the `google-protobuf` gem (C extension) | all 4 | 1.05x / 1.69x ✅ |
| Objective-C (ARC) | `make objc-bench` (`bindings/objc/benchmarks`) | the `protobuf` ObjC runtime + generated code from the same `matrix3.proto` | all 4 | 1.8x / 2.7x ✅ |
| Kotlin (same FFM layer as Java) | `make kotlin-bench` (`bindings/kotlin/benchmarks`) | `protoc --kotlin_out` over `protobuf-java`, same `matrix3.proto` | all 4 | 1.60x / 1.19x ✅ (0.67x at 136 B) |
| PHP (`ext-ffi` + C extension) | `make php-bench` (`bindings/php/benchmarks`) | the official PHP runtime (`protobuf/php/src`): pure-PHP + C extension (fastest) | all 4 | 374x / 127x vs pure (1.11x / 1.15x vs C ext) ✅ |
| Dart (`dart:ffi`) | `make dart-bench` (`bindings/dart/benchmarks`) | the `protobuf` Dart package + generated code | all 4 | 12.2x / 3.3x ✅ |
| Rust (static link) | `make rust-bench` (`bindings/rust/benchmarks`) | rust-protobuf 3.x generated code + prost (fastest) | all 4 | 1.57x / 1.45x vs rust-protobuf (0.93x / 1.46x vs prost) ✅ |
| C# (P/Invoke) | `make csharp-bench` (`bindings/csharp/benchmarks`) | Google.Protobuf C# generated code + protobuf-net (fastest) | all 4 | 2.90x / 1.07x vs official (3.6x / 1.62x vs protobuf-net) ✅ |

"vs baseline" is encode / decode throughput relative to the baseline on the
largest case the binding runs. `encode` is Python data to wire bytes and
`decode` is wire bytes to an in-memory message, so both sides start and end in
the same place; for the Python row that means `encode(dict)` against upb
filling a message from the same dict and serializing it, and `decode(bytes)`
against `FromString`. Every binding is ahead of its baseline now.

Correctness first everywhere: the core bench verifies byte-for-byte equality
before timing, the C and Python benches check their bytes against the
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
| core dynamic (`pbw_ctx`) | direct call | 85933 B | 1540.6 (1.32x ✅) | 642.3 (1.41x ✅) |
| core dynamic (plain API) | direct call | 85933 B | 1429.5 (1.23x ✅) | 619.9 (1.36x ✅) |
| C structs | direct call, layout tables | 74913 B | 1300.6 (1.42x ✅) | 885.4 (2.25x ✅) |
| Go | `cgo` dynamic over the C core | — (rebench pending) | — | — |
| Python (dict path) | CPython extension | 74913 B | 564.4 (6.5x ✅) | 1366.0 (1.69x ✅) |
| Java | pure-Java codec, no native code | 74913 B | 1082.8 (1.21x ✅) | 953.1 (2.40x ✅) |
| Kotlin | the Java FFM layer + DSL | 74913 B | 1441.1 (1.60x ✅) | 549.1 (1.19x ✅) |
| Node | N-API addon | 74913 B | 1350.2 (22.3x ✅) | 1655.0 (20.0x ✅) |
| Ruby | C extension over the core | 74913 B | 1427.1 (1.05x ✅) | 1186.7 (1.69x ✅) |
| Objective-C | ARC wrapper | 74913 B (one 74 KB string) | 71739.6 (1.8x ✅) | 76007.7 (2.7x ✅) |
| PHP | C extension | 74913 B | 1478.3 (1.11x ✅) | 661.1 (1.15x ✅) |
| Dart | `dart:ffi` | 74913 B | 1273.8 (12.2x ✅) | 555.9 (3.3x ✅) |
| Rust | static link | 74913 B | 1423.0 (1.57x ✅) | 575.8 (1.45x ✅) |
| C# | P/Invoke | 74913 B | 1393.5 (2.90x ✅) | 547.1 (1.07x ✅) |
| Erlang | NIF (`pbwire`) | 74913 B | 251.2 (2.04x ✅) | 365.2 (1.68x ✅) |

Read it as: the in-process dynamic core sets the pace (1.4x the C++
generated code at this size), a binding that links the core in (Ruby, Rust)
keeps within a few percent of it, and an FFI binding pays its boundary only
until the payload is large — Java, Kotlin, Ruby, Rust, C# and PHP all land at
~1.3-1.6 GB/s encode and ~535-660 MB/s decode on the xlarge tier, which is the
core's own number. The outliers are baseline or unit effects, not pbwire
effects: Python's encode column is the dict path (3000 sub-messages, 12000
ints), and the Objective-C tier is a memcpy race over one 74 KB string.
Node's ~22x and PHP's ~374x are against legacy baselines (jspb, pure-PHP
runtime); against the fastest baselines it is 2.7x / 4.1x over protobufjs
and 1.11x / 1.15x over the PHP C extension — see their sections.

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

The decoder used to inline fast paths only for one-byte tags and run everything
else through per-item generic calls; two-byte tags carried a second copy of the
inline bodies. All tag sizes now share one link-time action switch (the action
lives on the field, covering singular and repeated shapes), repeated appends
reuse a cached list slot instead of the get-or-create call, and packed runs
append through reserved list storage with no call per item. Measured two ways:
a C best-of-5 of decode+free on the 74913-byte matrix3 payload went 141.1 us to
107.4 us (-24%), and the PHP binding's full-xlarge decode over 10 interleaved
rounds against the official C extension moved from 0.91x to 1.10-1.19x. Packed
encode got the same treatment (one dispatch per run, blind kind-specific write
loops over a hoisted cursor): packed bool x3000 went 4.8 us to 1.1 us, packed
int32 x3000 7.0 us to 3.5 us, both best-of-5. Unpacked-item loops were tried
hoisted the same way and reverted: the bigger `encode_message` cost more in
icache and register pressure than the per-item switch saved, measured -33% on
the core bench's unpacked-heavy message.

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
| Java | 1.00x | 1.09x |
| Python (dataclass ⇄ dict) | 3.8x slower | 3.2x slower |

So the numbers below still describe what a typed user pays on the wire; for
Python hot loops the dict path (which the Python row measures) is the fast path.
No binding gained a slower encode/decode path from this layer.

## Cross-binding comparison (last run)

Per-language index: [Core](#core-pbw_ctx-vs-plain-vs-generated) · [C structs](#c-structs-vs-protoc-generated-same-matrix3proto) · [Go](#go-cgo-dynamic) · [Python](#python-vs-googleprotobuf-upb) · [Java](#java-vs-protobuf-java) · [Node](#node-vs-google-protobuf-jspb) · [Ruby](#ruby-c-extension-vs-the-google-protobuf-gem) · [Objective-C](#objective-c-vs-the-protobuf-objc-runtime) · [Kotlin](#kotlin-vs-protoc---kotlin_out-over-protobuf-java) · [PHP](#php-vs-the-official-php-runtime-pure--c-extension) · [Dart](#dart-vs-the-protobuf-dart-package) · [Rust](#rust-vs-rust-protobuf-3x) · [C#](#c-vs-googleprotobuf) · [Erlang](#erlang-pbwire-vs-gpb-vs-gpb-nif). In every table below, ✅ means that cell beats its baseline (≥1.0x), ❌ means it does not.

Apple M4 Pro, `-O2`, `protoc 36.2`, protobuf Python 7.36.2 (upb), Erlang
29.0.1. Every harness calibrates from its tier floor until one pass takes at
least 0.25 s, then reports the best of 3 passes (Erlang uses the same bar
with a 0.25 s sample time). Throughput in MB/s
on serialized wire size. Per-run noise is a few percent; regenerate below for
the current machine.

### Core: `pbw_ctx` vs plain vs generated

| payload | op | pbwire ctx (MB/s, x gen) | pbwire plain (MB/s, x gen) | protoc gen (MB/s) | DynamicMessage (MB/s) |
|---|---|---|---|---|---|
| 132 B | encode | 2160.7 (1.33x ✅) | 1648.8 (1.01x ✅) | 1625.3 | 110.7 |
| 132 B | decode | 1594.7 (1.71x ✅) | 1040.4 (1.12x ✅) | 932.9 | 292.7 |
| 169 B | encode | 1925.4 (1.14x ✅) | 1695.7 (1.01x ✅) | 1685.5 | 110.6 |
| 169 B | decode | 1218.0 (1.49x ✅) | 884.0 (1.08x ✅) | 818.0 | 279.1 |
| 2755 B | encode | 1445.1 (1.22x ✅) | 1154.7 (0.97x ❌) | 1184.4 | 135.4 |
| 2755 B | decode | 563.1 (1.19x ✅) | 562.0 (1.19x ✅) | 472.6 | 262.3 |
| 85933 B | encode | 1540.6 (1.32x ✅) | 1429.5 (1.23x ✅) | 1163.6 | 151.2 |
| 85933 B | decode | 642.3 (1.41x ✅) | 619.9 (1.36x ✅) | 455.2 | 269.8 |

Threads (169 B, 1 vs 8):

| op | pbwire 1thr (MB/s, x gen) | pbwire 8thr agg (MB/s, x gen) | gen 1thr (MB/s) | gen 8thr agg (MB/s) |
|---|---|---|---|---|
| encode | 1925.9 (1.11x ✅) | 13294.2 (1.51x ✅) | 1736.5 | 8784.1 |
| decode | 1176.3 (1.46x ✅) | 7753.7 (1.23x ✅) | 806.3 | 6311.8 |

`8thr agg` is the total throughput of all eight threads. The schema, the
descriptors and the wire input are shared by all threads; each thread only adds
its own `pbw_ctx` and message. pbwire scales 6.9x (encode) and 6.6x (decode)
from 1 to 8 threads in that run; generated code reaches 5.1x and 7.8x. Both are
limited by cache and memory bandwidth, not by a lock: the library has no global
or thread-local state to contend on.

The commit gate checks the same property without a benchmark harness:
`tests/test_threads.c` prints 1-thread vs 8-thread MB/s for the context API (its
own, smaller message, one run on the same machine):

```
scale ctx decode 1 thread: 3370.4 MB/s ( 57.6 ns/op) | 8 threads: 2916.1 MB/s/thread, 23328.7 MB/s aggregate (per-op 1.16x, aggregate 6.92x, 14 cores)
scale ctx encode 1 thread: 6354.4 MB/s ( 30.5 ns/op) | 8 threads: 5621.5 MB/s/thread, 44971.7 MB/s aggregate (per-op 1.13x, aggregate 7.08x, 14 cores)
```

It fails when the aggregate throughput collapses below 0.35x of
`min(8, cores)`, which is what a hidden lock or false sharing would cause.

#### `pbwire ctx` vs `pbwire plain`

The two pbwire columns are the same encoder and decoder with a different memory
and ownership strategy; the benchmark checks that they produce identical bytes
before timing anything. The ownership table is in
[docs/design.md](docs/design.md#two-allocation-strategies); on the wire it shows
up as the `malloc`/`free` pair per call, which is why plain sits at parity on
the small tiers (1.01x), trails the 2755 B encode (0.97x), and pulls ahead on
the xlarge tier, where both
columns are within a few percent of each other.

Read it as:

- **Against generated code**: the context API wins every binary cell — encode
  by 1.14x-1.33x, decode by 1.19x-1.71x. There is no tier where a compiled
  straight-line writer beats it outright, because the C++ serializer walks the
  message twice (size, then write) while pbwire writes one pass.
- **Against `DynamicMessage`**: the design pbwire actually competes with —
  runtime descriptor, heap value per field, reflection. pbwire is 2.1-3.6x
  faster there (decode) and 8.5-15.3x on encode.
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
   a 128-entry one-byte tag table, and a per-field decode action covering every
   tag size, so the hot loop dispatches a field with two indexed loads and runs
   it without re-checking its type, label, oneof membership or enum openness.
   Repeated appends hit a cached list slot instead of the get-or-create call.

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
| 136 B | encode | 1354.7 (0.96x ❌) | 1405.2 |
| 136 B | decode | 1123.1 (1.32x ✅) | 848.4 |
| 174 B | encode | 1436.5 (1.06x ✅) | 1350.2 |
| 174 B | decode | 1049.0 (1.35x ✅) | 777.9 |
| 2469 B | encode | 1043.0 (1.17x ✅) | 891.5 |
| 2469 B | decode | 786.7 (2.01x ✅) | 391.3 |
| 74913 B | encode | 1300.6 (1.42x ✅) | 913.2 |
| 74913 B | decode | 885.4 (2.25x ✅) | 393.6 |

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

### Go (`cgo` dynamic)

The static generator (`protoc-gen-pbwire-go`) is removed: Go keeps only the
`cgo` dynamic binding (`bindings/go/dynamic`), which loads a
`FileDescriptorSet` at run time and hot-swaps schemas without a restart.
No standard four-tier bench exists for it yet (`go test ./...` covers byte
parity plus hot-swap); rebench pending before any number lands in the tables
above.

### Python vs `google.protobuf` (upb)

| payload | op | pbwire-py (MB/s, x upb) | upb (MB/s) |
|---|---|---|---|
| 136 B | encode(data) | 298.9 (5.7x ✅) | 52.3 |
| 136 B | decode(bytes) | 689.3 (1.20x ✅) | 575.5 |
| 136 B | to_dict(bytes) | 162.0 (4.9x ✅) | 33.3 |
| 136 B | encode(parsed) | 3099.8 (4.1x ✅) | 747.6 |
| 174 B | encode(data) | 342.2 (5.6x ✅) | 60.6 |
| 174 B | decode(bytes) | 724.9 (1.25x ✅) | 580.8 |
| 174 B | to_dict(bytes) | 156.7 (5.7x ✅) | 27.6 |
| 174 B | encode(parsed) | 4005.2 (4.7x ✅) | 860.8 |
| 2469 B | encode(data) | 495.3 (6.5x ✅) | 76.5 |
| 2469 B | decode(bytes) | 1167.1 (1.60x ✅) | 730.5 |
| 2469 B | to_dict(bytes) | 195.9 (4.8x ✅) | 40.4 |
| 2469 B | encode(parsed) | 30710.1 (22x ✅) | 1400.5 |
| 74913 B | encode(data) | 564.4 (6.5x ✅) | 86.6 |
| 74913 B | decode(bytes) | 1366.0 (1.69x ✅) | 810.0 |
| 74913 B | to_dict(bytes) | 170.6 (3.8x ✅) | 44.6 |
| 74913 B | encode(parsed) | 97683.0 (63x ✅) | 1540.4 |

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

All four load a `FileDescriptorSet` at run time. Node and Objective-C call
the C ABI from an FFI runtime (Node through an
N-API addon, Objective-C through an ARC wrapper), so those tables measure the
cost of that boundary on top of the same C core. Java is a pure-Java codec
with no native code (see below); Kotlin reuses its runtime. Ruby is a C extension that links
the core statically, so it pays no boundary cost at all. Java, Node and Ruby run
the repeated-field payload; Objective-C pads `str` to the same wire sizes (see
Standard cases), which makes its xlarge tier a memcpy.

#### Java vs `protobuf-java`

| payload | op | pbwire-java (MB/s, x protobuf-java) | protobuf-java (MB/s) |
|---|---|---|---|
| 136 B | encode | 1717.6 (0.75x ❌) | 2275.6 |
| 136 B | decode | 929.6 (1.46x ✅) | 637.1 |
| 174 B | encode | 1523.3 (1.19x ✅) | 1284.8 |
| 174 B | decode | 833.1 (1.46x ✅) | 572.1 |
| 2469 B | encode | 1002.2 (1.17x ✅) | 856.4 |
| 2469 B | decode | 866.6 (1.98x ✅) | 437.6 |
| 74913 B | encode | 1082.8 (1.21x ✅) | 896.7 |
| 74913 B | decode | 953.1 (2.40x ✅) | 397.8 |

The binding is a pure-Java codec (`bindings/java/runtime`): the descriptor
set is parsed by its own `Proto` reader, values live in a columnar `DMsg`
tree, and one workspace per thread (a reusable output buffer plus a
string-image cache) makes an encode a single pass plus one `byte[]` copy.
Numbers stay eager while message, group, string and bytes fields decode
lazily into slices, so untouched sub-trees cost nothing until first access
and re-encode copies them verbatim. Tags and values go through one fused
write each with a single capacity check, backed by `sun.misc.Unsafe` stores
with no per-byte bounds checks -- the same trick the official encoder uses --
with a checked fallback when `Unsafe` is missing or the platform is
big-endian.
Only the 136 B encode still loses: generated `protobuf-java` writes that message
as one straight-line block of known tags and values (~60 ns), while the dynamic
codec pays a presence gate, a type dispatch and tag loads per field (~80 ns
including the `byte[]` copy). That floor is structural, not tuning: the
per-field work left -- loads, one `switch`, one call -- is what a generated
writer avoids by knowing every field at compile time. From 174 B up the lazy
decode and the fused writes win every cell, and by 2.5 KB the size pass
`protobuf-java` runs before writing (3000 nested messages at the xlarge tier)
costs more than the codec's dispatch does.

#### Node vs `google-protobuf` (jspb)

| payload | op | pbwire-node (MB/s, x jspb) | jspb (MB/s) | protobufjs (MB/s) |
|---|---|---|---|---|
| 136 B | encode | 608.5 (10.9x ✅) | 56.0 | 243.3 |
| 136 B | decode | 375.3 (3.1x ✅) | 119.5 | 395.7 |
| 174 B | encode | 568.6 (9.0x ✅) | 62.9 | 300.8 |
| 174 B | decode | 431.9 (3.8x ✅) | 112.7 | 391.1 |
| 2469 B | encode | 1201.6 (20.8x ✅) | 57.9 | 445.6 |
| 2469 B | decode | 1122.9 (14.0x ✅) | 80.3 | 375.4 |
| 74913 B | encode | 1350.2 (22.3x ✅) | 60.6 | 496.5 |
| 74913 B | decode | 1655.0 (20.0x ✅) | 82.9 | 402.0 |

jspb's throughput does not move with payload size: its writer resolves a
code/type table per field and hands every repeated element to a closure, where
the C core writes one tag byte and one inline value, or memcpys an already
validated slice. protobufjs, the fastest widely used JS runtime, is 4-8x
faster than jspb on the same bytes (and the harness gates its bytes too):
against it pbwire-node is 2.7x on xlarge encode and 4.1x on decode. The ~22x
is the legacy-baseline gap; the ~3-4x is the codec gap.

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
| 136 B | encode | 966.3 (1.42x ✅) | 680.4 |
| 136 B | decode | 377.2 (1.58x ✅) | 238.1 |
| 174 B | encode | 886.4 (1.11x ✅) | 795.9 |
| 174 B | decode | 469.6 (1.91x ✅) | 246.4 |
| 2469 B | encode | 1221.4 (0.97x ❌) | 1262.6 |
| 2469 B | decode | 972.4 (1.93x ✅) | 503.2 |
| 74913 B | encode | 1427.1 (1.05x ✅) | 1360.9 |
| 74913 B | decode | 1186.7 (1.69x ✅) | 700.8 |

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
  fields and non-canonical encodings. That is where the 1.6-2.1x decode numbers
  come from at 174 B and up, where sub-message tree building used to dominate.
- Field facts (number, kind, wire type, packed/required/oneof, sub type, a
  name -> index Hash) are resolved once per message type into a per-schema cache,
  so the scan and the accessors never walk the descriptor.

The 2469 B encode is the closest row (0.97x): the gem's encoder is also a single
pass, and at that size the tree walk and the generated straight-line writes are
even. Above it the C core's pack-aware writer pulls ahead again (1.05x).

#### Objective-C vs the `protobuf` ObjC runtime

| payload | op | pbwire-objc (MB/s, x protobuf-objc) | protobuf-objc (MB/s) |
|---|---|---|---|
| 136 B | encode | 1088.9 (6.4x ✅) | 169.6 |
| 136 B | decode | 670.8 (2.5x ✅) | 266.3 |
| 174 B | encode | 1391.3 (6.6x ✅) | 211.6 |
| 174 B | decode | 876.2 (2.8x ✅) | 307.5 |
| 2469 B | encode | 13639.6 (4.7x ✅) | 2889.9 |
| 2469 B | decode | 11046.0 (2.9x ✅) | 3806.2 |
| 74913 B | encode | 71739.6 (1.8x ✅) | 38862.9 |
| 74913 B | decode | 76007.7 (2.7x ✅) | 27919.2 |

The ARC wrapper adds one Objective-C object per call and no copy: `encode` hands
the encoder's own malloc'd buffer to `NSData` with
`dataWithBytesNoCopy:length:freeWhenDone:`, and `decode` wraps the decoder's
arena in one `PBWMessage`. That stays ahead of the generated runtime on all four
tiers and both ops (6.1x / 2.7x at 136 B down to 2.0x / 2.8x at 74913 B). The
xlarge tier is one 74 KB string, so it is a memcpy race: the generated encoder
writes it into an `NSMutableData` of the final size, and the wrapper writes it
into its own buffer, which `NSData` then takes over.

#### Kotlin vs `protoc --kotlin_out` over `protobuf-java`

| payload | op | pbwire-kotlin (MB/s, x protobuf-kotlin) | protobuf-kotlin (MB/s) |
|---|---|---|---|
| 136 B | encode | 1537.6 (0.67x ❌) | 2287.6 |
| 136 B | decode | 804.3 (1.17x ✅) | 689.7 |
| 174 B | encode | 1557.2 (1.31x ✅) | 1188.7 |
| 174 B | decode | 716.7 (1.20x ✅) | 596.9 |
| 2469 B | encode | 1347.8 (1.60x ✅) | 840.5 |
| 2469 B | decode | 496.0 (1.10x ✅) | 448.9 |
| 74913 B | encode | 1441.1 (1.60x ✅) | 899.9 |
| 74913 B | decode | 549.1 (1.19x ✅) | 462.3 |

Kotlin reuses the Java runtime (`bindings/java/runtime`), so these numbers are
`pbwire-java`'s: one workspace lookup plus one `byte[]` copy per call. The 136 B encode
is the case the Java section explains (a generated writer knows its tags, the
dynamic tree walk derives them), here 0.67x rather than 0.69x because the
Kotlin baseline reaches the same protobuf-java encoder through its builder/DSL
path. From 174 B up both ops are ahead at every tier.

#### PHP vs the official PHP runtime (pure + C extension)

| payload | op | pbwire-php-c (MB/s, x protobuf-php-c) | protobuf-php-c (MB/s) |
|---|---|---|---|
| 136 B | encode | 1370.4 (1.63x ✅) | 842.4 |
| 136 B | decode | 683.6 (1.65x ✅) | 414.6 |
| 174 B | encode | 1299.6 (1.41x ✅) | 921.4 |
| 174 B | decode | 637.9 (1.56x ✅) | 407.8 |
| 2469 B | encode | 1618.7 (1.24x ✅) | 1300.5 |
| 2469 B | decode | 607.8 (1.21x ✅) | 501.0 |
| 74913 B | encode | 1478.3 (1.11x ✅) | 1326.1 |
| 74913 B | decode | 661.1 (1.15x ✅) | 572.7 |

The native extension links the C core in, like the Ruby C extension: built
messages live in a per-message arena (one bump allocator instead of a malloc
per value, sequential layout for the encoder), decoded trees keep the core's
single-malloc arena, and both encode straight into the result string. Against
the official C extension — the same official API backed by C, which `bench.sh`
times whenever it is installed — that wins every tier, 1.1-1.6x on both ops.
The `ext-ffi` wrapper runs the same core through one FFI trampoline per call:
it pays that boundary on small payloads and converges to the same speed once
the payload dominates (xlarge 1.09x / 1.23x vs the C extension in the same
harness). The pure-PHP runtime, timed by a second `php -n` run
in the same harness, is a different race:

| payload | op | pbwire-php (MB/s, x protobuf-php) | protobuf-php (MB/s) |
|---|---|---|---|
| 136 B | encode | 252.7 (66.5x ✅) | 3.8 |
| 136 B | decode | 211.9 (33.6x ✅) | 6.3 |
| 174 B | encode | 299.0 (80.8x ✅) | 3.7 |
| 174 B | decode | 244.9 (41.5x ✅) | 5.9 |
| 2469 B | encode | 957.8 (281.7x ✅) | 3.4 |
| 2469 B | decode | 540.0 (108.0x ✅) | 5.0 |
| 74913 B | encode | 1384.2 (374.1x ✅) | 3.7 |
| 74913 B | decode | 658.7 (126.7x ✅) | 5.2 |

The pure-PHP runtime spends its time on per-field bookkeeping (~4-6 MB/s flat
across tiers), so quoting those ratios without the C-extension table is
quoting the interpreter, not the codec.

#### Dart vs the `protobuf` Dart package

| payload | op | pbwire-dart (MB/s, x protobuf-dart) | protobuf-dart (MB/s) |
|---|---|---|---|
| 136 B | encode | 775.8 (3.7x ✅) | 207.8 |
| 136 B | decode | 383.8 (1.8x ✅) | 218.3 |
| 174 B | encode | 709.9 (3.8x ✅) | 185.6 |
| 174 B | decode | 395.5 (2.0x ✅) | 196.2 |
| 2469 B | encode | 1113.7 (8.7x ✅) | 127.4 |
| 2469 B | decode | 492.3 (3.2x ✅) | 151.6 |
| 74913 B | encode | 1273.8 (12.2x ✅) | 104.0 |
| 74913 B | decode | 555.9 (3.3x ✅) | 166.3 |

Both sides allocate per call. `dart:ffi` adds a `malloc`/copy for the input
bytes and a `Uint8List` copy for the output, which is why the gain grows with
payload size: the Dart runtime's pure-Dart field loop dominates it by 2.5 KB,
and at 136 B the boundary is still visible in a 1.6x decode.

#### Rust vs `rust-protobuf` 3.x

| payload | op | pbwire-rust (MB/s, x protobuf-rust) | protobuf-rust (MB/s) | prost (MB/s) |
|---|---|---|---|---|
| 136 B | encode | 1404.6 (1.20x ✅) | 1170.6 | 1322.0 |
| 136 B | decode | 932.2 (1.26x ✅) | 739.7 | 872.9 |
| 174 B | encode | 1377.2 (1.16x ✅) | 1189.6 | 1282.9 |
| 174 B | decode | 856.7 (1.42x ✅) | 604.8 | 714.2 |
| 2469 B | encode | 1213.1 (1.49x ✅) | 811.7 | 1348.1 |
| 2469 B | decode | 525.4 (1.44x ✅) | 364.5 | 361.6 |
| 74913 B | encode | 1423.0 (1.57x ✅) | 906.5 | 1535.0 |
| 74913 B | decode | 575.8 (1.45x ✅) | 396.8 | 393.4 |

The archive is linked in, so a call is a direct C call with no `dlopen` lookup.
prost, the fastest widely used Rust runtime (generated at build time by
build.rs, bytes gated like the rest), takes the large/xlarge encodes
(xlarge 1535.0 vs 1423.0, 0.93x) while pbwire-rust leads the other six cells
(xlarge decode 1.45x); both beat rust-protobuf's generated codecs on every
cell.

#### C# vs Google.Protobuf

| payload | op | pbwire-csharp (MB/s, x protobuf-csharp) | protobuf-csharp (MB/s) | protobuf-net (MB/s) |
|---|---|---|---|---|
| 136 B | encode | 1365.4 (3.0x ✅) | 450.7 | 583.9 |
| 136 B | decode | 922.1 (1.9x ✅) | 487.5 | 467.5 |
| 174 B | encode | 1410.7 (3.4x ✅) | 417.7 | — |
| 174 B | decode | 825.1 (2.0x ✅) | 411.6 | — |
| 2469 B | encode | 1206.7 (2.2x ✅) | 546.8 | 384.0 |
| 2469 B | decode | 519.8 (1.18x ✅) | 441.8 | 303.7 |
| 74913 B | encode | 1393.5 (2.9x ✅) | 480.6 | 392.5 |
| 74913 B | decode | 547.1 (1.07x ✅) | 513.0 | 337.2 |

Google.Protobuf's C# runtime is already a fast tree codec, so this is the
closest race of the five. Every cell is at or above parity -- the closest is
the xlarge decode at 1.07x, inside run-to-run noise -- and the encode side
stays ~2-3x ahead at every tier. protobuf-net, the fastest widely used C#
runtime (hand-written contracts in Pbn.cs, bytes gated like the rest), is
slower than Google.Protobuf on every tier here (xlarge 392.5 vs 480.6
encode): the official runtime was already the fastest. Its medium rows are
absent on purpose: protobuf-net writes single-element packed fields in
singular form — valid protobuf, semantically equal (the harness checks that
via the Google parser), but not byte-identical, so there is nothing MB/s-fair
to time there.

All five land at ~1.4 GB/s encode and ~535 MB/s decode on the xlarge tier, which
is the C core's own number: by 2.5 KB the per-call boundary is invisible and only
the baseline's own codec is being measured. That is the point of the schema-at-
runtime design -- the binding owns ergonomics, the core owns the format.

### Erlang: `pbwire` vs gpb vs gpb NIF

`matrix3.proto`, all four tiers, best of three samples per op. Same payload / op /
MB/s layout as every other table: the `pbwire-erlang` column carries the ratio
against gpb, the third column is the gpb NIF (libprotobuf) second baseline.

| payload | op | pbwire-erlang (MB/s, x gpb) | gpb (MB/s) | gpb-nif (MB/s) |
|---|---|---|---|---|
| 136 B | encode | 341.0 (2.00x ✅) | 170.3 | 68.0 |
| 136 B | decode | 486.4 (2.18x ✅) | 223.4 | 69.9 |
| 174 B | encode | 312.0 (2.22x ✅) | 140.3 | 83.3 |
| 174 B | decode | 481.3 (2.33x ✅) | 206.6 | 92.6 |
| 2469 B | encode | 234.4 (1.99x ✅) | 117.8 | 93.4 |
| 2469 B | decode | 327.9 (1.69x ✅) | 194.2 | 172.1 |
| 74913 B | encode | 249.4 (2.03x ✅) | 122.6 | 118.0 |
| 74913 B | decode | 346.5 (1.52x ✅) | 228.5 | 198.8 |

## Run them

```bash
make bench        # core: 4 cases + text/json/stream/threads (needs protoc + libprotobuf)
make bench-c      # C structs vs protoc C++ on matrix3.proto (same deps)
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
./build/bench bench 2000          # fixed-count smoke run (default calibrates from 200000; xlarge floor scales /100)
./build/bench_struct_c bench 2000 # fixed-count smoke run (same scaling)
```

Every harness follows the same unified measurement standard, so a number from
one language means the same thing as a number from another:

1. **Floor iterations per tier**: 50,000 / 50,000 / 5,000 / 500
   (small / medium / large / xlarge). PHP keeps lower floors
   (20,000 / 20,000 / 2,000 / 200) because its pure-PHP ops are
   millisecond-scale — 200 iterations there already take seconds.
2. **Calibrate per op**: double from the floor until one pass of that op
   takes at least **0.25 s**. A slow op already exceeds the minimum at the
   floor and keeps it; a fast op doubles up. The tier line prints the
   reference op's (the binding's own encode) calibrated count; the other
   ops' counts differ silently — exactly like Go's `testing.Benchmark`,
   which has always reported its own per-op count. Erlang calibrates one
   count per case on its slowest op instead, which is equivalent when the
   backends sit within a factor of a few, as they do.
3. **Warm up** one full pass at the calibrated count, then report the
   **best of 3 timed passes** (interleaved per op where the harness
   supports it, so a load spike hits both implementations equally).

Two harnesses meet the same bar with their own calibrator instead of the
shared doubling loop: Go's `testing.Benchmark` calibrates internally to ~1 s
per op, and the core/C++ harnesses keep an explicit count only when one is
passed on the command line (`./build/bench bench 2000` fixes the count for a
smoke run but still warms up and takes the best of 3).

From `bindings/erlang/benchmarks` (needs Erlang/OTP + gpb; the gpb NIF baseline
additionally needs protoc + libprotobuf C++, and is skipped without them).
Each op is the best of three samples, each `BENCH_TARGET_TIME` seconds long
(default 0.25):

```bash
make benchmarks        # epb, gpb and (when built) gpb NIF on matrix3.proto
make benchmarks-quick  # same suite with BENCH_TARGET_TIME=0.05
```

## Regenerate this file

The core tables (single-thread and threads) are `scripts/perf.sh` output (same
layout, MB/s only); the C,
Python, Java, Kotlin, Node, Ruby, Objective-C, PHP, Dart, Rust and C# tables are
`make bench-c` / `make python-bench` / `make java-bench` /
`make kotlin-bench` / `make node-bench` / `make ruby-bench` / `make objc-bench` /
`make php-bench` / `make dart-bench` / `make rust-bench` / `make csharp-bench`
output reformatted the same way; the
Erlang table is copied from
`bindings/erlang/benchmarks/README.md` (`make update-readme` there). The
second baselines come along for free: Rust's build.rs generates prost from
crates.io, C# restores
protobuf-net from NuGet, Node npm-installs protobufjs, and `make php-bench`
runs the C-extension rows plus a `php -n` pure-PHP pass when the extension is
present. Rebuild the
at-a-glance table from those rows afterwards — it only copies the xlarge line of
each. Then sync the summary table in [README.md](README.md#performance): for
each binding take the best encode ratio and the best decode ratio across its
four tiers, with the tier in parentheses (`small`/`medium`/`large`/`xlarge`),
and keep the machine note (CPU, `protoc` version) next to it. Paste all runs from the same machine and note the date, CPU, `protoc`
version and iteration counts above.

`make bench` and `make difftest` are correctness-checked before they report a
number, so a stale or hand-edited row is worse than a missing one: paste the
harness output, do not transcribe by hand.

## Bench records (structured JSON)

Every `make *-bench` target also writes one structured record per run into
`results/` (git-ignored): `results/bench-<binding>-<utc-stamp>.json` plus the
full log next to it (`.log`). Stdout is unchanged. The record holds the
parsed case blocks (`case`, wire `bytes`, calibrated `iterations` (null
when the harness prints none, e.g. Python), repeat count, and every
`impl`/`op` row as printed `ns/op` + `MB/s`), the
reproduction facts (`timestamp`, git sha + dirty flag, OS/arch/CPU/cores/RAM,
`protoc` and compiler versions, bench exit status and wall time), and the log
file name. The schema is versioned (`version: 1`, fields only ever added);
`scripts/bench-record.py` parses, `scripts/bench-run.sh` wraps a bench
command, `scripts/bench-render.py` turns records into markdown tables:

```bash
python3 scripts/bench-render.py results/bench-python-*.json
jq '{binding, timestamp, host} + {cases: [.cases[] | {case, bytes}]}' results/bench-core-*.json
```

Use the JSON (not this file) to compare runs, hunt regressions, or debug a
slow machine: the tables above are pasted from same-machine runs, the JSON
is what proves two runs are comparable.

# pbwire performance benchmarks

This is the performance document for the whole project: the pbwire core, the C
struct codec and every host binding, all measured on the same four payload
tiers and each against its official counterpart, so the numbers can be read
across languages. The single-glance comparison is the
[at-a-glance table](#at-a-glance-every-binding-one-tier); the rest of the file
is the detail behind it.

- [Standard cases](#standard-cases) — what the four tiers are
- [Output format](#output-format) — what one harness block means
- [Test environment](#test-environment) — the machine and toolchain behind every number below
- [At a glance](#at-a-glance-every-binding-one-tier) — every binding, one tier
- [Bindings vs their baselines](#bindings-vs-their-baselines) — who is ahead and by how much
- [Core](#core-pbw_ctx-vs-plain-vs-generated) — ctx vs plain vs generated vs `DynamicMessage`, threads, streaming
- [Measurement notes](#measurement-notes-the-core-fast-path) — A/B numbers behind the core's fast paths
- [Generated models](#generated-models-codegen) — what the generated layer costs
- [Per-binding tables](#cross-binding-comparison-last-run) — one section per language (✅ exceeds baseline ≥1.0x, ❌ below):
  - [Core](#core-pbw_ctx-vs-plain-vs-generated) · [C structs](#c-structs-vs-protoc-generated-same-matrix3proto) · [Go](#go-cgo-dynamic) · [Python](#python-vs-googleprotobuf-upb)
  - [Java](#java-vs-protobuf-java) · [Node](#node-vs-google-protobuf-jspb-and-protobufjs) · [Ruby](#ruby-c-extension-vs-the-google-protobuf-gem) · [Objective-C](#objective-c-vs-the-protobuf-objc-runtime)
  - [Kotlin](#kotlin-vs-protoc---kotlin_out-over-protobuf-java) · [PHP](#php-vs-the-official-php-runtime-pure--c-extension) · [Dart](#dart-vs-the-protobuf-dart-package) · [Rust](#rust-vs-prost-and-rust-protobuf-3x) · [C#](#c-vs-googleprotobuf) · [Lua](#lua-c-module-tables-vs-lua-protobuf) · [Erlang](#erlang-pbwire-vs-gpb-vs-gpb-nif)
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

## Test environment

Every table below was pasted from runs on the same machine, so MB/s can be
read across rows (compare `x baseline` between bindings, not absolute MB/s
across languages — the payload, the boundary cost and the baseline all
differ). The per-run proof lives next to each harness run, not here: every
`make *-bench` target writes a versioned JSON record into `results/`
(git-ignored) holding the timestamp, git sha, OS/arch/CPU/cores/RAM,
`protoc` and compiler versions, and the full log (see
[Regenerate this file](#regenerate-this-file)). Re-paste the tables after
re-running, and refresh this section when the machine or toolchain moves.

Last full refresh: 2026-11-05, all rows below:

| piece | version |
|---|---|
| machine | Apple M4 Pro, 14 cores, 48 GB RAM |
| OS | macOS (Darwin 27.0.0, arm64) |
| C/C++ | Apple clang 21.0.0, `-O2` |
| `protoc` / libprotobuf C++ | 36.2 |
| Python | 3.15.2, `protobuf` 7.36.2 (upb) |
| Erlang/OTP | 29.0.1 |
| Node | v26.3.0 (`google-protobuf` 4.0.3, protobufjs 8.8.1) |
| Go | 1.27.3 |
| Java | OpenJDK 26.0.1 |
| Ruby | 4.0.9 (`google-protobuf` gem 4.36.2) |
| Objective-C | same clang, Xcode Foundation |
| PHP | 8.5.12 (`protobuf` C extension 5.36.2) |
| Dart | 3.13.7 |
| Rust | 1.99.1 |
| C# | .NET 10.0.403 |
| Lua | 5.5.1, `lua-protobuf` 0.6.0 |
| Swift | 6.4.2, `swift-protobuf` 1.39.0 |
| Scala | 3.10.0, ScalaPB 0.11.18, `protobuf-java` 4.36.2 |
| Elixir | 1.20.5 / OTP 29, `protobuf` 0.17.1, `pbwire` 0.1.7 (Hex) |
| Zig | 0.17.1 (`-Doptimize=ReleaseFast`), zig-protobuf 7.0.0 |
| F# | .NET 10.0.403 (Google.Protobuf baseline via C#) |
| Clojure | 1.12.0, `protobuf-java` 4.36.2 |
| Julia | 1.13.3, ProtoBuf.jl 1.3.0 |
| R | 4.6.2, RProtoBuf 0.4.28 |

## Bindings vs their baselines

| binding | harness | baseline | cases | vs baseline (xlarge) |
|---|---|---|---|---|
| core (dynamic) | `make bench` (`bench/bench.cc`) | `protoc` C++ generated + DynamicMessage | all 4 | 1.40x / 1.57x ✅ |
| C structs | `make bench-c` (`bindings/c/benchmarks/bench_struct.cc`) | `protoc` C++ generated from the same `matrix3.proto` | all 4 | 1.32x / 2.39x ✅ |
| Erlang/OTP NIF | `make benchmarks` in `bindings/erlang/benchmarks` | gpb (pure Erlang), with gpb NIF (libprotobuf) as the second baseline, same `matrix3.proto` | all 4 | 3.91x / 1.66x vs best-of-two ✅ |
| Python (CPython ext) | `make python-bench` (`bindings/python/benchmarks/bench.py`) | `google.protobuf` (upb C extension) on the same `matrix3.proto` | all 4 | 7.0x / 1.72x ✅ |
| C++ | same engine as C (`bindings/cpp` mirrors the C structs; `make bench-c` is the harness) | `protoc` C++ generated from the same `matrix3.proto` | all 4 | 1.32x / 2.39x ✅ |
| Go (`cgo` dynamic) | `make go-bench` (`bindings/go/benchmarks`) | `protobuf-go` generated code from the same `matrix3.proto` | all 4 | 2.58x / 2.07x ✅ |
| Java (pure-Java codec) | `make java-bench` (`bindings/java/benchmarks`) | `protobuf-java` generated code from the same `matrix3.proto` | all 4 | 1.06x / 2.47x ✅ (0.76x at 136 B) |
| Node (N-API addon) | `make node-bench` (`bindings/node/benchmarks`) | [protobufjs](https://www.npmjs.com/package/protobufjs) (fastest) + `google-protobuf` (jspb) from the same `matrix3.proto` | all 4 | 3.07x / 4.20x vs protobufjs ✅ (0.98x small decode) |
| Ruby (C extension) | `make ruby-bench` (`bindings/ruby/benchmarks`) | the `google-protobuf` gem (C extension) | all 4 | 1.25x / 1.76x ✅ |
| Objective-C (ARC) | `make objc-bench` (`bindings/objc/benchmarks`) | the `protobuf` ObjC runtime + generated code from the same `matrix3.proto` | all 4 | 1.87x / 2.89x ✅ |
| Kotlin (same FFM layer as Java) | `make kotlin-bench` (`bindings/kotlin/benchmarks`) | `protoc --kotlin_out` over `protobuf-java`, same `matrix3.proto` | all 4 | 1.05x / 2.08x ✅ (0.80x at 136 B) |
| PHP (`ext-ffi` + C extension) | `make php-bench` (`bindings/php/benchmarks`) | the official PHP runtime (`protobuf/php/src`): C extension (fastest) + pure-PHP | all 4 | 1.13x / 1.28x vs C ext ✅ (356x / 132x vs pure) |
| Dart (`dart:ffi`) | `make dart-bench` (`bindings/dart/benchmarks`) | the `protobuf` Dart package + generated code | all 4 | 16.4x / 4.19x ✅ |
| Rust (static link) | `make rust-bench` (`bindings/rust/benchmarks`) | prost (fastest) + rust-protobuf 3.x generated code | all 4 | 1.11x / 1.83x vs prost ✅ |
| C# (P/Invoke) | `make csharp-bench` (`bindings/csharp/benchmarks`) | Google.Protobuf C# generated code (fastest) + protobuf-net | all 4 | 2.88x / 1.40x vs official ✅ |
| Lua (C module, tables) | `make lua-bench` (`bindings/lua/benchmarks`) | `lua-protobuf` 0.6.0 | all 4 | 1.04x / 1.16x ✅ (8.47x via builder encode) |
| Swift (SwiftPM, static link) | `make swift-bench` (`bindings/swift/benchmarks`) | `apple/swift-protobuf` 1.39.0 generated code from the same `matrix3.proto` | all 4 | 5.57x / 2.64x ✅ |
| Scala (Java runtime + DSL) | `make scala-bench` (`bindings/scala/benchmarks`) | ScalaPB 0.11.18 generated code from the same `matrix3.proto` | all 4 | 1.17x / 1.62x ✅ |
| Elixir (NIF over the shared core) | `make elixir-bench` (`bindings/elixir/benchmarks`) | `elixir-protobuf` 0.17.1 structs from the same `matrix3.proto` | all 4 | 2.00x / 1.25x ✅ |
| Zig (static link) | `make zig-bench` (`bindings/zig/benchmarks`, `-Doptimize=ReleaseFast`) | Arwalk/zig-protobuf 7.0.0 generated code from the same `matrix3.proto` | all 4 | 1.83x / 2.28x ✅ |
| F# (facade over the C# runtime) | `make fsharp-bench` (`bindings/fsharp/benchmarks`) | Google.Protobuf C# generated code from the same `matrix3.proto` | all 4 | 3.00x / 1.42x ✅ |
| Clojure (Java runtime + interop) | `make clojure-bench` (`bindings/clojure/benchmarks`) | `protobuf-java` generated code from the same `matrix3.proto` | all 4 | 1.05x / 2.19x ✅ |
| Julia (`ccall` FFI) | `make julia-bench` (`bindings/julia/benchmarks`) | JuliaIO/ProtoBuf.jl 1.3.0 generated code from the same `matrix3.proto` | all 4 | 126.8x / 0.99x ✅/❌ (baseline decode ties from 2.5 KB) |
| R (C extension, named lists) | `make r-bench` (`bindings/r/benchmarks`) | RProtoBuf 0.4.28 on the same `matrix3.proto` | all 4 | 66.5x / 52.3x ✅ |

"vs baseline" is encode / decode throughput relative to the best baseline on
the largest case the binding runs. Where a binding has several baselines the
headline names the fastest: protobufjs over jspb, the PHP C extension over
pure PHP, prost over rust-protobuf, Google.Protobuf over protobuf-net, the
faster of gpb / gpb NIF per op. `encode` is host data to wire bytes and
`decode` is wire bytes to an in-memory message on both sides (R materializes
plain lists on both sides: RProtoBuf's bare `read()` is lazily parsed and
faster, but hands back no R data — see its section). Every binding with a
wired baseline is ahead of it on the xlarge encode column now; bytes are
identical everywhere. The remaining sub-1.0x cells are all small-tier
boundary floors (FFI/downcall cost on 136-174 B payloads: Java/Kotlin
small-tier encode 0.76x/0.80x, Julia small decode 0.91x, Node small decode
0.98x vs protobufjs, Go small-tier encode at parity, the PHP FFI wrapper) or
the plain-C-API malloc-per-call on the 169 B tier (0.94x, while the context
API it is measured against wins) — boundary cost on a tiny payload, not a
byte gap, and every encode headline still wins.

Correctness first everywhere: the core bench verifies byte-for-byte equality
before timing, the C and Python benches check their bytes against the
`protoc`-produced reference (plus a decode→encode round trip) before timing,
the Java, Kotlin, Ruby, Rust, C#, PHP and Zig harnesses
require their bytes to equal the official encoder's bytes before timing
anything, the Swift, Scala, Elixir, F#, Clojure and Julia harnesses do
the same against swift-protobuf / ScalaPB / elixir-protobuf / Google.Protobuf /
protobuf-java / ProtoBuf.jl respectively
(the R harness gates on the tier sizes, baseline byte equality, plus a full decode round trip, since
R cannot spell INT32_MIN distinctly from NA), and the Erlang harness requires gpb's pinned wire size and byte
equality (encode, plus re-encoding a decoded reference) from every backend
before timing.

## At a glance: every binding, one tier

One row per implementation, on the largest tier that binding runs, from the
per-binding tables further down (same machine, same run). `x baseline` is the
column to compare: absolute MB/s is not comparable across languages because the
payload, the boundary cost and the baseline all differ.

| binding | reaches the core | xlarge wire size | encode MB/s (x baseline) | decode MB/s (x baseline) |
|---|---|---|---|---|
| core dynamic (`pbw_ctx`) | direct call | 85933 B | 1719.5 (1.40x ✅) | 718.4 (1.57x ✅) |
| core dynamic (plain API) | direct call | 85933 B | 1556.4 (1.27x ✅) | 720.4 (1.57x ✅) |
| C structs | direct call, layout tables | 74913 B | 1168.6 (1.32x ✅) | 847.8 (2.39x ✅) |
| Go | `cgo` dynamic over the C core | 74913 B | 1543.6 (2.58x ✅) | 698.0 (2.07x ✅) |
| Python (dict path) | CPython extension | 74913 B | 590.9 (7.03x ✅) | 1369.8 (1.72x ✅) |
| Java | pure-Java codec, no native code | 74913 B | 969.5 (1.06x ✅) | 986.9 (2.47x ✅) |
| Kotlin | the Java FFM layer + DSL | 74913 B | 954.0 (1.05x ✅) | 952.7 (2.08x ✅) |
| Node | N-API addon | 74913 B | 1529.8 (3.07x ✅) | 1645.9 (4.20x ✅) |
| Ruby | C extension over the core | 74913 B | 1641.5 (1.25x ✅) | 1207.6 (1.76x ✅) |
| Objective-C | ARC wrapper | 74913 B (one 74 KB string) | 74470.3 (1.87x ✅) | 81497.8 (2.89x ✅) |
| PHP | C extension | 74913 B | 1422.8 (1.13x ✅) | 688.4 (1.28x ✅) |
| Dart | `dart:ffi` | 74913 B | 1454.3 (16.4x ✅) | 678.3 (4.19x ✅) |
| Rust | static link | 74913 B | 1579.2 (1.11x ✅) | 715.4 (1.83x ✅) |
| C# | P/Invoke | 74913 B | 1425.9 (2.88x ✅) | 719.7 (1.40x ✅) |
| Lua | C module, tables | 74913 B | 196.4 (1.04x ✅) | 105.5 (1.16x ✅) |
| Erlang | NIF (`pbwire`) | 74913 B | 477.8 (3.91x ✅) | 364.6 (1.66x ✅) |
| Swift | static link (SwiftPM) | 74913 B | 1564.2 (5.57x ✅) | 589.7 (2.64x ✅) |
| Scala | the Java runtime + DSL | 74913 B | 980.9 (1.17x ✅) | 956.7 (1.62x ✅) |
| Elixir | NIF (Hex `:pbwire`, same core as Erlang) | 74913 B | 115.3 (2.00x ✅) | 67.4 (1.25x ✅) |
| Zig | static link | 74913 B | 1537.6 (1.83x ✅) | 716.9 (2.28x ✅) |
| F# | P/Invoke over the C# runtime | 74913 B | 1436.1 (3.00x ✅) | 710.1 (1.42x ✅) |
| Clojure | the Java runtime + interop | 74913 B | 939.2 (1.05x ✅) | 972.7 (2.19x ✅) |
| Julia | `dlopen` + `ccall` | 74913 B | 1445.3 (126.8x ✅) | 726.3 (0.99x ❌) |
| R | C extension, named lists | 74913 B | 186.4 (66.5x ✅) | 141.1 (52.3x ✅) |

Read it as: the in-process dynamic core sets the pace (1.4x the C++
generated code at this size), a binding that links the core in (Ruby, Rust)
keeps within a few percent of it, and an FFI binding pays its boundary only
until the payload is large — Java, Kotlin, Ruby, Rust, C# and PHP all land at
~1.0-1.6 GB/s encode and ~680-1200 MB/s decode on the xlarge tier, which is
near the core's own number. The outliers are baseline or unit effects, not pbwire
effects: Python's encode column is the dict path (3000 sub-messages, 12000
ints), and the Objective-C tier is a memcpy race over one 74 KB string.
The headline always names the fastest baseline: Node's 3.07x / 4.20x is over
protobufjs (jspb, the legacy baseline, trails it by ~25x / ~19x), PHP's 1.13x /
1.28x is over the official C extension (the pure-PHP runtime trails it by
~356x / ~132x) — see their sections. Swift, Scala, F#, Zig and Julia join
the ~1.1-1.6 GB/s encode club on xlarge (Swift 5.57x and Julia 127x encode
are baseline effects: swift-protobuf and ProtoBuf.jl encode much slower than
their own decode).

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

## Generated models (codegen)

The generated models are ergonomics, not a second hot path: their
`encode`/`decode` call the same dynamic API the tables below measure. The cost
is the per-call type lookup plus, in Python, the dataclass ⇄ dict conversion.
Measured on this machine with `matrix3` x3000 (30 KB) and identical values
through both paths:

| language | encode, generated / dynamic | decode, generated / dynamic |
|---|---|---|
| C (struct binding) | 1.00x (the struct path itself) | 1.00x |
| Java | 1.00x | 1.09x |
| Python (dataclass ⇄ dict) | 3.8x slower | 3.2x slower |

So the numbers below still describe what a generated-model user pays on the wire; for
Python hot loops the dict path (which the Python row measures) is the fast path.
No binding gained a slower encode/decode path from this layer.

## Cross-binding comparison (last run)

Per-language index: [Core](#core-pbw_ctx-vs-plain-vs-generated) · [C structs](#c-structs-vs-protoc-generated-same-matrix3proto) · [Go](#go-cgo-dynamic) · [Python](#python-vs-googleprotobuf-upb) · [Java](#java-vs-protobuf-java) · [Node](#node-vs-google-protobuf-jspb-and-protobufjs) · [Ruby](#ruby-c-extension-vs-the-google-protobuf-gem) · [Objective-C](#objective-c-vs-the-protobuf-objc-runtime) · [Kotlin](#kotlin-vs-protoc---kotlin_out-over-protobuf-java) · [PHP](#php-vs-the-official-php-runtime-pure--c-extension) · [Dart](#dart-vs-the-protobuf-dart-package) · [Rust](#rust-vs-prost-and-rust-protobuf-3x) · [C#](#c-vs-googleprotobuf) · [Lua](#lua-c-module-tables-vs-lua-protobuf) · [Swift](#swift-swiftpm-static-link-vs-appleswift-protobuf) · [Scala](#scala-java-runtime--dsl-vs-scalapb) · [Elixir](#elixir-nif-over-the-shared-core-vs-elixir-protobuf) · [Zig](#zig-static-link-vs-arwalkzig-protobuf) · [F#](#f-facade-over-the-c-runtime-vs-googleprotobuf) · [Clojure](#clojure-java-runtime--interop-vs-protobuf-java) · [Julia](#julia-dlopen--ccall-vs-protobufjl) · [R](#r-c-extension-named-lists-vs-rprotobuf) · [Erlang](#erlang-pbwire-vs-gpb-vs-gpb-nif). In every table below, ✅ means that cell beats its baseline (≥1.0x), ❌ means it does not.

See [Test environment](#test-environment) for the machine and toolchain.
Every harness calibrates from its tier floor until one pass takes at
least 0.25 s, then reports the best of 3 passes (Erlang uses the same bar
with a 0.25 s sample time). Throughput in MB/s
on serialized wire size. Per-run noise is a few percent; regenerate below for
the current machine.

### Core: `pbw_ctx` vs plain vs generated

| payload | op | pbwire ctx (MB/s, x gen) | pbwire plain (MB/s, x gen) | protoc gen (MB/s) | DynamicMessage (MB/s) |
|---|---|---|---|---|---|
| 132 B | encode | 1961.4 (1.19x ✅) | 1699.8 (1.03x ✅) | 1647.3 | 113.6 |
| 132 B | decode | 1525.4 (1.63x ✅) | 1357.8 (1.45x ✅) | 934.9 | 295.9 |
| 169 B | encode | 1870.4 (1.09x ✅) | 1605.3 (0.94x ❌) | 1709.2 | 108.9 |
| 169 B | decode | 1253.7 (1.47x ✅) | 1132.2 (1.33x ✅) | 851.5 | 293.9 |
| 2755 B | encode | 1616.8 (1.37x ✅) | 1342.4 (1.14x ✅) | 1176.0 | 135.5 |
| 2755 B | decode | 691.9 (1.48x ✅) | 680.1 (1.45x ✅) | 468.6 | 265.7 |
| 85933 B | encode | 1719.5 (1.40x ✅) | 1556.4 (1.27x ✅) | 1229.7 | 154.7 |
| 85933 B | decode | 718.4 (1.57x ✅) | 720.4 (1.57x ✅) | 459.0 | 270.8 |

Threads (169 B, 1 vs 8):

| op | pbwire 1thr (MB/s, x gen) | pbwire 8thr agg (MB/s, x gen) | gen 1thr (MB/s) | gen 8thr agg (MB/s) |
|---|---|---|---|---|
| encode | 1863.7 (1.07x ✅) | 12467.5 (1.35x ✅) | 1736.7 | 9212.2 |
| decode | 1222.1 (1.49x ✅) | 7928.5 (1.24x ✅) | 822.8 | 6404.3 |

`8thr agg` is the total throughput of all eight threads. The schema, the
descriptors and the wire input are shared by all threads; each thread only adds
its own `pbw_ctx` and message. pbwire scales 6.7x (encode) and 6.5x (decode)
from 1 to 8 threads in that run; generated code reaches 5.3x and 7.8x. Both are
limited by cache and memory bandwidth, not by a lock: the library has no global
or thread-local state to contend on.

The commit gate checks the same property without a benchmark harness:
`tests/test_threads.c` prints 1-thread vs 8-thread MB/s for the context API (its
own, smaller message, one run on the same machine):

```
scale ctx decode 1 thread:   3466.1 MB/s (  56.0 ns/op) | 8 threads:   2672.9 MB/s/thread,  21383.3 MB/s aggregate (per-op 1.30x, aggregate 6.17x, 14 cores)
scale ctx encode 1 thread:   6516.6 MB/s (  29.8 ns/op) | 8 threads:   4639.5 MB/s/thread,  37115.9 MB/s aggregate (per-op 1.40x, aggregate 5.70x, 14 cores)
```

It fails when the aggregate throughput collapses below 0.35x of
`min(8, cores)`, which is what a hidden lock or false sharing would cause.

#### `pbwire ctx` vs `pbwire plain`

The two pbwire columns are the same encoder and decoder with a different memory
and ownership strategy; the benchmark checks that they produce identical bytes
before timing anything. The ownership table is in
[docs/design.md](docs/design.md#two-allocation-strategies); on the wire it shows
up as the `malloc`/`free` pair per call, which is why plain sits at parity on
the 132 B tier (1.03x), trails the 169 B encode (0.94x), and wins everything at 2755 B and up, where both
columns are within a few percent of each other.

Read it as:

- **Against generated code**: the context API wins every binary cell — encode
  by 1.09x-1.40x, decode by 1.47x-1.63x. There is no tier where a compiled
  straight-line writer beats it outright, because the C++ serializer walks the
  message twice (size, then write) while pbwire writes one pass.
- **Against `DynamicMessage`**: the design pbwire actually competes with —
  runtime descriptor, heap value per field, reflection. pbwire is 1.7-3.2x
  faster there (decode) and 7.9-15.7x on encode.
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
| 136 B | encode | 1444.2 (1.09x ✅) | 1327.7 |
| 136 B | decode | 1041.2 (1.22x ✅) | 853.0 |
| 174 B | encode | 1460.6 (1.09x ✅) | 1343.1 |
| 174 B | decode | 968.1 (1.29x ✅) | 752.9 |
| 2469 B | encode | 1027.3 (1.19x ✅) | 866.8 |
| 2469 B | decode | 761.4 (2.00x ✅) | 380.7 |
| 74913 B | encode | 1168.6 (1.32x ✅) | 883.2 |
| 74913 B | decode | 847.8 (2.39x ✅) | 354.3 |

The struct codec writes and reads wire bytes directly from the layout tables;
there is no dynamic message tree in between. Four changes got it ahead of
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
  stay valid);
- a link-time encode budget (`tail`, one suffix sum per plan level): a
  single reserve at level entry covers every singular scalar, so those write
  through a hoisted cursor with no capacity check at all (measured 136 B
  encode 0.93x -> 1.05x; per-field reserves chained every check through
  `b->len`, which is what the last 7 ns were).

The struct encoder's single pass
(where `SerializeToString` walks the message twice: size, then write) and the
decode arena (no per-value malloc, no free walk) dominate at every tier; the
link-time budget above is what closed the last small-tier gap, where
generated straight-line code has no per-field dispatch at all.

The harness also prints a `pbwire-c1` block: the one-shot API, which compiles
the layout per call and mallocs per value (2.8x-5x slower). It stays in the
output so that trade-off is visible rather than hidden.

### Go (`cgo` dynamic)

The static generator (`protoc-gen-pbwire-go`) is removed: Go keeps only the
`cgo` dynamic binding (`bindings/go/dynamic`), which loads a
`FileDescriptorSet` at run time and hot-swaps schemas without a restart.
`make go-bench` (`bindings/go/benchmarks`, build-tagged so `go test ./...`
stays fast) times it against `protobuf-go` generated code from the same
`matrix3.proto` on all four tiers; `bench.sh` generates the baseline with
`protoc-gen-go` and the `FileDescriptorSet` both sides load.

| payload | op | pbwire-go (MB/s, x protobuf-go) | protobuf-go (MB/s) |
|---|---|---|---|
| 136 B | encode | 662.4 (1.02x ✅) | 650.1 |
| 136 B | decode | 723.5 (1.26x ✅) | 576.4 |
| 174 B | encode | 669.3 (1.04x ✅) | 646.1 |
| 174 B | decode | 715.1 (1.41x ✅) | 506.1 |
| 2469 B | encode | 1196.4 (2.23x ✅) | 535.6 |
| 2469 B | decode | 638.1 (1.99x ✅) | 321.5 |
| 74913 B | encode | 1543.6 (2.58x ✅) | 597.7 |
| 74913 B | decode | 698.0 (2.07x ✅) | 336.4 |

Every tier is gated before timing: the official bytes must land on the
pinned size, `pbwire-go` must encode byte-identical bytes, decoding the
reference and re-encoding must return the same bytes, and the official
decode is spot-checked (`str`, inner `kid`/42, repeated count and last
`inners` value). The `cgo` boundary is one downcall per op with zero-copy
decode in, so the small tiers sit at or just above parity (1.02x / 1.26x) and the C
core's single pass pulls ahead from 2.5 KB up.

### Python vs `google.protobuf` (upb)

| payload | op | pbwire-py (MB/s, x upb) | upb (MB/s) |
|---|---|---|---|
| 136 B | encode(data) | 264.6 (5.26x ✅) | 50.3 |
| 136 B | decode(bytes) | 653.9 (1.14x ✅) | 575.8 |
| 136 B | to_dict(bytes) | 157.7 (4.78x ✅) | 33.0 |
| 136 B | encode(parsed) | 3074.4 (4.14x ✅) | 742.4 |
| 174 B | encode(data) | 325.7 (5.61x ✅) | 58.1 |
| 174 B | decode(bytes) | 716.0 (1.28x ✅) | 559.2 |
| 174 B | to_dict(bytes) | 152.2 (5.72x ✅) | 26.6 |
| 174 B | encode(parsed) | 3773.7 (4.62x ✅) | 817.2 |
| 2469 B | encode(data) | 496.2 (6.48x ✅) | 76.6 |
| 2469 B | decode(bytes) | 1138.1 (1.59x ✅) | 717.3 |
| 2469 B | to_dict(bytes) | 200.2 (5.03x ✅) | 39.8 |
| 2469 B | encode(parsed) | 29958.6 (20.6x ✅) | 1452.1 |
| 74913 B | encode(data) | 590.9 (7.03x ✅) | 84.0 |
| 74913 B | decode(bytes) | 1369.8 (1.72x ✅) | 795.6 |
| 74913 B | to_dict(bytes) | 166.8 (3.91x ✅) | 42.7 |
| 74913 B | encode(parsed) | 83934.6 (78.8x ✅) | 1064.7 |

Rows are the same user-visible task on both sides:

- **encode(data)** starts from the same dict. upb has no dict entry point, so
  its side is filling a generated message field by field and then serializing;
  that population is most of the 874 µs on the 75 KB tier. pbwire walks the
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
| 136 B | encode | 1726.4 (0.76x ❌) | 2285.1 |
| 136 B | decode | 942.9 (1.48x ✅) | 635.1 |
| 174 B | encode | 1509.2 (1.28x ✅) | 1182.8 |
| 174 B | decode | 825.2 (1.47x ✅) | 560.4 |
| 2469 B | encode | 942.9 (1.09x ✅) | 861.4 |
| 2469 B | decode | 864.2 (2.03x ✅) | 426.5 |
| 74913 B | encode | 969.5 (1.06x ✅) | 914.9 |
| 74913 B | decode | 986.9 (2.47x ✅) | 398.9 |

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
codec pays a presence gate, a type dispatch and tag loads per field (~79 ns
including the `byte[]` copy). That floor is structural, not tuning: the
per-field work left -- loads, one `switch`, one call -- is what a generated
writer avoids by knowing every field at compile time. From 174 B up the lazy
decode and the fused writes win every cell, and by 2.5 KB the size pass
`protobuf-java` runs before writing (3000 nested messages at the xlarge tier)
costs more than the codec's dispatch does.

#### Node vs `google-protobuf` (jspb) and protobufjs

| payload | op | pbwire-node (MB/s, x protobufjs) | protobufjs (MB/s) | jspb (MB/s) |
|---|---|---|---|---|
| 136 B | encode | 610.8 (2.44x ✅) | 250.5 | 59.4 |
| 136 B | decode | 393.0 (0.98x ❌) | 400.3 | 121.0 |
| 174 B | encode | 560.5 (1.90x ✅) | 295.1 | 63.0 |
| 174 B | decode | 442.1 (1.15x ✅) | 383.0 | 109.9 |
| 2469 B | encode | 1392.0 (3.06x ✅) | 454.6 | 54.4 |
| 2469 B | decode | 1111.3 (2.89x ✅) | 384.6 | 78.7 |
| 74913 B | encode | 1529.8 (3.07x ✅) | 498.5 | 61.4 |
| 74913 B | decode | 1645.9 (4.21x ✅) | 391.3 | 82.9 |

jspb's throughput does not move with payload size: its writer resolves a
code/type table per field and hands every repeated element to a closure, where
the C core writes one tag byte and one inline value, or memcpys an already
validated slice. protobufjs, the fastest widely used JS runtime, is 4-8x
faster than jspb on the same bytes (and the harness gates its bytes too):
the ratio column above is against it — 3.07x on xlarge encode and 4.21x on
decode. The ~25x over jspb is the legacy-baseline gap; the ~3-4x is the codec
gap. Only the small-tier decode still trails protobufjs (0.98x): one N-API
downcall on a 136 B payload, where the boundary is the whole budget.

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
| 136 B | encode | 967.1 (1.38x ✅) | 700.4 |
| 136 B | decode | 383.3 (1.56x ✅) | 245.4 |
| 174 B | encode | 882.9 (1.11x ✅) | 794.7 |
| 174 B | decode | 489.4 (1.90x ✅) | 257.4 |
| 2469 B | encode | 1516.9 (1.23x ✅) | 1231.5 |
| 2469 B | decode | 981.0 (1.95x ✅) | 502.7 |
| 74913 B | encode | 1641.5 (1.25x ✅) | 1318.1 |
| 74913 B | decode | 1207.6 (1.76x ✅) | 688.0 |

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

All four tiers sit at or above parity (the closest row is the 174 B encode at
1.12x); above it the C core's pack-aware writer pulls ahead.

#### Objective-C vs the `protobuf` ObjC runtime

| payload | op | pbwire-objc (MB/s, x protobuf-objc) | protobuf-objc (MB/s) |
|---|---|---|---|
| 136 B | encode | 1077.4 (6.5x ✅) | 164.9 |
| 136 B | decode | 747.8 (2.9x ✅) | 255.3 |
| 174 B | encode | 1384.6 (6.6x ✅) | 210.9 |
| 174 B | decode | 943.3 (3.2x ✅) | 298.4 |
| 2469 B | encode | 13317.2 (4.7x ✅) | 2851.5 |
| 2469 B | decode | 11877.2 (3.2x ✅) | 3735.6 |
| 74913 B | encode | 74470.3 (1.87x ✅) | 39748.6 |
| 74913 B | decode | 81497.8 (2.89x ✅) | 28223.3 |

The ARC wrapper adds one Objective-C object per call and no copy: `encode` hands
the encoder's own malloc'd buffer to `NSData` with
`dataWithBytesNoCopy:length:freeWhenDone:`, and `decode` wraps the decoder's
arena in one `PBWMessage`. That stays ahead of the generated runtime on all four
tiers and both ops (6.5x / 2.9x at 136 B down to 2.0x / 2.8x at 74913 B). The
xlarge tier is one 74 KB string, so it is a memcpy race: the generated encoder
writes it into an `NSMutableData` of the final size, and the wrapper writes it
into its own buffer, which `NSData` then takes over.

#### Kotlin vs `protoc --kotlin_out` over `protobuf-java`

| payload | op | pbwire-kotlin (MB/s, x protobuf-kotlin) | protobuf-kotlin (MB/s) |
|---|---|---|---|
| 136 B | encode | 1832.8 (0.80x ❌) | 2287.0 |
| 136 B | decode | 958.3 (1.44x ✅) | 665.4 |
| 174 B | encode | 1546.5 (1.28x ✅) | 1206.4 |
| 174 B | decode | 830.6 (1.40x ✅) | 593.6 |
| 2469 B | encode | 969.6 (1.11x ✅) | 871.6 |
| 2469 B | decode | 864.0 (1.84x ✅) | 470.8 |
| 74913 B | encode | 954.0 (1.05x ✅) | 905.5 |
| 74913 B | decode | 952.7 (2.08x ✅) | 458.8 |

Kotlin reuses the Java runtime (`bindings/java/runtime`), so these numbers are
`pbwire-java`'s: one workspace lookup plus one `byte[]` copy per call. The 136 B encode
is the case the Java section explains (a generated writer knows its tags, the
dynamic tree walk derives them), here 0.80x rather than 0.76x because the
Kotlin baseline reaches the same protobuf-java encoder through its builder/DSL
path. From 174 B up both ops are ahead at every tier.

#### PHP vs the official PHP runtime (pure + C extension)

| payload | op | pbwire-php-c (MB/s, x protobuf-php-c) | protobuf-php-c (MB/s) |
|---|---|---|---|
| 136 B | encode | 1307.2 (1.56x ✅) | 839.8 |
| 136 B | decode | 795.3 (1.90x ✅) | 419.4 |
| 174 B | encode | 1305.5 (1.47x ✅) | 886.8 |
| 174 B | decode | 756.9 (1.86x ✅) | 406.0 |
| 2469 B | encode | 1493.2 (1.18x ✅) | 1266.6 |
| 2469 B | decode | 623.1 (1.26x ✅) | 494.3 |
| 74913 B | encode | 1422.8 (1.13x ✅) | 1256.0 |
| 74913 B | decode | 688.4 (1.28x ✅) | 538.5 |

The native extension links the C core in, like the Ruby C extension: built
messages live in a per-message arena (one bump allocator instead of a malloc
per value, sequential layout for the encoder), decoded trees keep the core's
single-malloc arena, and both encode straight into the result string. Against
the official C extension — the same official API backed by C, which `bench.sh`
times whenever it is installed — that wins every tier, 1.1-1.6x on encode
and 1.3-1.9x on decode. The `ext-ffi` wrapper runs the same core through one FFI trampoline per call:
it trails on small payloads (0.30x / 0.33x encode at 136-174 B) and converges
from 2.5 KB up (xlarge 1.01x / 1.33x vs the C extension in the same
harness). The pure-PHP runtime, timed by a second `php -n` run
in the same harness, is a different race:

| payload | op | pbwire-php (MB/s, x protobuf-php) | protobuf-php (MB/s) |
|---|---|---|---|
| 136 B | encode | 230.3 (64.0x ✅) | 3.6 |
| 136 B | decode | 215.6 (35.3x ✅) | 6.1 |
| 174 B | encode | 296.0 (80.0x ✅) | 3.7 |
| 174 B | decode | 264.4 (44.8x ✅) | 5.9 |
| 2469 B | encode | 1053.0 (301x ✅) | 3.5 |
| 2469 B | decode | 577.8 (116x ✅) | 5.0 |
| 74913 B | encode | 1282.1 (356x ✅) | 3.6 |
| 74913 B | decode | 688.0 (132x ✅) | 5.2 |

The pure-PHP runtime spends its time on per-field bookkeeping (~4-6 MB/s flat
across tiers), so quoting those ratios without the C-extension table is
quoting the interpreter, not the codec.

#### Dart vs the `protobuf` Dart package

| payload | op | pbwire-dart (MB/s, x protobuf-dart) | protobuf-dart (MB/s) |
|---|---|---|---|
| 136 B | encode | 734.9 (3.6x ✅) | 203.7 |
| 136 B | decode | 377.8 (1.8x ✅) | 206.4 |
| 174 B | encode | 699.2 (4.1x ✅) | 171.4 |
| 174 B | decode | 406.3 (2.2x ✅) | 182.5 |
| 2469 B | encode | 1210.3 (10.2x ✅) | 118.7 |
| 2469 B | decode | 602.9 (4.3x ✅) | 140.6 |
| 74913 B | encode | 1454.3 (16.4x ✅) | 88.5 |
| 74913 B | decode | 678.3 (4.2x ✅) | 161.8 |

Both sides allocate per call. `dart:ffi` adds a `malloc`/copy for the input
bytes and a `Uint8List` copy for the output, which is why the gain grows with
payload size: the Dart runtime's pure-Dart field loop dominates it by 2.5 KB,
and at 136 B the boundary is still visible in a 1.8x decode.

#### Rust vs prost (and `rust-protobuf` 3.x)

| payload | op | pbwire-rust (MB/s, x prost) | prost (MB/s) | protobuf-rust (MB/s) |
|---|---|---|---|---|
| 136 B | encode | 1786.5 (1.13x ✅) | 1579.1 | 1290.6 |
| 136 B | decode | 1068.6 (1.15x ✅) | 927.5 | 743.3 |
| 174 B | encode | 1735.7 (1.14x ✅) | 1524.0 | 1330.7 |
| 174 B | decode | 960.4 (1.48x ✅) | 648.5 | 567.2 |
| 2469 B | encode | 1618.0 (1.18x ✅) | 1375.2 | 804.2 |
| 2469 B | decode | 658.3 (1.87x ✅) | 351.1 | 354.5 |
| 74913 B | encode | 1579.2 (1.11x ✅) | 1425.5 | 875.0 |
| 74913 B | decode | 715.4 (1.83x ✅) | 390.1 | 380.2 |

The archive is linked in, so a call is a direct C call with no `dlopen` lookup.
prost, the fastest widely used Rust runtime (generated at build time by
build.rs, bytes gated like the rest), loses every cell now that encode reuses
one buffer per implementation (pbwire `encode_to()`, rust-protobuf
`write_to_vec()`, prost `encode()`): the one-shot `encode()` paid the core
malloc plus a 75 KB copy that neither baseline pays. pbwire-rust leads every
cell against both (xlarge encode 1.11x / 1.80x, decode 1.83x / 1.88x).

#### C# vs Google.Protobuf

| payload | op | pbwire-csharp (MB/s, x protobuf-csharp) | protobuf-csharp (MB/s) | protobuf-net (MB/s) |
|---|---|---|---|---|
| 136 B | encode | 1351.5 (2.96x ✅) | 456.6 | 550.0 |
| 136 B | decode | 1084.0 (2.29x ✅) | 473.3 | 448.7 |
| 174 B | encode | 1355.2 (3.34x ✅) | 405.5 | — |
| 174 B | decode | 953.0 (2.44x ✅) | 391.2 | — |
| 2469 B | encode | 1232.8 (2.15x ✅) | 573.1 | 389.0 |
| 2469 B | decode | 664.1 (1.44x ✅) | 459.8 | 161.5 |
| 74913 B | encode | 1425.9 (2.88x ✅) | 495.6 | 399.3 |
| 74913 B | decode | 719.7 (1.40x ✅) | 513.8 | 179.5 |

Google.Protobuf's C# runtime is already a fast tree codec, so this is the
closest race of the five. Every cell is at or above parity -- the closest is
the xlarge decode at 1.40x -- and the encode side
stays ~2.2-3.3x ahead at every tier. protobuf-net, the other widely used C#
runtime (hand-written contracts in Pbn.cs, bytes gated like the rest), is
slower than Google.Protobuf on every tier here (xlarge 399.3 vs 495.6
encode): the official runtime is the fastest baseline, so the ratio column
is against it. Its medium rows are
absent on purpose: protobuf-net writes single-element packed fields in
singular form — valid protobuf, semantically equal (the harness checks that
via the Google parser), but not byte-identical, so there is nothing MB/s-fair
to time there.

These rows land at ~1.5 GB/s encode and ~680 MB/s decode on the xlarge tier, which
is the C core's own number: by 2.5 KB the per-call boundary is invisible and only
the baseline's own codec is being measured. That is the point of the schema-at-
runtime design -- the binding owns ergonomics, the core owns the format.

### Lua (C module, tables) vs `lua-protobuf`

| payload | op | pbwire-lua (MB/s, x lua-protobuf) | lua-protobuf (MB/s) |
|---|---|---|---|
| 136 B | encode | 132.2 (1.04x ✅) | 127.4 |
| 136 B | encode_msg | 949.3 (7.45x ✅) | 127.4 |
| 136 B | decode | 101.8 (1.13x ✅) | 89.8 |
| 174 B | encode | 149.6 (1.06x ✅) | 140.7 |
| 174 B | encode_msg | 1020.8 (7.26x ✅) | 140.7 |
| 174 B | decode | 101.7 (1.14x ✅) | 89.2 |
| 2469 B | encode | 178.5 (1.05x ✅) | 169.9 |
| 2469 B | encode_msg | 1257.6 (7.40x ✅) | 169.9 |
| 2469 B | decode | 105.6 (1.19x ✅) | 88.7 |
| 74913 B | encode | 196.4 (1.04x ✅) | 188.4 |
| 74913 B | encode_msg | 1594.8 (8.47x ✅) | 188.4 |
| 74913 B | decode | 105.5 (1.16x ✅) | 91.1 |

`encode_msg` is the builder hot path (`schema:message()` once, then
`encode()` per send): one core walk plus one buffer malloc, no table
conversion, which is why it runs at the core's own ~1.0-1.5 GB/s. The table
path writes straight from the Lua table into the wire buffer (one pass over
schema fields in descriptor order, no intermediate tree, no per-value
allocation), so it beats the baseline at every tier. Decode wins at every tier because
it is one validating parse straight into tables with no tree kept. The gate
is tier size (the bytes are canonical and match `protoc --encode` on the
small tier) plus a full-field expectation check; the baseline only has to
agree on size and decode, because `lua-protobuf` writes nested messages in
table hash order, so raw baseline bytes are not comparable.

### Erlang: `pbwire` vs gpb vs gpb NIF

`matrix3.proto`, all four tiers, best of three samples per op. Same payload / op /
MB/s layout as every other table: the `pbwire-erlang` column carries the ratio
against the faster of gpb / gpb NIF per op (gpb is the byte reference either
way), the other columns are the two baselines side by side.

| payload | op | pbwire-erlang (MB/s, x best) | gpb (MB/s) | gpb-nif (MB/s) |
|---|---|---|---|---|
| 136 B | encode | 471.2 (2.33x ✅) | 202.0 | 61.1 |
| 136 B | decode | 519.1 (2.28x ✅) | 227.6 | 67.5 |
| 174 B | encode | 455.7 (3.26x ✅) | 139.7 | 82.0 |
| 174 B | decode | 482.2 (2.17x ✅) | 221.9 | 98.1 |
| 2469 B | encode | 423.4 (3.68x ✅) | 114.9 | 94.3 |
| 2469 B | decode | 327.9 (1.67x ✅) | 196.3 | 170.1 |
| 74913 B | encode | 477.8 (3.91x ✅) | 121.3 | 122.2 |
| 74913 B | decode | 364.6 (1.66x ✅) | 220.2 | 212.3 |

### Swift (SwiftPM, static link) vs `apple/swift-protobuf`

| payload | op | pbwire-swift (MB/s, x swift-protobuf) | swift-protobuf (MB/s) |
|---|---|---|---|
| 136 B | encode | 1073.2 (2.45x ✅) | 438.7 |
| 136 B | decode | 957.3 (3.92x ✅) | 244.4 |
| 174 B | encode | 1165.4 (3.53x ✅) | 329.7 |
| 174 B | decode | 876.6 (4.41x ✅) | 199.0 |
| 2469 B | encode | 1354.5 (5.15x ✅) | 262.9 |
| 2469 B | decode | 673.4 (3.40x ✅) | 197.8 |
| 74913 B | encode | 1564.2 (5.57x ✅) | 280.9 |
| 74913 B | decode | 589.7 (2.64x ✅) | 223.2 |

FFI host like Objective-C/Dart: `FileDescriptorSet` at run time, C ABI calls
only, no wire code, no plugin. SwiftPM links `build/libpbwire.a` via
`unsafeFlags` (so plain `swift test` works offline); the baseline is a
separate package using swift-protobuf's own build plugin, so no generated
code is checked in. The gate is byte parity per tier plus the pinned
`matrix3.bin` oracle.

### Scala (Java runtime + DSL) vs ScalaPB

| payload | op | pbwire-scala (MB/s, x ScalaPB) | ScalaPB (MB/s) |
|---|---|---|---|
| 136 B | encode | 1741.9 (1.14x ✅) | 1527.2 |
| 136 B | decode | 923.1 (1.20x ✅) | 772.2 |
| 174 B | encode | 1445.6 (1.44x ✅) | 1004.3 |
| 174 B | decode | 795.8 (1.03x ✅) | 776.1 |
| 2469 B | encode | 948.0 (1.22x ✅) | 774.9 |
| 2469 B | decode | 864.1 (1.40x ✅) | 615.8 |
| 74913 B | encode | 980.9 (1.17x ✅) | 836.8 |
| 74913 B | decode | 956.7 (1.62x ✅) | 590.9 |

Same class as Kotlin: the JVM binding reuses the Java runtime and adds only
an idiomatic DSL — no second codec, no JNI, schema from a `FileDescriptorSet`
at run time. The baseline is `scalapbc`-generated code (needs `cs install
scalapbc` once); the jars land in `benchmarks/deps`. Byte parity per tier
before timing, like every other row.

### Elixir (NIF over the shared core) vs `elixir-protobuf`

| payload | op | pbwire-elixir (MB/s, x elixir-protobuf) | elixir-protobuf (MB/s) |
|---|---|---|---|
| 136 B | encode | 148.2 (3.08x ✅) | 48.1 |
| 136 B | decode | 85.0 (1.42x ✅) | 59.8 |
| 174 B | encode | 146.0 (2.94x ✅) | 49.7 |
| 174 B | decode | 94.7 (1.75x ✅) | 54.0 |
| 2469 B | encode | 130.8 (2.37x ✅) | 55.3 |
| 2469 B | decode | 87.1 (1.61x ✅) | 54.1 |
| 74913 B | encode | 115.3 (2.00x ✅) | 57.5 |
| 74913 B | decode | 67.4 (1.25x ✅) | 54.1 |

Same class as Erlang: the BEAM binding reuses the published `:pbwire` Hex
NIF (the same C core as `bindings/erlang`) and owns only the struct↔tuple
translation; `elixir-protobuf` is the schema plugin, like gpb is for Erlang.
Every encode is byte-identical to `Protobuf.encode/1` before timing. Known
deliberate divergences from the oracle: NaN bit identity, unknown-field drop
(the NIF tuple form has no storage), raise on missing `required`.

### Zig (static link) vs Arwalk/zig-protobuf

| payload | op | pbwire-zig (MB/s, x zig-protobuf) | zig-protobuf (MB/s) |
|---|---|---|---|
| 136 B | encode | 1880.7 (1.51x ✅) | 1244.6 |
| 136 B | decode | 1147.1 (45.18x ✅) | 25.4 |
| 174 B | encode | 1837.4 (1.79x ✅) | 1026.8 |
| 174 B | decode | 1040.1 (51.33x ✅) | 20.3 |
| 2469 B | encode | 1612.1 (1.89x ✅) | 851.3 |
| 2469 B | decode | 669.1 (11.31x ✅) | 59.2 |
| 74913 B | encode | 1537.6 (1.83x ✅) | 840.4 |
| 74913 B | decode | 716.9 (2.28x ✅) | 314.0 |

Same class as Rust: the core sources compile into a static archive that links
into one self-contained binary, schema from a `FileDescriptorSet` at run
time. The baseline is Arwalk/zig-protobuf 7.0.0 generated code from the same
`matrix3.proto` (tarball pinned in `build.zig.zon`, fetched on first run and
cached; generation runs the system `protoc` against their plugin, and the
`test` / plain `bench` steps never resolve the dependency, so they stay
offline-safe — `bench.sh` runs pbwire-only by default, `--baseline` (what
`make zig-bench` passes) runs the comparison). Bytes are gated equal on all
four tiers before timing. The encode gap stays narrow: generated straight-line
writers know every tag at compile time, the dynamic dispatch costs ~2x at
most. The decode gap is structural the other way: the baseline materializes
every field with its own allocation (which is also what the small tiers time
most of), while the binding decodes into one arena; the lead shrinks as the
payload grows and per-byte work takes over. At `-Doptimize=ReleaseFast` (what
`bench.sh` runs) both sides sit near the core's pace; the default debug build
is ~20x slower and is not timed.

### F# (facade over the C# runtime) vs Google.Protobuf

| payload | op | pbwire-fsharp (MB/s, x Google.Protobuf) | Google.Protobuf (MB/s) |
|---|---|---|---|
| 136 B | encode | 1333.4 (2.99x ✅) | 445.5 |
| 136 B | decode | 1072.7 (2.29x ✅) | 468.1 |
| 174 B | encode | 1341.1 (3.33x ✅) | 402.7 |
| 174 B | decode | 936.4 (2.42x ✅) | 387.7 |
| 2469 B | encode | 1306.3 (2.34x ✅) | 557.4 |
| 2469 B | decode | 669.2 (1.42x ✅) | 470.5 |
| 74913 B | encode | 1436.1 (3.00x ✅) | 477.9 |
| 74913 B | decode | 710.1 (1.42x ✅) | 500.1 |

Same class as Kotlin: F# reuses the C# binding's project via
`ProjectReference` and adds only an F#-idiomatic facade — no second runtime,
no packages, schema from a `FileDescriptorSet` at run time. The baseline is
`protoc`-generated C# compiled in a sibling project (F# cannot compile
`.cs`). Byte parity per tier before timing.

### Clojure (Java runtime + interop) vs `protobuf-java`

| payload | op | pbwire-clj (MB/s, x protobuf-java) | protobuf-java (MB/s) |
|---|---|---|---|
| 136 B | encode | 1722.0 (9.66x ✅) | 178.3 |
| 136 B | decode | 932.1 (1.38x ✅) | 675.9 |
| 174 B | encode | 1409.4 (6.70x ✅) | 210.2 |
| 174 B | decode | 819.4 (1.38x ✅) | 594.2 |
| 2469 B | encode | 917.4 (1.30x ✅) | 705.0 |
| 2469 B | decode | 870.8 (1.90x ✅) | 458.8 |
| 74913 B | encode | 939.2 (1.05x ✅) | 893.7 |
| 74913 B | decode | 972.7 (2.19x ✅) | 444.4 |

The timed path reuses a pre-built mutable message (fill once, encode many;
decode plus close, no map materialization), like the Java row, so it beats
generated straight-line code. The persistent-map fns stay for convenience
and are gated for parity, but hot loops use the mutable path — the same
split Lua documents (table vs builder). Same codec as Java/Scala underneath
(plain interop over the Java runtime, no second wire implementation), byte
parity per tier.

### Julia (`dlopen` + `ccall`) vs ProtoBuf.jl

| payload | op | pbwire-julia (MB/s, x ProtoBuf.jl) | ProtoBuf.jl (MB/s) |
|---|---|---|---|
| 136 B | encode | 1145.3 (2.76x ✅) | 414.9 |
| 136 B | decode | 1118.1 (0.91x ❌) | 1235.3 |
| 174 B | encode | 1257.8 (3.65x ✅) | 344.6 |
| 174 B | decode | 1003.5 (1.32x ✅) | 760.5 |
| 2469 B | encode | 1293.1 (9.44x ✅) | 137.0 |
| 2469 B | decode | 682.9 (1.06x ✅) | 643.1 |
| 74913 B | encode | 1445.3 (126.8x ✅) | 11.4 |
| 74913 B | decode | 726.3 (0.99x ❌) | 731.5 |

FFI host like Dart/C#: `Libdl.dlopen` + `ccall` into the C ABI (function
pointers resolved once via `dlsym`, since recent Julia needs a compile-time
library name), `Dict`-based messages convert through the C ABI, no wire
code, no `.proto` parser. The baseline is `protojl`-generated code
(`benchmarks/oracle/matrix3_pb.jl`, produced by `protojl("matrix3.proto",
"<proto dir>", "benchmarks/oracle")`). Its encode collapses with size
(pure-Julia per-field appends). Decode sits at parity: both sides land near
the C core's own ~700 MB/s, so the medium and large tiers lead while the small
tier trails on fixed FFI overhead (like Java/Kotlin small-tier floors); the
xlarge tier lands within run noise either side of 1.0x, and the headline
remains the xlarge encode column.

### R (C extension, named lists) vs RProtoBuf

| payload | op | pbwire-r (MB/s, x RProtoBuf) | RProtoBuf (MB/s) |
|---|---|---|---|
| 136 B | encode | 99.6 (35.6x ✅) | 2.8 |
| 136 B | decode | 77.1 (28.6x ✅) | 2.7 |
| 174 B | encode | 114.9 (38.3x ✅) | 3.0 |
| 174 B | decode | 84.5 (28.2x ✅) | 3.0 |
| 2469 B | encode | 163.0 (60.4x ✅) | 2.7 |
| 2469 B | decode | 124.1 (41.4x ✅) | 3.0 |
| 74913 B | encode | 186.4 (66.5x ✅) | 2.8 |
| 74913 B | decode | 141.1 (52.3x ✅) | 2.7 |

Same class as Lua/Ruby: a C extension over the core (named lists in/out
through one transient tree per call, statically linked), schema from
a `FileDescriptorSet` at run time. The wrappers scope every `.Call` to
`PACKAGE = "pbwire"`: a bare entry-point name re-searches every loaded DLL
per call, which costs 30+ us once a native-heavy package (RProtoBuf itself,
Rcpp) shares the session; the registered-table hit is ~100 ns. Bytes are
gated equal on all four tiers before timing (RProtoBuf takes the `u64` max as
its decimal string — R numerics cannot spell it — everything else is the same
R values pbwire encodes), and the materialized decode is gated identical too,
plus a full decode round trip. Like Lua, the one-shot
list path pays a conversion per field while the builder path (`pbwire_message`
once, `encode` per send, 1444.1 MB/s on the xlarge tier) runs at the core's
speed; the table above times the one-shot path both sides. Encode is no
contest at any tier: setting 3000 nested messages field by field through S4
dispatch costs RProtoBuf ~27 ms where the C core walks the list once. Decode
is compared like for like: `RProtoBuf::read` parses in C++ and materializes
fields lazily on `$`, while `pbwire_decode` builds the full named-list tree
eagerly, so the baseline side reads every field into the same plain-list shape
(the table's `decode` row). A bare `read()` alone is a few times faster than
`pbwire_decode`, but it hands back a Message, not R data; once both sides
produce plain lists, pbwire leads 28-52x. Two R spellings to know: `INT32_MIN`
reads back
as `NA` (it *is* `NA_INTEGER`), and doubles past 2^63 clamp to the int/uint
limits — both documented in the binding README.

## Run them

```bash
make bench        # core: 4 cases + text/json/stream/threads (needs protoc + libprotobuf)
make bench-c      # C structs vs protoc C++ on matrix3.proto (same deps)
make python-bench # Python extension vs google.protobuf (needs protobuf + protoc)
make erlang-bench # Erlang binding: epb, gpb, gpb NIF on matrix3.proto (needs OTP + gpb)
make go-bench     # Go cgo dynamic vs protobuf-go generated code (needs Go + protoc-gen-go)
make java-bench   # Java FFM vs protobuf-java (fetches the jar from Maven Central once)
make node-bench   # N-API addon vs google-protobuf/jspb (npm installs into the harness)
make ruby-bench   # Ruby C extension vs the google-protobuf gem (`RUBY=...` picks the interpreter)
make objc-bench   # ARC wrapper vs the protobuf ObjC runtime (needs `make conformance-setup`)
make kotlin-bench # Kotlin DSL vs protoc --kotlin_out over protobuf-java (fetches two jars once)
make php-bench    # ext-ffi binding vs the official PHP runtime (needs `make conformance-setup`)
make dart-bench   # dart:ffi binding vs the protobuf Dart package (needs `dart pub global activate protoc_plugin`)
make rust-bench   # static-link binding vs rust-protobuf (needs `cargo install protobuf-codegen`)
make csharp-bench # P/Invoke binding vs Google.Protobuf (NuGet restore on first run)
make lua-bench    # C-module binding vs lua-protobuf (luarocks install lua-protobuf for the baseline)
make swift-bench  # SwiftPM binding vs apple/swift-protobuf (needs swift-protobuf 1.39.0 once, pinned)
make scala-bench  # Scala DSL vs ScalaPB (fetches ScalaPB + protobuf-java jars once, needs scalapbc)
make elixir-bench # Elixir NIF wrapper vs elixir-protobuf (mix fetches hex deps once)
make zig-bench    # static-link binding vs Arwalk/zig-protobuf (package fetch needs network once)
make fsharp-bench # F# facade vs Google.Protobuf (NuGet restore on first run)
make clojure-bench # Clojure interop vs protobuf-java (fetches jars from Maven Central once)
make julia-bench  # ccall binding vs ProtoBuf.jl (generate benchmarks/oracle with protojl once)
make r-bench      # R C extension vs RProtoBuf (install RProtoBuf for the baseline columns)
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
   ops' counts differ silently. Erlang calibrates one
   count per case on its slowest op instead, which is equivalent when the
   backends sit within a factor of a few, as they do.
3. **Warm up** one full pass at the calibrated count, then report the
   **best of 3 timed passes** (interleaved per op where the harness
   supports it, so a load spike hits both implementations equally).

One harness meets the same bar with its own calibrator instead of the
shared doubling loop: the core/C++ harnesses keep an explicit count only when one is
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
Python, Java, Kotlin, Node, Ruby, Objective-C, PHP, Dart, Rust, C#, Lua, Go,
Swift, Scala, Elixir, Zig, F#, Clojure, Julia and R tables are
`make bench-c` / `make python-bench` / `make java-bench` /
`make kotlin-bench` / `make node-bench` / `make ruby-bench` / `make objc-bench` /
`make php-bench` / `make dart-bench` / `make rust-bench` / `make csharp-bench` /
`make lua-bench` / `make go-bench` / `make swift-bench` / `make scala-bench` /
`make elixir-bench` / `make zig-bench` / `make fsharp-bench` /
`make clojure-bench` / `make julia-bench` / `make r-bench`
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

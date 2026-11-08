# pbwire performance benchmarks

This is the performance document for the whole project: the pbwire core, the C
struct codec and every host binding, all measured on the same four payload
tiers and each against its official counterpart, so the numbers can be read
across languages. This file is the collection entry: the
[at-a-glance table](#at-a-glance-every-binding-one-tier) is the one-glance
view, and each per-language section below pairs its description and full
tier table with a chart (see the [per-binding index](#cross-binding-comparison-last-run)).

- [Standard cases](#standard-cases) — what the four tiers are
- [Output format](#output-format) — what one harness block means
- [Test environment](#test-environment) — the machine and toolchain behind every number below
- [At a glance](#at-a-glance-every-binding-one-tier) — every binding, one tier
- [Linux (docker)](#linux-docker-the-same-harnesses-one-container-away) — the same harnesses in a Linux container, paired with the host run
- [Bindings vs their baselines](#bindings-vs-their-baselines) — who is ahead and by how much
- [Core](#core-pbw_ctx-vs-plain-vs-generated) — ctx vs plain vs generated vs `DynamicMessage`, threads, streaming
- [Measurement notes](#measurement-notes-the-core-fast-path) — A/B numbers behind the core's fast paths
- [Attempts ledger](docs/perf-attempts.md) — every optimization tried against a below-baseline cell, kept or rejected, with its numbers
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

Last full refresh: 2026-11-06, all rows below:

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
| core (dynamic) | `make bench` (`bench/bench.cc`) | `protoc` C++ generated + DynamicMessage | all 4 | 1.43x / 1.61x ✅ |
| C structs | `make bench-c` (`bindings/c/benchmarks/bench_struct.cc`) | `protoc` C++ generated from the same `matrix3.proto` | all 4 | 1.30x / 2.31x ✅ |
| Erlang/OTP NIF | `make benchmarks` in `bindings/erlang/benchmarks` | gpb (pure Erlang), with gpb NIF (libprotobuf) as the second baseline, same `matrix3.proto` | all 4 | 3.92x / 1.68x vs best-of-two ✅ |
| Python (CPython ext) | `make python-bench` (`bindings/python/benchmarks/bench.py`) | `google.protobuf` (upb C extension) on the same `matrix3.proto` | all 4 | 6.91x / 1.70x ✅ |
| C++ | same engine as C (`bindings/cpp` mirrors the C structs; `make bench-c` is the harness) | `protoc` C++ generated from the same `matrix3.proto` | all 4 | 1.30x / 2.31x ✅ |
| Go (`cgo` dynamic) | `make go-bench` (`bindings/go/benchmarks`) | `protobuf-go` generated code from the same `matrix3.proto` | all 4 | 2.68x / 2.23x ✅ |
| Java (pure-Java codec) | `make java-bench` (`bindings/java/benchmarks`) | `protobuf-java` generated code from the same `matrix3.proto` | all 4 | 1.17x / 2.36x ✅ (0.82x at 136 B) |
| Node (N-API addon) | `make node-bench` (`bindings/node/benchmarks`) | [protobufjs](https://www.npmjs.com/package/protobufjs) (fastest) + `google-protobuf` (jspb) from the same `matrix3.proto` | all 4 | 2.89x / 4.15x vs protobufjs ✅ |
| Ruby (C extension) | `make ruby-bench` (`bindings/ruby/benchmarks`) | the `google-protobuf` gem (C extension) | all 4 | 1.21x / 1.68x ✅ |
| Objective-C (ARC) | `make objc-bench` (`bindings/objc/benchmarks`) | the `protobuf` ObjC runtime + generated code from the same `matrix3.proto` | all 4 | 2.03x / 2.92x ✅ |
| Kotlin (pure-Java codec + DSL) | `make kotlin-bench` (`bindings/kotlin/benchmarks`) | `protoc --kotlin_out` over `protobuf-java`, same `matrix3.proto` | all 4 | 1.09x / 2.07x ✅ (0.80x at 136 B) |
| PHP (`ext-ffi` + C extension) | `make php-bench` (`bindings/php/benchmarks`) | the official PHP runtime (`protobuf/php/src`): C extension (fastest) + pure-PHP | all 4 | 1.06x / 1.21x vs C ext ✅ (342x / 130x vs pure) |
| Dart (`dart:ffi`) | `make dart-bench` (`bindings/dart/benchmarks`) | the `protobuf` Dart package + generated code | all 4 | 15.4x / 4.27x ✅ |
| Rust (static link) | `make rust-bench` (`bindings/rust/benchmarks`) | prost (fastest) + rust-protobuf 3.x generated code | all 4 | 1.08x / 1.87x vs prost ✅ |
| C# (P/Invoke) | `make csharp-bench` (`bindings/csharp/benchmarks`) | Google.Protobuf C# generated code (fastest) + protobuf-net | all 4 | 2.91x / 1.42x vs official ✅ |
| Lua (C module, tables) | `make lua-bench` (`bindings/lua/benchmarks`) | `lua-protobuf` 0.6.0 | all 4 | 1.10x / 1.44x ✅ (8.21x via builder encode) |
| Swift (SwiftPM, static link) | `make swift-bench` (`bindings/swift/benchmarks`) | `apple/swift-protobuf` 1.39.0 generated code from the same `matrix3.proto` | all 4 | 5.45x / 2.69x ✅ |
| Scala (Java runtime + DSL) | `make scala-bench` (`bindings/scala/benchmarks`) | ScalaPB 0.11.18 generated code from the same `matrix3.proto` | all 4 | 1.20x / 1.46x ✅ |
| Elixir (NIF over the shared core) | `make elixir-bench` (`bindings/elixir/benchmarks`) | `elixir-protobuf` 0.17.1 structs from the same `matrix3.proto` | all 4 | 2.30x / 1.40x ✅ |
| Zig (static link) | `make zig-bench` (`bindings/zig/benchmarks`, `-Doptimize=ReleaseFast`) | Arwalk/zig-protobuf 7.0.0 generated code from the same `matrix3.proto` | all 4 | 1.87x / 2.27x ✅ |
| F# (facade over the C# runtime) | `make fsharp-bench` (`bindings/fsharp/benchmarks`) | Google.Protobuf C# generated code from the same `matrix3.proto` | all 4 | 2.93x / 1.43x ✅ |
| Clojure (Java runtime + interop) | `make clojure-bench` (`bindings/clojure/benchmarks`) | `protobuf-java` generated code from the same `matrix3.proto` | all 4 | 1.08x / 2.16x ✅ |
| Julia (`ccall` FFI) | `make julia-bench` (`bindings/julia/benchmarks`) | JuliaIO/ProtoBuf.jl 1.3.0 generated code from the same `matrix3.proto` | all 4 | 123.9x / 1.01x ✅/✅ (decodes tie from 2.5 KB up) |
| R (C extension, named lists) | `make r-bench` (`bindings/r/benchmarks`) | RProtoBuf 0.4.28 on the same `matrix3.proto` | all 4 | 59.7x / 45.3x ✅ |

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
small-tier encode 0.82x/0.80x, Julia small decode 0.92x,
Go small-tier encode near parity, the PHP FFI wrapper) —
boundary cost on a tiny payload, not a
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
| core dynamic (`pbw_ctx`) | direct call | 85933 B | 1674.9 (1.43x ✅) | 703.1 (1.61x ✅) |
| core dynamic (plain API) | direct call | 85933 B | 1531.7 (1.30x ✅) | 711.4 (1.63x ✅) |
| C structs | direct call, layout tables | 74913 B | 1213.2 (1.30x ✅) | 899.1 (2.31x ✅) |
| Go | `cgo` dynamic over the C core | 74913 B | 1508.7 (2.68x ✅) | 709.6 (2.23x ✅) |
| Python (dict path) | CPython extension | 74913 B | 593.1 (6.91x ✅) | 1376.6 (1.70x ✅) |
| Java | pure-Java codec, no native code | 74913 B | 1061.4 (1.17x ✅) | 968.7 (2.36x ✅) |
| Kotlin | the pure-Java codec + DSL | 74913 B | 1003.9 (1.09x ✅) | 973.2 (2.07x ✅) |
| Node | N-API addon | 74913 B | 1446.4 (2.89x ✅) | 1584.5 (4.15x ✅) |
| Ruby | C extension over the core | 74913 B | 1582.4 (1.21x ✅) | 1170.0 (1.68x ✅) |
| Objective-C | ARC wrapper | 74913 B (one 74 KB string) | 79927.5 (2.03x ✅) | 81115.5 (2.92x ✅) |
| PHP | C extension | 74913 B | 1378.4 (1.06x ✅) | 681.7 (1.21x ✅) |
| Dart | `dart:ffi` | 74913 B | 1430.7 (15.4x ✅) | 694.0 (4.27x ✅) |
| Rust | static link | 74913 B | 1398.4 (1.08x ✅) | 729.1 (1.87x ✅) |
| C# | P/Invoke | 74913 B | 1532.7 (2.91x ✅) | 745.6 (1.42x ✅) |
| Lua | C module, tables | 74913 B | 218.5 (1.10x ✅) | 141.2 (1.44x ✅) |
| Erlang | NIF (`pbwire`) | 74913 B | 428.9 (3.92x ✅) | 322.1 (1.68x ✅) |
| Swift | static link (SwiftPM) | 74913 B | 1543.2 (5.45x ✅) | 623.9 (2.69x ✅) |
| Scala | the Java runtime + DSL | 74913 B | 997.2 (1.20x ✅) | 947.3 (1.46x ✅) |
| Elixir | NIF (Hex `:pbwire`, same core as Erlang) | 74913 B | 145.6 (2.30x ✅) | 84.7 (1.40x ✅) |
| Zig | static link | 74913 B | 1548.2 (1.87x ✅) | 736.8 (2.27x ✅) |
| F# | P/Invoke over the C# runtime | 74913 B | 1441.6 (2.93x ✅) | 728.3 (1.43x ✅) |
| Clojure | the Java runtime + interop | 74913 B | 947.3 (1.08x ✅) | 951.0 (2.16x ✅) |
| Julia | `dlopen` + `ccall` | 74913 B | 1412.7 (123.9x ✅) | 727.3 (1.01x ✅) |
| R | C extension, named lists | 74913 B | 179.0 (59.7x ✅) | 135.8 (45.3x ✅) |

Read it as: the in-process dynamic core sets the pace (1.4x the C++
generated code at this size), a binding that links the core in (Ruby, Rust)
keeps within a few percent of it, and an FFI binding pays its boundary only
until the payload is large — Java, Kotlin, Ruby, Rust, C# and PHP all land at
~1.0-1.6 GB/s encode and ~680-1200 MB/s decode on the xlarge tier, which is
near the core's own number. The outliers are baseline or unit effects, not pbwire
effects: Python's encode column is the dict path (3000 sub-messages, 12000
ints), and the Objective-C tier is a memcpy race over one 74 KB string.
The headline always names the fastest baseline: Node's 2.89x / 4.15x is over
protobufjs (jspb, the legacy baseline, trails it by ~26x / ~19x), PHP's 1.06x /
1.21x is over the official C extension (the pure-PHP runtime trails it by
~342x / ~130x) — see their sections. Swift, Scala, F#, Zig and Julia join
the ~1.0-1.6 GB/s encode club on xlarge (Swift 5.45x and Julia 124x encode
are baseline effects: swift-protobuf and ProtoBuf.jl encode much slower than
their own decode).

## Linux (docker): the same harnesses, one container away

Every table above is Apple-silicon. The claims have to hold on the other
architecture too, so `make bench-docker` runs the same harnesses — same
payloads, same baselines, same measurement standard — inside
`docker/bench.Dockerfile`, and `make bench-compare` pairs the newest record
per binding/platform in `results/`, row by row:

```bash
make bench            # the macOS side (or whatever host you are benching)
make bench-docker     # the Linux side: core, c, go, rust, node, ruby, lua, java, python
make bench-compare    # newest pair per binding, xlarge tier, speedup per row
```

The image installs the pinned protobuf (so the generated baseline is the same
version as the tables above) plus the Go, Rust, Node, Ruby, Lua, Java and
Python toolchains. It covers what runs without an Apple toolchain:
Objective-C and Swift stay host-only, PHP needs the official C extension as its
baseline (Ubuntu packages none, and building it from source needs a generated
`utf8_range` amalgamation only protobuf's own build produces — a pure-PHP
baseline would be a ~300x different comparison), and the toolchains the image
does not install (Dart, Zig, C#/F#, Julia, R, Erlang/Elixir,
Kotlin/Scala/Clojure) run on the host only. `--bindings core,c` narrows a run,
`--rebuild` rebuilds the image, and the container always benches a *copy* of the
tree, so ELF objects never land in the host's `build/`.

Last run: 2026-11-07, `make bench-docker` on the image in the repo, `make
bench-compare` for the pairing. Absolute MB/s per row is what `--absolute`
prints; the speedup against the same baseline on the same side is what travels.

### core

- macOS: 2026-11-07T02:43:13+00:00 (Darwin 27.0.0, arm64, Apple M4 Pro, 14 cores, libprotoc 36.2)
- Linux: 2026-11-07T04:20:36+00:00 (Linux 6.8.0-64-generic container, aarch64, ?, 4 cores, libprotoc 36.2)

| case | op | macOS x baseline | Linux x baseline |
|---|---|---|---|
| xlarge | encode | 1.45x (pb gen) | 1.17x (pb gen) |
| xlarge | decode | 1.58x (pb gen) | 1.89x (pb gen) |

### c

- macOS: 2026-11-07T02:56:19+00:00 (Darwin 27.0.0, arm64, Apple M4 Pro, 14 cores, libprotoc 36.2)
- Linux: 2026-11-07T04:21:50+00:00 (Linux 6.8.0-64-generic container, aarch64, ?, 4 cores, libprotoc 36.2)

| case | op | macOS x baseline | Linux x baseline |
|---|---|---|---|
| xlarge | encode | 1.47x (pb gen) | 1.42x (pb gen) |
| xlarge | decode | 2.31x (pb gen) | 3.35x (pb gen) |

### go

- macOS: 2026-11-06T23:47:50+00:00 (Darwin 27.0.0, arm64, Apple M4 Pro, 14 cores, libprotoc 36.2)
- Linux: 2026-11-07T04:22:34+00:00 (Linux 6.8.0-64-generic container, aarch64, ?, 4 cores, libprotoc 36.2)

| case | op | macOS x baseline | Linux x baseline |
|---|---|---|---|
| xlarge | encode | 2.68x (protobuf-go) | 2.38x (protobuf-go) |
| xlarge | decode | 2.23x (protobuf-go) | 2.57x (protobuf-go) |

### python

- macOS: 2026-11-06T23:47:46+00:00 (Darwin 27.0.0, arm64, Apple M4 Pro, 14 cores, libprotoc 36.2)
- Linux: 2026-11-07T04:29:30+00:00 (Linux 6.8.0-64-generic container, aarch64, ?, 4 cores, libprotoc 36.2)

| case | op | macOS x baseline | Linux x baseline |
|---|---|---|---|
| xlarge | encode(data) | 7.24x (pb upb) | 7.49x (pb upb) |
| xlarge | decode(bytes) | 1.68x (pb upb) | 2.11x (pb upb) |
| xlarge | to_dict(bytes) | 3.89x (pb upb) | 5.17x (pb upb) |
| xlarge | encode(parsed) | 63.32x (pb upb) | 66.84x (pb upb) |

### java

- macOS: 2026-11-06T23:50:47+00:00 (Darwin 27.0.0, arm64, Apple M4 Pro, 14 cores, libprotoc 36.2)
- Linux: 2026-11-07T04:28:29+00:00 (Linux 6.8.0-64-generic container, aarch64, ?, 4 cores, libprotoc 36.2)

| case | op | macOS x baseline | Linux x baseline |
|---|---|---|---|
| xlarge | encode | 1.08x (protobuf-java) | 1.67x (protobuf-java) |
| xlarge | decode | 2.40x (protobuf-java) | 2.00x (protobuf-java) |

### node

- macOS: 2026-11-06T23:49:02+00:00 (Darwin 27.0.0, arm64, Apple M4 Pro, 14 cores, libprotoc 36.2)
- Linux: 2026-11-07T04:26:11+00:00 (Linux 6.8.0-64-generic container, aarch64, ?, 4 cores, libprotoc 36.2)

| case | op | macOS x baseline | Linux x baseline |
|---|---|---|---|
| xlarge | encode | 3.07x (protobufjs) | 5.22x (protobufjs) |
| xlarge | decode | 3.73x (protobufjs) | 2.74x (protobufjs) |

### ruby

- macOS: 2026-11-07T00:03:03+00:00 (Darwin 27.0.0, arm64, Apple M4 Pro, 14 cores, libprotoc 36.2)
- Linux: 2026-11-07T04:26:56+00:00 (Linux 6.8.0-64-generic container, aarch64, ?, 4 cores, libprotoc 36.2)

| case | op | macOS x baseline | Linux x baseline |
|---|---|---|---|
| xlarge | encode | 1.18x (google-protobuf) | 0.98x (google-protobuf) |
| xlarge | decode | 1.72x (google-protobuf) | 1.84x (google-protobuf) |

### rust

- macOS: 2026-11-07T00:08:28+00:00 (Darwin 27.0.0, arm64, Apple M4 Pro, 14 cores, libprotoc 36.2)
- Linux: 2026-11-07T04:23:57+00:00 (Linux 6.8.0-64-generic container, aarch64, ?, 4 cores, libprotoc 36.2)

| case | op | macOS x baseline | Linux x baseline |
|---|---|---|---|
| xlarge | encode | 1.12x (prost) | 1.15x (prost) |
| xlarge | decode | 1.71x (prost) | 1.04x (prost) |

### lua

- macOS: 2026-11-07T00:03:42+00:00 (Darwin 27.0.0, arm64, Apple M4 Pro, 14 cores, libprotoc 36.2)
- Linux: 2026-11-07T04:27:36+00:00 (Linux 6.8.0-64-generic container, aarch64, ?, 4 cores, libprotoc 36.2)

no comparable row for xlarge

Two caveats when reading it:

- On macOS the container runs in a Linux VM, so this column is *virtualized*
  pace (aarch64, 4 vCPU, a generic `virt` CPU model), not a server's: podman,
  docker, CI and a bare-metal Linux box each produce their own absolute numbers,
  and on this VM the speedup itself moves ~10% run to run. The ratio to the same
  baseline on the same side is the part that reproduces.
- Two rows do not reproduce here. Rust decode is 1.71x on the host and 1.04x in
  the VM (stable across two runs, with the absolute rate roughly halved, so the
  VM's cores are part of the story); Ruby's encode advantage sits at the run-to-run
  noise floor (1.18x host, 0.98x VM). The nightly job records `core` and `c` from
  this same image on real `ubuntu-latest` x86_64 (artifact
  `bench-baseline-x86_64`), and that is the Linux side to trust for a decision.

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

Decoding materializes a tree, and two structural costs sat on that path: a
decoded string or bytes item took two allocations (the value struct and its
payload), and every decoded item crossed a translation unit into `message.c`
(`pbw_value_new_uint_a`, `pbw_value_new_bytes_a`, `pbw_value_new_message_a`,
`pbw_value_list_push`, `pbw_message_new_a`) — `make` compiles each `src/*.c`
with no LTO, so those were real calls, one or two per decoded value. Three
changes removed them:

- an arena string/bytes value puts its payload in the same bump as the value
  struct (`pbw_value_new_bytes_a`); the heap path keeps its two allocations,
  which `pbw_value_free` releases separately;
- a packed varint run allocates all of its item nodes in one bump, using the
  item count the >64-byte pre-scan already computed; a packed closed-enum run
  keeps the per-item node path (no scan, no node block), because it drops
  unknown values and the block would hold nodes nothing uses. A/B against the
  scan+reserve form on a 100k-value packed closed enum (C probe,
  `pbw_decode_ctx` + reset, median of 15, three interleaved rounds) put every
  row inside the +/-3% noise floor (all-known 442 vs 442 ns, mixed-unknown 578
  vs 583 ns at 100/125 KB), so this one is allocation locality, not a
  throughput win, and the smaller diff was kept;
- the tree builders are `PBW_INLINE` in `internal.h` (the same pattern as
  `pbw_arena_alloc` and `pbw_find_field`). They stay one definition, moved and
  not copied, so a translation unit that never calls one emits nothing for it.

A/B measured on the settled tree with two binaries built from the same sources,
one with the change reverted, alternating in one session (C probe, best-of-5,
`bench.Msg` against the generated payload, `pbw_decode` + `pbw_message_free`,
median ns per decode; 60000 iterations at 132 B, 20000 at 169 B, 2000 at
2755 B, 400 at 85933 B):

| case | before | after | delta |
|---|---|---|---|
| 132 B | 101.1 | 100.1 | -1.0% (noise) |
| 169 B | 158.3 | 155.1 | -2.0% |
| 2755 B | 4076.6 | 3713.3 | -8.9% |
| 85933 B | 119472.8 | 107836.5 | -9.7% |

The win scales with the number of decoded values, which is where the two costs
were; a small message is dominated by the one tree allocation and the call
overhead into the API instead. Encode is untouched: `encode.o` built from the
reverted tree and from the changed tree is byte-identical, and the probe's
encode rows stayed inside the noise floor in every round. An LTO build limited to
`message.c`, `decode.c` and `wire.c` measured the same decode gain on the
same tier, which is what identified the cross-TU call cost; LTO as a default
build flag would change every binding's link step, so the helpers moved
instead.

Packed *fixed-width* runs still allocated one node per item: the list was
reserved (the payload length divides exactly by the item width) but the nodes
were not batched. `read_packed()` now takes the same one-bump node block for
fixed-width runs that the varint scan enables, which covers packed double /
fixed32 / sfixed / float arrays. A/B on a hand-built message with one packed
field (generic shape, not the tier message), alternating runs, best-of-5,
decode+free, median ns:

| case | before | after | delta |
|---|---|---|---|
| packed double x3000, 24 KB | 8666.2 | 6815.0 | -21.4% |
| packed fixed32 x3000, 12 KB | 7383.3 | 6163.9 | -16.5% |
| packed double x64, 515 B | 221.7 | 188.0 | -15.2% |
| packed double x8, 66 B | 53.2 | 51.3 | -3.6% |
| packed sint32 x3000 (control, same varint path) | 11974.9 | 12183.3 | noise |

Three candidates were measured and rejected in the same pass:

- **3-byte varint window in `read_varint()`** (the 1- and 2-byte windows are
  inline already): -4..5% on the 85933-byte tier, but +3..5% on 132 and 169 B,
  where the extra inline copy costs more in the per-field dispatch than the
  call it removes. Same conclusion as the Julia-cell note in
  [docs/perf-attempts.md](docs/perf-attempts.md); the small tiers are the
  common real case, so it stays out.
- **Presence-bitmap hoist in `decode_fields()`** (keep one word in a register
  and flush at the end): measured with a write-skipping ablation, which is
  upper-bound only - 132 B decode moved -3% plain / -0.6% ctx, i.e. at the
  noise floor, and the hoist would need every storing path to cooperate
  (`pbw_message_put`, `pbw_message_list_for`). Not built.
- **`pbw_ctx` retention for trees above the 1 MiB budget**: raising the cap to
  8 MiB on the 85933-byte tier changed nothing, so the arena's growth
  mallocs are not a cost worth touching; the documented knob is enough.

## Generated models (codegen)

The generated models are ergonomics, not a second hot path: all but Java
`encode`/`decode` through the same dynamic API the tables below measure; Java
POJOs write straight into a Wire buffer instead (reflection over the
annotated fields, no Message tree). The cost is the per-call metadata
lookup plus, in Python, the dataclass ⇄ dict conversion and, in Java, the
reflective field access.
Measured on this machine with `matrix3` x3000 (~33 KB) and identical values
through both paths:

| language | encode, generated / dynamic | decode, generated / dynamic |
|---|---|---|
| C (struct binding) | 1.00x (the struct path itself) | 1.00x |
| Java (reflective POJOs, direct Wire codec) | 1.7x slower | 1.0x |
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

![Core: pbw_ctx vs plain vs generated — MB/s by tier](docs/charts/bench-core-pbw-ctx-vs-plain-vs-generated.svg)

| payload | op | pbwire ctx (MB/s, x gen) | pbwire plain (MB/s, x gen) | protoc gen (MB/s) | DynamicMessage (MB/s) |
|---|---|---|---|---|---|
| 132 B | encode | 2142.0 (1.36x ✅) | 1824.8 (1.16x ✅) | 1577.5 | 110.8 |
| 132 B | decode | 1539.3 (1.66x ✅) | 1312.0 (1.41x ✅) | 928.4 | 294.8 |
| 169 B | encode | 1983.1 (1.19x ✅) | 1719.8 (1.03x ✅) | 1669.8 | 110.2 |
| 169 B | decode | 1231.7 (1.50x ✅) | 1085.4 (1.32x ✅) | 820.7 | 288.6 |
| 2755 B | encode | 1553.7 (1.33x ✅) | 1320.8 (1.13x ✅) | 1172.0 | 122.8 |
| 2755 B | decode | 680.8 (1.52x ✅) | 677.0 (1.51x ✅) | 447.1 | 252.5 |
| 85933 B | encode | 1674.9 (1.43x ✅) | 1531.7 (1.30x ✅) | 1174.5 | 146.4 |
| 85933 B | decode | 703.1 (1.61x ✅) | 711.4 (1.63x ✅) | 436.4 | 262.6 |

Threads (169 B, 1 vs 8):

| op | pbwire 1thr (MB/s, x gen) | pbwire 8thr agg (MB/s, x gen) | gen 1thr (MB/s) | gen 8thr agg (MB/s) |
|---|---|---|---|---|
| encode | 1921.4 (1.16x ✅) | 12641.1 (1.39x ✅) | 1658.5 | 9083.2 |
| decode | 1254.0 (1.59x ✅) | 7492.0 (1.22x ✅) | 790.9 | 6146.3 |

`8thr agg` is the total throughput of all eight threads. The schema, the
descriptors and the wire input are shared by all threads; each thread only adds
its own `pbw_ctx` and message. pbwire scales 6.6x (encode) and 6.0x (decode)
from 1 to 8 threads in that run; generated code reaches 5.5x and 7.8x. Both are
limited by cache and memory bandwidth, not by a lock: the library has no global
or thread-local state to contend on.

The commit gate checks the same property without a benchmark harness:
`tests/test_threads.c` prints 1-thread vs 8-thread MB/s for the context API (its
own, smaller message, one run on the same machine):

```
scale ctx decode 1 thread:   3788.3 MB/s (  51.2 ns/op) | 8 threads:   2555.9 MB/s/thread,  20447.3 MB/s aggregate (per-op 1.48x, aggregate 5.40x, 14 cores)
scale ctx encode 1 thread:   5850.4 MB/s (  33.2 ns/op) | 8 threads:   4811.9 MB/s/thread,  38495.0 MB/s aggregate (per-op 1.22x, aggregate 6.58x, 14 cores)
```

It fails when the aggregate throughput collapses below 0.35x of
`min(8, cores)`, which is what a hidden lock or false sharing would cause.

#### `pbwire ctx` vs `pbwire plain`

The two pbwire columns are the same encoder and decoder with a different memory
and ownership strategy; the benchmark checks that they produce identical bytes
before timing anything. The ownership table is in
[docs/design.md](docs/design.md#two-allocation-strategies); on the wire it shows
up as the `malloc`/`free` pair per call, which is why plain trails ctx by
about 0.1-0.2x on the small tiers (1.16x / 1.03x) and runs within a few percent
of it at 2755 B and up, while still beating generated code in every cell.

Read it as:

- **Against generated code**: the context API wins every binary cell — encode
  by 1.19x-1.43x, decode by 1.50x-1.66x. There is no tier where a compiled
  straight-line writer beats it outright, because the C++ serializer walks the
  message twice (size, then write) while pbwire writes one pass.
- **Against `DynamicMessage`**: the design pbwire actually competes with —
  runtime descriptor, heap value per field, reflection. pbwire is 2.7-5.2x
  faster there (decode) and 11.4-19.3x on encode.
- **The plain column** pays exactly one `malloc`/`free` pair per call (decoded
  tree, output buffer), which is why it sits ~0.1-0.2x behind ctx on the small
  tiers yet still beats generated code everywhere now (the 169 B encode moved
  from 0.94x to 1.03x). Use the plain API for one-off
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

![C structs vs protoc generated (same matrix3.proto) — MB/s by tier](docs/charts/bench-c-structs-vs-protoc-generated-same-matrix3-proto.svg)

| payload | op | pbwire-c (MB/s, x gen) | protoc gen (MB/s) |
|---|---|---|---|
| 136 B | encode | 1476.8 (1.04x ✅) | 1416.4 |
| 136 B | decode | 1090.8 (1.22x ✅) | 894.2 |
| 174 B | encode | 1441.1 (1.02x ✅) | 1407.5 |
| 174 B | decode | 1025.0 (1.28x ✅) | 799.8 |
| 2469 B | encode | 1064.7 (1.16x ✅) | 919.3 |
| 2469 B | decode | 812.4 (2.02x ✅) | 401.8 |
| 74913 B | encode | 1213.2 (1.30x ✅) | 931.8 |
| 74913 B | decode | 899.1 (2.31x ✅) | 389.2 |

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

![Go (cgo dynamic) — MB/s by tier](docs/charts/bench-go-cgo-dynamic.svg)

The static generator (`protoc-gen-pbwire-go`) is removed: Go keeps only the
`cgo` dynamic binding (`bindings/go/dynamic`), which loads a
`FileDescriptorSet` at run time and hot-swaps schemas without a restart.
`make go-bench` (`bindings/go/benchmarks`, build-tagged so `go test ./...`
stays fast) times it against `protobuf-go` generated code from the same
`matrix3.proto` on all four tiers; `bench.sh` generates the baseline with
`protoc-gen-go` and the `FileDescriptorSet` both sides load.

| payload | op | pbwire-go (MB/s, x protobuf-go) | protobuf-go (MB/s) |
|---|---|---|---|
| 136 B | encode | 684.1 (1.04x ✅) | 656.4 |
| 136 B | decode | 734.3 (1.23x ✅) | 599.0 |
| 174 B | encode | 637.3 (1.01x ✅) | 628.2 |
| 174 B | decode | 716.8 (1.44x ✅) | 496.3 |
| 2469 B | encode | 1132.2 (2.05x ✅) | 553.4 |
| 2469 B | decode | 663.9 (2.14x ✅) | 310.6 |
| 74913 B | encode | 1508.7 (2.68x ✅) | 563.1 |
| 74913 B | decode | 709.6 (2.23x ✅) | 318.2 |

Every tier is gated before timing: the official bytes must land on the
pinned size, `pbwire-go` must encode byte-identical bytes, decoding the
reference and re-encoding must return the same bytes, and the official
decode is spot-checked (`str`, inner `kid`/42, repeated count and last
`inners` value). The `cgo` boundary is one downcall per op with zero-copy
decode in, so the small tiers sit just above parity (1.04x / 1.23x) and the C
core's single pass pulls ahead from 2.5 KB up.

### Python vs `google.protobuf` (upb)

![Python vs google.protobuf (upb) — MB/s by tier](docs/charts/bench-python-vs-google-protobuf-upb.svg)

| payload | op | pbwire-py (MB/s, x upb) | upb (MB/s) |
|---|---|---|---|
| 136 B | encode(data) | 291.4 (5.66x ✅) | 51.5 |
| 136 B | decode(bytes) | 663.7 (1.17x ✅) | 568.6 |
| 136 B | to_dict(bytes) | 161.4 (4.85x ✅) | 33.3 |
| 136 B | encode(parsed) | 3087.4 (4.11x ✅) | 751.3 |
| 174 B | encode(data) | 334.7 (5.73x ✅) | 58.4 |
| 174 B | decode(bytes) | 727.6 (1.27x ✅) | 573.3 |
| 174 B | to_dict(bytes) | 154.5 (5.60x ✅) | 27.6 |
| 174 B | encode(parsed) | 3897.8 (4.75x ✅) | 819.8 |
| 2469 B | encode(data) | 497.8 (6.62x ✅) | 75.2 |
| 2469 B | decode(bytes) | 1127.3 (1.57x ✅) | 717.7 |
| 2469 B | to_dict(bytes) | 200.3 (4.84x ✅) | 41.4 |
| 2469 B | encode(parsed) | 31535.5 (22.0x ✅) | 1430.8 |
| 74913 B | encode(data) | 593.1 (6.91x ✅) | 85.8 |
| 74913 B | decode(bytes) | 1376.6 (1.70x ✅) | 810.5 |
| 74913 B | to_dict(bytes) | 173.2 (3.98x ✅) | 43.5 |
| 74913 B | encode(parsed) | 94176.6 (62.8x ✅) | 1500.0 |

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

![Java vs protobuf-java — MB/s by tier](docs/charts/bench-java-vs-protobuf-java.svg)

| payload | op | pbwire-java (MB/s, x protobuf-java) | protobuf-java (MB/s) |
|---|---|---|---|
| 136 B | encode | 1831.6 (0.82x ❌) | 2243.7 |
| 136 B | decode | 939.2 (1.48x ✅) | 633.9 |
| 174 B | encode | 1575.1 (1.25x ✅) | 1264.2 |
| 174 B | decode | 845.7 (1.54x ✅) | 550.5 |
| 2469 B | encode | 1023.6 (1.17x ✅) | 873.3 |
| 2469 B | decode | 892.9 (2.00x ✅) | 446.3 |
| 74913 B | encode | 1061.4 (1.17x ✅) | 904.7 |
| 74913 B | decode | 968.7 (2.36x ✅) | 411.0 |

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
as one straight-line block of known tags and values (~61 ns), while the dynamic
codec pays a presence gate, a type dispatch and tag loads per field (~74 ns
including the `byte[]` copy). That floor is structural, not tuning: the
per-field work left -- loads, one `switch`, one call -- is what a generated
writer avoids by knowing every field at compile time. From 174 B up the lazy
decode and the fused writes win every cell, and by 2.5 KB the size pass
`protobuf-java` runs before writing (3000 nested messages at the xlarge tier)
costs more than the codec's dispatch does.

#### Node vs `google-protobuf` (jspb) and protobufjs

![Node vs google-protobuf (jspb) and protobufjs — MB/s by tier](docs/charts/bench-node-vs-google-protobuf-jspb-and-protobufjs.svg)

| payload | op | pbwire-node (MB/s, x protobufjs) | protobufjs (MB/s) | jspb (MB/s) |
|---|---|---|---|---|
| 136 B | encode | 627.7 (2.56x ✅) | 245.5 | 58.7 |
| 136 B | decode | 419.3 (1.06x ✅) | 396.1 | 121.1 |
| 174 B | encode | 550.2 (1.84x ✅) | 298.8 | 62.4 |
| 174 B | decode | 489.3 (1.25x ✅) | 392.2 | 110.1 |
| 2469 B | encode | 1319.5 (3.21x ✅) | 410.5 | 57.8 |
| 2469 B | decode | 1161.8 (2.71x ✅) | 429.2 | 80.5 |
| 74913 B | encode | 1446.4 (2.89x ✅) | 499.8 | 56.0 |
| 74913 B | decode | 1584.5 (4.15x ✅) | 381.5 | 81.7 |

jspb's throughput does not move with payload size: its writer resolves a
code/type table per field and hands every repeated element to a closure, where
the C core writes one tag byte and one inline value, or memcpys an already
validated slice. protobufjs, the fastest widely used JS runtime, is 4-8x
faster than jspb on the same bytes (and the harness gates its bytes too):
the ratio column above is against it — 3.21x on large encode and 4.15x on
decode. The ~23x over jspb is the legacy-baseline gap; the ~3-4x is the codec
gap. Every tier wins now, including the 136 B decode (1.06x): the slice list
decode records lives on the stack up to 32 entries, so a small message scans
with no native allocation at all, which is what closed the last sub-parity
cell (one N-API downcall on a 136 B payload, where the boundary is the whole
budget).

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

![Ruby (C extension) vs the google-protobuf gem — MB/s by tier](docs/charts/bench-ruby-c-extension-vs-the-google-protobuf-gem.svg)

| payload | op | pbwire-ruby (MB/s, x gem) | gem (MB/s) |
|---|---|---|---|
| 136 B | encode | 1015.0 (1.37x ✅) | 741.1 |
| 136 B | decode | 462.9 (1.76x ✅) | 263.5 |
| 174 B | encode | 956.9 (1.19x ✅) | 803.2 |
| 174 B | decode | 514.3 (1.92x ✅) | 267.4 |
| 2469 B | encode | 1473.1 (1.17x ✅) | 1256.4 |
| 2469 B | decode | 970.6 (1.84x ✅) | 528.9 |
| 74913 B | encode | 1582.4 (1.21x ✅) | 1310.6 |
| 74913 B | decode | 1170.0 (1.68x ✅) | 694.7 |

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

All four tiers sit at or above parity (the closest row is the 2469 B encode at
1.17x); above it the C core's pack-aware writer pulls ahead.

#### Objective-C vs the `protobuf` ObjC runtime

![Objective-C vs the protobuf ObjC runtime — MB/s by tier](docs/charts/bench-objective-c-vs-the-protobuf-objc-runtime.svg)

| payload | op | pbwire-objc (MB/s, x protobuf-objc) | protobuf-objc (MB/s) |
|---|---|---|---|
| 136 B | encode | 1162.0 (6.9x ✅) | 168.8 |
| 136 B | decode | 766.0 (2.9x ✅) | 261.7 |
| 174 B | encode | 1509.9 (7.0x ✅) | 215.6 |
| 174 B | decode | 984.5 (3.2x ✅) | 304.3 |
| 2469 B | encode | 14200.6 (4.9x ✅) | 2908.1 |
| 2469 B | decode | 11862.8 (3.0x ✅) | 3893.8 |
| 74913 B | encode | 79927.5 (2.0x ✅) | 39423.8 |
| 74913 B | decode | 81115.5 (2.9x ✅) | 27736.1 |

The ARC wrapper adds one Objective-C object per call and no copy: `encode` hands
the encoder's own malloc'd buffer to `NSData` with
`dataWithBytesNoCopy:length:freeWhenDone:`, and `decode` wraps the decoder's
arena in one `PBWMessage`. That stays ahead of the generated runtime on all four
tiers and both ops (6.9x / 2.9x at 136 B down to 2.0x / 2.9x at 74913 B). The
xlarge tier is one 74 KB string, so it is a memcpy race: the generated encoder
writes it into an `NSMutableData` of the final size, and the wrapper writes it
into its own buffer, which `NSData` then takes over.

#### Kotlin vs `protoc --kotlin_out` over `protobuf-java`

![Kotlin vs protoc --kotlin_out over protobuf-java — MB/s by tier](docs/charts/bench-kotlin-vs-protoc-kotlin-out-over-protobuf-java.svg)

| payload | op | pbwire-kotlin (MB/s, x protobuf-kotlin) | protobuf-kotlin (MB/s) |
|---|---|---|---|
| 136 B | encode | 1849.3 (0.80x ❌) | 2311.1 |
| 136 B | decode | 978.2 (1.44x ✅) | 679.3 |
| 174 B | encode | 1552.8 (1.26x ✅) | 1232.6 |
| 174 B | decode | 832.1 (1.48x ✅) | 562.4 |
| 2469 B | encode | 967.3 (1.11x ✅) | 870.0 |
| 2469 B | decode | 862.1 (1.79x ✅) | 481.0 |
| 74913 B | encode | 1003.9 (1.09x ✅) | 919.4 |
| 74913 B | decode | 973.2 (2.07x ✅) | 470.6 |

Kotlin reuses the Java runtime (`bindings/java/runtime`), so these numbers are
`pbwire-java`'s: one workspace lookup plus one `byte[]` copy per call. The 136 B encode
is the case the Java section explains (a generated writer knows its tags, the
dynamic tree walk derives them), here 0.80x rather than 0.82x because the
Kotlin baseline reaches the same protobuf-java encoder through its builder/DSL
path. From 174 B up both ops are ahead at every tier.

#### PHP vs the official PHP runtime (pure + C extension)

![PHP vs the official PHP runtime (pure + C extension) — MB/s by tier](docs/charts/bench-php-vs-the-official-php-runtime-pure-c-extension.svg)

| payload | op | pbwire-php-c (MB/s, x protobuf-php-c) | protobuf-php-c (MB/s) |
|---|---|---|---|
| 136 B | encode | 1493.8 (1.73x ✅) | 864.1 |
| 136 B | decode | 860.6 (1.98x ✅) | 435.3 |
| 174 B | encode | 1349.9 (1.46x ✅) | 922.8 |
| 174 B | decode | 772.1 (1.83x ✅) | 422.9 |
| 2469 B | encode | 1406.4 (1.10x ✅) | 1280.4 |
| 2469 B | decode | 624.3 (1.24x ✅) | 503.9 |
| 74913 B | encode | 1378.4 (1.06x ✅) | 1295.8 |
| 74913 B | decode | 681.7 (1.21x ✅) | 561.4 |

The native extension links the C core in, like the Ruby C extension: built
messages live in a per-message arena (one bump allocator instead of a malloc
per value, sequential layout for the encoder), decoded trees keep the core's
single-malloc arena, and both encode straight into the result string. Against
the official C extension — the same official API backed by C, which `bench.sh`
times whenever it is installed — that wins every tier, 1.1-1.7x on encode
and 1.2-2.0x on decode. The `ext-ffi` wrapper runs the same core through one FFI trampoline per call:
it trails on small payloads (0.28x / 0.31x encode at 136-174 B) and converges
from 2.5 KB up (xlarge 0.90x / 1.30x vs the C extension in the same
harness). The pure-PHP runtime, timed by a second `php -n` run
in the same harness, is a different race:

| payload | op | pbwire-php (MB/s, x protobuf-php) | protobuf-php (MB/s) |
|---|---|---|---|
| 136 B | encode | 253.1 (65.0x ✅) | 3.9 |
| 136 B | decode | 233.2 (35.9x ✅) | 6.5 |
| 174 B | encode | 288.7 (76.0x ✅) | 3.8 |
| 174 B | decode | 265.3 (43.6x ✅) | 6.1 |
| 2469 B | encode | 1125.7 (313x ✅) | 3.6 |
| 2469 B | decode | 590.1 (114x ✅) | 5.2 |
| 74913 B | encode | 1297.9 (342x ✅) | 3.8 |
| 74913 B | decode | 712.8 (130x ✅) | 5.5 |

The pure-PHP runtime spends its time on per-field bookkeeping (~4-6 MB/s flat
across tiers), so quoting those ratios without the C-extension table is
quoting the interpreter, not the codec.

#### Dart vs the `protobuf` Dart package

![Dart vs the protobuf Dart package — MB/s by tier](docs/charts/bench-dart-vs-the-protobuf-dart-package.svg)

| payload | op | pbwire-dart (MB/s, x protobuf-dart) | protobuf-dart (MB/s) |
|---|---|---|---|
| 136 B | encode | 783.4 (3.6x ✅) | 217.5 |
| 136 B | decode | 401.6 (1.9x ✅) | 206.6 |
| 174 B | encode | 689.2 (3.8x ✅) | 179.2 |
| 174 B | decode | 411.7 (2.2x ✅) | 187.3 |
| 2469 B | encode | 1237.0 (11.5x ✅) | 107.3 |
| 2469 B | decode | 624.1 (4.4x ✅) | 141.3 |
| 74913 B | encode | 1430.7 (15.4x ✅) | 92.8 |
| 74913 B | decode | 694.0 (4.3x ✅) | 162.4 |

Both sides allocate per call. `dart:ffi` adds a `malloc`/copy for the input
bytes and a `Uint8List` copy for the output, which is why the gain grows with
payload size: the Dart runtime's pure-Dart field loop dominates it by 2.5 KB,
and at 136 B the boundary is still visible in a 1.9x decode.

#### Rust vs prost (and `rust-protobuf` 3.x)

![Rust vs prost (and rust-protobuf 3.x) — MB/s by tier](docs/charts/bench-rust-vs-prost-and-rust-protobuf-3-x.svg)

| payload | op | pbwire-rust (MB/s, x prost) | prost (MB/s) | protobuf-rust (MB/s) |
|---|---|---|---|---|
| 136 B | encode | 1962.9 (1.23x ✅) | 1601.8 | 1288.1 |
| 136 B | decode | 1169.9 (1.24x ✅) | 941.0 | 775.2 |
| 174 B | encode | 1870.2 (1.27x ✅) | 1470.4 | 1338.7 |
| 174 B | decode | 963.1 (1.47x ✅) | 655.5 | 570.7 |
| 2469 B | encode | 1532.9 (1.23x ✅) | 1248.4 | 838.7 |
| 2469 B | decode | 682.0 (1.87x ✅) | 365.5 | 370.2 |
| 74913 B | encode | 1398.4 (1.08x ✅) | 1296.6 | 849.5 |
| 74913 B | decode | 729.1 (1.87x ✅) | 390.3 | 406.1 |

The archive is linked in, so a call is a direct C call with no `dlopen` lookup.
prost, the fastest widely used Rust runtime (generated at build time by
build.rs, bytes gated like the rest), loses every cell now that encode reuses
one buffer per implementation (pbwire `encode_to()`, rust-protobuf
`write_to_vec()`, prost `encode()`): the one-shot `encode()` paid the core
malloc plus a 75 KB copy that neither baseline pays. pbwire-rust leads every
cell against both (xlarge encode 1.11x / 1.80x, decode 1.83x / 1.88x).

#### C# vs Google.Protobuf

![C# vs Google.Protobuf — MB/s by tier](docs/charts/bench-c-vs-google-protobuf.svg)

| payload | op | pbwire-csharp (MB/s, x protobuf-csharp) | protobuf-csharp (MB/s) | protobuf-net (MB/s) |
|---|---|---|---|---|
| 136 B | encode | 1470.5 (3.07x ✅) | 479.4 | 587.7 |
| 136 B | decode | 1164.5 (2.43x ✅) | 480.1 | 463.1 |
| 174 B | encode | 1377.2 (3.31x ✅) | 416.0 | — |
| 174 B | decode | 983.4 (2.46x ✅) | 399.1 | — |
| 2469 B | encode | 1326.1 (2.28x ✅) | 582.1 | 409.5 |
| 2469 B | decode | 660.2 (1.40x ✅) | 471.3 | 306.7 |
| 74913 B | encode | 1532.7 (2.91x ✅) | 526.7 | 414.2 |
| 74913 B | decode | 745.6 (1.42x ✅) | 526.6 | 344.9 |

Google.Protobuf's C# runtime is already a fast tree codec, so this is the
closest race of the five. Every cell is at or above parity -- the closest is
the 2469 B decode at 1.40x -- and the encode side
stays ~2.3-3.3x ahead at every tier. protobuf-net, the other widely used C#
runtime (hand-written contracts in Pbn.cs, bytes gated like the rest), is
slower than Google.Protobuf on every tier here (xlarge 414.2 vs 526.7
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

![Lua (C module, tables) vs lua-protobuf — MB/s by tier](docs/charts/bench-lua-c-module-tables-vs-lua-protobuf.svg)

| payload | op | pbwire-lua (MB/s, x lua-protobuf) | lua-protobuf (MB/s) |
|---|---|---|---|
| 136 B | encode | 151.4 (1.14x ✅) | 132.5 |
| 136 B | encode_msg | 1051.4 (7.93x ✅) | 132.5 |
| 136 B | decode | 157.1 (1.76x ✅) | 89.5 |
| 174 B | encode | 165.2 (1.15x ✅) | 144.1 |
| 174 B | encode_msg | 1087.1 (7.54x ✅) | 144.1 |
| 174 B | decode | 153.2 (1.70x ✅) | 90.2 |
| 2469 B | encode | 202.8 (1.13x ✅) | 179.2 |
| 2469 B | encode_msg | 1498.5 (8.36x ✅) | 179.2 |
| 2469 B | decode | 148.4 (1.59x ✅) | 93.6 |
| 74913 B | encode | 218.5 (1.10x ✅) | 198.0 |
| 74913 B | encode_msg | 1626.7 (8.21x ✅) | 198.0 |
| 74913 B | decode | 141.2 (1.44x ✅) | 97.9 |

`encode_msg` is the builder hot path (`schema:message()` once, then
`encode()` per send): one core walk plus one buffer malloc, no table
conversion, which is why it runs at the core's own ~1.0-1.5 GB/s. The table
path writes straight from the Lua table into the wire buffer (one pass over
schema fields in descriptor order, no intermediate tree, no per-value
allocation), so it beats the baseline at every tier. Decode wins at every tier because
it is one validating parse straight into tables with no tree kept. Both
directions walk one prepared per-descriptor cache, so a decoded table costs
no field-name interning, no descriptor lookup and no hash growth per field
(each table is created with its exact hash size, and an absent repeated
field's empty table is written in the same pass). The gate
is tier size (the bytes are canonical and match `protoc --encode` on the
small tier) plus a full-field expectation check; the baseline only has to
agree on size and decode, because `lua-protobuf` writes nested messages in
table hash order, so raw baseline bytes are not comparable.

### Erlang: `pbwire` vs gpb vs gpb NIF

![Erlang: pbwire vs gpb vs gpb NIF — MB/s by tier](docs/charts/bench-erlang-pbwire-vs-gpb-vs-gpb-nif.svg)

`matrix3.proto`, all four tiers, best of three samples per op. Same payload / op /
MB/s layout as every other table: the `pbwire-erlang` column carries the ratio
against the faster of gpb / gpb NIF per op (gpb is the byte reference either
way), the other columns are the two baselines side by side.

| payload | op | pbwire-erlang (MB/s, x best) | gpb (MB/s) | gpb-nif (MB/s) |
|---|---|---|---|---|
| 136 B | encode | 458.2 (2.28x ✅) | 200.8 | 48.7 |
| 136 B | decode | 524.1 (2.42x ✅) | 216.5 | 53.5 |
| 174 B | encode | 462.2 (3.72x ✅) | 124.2 | 68.9 |
| 174 B | decode | 475.6 (2.30x ✅) | 206.4 | 81.5 |
| 2469 B | encode | 411.9 (4.00x ✅) | 103.0 | 80.6 |
| 2469 B | decode | 310.5 (1.69x ✅) | 183.8 | 156.3 |
| 74913 B | encode | 428.9 (3.92x ✅) | 101.4 | 109.4 |
| 74913 B | decode | 322.1 (1.68x ✅) | 191.2 | 177.7 |

### Swift (SwiftPM, static link) vs `apple/swift-protobuf`

![Swift (SwiftPM, static link) vs apple/swift-protobuf — MB/s by tier](docs/charts/bench-swift-swiftpm-static-link-vs-apple-swift-protobuf.svg)

| payload | op | pbwire-swift (MB/s, x swift-protobuf) | swift-protobuf (MB/s) |
|---|---|---|---|
| 136 B | encode | 1148.0 (2.54x ✅) | 452.5 |
| 136 B | decode | 974.2 (3.88x ✅) | 251.1 |
| 174 B | encode | 1199.0 (3.64x ✅) | 329.0 |
| 174 B | decode | 873.0 (4.41x ✅) | 197.9 |
| 2469 B | encode | 1339.3 (5.00x ✅) | 268.1 |
| 2469 B | decode | 662.5 (3.29x ✅) | 201.3 |
| 74913 B | encode | 1543.2 (5.45x ✅) | 283.0 |
| 74913 B | decode | 623.9 (2.69x ✅) | 232.1 |

FFI host like Objective-C/Dart: `FileDescriptorSet` at run time, C ABI calls
only, no wire code, no plugin. SwiftPM links `build/libpbwire.a` via
`unsafeFlags` (so plain `swift test` works offline); the baseline is a
separate package using swift-protobuf's own build plugin, so no generated
code is checked in. The gate is byte parity per tier plus the pinned
`matrix3.bin` oracle.

### Scala (Java runtime + DSL) vs ScalaPB

![Scala (Java runtime + DSL) vs ScalaPB — MB/s by tier](docs/charts/bench-scala-java-runtime-dsl-vs-scalapb.svg)

| payload | op | pbwire-scala (MB/s, x ScalaPB) | ScalaPB (MB/s) |
|---|---|---|---|
| 136 B | encode | 1788.2 (1.62x ✅) | 1105.1 |
| 136 B | decode | 940.9 (1.15x ✅) | 817.3 |
| 174 B | encode | 1518.8 (1.48x ✅) | 1025.3 |
| 174 B | decode | 835.9 (1.07x ✅) | 783.8 |
| 2469 B | encode | 928.9 (1.21x ✅) | 769.4 |
| 2469 B | decode | 853.2 (1.30x ✅) | 656.0 |
| 74913 B | encode | 997.2 (1.20x ✅) | 828.1 |
| 74913 B | decode | 947.3 (1.46x ✅) | 649.6 |

Same class as Kotlin: the JVM binding reuses the Java runtime and adds only
an idiomatic DSL — no second codec, no JNI, schema from a `FileDescriptorSet`
at run time. The baseline is `scalapbc`-generated code (needs `cs install
scalapbc` once); the jars land in `benchmarks/deps`. Byte parity per tier
before timing, like every other row.

### Elixir (NIF over the shared core) vs `elixir-protobuf`

![Elixir (NIF over the shared core) vs elixir-protobuf — MB/s by tier](docs/charts/bench-elixir-nif-over-the-shared-core-vs-elixir-protobuf.svg)

| payload | op | pbwire-elixir (MB/s, x elixir-protobuf) | elixir-protobuf (MB/s) |
|---|---|---|---|
| 136 B | encode | 154.1 (3.26x ✅) | 47.3 |
| 136 B | decode | 84.8 (1.43x ✅) | 59.5 |
| 174 B | encode | 157.3 (3.05x ✅) | 51.6 |
| 174 B | decode | 102.0 (1.61x ✅) | 63.4 |
| 2469 B | encode | 166.8 (2.77x ✅) | 60.2 |
| 2469 B | decode | 99.0 (1.71x ✅) | 57.8 |
| 74913 B | encode | 145.6 (2.30x ✅) | 63.2 |
| 74913 B | decode | 84.7 (1.40x ✅) | 60.6 |

Same class as Erlang: the BEAM binding reuses the published `:pbwire` Hex
NIF (the same C core as `bindings/erlang`) and owns only the struct↔tuple
translation; `elixir-protobuf` is the schema plugin, like gpb is for Erlang.
Every encode is byte-identical to `Protobuf.encode/1` before timing. Known
deliberate divergences from the oracle: NaN bit identity, unknown-field drop
(the NIF tuple form has no storage), raise on missing `required`.

### Zig (static link) vs Arwalk/zig-protobuf

![Zig (static link) vs Arwalk/zig-protobuf — MB/s by tier](docs/charts/bench-zig-static-link-vs-arwalk-zig-protobuf.svg)

| payload | op | pbwire-zig (MB/s, x zig-protobuf) | zig-protobuf (MB/s) |
|---|---|---|---|
| 136 B | encode | 2045.8 (1.63x ✅) | 1254.1 |
| 136 B | decode | 1197.5 (45.2x ✅) | 26.5 |
| 174 B | encode | 1947.6 (1.82x ✅) | 1070.9 |
| 174 B | decode | 1046.6 (44.0x ✅) | 23.8 |
| 2469 B | encode | 1639.5 (1.86x ✅) | 880.1 |
| 2469 B | decode | 684.1 (9.4x ✅) | 72.9 |
| 74913 B | encode | 1548.2 (1.87x ✅) | 826.1 |
| 74913 B | decode | 736.8 (2.27x ✅) | 324.6 |

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

![F# (facade over the C# runtime) vs Google.Protobuf — MB/s by tier](docs/charts/bench-f-facade-over-the-c-runtime-vs-google-protobuf.svg)

| payload | op | pbwire-fsharp (MB/s, x Google.Protobuf) | Google.Protobuf (MB/s) |
|---|---|---|---|
| 136 B | encode | 1382.3 (3.02x ✅) | 458.1 |
| 136 B | decode | 1145.8 (2.32x ✅) | 494.4 |
| 174 B | encode | 1379.4 (3.35x ✅) | 412.3 |
| 174 B | decode | 976.0 (2.46x ✅) | 396.9 |
| 2469 B | encode | 1331.3 (2.38x ✅) | 559.5 |
| 2469 B | decode | 669.4 (1.44x ✅) | 466.4 |
| 74913 B | encode | 1441.6 (2.93x ✅) | 492.2 |
| 74913 B | decode | 728.3 (1.43x ✅) | 510.7 |

Same class as Kotlin: F# reuses the C# binding's project via
`ProjectReference` and adds only an F#-idiomatic facade — no second runtime,
no packages, schema from a `FileDescriptorSet` at run time. The baseline is
`protoc`-generated C# compiled in a sibling project (F# cannot compile
`.cs`). Byte parity per tier before timing.

### Clojure (Java runtime + interop) vs `protobuf-java`

![Clojure (Java runtime + interop) vs protobuf-java — MB/s by tier](docs/charts/bench-clojure-java-runtime-interop-vs-protobuf-java.svg)

| payload | op | pbwire-clj (MB/s, x protobuf-java) | protobuf-java (MB/s) |
|---|---|---|---|
| 136 B | encode | 1742.7 (9.9x ✅) | 175.6 |
| 136 B | decode | 915.3 (1.36x ✅) | 673.9 |
| 174 B | encode | 1514.0 (7.1x ✅) | 211.8 |
| 174 B | decode | 807.5 (1.37x ✅) | 591.4 |
| 2469 B | encode | 944.0 (1.33x ✅) | 707.2 |
| 2469 B | decode | 869.6 (1.88x ✅) | 463.1 |
| 74913 B | encode | 947.3 (1.08x ✅) | 876.7 |
| 74913 B | decode | 951.0 (2.16x ✅) | 440.7 |

The timed path reuses a pre-built mutable message (fill once, encode many;
decode plus close, no map materialization), like the Java row, so it beats
generated straight-line code. The persistent-map fns stay for convenience
and are gated for parity, but hot loops use the mutable path — the same
split Lua documents (table vs builder). Same codec as Java/Scala underneath
(plain interop over the Java runtime, no second wire implementation), byte
parity per tier.

### Julia (`dlopen` + `ccall`) vs ProtoBuf.jl

![Julia (dlopen + ccall) vs ProtoBuf.jl — MB/s by tier](docs/charts/bench-julia-dlopen-ccall-vs-protobuf-jl.svg)

| payload | op | pbwire-julia (MB/s, x ProtoBuf.jl) | ProtoBuf.jl (MB/s) |
|---|---|---|---|
| 136 B | encode | 1098.3 (2.8x ✅) | 393.4 |
| 136 B | decode | 1085.7 (0.92x ❌) | 1185.5 |
| 174 B | encode | 1135.0 (3.3x ✅) | 343.2 |
| 174 B | decode | 953.9 (1.24x ✅) | 772.3 |
| 2469 B | encode | 1269.7 (9.7x ✅) | 131.1 |
| 2469 B | decode | 677.9 (1.04x ✅) | 651.0 |
| 74913 B | encode | 1412.7 (123.9x ✅) | 11.4 |
| 74913 B | decode | 727.3 (1.01x ✅) | 720.8 |

FFI host like Dart/C#: `Libdl.dlopen` + `ccall` into the C ABI (function
pointers resolved once via `dlsym`, since recent Julia needs a compile-time
library name), `Dict`-based messages convert through the C ABI, no wire
code, no `.proto` parser. The baseline is `protojl`-generated code
(`benchmarks/oracle/matrix3_pb.jl`, produced by `protojl("matrix3.proto",
"<proto dir>", "benchmarks/oracle")`). Its encode collapses with size
(pure-Julia per-field appends). Decode sits at parity: both sides land near
the C core's own ~700 MB/s, so the medium and large tiers lead while the small
tier trails on fixed FFI overhead (like Java/Kotlin small-tier floors); the
large and xlarge tiers land within run noise either side of 1.0x, and the
headline remains the xlarge encode column.

### R (C extension, named lists) vs RProtoBuf

![R (C extension, named lists) vs RProtoBuf — MB/s by tier](docs/charts/bench-r-c-extension-named-lists-vs-rprotobuf.svg)

| payload | op | pbwire-r (MB/s, x RProtoBuf) | RProtoBuf (MB/s) |
|---|---|---|---|
| 136 B | encode | 99.6 (36.9x ✅) | 2.7 |
| 136 B | decode | 74.7 (26.7x ✅) | 2.8 |
| 174 B | encode | 114.1 (35.7x ✅) | 3.2 |
| 174 B | decode | 82.7 (26.7x ✅) | 3.1 |
| 2469 B | encode | 160.8 (55.4x ✅) | 2.9 |
| 2469 B | decode | 123.8 (38.8x ✅) | 3.2 |
| 74913 B | encode | 179.0 (59.7x ✅) | 3.0 |
| 74913 B | decode | 135.8 (45.3x ✅) | 3.0 |

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
once, `encode` per send, 1479.8 MB/s on the xlarge tier) runs at the core's
speed; the table above times the one-shot path both sides. Encode is no
contest at any tier: setting 3000 nested messages field by field through S4
dispatch costs RProtoBuf ~27 ms where the C core walks the list once. Decode
is compared like for like: `RProtoBuf::read` parses in C++ and materializes
fields lazily on `$`, while `pbwire_decode` builds the full named-list tree
eagerly, so the baseline side reads every field into the same plain-list shape
(the table's `decode` row). A bare `read()` alone is a few times faster than
`pbwire_decode`, but it hands back a Message, not R data; once both sides
produce plain lists, pbwire leads 27-60x. Two R spellings to know: `INT32_MIN`
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
make java-bench   # pure-Java codec vs protobuf-java (fetches the jar from Maven Central once)
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
make bench-docker                 # the same harnesses in a Linux container (docker/bench.Dockerfile)
make bench-compare                # newest record per binding/platform in results/, paired
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
each — then re-render the per-language charts (`python3 scripts/bench-charts.py`
parses every per-language tier table and re-embeds its SVG, so no chart can
drift from its table; rows are grouped by op, tiers ascending within each op). Then sync the summary table in [README.md](README.md#performance): for
each binding take the best encode ratio and the best decode ratio across its
four tiers, with the tier in parentheses (`small`/`medium`/`large`/`xlarge`),
and keep the machine note (CPU, `protoc` version) next to it. Paste all runs from the same machine and note the date, CPU, `protoc`
version and iteration counts above.

The [Linux (docker)](#linux-docker-the-same-harnesses-one-container-away)
section is `make bench-compare` output (stdout only, so it pastes clean):
re-run `make bench` on the host and `make bench-docker` on the same tree, then
paste the xlarge tier table.

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

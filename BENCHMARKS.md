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
- [Embedded vs nanopb](#embedded-nanopb-same-message-both-sides) — the microcontroller tier, footprint and throughput
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

> **Note (2026-11-14):** these tables predate the proto3 `string` UTF-8
> validation that now runs at parse time (see
> [perf-attempts.md](docs/perf-attempts.md#aligned-cost-proto3-string-utf-8-validation)).
> Decode rows are therefore up to 14% optimistic on payloads with many proto3
> strings (the `matrix3` payload carries 3000 of them), and the C/Java/Python
> figures move most. A re-measure is pending a quiet host; the alignment itself
> is proved by the byte-equality, differential and conformance gates, not by
> these numbers.

Last full refresh: 2026-11-13. Every binding's four tiers were re-run on the
same host that day (`make bench`, `make bench-c`, `make python-bench`, ...) and
pasted from the harness output; each row is the best of the day's runs for
that implementation (the harness reports best-of-3 passes per run). One run
per binding is pasted verbatim, so both columns of a row come from the same
measurement: the newest run whose own baseline did not collapse under host load
(a run whose fastest baseline is under 85% of that binding's median baseline is
treated as contaminated and skipped). The
host carried some background load, so absolute MB/s reads 10-15% low against a
quiet machine while `x baseline` reproduces. Dart is the
one exception: `protoc-gen-dart` is missing on this host, so its rows are the
2026-11-12 run. The Java-family rows come from that same run.

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
| core (dynamic) | `make bench` (`bench/bench.cc`) | `protoc` C++ generated + DynamicMessage | all 4 | 1.33x ✅ / 1.82x ✅ |
| C structs | `make bench-c` (`bindings/c/benchmarks/bench_struct.cc`) | `protoc` C++ generated from the same `matrix3.proto` | all 4 | 1.25x ✅ / 2.22x ✅ |
| Erlang/OTP NIF | `make benchmarks` in `bindings/erlang/benchmarks` | gpb (pure Erlang), with gpb NIF (libprotobuf) as the second baseline, same `matrix3.proto` | all 4 | 3.89x / 1.70x vs best-of-two ✅ |
| Python (CPython ext) | `make python-bench` (`bindings/python/benchmarks/bench.py`) | `google.protobuf` (upb C extension) on the same `matrix3.proto` | all 4 | 6.98x ✅ / 1.38x ✅ |
| C++ | same engine as C (`bindings/cpp` mirrors the C structs; `make bench-c` is the harness) | `protoc` C++ generated from the same `matrix3.proto` | all 4 | 1.25x ✅ / 2.22x ✅ |
| Go (`cgo` dynamic) | `make go-bench` (`bindings/go/benchmarks`) | `protobuf-go` generated code from the same `matrix3.proto` | all 4 | 2.29x ✅ / 2.54x ✅ |
| Java (pure-Java codec) | `make java-bench` (`bindings/java/benchmarks`) | `protobuf-java` generated code from the same `matrix3.proto` | all 4 | 1.01x ✅ / 2.08x ✅ (0.76x at 136 B) |
| Node (N-API addon) | `make node-bench` (`bindings/node/benchmarks`) | [protobufjs](https://www.npmjs.com/package/protobufjs) (fastest) + `google-protobuf` (jspb) from the same `matrix3.proto` | all 4 | 3.09x ✅ / 3.91x ✅ |
| Ruby (C extension) | `make ruby-bench` (`bindings/ruby/benchmarks`) | the `google-protobuf` gem (C extension) | all 4 | 1.21x ✅ / 1.67x ✅ |
| Objective-C (ARC) | `make objc-bench` (`bindings/objc/benchmarks`) | the `protobuf` ObjC runtime + generated code from the same `matrix3.proto` | all 4 | 2.03x ✅ / 2.82x ✅ |
| Kotlin (pure-Java codec + DSL) | `make kotlin-bench` (`bindings/kotlin/benchmarks`) | `protoc --kotlin_out` over `protobuf-java`, same `matrix3.proto` | all 4 | 1.14x ✅ / 1.78x ✅ (0.81x at 136 B, 0.79x at 174 B) |
| PHP (`ext-ffi` + C extension) | `make php-bench` (`bindings/php/benchmarks`) | the official PHP runtime (`protobuf/php/src`): C extension (fastest) + pure-PHP | all 4 | 1.10x / 1.41x vs C ext ✅ (368x / 154x vs pure) |
| Dart (`dart:ffi`) | `make dart-bench` (`bindings/dart/benchmarks`) | the `protobuf` Dart package + generated code | all 4 | 15.42x ✅ / 4.27x ✅ |
| Rust (static link) | `make rust-bench` (`bindings/rust/benchmarks`) | prost (fastest) + rust-protobuf 3.x generated code | all 4 | 1.01x ✅ / 2.16x ✅ |
| C# (P/Invoke) | `make csharp-bench` (`bindings/csharp/benchmarks`) | Google.Protobuf C# generated code (fastest) + protobuf-net | all 4 | 2.81x ✅ / 1.68x ✅ |
| Lua (C module, tables) | `make lua-bench` (`bindings/lua/benchmarks`) | `lua-protobuf` 0.6.0 | all 4 | 1.12x ✅ / 1.47x ✅ (8.14x via builder encode) |
| Swift (SwiftPM, static link) | `make swift-bench` (`bindings/swift/benchmarks`) | `apple/swift-protobuf` 1.39.0 generated code from the same `matrix3.proto` | all 4 | 5.32x ✅ / 2.88x ✅ |
| Scala (Java runtime + DSL) | `make scala-bench` (`bindings/scala/benchmarks`) | ScalaPB 0.11.18 generated code from the same `matrix3.proto` | all 4 | 1.20x ✅ / 1.38x ✅ |
| Elixir (NIF over the shared core) | `make elixir-bench` (`bindings/elixir/benchmarks`) | `elixir-protobuf` 0.17.1 structs from the same `matrix3.proto` | all 4 | 2.25x ✅ / 1.38x ✅ |
| Zig (static link) | `make zig-bench` (`bindings/zig/benchmarks`, `-Doptimize=ReleaseFast`) | Arwalk/zig-protobuf 7.0.0 generated code from the same `matrix3.proto` | all 4 | 1.71x ✅ / 2.71x ✅ |
| F# (facade over the C# runtime) | `make fsharp-bench` (`bindings/fsharp/benchmarks`) | Google.Protobuf C# generated code from the same `matrix3.proto` | all 4 | 3.13x ✅ / 1.65x ✅ |
| Clojure (Java runtime + interop) | `make clojure-bench` (`bindings/clojure/benchmarks`) | `protobuf-java` generated code from the same `matrix3.proto` | all 4 | 1.17x ✅ / 1.80x ✅ (0.73x at 136 B) |
| Julia (`ccall` FFI) | `make julia-bench` (`bindings/julia/benchmarks`) | JuliaIO/ProtoBuf.jl 1.3.0 generated code from the same `matrix3.proto` | all 4 | 123.40x ✅ / 1.13x ✅ |
| R (C extension, named lists) | `make r-bench` (`bindings/r/benchmarks`) | RProtoBuf 0.4.28 on the same `matrix3.proto` | all 4 | 65.04x ✅ / 64.75x ✅ |

"vs baseline" is encode / decode throughput relative to the best baseline on
the largest case the binding runs. Where a binding has several baselines the
headline names the fastest: protobufjs over jspb, the PHP C extension over
pure PHP, prost over rust-protobuf, Google.Protobuf over protobuf-net, the
faster of gpb / gpb NIF per op. `encode` is host data to wire bytes and
`decode` is wire bytes to an in-memory message on both sides (R materializes
plain lists on both sides: RProtoBuf's bare `read()` is lazily parsed and
faster, but hands back no R data — see its section). Every binding with a
wired baseline is ahead of it on the xlarge encode column now; bytes are
identical everywhere. The sub-1.0x cells are small-tier boundary floors
(downcall, JIT warm-up or GC cost on 132-174 B payloads: Java/Clojure
small-tier encode at 0.76x/0.73x, Kotlin small at 0.81x and medium at 0.79x
(GC-sensitive, see its section), core plain 169 B encode at 0.95x, Go
small-tier encode at 0.98x, Ruby 174 B encode at 0.96x, Node 136 B decode
at 0.96x, Julia small decode at 0.89x) — boundary cost on a tiny payload,
not a byte gap, and every xlarge encode headline still wins.

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
| core dynamic (`pbw_ctx`) | direct call | 85933 B | 1497.1 (1.33x ✅) | 812.7 (1.82x ✅) |
| core dynamic (plain API) | direct call | 85933 B | 1359.3 (1.21x ✅) | 802.4 (1.80x ✅) |
| C structs | direct call, layout tables | 74913 B | 1139.5 (1.25x ✅) | 820.5 (2.22x ✅) |
| Go | `cgo` dynamic over the C core | 74913 B | 1298.3 (2.29x ✅) | 801.2 (2.54x ✅) |
| Python (dict path) | CPython extension | 74913 B | 590.9 (6.98x ✅) | 1113.1 (1.38x ✅) |
| Java | pure-Java codec, no native code | 74913 B | 989.2 (1.01x ✅) | 824.4 (2.08x ✅) |
| Kotlin | the pure-Java codec + DSL | 74913 B | 961.7 (1.14x ✅) | 794.0 (1.78x ✅) |
| Node | N-API addon | 74913 B | 1524.6 (3.09x ✅) | 1513.5 (3.91x ✅) |
| Ruby | C extension over the core | 74913 B | 1600.5 (1.21x ✅) | 1147.7 (1.67x ✅) |
| Objective-C | ARC wrapper | 74913 B | 78115.4 (2.03x ✅) | 75447.9 (2.82x ✅) |
| PHP | C extension | 74913 B | 1364.5 (1.10x ✅) | 774.5 (1.41x ✅) |
| Rust | static link | 74913 B | 1381.0 (1.01x ✅) | 799.1 (2.16x ✅) |
| C# | P/Invoke | 74913 B | 1361.5 (2.81x ✅) | 785.5 (1.68x ✅) |
| Lua | C module, tables | 74913 B | 206.6 (1.12x ✅) | 130.1 (1.47x ✅) |
| Erlang | NIF (`pbwire`) | 74913 B | 445.5 (3.89x ✅) | 331.6 (1.70x ✅) |
| Swift | static link (SwiftPM) | 74913 B | 1408.5 (5.32x ✅) | 595.0 (2.88x ✅) |
| Scala | the Java runtime + DSL | 74913 B | 914.2 (1.20x ✅) | 755.6 (1.38x ✅) |
| Elixir | NIF (Hex `:pbwire`, same core as Erlang) | 74913 B | 129.6 (2.25x ✅) | 74.8 (1.38x ✅) |
| Zig | static link | 74913 B | 1416.2 (1.71x ✅) | 806.1 (2.71x ✅) |
| F# | P/Invoke over the C# runtime | 74913 B | 1386.4 (3.13x ✅) | 786.9 (1.65x ✅) |
| Clojure | the Java runtime + interop | 74913 B | 996.7 (1.17x ✅) | 784.8 (1.80x ✅) |
| Julia | `dlopen` + `ccall` | 74913 B | 1394.4 (123.40x ✅) | 768.0 (1.13x ✅) |
| R | C extension, named lists | 74913 B | 169.1 (65.04x ✅) | 155.4 (64.75x ✅) |

Read it as: the in-process dynamic core sets the pace (1.33x the C++
generated code at this size), a binding that links the core in (Ruby, Rust)
keeps within a few percent of it, and an FFI binding pays its boundary only
until the payload is large — Java, Kotlin, Ruby, Rust, C# and PHP all land at
~0.96-1.6 GB/s encode and ~775-1150 MB/s decode on the xlarge tier, which is
near the core's own number. The outliers are baseline or unit effects, not pbwire
effects: Python's encode column is the dict path (3000 sub-messages, 12000
ints), and the Objective-C tier is a memcpy race over one 74 KB string.
The headline always names the fastest baseline: Node's 3.09x / 3.91x is over
protobufjs (jspb, the legacy baseline, trails it by ~28x / ~19x), PHP's 1.10x /
1.41x is over the official C extension (the pure-PHP runtime trails it by
~368x / ~154x) — see their sections. Swift, Scala, F#, Zig and Julia join
the ~1.0-1.6 GB/s encode club on xlarge (Swift 5.32x and Julia 123x encode
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
| 132 B | encode | 1993.3 (1.24x ✅) | 1627.8 (1.01x ✅) | 1606.2 | 110.0 |
| 132 B | decode | 1507.2 (1.64x ✅) | 1291.8 (1.41x ✅) | 918.3 | 293.6 |
| 169 B | encode | 1812.0 (1.10x ✅) | 1560.8 (0.95x ❌) | 1642.0 | 108.5 |
| 169 B | decode | 1208.8 (1.51x ✅) | 1075.3 (1.34x ✅) | 803.0 | 280.2 |
| 2755 B | encode | 1470.2 (1.27x ✅) | 1251.5 (1.08x ✅) | 1157.1 | 130.2 |
| 2755 B | decode | 759.4 (1.66x ✅) | 738.6 (1.62x ✅) | 457.2 | 250.3 |
| 85933 B | encode | 1497.1 (1.33x ✅) | 1359.3 (1.21x ✅) | 1125.4 | 146.4 |
| 85933 B | decode | 812.7 (1.82x ✅) | 802.4 (1.80x ✅) | 445.7 | 258.0 |

Threads (169 B, 1 vs 8):

| op | pbwire 1thr (MB/s, x gen) | pbwire 8thr agg (MB/s, x gen) | gen 1thr (MB/s) | gen 8thr agg (MB/s) |
|---|---|---|---|---|
| encode | 1708.9 (1.06x ✅) | 11500.6 (1.37x ✅) | 1615.4 | 8403.4 |
| decode | 1216.7 (1.52x ✅) | 7478.1 (1.24x ✅) | 798.1 | 6047.8 |

`8thr agg` is the total throughput of all eight threads. The schema, the
descriptors and the wire input are shared by all threads; each thread only adds
its own `pbw_ctx` and message. pbwire scales 6.7x (encode) and 6.2x (decode)
from 1 to 8 threads in that run; generated code reaches 5.2x and 7.6x. Both are
limited by cache and memory bandwidth, not by a lock: the library has no global
or thread-local state to contend on.

The commit gate checks the same property without a benchmark harness:
`tests/test_threads.c` prints 1-thread vs 8-thread MB/s for the context API (its
own, smaller message, one run on the same machine):

```
scale ctx decode 1 thread:   3430.3 MB/s (  56.6 ns/op) | 8 threads:   2636.3 MB/s/thread,  21090.2 MB/s aggregate (per-op 1.30x, aggregate 6.15x, 14 cores)
scale ctx encode 1 thread:   5638.7 MB/s (  34.4 ns/op) | 8 threads:   4055.9 MB/s/thread,  32447.4 MB/s aggregate (per-op 1.39x, aggregate 5.75x, 14 cores)
```

It fails when the aggregate throughput collapses below 0.35x of
`min(8, cores)`, which is what a hidden lock or false sharing would cause.

#### `pbwire ctx` vs `pbwire plain`

The two pbwire columns are the same encoder and decoder with a different memory
and ownership strategy; the benchmark checks that they produce identical bytes
before timing anything. The ownership table is in
[docs/design.md](docs/design.md#two-allocation-strategies); on the wire it shows
up as the `malloc`/`free` pair per call, which is why plain trails ctx by
about 0.2x on the small tiers (1.24x / 1.01x) and runs within a few percent
of it at 2755 B and up, beating generated code everywhere except the plain
169 B encode (0.95x).

Read it as:

- **Against generated code**: the context API wins every binary cell — encode
  by 1.10x-1.33x, decode by 1.51x-1.82x. There is no tier where a compiled
  straight-line writer beats it outright, because the C++ serializer walks the
  message twice (size, then write) while pbwire writes one pass.
- **Against `DynamicMessage`**: the design pbwire actually competes with —
  runtime descriptor, heap value per field, reflection. pbwire is 2.9-4.9x
  faster there (decode) and 9.7-18.0x on encode.
- **The plain column** pays exactly one `malloc`/`free` pair per call (decoded
  tree, output buffer), which is why it sits ~0.2x behind ctx on the small
  tiers and trails generated code on the 169 B encode (0.95x, the malloc
  against 169 wire bytes). Use the plain API for one-off
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
| 136 B | encode | 1551.5 (1.18x ✅) | 1317.4 |
| 136 B | decode | 1109.1 (1.52x ✅) | 731.7 |
| 174 B | encode | 1470.2 (1.30x ✅) | 1134.1 |
| 174 B | decode | 839.5 (1.48x ✅) | 567.5 |
| 2469 B | encode | 883.5 (1.39x ✅) | 635.0 |
| 2469 B | decode | 730.9 (1.90x ✅) | 383.7 |
| 74913 B | encode | 1139.5 (1.25x ✅) | 908.0 |
| 74913 B | decode | 820.5 (2.22x ✅) | 369.1 |

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
the layout per call and mallocs per value (6.3x / 7.1x slower at 136 B, 4.6x /
3.8x at 174 B, and down to 1.0-1.8x from 2.5 KB up, where the per-call layout
compile is amortised). It stays in the
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
| 136 B | encode | 605.6 (0.98x ❌) | 619.0 |
| 136 B | decode | 686.9 (1.23x ✅) | 556.7 |
| 174 B | encode | 595.4 (0.98x ❌) | 606.8 |
| 174 B | decode | 683.6 (1.49x ✅) | 460.1 |
| 2469 B | encode | 1107.4 (2.05x ✅) | 539.4 |
| 2469 B | decode | 724.0 (2.42x ✅) | 299.4 |
| 74913 B | encode | 1298.3 (2.29x ✅) | 567.1 |
| 74913 B | decode | 801.2 (2.54x ✅) | 315.8 |

Every tier is gated before timing: the official bytes must land on the
pinned size, `pbwire-go` must encode byte-identical bytes, decoding the
reference and re-encoding must return the same bytes, and the official
decode is spot-checked (`str`, inner `kid`/42, repeated count and last
`inners` value). The `cgo` boundary is one downcall per op with zero-copy
decode in, so the small encode tier sits just under parity (0.98x) while
small decode stays ahead (1.23x), and the C core's single pass pulls
ahead from 2.5 KB up.

### Python vs `google.protobuf` (upb)

![Python vs google.protobuf (upb) — MB/s by tier](docs/charts/bench-python-vs-google-protobuf-upb.svg)

| payload | op | pbwire-py (MB/s, x upb) | upb (MB/s) |
|---|---|---|---|
| 136 B | encode(data) | 290.0 (6.13x ✅) | 47.3 |
| 136 B | decode(bytes) | 551.7 (1.03x ✅) | 534.1 |
| 136 B | to_dict(bytes) | 143.5 (4.54x ✅) | 31.6 |
| 136 B | encode(parsed) | 2711.0 (3.66x ✅) | 741.5 |
| 174 B | encode(data) | 335.6 (5.69x ✅) | 59.0 |
| 174 B | decode(bytes) | 643.2 (1.14x ✅) | 563.0 |
| 174 B | to_dict(bytes) | 147.7 (5.31x ✅) | 27.8 |
| 174 B | encode(parsed) | 3569.7 (4.31x ✅) | 828.3 |
| 2469 B | encode(data) | 512.5 (6.60x ✅) | 77.6 |
| 2469 B | decode(bytes) | 947.4 (1.31x ✅) | 721.1 |
| 2469 B | to_dict(bytes) | 189.9 (4.59x ✅) | 41.4 |
| 2469 B | encode(parsed) | 30875.9 (21.50x ✅) | 1435.9 |
| 74913 B | encode(data) | 590.9 (6.98x ✅) | 84.7 |
| 74913 B | decode(bytes) | 1113.1 (1.38x ✅) | 804.8 |
| 74913 B | to_dict(bytes) | 163.2 (3.79x ✅) | 43.1 |
| 74913 B | encode(parsed) | 95491.1 (63.64x ✅) | 1500.6 |

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
| 136 B | encode | 1751.8 (0.76x ❌) | 2304.9 |
| 136 B | decode | 949.4 (1.51x ✅) | 628.6 |
| 174 B | encode | 1518.9 (1.20x ✅) | 1266.1 |
| 174 B | decode | 794.6 (1.40x ✅) | 568.4 |
| 2469 B | encode | 937.2 (1.00x ✅) | 933.4 |
| 2469 B | decode | 756.6 (1.77x ✅) | 427.9 |
| 74913 B | encode | 989.2 (1.01x ✅) | 976.1 |
| 74913 B | decode | 824.4 (2.08x ✅) | 396.0 |

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
as one straight-line block of known tags and values (~58 ns), while the dynamic
codec pays a presence gate, a type dispatch and tag loads per field (~78 ns
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
| 136 B | encode | 551.1 (2.19x ✅) | 251.1 | 59.3 |
| 136 B | decode | 392.0 (0.96x ❌) | 407.1 | 120.2 |
| 174 B | encode | 529.0 (1.76x ✅) | 299.9 | 60.4 |
| 174 B | decode | 449.3 (1.16x ✅) | 388.3 | 110.9 |
| 2469 B | encode | 1330.1 (3.04x ✅) | 437.9 | 54.1 |
| 2469 B | decode | 1146.1 (2.69x ✅) | 426.2 | 78.4 |
| 74913 B | encode | 1524.6 (3.09x ✅) | 493.3 | 55.2 |
| 74913 B | decode | 1513.5 (3.91x ✅) | 386.8 | 79.5 |

jspb's throughput does not move with payload size: its writer resolves a
code/type table per field and hands every repeated element to a closure, where
the C core writes one tag byte and one inline value, or memcpys an already
validated slice. protobufjs, the fastest widely used JS runtime, is 4-8x
faster than jspb on the same bytes (and the harness gates its bytes too):
the ratio column above is against it — 3.04x on large encode and 2.69x on
decode. The ~28x over jspb is the legacy-baseline gap; the ~3-4x is the codec
gap. Small decode sits at the parity line (0.96x this run, 1.04x on the
previous one — one N-API downcall on a 136 B payload is the whole budget
there): the slice list
decode records lives on the stack up to 32 entries, so a small message scans
with no native allocation at all.

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
| 136 B | encode | 890.4 (1.31x ✅) | 678.4 |
| 136 B | decode | 356.7 (1.49x ✅) | 238.8 |
| 174 B | encode | 771.7 (0.96x ❌) | 804.1 |
| 174 B | decode | 463.9 (1.84x ✅) | 252.7 |
| 2469 B | encode | 1361.7 (1.12x ✅) | 1214.2 |
| 2469 B | decode | 898.4 (1.83x ✅) | 491.3 |
| 74913 B | encode | 1600.5 (1.21x ✅) | 1325.7 |
| 74913 B | decode | 1147.7 (1.67x ✅) | 685.3 |

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
  fields and non-canonical encodings. That is where the 1.6-1.8x decode numbers
  come from at 174 B and up, where sub-message tree building used to dominate.
- Field facts (number, kind, wire type, packed/required/oneof, sub type, a
  name -> index Hash) are resolved once per message type into a per-schema cache,
  so the scan and the accessors never walk the descriptor.

All four tiers sit at or above parity except the 174 B encode, which is a
band-boundary cell (0.95-1.08x across runs; today 0.96x, the closest row);
above it the C core's pack-aware writer pulls ahead.

#### Objective-C vs the `protobuf` ObjC runtime

![Objective-C vs the protobuf ObjC runtime — MB/s by tier](docs/charts/bench-objective-c-vs-the-protobuf-objc-runtime.svg)

| payload | op | pbwire-objc (MB/s, x protobuf-objc) | protobuf-objc (MB/s) |
|---|---|---|---|
| 136 B | encode | 1096.3 (6.58x ✅) | 166.5 |
| 136 B | decode | 739.7 (2.84x ✅) | 260.6 |
| 174 B | encode | 1400.4 (6.60x ✅) | 212.1 |
| 174 B | decode | 926.0 (3.09x ✅) | 299.4 |
| 2469 B | encode | 13443.3 (4.64x ✅) | 2895.1 |
| 2469 B | decode | 11645.4 (3.08x ✅) | 3775.5 |
| 74913 B | encode | 78115.4 (2.03x ✅) | 38403.4 |
| 74913 B | decode | 75447.9 (2.82x ✅) | 26773.8 |

The ARC wrapper adds one Objective-C object per call and no copy: `encode` hands
the encoder's own malloc'd buffer to `NSData` with
`dataWithBytesNoCopy:length:freeWhenDone:`, and `decode` wraps the decoder's
arena in one `PBWMessage`. That stays ahead of the generated runtime on all four
tiers and both ops (6.6x / 2.8x at 136 B down to 2.0x / 2.8x at 74913 B). The
xlarge tier is one 74 KB string, so it is a memcpy race: the generated encoder
writes it into an `NSMutableData` of the final size, and the wrapper writes it
into its own buffer, which `NSData` then takes over.

#### Kotlin vs `protoc --kotlin_out` over `protobuf-java`

![Kotlin vs protoc --kotlin_out over protobuf-java — MB/s by tier](docs/charts/bench-kotlin-vs-protoc-kotlin-out-over-protobuf-java.svg)

| payload | op | pbwire-kotlin (MB/s, x protobuf-kotlin) | protobuf-kotlin (MB/s) |
|---|---|---|---|
| 136 B | encode | 1717.8 (0.81x ❌) | 2122.4 |
| 136 B | decode | 973.0 (1.50x ✅) | 650.6 |
| 174 B | encode | 924.6 (0.79x ❌) | 1174.6 |
| 174 B | decode | 785.8 (1.32x ✅) | 593.8 |
| 2469 B | encode | 955.6 (1.11x ✅) | 858.8 |
| 2469 B | decode | 739.0 (1.65x ✅) | 448.8 |
| 74913 B | encode | 961.7 (1.14x ✅) | 840.7 |
| 74913 B | decode | 794.0 (1.78x ✅) | 446.1 |

Kotlin reuses the Java runtime (`bindings/java/runtime`), so these numbers are
`pbwire-java`'s: one workspace lookup plus one `byte[]` copy per call. The 136 B encode
is the case the Java section explains (a generated writer knows its tags, the
dynamic tree walk derives them), here 0.81x against a baseline that reaches
the same protobuf-java encoder through its builder/DSL path. The 174 B encode
trails at 0.79x while the Java row leads on the same codec and bytes: the
codec-level A/B shows no codec-side cause, so this is the harness's
allocation/GC sensitivity on a loaded host, not the codec. From 2.5 KB up
both ops are ahead at every tier.

#### PHP vs the official PHP runtime (pure + C extension)

![PHP vs the official PHP runtime (pure + C extension) — MB/s by tier](docs/charts/bench-php-vs-the-official-php-runtime-pure-c-extension.svg)

| payload | op | pbwire-php-c (MB/s, x protobuf-php-c) | protobuf-php-c (MB/s) |
|---|---|---|---|
| 136 B | encode | 1321.4 (1.68x ✅) | 784.3 |
| 136 B | decode | 783.9 (2.05x ✅) | 381.9 |
| 174 B | encode | 1250.4 (1.45x ✅) | 859.6 |
| 174 B | decode | 700.1 (1.86x ✅) | 375.6 |
| 2469 B | encode | 1436.6 (1.15x ✅) | 1251.1 |
| 2469 B | decode | 689.5 (1.40x ✅) | 493.6 |
| 74913 B | encode | 1364.5 (1.10x ✅) | 1238.9 |
| 74913 B | decode | 774.5 (1.41x ✅) | 547.8 |

The native extension links the C core in, like the Ruby C extension: built
messages live in a per-message arena (one bump allocator instead of a malloc
per value, sequential layout for the encoder), decoded trees keep the core's
single-malloc arena, and both encode straight into the result string. Against
the official C extension — the same official API backed by C, which `bench.sh`
times whenever it is installed — that wins every tier, 1.10-1.68x on encode
and 1.40-2.05x on decode. The `ext-ffi` wrapper runs the same core through one FFI trampoline per call:
it trails on small payloads (0.29x / 0.31x encode at 136-174 B) and converges
from 2.5 KB up (xlarge 1.02x / 1.51x vs the C extension in the same
harness). The pure-PHP runtime, timed by a second `php -n` run
in the same harness, is a different race:

| payload | op | pbwire-php (MB/s, x protobuf-php) | protobuf-php (MB/s) |
|---|---|---|---|
| 136 B | encode | 235.2 (63.57x ✅) | 3.7 |
| 136 B | decode | 213.3 (34.40x ✅) | 6.2 |
| 174 B | encode | 272.5 (77.86x ✅) | 3.5 |
| 174 B | decode | 247.8 (43.47x ✅) | 5.7 |
| 2469 B | encode | 997.3 (293.32x ✅) | 3.4 |
| 2469 B | decode | 610.0 (127.08x ✅) | 4.8 |
| 74913 B | encode | 1286.3 (367.51x ✅) | 3.5 |
| 74913 B | decode | 769.1 (153.82x ✅) | 5.0 |

The pure-PHP runtime spends its time on per-field bookkeeping (~4-6 MB/s flat
across tiers), so quoting those ratios without the C-extension table is
quoting the interpreter, not the codec.

#### Dart vs the `protobuf` Dart package

![Dart vs the protobuf Dart package — MB/s by tier](docs/charts/bench-dart-vs-the-protobuf-dart-package.svg)

| payload | op | pbwire-dart (MB/s, x protobuf-dart) | protobuf-dart (MB/s) |
|---|---|---|---|
| 136 B | encode | 783.4 (3.60x ✅) | 217.5 |
| 136 B | decode | 401.6 (1.94x ✅) | 206.6 |
| 174 B | encode | 689.2 (3.85x ✅) | 179.2 |
| 174 B | decode | 411.7 (2.20x ✅) | 187.3 |
| 2469 B | encode | 1237.0 (11.53x ✅) | 107.3 |
| 2469 B | decode | 624.1 (4.42x ✅) | 141.3 |
| 74913 B | encode | 1430.7 (15.42x ✅) | 92.8 |
| 74913 B | decode | 694.0 (4.27x ✅) | 162.4 |

Both sides allocate per call. `dart:ffi` adds a `malloc`/copy for the input
bytes and a `Uint8List` copy for the output, which is why the gain grows with
payload size: the Dart runtime's pure-Dart field loop dominates it by 2.5 KB,
and at 136 B the boundary is still visible in a 1.9x decode.

#### Rust vs prost (and `rust-protobuf` 3.x)

![Rust vs prost (and rust-protobuf 3.x) — MB/s by tier](docs/charts/bench-rust-vs-prost-and-rust-protobuf-3-x.svg)

| payload | op | pbwire-rust (MB/s, x prost) | prost (MB/s) | protobuf-rust (MB/s) |
|---|---|---|---|---|
| 136 B | encode | 1794.7 (1.18x ✅) | 1523.6 | 1253.2 |
| 136 B | decode | 1095.6 (1.24x ✅) | 886.6 | 742.5 |
| 174 B | encode | 1660.0 (1.12x ✅) | 1481.7 | 1283.5 |
| 174 B | decode | 937.1 (1.51x ✅) | 620.1 | 557.5 |
| 2469 B | encode | 1495.3 (1.06x ✅) | 1407.3 | 787.0 |
| 2469 B | decode | 718.2 (2.13x ✅) | 337.9 | 349.2 |
| 74913 B | encode | 1381.0 (1.01x ✅) | 1364.5 | 849.5 |
| 74913 B | decode | 799.1 (2.16x ✅) | 370.6 | 372.3 |

The archive is linked in, so a call is a direct C call with no `dlopen` lookup.
prost, the fastest widely used Rust runtime (generated at build time by
build.rs, bytes gated like the rest), loses every cell now that encode reuses
one buffer per implementation (pbwire `encode_to()`, rust-protobuf
`write_to_vec()`, prost `encode()`): the one-shot `encode()` paid the core
malloc plus a 75 KB copy that neither baseline pays. pbwire-rust leads every
cell against both (xlarge encode 1.01x / 1.63x, decode 2.16x / 2.15x).

`encode_to()` tries the caller's buffer first and only asks for the exact size
(a second tree walk) when the buffer does not fit, which is what keeps the
measured loop at one walk per call. Sizing on every call instead halves the
encode throughput (xlarge 624 MB/s, 0.52x prost): the number worth checking
first if this row ever regresses.

#### C# vs Google.Protobuf

![C# vs Google.Protobuf — MB/s by tier](docs/charts/bench-c-vs-google-protobuf.svg)

| payload | op | pbwire-csharp (MB/s, x protobuf-csharp) | protobuf-csharp (MB/s) | protobuf-net (MB/s) |
|---|---|---|---|---|
| 136 B | encode | 1279.2 (2.83x ✅) | 451.7 | 555.6 |
| 136 B | decode | 854.0 (1.86x ✅) | 460.3 | 442.3 |
| 174 B | encode | 1309.7 (3.26x ✅) | 401.2 | — |
| 174 B | decode | 820.5 (2.13x ✅) | 385.3 | — |
| 2469 B | encode | 1166.4 (2.23x ✅) | 523.5 | 373.5 |
| 2469 B | decode | 728.7 (1.64x ✅) | 444.4 | 280.9 |
| 74913 B | encode | 1361.5 (2.81x ✅) | 484.3 | 365.1 |
| 74913 B | decode | 785.5 (1.68x ✅) | 468.4 | 306.9 |

Google.Protobuf's C# runtime is already a fast tree codec, so this is the
closest race of the five. Every cell is at or above parity -- the closest is
the 2469 B decode at 1.64x -- and the encode side
stays ~2.2-3.3x ahead at every tier. protobuf-net, the other widely used C#
runtime (hand-written contracts in Pbn.cs, bytes gated like the rest), is
slower than Google.Protobuf on every tier here (xlarge 365.1 vs 484.3
encode): the official runtime is the fastest baseline, so the ratio column
is against it. Its medium rows are
absent on purpose: protobuf-net writes single-element packed fields in
singular form — valid protobuf, semantically equal (the harness checks that
via the Google parser), but not byte-identical, so there is nothing MB/s-fair
to time there.

These rows land at ~1.4 GB/s encode and ~785 MB/s decode on the xlarge tier, which
is the C core's own number: by 2.5 KB the per-call boundary is invisible and only
the baseline's own codec is being measured. That is the point of the schema-at-
runtime design -- the binding owns ergonomics, the core owns the format.

### Lua (C module, tables) vs `lua-protobuf`

![Lua (C module, tables) vs lua-protobuf — MB/s by tier](docs/charts/bench-lua-c-module-tables-vs-lua-protobuf.svg)

| payload | op | pbwire-lua (MB/s, x lua-protobuf) | lua-protobuf (MB/s) |
|---|---|---|---|
| 136 B | encode | 139.4 (1.13x ✅) | 123.7 |
| 136 B | encode_msg | 955.6 (7.73x ✅) | 123.7 |
| 136 B | decode | 144.1 (1.72x ✅) | 83.8 |
| 174 B | encode | 160.1 (1.22x ✅) | 131.3 |
| 174 B | encode_msg | 982.3 (7.48x ✅) | 131.3 |
| 174 B | decode | 141.7 (1.65x ✅) | 85.8 |
| 2469 B | encode | 181.5 (1.12x ✅) | 162.6 |
| 2469 B | encode_msg | 1423.0 (8.75x ✅) | 162.6 |
| 2469 B | decode | 139.3 (1.62x ✅) | 86.0 |
| 74913 B | encode | 206.6 (1.12x ✅) | 183.8 |
| 74913 B | encode_msg | 1496.4 (8.14x ✅) | 183.8 |
| 74913 B | decode | 130.1 (1.47x ✅) | 88.3 |

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
| 136 B | encode | 470.1 (2.38x ✅) | 197.4 | 36.4 |
| 136 B | decode | 499.7 (2.15x ✅) | 232.9 | 38.9 |
| 174 B | encode | 455.4 (3.47x ✅) | 131.3 | 38.9 |
| 174 B | decode | 505.7 (2.17x ✅) | 232.7 | 47.9 |
| 2469 B | encode | 396.4 (3.45x ✅) | 115.0 | 79.5 |
| 2469 B | decode | 320.2 (1.82x ✅) | 175.6 | 139.5 |
| 74913 B | encode | 445.5 (3.89x ✅) | 105.3 | 114.5 |
| 74913 B | decode | 331.6 (1.70x ✅) | 195.6 | 191.6 |

### Swift (SwiftPM, static link) vs `apple/swift-protobuf`

![Swift (SwiftPM, static link) vs apple/swift-protobuf — MB/s by tier](docs/charts/bench-swift-swiftpm-static-link-vs-apple-swift-protobuf.svg)

| payload | op | pbwire-swift (MB/s, x swift-protobuf) | swift-protobuf (MB/s) |
|---|---|---|---|
| 136 B | encode | 985.6 (2.33x ✅) | 423.6 |
| 136 B | decode | 889.3 (3.86x ✅) | 230.5 |
| 174 B | encode | 1077.8 (3.46x ✅) | 311.8 |
| 174 B | decode | 809.6 (4.42x ✅) | 183.3 |
| 2469 B | encode | 1222.5 (4.86x ✅) | 251.8 |
| 2469 B | decode | 685.0 (3.82x ✅) | 179.1 |
| 74913 B | encode | 1408.5 (5.32x ✅) | 264.6 |
| 74913 B | decode | 595.0 (2.88x ✅) | 206.7 |

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
| 136 B | encode | 1629.1 (1.13x ✅) | 1436.1 |
| 136 B | decode | 917.3 (1.44x ✅) | 636.0 |
| 174 B | encode | 1386.2 (1.25x ✅) | 1112.6 |
| 174 B | decode | 751.7 (1.02x ✅) | 736.1 |
| 2469 B | encode | 959.9 (1.31x ✅) | 734.8 |
| 2469 B | decode | 712.4 (1.25x ✅) | 569.8 |
| 74913 B | encode | 914.2 (1.20x ✅) | 762.8 |
| 74913 B | decode | 755.6 (1.38x ✅) | 549.3 |

Same class as Kotlin: the JVM binding reuses the Java runtime and adds only
an idiomatic DSL — no second codec, no JNI, schema from a `FileDescriptorSet`
at run time. The baseline is `scalapbc`-generated code (needs `cs install
scalapbc` once); the jars land in `benchmarks/deps`. Byte parity per tier
before timing, like every other row.

### Elixir (NIF over the shared core) vs `elixir-protobuf`

![Elixir (NIF over the shared core) vs elixir-protobuf — MB/s by tier](docs/charts/bench-elixir-nif-over-the-shared-core-vs-elixir-protobuf.svg)

| payload | op | pbwire-elixir (MB/s, x elixir-protobuf) | elixir-protobuf (MB/s) |
|---|---|---|---|
| 136 B | encode | 143.6 (3.20x ✅) | 44.9 |
| 136 B | decode | 78.8 (1.36x ✅) | 57.9 |
| 174 B | encode | 147.6 (3.10x ✅) | 47.6 |
| 174 B | decode | 94.0 (1.63x ✅) | 57.8 |
| 2469 B | encode | 140.2 (2.57x ✅) | 54.5 |
| 2469 B | decode | 90.4 (1.70x ✅) | 53.3 |
| 74913 B | encode | 129.6 (2.25x ✅) | 57.5 |
| 74913 B | decode | 74.8 (1.38x ✅) | 54.2 |

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
| 136 B | encode | 1746.3 (1.39x ✅) | 1256.2 |
| 136 B | decode | 1051.8 (48.69x ✅) | 21.6 |
| 174 B | encode | 1671.3 (1.70x ✅) | 982.5 |
| 174 B | decode | 928.0 (49.10x ✅) | 18.9 |
| 2469 B | encode | 1526.9 (1.88x ✅) | 810.5 |
| 2469 B | decode | 747.8 (12.53x ✅) | 59.7 |
| 74913 B | encode | 1416.2 (1.71x ✅) | 829.9 |
| 74913 B | decode | 806.1 (2.71x ✅) | 297.4 |

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
| 136 B | encode | 1149.6 (2.63x ✅) | 437.5 |
| 136 B | decode | 856.8 (1.85x ✅) | 462.0 |
| 174 B | encode | 1256.2 (3.29x ✅) | 381.9 |
| 174 B | decode | 820.0 (2.21x ✅) | 371.5 |
| 2469 B | encode | 1136.5 (2.21x ✅) | 514.5 |
| 2469 B | decode | 702.3 (1.54x ✅) | 457.3 |
| 74913 B | encode | 1386.4 (3.13x ✅) | 443.4 |
| 74913 B | decode | 786.9 (1.65x ✅) | 477.3 |

Same class as Kotlin: F# reuses the C# binding's project via
`ProjectReference` and adds only an F#-idiomatic facade — no second runtime,
no packages, schema from a `FileDescriptorSet` at run time. The baseline is
`protoc`-generated C# compiled in a sibling project (F# cannot compile
`.cs`). Byte parity per tier before timing.

### Clojure (Java runtime + interop) vs `protobuf-java`

![Clojure (Java runtime + interop) vs protobuf-java — MB/s by tier](docs/charts/bench-clojure-java-runtime-interop-vs-protobuf-java.svg)

| payload | op | pbwire-clj (MB/s, x protobuf-java) | protobuf-java (MB/s) |
|---|---|---|---|
| 136 B | encode | 1493.8 (0.73x ❌) | 2038.3 |
| 136 B | decode | 897.4 (1.41x ✅) | 634.4 |
| 174 B | encode | 1339.1 (1.10x ✅) | 1216.2 |
| 174 B | decode | 742.4 (1.30x ✅) | 571.7 |
| 2469 B | encode | 1063.2 (1.29x ✅) | 822.7 |
| 2469 B | decode | 726.5 (1.65x ✅) | 440.0 |
| 74913 B | encode | 996.7 (1.17x ✅) | 850.8 |
| 74913 B | decode | 784.8 (1.80x ✅) | 436.4 |

The timed path reuses a pre-built mutable message (fill once, encode many;
decode plus close, no map materialization), like the Java row, and the
baseline is a direct `toByteArray` call: the measured type carries a hint so
the call compiles direct. An unhinted local reflects (~800 ns a call here)
and never warms, which once inflated this row past 9x. The persistent-map
fns stay for convenience and are gated for parity, but hot loops use the
mutable path — the same split Lua documents (table vs builder). Same codec
as Java/Scala underneath (plain interop over the Java runtime, no second
wire implementation), byte parity per tier.

### Julia (`dlopen` + `ccall`) vs ProtoBuf.jl

![Julia (dlopen + ccall) vs ProtoBuf.jl — MB/s by tier](docs/charts/bench-julia-dlopen-ccall-vs-protobuf-jl.svg)

| payload | op | pbwire-julia (MB/s, x ProtoBuf.jl) | ProtoBuf.jl (MB/s) |
|---|---|---|---|
| 136 B | encode | 1331.1 (3.55x ✅) | 375.3 |
| 136 B | decode | 1034.1 (0.89x ❌) | 1162.3 |
| 174 B | encode | 1492.8 (4.78x ✅) | 312.6 |
| 174 B | decode | 873.4 (1.28x ✅) | 681.8 |
| 2469 B | encode | 1233.6 (11.00x ✅) | 112.1 |
| 2469 B | decode | 707.3 (1.19x ✅) | 592.5 |
| 74913 B | encode | 1394.4 (123.40x ✅) | 11.3 |
| 74913 B | decode | 768.0 (1.13x ✅) | 677.3 |

FFI host like Dart/C#: `Libdl.dlopen` + `ccall` into the C ABI (function
pointers resolved once via `dlsym`, since recent Julia needs a compile-time
library name), `Dict`-based messages convert through the C ABI, no wire
code, no `.proto` parser. The baseline is `protojl`-generated code
(`benchmarks/oracle/matrix3_pb.jl`, produced by `protojl("matrix3.proto",
"<proto dir>", "benchmarks/oracle")`). Its encode collapses with size
(pure-Julia per-field appends). Decode sits at parity: both sides land near
the C core's own ~700 MB/s, so the medium, large and xlarge tiers lead at
1.1-1.3x while the small tier trails on fixed FFI overhead (like
Java/Kotlin small-tier floors); the headline remains the xlarge encode
column.

### R (C extension, named lists) vs RProtoBuf

![R (C extension, named lists) vs RProtoBuf — MB/s by tier](docs/charts/bench-r-c-extension-named-lists-vs-rprotobuf.svg)

| payload | op | pbwire-r (MB/s, x RProtoBuf) | RProtoBuf (MB/s) |
|---|---|---|---|
| 136 B | encode | 92.2 (36.88x ✅) | 2.5 |
| 136 B | decode | 88.0 (35.20x ✅) | 2.5 |
| 174 B | encode | 103.3 (38.26x ✅) | 2.7 |
| 174 B | decode | 96.7 (35.81x ✅) | 2.7 |
| 2469 B | encode | 147.4 (61.42x ✅) | 2.4 |
| 2469 B | decode | 133.1 (49.30x ✅) | 2.7 |
| 74913 B | encode | 169.1 (65.04x ✅) | 2.6 |
| 74913 B | decode | 155.4 (64.75x ✅) | 2.4 |

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
once, `encode` per send, 1390.5 MB/s on the xlarge tier) runs at the core's
speed; the table above times the one-shot path both sides. Encode is no
contest at any tier: setting 3000 nested messages field by field through S4
dispatch costs RProtoBuf ~27 ms where the C core walks the list once. Decode
is compared like for like: `RProtoBuf::read` parses in C++ and materializes
fields lazily on `$`, while `pbwire_decode` builds the full named-list tree
eagerly, so the baseline side reads every field into the same plain-list shape
(the table's `decode` row). A bare `read()` alone is a few times faster than
`pbwire_decode`, but it hands back a Message, not R data; once both sides
produce plain lists, pbwire leads 64x at xlarge. Two R spellings to know: `INT32_MIN`
reads back
as `NA` (it *is* `NA_INTEGER`), and doubles past 2^63 clamp to the int/uint
limits — both documented in the binding README.

## Embedded: nanopb, same message, both sides

The embedded facade has its own paired harness, because nanopb is the baseline
that matters on that target: one fixed-size message (`bindings/embedded/benchmarks/msg.proto`),
four tiers, pbwire through `pbwe_open_mem` (caller-memory arena) against
nanopb's generated static struct. Encoders produce byte-identical wire on every
tier (asserted in the run).

| tier | bytes | pbwire enc | nanopb enc | pbwire dec | nanopb dec |
|---|---|---|---|---|---|
| x0 | 52 | 23.3 ns | 131.0 ns | 35.7 ns | 264.6 ns |
| x1 | 55 | 26.1 ns | 141.6 ns | 45.3 ns | 280.1 ns |
| x100 | 154 | 87.9 ns | 753.0 ns | 338.4 ns | 723.9 ns |
| x3000 | 5928 | 3100.0 ns | 23506.0 ns | 9924.5 ns | 18768.0 ns |

Flash for the same message, `arm-none-eabi-gcc -Os -mthumb` (no libc, no LTO):
pbwire's generic codec is 39211 B for **every** type in the schema, nanopb's
runtime + generated code is 7021 B for **one** message type. The full tables,
the RAM model and the reasoning (including the static-schema option that was
evaluated and left out) are in
[bindings/embedded/README.md](bindings/embedded/README.md#footprint-and-the-nanopb-comparison).

## Run them

```bash
make bench        # core: 4 cases + text/json/stream/threads (needs protoc + libprotobuf)
make bench-c      # C structs vs protoc C++ on matrix3.proto (same deps)
make nanopb-setup # once: shallow nanopb clone for the embedded comparison
make embedded-bench  # embedded facade vs nanopb (host throughput, byte check)
make embedded-size   # embedded facade vs nanopb (Cortex-M flash)
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
version and iteration counts above. Run one harness at a time on an
otherwise idle machine: back-to-back batches throttle a laptop CPU and
under-report by up to 2x.

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

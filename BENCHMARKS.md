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
- [Fairness contract](docs/perf-fairness.md) — what makes a row comparable: full-flow native in/out, matched freshness and depth, same-run calibration
- [Generated models](#generated-models-codegen) — what the generated layer costs
- [Per-binding tables](#cross-binding-comparison-last-run) — one section per language (✅ exceeds baseline ≥1.0x, ❌ below):
  - [Core](#core-pbw_ctx-vs-plain-vs-generated) · [C structs](#c-structs-vs-protoc-generated-same-matrix3proto) · [Go](#go-thin-model-vs-protobuf-go) · [Python](#python-vs-googleprotobuf-upb)
  - [Java](#java-vs-protobuf-java) · [Node](#node-vs-protobufjs) · [Ruby](#ruby-c-extension-vs-the-google-protobuf-gem) · [Objective-C](#objective-c-vs-the-protobuf-objc-runtime)
  - [Kotlin](#kotlin-vs-protoc---kotlin_out-over-protobuf-java) · [PHP](#php-vs-the-official-php-runtime-pure--c-extension) · [Dart](#dart-vs-the-protobuf-dart-package) · [Rust](#rust-vs-prost) · [C#](#c-vs-googleprotobuf) · [Lua](#lua-c-module-tables-vs-lua-protobuf) · [Erlang](#erlang-pbwire-vs-gpb-vs-gpb-nif)
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
(`MB/s = bytes / ns/op * 1000`). The comma and the iteration count are
optional: a harness that calibrates per op (Go, Python) has no single count
for a case, and `scripts/bench-record.py` accepts the shorter header.
`make bench-c` adds one more block per case
(`pbwire-c1`: the C binding's one-shot API, which compiles the layout on every
call) after the standard rows; only the standard block is aggregated
below.
Rows are named by the shipped entry point they time. The published comparison
is the struct-passing headline: one native object in, one codec call -- the
same task the generated baseline runs when it fills its struct. `shim` is that
task in one crossing: C reads the host's memory through a layout table, with
no intermediate tree. `shim-ffi` (PHP only) is the same array API on the
no-compiler `ext-ffi` deployment. Go's `-par` rows time the same thin-model ops
across `GOMAXPROCS` goroutines. The JVM bindings and Clojure publish the
generated-model `core`: the same pure-Java runtime with shipped ergonomics
(the mutable-message fast path on Clojure). The per-field `tree` arm -- a
native object in and out through the shipped per-field API, no prebuilt tree
before the timer -- is a `pbench/`-only arm now: compared in
`bindings/<lang>/pbench/README.md`, never pasted here. Anything else a harness
prints -- the prebuilt-handle `proxy` task, pure host-language `core` ports,
Go's per-field `dync` tree, the vendored `cshim` candidate, the one-crossing
JNI ceiling -- lives in the same place.

Timer boundary (all bindings): a fresh native object per iteration, allocation
and `free`/`close` inside the timer, deep materialization of every
gate-checked field. Headline encode arms build that input fresh inside the
timer on both sides (new + set + encode, the real scenario); reused
encode buffers stay allowed only where the harness already does it and says
so. A steady-state reference row (app-owned input reused across iterations)
may stay in a harness labeled as such, but it is never the pasted headline.
Plain-data fixtures may be reused across iterations
(fixture-reuse ruling, `docs/perf-attempts.md` Python worker D3).

The Python harness names its ops by the task as well as the representation
(`encode(data)`, `decode(bytes)`, `to_dict(bytes)`): a
dict-based binding has two representations, and the row name says which one the
timer covers.


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

> **Optimization (2026-11-14):** the proto3 `string` UTF-8 validation that
> runs at parse time (see
> [perf-attempts.md](docs/perf-attempts.md#aligned-cost-proto3-string-utf-8-validation))
> was the main reason the decode rows below had fallen behind. It now shares
> one sequence rule and skips ASCII runs 16 bytes at a time, so a 74 KB value
> costs ~1.3 us instead of ~20 us and a short value is checked inline (measured
> in the ledger). The tables below are the post-change runs; the remaining
> decode gaps are baseline strength, not validator cost.

Last full refresh: 2026-11-23, 05:17-06:15 UTC (every `make *-bench`
on this tree, one harness at a time, the whole matrix in ~60 minutes), plus an
08:30 UTC core re-run after `bench/bench.cc` switched the generated and
DynamicMessage encode arms to fresh-build per iteration (same construct +
serialize calling method as the pbwire arms; see the Core section). Core
re-run 2026-11-24, 07:19 UTC: the pbwire arms now go through the generated C
struct and the compiled struct codec (thin model, same new + set + encode task
as the baseline; formal run `results/bench-core-20261124T071914Z.json`).
Core re-run 2026-11-24, 07:38 UTC: the plain arms go through a fresh arena per
call (same batched profile as the reference parse; formal run
`results/bench-core-20261124T073808Z.json`). Go-parity re-time 2026-11-21/22 (issue 41 plan): every binding
with a `pbench/` re-times its `tree` arm as the native-in/native-out full task
(fresh native object per iteration, alloc and free inside the timer, deep
materialization of every gate-checked field); `shim` rows and the shipped JVM
`core` rows keep their timers except where the task
changed. Pbench-only arms (the prebuilt `proxy` task, pure-language `core`
ports, Go `dync`, the vendored `cshim`, the JNI ceiling) stay in
`bindings/<lang>/pbench/README.md` and are never pasted here. The re-timed runs are `results/bench-<lang>-2026112*.json` (one
binding at a time under `results/.bench.lock`), and the per-binding tables,
the at-a-glance rows and the per-binding summary below are pasted from them.
Partial refresh 2026-11-15, 02:26-03:12 UTC: core, Python
and Julia re-ran after the first generic pass (plain-encode `wire_len` hint,
32 inline Python slices, sealed-arena implicit reset), then Java, Kotlin,
Scala, Clojure and Julia again plus core and Python once more after the
follow-ups (arena reset keeps the used chunk prefix, `Message` drops its
`AtomicBoolean`, decode drops its `ThreadLocal` reader, Scala measures
interleaved), and finally 04:00-04:30 UTC core, C, Python, Julia, Go, Rust,
Node, Ruby, Lua, PHP and Zig re-ran once the encoder cached its buffer base
pointer and capacity (a C-only change, so Java, Kotlin, Scala and Clojure kept
their 03:00 numbers). The at-a-glance rows, the per-binding summary and the
core table reflect those runs, except where the Go-parity re-time above
re-pasted a binding's table from its `results/bench-<lang>-2026112*.json`
run. Each row is pasted verbatim from one run of that binding, so
both columns of a row come from the same measurement; where a binding
was re-run (Ruby, Objective-C, Kotlin, Elixir, Julia, Erlang) the pasted run is the one
whose own baseline did not collapse under host load (a run whose fastest
baseline is under 85% of that binding's median baseline is treated as
contaminated and skipped). The host carried someone else's background load
throughout (load average 5-14 on 14 cores, ~0.4-1.0 normalised), so absolute
MB/s reads 10-20% low against a quiet machine while `x baseline` reproduces
(run-to-run variation is ±10% per cell, more on the JVM narrow tiers).
Erlang ran from its own record this time
(`results/bench-erlang-20261123T055756Z.json`), pasted like every other binding.
Dart ran with `protoc-gen-dart` 25.1.0 on `PATH` (`dart pub global activate
protoc_plugin` plus `~/.pub-cache/bin`).

| piece | version |
|---|---|
| machine | Apple M4 Pro, 14 cores, 48 GB RAM |
| OS | macOS (Darwin 27.2.0, arm64) |
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
| core (dynamic) | `make bench` (`bench/bench.cc`) | `protoc` C++ generated + DynamicMessage | all 4 | 2.19x ✅ / 1.88x ✅ `ctx` (plain 2.22x ✅ / 1.89x ✅; thin model, both sides build fresh per iteration) |
| C structs | `make bench-c` (`bindings/c/benchmarks/bench_struct.cc`) | `protoc` C++ generated from the same `matrix3.proto` | all 4 | 1.34x ✅ / 2.30x ✅ `shim` |
| Erlang/OTP NIF | `make benchmarks` in `bindings/erlang/benchmarks` | gpb (pure Erlang), with gpb NIF (libprotobuf) as the second baseline, same `matrix3.proto` | all 4 | 3.00x / 1.52x vs best-of-two ✅ |
| Python (CPython ext) | `make python-bench` (`bindings/python/benchmarks/bench.py`) | `google.protobuf` (upb C extension) on the same `matrix3.proto` | all 4 | 7.18x ✅ `shim encode(data)` / 3.75x ✅ `shim to_dict` |
| C++ | same engine as C (`bindings/cpp` mirrors the C structs; `make bench-c` is the harness) | `protoc` C++ generated from the same `matrix3.proto` | all 4 | 1.34x ✅ / 2.30x ✅ |
| Go (thin model, pure-Go engine) | `make go-bench` (`bindings/go/benchmarks`) | `protobuf-go` generated code from the same `matrix3.proto` | all 4 | 1.70x ✅ / 1.25x ✅ (xlarge; full tiers + allocs in section) |
| Java (pure-Java codec) | `make java-bench` (`bindings/java/benchmarks`) | `protobuf-java` generated code from the same `matrix3.proto` | all 4 | 0.72x ❌ / 0.79x ❌ `core` (generated model) |
| Node (N-API addon) | `make node-bench` (`bindings/node/benchmarks`) | [protobufjs](https://www.npmjs.com/package/protobufjs) from the same `matrix3.proto` | all 4 | 0.12x ❌ / 0.12x ❌ `shim` vs protobufjs |
| Ruby (C extension) | `make ruby-bench` (`bindings/ruby/benchmarks`) | the `google-protobuf` gem (C extension) | all 4 | 0.18x ❌ / 0.23x ❌ `shim` (one C-extension entry per op) |

| Objective-C (ARC) | `make objc-bench` (`bindings/objc/benchmarks`) | the `protobuf` ObjC runtime + generated code from the same `matrix3.proto` | all 4 | 0.01x ❌ / 1.18x ✅ `tree` (encode 0.37x-0.005x, decode 1.18x-2.95x; fresh build per iteration) |
| Kotlin (pure-Java codec + DSL) | `make kotlin-bench` (`bindings/kotlin/benchmarks`) | `protoc --kotlin_out` over `protobuf-java`, same `matrix3.proto` | all 4 | 0.76x ❌ / 0.75x ❌ `core` (generated model) |
| PHP (`ext-ffi` + C extension) | `make php-bench` (`bindings/php/benchmarks`) | the official PHP runtime (`protobuf/php/src`): C extension (fastest) + pure-PHP | all 4 | 0.16x ❌ / 0.37x ❌ `shim` vs C ext (`shim-ffi` interpreter-scale; pure-PHP tables in section) |
| Dart (`dart:ffi`) | `make dart-bench` (`bindings/dart/benchmarks`) | the `protobuf` Dart package + generated code | all 4 | 11.03x ✅ / 4.64x ✅ `shim` (one C-side reader call per op) |
| Rust (static link) | `make rust-bench` (`bindings/rust/benchmarks`) | prost generated code from the same `matrix3.proto` | all 4 | 0.92x ❌ / 1.90x ✅ `shim` vs prost (one C-side struct-codec call per op; encode above prost at 136/174 B) |
| C# (P/Invoke) | `make csharp-bench` (`bindings/csharp/benchmarks`) | Google.Protobuf C# generated code from the same `matrix3.proto` | all 4 | 1.42x ✅ / 0.73x ❌ `shim` (one P/Invoke per op over the flat image) |
| Lua (C module, tables) | `make lua-bench` (`bindings/lua/benchmarks`) | `lua-protobuf` 0.6.0 | all 4 | 0.99x ❌ / 1.42x ✅ `shim` (native-object, one C entry per op; xlarge encode is a run-to-run tie) |
| Swift (SwiftPM, static link) | `make swift-bench` (`bindings/swift/benchmarks`) | `apple/swift-protobuf` 1.39.0 generated code from the same `matrix3.proto` | all 4 | 3.22x ✅ / 2.49x ✅ `shim` (one C-side layout-codec call per op) |
| Scala (Java runtime + DSL) | `make scala-bench` (`bindings/scala/benchmarks`) | ScalaPB 0.11.18 generated code from the same `matrix3.proto` | all 4 | 0.39x ❌ / 0.25x ❌ `core` (case class) |
| Elixir (NIF over the shared core) | `make elixir-bench` (`bindings/elixir/benchmarks`) | `elixir-protobuf` 0.17.1 structs from the same `matrix3.proto` | all 4 | 2.84x ✅ / 1.84x ✅ |
| Zig (static link) | `make zig-bench` (`bindings/zig/benchmarks`, `-Doptimize=ReleaseFast`) | Arwalk/zig-protobuf 7.0.0 generated code from the same `matrix3.proto` | all 4 | 1.38x ✅ / 1.37x ✅ `shim` (`extern struct` mirror, one C struct-codec call per op) |
| F# (facade over the C# runtime) | `make fsharp-bench` (`bindings/fsharp/benchmarks`) | Google.Protobuf C# generated code from the same `matrix3.proto` | all 4 | 1.34x ✅ / 0.70x ❌ `shim` (one C-reader call over the flat image) |
| Clojure (Java runtime + interop) | `make clojure-bench` (`bindings/clojure/benchmarks`) | `protobuf-java` generated code from the same `matrix3.proto` | all 4 | 1.18x ✅ / 1.77x ✅ `core` (0.70x at 136 B) |
| Julia (`ccall` FFI) | `make julia-bench` (`bindings/julia/benchmarks`) | JuliaIO/ProtoBuf.jl 1.3.0 generated code from the same `matrix3.proto` | all 4 | 94.42x ✅ / 1.07x ✅ `shim` (one C-side layout read per op; xlarge encode on the ProtoBuf.jl baseline collapse; 136 B shim decode 0.95x) |
| R (C extension, named lists) | `make r-bench` (`bindings/r/benchmarks`) | RProtoBuf 0.4.28 on the same `matrix3.proto` | all 4 | 51.90x ✅ / 53.00x ✅ `shim` (named list, one `.Call`, the walk in C) |

"vs baseline" is encode / decode throughput relative to the most-popular baseline on
the largest case the binding runs. Each binding is compared against its
most-popular package (a single baseline; PHP and Erlang keep their
same-package deployment variants: the official C extension next to pure-PHP
rows, gpb next to the gpb NIF). Every binding publishes the row its headline mechanism allows, and every row name says what it times. Every arm times a
native object in and out on the same payload, never a tree built before the
timer.

| binding | runtime | shipped entry points |
|---|---|---|
| core (dynamic) | C core, direct call | `tree` (`pbw_ctx` + plain API) |
| C structs | C struct codec, layout tables | `shim` (the dynamic `pbw_ctx` block is its `tree` arm) |
| C++ | same engine as C | `shim` (mirrors the C structs) |
| Embedded | C core, caller-memory arena | `tree` |
| Erlang/OTP NIF | NIF over the shared core | single NIF arm |
| Elixir | NIF over the shared core | single NIF arm |
| Go | pure-Go table engine, no cgo | thin model + `-par` rows |
| Python | CPython extension | `shim` |
| Java | pure-Java codec | `core` (generated model) |
| Kotlin | the pure-Java codec + DSL | `core` (generated model) |
| Scala | the Java runtime + DSL | `core` (case class) |
| Clojure | the Java runtime + interop | `core` (mutable-message fast path) |
| Node | N-API addon | `shim` |
| Ruby | C extension | `shim` |
| Objective-C | ARC wrapper over the core | single ARC arm |
| PHP | C extension + `ext-ffi` deployment | `shim` + `shim-ffi` |
| Dart | `dart:ffi` binding | `shim` |
| Rust | static link | `shim` |
| C# | P/Invoke | `shim` |
| Lua | C module, tables | `shim` |
| Swift | static link (SwiftPM) | `shim` |
| Zig | static link | `shim` |
| F# | facade over the C# runtime | `shim` |
| Julia | `dlopen` + `ccall` | `shim` |
| R | C extension, named lists | `shim` |

Pbench-only arms (the prebuilt `proxy` task, pure host-language `core` ports,
Go's per-field `dync` tree, the vendored `cshim` candidate, the one-crossing
JNI ceiling) are compared in `bindings/<lang>/pbench/README.md`, never here.

So the standard block is the struct-passing headline arm: `shim` on every
binding that ships it, the generated-model `core` on the JVM bindings and
Clojure, `shim` alone on C/C++ (whose `tree` arm is the dynamic `pbw_ctx`
block), the single ARC arm on Objective-C and the single NIF arm on
Erlang/Elixir, the thin model plus `-par` rows on Go, `tree` alone on
Embedded, and `shim` next to `shim-ffi` on PHP. The per-field `tree` arm
moved to `pbench/`: the tier tables below keep the headline arm only, plus
the Go `-par` rows in the same table, the two extra PHP FFI tables, and the
Julia `dec+ctx` rows in the same table. The single-mechanism bindings
(C/C++/Embedded/ObjC/Erlang/Elixir) publish one arm where a second would
measure pbwire against itself or against a baseline that is already ours; F#
reuses the C# runtime, and Kotlin/Scala/Clojure reuse the Java runtime, so
their rows are native rows of the same engine.

Every binding with a
wired baseline is re-timed Go-parity (2026-11-21/22, C-side 2026-11-23): every published row
times the same native-in/native-out task on the same payload, never a tree
built before the timer; bytes are identical everywhere. The headline rows now
time the same task the generated baseline times -- one native object in, one
codec call -- so the sub-1.0x cells that remain are codec gaps, not boundary
floors: Node's `shim` (0.12x / 0.12x of protobufjs at
74 KB), Ruby's and PHP's `shim` (0.18x / 0.23x of the gem; 0.16x / 0.37x of
the official C extension), the JVM `core` (Java 0.72x / 0.79x of
protobuf-java; Kotlin 0.76x / 0.75x and Scala 0.39x / 0.25x ride the same
runtime), Rust's `shim` encode from 2469 B up (0.92x / 0.92x: the shared C
struct encoder reserves per repeated element, while the same arm's decode rows
are above the line), the managed materialization on C#/F# `shim` decode at
74 KB (0.73x / 0.70x: the C decode is faster than the reference parser, but
materializing 3000 managed objects costs more than its whole run), and
Julia's 136 B `shim` decode (0.95x: the C struct decode plus a `ccall`
against ProtoBuf.jl's pure-Julia generated decoder), and ObjC `tree` encode
(0.37x-0.01x: a fresh per-field build plus alloc/free inside
the timer; the ObjC xlarge tier
rebuilds its 74 KB `str` fixture per iteration). The core's old `tree` headline
(0.39x-0.58x) retired with the fairness contract: the tree-API build price
(malloc + field lookup per value vs direct stores) is no longer a headline. The Lua xlarge `shim` encode cell (0.99x) is a
run-to-run tie, and so is Go's xlarge decode-par (0.99x, serial twin 1.25x).
Julia's xlarge `shim`
encode ratio (94.42x) is a baseline collapse, not a codec win:
ProtoBuf.jl's generated encoder falls to 12.4 MB/s there. Each cell is inside
the run-to-run band of its own baseline; `docs/perf-attempts.md` records what
was tried there.


Correctness first everywhere: the core bench verifies byte-for-byte equality
before timing, the C and Python benches check their bytes against the
`protoc`-produced reference (plus a decode→encode round trip) before timing,
the Java, Kotlin, Ruby, Rust, C#, PHP and Zig harnesses
require their bytes to equal the official encoder's bytes before timing
anything, the Swift, Scala, Elixir, F#, Clojure and Julia harnesses do
the same against swift-protobuf / ScalaPB / elixir-protobuf / Google.Protobuf /
protobuf-java / ProtoBuf.jl respectively
(the R harness gates on the tier sizes, baseline byte equality, plus a full decode round trip, since
R cannot spell INT32_MIN distinctly from NA), and the Erlang and Elixir harnesses require the pinned wire size and byte
equality (encode, plus re-encoding a decoded reference) plus a field-by-field read-back of the decoded record/struct
from every backend
before timing.

## At a glance: every binding, one tier

One row per implementation, on the largest tier that binding runs, from the
per-binding tables further down (same machine, same run). `x baseline` is the
column to compare: absolute MB/s is not comparable across languages because the
payload, the boundary cost and the baseline all differ.

| binding | reaches the core | xlarge wire size | encode MB/s (x baseline) | decode MB/s (x baseline) |
|---|---|---|---|---|
| core dynamic (`pbw_ctx`) | direct call, thin model | 85933 B | 712.4 (2.19x ✅) `ctx` (fresh thin-model build per iteration) | 851.2 (1.88x ✅) |
| core dynamic (plain API) | direct call, thin model | 85933 B | 718.1 (2.22x ✅) (fresh thin-model build per iteration) | 852.6 (1.89x ✅) |
| C structs | direct call, layout tables | 74913 B | 1101.4 (1.34x ✅) `shim` | 701.1 (2.30x ✅) `shim` |
| Go | pure-Go table engine, no cgo | 74913 B | 976.4 (1.70x ✅) | 362.3 (1.25x ✅) |
| Python | CPython extension, dict path | 74913 B | 615.0 (7.18x ✅) `shim encode(data)` | 164.9 (3.75x ✅) `shim to_dict` |
| Java | pure-Java codec, no native code | 74913 B | 660.7 (0.72x ❌) `core` (generated model) | 338.2 (0.79x ❌) `core` |
| Kotlin | the pure-Java codec + DSL | 74913 B | 687.1 (0.76x ❌) `core` | 352.8 (0.75x ❌) `core` |
| Node | N-API addon | 74913 B | 60.1 (0.12x ❌ vs protobufjs) `shim` | 54.7 (0.12x ❌ vs protobufjs) `shim` |
| Dart | `dart:ffi` binding | 74913 B | 1125.1 (11.03x ✅) `shim` | 725.1 (4.64x ✅) `shim` |
| Ruby | C extension over the core | 74913 B | 252.7 (0.18x ❌) `shim` | 163.1 (0.23x ❌) `shim` |

| Objective-C | ARC wrapper | 74913 B | 214.0 (0.01x ❌) `tree` (fresh build per iteration) | 33177.3 (1.18x ✅) |
| PHP | C extension | 74913 B | 216.0 (0.16x ❌) `shim`; `shim-ffi` 5.4 | 211.3 (0.37x ❌) `shim`; `shim-ffi` 4.6 |
| Rust | static link | 74913 B | 1190.7 (0.92x ❌ vs prost) `shim` | 766.0 (1.90x ✅) `shim` |
| C# | P/Invoke | 74913 B | 657.6 (1.42x ✅) `shim` | 370.3 (0.73x ❌) `shim` |
| Lua | C module, tables | 74913 B | 99.4 (0.99x, tie) `shim` | 123.0 (1.42x ✅) `shim` |
| Erlang | NIF (`pbwire`) | 74913 B | 328.5 (3.00x ✅) | 356.1 (1.52x ✅) |
| Swift | static link (SwiftPM) | 74913 B | 923.2 (3.22x ✅) `shim` | 568.3 (2.49x ✅) `shim` |
| Scala | the Java runtime + DSL | 74913 B | 332.0 (0.39x ❌) `core` (case class) | 175.5 (0.25x ❌) `core` |
| Elixir | NIF (Hex `:pbwire`, same core as Erlang) | 74913 B | 147.9 (2.84x ✅) | 108.1 (1.84x ✅) |
| Zig | static link | 74913 B | 1186.6 (1.38x ✅) `shim` | 768.7 (1.37x ✅) `shim` |
| F# | P/Invoke over the C# runtime | 74913 B | 657.0 (1.34x ✅) `shim` | 362.8 (0.70x ❌) `shim` |
| Clojure | the Java runtime + interop | 74913 B | 1084.8 (1.18x ✅) `core` (0.70x at 136 B) | 804.8 (1.77x ✅) `core` |
| Julia | `dlopen` + `ccall` | 74913 B | 1170.8 (94.42x ✅, baseline collapse) `shim` | 766.7 (1.07x ✅) `shim` |
| R | C extension, named lists | 74913 B | 155.7 (51.90x ✅) `shim` | 153.7 (53.00x ✅) `shim` |

Read it as: the in-process core sets the pace (2.2x the C++
generated code on encode at this size, 1.9x on decode), and every headline row times the same task as
the generated baseline -- one native object in, one codec call -- so the
below-the-line cells are codec gaps, not boundary floors: Node `shim`
(0.12x / 0.12x of protobufjs), Ruby `shim`
(0.18x / 0.23x), PHP `shim` (0.16x / 0.37x of the official C extension), the
JVM `core` (Java 0.72x / 0.79x, Kotlin 0.76x / 0.75x, Scala 0.39x / 0.25x).
The above-the-line cells are the same task won outright: the core thin
model 2.2x / 2.0x, Rust `shim` decode
1.90x, Dart 4.64x, C# `shim` encode 1.42x, Swift 3.22x / 2.49x, Zig
1.38x / 1.37x. The outliers are baseline or unit effects, not pbwire
effects: Python's encode column is the dict path (3000 sub-messages, 12000
ints), Julia's xlarge encode column is a baseline collapse (ProtoBuf.jl's
generated encoder falls to 12.4 MB/s there), and the Objective-C tier is a
memcpy race over one 74 KB string (the 16-byte UTF-8 word scan is what keeps
its decode near the copy cost).
The headline names the most-popular baseline: Node's native rows are quoted
over protobufjs, and Node's `shim` row -- the native object in and out --
is 0.12x / 0.12x of protobufjs: the same task on both sides, so the gap is
the codec's, not the boundary's. PHP's native rows are over the official C
extension (the pure-PHP runtime trails it by ~360x / ~110x at 74 KB) -- see
their sections. The old per-field `tree` headlines are retired: the published
rows are the struct-passing headline arm, and per-field timings stay in
`bindings/<lang>/pbench/README.md`.

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

Last run: 2026-11-14, `make bench-docker` on the image in the repo, `make
bench-compare` for the pairing. Absolute MB/s per row is what `--absolute`
prints; the speedup against the same baseline on the same side is what travels.

### core

- macOS: 2026-11-14T18:00:19+00:00 (Darwin 27.0.0, arm64, Apple M4 Pro, 14 cores, libprotoc 36.2)
- Linux: 2026-11-14T06:30:14+00:00 (Linux 6.8.0-64-generic container, aarch64, ?, 4 cores, libprotoc 36.2)

| case | op | macOS x baseline | Linux x baseline |
|---|---|---|---|
| xlarge | encode | 1.42x (pb gen) | 1.14x (pb gen) |
| xlarge | decode | 1.78x (pb gen) | 2.31x (pb gen) |

### c

- macOS: 2026-11-14T18:19:31+00:00 (Darwin 27.0.0, arm64, Apple M4 Pro, 14 cores, libprotoc 36.2)
- Linux: 2026-11-14T06:31:26+00:00 (Linux 6.8.0-64-generic container, aarch64, ?, 4 cores, libprotoc 36.2)

| case | op | macOS x baseline | Linux x baseline |
|---|---|---|---|
| xlarge | encode | 1.32x (pb gen) | 1.32x (pb gen) |
| xlarge | decode | 1.96x (pb gen) | 2.82x (pb gen) |

### go

- macOS: 2026-11-14T18:20:12+00:00 (Darwin 27.0.0, arm64, Apple M4 Pro, 14 cores, libprotoc 36.2)
- Linux: 2026-11-14T06:32:08+00:00 (Linux 6.8.0-64-generic container, aarch64, ?, 4 cores, libprotoc 36.2)

| case | op | macOS x baseline | Linux x baseline |
|---|---|---|---|
| xlarge | encode | 2.61x (protobuf-go) | 2.65x (protobuf-go) |
| xlarge | decode | 2.38x (protobuf-go) | 2.27x (protobuf-go) |

### python

- macOS: 2026-11-14T18:21:09+00:00 (Darwin 27.0.0, arm64, Apple M4 Pro, 14 cores, libprotoc 36.2)
- Linux: 2026-11-14T06:38:46+00:00 (Linux 6.8.0-64-generic container, aarch64, ?, 4 cores, libprotoc 36.2)

| case | op | macOS x baseline | Linux x baseline |
|---|---|---|---|
| xlarge | encode(data) | 7.11x (pb upb) | 8.05x (pb upb) |
| xlarge | decode(bytes) | 1.17x (pb upb) | 1.28x (pb upb) |
| xlarge | to_dict(bytes) | 3.70x (pb upb) | 4.73x (pb upb) |
| xlarge | encode(parsed) | 63.05x (pb upb) | 55.86x (pb upb) |

### java

- macOS: 2026-11-14T18:01:43+00:00 (Darwin 27.0.0, arm64, Apple M4 Pro, 14 cores, libprotoc 36.2)
- Linux: 2026-11-14T06:37:47+00:00 (Linux 6.8.0-64-generic container, aarch64, ?, 4 cores, libprotoc 36.2)

| case | op | macOS x baseline | Linux x baseline |
|---|---|---|---|
| xlarge | encode | 1.09x (protobuf-java) | 0.96x (protobuf-java) |
| xlarge | decode | 1.94x (protobuf-java) | 1.72x (protobuf-java) |

### node

- macOS: 2026-11-14T18:22:24+00:00 (Darwin 27.0.0, arm64, Apple M4 Pro, 14 cores, libprotoc 36.2)
- Linux: 2026-11-14T06:35:33+00:00 (Linux 6.8.0-64-generic container, aarch64, ?, 4 cores, libprotoc 36.2)

| case | op | macOS x baseline | Linux x baseline |
|---|---|---|---|
| xlarge | encode | 2.99x (protobufjs) | 4.91x (protobufjs) |
| xlarge | decode | 2.77x (protobufjs) | 2.37x (protobufjs) |

### ruby

- macOS: 2026-11-14T18:44:34+00:00 (Darwin 27.0.0, arm64, Apple M4 Pro, 14 cores, libprotoc 36.2)
- Linux: 2026-11-14T06:36:19+00:00 (Linux 6.8.0-64-generic container, aarch64, ?, 4 cores, libprotoc 36.2)

| case | op | macOS x baseline | Linux x baseline |
|---|---|---|---|
| xlarge | encode | 1.19x (google-protobuf) | 1.11x (google-protobuf) |
| xlarge | decode | 1.69x (google-protobuf) | 1.89x (google-protobuf) |

### rust

- macOS: 2026-11-14T18:03:19+00:00 (Darwin 27.0.0, arm64, Apple M4 Pro, 14 cores, libprotoc 36.2)
- Linux: 2026-11-14T06:33:27+00:00 (Linux 6.8.0-64-generic container, aarch64, ?, 4 cores, libprotoc 36.2)

skipped 2 row(s): the baseline differs across sides (prost vs protobuf-rust, protobuf-rust vs prost)

| case | op | macOS x baseline | Linux x baseline |
|---|---|---|---|
| xlarge | encode | 1.04x (prost) | 1.49x (prost) |
| xlarge | decode | 1.92x (prost) | 1.10x (prost) |

### lua

- macOS: 2026-11-14T18:31:09+00:00 (Darwin 27.0.0, arm64, Apple M4 Pro, 14 cores, libprotoc 36.2)
- Linux: 2026-11-14T06:36:57+00:00 (Linux 6.8.0-64-generic container, aarch64, ?, 4 cores, libprotoc 36.2)

no comparable row for xlarge

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

Per-language index: [Core](#core-pbw_ctx-vs-plain-vs-generated) · [C structs](#c-structs-vs-protoc-generated-same-matrix3proto) · [Go](#go-thin-model-vs-protobuf-go) · [Python](#python-vs-googleprotobuf-upb) · [Java](#java-vs-protobuf-java) · [Node](#node-vs-protobufjs) · [Ruby](#ruby-c-extension-vs-the-google-protobuf-gem) · [Objective-C](#objective-c-vs-the-protobuf-objc-runtime) · [Kotlin](#kotlin-vs-protoc---kotlin_out-over-protobuf-java) · [PHP](#php-vs-the-official-php-runtime-pure--c-extension) · [Dart](#dart-vs-the-protobuf-dart-package) · [Rust](#rust-vs-prost) · [C#](#c-vs-googleprotobuf) · [Lua](#lua-c-module-tables-vs-lua-protobuf) · [Swift](#swift-swiftpm-static-link-vs-appleswift-protobuf) · [Scala](#scala-java-runtime--dsl-vs-scalapb) · [Elixir](#elixir-nif-over-the-shared-core-vs-elixir-protobuf) · [Zig](#zig-static-link-vs-arwalkzig-protobuf) · [F#](#f-facade-over-the-c-runtime-vs-googleprotobuf) · [Clojure](#clojure-java-runtime--interop-vs-protobuf-java) · [Julia](#julia-dlopen--ccall-vs-protobufjl) · [R](#r-c-extension-named-lists-vs-rprotobuf) · [Erlang](#erlang-pbwire-vs-gpb-vs-gpb-nif). In every table below, ✅ means that cell beats its baseline (≥1.0x), ❌ means it does not.

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
| 132 B | encode | 1311.0 (1.70x ✅) | 1103.6 (1.43x ✅) | 769.7 | 96.9 |
| 132 B | decode | 1348.2 (1.47x ✅) | 1099.3 (1.20x ✅) | 916.6 | 292.3 |
| 169 B | encode | 1070.9 (1.57x ✅) | 913.9 (1.34x ✅) | 680.6 | 91.9 |
| 169 B | decode | 1163.6 (1.45x ✅) | 988.6 (1.23x ✅) | 804.5 | 284.6 |
| 2755 B | encode | 731.1 (2.25x ✅) | 679.3 (2.09x ✅) | 325.1 | 95.5 |
| 2755 B | decode | 756.2 (1.63x ✅) | 741.7 (1.60x ✅) | 464.9 | 254.9 |
| 85933 B | encode | 712.4 (2.19x ✅) | 718.1 (2.22x ✅) | 324.7 | 105.4 |
| 85933 B | decode | 851.2 (1.88x ✅) | 852.6 (1.89x ✅) | 452.4 | 265.4 |

The encode rows are the same new + set + encode task on both sides: a
fresh native struct per iteration (plain stores plus the payload allocations
the generated side makes too, then one codec call, released inside the timer
for pbwire; fill setters, serialize, destructor for generated code;
reflection fill, serialize, delete for DynamicMessage) — formal run
`results/bench-core-20261124T073808Z.json`. The generated baseline builds
fresh per iteration too, so the comparison is the same calling method, not
build-against-re-encode. The pbwire side goes through the generated C struct
(`bench/gen/msg_struct.h`, the shipped thin model) and the compiled struct
codec, never the raw tree API: that is what the fairness contract
(`docs/perf-fairness.md`) requires of a headline.

Threads (169 B, 1 vs 8):

| op | pbwire 1thr (MB/s, x gen) | pbwire 8thr agg (MB/s, x gen) | gen 1thr (MB/s) | gen 8thr agg (MB/s) |
|---|---|---|---|---|
| encode | 990.1 (1.45x ✅) | 7782.9 (1.43x ✅) | 682.1 | 5441.4 |
| decode | 1180.5 (1.45x ✅) | 6934.8 (1.09x ✅) | 816.4 | 6369.1 |

`8thr agg` is the total throughput of all eight threads. The compiled
struct codec and the wire input are shared by all threads; each thread only
adds its own output buffer and decode arena. pbwire scales 7.9x (encode) and
5.9x (decode)
from 1 to 8 threads in that run; generated code reaches 8.0x and 7.8x. Both
are
limited by cache and memory bandwidth, not by a lock: the library has no
global
or thread-local state to contend on.

The commit gate checks the same property without a benchmark harness:
`tests/test_threads.c` prints 1-thread vs 8-thread MB/s for the context API (its
own, smaller message, one run on the same machine):

```
scale ctx decode 1 thread:   3061.6 MB/s (  63.4 ns/op) | 8 threads:   2495.0 MB/s/thread,  19960.0 MB/s aggregate (per-op 1.23x, aggregate 6.52x, 14 cores)
scale ctx encode 1 thread:   5387.4 MB/s (  36.0 ns/op) | 8 threads:   4159.2 MB/s/thread,  33273.9 MB/s aggregate (per-op 1.30x, aggregate 6.18x, 14 cores)
```

It fails when the aggregate throughput collapses below 0.35x of
`min(8, cores)`, which is what a hidden lock or false sharing would cause.

#### `pbwire ctx` vs `pbwire plain`

The two pbwire columns are the same thin-model codec with a different memory
and ownership strategy; the benchmark checks that they produce identical bytes
before timing anything. Plain builds and frees a fresh arena per call, ctx
reuses one arena and the output buffer across calls — which is why the two
decode columns now read almost identically (1.45x vs 1.23x at 169 B is the
per-call arena setup alone), while encode keeps a small ctx edge on the
narrow tiers (buffer reuse) that vanishes once the payload allocations
dominate. Use the plain pattern for one-off
work and long-lived results. The per-value malloc path (`arena == NULL` +
`pbw_struct_free`) stays shipped and correctness-tested, but it is no longer
the published number — `bench-c`'s one-shot rows show what it costs.

Read it as:

- **Against generated code**: the thin model wins every cell — encode
  1.34x-2.25x, decode 1.20x-1.98x — on the same new + set + encode task.
  The single pass against generated code's size-then-write two passes shows
  most on the packed-heavy large tiers, and the reused arena on decode.
- **Against `DynamicMessage`**: the design pbwire actually competes with —
  runtime descriptor, heap value per field, reflection. pbwire is 2.9-3.8x
  faster there (decode) and 6.8-11.4x on encode.
- **The old tree headline** (0.39x-0.58x encode) measured the gateway task —
  new + set + encode through the dynamic API — and retired with the
  fairness contract (`docs/perf-fairness.md`): no headline builds its number
  through the raw tree API any more. The tree path itself is unchanged and
  still fully verified (`make difftest`, conformance, the proxy rows).

#### Why the thin model wins

The headline columns above ride the compiled struct codec over the
generated C struct — the same engine as the C structs section below, not the
dynamic tree. The four changes that put that engine ahead of generated
straight-line code are listed there (compiled layout with precomputed opcodes
and tag bytes, fnum map plus free list on decode, caller-owned buffers,
link-time encode budget); the shared wire primitives (inlined varint/tag
paths with one- and two-byte fast paths, the 16-byte ASCII fast path in
proto3 string validation) come from the core. The dynamic tree path keeps its
own mechanisms (single-allocation decoded tree, arena reuse behind `pbw_ctx`,
dense field-number tables at `pbw_schema_link()`), still fully verified by
`make difftest` and the conformance suite — it is just no longer the
published speed.

#### Streaming and framing

`make bench` also prints a `stream` table for the same payload: deterministic
encode ties the stream encode (no maps in that message, so it is one branch), `size`
(`pbw_message_encoded_size`) costs about 0.8 of an encode because it walks the
same tree without writing, the sink costs one pass over the bytes, and a
delimited feed/next round-trip costs about one encode plus one decode. Ratios
only — absolute numbers move between machines.

### C structs vs `protoc` generated (same `matrix3.proto`)

![C structs vs protoc generated (same matrix3.proto) — MB/s by tier](docs/charts/bench-c-structs-vs-protoc-generated-same-matrix3-proto.svg)

The `pbwire-c-shim` rows are the struct codec reading C memory in place. They
keep their prebuilt native input across iterations under the steady-state
carve-out (the boundary price is their question, not the build price), with
this disclosure instead of a fresh-build timer. The dynamic `pbw_ctx` block
`make bench` prints for the same message is the binding's `pbwire-c-tree`
arm, and `pbwire-c1` stays the one-shot probe.

| payload | op | pbwire-c-shim (MB/s, x gen) | protoc gen (MB/s) |
|---|---|---|---|
| 136 B | encode | 1708.7 (1.29x ✅) | 1320.9 |
| 136 B | decode | 1101.1 (1.55x ✅) | 709.2 |
| 174 B | encode | 1544.2 (1.08x ✅) | 1428.8 |
| 174 B | decode | 1089.8 (1.40x ✅) | 779.7 |
| 2469 B | encode | 1054.7 (1.28x ✅) | 821.8 |
| 2469 B | decode | 579.6 (1.61x ✅) | 359.7 |
| 74913 B | encode | 1101.4 (1.34x ✅) | 819.1 |
| 74913 B | decode | 701.1 (2.30x ✅) | 305.1 |

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
the layout per call and mallocs per value (8.9x / 6.4x slower at 136 B, 5.6x /
4.9x at 174 B, and down to ~1.0x / ~1.9x from 2.5 KB up, where the per-call layout
compile is amortised). It stays in the
output so that trade-off is visible rather than hidden.

Concurrency (same tree, `build/test_concurrency`, 8 threads x 200 iters:
3200 verifications, 0 failures): scale encode 1thr 286.7 MB/s (669.6 ns/op),
8thr 262.9 MB/s/thread, agg 2103.6 MB/s. The C++ mirror
(`build/test_cpp_concurrency`, same engine, no separate bench section) passes
the same way: 8 threads x 200 iters, 4800 verifications, 0 failures; scale
encode 1thr 292.9 MB/s (655.6 ns/op), 8thr agg 1234.2 MB/s (155.6 ns/op).

### Go (thin model vs protobuf-go)

![Go (thin model vs protobuf-go) — MB/s by tier](docs/charts/bench-go-thin-model-vs-protobuf-go.svg)

The shipped engine is the pure-Go table codec in
[`bindings/go/runtime/`](bindings/go/runtime/): no cgo,
no layout tables. `make go-bench` (`bindings/go/benchmarks`, build-tagged
so `go test ./...` stays fast) times the thin generated model —
`Marshal()`/`Unmarshal()` over that engine, native struct in and out —
against `protobuf-go` generated code from the same `matrix3.proto` on all
four tiers. The two generated packages are the committed ones under
`bindings/go/tests`, so `bench.sh` needs Go only: no protoc, no C toolchain,
no descriptor.

The rows below time the same user-visible task, struct in and struct
out, gated on byte-identical output and a field-by-field decode before
timing; the `*-par` rows run the same ops across `GOMAXPROCS` goroutines.
Apple M4 Pro, same process times both sides:

| payload | op | pbwire-go (MB/s, x protobuf-go) | protobuf-go (MB/s) | allocs/op (pbwire / protobuf-go) | B/op (pbwire / protobuf-go) |
|---|---|---|---|---|---|
| 136 B | encode | 869.0 (1.35x ✅) | 645.3 | 1 / 1 | 144 / 144 |
| 136 B | decode | 797.0 (1.42x ✅) | 563.2 | 5 / 7 | 368 / 384 |
| 174 B | encode | 899.4 (1.45x ✅) | 620.5 | 1 / 1 | 176 / 176 |
| 174 B | decode | 612.6 (1.30x ✅) | 471.2 | 10 / 14 | 504 / 488 |
| 2469 B | encode | 934.2 (1.75x ✅) | 532.5 | 1 / 1 | 2688 / 2688 |
| 2469 B | decode | 405.3 (1.42x ✅) | 285.3 | 117 / 325 | 12556 / 15064 |
| 74913 B | encode | 976.4 (1.70x ✅) | 573.4 | 1 / 1 | 81920 / 81920 |
| 74913 B | decode | 362.3 (1.25x ✅) | 290.4 | 3035 / 9037 | 451168 / 472236 |
| 136 B | encode-par | 2424.7 (1.07x ✅) | 2269.5 | 1 / 1 | 144 / 144 |
| 136 B | decode-par | 1290.0 (1.14x ✅) | 1132.4 | 5 / 7 | 368 / 384 |
| 174 B | encode-par | 2389.9 (1.08x ✅) | 2220.5 | 1 / 1 | 176 / 176 |
| 174 B | decode-par | 1079.7 (1.01x ✅) | 1069.1 | 10 / 14 | 504 / 488 |
| 2469 B | encode-par | 2438.4 (1.02x ✅) | 2396.9 | 1 / 1 | 2688 / 2688 |
| 2469 B | decode-par | 536.3 (1.20x ✅) | 445.5 | 117 / 325 | 12556 / 15064 |
| 74913 B | encode-par | 4615.3 (1.14x ✅) | 4035.6 | 1 / 1 | 81920 / 81920 |
| 74913 B | decode-par | 639.8 (0.99x ❌) | 644.0 | 3035 / 9037 | 451168 / 472236 |

Encode is ahead on every tier: the fast path reads scalars, strings and
bytes straight from struct memory with no getter calls and no tree, staging
each row in a stack-local buffer with a single writeback so parallel encodes
no longer pay a GC write barrier per append. Decode stores plain integer-width scalars straight into struct memory
through the same offset columns (no pointers cross, so no write barrier is
skipped); strings, bytes and optionals go through typed unsafe stores with
the barrier intact, slices append straight to the struct header, and only
submessage creation stays on closures. Two-byte varints skip the counting
loop in both tag and value parsing. The decode loop parses plain Fast
scalar fields inline — tag, value and store with no call and no nested
dispatch. Two-byte tags resolve the same way: the first byte indexes the
row and the second verifies it, so repeated and optional fields skip Next
and lookup with no new call on the path. Encode leads 1.35x–1.75x serial
(1.02x–1.14x par); serial decode wins on all four tiers
(1.42x/1.30x/1.42x/1.25x). Both hints size exactly, so B/op lands
in the same Go size class as the baseline on every tier, and
decode takes fewer allocs on every tier. 15 of 16 cells are green; the
one red row is xlarge decode-par 0.99x, within a few percent of
parity on work that flips either way between runs on this shared box,
while the serial twin on identical work wins 1.25x. Figures are
medians of three same-host runs (the box carried other tenants'
load throughout, so single runs flop ±10%); published as measured,
not as a scaling claim. The
pbench gate pins the semantic edge the table cannot show: invalid UTF-8 in
a proto3 string fails on every path, and unknown fields round-trip through
the struct paths byte for byte (`TestGateSemantics`). The dynamic arms
behind the thin model (the per-field `dync` tree, the pure-Go `core` paths)
and the vendored `cshim` candidate are compared in
[`bindings/go/pbench/`](bindings/go/pbench/) (see its README), not here.

### Python vs `google.protobuf` (upb)

![Python vs google.protobuf (upb) — MB/s by tier](docs/charts/bench-python-vs-google-protobuf-upb.svg)

| payload | op | pbwire-py shim (MB/s, x upb) | upb (MB/s) |
|---|---|---|---|
| 136 B | encode(data) | 260.8 (6.92x ✅) | 37.7 |
| 136 B | to_dict(bytes) | 147.9 (4.67x ✅) | 31.7 |
| 174 B | encode(data) | 336.0 (5.69x ✅) | 59.0 |
| 174 B | to_dict(bytes) | 156.6 (5.55x ✅) | 28.2 |
| 2469 B | encode(data) | 514.1 (6.51x ✅) | 79.0 |
| 2469 B | to_dict(bytes) | 194.3 (4.62x ✅) | 42.1 |
| 74913 B | encode(data) | 615.0 (7.18x ✅) | 85.7 |
| 74913 B | to_dict(bytes) | 164.9 (3.75x ✅) | 44.0 |

The `shim` rows are the published comparison: the same native task in one
downcall per op (fresh dict in, `encode(dict)`; `decode(bytes)` plus
`to_dict()` with every field read back). The per-field `tree` arm is compared
in `bindings/python/pbench/README.md`, not here. Formal run `results/bench-python-20261123T052144Z.json`.

Rows are the same user-visible task in both arms:

- **encode(data)** starts from the same dict. upb has no dict entry point, so
  its side is filling a generated message field by field and then serializing;
  that population is most of the ~945 us on the 75 KB tier. pbwire walks the
  dict once and writes the wire directly (the one-downcall `shim` path).
- **to_dict(bytes)** ends with plain Python data on either side: native
  `decode(bytes)` plus `to_dict()`, against `FromString` plus reading every field through
  `ListFields()`. This is the `malloc`-bound row — 3000 sub-messages mean 3000
  dicts, 12000 ints and 3000 strings on either side, and nobody escapes it.
  (`json_format.MessageToDict` is ~4x slower than reading the fields, so the
  harness does not use it.)

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

The pure-Python core port is compared in `bindings/python/pbench/README.md`, not here.

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

| payload | op | pbwire-java-core (MB/s, x protobuf-java) | protobuf-java (MB/s) |
|---|---|---|---|
| 136 B | encode | 648.0 (0.28x ❌) | 2322.4 |
| 136 B | decode | 326.5 (0.51x ❌) | 639.3 |
| 174 B | encode | 651.3 (0.54x ❌) | 1198.5 |
| 174 B | decode | 320.5 (0.56x ❌) | 571.5 |
| 2469 B | encode | 614.5 (0.70x ❌) | 879.8 |
| 2469 B | decode | 312.4 (0.71x ❌) | 440.3 |
| 74913 B | encode | 660.7 (0.72x ❌) | 912.8 |
| 74913 B | decode | 338.2 (0.79x ❌) | 426.5 |

The pbwire column is the committed generated model (`Structtest_Scalars3`)
used as the native object (`toByteArray()`/`parseFrom()`): the same task the
`protobuf-java` baseline runs. The per-field `tree` arm is compared in
`bindings/java/pbench/` (see its README), not here. Formal run `results/bench-java-20261123T052448Z.json`.

The `core` column above is Java's headline: the shipped typed path (0.28x at
136 B, 0.72x at 74 KB encode, 0.51x-0.79x decode): the generated table walks
one `FieldMeta` lambda per field and its decode builds the eager typed tree,
against a generated writer that knows every tag and a decode that
materializes nothing until read. The model stays the ergonomic path. The
`core` decode column is eager. The one-crossing JNI probe
lives in `bindings/java/pbench/` (see its README), not here.

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
big-endian. Against generated `protobuf-java` the per-field work left --
loads, one `switch`, one call, a presence gate and a type dispatch per field
-- is what a generated writer avoids by knowing every tag and value at
compile time, which is why the generated-model `core` encode sits below the
line at every tier: that floor is structural, not tuning.

#### Node vs protobufjs

![Node vs protobufjs — MB/s by tier](docs/charts/bench-node-vs-protobufjs.svg)

| payload | op | pbwire-node-shim (MB/s, x protobufjs) | protobufjs (MB/s) |
|---|---|---|---|---|
| 136 B | encode | 52.0 (0.20x ❌) | 255.1 |
| 136 B | decode | 63.3 (0.15x ❌) | 409.8 |
| 174 B | encode | 55.7 (0.20x ❌) | 273.1 |
| 174 B | decode | 47.3 (0.12x ❌) | 407.7 |
| 2469 B | encode | 54.9 (0.13x ❌) | 429.6 |
| 2469 B | decode | 49.0 (0.11x ❌) | 435.5 |
| 74913 B | encode | 60.1 (0.12x ❌) | 515.6 |
| 74913 B | decode | 54.7 (0.12x ❌) | 437.9 |

The pbwire column is the native-in/native-out full task
(`bindings/node/pbench/`): `shim` runs the native-object task in one
`encodeObject` / `decodeToObject` downcall. The per-field `tree` arm is
compared in `bindings/node/pbench/README.md`, not here. Formal run
`results/bench-node-20261123T053041Z.json`.

Each field of a native-object op is one N-API property read or
write (~60-70 ns) where protobufjs pays a few ns in its own heap.
The verdict (tree handle path stays the documented default, the one-downcall
`shim` is the recommended one-shot path) is in `bindings/node/pbench/README.md`
and the ledger; the old prebuilt-handle `tree` headline is retired. The
pure-TypeScript core port is compared in `bindings/node/pbench/README.md`, not here.

protobufjs is the published baseline, and the harness gates its bytes too.

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

| payload | op | pbwire-ruby-shim (MB/s, x gem) | gem (MB/s) |
|---|---|---|---|
| 136 B | encode | 186.9 (0.27x ❌) | 696.6 |
| 136 B | decode | 140.8 (0.58x ❌) | 242.4 |
| 174 B | encode | 204.8 (0.26x ❌) | 796.3 |
| 174 B | decode | 159.5 (0.60x ❌) | 265.8 |
| 2469 B | encode | 224.7 (0.19x ❌) | 1212.6 |
| 2469 B | decode | 147.0 (0.30x ❌) | 489.8 |
| 74913 B | encode | 252.7 (0.18x ❌) | 1366.1 |
| 74913 B | decode | 163.1 (0.23x ❌) | 702.7 |

Ruby is a C extension over the same core, so this is C extension against C
extension. The first version drove the same C ABI through stdlib Fiddle and lost
every row by an order of magnitude: a downcall costs ~3 µs, so a 136 B encode was
3.3 µs against the core's ~65 ns, and the tiers could only be compared by padding
`str` because the wrapper had no repeated fields. The rewrite removed the
boundary, added the repeated-field payload (so the xlarge tier is 3000 nested
messages, not one 74 KB string), and split the message into two representations.
The pbwire column is the native-in/native-out full task
(`bindings/ruby/pbench/`), formal run
`results/bench-ruby-20261123T053358Z.json`:

- **`shim` (native object in and out).** `type.encode_object(obj)` walks the
generated model inside C in one entry call and fills a pbw tree; `decode_object`
decodes once and materializes the generated model classes in C. It is the
published comparison on both ops (0.18x-0.27x / 0.23x-0.60x vs the gem) and the
simplest -- one extension call removes the Ruby per-field loop -- but it stays
below the gem: the gem's generated object already owns C-backed storage (and
its decode stays lazy), while this row builds a C tree from Ruby ivars and
materializes every model and array. The per-field `tree` arm is compared in
`bindings/ruby/pbench/README.md`, not here.

The old `tree` headline is retired: the published row is the native-object
`shim` arm.

#### Objective-C vs the `protobuf` ObjC runtime

![Objective-C vs the protobuf ObjC runtime — MB/s by tier](docs/charts/bench-objective-c-vs-the-protobuf-objc-runtime.svg)

The `pbwire-objc` rows are the `tree` arm: a fresh `PBWMessage` built per
field per iteration, then encoded (formal run
`results/bench-objc-20261123T061357Z.json`). The gap is the per-iteration build
cost priced in pbench: seventeen
boxed setters plus the `str` copy, and the xlarge tier rebuilds its 74 KB
`str` fixture per iteration).

| payload | op | pbwire-objc (MB/s, x protobuf-objc) | protobuf-objc (MB/s) |
|---|---|---|---|
| 136 B | encode | 59.8 (0.37x ❌) | 159.8 |
| 136 B | decode | 698.9 (2.75x ✅) | 253.7 |
| 174 B | encode | 64.8 (0.32x ❌) | 202.9 |
| 174 B | decode | 886.2 (2.95x ✅) | 300.8 |
| 2469 B | encode | 187.7 (0.07x ❌) | 2779.4 |
| 2469 B | decode | 9274.1 (2.43x ✅) | 3814.1 |
| 74913 B | encode | 214.0 (0.01x ❌) | 38906.2 |
| 74913 B | decode | 33177.3 (1.18x ✅) | 28016.9 |

The direct struct path is the same C code, measured by `make bench-c` (the
`shim` rows); there is no third mechanism to compare here. No second pbwire
path exists here: the ARC wrapper is the single native arm, so this section
keeps its row unchanged.

The ARC wrapper adds one Objective-C object per call and no copy: `encode`
hands
the encoder's own malloc'd buffer to `NSData` with
`dataWithBytesNoCopy:length:freeWhenDone:`, and `decode` wraps the decoder's
arena in one `PBWMessage`. Decode stays ahead of the generated runtime on all
four tiers (2.75x / 2.95x at 136-174 B; the 74 KB tier is 1.18x, the closest
row). The xlarge tier is one 74 KB string, so decode is a memcpy race: the
UTF-8 validation that runs at parse is the one walk over those bytes that is
not a copy, which is why it now uses the 16-byte ASCII scan (see the ledger);
the generated encoder
writes it into an `NSMutableData` of the final size, and the wrapper writes it
into its own buffer, which `NSData` then takes over.

#### Kotlin vs `protoc --kotlin_out` over `protobuf-java`

![Kotlin vs protoc --kotlin_out over protobuf-java — MB/s by tier](docs/charts/bench-kotlin-vs-protoc-kotlin-out-over-protobuf-java.svg)

| payload | op | pbwire-kotlin-core (MB/s, x protobuf-kotlin) | protobuf-kotlin (MB/s) |
|---|---|---|---|
| 136 B | encode | 636.9 (0.28x ❌) | 2295.1 |
| 136 B | decode | 321.8 (0.49x ❌) | 659.1 |
| 174 B | encode | 640.6 (0.51x ❌) | 1250.5 |
| 174 B | decode | 275.9 (0.47x ❌) | 590.6 |
| 2469 B | encode | 608.5 (0.69x ❌) | 875.8 |
| 2469 B | decode | 320.3 (0.68x ❌) | 469.6 |
| 74913 B | encode | 687.1 (0.76x ❌) | 906.8 |
| 74913 B | decode | 352.8 (0.75x ❌) | 472.2 |

Kotlin reuses the Java runtime (`bindings/java/runtime`), so these numbers are
`pbwire-java`'s: one workspace lookup plus one `byte[]` copy per call. `core`
is the same committed generated Java model the Kotlin DSL builds (the
`tests/gen/matrix3.kt` factories are inline sugar over it) and the published
comparison here; it tracks Java's `core` row (0.28x at 136 B, 0.76x at 74 KB
encode, 0.47x-0.75x decode) for the same reason: a generated writer knows
every tag at compile time while the dynamic tree walk derives them, the same
structural floor the Java section explains. The `core` column is eager. The
per-field `tree` arm is compared in `bindings/java/pbench/` (see its README),
not here. The `Message`-wrapper cut (no per-decode
`AtomicBoolean`, no volatile open flag) removes the binding's own per-call
overhead on top, so the narrow tiers track the codec instead of the wrapper.
Formal run
`results/bench-kotlin-20261123T052551Z.json`.

#### PHP vs the official PHP runtime (pure + C extension)

![PHP vs the official PHP runtime (pure + C extension) — MB/s by tier](docs/charts/bench-php-vs-the-official-php-runtime-pure-c-extension.svg)

The native C extension publishes the native-array entry
(`pbwire-php-shim`, one extension entry that walks the array in C) as its
headline row on the same payload. The `ext-ffi` wrapper is the no-compiler
deployment and gets its own row (`pbwire-php-shim-ffi`). The per-field `tree`
arm is compared in `bindings/php/pbench/` (see its README), not here. Formal
run
`results/bench-php-20261123T053652Z.json`: the C-extension pass first, then
the `ext-ffi` pass against the same C-extension baseline, then the `ext-ffi`
pass against the pure-PHP baseline.

| payload | op | pbwire-php-shim (MB/s, x) | protobuf-php-c (MB/s) |
|---|---|---|---|
| 136 B | encode | 186.0 (0.22x ❌) | 852.0 |
| 136 B | decode | 168.2 (0.40x ❌) | 420.9 |
| 174 B | encode | 198.8 (0.21x ❌) | 969.1 |
| 174 B | decode | 175.5 (0.42x ❌) | 420.4 |
| 2469 B | encode | 199.6 (0.15x ❌) | 1329.3 |
| 2469 B | decode | 194.6 (0.37x ❌) | 524.8 |
| 74913 B | encode | 216.0 (0.16x ❌) | 1330.3 |
| 74913 B | decode | 211.3 (0.37x ❌) | 565.1 |


`pbwire-php-shim` is the task a PHP application runs when its
data is an array: one extension entry walks the array in C
(`zend_hash_str_find` per field, recursion) and fills the tree, or materializes
the array on decode. It is the published comparison on both ops (0.15x-0.22x
encode, 0.37x-0.42x decode), but below the official C
extension at every tier because every field still costs a hashtable lookup plus
a C tree store, and the decode materializes the array eagerly.

The `ext-ffi` wrapper runs the same array API through one FFI trampoline per
field (the no-compiler deployment), so its row is interpreter-scale; the
official C extension is the baseline in that same run:

| payload | op | pbwire-php-shim-ffi (MB/s, x) | protobuf-php-c (MB/s) |
|---|---|---|---|
| 136 B | encode | 5.0 | 837.0 |
| 136 B | decode | 5.6 | 404.2 |
| 174 B | encode | 5.2 | 909.4 |
| 174 B | decode | 5.5 | 416.4 |
| 2469 B | encode | 5.0 | 1332.6 |
| 2469 B | decode | 4.5 | 532.3 |
| 74913 B | encode | 5.4 | 1318.4 |
| 74913 B | decode | 4.6 | 562.2 |

The `shim-ffi` column is the same array API through the FFI trampoline (one
FFI call per field), which is why it reads interpreter-scale next to the
C-entry `shim` row above.

The pure-PHP runtime, timed by a second `php -n` run in the same harness, is a
different race; the FFI array entry is the pbwire side there:

| payload | op | pbwire-php-shim-ffi (MB/s) | protobuf-php (MB/s) |
|---|---|---|---|
| 136 B | encode | 5.0 | 3.9 |
| 136 B | decode | 5.7 | 6.5 |
| 174 B | encode | 5.1 | 3.9 |
| 174 B | decode | 5.7 | 6.3 |
| 2469 B | encode | 5.0 | 3.7 |
| 2469 B | decode | 4.4 | 5.3 |
| 74913 B | encode | 5.4 | 3.7 |
| 74913 B | decode | 4.7 | 5.2 |

The pure-PHP runtime spends its time on per-field bookkeeping (~4-6 MB/s flat
across tiers). Against it the FFI-run `shim-ffi` row -- the same array API,
no C extension -- stays above the pure-PHP runtime on encode and near it on
decode; quoting a held-handle number here would be
quoting the C core through FFI, not the interpreter, so the native `shim` row
stays the comparison. Both shim builds (C extension and
`ext-ffi`) produce identical bytes and pass the pbench gate on all four tiers;
the gap between them is the per-field PHP loop plus the FFI trampoline, not a
different codec.

#### Dart vs the `protobuf` Dart package

![Dart vs the protobuf Dart package — MB/s by tier](docs/charts/bench-dart-vs-the-protobuf-dart-package.svg)

| payload | op | pbwire-dart-shim (MB/s, x protobuf-dart) | protobuf-dart (MB/s) |
|---|---|---|---|
| 136 B | encode | 1394.5 (6.59x ✅) | 211.7 |
| 136 B | decode | 1042.0 (5.08x ✅) | 205.0 |
| 174 B | encode | 1270.6 (7.33x ✅) | 173.3 |
| 174 B | decode | 908.9 (5.46x ✅) | 166.6 |
| 2469 B | encode | 1021.4 (8.32x ✅) | 122.7 |
| 2469 B | decode | 669.5 (4.78x ✅) | 140.2 |
| 74913 B | encode | 1125.1 (11.03x ✅) | 102.0 |
| 74913 B | decode | 725.1 (4.64x ✅) | 156.2 |

The pbwire column is the native-in/native-out full task
(`bindings/dart/pbench/`, formal run
`results/bench-dart-20261123T061122Z.json`): `shim` runs it in one FFI call
over the flat `<Message>Layout`. The per-field `tree` arm is compared in
`bindings/dart/pbench/README.md`, not here.

`pbwire-dart-shim` is the shipped native-object path: the
generated `<Message>Layout` is a `dart:ffi` `Struct`, and
`bindings/dart/native/pbw_dart_shim.c` walks that image in place through the C
binding's `pbw_struct` codec -- one FFI call per op, no per-field crossing. It
is above the baseline at every tier (6.6x-11.0x encode, 4.6x-5.5x decode)
because
the codec runs in C and the boundary is paid once; only the `Uint8List` copy of
the result remains. The shim row is the headline for native objects.

The pure-Dart core port is compared in `bindings/dart/pbench/README.md`, not here.


#### Rust vs prost

![Rust vs prost — MB/s by tier](docs/charts/bench-rust-vs-prost.svg)

| payload | op | pbwire-rust-shim (MB/s, x prost) | prost (MB/s) |
|---|---|---|---|---|
| 136 B | encode | 1841.7 (1.40x ✅) | 1317.4 |
| 136 B | decode | 1226.9 (1.35x ✅) | 911.1 |
| 174 B | encode | 1693.8 (1.35x ✅) | 1255.5 |
| 174 B | decode | 1098.5 (1.60x ✅) | 687.7 |
| 2469 B | encode | 1108.6 (0.92x ❌) | 1204.2 |
| 2469 B | decode | 643.3 (1.87x ✅) | 344.0 |
| 74913 B | encode | 1190.7 (0.92x ❌) | 1290.1 |
| 74913 B | decode | 766.0 (1.90x ✅) | 403.9 |

The archive is linked in, so a call is a direct C call with no `dlopen` lookup.
The pbwire column is the native-in/native-out full task (formal run
`results/bench-rust-20261123T054311Z.json`).
`pbwire-rust-shim` is the published comparison: the
generated `#[repr(C)]` mirror read and written by the C struct codec in one
call per op, with the image and its decode arena allocated once. prost, the
published baseline (generated at build time by build.rs, bytes
gated like the rest), is behind the `shim` decode on every cell and behind its
encode on the two small tiers.

The pure-Rust core port is compared in `bindings/rust/pbench/README.md`, not here.

The shim's decode leads prost at every tier (1.35x at 136 B, 1.90x at 74 KB)
and its encode leads at 136/174 B (1.40x/1.35x) and trails at 2469 B/74913 B
(0.92x/0.92x): that is the shared C struct encoder's
per-repeated-element reserve path (`make bench-c` measures the same codec at
1101.4 MB/s on 74913 B), while prost's generated writer is straight-line code
with no per-element reserve. The ledger records the `-O3` A/B attempt on that
cell and why it was rejected (the sibling layout arms compile the same C file
at `-O2`).

`encode_to()` tries the caller's buffer first and only asks for the exact size
(a second tree walk) when the buffer does not fit, which is what keeps the
measured loop at one walk per call. Sizing on every call instead halves the
encode throughput (xlarge 624 MB/s, 0.52x prost): the number worth checking
first if this row ever regresses.

#### C# vs Google.Protobuf

![C# vs Google.Protobuf — MB/s by tier](docs/charts/bench-c-vs-google-protobuf.svg)

| payload | op | pbwire-csharp-shim (MB/s, x protobuf-csharp) | protobuf-csharp (MB/s) |
|---|---|---|---|---|
| 136 B | encode | 1152.8 (2.46x ✅) | 468.6 |
| 136 B | decode | 751.8 (1.56x ✅) | 480.7 |
| 174 B | encode | 972.2 (2.35x ✅) | 413.1 |
| 174 B | decode | 614.0 (1.56x ✅) | 393.8 |
| 2469 B | encode | 623.0 (1.13x ✅) | 549.9 |
| 2469 B | decode | 356.3 (0.75x ❌) | 472.0 |
| 74913 B | encode | 657.6 (1.42x ✅) | 461.9 |
| 74913 B | decode | 370.3 (0.73x ❌) | 506.3 |

The pbwire column is the native-in/native-out full task
(`bindings/csharp/pbench/`, formal run
`results/bench-csharp-20261123T054505Z.json`): `shim` runs it in one P/Invoke
per op. The per-field `tree` arm is compared in
`bindings/csharp/pbench/README.md`, not here.

`pbwire-csharp-shim` is the shipped native-object path:
the generated model is copied into one contiguous unmanaged image per op and
**one** `P/Invoke` (`bindings/csharp/native/pbw_cs_shim.c`, built by
`make csharp-shim-native`) walks it through the C layout codec, so no field
crosses the boundary. It is above Google.Protobuf on every encode tier
(1.13x-2.46x) and on the two small decode tiers (1.56x). The 2469 B and
74913 B decode rows are below the baseline (0.75x, 0.73x): the C decode itself
is faster than the reference parser, but materialising 3000 ints, 3000 strings,
3000 sub-messages and 3000 bools into managed objects costs more than the
reference parser's whole run.

The native `shim` rows land at ~660 MB/s encode and ~370 MB/s decode on the
xlarge tier: by 2.5 KB the per-call boundary is invisible and the C layout
codec plus the managed materialisation are what is being measured. That is the
point of the schema-at-runtime design -- the binding owns ergonomics, the core
owns the format.

The pure-C# core port is compared in `bindings/csharp/pbench/README.md`, not here.

### Lua (C module, tables) vs `lua-protobuf`

![Lua (C module, tables) vs lua-protobuf — MB/s by tier](docs/charts/bench-lua-c-module-tables-vs-lua-protobuf.svg)

| payload | op | pbwire-lua-shim (MB/s, x lua-protobuf) | lua-protobuf (MB/s) |
|---|---|---|---|
| 136 B | encode | 106.0 (1.08x ✅) | 98.6 |
| 136 B | decode | 151.7 (1.63x ✅) | 93.1 |
| 174 B | encode | 106.6 (1.10x ✅) | 97.2 |
| 174 B | decode | 148.7 (1.61x ✅) | 92.6 |
| 2469 B | encode | 101.2 (1.07x ✅) | 94.4 |
| 2469 B | decode | 135.3 (1.61x ✅) | 83.9 |
| 74913 B | encode | 99.4 (0.99x ❌) | 100.1 |
| 74913 B | decode | 123.0 (1.42x ✅) | 86.9 |

Same class as Ruby/R: a C extension over the core (plain tables in/out
through one transient tree per call, statically linked), schema from
a `FileDescriptorSet` at run time. The pbwire column is the native-in/native-out
full task (`bindings/lua/pbench/`), formal run
`results/bench-lua-20261123T054814Z.json`: `shim` runs the plain-table task in
one C entry. The per-field `tree` arm is compared in
`bindings/lua/pbench/README.md`, not here.

One published row (`bindings/lua/pbench/README.md` carries the selection):

- **`pbwire-lua-shim`** is the task a Lua application runs: the plain table
in, wire bytes out. One C entry (`schema:encode`) walks the table against the
schema's prepared field table and writes straight from it into the wire
buffer (one pass over schema fields in descriptor order, no intermediate
tree, no per-value allocation); the decode is the same one entry in reverse
and materializes the fresh table eagerly. It is the published comparison and
beats lua-protobuf on every cell but one
(1.07x-1.10x / 1.42x-1.63x vs lua-protobuf). It is also the already-shipped
path, so the selection places nothing new in `runtime/`.

The xlarge `shim` encode cell (0.99x) is a tie, not a finding: it flips
between runs (1.07x-1.10x at the other tiers, 1.05x-1.06x for xlarge in the
provisional `compare.lua` runs) inside the host's ±10% run-to-run spread, and
both sides share the same fresh-table construction inside the timer. Decode
wins at every tier because it is one validating parse straight into tables
with no tree kept. Both directions walk one prepared per-descriptor cache,
so a decoded table costs no field-name interning, no descriptor lookup and
no hash growth per field (each table is created with its exact hash size,
and an absent repeated field's empty table is written in the same pass). The
gate is tier size (the bytes are canonical and match `protoc --encode` on
the small tier) plus a full-field expectation check, with tree/shim byte
agreement and the unknown-field-999 vectors (top level and nested) on top;
the baseline only has to agree on size and decode, because `lua-protobuf`
writes nested messages in table hash order, so raw baseline bytes are not
comparable.

### Erlang: `pbwire` vs gpb vs gpb NIF

![Erlang: pbwire vs gpb vs gpb NIF — MB/s by tier](docs/charts/bench-erlang-pbwire-vs-gpb-vs-gpb-nif.svg)

`matrix3.proto`, all four tiers, best of three samples per op. Same payload / op /
MB/s layout as every other table: the `pbwire-erlang` column carries the ratio
against the faster of gpb / gpb NIF per op (gpb is the byte reference either
way), the other columns are the two baselines side by side. Formal run
`results/bench-erlang-20261123T055756Z.json`. The `pbwire-erlang`
rows are the single NIF arm with a fresh record built inside the timed closure
per iteration (native-in full task); gpb, the pure-Erlang codec,
**is** the `core` comparison, so a second BEAM port would measure our code
against the competitor rather than a decision. No second pbwire path exists
here, so this single-arm section is unchanged.

| payload | op | pbwire-erlang (MB/s, x best) | gpb (MB/s) | gpb-nif (MB/s) |
|---|---|---|---|---|
| 136 B | encode | 449.9 (2.96x ✅) | 151.8 | 85.7 |
| 136 B | decode | 516.0 (2.18x ✅) | 236.3 | 100.3 |
| 174 B | encode | 451.1 (3.26x ✅) | 138.5 | 102.6 |
| 174 B | decode | 508.5 (2.31x ✅) | 220.4 | 119.4 |
| 2469 B | encode | 306.4 (2.79x ✅) | 109.7 | 77.7 |
| 2469 B | decode | 342.2 (1.58x ✅) | 216.7 | 191.8 |
| 74913 B | encode | 328.5 (3.00x ✅) | 105.2 | 109.4 |
| 74913 B | decode | 356.1 (1.52x ✅) | 233.7 | 216.0 |

### Swift (SwiftPM, static link) vs `apple/swift-protobuf`

![Swift (SwiftPM, static link) vs apple/swift-protobuf — MB/s by tier](docs/charts/bench-swift-swiftpm-static-link-vs-apple-swift-protobuf.svg)

| payload | op | pbwire-swift-shim (MB/s, x swift-protobuf) | swift-protobuf (MB/s) |
|---|---|---|---|
| 136 B | encode | 861.4 (3.39x ✅) | 254.3 |
| 136 B | decode | 533.6 (2.15x ✅) | 248.6 |
| 174 B | encode | 698.6 (2.14x ✅) | 326.9 |
| 174 B | decode | 521.4 (2.58x ✅) | 202.0 |
| 2469 B | encode | 1047.8 (3.88x ✅) | 270.4 |
| 2469 B | decode | 528.1 (2.75x ✅) | 192.3 |
| 74913 B | encode | 923.2 (3.22x ✅) | 286.4 |
| 74913 B | decode | 568.3 (2.49x ✅) | 227.9 |

The pbwire column is the native-in/native-out full task (formal run
`results/bench-swift-20261123T055003Z.json`): `shim` runs it in one
`encodeInPlace`/`decodeInPlace` call. The per-field `tree` arm is compared in
`bindings/swift/pbench/README.md`, not here. Pairing note: the formal run prints the four native
rows without the baseline in its first case set and the native row against
`swift-protobuf` in its second; the ratios above pair each MB/s with
the same run's own baseline MB/s (e.g. xlarge shim 923.2 vs 286.4 = 3.22x).

FFI host like Objective-C/Dart: `FileDescriptorSet` at run time, C ABI calls
only, no wire code, no plugin. SwiftPM links `build/libpbwire.a` via
`unsafeFlags` (so plain `swift test` works offline); the baseline is a
separate package using swift-protobuf's own build plugin, so no generated
code is checked in. The gate is byte parity per tier plus the pinned
`matrix3.bin` oracle.

`pbwire-swift-shim` is the published comparison: it times the generated `@frozen <Message>Repr` mirror with one
`encodeInPlace`/`decodeInPlace` call into `bindings/c/runtime/pbw_struct.c`
(compiled into the package's `CPbwire` target), so no field and no ARC crosses
per field. The shim answers
the native-object question at 2.1-3.9x encode and 2.2-2.8x decode.

The pure-Swift core port is compared in `bindings/swift/pbench/README.md`, not here.

### Scala (Java runtime + DSL) vs ScalaPB

![Scala (Java runtime + DSL) vs ScalaPB — MB/s by tier](docs/charts/bench-scala-java-runtime-dsl-vs-scalapb.svg)

| payload | op | pbwire-scala-core (MB/s, x scalapb) | scalapb (MB/s) |
|---|---|---|---|
| 136 B | encode | 570.8 (0.47x ❌) | 1209.5 |
| 136 B | decode | 339.7 (0.41x ❌) | 824.8 |
| 174 B | encode | 493.1 (0.47x ❌) | 1055.2 |
| 174 B | decode | 235.0 (0.32x ❌) | 726.2 |
| 2469 B | encode | 306.1 (0.38x ❌) | 796.1 |
| 2469 B | decode | 161.5 (0.25x ❌) | 641.9 |
| 74913 B | encode | 332.0 (0.39x ❌) | 842.5 |
| 74913 B | decode | 175.5 (0.25x ❌) | 701.3 |

Same class as Kotlin: the JVM binding reuses the Java runtime and adds only
an idiomatic DSL — no second codec, no JNI, schema from a `FileDescriptorSet`
at run time. The baseline is `scalapbc`-generated code (needs `cs install
scalapbc` once); the jars land in `benchmarks/deps`. Byte parity per tier
before timing, like every other row. `core` is the committed generated
`matrix3Gen` case class and the published comparison here; its `encode`/`decode`
fill and load a `Message`
field by field by name, so it is the slowest JVM `core` row (0.47x at 136 B,
0.39x at 74 KB encode, 0.25x-0.41x decode): the translation is the cost, not
the wire engine. The per-field `tree` arm is compared in
`bindings/scala/pbench/` (see its README), not here. Formal run
`results/bench-scala-20261123T052700Z.json`.

### Elixir (NIF over the shared core) vs `elixir-protobuf`

![Elixir (NIF over the shared core) vs elixir-protobuf — MB/s by tier](docs/charts/bench-elixir-nif-over-the-shared-core-vs-elixir-protobuf.svg)

| payload | op | pbwire-elixir (MB/s, x elixir-protobuf) | elixir-protobuf (MB/s) |
|---|---|---|---|
| 136 B | encode | 152.2 (3.18x ✅) | 47.9 |
| 136 B | decode | 106.8 (1.72x ✅) | 62.2 |
| 174 B | encode | 152.6 (3.06x ✅) | 49.9 |
| 174 B | decode | 110.7 (1.76x ✅) | 63.0 |
| 2469 B | encode | 143.2 (2.73x ✅) | 52.5 |
| 2469 B | decode | 113.8 (1.99x ✅) | 57.2 |
| 74913 B | encode | 147.9 (2.84x ✅) | 52.0 |
| 74913 B | decode | 108.1 (1.84x ✅) | 58.9 |

Same class as Erlang: the BEAM binding reuses the published `:pbwire` Hex
NIF (the same C core as `bindings/erlang`) and owns only the struct↔tuple
translation; `elixir-protobuf` is the schema plugin, like gpb is for Erlang, and
its pure-Elixir rows **are** the `core` comparison for this binding. No
second pbwire path exists here, so this single-arm section is unchanged.
Every encode is byte-identical to `Protobuf.encode/1` and every decode reads
back field-for-field equal to the input struct before timing; each encode
iteration builds a fresh struct inside the timed closure (native-in full task). Formal run `results/bench-elixir-20261123T055715Z.json`. Known
deliberate divergences from the oracle: NaN bit identity, unknown-field drop
(the NIF tuple form has no storage), raise on missing `required`.

The struct build is one pass over the NIF's record: the per-position field
tables are built once per module at schema load, so a decode indexes them
instead of walking two lists through `Tuple.to_list` + `Enum.zip` + `Enum.map`
+ a list concat. That is what lifted the xlarge decode row off the 0.99x floor
(see the ledger).

### Zig (static link) vs Arwalk/zig-protobuf

![Zig (static link) vs Arwalk/zig-protobuf — MB/s by tier](docs/charts/bench-zig-static-link-vs-arwalk-zig-protobuf.svg)

| payload | op | pbwire-zig-shim (MB/s, x zig-protobuf) | zig-protobuf (MB/s) |
|---|---|---|---|
| 136 B | encode | 1837.5 (1.39x ✅) | 1323.1 |
| 136 B | decode | 1260.6 (1.46x ✅) | 861.5 |
| 174 B | encode | 1639.5 (1.51x ✅) | 1083.0 |
| 174 B | decode | 1072.7 (1.44x ✅) | 743.6 |
| 2469 B | encode | 1140.8 (1.30x ✅) | 880.7 |
| 2469 B | decode | 696.0 (1.12x ✅) | 623.3 |
| 74913 B | encode | 1186.6 (1.38x ✅) | 862.2 |
| 74913 B | decode | 768.7 (1.37x ✅) | 559.6 |

The pbwire column is the native-in/native-out full task (formal run
`results/bench-zig-20261123T055513Z.json`): `shim` runs it in one
`encodeInPlace`/`decodeInPlace` call. The per-field `tree` arm is compared in
`bindings/zig/pbench/README.md`, not here.

Same class as Rust: the core sources compile into a static archive that links
into one self-contained binary, schema from a `FileDescriptorSet` at run
time. The baseline is Arwalk/zig-protobuf 7.0.0 generated code from the same
`matrix3.proto` (tarball pinned in `build.zig.zon`, fetched on first run and
cached; generation runs the system `protoc` against their plugin, and the
`test` / plain `bench` steps never resolve the dependency, so they stay
offline-safe — `bench.sh` runs pbwire-only by default, `--baseline` (what
`make zig-bench` passes) runs the comparison). Bytes are gated equal on all
four tiers before timing. `pbwire-zig-shim` is the published comparison: it
times the generated
`extern struct <Message>Mirror` with one `encodeInPlace`/`decodeInPlace` call
into `bindings/c/runtime/pbw_struct.c` (compiled into the same archive), so no
field crosses per field. The shim is above the baseline on every cell.

The pure-Zig core port is compared in `bindings/zig/pbench/README.md`, not here.

The baseline's decode is structural:
it materializes every field with its own allocation (which is also what the
small tiers time most of), while the binding decodes into one arena; the lead
shrinks as the payload grows and per-byte work takes over. At
`-Doptimize=ReleaseFast` (what `bench.sh` runs) both sides sit near the core's
pace; the default debug build is ~20x slower and is not timed.

### F# (facade over the C# runtime) vs Google.Protobuf

![F# (facade over the C# runtime) vs Google.Protobuf — MB/s by tier](docs/charts/bench-f-facade-over-the-c-runtime-vs-google-protobuf.svg)

| payload | op | pbwire-fsharp-shim (MB/s, x) | Google.Protobuf (MB/s) |
|---|---|---|---|
| 136 B | encode | 1172.1 (2.52x ✅) | 464.5 |
| 136 B | decode | 738.3 (1.47x ✅) | 501.4 |
| 174 B | encode | 932.4 (3.05x ✅) | 306.2 |
| 174 B | decode | 533.5 (1.38x ✅) | 387.9 |
| 2469 B | encode | 632.2 (1.11x ✅) | 569.2 |
| 2469 B | decode | 365.1 (0.75x ❌) | 486.1 |
| 74913 B | encode | 657.0 (1.34x ✅) | 490.0 |
| 74913 B | decode | 362.8 (0.70x ❌) | 518.9 |

Same class as Kotlin: F# reuses the C# binding's project via
`ProjectReference` and adds only an F#-idiomatic facade — no second runtime,
no packages, schema from a `FileDescriptorSet` at run time. The baseline is
`protoc`-generated C# compiled in a sibling project (F# cannot compile
`.cs`). Byte parity per tier before timing. Both rows come from the C#
runtime: `pbwire-fsharp-shim` is the C# generated model's native-object entry
(`bindings/csharp/pbench/Shim.cs`), i.e. the same one-call C reader over the
flat image, so F# has no separate port and no second wire implementation.
Formal run `results/bench-fsharp-20261123T054655Z.json`.

`pbwire-fsharp-shim` is the published comparison: the
generated model is copied into one contiguous unmanaged image per op and one
C-reader call walks it, so no field crosses the boundary. It is above
Google.Protobuf on every encode tier (1.11x-3.05x) and on the two small
decode tiers (1.38x-1.47x). The 2469 B and 74913 B decode rows are below the
baseline (0.75x, 0.70x): the C decode itself is faster than the reference
parser, but materialising 3000 ints, 3000 strings, 3000 sub-messages and 3000
bools into managed objects costs more than the reference parser's whole run.
These F# rows are native-in/native-out rows.

### Clojure (Java runtime + interop) vs `protobuf-java`

![Clojure (Java runtime + interop) vs protobuf-java — MB/s by tier](docs/charts/bench-clojure-java-runtime-interop-vs-protobuf-java.svg)

| payload | op | pbwire-clj-core (MB/s, x protobuf-java) | protobuf-java (MB/s) |
|---|---|---|---|
| 136 B | encode | 1679.7 (0.70x ❌) | 2408.8 |
| 136 B | decode | 887.3 (1.33x ✅) | 668.7 |
| 174 B | encode | 1399.0 (1.07x ✅) | 1312.3 |
| 174 B | decode | 795.8 (1.33x ✅) | 596.3 |
| 2469 B | encode | 950.1 (1.09x ✅) | 874.9 |
| 2469 B | decode | 740.3 (1.61x ✅) | 460.0 |
| 74913 B | encode | 1084.8 (1.18x ✅) | 916.4 |
| 74913 B | decode | 804.8 (1.77x ✅) | 454.6 |

`core` is the mutable-message fast path and the published comparison: the
prebuilt `Message` (fill once, encode many; decode plus close), the generated
model's native object with no map walk. The persistent-map API stays for
convenience and is gated for parity, but hot loops use the mutable path — the
same split the README documents (maps for convenience, the mutable path for
hot loops) and Lua documents (table vs builder). The per-field `tree` map-API
arm is compared in `bindings/clojure/pbench/` (see its README), not here.
Formal run
`results/bench-clojure-20261123T052803Z.json`.

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

| payload | op | pbwire-julia-shim (MB/s, x ProtoBuf.jl) | pbwire-julia dec+ctx (MB/s, x ProtoBuf.jl) | ProtoBuf.jl (MB/s) |
|---|---|---|---|---|
| 136 B | encode | 1389.4 (3.44x ✅) | -- | 404.0 |
| 136 B | decode | 1134.4 (0.95x ❌) | 1174.2 (0.98x ❌) | 1197.9 |
| 174 B | encode | 1270.4 (3.90x ✅) | -- | 325.6 |
| 174 B | decode | 1006.5 (1.42x ✅) | 941.3 (1.33x ✅) | 708.2 |
| 2469 B | encode | 1064.1 (8.18x ✅) | -- | 130.1 |
| 2469 B | decode | 699.1 (1.07x ✅) | 698.5 (1.07x ✅) | 650.5 |
| 74913 B | encode | 1170.8 (94.42x ✅) | -- | 12.4 |
| 74913 B | decode | 766.7 (1.07x ✅) | 750.3 (1.04x ✅) | 719.4 |

The `shim` column is the native-in/native-out full task (formal run
`results/bench-julia-20261123T055825Z.json`): one `ccall` over the generated
layout image per op. The per-field `tree` arm is compared in
`bindings/julia/pbench/README.md`, not here. The xlarge encode column is a baseline
collapse (ProtoBuf.jl's generated encoder falls to 12.4 MB/s there), so the
shim ratio spikes (94.42x); the decode row is the honest comparison (1.07x).

FFI host like Dart/C#: `Libdl.dlopen` + `ccall` into the C ABI (function
pointers resolved once via `dlsym`, since recent Julia needs a compile-time
library name), `Dict`-based messages convert through the C ABI, no wire
code, no `.proto` parser. The baseline is `protojl`-generated code
(`benchmarks/oracle/structtest/matrix3_pb.jl`, produced by
`protojl("matrix3.proto", "<proto dir>", "benchmarks/oracle")`). Its encode
collapses with size (pure-Julia per-field appends).

`pbwire-julia-shim` is the published comparison: a generated `mutable struct Scalars3Layout` is the object,
and `bindings/julia/native/pbw_julia_shim.c` walks it in place through
`pbw_struct_codec_*`, one `ccall` per op, with the image and caller-memory
arena allocated once. The shim beats the baseline on all four encode tiers
(3.44x at 136 B, 94.42x at 74 KB on the baseline collapse) and on
medium/large/xlarge decode (1.42x / 1.07x / 1.07x). The 136 B decode stays
below (0.95x `shim`): the small
tier is the C struct decode plus the boundary, not an arena cost (the plan
removes the per-call `malloc`/`free` pair, and Stage 6 recorded two attempts
that did not beat it). The decode row remains the honest headline; the xlarge
encode column quotes the baseline collapse, not the codec.

The `dec+ctx` rows are the binding's other decode entry point:
`decode_in(ctx, ty, wire)` decodes into a reusable `Ctx`, so steady-state
decoding stops allocating (the C core's `pbw_decode_ctx`, which resets a sealed
arena on entry, so no separate reset call is needed). It matches the plain
path on the 136 B tier, leads by 11% on 174 B and 1-8% on the larger tiers,
where the arena reset now keeps the used chunk prefix instead of re-mallocing
it every decode. The tree it returns is read-only and dies at the
next `decode_in`/`reset!` on that context.

The pure-Julia core port is compared in `bindings/julia/pbench/README.md`, not here.

### R (C extension, named lists) vs RProtoBuf

![R (C extension, named lists) vs RProtoBuf — MB/s by tier](docs/charts/bench-r-c-extension-named-lists-vs-rprotobuf.svg)

| payload | op | pbwire-r-shim (MB/s, x RProtoBuf) | RProtoBuf (MB/s) |
|---|---|---|---|
| 136 B | encode | 63.4 (23.48x ✅) | 2.7 |
| 136 B | decode | 82.2 (29.36x ✅) | 2.8 |
| 174 B | encode | 102.7 (35.41x ✅) | 2.9 |
| 174 B | decode | 98.9 (32.97x ✅) | 3.0 |
| 2469 B | encode | 137.9 (53.04x ✅) | 2.6 |
| 2469 B | decode | 127.6 (41.16x ✅) | 3.1 |
| 74913 B | encode | 155.7 (51.90x ✅) | 3.0 |
| 74913 B | decode | 153.7 (53.00x ✅) | 2.9 |

Same class as Lua/Ruby: a C extension over the core (named lists in/out
through one transient tree per call, statically linked), schema from
a `FileDescriptorSet` at run time. The pbwire column is the native-in/native-out
full task (formal run `results/bench-r-20261123T060035Z.json`): `shim` runs the
named-list task in one `.Call`. The per-field `tree` arm is compared in
`bindings/r/pbench/README.md`, not here.

Same class as Lua/Ruby: a C extension over the core (named lists in/out
through one transient tree per call, statically linked), schema from
a `FileDescriptorSet` at run time. The wrappers scope every `.Call` to
`PACKAGE = "pbwire"`: a bare entry-point name re-searches every loaded DLL
per call, which costs 30+ us once a native-heavy package (RProtoBuf itself,
Rcpp) shares the session; the registered-table hit is ~100 ns. Bytes are
gated equal on all four tiers before timing (RProtoBuf takes the `u64` max as
its decimal string — R numerics cannot spell it — everything else is the same
R values pbwire encodes), and the materialized decode is gated identical too,
plus a full decode round trip.

One published row (`bindings/r/pbench/README.md`):

- **`pbwire-r-shim`** is the task an R application runs: the generated model's
  named list in, wire bytes out. One `.Call` hands the list to C, which walks it
  and fills the tree inside C; the decode is the same one call in reverse and
  returns the fully materialized named list. The type is resolved once into a
  schema-owned message number, so the call does not look a name up per message.
  It is the published comparison on both ops
  (23.5x-53.0x / 29.4x-53.0x vs RProtoBuf).

Encode is no contest at any tier: setting 3000 nested messages field by field
through S4 dispatch costs RProtoBuf ~27 ms where the C core walks the list once.
Decode is compared as the same user-visible task: `RProtoBuf::read` parses in C++
and materializes fields lazily on `$`, while `pbwire_decode` builds the full
named-list tree eagerly, so the baseline side reads every field into the same
plain-list shape (the table's `decode` rows). A bare `read()` alone is a few
times faster than `pbwire_decode`, but it hands back a Message, not R data; once
both sides produce plain lists, the list path leads 52x / 53x (encode / decode)
at xlarge. Two R spellings
to know: `INT32_MIN` reads back as `NA` (it *is* `NA_INTEGER`), and doubles past
2^63 clamp to the int/uint limits — both documented in the binding README.

## Embedded: nanopb, same message, both sides

The embedded facade has its own paired harness, because nanopb is the baseline
that matters on that target: one fixed-size message (`bindings/embedded/benchmarks/msg.proto`),
four tiers, pbwire through `pbwe_open_mem` (caller-memory arena) against
nanopb's generated static struct. Encoders produce byte-identical wire on every
tier (asserted in the run).

| tier | bytes | pbwire enc | nanopb enc | pbwire dec | nanopb dec |
|---|---|---|---|---|---|
| x0 | 52 | 125.1 ns | 141.0 ns | 39.7 ns | 312.3 ns |
| x1 | 55 | 130.9 ns | 152.5 ns | 48.2 ns | 300.1 ns |
| x100 | 154 | 1514.8 ns | 863.4 ns | 345.2 ns | 758.6 ns |
| x3000 | 5928 | 46718.5 ns | 25684.5 ns | 10721.0 ns | 19117.0 ns |

The `pbwire` encode column is the `tree` full task (build + fill + encode
fresh per iteration). Re-encoding the held tree costs less than the fresh build
at every tier, which bounds the encoder alone; the headline pays the
per-iteration build.

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
make go-bench     # Go thin model (pure-Go engine) vs protobuf-go generated code (needs Go only)
make java-bench   # pure-Java codec vs protobuf-java (fetches the jar from Maven Central once)
make node-bench   # N-API addon vs protobufjs (npm installs into the harness)
make ruby-bench   # Ruby C extension vs the google-protobuf gem (`RUBY=...` picks the interpreter)
make objc-bench   # ARC wrapper vs the protobuf ObjC runtime (needs `make conformance-setup`)
make kotlin-bench # Kotlin DSL vs protoc --kotlin_out over protobuf-java (fetches two jars once)
make php-bench    # ext-ffi binding vs the official PHP runtime (needs `make conformance-setup`)
make dart-bench   # dart:ffi binding vs the protobuf Dart package (needs `dart pub global activate protoc_plugin`)
make rust-bench   # static-link binding vs prost (needs `cargo install protobuf-codegen`)
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
output reformatted the same way, pasting only the shipped-headline rows
(`shim`, `shim-ffi` on PHP, `-par` rows on Go, `dec+ctx` rows on Julia, and
the generated-model `core` on the JVM bindings and Clojure; the single native
arm on Objective-C, Erlang and Elixir; the thin-model `pbwire`/`pbwire c`
rows on the core, `tree` on C++ and Embedded).
Pbench-only arms (the per-field `tree` task, the prebuilt `proxy` task, pure
host-language `core` ports, Go's per-field `dync` tree, the vendored `cshim`
candidate, the one-crossing JNI ceiling) are compared
in `bindings/<lang>/pbench/README.md` and are never pasted here; the
Erlang table is pasted from its own record (`results/bench-erlang-*.json`)
like every other binding. The
harnesses may time extra baselines where they are already wired (Rust's
build.rs generates prost from crates.io, C# restores its NuGet packages,
Node npm-installs protobufjs, and `make php-bench`
runs the C-extension rows plus a `php -n` pure-PHP pass when the extension is
present); the tables above publish the most-popular baseline only (PHP and
Erlang keep their same-package deployment variants). Rebuild the
at-a-glance table from those rows afterwards — it only copies the xlarge line of
each headline arm — then re-render the per-language charts (`python3 scripts/bench-charts.py`
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

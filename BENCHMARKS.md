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
  - [Core](#core-pbw_ctx-vs-plain-vs-generated) · [C structs](#c-structs-vs-protoc-generated-same-matrix3proto) · [Go](#go-thin-model-vs-protobuf-go) · [Python](#python-vs-googleprotobuf-upb)
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
(`MB/s = bytes / ns/op * 1000`). The comma and the iteration count are
optional: a harness that calibrates per op (Go, Python) has no single count
for a case, and `scripts/bench-record.py` accepts the shorter header.
`make bench-c` adds one more block per case
(`pbwire-c1`: the C binding's one-shot API, which compiles the layout on every
call) after the standard four rows; only the standard block is aggregated
below.

Rows are named by the mechanism they time: `tree` (a native object in and out
through the shipped per-field API, no prebuilt tree before the timer), `shim`
(C reads the host's memory through a layout table, one crossing, no
intermediate tree), `core` (a host-language port of the core, native object in
and out) and `proxy` (the prebuilt task: re-encode a held handle,
decode-and-free with no materialization; published, never the headline). The
`tree`, `shim` and `core` rows all time the same native-in/native-out task.
`n/a` means the arm is not built for that binding; the reason is in
`bindings/<lang>/pbench/README.md`.

Timer boundary (all bindings): a fresh native object per iteration, allocation
and `free`/`close` inside the timer, deep materialization of every
gate-checked field. Plain-data fixtures may be reused across iterations
(fixture-reuse ruling, `docs/perf-attempts.md` Python worker D3); reused
encode buffers stay allowed only where the harness already does it and says
so. The `shim`/`core` encode arms may reuse app-owned native input across
iterations (the steady-state carve-out: the boundary price is their question,
not the build price); the `tree` arm always builds fresh.

The Python harness names its ops by the task as well as the representation
(`encode(data)`, `decode(bytes)`, `to_dict(bytes)`, `encode(parsed)`): a
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
> in the ledger). The tables below are the post-change runs (Dart excepted:
> its toolchain is missing on this host); the remaining decode gaps are baseline
> strength, not validator cost.

Last full refresh: 2026-11-14, 17:54-18:58 UTC (`make bench`, `make bench-c`,
`make python-bench`, ... on this tree, one harness at a time, the whole matrix
in ~65 minutes). Go-parity re-time 2026-11-21/22 (issue 41 plan): every binding
with a `pbench/` re-times its `tree` arm as the native-in/native-out full task
(fresh native object per iteration, alloc and free inside the timer, deep
materialization of every gate-checked field) and publishes the old prebuilt
task as the `proxy` row (relabelled, same-run timing only, never re-measured
as a separate run); `shim`/`core` rows keep their timers except where the task
changed. The re-timed runs are `results/bench-<lang>-2026112*.json` (one
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
was re-run (Ruby, Objective-C, Kotlin, Elixir, Julia) the pasted run is the one
whose own baseline did not collapse under host load (a run whose fastest
baseline is under 85% of that binding's median baseline is treated as
contaminated and skipped). The host carried someone else's background load
throughout (load average 5-14 on 14 cores, ~0.4-1.0 normalised), so absolute
MB/s reads 10-20% low against a quiet machine while `x baseline` reproduces
(run-to-run variation is ±10% per cell, more on the JVM narrow tiers).
Erlang is the exception: `rebar3` is missing on this host, so `make
erlang-bench` cannot build the NIF suite and the Erlang table is copied from its
own README. Dart ran once `dart pub global activate protoc_plugin` provided
`protoc-gen-dart`.

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
| core (dynamic) | `make bench` (`bench/bench.cc`) | `protoc` C++ generated + DynamicMessage | all 4 | 1.56x ✅ / 1.80x ✅ |
| C structs | `make bench-c` (`bindings/c/benchmarks/bench_struct.cc`) | `protoc` C++ generated from the same `matrix3.proto` | all 4 | 1.32x ✅ / 1.96x ✅ `shim` |
| Erlang/OTP NIF | `make benchmarks` in `bindings/erlang/benchmarks` | gpb (pure Erlang), with gpb NIF (libprotobuf) as the second baseline, same `matrix3.proto` | all 4 | 3.91x / 1.60x vs best-of-two ✅ |
| Python (CPython ext) | `make python-bench` (`bindings/python/benchmarks/bench.py`) | `google.protobuf` (upb C extension) on the same `matrix3.proto` | all 4 | 7.19x ✅ `tree encode(data)` / 3.65x ✅ `shim to_dict` (`tree to_dict` 1.57x) |
| Python (pure-Python `core`) | `make python-bench` | `google.protobuf`'s own pure-Python runtime (`PROTOCOL_BUFFERS_PYTHON_IMPLEMENTATION=python`), no C extension | all 4 | 1.25x ✅ `encode(parsed)` / 1.05x ✅ decode at 74 KB; `encode(data)` 0.69x (the pure-Python baseline has no dict entry point) |
| C++ | same engine as C (`bindings/cpp` mirrors the C structs; `make bench-c` is the harness) | `protoc` C++ generated from the same `matrix3.proto` | all 4 | 1.33x ✅ / 2.00x ✅ |
| Go (thin model, shim runtime) | `make go-bench` (`bindings/go/benchmarks`) | `protobuf-go` generated code from the same `matrix3.proto` | all 4 | 1.92x ✅ / 1.60x ✅ `core` (`tree` 0.05x / 0.12x, pbench full-task row) |
| Java (pure-Java codec) | `make java-bench` (`bindings/java/benchmarks`) | `protobuf-java` generated code from the same `matrix3.proto` | all 4 | 0.28x ❌ / 0.87x ❌ `tree` (0.26x-0.34x encode, 0.72x-0.93x decode; `core` generated model 0.63x / 0.81x) |
| Node (N-API addon) | `make node-bench` (`bindings/node/benchmarks`) | [protobufjs](https://www.npmjs.com/package/protobufjs) (fastest) + `google-protobuf` (jspb) from the same `matrix3.proto` | all 4 | 0.10x ❌ / 0.12x ❌ `tree` vs protobufjs (native-object `shim` 0.12x / 0.13x, 1.02x / 0.64x vs jspb; pure-TS `core` 0.16x / 0.19x vs protobufjs, 1.37x / 0.95x vs jspb) |
| Ruby (C extension) | `make ruby-bench` (`bindings/ruby/benchmarks`) | the `google-protobuf` gem (C extension) | all 4 | 0.03x ❌ / 0.08x ❌ `tree` (native-object `shim` 0.19x / 0.23x, one C-extension entry per op) |
| Objective-C (ARC) | `make objc-bench` (`bindings/objc/benchmarks`) | the `protobuf` ObjC runtime + generated code from the same `matrix3.proto` | all 4 | 1.72x ✅ / 1.18x ✅ |
| Kotlin (pure-Java codec + DSL) | `make kotlin-bench` (`bindings/kotlin/benchmarks`) | `protoc --kotlin_out` over `protobuf-java`, same `matrix3.proto` | all 4 | 1.11x ✅ / 1.77x ✅ `tree` (0.71x at 136 B; `core` 0.79x / 0.76x) |
| PHP (`ext-ffi` + C extension) | `make php-bench` (`bindings/php/benchmarks`) | the official PHP runtime (`protobuf/php/src`): C extension (fastest) + pure-PHP | all 4 | 0.07x ❌ / 0.09x ❌ `tree` vs C ext (2.54x / 0.89x vs pure; native-object `shim` 0.17x / 0.37x, `shim-ffi` interpreter-scale) |
| Dart (`dart:ffi`) | `make dart-bench` (`bindings/dart/benchmarks`) | the `protobuf` Dart package + generated code | all 4 | 0.50x ❌ / 4.57x ✅ `tree` (native-object `shim` 10.99x / 4.81x, one C-side reader call per op; pure-Dart `core` 4.78x ✅ encode / 1.42x ✅ decode at 74 KB, 0.32x / 0.37x at 136 B, no native library) |
| Rust (static link) | `make rust-bench` (`bindings/rust/benchmarks`) | prost (fastest) + rust-protobuf 3.x generated code | all 4 | 0.06x ❌ / 1.99x ✅ `tree` vs prost (native-object `shim` 0.88x / 2.01x, one C-side struct-codec call per op; `shim` encode above prost at 136/174 B) |
| Rust (pure-Rust `core`) | `make rust-bench` + `make conformance-rust` (`bindings/rust/src/core/`, `core` cargo feature) | prost (the fastest pure-Rust codec) + rust-protobuf 3.x | all 4 | 0.70x ❌ / 0.63x ❌ vs prost (1.08x ✅ / 0.62x vs rust-protobuf at 74 KB; no C library, no `unsafe`, official binary suite passed) |
| C# (P/Invoke) | `make csharp-bench` (`bindings/csharp/benchmarks`) | Google.Protobuf C# generated code (fastest) + protobuf-net | all 4 | 0.17x ❌ / 1.49x ✅ `tree` (native-object `shim` 1.36x / 0.70x, one P/Invoke per op over the flat image; pure-C# `core` 1.34x encode / 0.62x decode vs protobuf-csharp at 74 KB, no native library) |
| Lua (C module, tables) | `make lua-bench` (`bindings/lua/benchmarks`) | `lua-protobuf` 0.6.0 | all 4 | 1.09x ✅ / 1.42x ✅ (8.93x via builder encode — data already in pbwire) |
| Swift (SwiftPM, static link) | `make swift-bench` (`bindings/swift/benchmarks`) | `apple/swift-protobuf` 1.39.0 generated code from the same `matrix3.proto` | all 4 | 0.24x ❌ / 2.68x ✅ `tree` (native-object `shim` 3.31x / 2.69x, one C-side layout-codec call per op) |
| Swift (pure-Swift `core`) | `make swift-bench` + `make conformance-swift` (`bindings/swift/Sources/PbwireCore/`) | `apple/swift-protobuf` 1.39.0 generated code from the same `matrix3.proto` | all 4 | 0.90x ❌ / 0.30x ❌ (no C module, no `libpbwire.a`, no `unsafeFlags`; official binary suite passed) |
| Scala (Java runtime + DSL) | `make scala-bench` (`bindings/scala/benchmarks`) | ScalaPB 0.11.18 generated code from the same `matrix3.proto` | all 4 | 1.45x ✅ / 1.37x ✅ `tree` (`core` case class 0.33x / 0.30x) |
| Elixir (NIF over the shared core) | `make elixir-bench` (`bindings/elixir/benchmarks`) | `elixir-protobuf` 0.17.1 structs from the same `matrix3.proto` | all 4 | 2.56x ✅ / 1.90x ✅ |
| Zig (static link) | `make zig-bench` (`bindings/zig/benchmarks`, `-Doptimize=ReleaseFast`) | Arwalk/zig-protobuf 7.0.0 generated code from the same `matrix3.proto` | all 4 | 0.19x ❌ / 1.34x ✅ `tree` (native-object `extern struct` mirror `shim` 1.45x / 1.46x, one C struct-codec call per op) |
| Zig (pure-Zig `core`) | `make zig-bench` + `make conformance-zig-core` (`bindings/zig/core/`, `-Dcore=true`) | Arwalk/zig-protobuf 7.0.0 generated code from the same `matrix3.proto` | all 4 | 0.88x ❌ / 0.66x ❌ at 74 KB (0.42x / 0.93x at 136 B; no C source, no translate-c; official binary suite passed) |
| F# (facade over the C# runtime) | `make fsharp-bench` (`bindings/fsharp/benchmarks`) | Google.Protobuf C# generated code from the same `matrix3.proto` | all 4 | 1.35x ✅ / 1.40x ✅ `tree` (that xlarge encode cell dipped in the published run -- 2469 B reads 1461.5; `shim` 1.33x / 0.69x) |
| Clojure (Java runtime + interop) | `make clojure-bench` (`bindings/clojure/benchmarks`) | `protobuf-java` generated code from the same `matrix3.proto` | all 4 | 1.26x ✅ / 1.70x ✅ `core` (0.68x at 136 B; `tree` map API 0.01x) |
| Julia (`ccall` FFI) | `make julia-bench` (`bindings/julia/benchmarks`) | JuliaIO/ProtoBuf.jl 1.3.0 generated code from the same `matrix3.proto` | all 4 | 92.74x ✅ / 1.04x ✅ `shim` (one C-side layout read per op; full-task `tree` 7.26x encode on the xlarge baseline collapse, 1.04x decode; 136 B shim decode 0.93x) |
| R (C extension, named lists) | `make r-bench` (`bindings/r/benchmarks`) | RProtoBuf 0.4.28 on the same `matrix3.proto` | all 4 | 38.71x ✅ / 25.26x ✅ `tree` (native-object `shim` 55.79x / 58.78x, named list, one `.Call`, the walk in C) |

"vs baseline" is encode / decode throughput relative to the best baseline on
the largest case the binding runs. Where a binding has several baselines the
headline names the fastest: protobufjs over jspb, the PHP C extension over
pure PHP, prost over rust-protobuf, Google.Protobuf over protobuf-net, the
faster of gpb / gpb NIF per op. Every binding publishes the rows its
mechanisms allow, and every row name says what it times. Every arm times a
native object in and out on the same payload, never a tree built before the
timer.

| binding | published arms | intentionally absent | verdict |
|---|---|---|---|
| C | `shim` (struct codec); its `tree` arm is the dynamic `pbw_ctx` block | no third arm: two mechanisms (`shim` + `tree`, same engine) | two mechanisms; the third row is labelled `core (= shipped)` |
| C++ | mirrors the C structs (same harness) | no third arm: two mechanisms | two mechanisms; the third row is labelled `core (= shipped)` |
| Embedded | shipped core vs nanopb | no third arm: two mechanisms | two mechanisms; the third row is the shipped core |
| Objective-C | ARC wrapper over the core | no third arm: two mechanisms | two mechanisms |
| Erlang | NIF (`pbwire`) vs gpb / gpb NIF | no third arm: a second pure-BEAM codec would measure our code against gpb, not a decision | two mechanisms |
| Elixir | NIF over the shared core vs `elixir-protobuf` | no third arm: `elixir-protobuf` is that baseline | two mechanisms |
| Java | `tree` (full-task native `DMsg`), `core` (generated model, slower), `proxy` (prebuilt, same-run only) | `shim`: not a published column — one JNI crossing lives in `bindings/java/pbench/` as the ceiling, not as a binding cost | three arms + `proxy` (no `shim` column) |
| Kotlin | `tree`, `core` (reuses Java's runtime), shared `proxy` in `bindings/java/pbench/` | `shim`: the JNI ceiling in `bindings/java/pbench/`, not a published column | three arms + shared `proxy` |
| Scala | `tree` (shipped), `core` (generated case class, slower), shared `proxy` | `shim`: the JNI ceiling in `bindings/java/pbench/`, not a published column | three arms + shared `proxy` |
| Clojure | `core` (mutable-message fast path, shipped headline), `tree` (persistent-map API), shared `proxy` | `shim`: the JNI ceiling in `bindings/java/pbench/`, not a published column | three arms + shared `proxy` |
| Go | `core` (`cshim` runtime), `tree` (per-field `dync`), `proxy` (held `dync` tree, same-run only) | `shim`: no separate arm; the `cshim` runtime is the one-downcall path | three arms + `proxy` |
| C# | `tree` (full-task), `shim` (flat-image `P/Invoke`), `core` (pure C#), `proxy` (prebuilt, same-run only) | none | four arms |
| F# | `tree`, `shim` (the C# binding's native-object entry), `proxy` (shared C# run) | `core`: no separate F# port, so the C# core is not an F# row | three arms + shared `proxy` |
| Node | `tree` (full-task native object), `shim` (one call over the plain object), `core` (pure TS), `proxy` (prebuilt, same-run only) | none | four arms |
| Dart | `tree` (full-task), `shim` (one call over the flat `Struct`), `core` (pure Dart), `proxy` (prebuilt, same-run only) | none | four arms |
| Julia | `tree` (full-task), `shim` (one call over the layout struct), `core` (pure Julia), `proxy` (prebuilt, same-run only) | none | four arms |
| Rust | `tree` (full-task), `shim` (one call over the `#[repr(C)]` mirror), `core` (pure Rust), `proxy` (prebuilt, same-run only) | none | four arms |
| Swift | `tree` (full-task), `shim` (one call over the `@frozen` mirror), `core` (pure Swift), `proxy` (prebuilt, same-run only) | none | four arms |
| Zig | `tree` (full-task), `shim` (one call over the `extern struct` mirror), `core` (pure Zig), `proxy` (prebuilt, same-run only) | none | four arms |
| Python | `tree` + `shim` (the C extension's dict path: per-field vs one-downcall), `core` (pure Python), `proxy` (prebuilt, same-run only) | none | four arms |
| PHP | `tree` (full-task), `shim` (one C-extension entry walks the array), `shim-ffi` (same API on `ext-ffi`), `proxy` (prebuilt, same-run only) | `core`: deferred — `shortcut: no pure-PHP core, reopen when a native-less PHP target needs one` (TODO in `runtime/pbwire.php`, arm table in `bindings/php/pbench/README.md`); the official pure-PHP runtime is already the baseline | three native rows + `proxy` + the FFI deployment |
| Ruby | `tree` (per-field facade), `shim` (one C-extension entry), `proxy` (prebuilt, same-run only) | `core`: deferred — `shortcut: no pure-Ruby core, reopen when a native-less Ruby target needs one` (TODO in `lib/pbwire.rb`, `bindings/ruby/pbench/README.md`) | two native arms + `proxy` |
| R | `tree` (per-field reference), `shim` (one `.Call` walks the named list in C), `proxy` (prebuilt, same-run only) | `core`: deferred — `shortcut: no pure-R core, reopen when a native-less R target needs one` (TODO in `runtime/R/pbwire.R`, `bindings/r/pbench/README.md`) | two native arms + `proxy` |

So the standard block is four rows -- `pbwire-<lang>-tree` / `-shim` / `-core` /
`-proxy` -- where an arm that is not built is `n/a` with the reason in this
table, never a silently missing row. 39 published native columns count as
tree/shim/core (plus the three pure-core appendix tables for Python, C# and
Julia), and 13 `proxy` rows stand beside them in the same runs (same-run
timing only, never headlined; F# and the JVM riders share their host's
`proxy`). The arms that are absent are deliberate: the JVM four publish no
`shim` column (the
JNI ceiling is a probe in `bindings/java/pbench/`, so the JVM tables show the
binding, not a best case), PHP/Ruby/R publish no `core` column (each records the
`shortcut:` TODO its pbench README and runtime file carry), Go publishes no
`shim` arm (the `cshim` runtime is the one-downcall path), and
C/C++/Embedded/ObjC/Erlang/Elixir have two mechanisms, where a third would
measure pbwire against itself or against a baseline that is already ours.

Every binding with a
wired baseline is re-timed Go-parity (2026-11-21/22): every published row
times the same native-in/native-out task on the same payload, never a tree
built before the timer; bytes are identical everywhere. The sub-1.0x cells
that remain are boundary floors: Node's per-field N-API crossings (tree
0.10x / 0.12x of protobufjs, shim 1.02x / 0.64x of jspb at 74 KB), Ruby's
and PHP's per-field walks (0.03x / 0.08x of the gem; 0.07x / 0.09x of the
official C extension), Java's per-field dispatch (tree 0.28x / 0.87x of
protobuf-java; Kotlin/Scala ride the same runtime), Rust's `shim` encode from
2469 B up (0.87x / 0.88x: the shared C struct encoder reserves per repeated
element, while the same arm's decode and the `tree` decode row are above the
line), Dart/C#/Swift/Zig `tree` encode (0.50x / 0.17x / 0.24x / 0.19x: one
crossing per field against generated straight-line writers), and Julia's 136 B
decode (tree 0.18x, `shim` 0.93x: the C struct decode plus a `ccall` against
ProtoBuf.jl's pure-Julia generated decoder). Julia's xlarge `tree`/`shim`
encode ratios (7.26x / 92.74x) are a baseline collapse, not a codec win:
ProtoBuf.jl's generated encoder falls to 12.1 MB/s there. Each cell is inside
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
| core dynamic (`pbw_ctx`) | direct call | 85933 B | 1644.2 (1.56x ✅) | 765.9 (1.80x ✅) |
| core dynamic (plain API) | direct call | 85933 B | 1593.2 (1.51x ✅) | 771.3 (1.81x ✅) |
| C structs | direct call, layout tables | 74913 B | 1171.4 (1.32x ✅) `shim` (its `tree` arm is the `pbw_ctx` row above) | 728.1 (1.96x ✅) `shim` |
| Go | `cshim` runtime, one cgo call per op | 74913 B | 987.9 (1.92x ✅) `core`; `tree` 28.4 (0.05x ❌, pbench full-task row) | 443.7 (1.60x ✅) `core`; `tree` 35.2 (0.12x ❌) |
| Python | CPython extension, dict path | 74913 B | 570.2 (7.19x ✅) `tree encode(data)`; `shim` 516.1 (6.51x ✅); pure-Python `core` 18.8 (1.25x ✅) `encode(parsed)`, 10.3 `encode(data)` | 149.3 (3.65x ✅) `shim to_dict`; `tree` 64.1 (1.57x ✅); pure-Python `core` 6.5 (1.05x ✅) |
| Java | pure-Java codec, no native code | 74913 B | 247.2 (0.28x ❌) `tree`; generated model `core` 563.6 (0.63x ❌) | 357.2 (0.87x ❌) `tree`; `core` 329.6 (0.81x ❌) |
| Kotlin | the pure-Java codec + DSL | 74913 B | 990.3 (1.11x ✅) `tree`; `core` 705.6 (0.81x ❌) | 772.9 (1.77x ✅) `tree`; `core` 333.1 (0.75x ❌) |
| Node | N-API addon | 74913 B | 51.1 (0.10x ❌ vs protobufjs, 0.88x vs jspb) `tree`; native-object `shim` 58.9 (0.12x / 1.02x); pure-TS `core` 79.4 (0.16x / 1.37x) | 50.2 (0.12x / 0.60x) `tree`; `shim` 53.4 (0.13x / 0.64x); `core` 79.4 (0.19x / 0.95x) |
| Dart | `dart:ffi` binding | 74913 B | 52.4 (0.50x ❌) `tree`; native-object `shim` 1152.1 (10.99x ✅); pure-Dart `core` 501.8 (4.78x ✅) | 715.6 (4.57x ✅) `tree`; `shim` 752.9 (4.81x ✅); pure-Dart `core` 222.3 (1.42x ✅) |
| Ruby | C extension over the core | 74913 B | 35.9 (0.03x ❌) `tree`; native-object `shim` 249.3 (0.19x ❌) | 51.8 (0.08x ❌) `tree`; `shim` 159.2 (0.23x ❌) |
| Objective-C | ARC wrapper | 74913 B | 62089.8 (1.72x ✅) | 29297.5 (1.18x ✅) |
| PHP | C extension | 74913 B | 84.7 (0.07x ❌) `tree`; native-object `shim` 211.7 (0.17x ❌); `shim-ffi` 5.5 | 53.3 (0.09x ❌) `tree`; `shim` 212.5 (0.37x ❌); `shim-ffi` 4.4 |
| Rust | static link | 74913 B | 79.3 (0.06x ❌ vs prost) `tree`; native-object `shim` 1192.2 (0.88x ❌); pure-Rust `core` 951.3 (0.70x ❌) | 753.3 (1.99x ✅) `tree`; `shim` 762.4 (2.01x ✅); `core` 240.1 (0.63x ❌) |
| C# | P/Invoke | 74913 B | 81.1 (0.17x ❌) `tree`; native-object `shim` 658.3 (1.36x ✅); pure-C# `core` 649.1 (1.34x ✅) | 732.1 (1.49x ✅) `tree`; `shim` 346.8 (0.70x ❌); `core` 306.3 (0.62x ❌) |
| Lua | C module, tables | 74913 B | 196.9 (1.09x ✅) | 124.7 (1.42x ✅) |
| Erlang | NIF (`pbwire`) | 74913 B | 483.3 (3.91x ✅) | 356.5 (1.60x ✅) |
| Swift | static link (SwiftPM) | 74913 B | 66.3 (0.24x ❌) `tree`; native-object `shim` 915.1 (3.31x ✅); pure-Swift `core` 247.7 (0.90x ❌) | 578.9 (2.68x ✅) `tree`; `shim` 580.9 (2.69x ✅); `core` 65.9 (0.30x ❌) |
| Scala | the Java runtime + DSL | 74913 B | 1050.5 (1.45x ✅) `tree`; case-class `core` 334.6 (0.42x ❌) | 707.4 (1.37x ✅) `tree`; `core` 169.7 (0.34x ❌) |
| Elixir | NIF (Hex `:pbwire`, same core as Erlang) | 74913 B | 152.6 (2.56x ✅) | 105.7 (1.90x ✅) |
| Zig | static link | 74913 B | 164.0 (0.19x ❌) `tree`; native-object `shim` 1227.4 (1.45x ✅); pure-Zig `core` 741.2 (0.88x ❌) | 723.8 (1.34x ✅) `tree`; `shim` 788.6 (1.46x ✅); `core` 357.0 (0.66x ❌) |
| F# | P/Invoke over the C# runtime | 74913 B | 671.2 (1.35x ✅) `tree` (that row dipped in this run: 2469 B reads 1461.5); `shim` 658.3 (1.33x ✅) | 722.6 (1.40x ✅) `tree`; `shim` 354.6 (0.69x ❌) |
| Clojure | the Java runtime + interop | 74913 B | 1123.1 (1.26x ✅) `core`; persistent-map `tree` 10.0 (0.01x ❌) | 742.4 (1.70x ✅) `core`; `tree` 8.5 (0.02x ❌) |
| Julia | `dlopen` + `ccall` | 74913 B | 87.9 (7.26x ✅, baseline collapse) `tree`; native-object `shim` 1122.2 (92.74x ✅); pure-Julia `core` 29.3 (2.42x ✅) | 739.8 (1.04x ✅) `tree`; `shim` 738.9 (1.04x ✅); pure-Julia `core` 24.9 (0.04x ❌) |
| R | C extension, named lists | 74913 B | 108.4 (38.71x ✅) `tree`; native-object `shim` 156.2 (55.79x ✅) | 68.2 (25.26x ✅) `tree`; `shim` 158.7 (58.78x ✅) |

Read it as: the in-process dynamic core sets the pace (1.56x the C++
generated code at this size), and a native-object row pays its boundary, not
the codec. The Go-parity re-time (2026-11-21/22, one binding at a time) moved
every `tree` row from the prebuilt handle to the native-in/native-out full
task, so the below-the-line cells are now the per-field crossing made visible:
Node (0.10x / 0.12x of protobufjs), Ruby (0.03x / 0.08x), PHP (0.07x / 0.09x of
the official C extension), Java (0.28x / 0.87x), Rust encode (0.06x of prost),
Dart/C#/Swift/Zig encode (0.50x / 0.17x / 0.24x / 0.19x). Their decode rows
tell which boundaries amortize: Rust 1.99x, Dart 4.57x, C# 1.49x, Swift 2.68x,
Zig 1.34x. The outliers are baseline or unit effects, not pbwire
effects: Python's encode column is the dict path (3000 sub-messages, 12000
ints), Julia's xlarge encode column is a baseline collapse (ProtoBuf.jl's
generated encoder falls to 12.1 MB/s there), and the Objective-C tier is a
memcpy race over one 74 KB string (the 16-byte UTF-8 word scan is what keeps
its decode near the copy cost).
The headline always names the fastest baseline: Node's native rows are quoted
over protobufjs (jspb, the legacy baseline, trails it by ~9x / ~5x at 74 KB),
and Node's `shim` row -- the native object in and out -- is 0.12x / 0.13x of
protobufjs (1.02x / 0.64x of jspb): the boundary, not the codec, is what that
row measures. PHP's native rows are over the official C extension (the pure-PHP
runtime trails it by ~350x / ~100x at 74 KB) -- see their sections. The old
prebuilt-handle `tree` headlines (Node 3.36x / 2.95x, Swift 6.01x / 2.71x,
Julia 137.75x / 1.08x) now live on the `proxy` rows: same-run timing only,
published beside the native rows, never headlined.

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

Per-language index: [Core](#core-pbw_ctx-vs-plain-vs-generated) · [C structs](#c-structs-vs-protoc-generated-same-matrix3proto) · [Go](#go-thin-model-vs-protobuf-go) · [Python](#python-vs-googleprotobuf-upb) · [Java](#java-vs-protobuf-java) · [Node](#node-vs-google-protobuf-jspb-and-protobufjs) · [Ruby](#ruby-c-extension-vs-the-google-protobuf-gem) · [Objective-C](#objective-c-vs-the-protobuf-objc-runtime) · [Kotlin](#kotlin-vs-protoc---kotlin_out-over-protobuf-java) · [PHP](#php-vs-the-official-php-runtime-pure--c-extension) · [Dart](#dart-vs-the-protobuf-dart-package) · [Rust](#rust-vs-prost-and-rust-protobuf-3x) · [C#](#c-vs-googleprotobuf) · [Lua](#lua-c-module-tables-vs-lua-protobuf) · [Swift](#swift-swiftpm-static-link-vs-appleswift-protobuf) · [Scala](#scala-java-runtime--dsl-vs-scalapb) · [Elixir](#elixir-nif-over-the-shared-core-vs-elixir-protobuf) · [Zig](#zig-static-link-vs-arwalkzig-protobuf) · [F#](#f-facade-over-the-c-runtime-vs-googleprotobuf) · [Clojure](#clojure-java-runtime--interop-vs-protobuf-java) · [Julia](#julia-dlopen--ccall-vs-protobufjl) · [R](#r-c-extension-named-lists-vs-rprotobuf) · [Erlang](#erlang-pbwire-vs-gpb-vs-gpb-nif). In every table below, ✅ means that cell beats its baseline (≥1.0x), ❌ means it does not.

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
| 132 B | encode | 2078.5 (1.32x ✅) | 1789.2 (1.13x ✅) | 1558.3 | 106.6 |
| 132 B | decode | 1467.5 (1.63x ✅) | 1242.4 (1.38x ✅) | 886.8 | 277.9 |
| 169 B | encode | 1929.2 (1.24x ✅) | 1718.8 (1.10x ✅) | 1551.9 | 102.9 |
| 169 B | decode | 1144.1 (1.50x ✅) | 996.5 (1.30x ✅) | 763.6 | 268.1 |
| 2755 B | encode | 1572.9 (1.46x ✅) | 1382.4 (1.28x ✅) | 1078.3 | 121.8 |
| 2755 B | decode | 751.5 (1.72x ✅) | 699.3 (1.60x ✅) | 436.6 | 240.5 |
| 85933 B | encode | 1644.2 (1.56x ✅) | 1593.2 (1.51x ✅) | 1053.1 | 139.7 |
| 85933 B | decode | 765.9 (1.80x ✅) | 771.3 (1.81x ✅) | 426.6 | 239.6 |

Threads (169 B, 1 vs 8):

| op | pbwire 1thr (MB/s, x gen) | pbwire 8thr agg (MB/s, x gen) | gen 1thr (MB/s) | gen 8thr agg (MB/s) |
|---|---|---|---|---|
| encode | 1885.5 (1.20x ✅) | 13298.4 (1.57x ✅) | 1535.8 | 8263.2 |
| decode | 1132.4 (1.42x ✅) | 6338.5 (1.08x ✅) | 773.2 | 5750.4 |

`8thr agg` is the total throughput of all eight threads. The schema, the
descriptors and the wire input are shared by all threads; each thread only adds
its own `pbw_ctx` and message. pbwire scales 7.5x (encode) and 5.6x (decode)
from 1 to 8 threads in that run; generated code reaches 5.4x and 7.4x. Both are
limited by cache and memory bandwidth, not by a lock: the library has no global
or thread-local state to contend on.

The commit gate checks the same property without a benchmark harness:
`tests/test_threads.c` prints 1-thread vs 8-thread MB/s for the context API (its
own, smaller message, one run on the same machine):

```
scale ctx decode 1 thread:   3304.7 MB/s (  58.7 ns/op) | 8 threads:   2119.4 MB/s/thread,  16955.3 MB/s aggregate (per-op 1.56x, aggregate 5.13x, 14 cores)
scale ctx encode 1 thread:   5665.9 MB/s (  34.2 ns/op) | 8 threads:   3100.7 MB/s/thread,  24806.0 MB/s aggregate (per-op 1.83x, aggregate 4.38x, 14 cores)
```

It fails when the aggregate throughput collapses below 0.35x of
`min(8, cores)`, which is what a hidden lock or false sharing would cause.

#### `pbwire ctx` vs `pbwire plain`

The two pbwire columns are the same encoder and decoder with a different memory
and ownership strategy; the benchmark checks that they produce identical bytes
before timing anything. The ownership table is in
[docs/design.md](docs/design.md#two-allocation-strategies); on the wire it shows
up as the `malloc`/`free` pair per call, which is why plain trails ctx by
about 0.1-0.2x on the small tiers (1.22x / 1.20x at 132 B, 1.16x / 1.08x at
169 B) and lands within a few percent of it at 2755 B and up. The plain 169 B
encode now leads generated code too (1.10x): the writer's cached base pointer
(see the ledger) removed enough per-field work that one `malloc`/`free` pair
no longer costs the cell.

Read it as:

- **Against generated code**: the context API wins every binary cell — encode
  by 1.24x-1.56x, decode by 1.50x-1.81x. There is no tier where a compiled
  straight-line writer beats it outright, because the C++ serializer walks the
  message twice (size, then write) while pbwire writes one pass.
- **Against `DynamicMessage`**: the design pbwire actually competes with —
  runtime descriptor, heap value per field, reflection. pbwire is 3.0-5.3x
  faster there (decode) and 11.7-18.0x on encode.
- **The plain column** pays exactly one `malloc`/`free` pair per call (decoded
  tree, output buffer), which is why it sits ~0.1-0.3x behind ctx on the small
  tiers and is a tie on the 169 B encode. Use the plain API for one-off
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
(`pbw_message_encoded_size`) costs about 0.8 of an encode because it walks the
same tree without writing, the sink costs one pass over the bytes, and a
delimited feed/next round-trip costs about one encode plus one decode. Ratios
only — absolute numbers move between machines.

### C structs vs `protoc` generated (same `matrix3.proto`)

![C structs vs protoc generated (same matrix3.proto) — MB/s by tier](docs/charts/bench-c-structs-vs-protoc-generated-same-matrix3-proto.svg)

The `pbwire-c-shim` rows are the struct codec reading C memory in place. The
dynamic `pbw_ctx` block `make bench` prints for the same message is the
binding's `pbwire-c-tree` arm, and `pbwire-c1` stays the one-shot probe.

| payload | op | pbwire-c-shim (MB/s, x gen) | protoc gen (MB/s) |
|---|---|---|---|
| 136 B | encode | 1766.1 (1.29x ✅) | 1371.8 |
| 136 B | decode | 1193.5 (1.44x ✅) | 827.3 |
| 174 B | encode | 1583.3 (1.21x ✅) | 1313.5 |
| 174 B | decode | 1025.7 (1.37x ✅) | 749.2 |
| 2469 B | encode | 1101.9 (1.27x ✅) | 869.8 |
| 2469 B | decode | 670.4 (1.76x ✅) | 380.6 |
| 74913 B | encode | 1171.4 (1.32x ✅) | 886.8 |
| 74913 B | decode | 728.1 (1.96x ✅) | 372.4 |

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
the layout per call and mallocs per value (5.3x / 3.9x slower at 136 B, 4.2x /
3.1x at 174 B, and down to 1.0-1.1x from 2.5 KB up, where the per-call layout
compile is amortised). It stays in the
output so that trade-off is visible rather than hidden.

### Go (thin model vs protobuf-go)

![Go (thin model vs protobuf-go) — MB/s by tier](docs/charts/bench-go-thin-model-vs-protobuf-go.svg)

The shipped runtime is [`bindings/go/runtime/`](bindings/go/runtime/): the
Erlang-style C codec reading Go structs through generated layout tables, one
cgo crossing per op. `make go-bench` (`bindings/go/benchmarks`, build-tagged
so `go test ./...` stays fast) times the thin generated model —
`Marshal()`/`Unmarshal()` over that runtime, native struct in and out —
against `protobuf-go` generated code from the same `matrix3.proto` on all
four tiers. The two generated packages are the committed ones under
`bindings/go/tests`, so `bench.sh` needs Go only: no protoc, no C toolchain,
no descriptor.

The rows below compare the same user-visible task, struct in and struct out,
gated on byte-identical output and a field-by-field decode before timing. Apple
M4 Pro, one process times both arms:

| payload | op | pbwire-go-core (MB/s, x protobuf-go) | protobuf-go (MB/s) |
|---|---|---|---|
| 136 B | encode | 866.3 (1.35x ✅) | 641.8 |
| 136 B | decode | 691.9 (1.21x ✅) | 573.2 |
| 174 B | encode | 905.1 (1.47x ✅) | 613.8 |
| 174 B | decode | 566.7 (1.17x ✅) | 484.7 |
| 2469 B | encode | 958.2 (1.79x ✅) | 535.5 |
| 2469 B | decode | 426.3 (1.55x ✅) | 275.2 |
| 74913 B | encode | 987.9 (1.92x ✅) | 515.5 |
| 74913 B | decode | 443.7 (1.60x ✅) | 277.2 |

Every `pbwire-go-core` cell is ahead: encode writes straight from struct
memory through one cgo call, decode fills the struct plus a single string
arena with no per-field dispatch and no tree. `make go-bench` is the formal
run (recorded in `results/bench-go-20261121T182323Z.json`); the `tree` arm is
the `dync` per-field path from the pbench README -- the C core reached through
a value tree, one cgo call per field, native struct in and out -- and it is
never the headline (0.05x encode / 0.12x decode at 74 KB: the per-field copy
into a tree is what separates the two arms). The `proxy` row prices the
held-tree task on that same `dync` arm in the same formal run (1461.6 MB/s
encode / 681.1 MB/s decode at 74 KB, 2.84x / 2.46x the run's `protobuf-go`;
same-run timing only, published, never the headline): 51.5x the tree encode
and 19.3x the tree decode at xlarge, the cost of the per-field copy made
visible.

The three dynamic arms behind the thin
model were compared under [`bindings/go/pbench/`](bindings/go/pbench/)
(same tiers, interleaved, native struct in and out, converter-only glue):
the `tree` arm at 0.05-0.10x encode and 0.11-0.13x decode, the `core` arm
(direct, no cgo) at 1.43-1.57x encode and 0.84-1.23x decode, and the `shim`
arm at 1.41-1.82x encode and 1.11-2.13x decode. The per-field copy into any
tree is what separates them.

### Python vs `google.protobuf` (upb)

![Python vs google.protobuf (upb) — MB/s by tier](docs/charts/bench-python-vs-google-protobuf-upb.svg)

| payload | op | pbwire-py tree (MB/s, x upb) | pbwire-py shim (MB/s, x upb) | upb (MB/s) |
|---|---|---|---|---|
| 136 B | encode(data) | 280.8 (5.49x ✅) | 285.2 (5.57x ✅) | 51.2 |
| 136 B | to_dict(bytes) | 56.7 (1.66x ✅) | 164.7 (4.83x ✅) | 34.1 |
| 174 B | encode(data) | 310.6 (5.69x ✅) | 323.5 (5.93x ✅) | 54.6 |
| 174 B | to_dict(bytes) | 55.4 (2.14x ✅) | 149.4 (5.77x ✅) | 25.9 |
| 2469 B | encode(data) | 372.2 (4.90x ✅) | 420.5 (5.53x ✅) | 76.0 |
| 2469 B | to_dict(bytes) | 73.9 (1.82x ✅) | 183.3 (4.51x ✅) | 40.6 |
| 74913 B | encode(data) | 570.2 (7.19x ✅) | 516.1 (6.51x ✅) | 79.3 |
| 74913 B | to_dict(bytes) | 64.1 (1.57x ✅) | 149.3 (3.65x ✅) | 40.9 |

The `tree` rows time the per-field full task (fresh dict in, `encode(dict)`;
`decode(bytes)` plus `to_dict()` with every field read back); the `shim` rows
time the same task in one downcall per op. The `proxy` rows in the same formal
run time the prebuilt task instead (held-handle `encode(parsed)`, bare
`decode(bytes)` with no materialization; same-run timing only, published,
never the headline): decode 584.2 / 594.2 / 828.9 / 911.2 MB/s
(1.02x / 1.09x / 1.18x / 1.15x of upb `decode`), `encode(parsed)` 2727.9 /
2975.6 / 29189.3 / 89182.7 MB/s (3.68x / 4.41x / 20.94x / 60.92x of upb
`encode(parsed)`). Formal run `results/bench-python-20261121T183716Z.json`.

Rows are the same user-visible task in both arms:

- **encode(data)** starts from the same dict. upb has no dict entry point, so
  its side is filling a generated message field by field and then serializing;
  that population is most of the ~945 us on the 75 KB tier. pbwire walks the
  dict once and writes the wire directly (both the per-field `tree` path and
  the one-downcall `shim` path).
- **decode(bytes)** ends with an in-memory message. The `proxy` row parses into
  the held handle and stops there (no materialization); upb parses into its
  arena. The native rows decode further, see below.
- **to_dict(bytes)** ends with plain Python data on either side: native
  `decode(bytes)` plus `to_dict()`, against `FromString` plus reading every field through
  `ListFields()`. This is the `malloc`-bound row — 3000 sub-messages mean 3000
  dicts, 12000 ints and 3000 strings on either side, and nobody escapes it.
  (`json_format.MessageToDict` is ~4x slower than reading the fields, so the
  harness does not use it.)
- **encode(parsed)** starts from an in-memory message in both arms. The
  `proxy` row returns the bytes a `Message` was parsed from, so the round trip
  is byte-exact and preserves unknown fields; upb re-encodes every field from
  its own structs.

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

#### The pure-Python core (no extension)

The binding's third arm is a port of the C core in stdlib-only Python
(`bindings/python/pure/`: descriptor reader, tree, encoder, decoder; no C
extension, no `google.protobuf`), for the modes where the extension cannot
load -- a pure-Python interpreter, PyPy, a sandbox. `make python-bench` prints
it as `pbwire-py-core`, gated first on protoc's bytes and a field-by-field
decode. Formal run, same run (`results/bench-python-20261121T183716Z.json`,
2026-11-21), MB/s:

| payload | `encode(data)` | `to_dict(bytes)` | `encode(parsed)` |
|---|---|---|---|
| 136 B | 8.7 | 7.1 | 13.1 |
| 174 B | 8.7 | 6.3 | 12.8 |
| 2469 B | 10.0 | 6.8 | 18.0 |
| 74913 B | 10.3 | 6.5 | 18.8 |

upb in the same run, at 74913 B: 79.3 / 40.9 / 1463.9. Comparing the core
against upb would be dishonest -- upb is a C extension, and the whole point of
this arm is running where none can load. The comparable baseline is
`google.protobuf`'s own pure-Python runtime, measured on the same 74913 B
payload in a separate process
(`PROTOCOL_BUFFERS_PYTHON_IMPLEMENTATION=python`): `FromString` 6.2 MB/s and
`SerializeToString` on a parsed message 15.0 MB/s. So at 74 KB the core decodes
1.05x that parser (6.5 vs 6.2) and encodes a parsed tree 1.25x (18.8 vs 15.0);
the official pure-Python runtime has no dict entry point, so `encode(data)`
has no counterpart there.

That is the honest shape of a pure-Python codec: ~100x behind the extension,
which materializes in C, and at parity or slightly ahead of the only other
pure-Python implementation of the same format. The official binary suite is
green through it (`make conformance-python-pure`: 1412 successes, 1403
JSON/text skipped, 2 expected, 0 unexpected -- the same failure set as the C
core), which is the bar for publishing. Verdict and the Stage 6 decision are in
`bindings/python/pbench/README.md` and `docs/perf-attempts.md`: the extension
stays the shipped codec, the core is the no-extension mode, never the headline.

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

| payload | op | pbwire-java-tree (MB/s, x protobuf-java) | pbwire-java-core (MB/s) | protobuf-java (MB/s) |
|---|---|---|---|---|
| 136 B | encode | 526.7 (0.26x ❌) | 607.3 | 2026.1 |
| 136 B | decode | 552.9 (0.93x ❌) | 336.5 | 597.3 |
| 174 B | encode | 432.1 (0.34x ❌) | 593.8 | 1280.0 |
| 174 B | decode | 399.0 (0.72x ❌) | 314.6 | 554.5 |
| 2469 B | encode | 292.2 (0.34x ❌) | 536.2 | 866.5 |
| 2469 B | decode | 328.0 (0.75x ❌) | 296.1 | 438.1 |
| 74913 B | encode | 247.2 (0.28x ❌) | 563.6 | 898.1 |
| 74913 B | decode | 357.2 (0.87x ❌) | 329.6 | 408.7 |

The two pbwire columns are the native arms (`bindings/java/pbench/`): `tree`
is the per-field full task (a fresh `DMsg` built field by field per iteration,
then encode; decode plus a full field-by-field read-back), `core` is the
committed generated model (`Structtest_Scalars3`) used as the native object
(`toByteArray()`/`parseFrom()`). The `proxy` rows in the same formal run time
the prebuilt task instead (held `DMsg` re-encode, bare decode with no
read-back; same-run timing only, published, never the headline): encode
1604.1 / 1420.0 / 942.9 / 993.1 MB/s (0.79x / 1.11x / 1.09x / 1.11x),
decode 854.2 / 701.4 / 736.7 / 792.1 MB/s (1.43x / 1.26x / 1.68x / 1.94x).
Formal run `results/bench-java-20261121T183049Z.json`.

The two pbwire columns above are the native arms: `tree` is the full-task
native path and stays Java's headline even though it sits below the baseline
(encode 0.26x-0.34x, decode 0.72x-0.93x); `core` is the shipped typed path and
it loses at every tier here (0.30x at 136 B, 0.63x at 74 KB encode, 0.56x-0.81x
decode): the generated table walks one `FieldMeta` lambda per field and its
decode builds the eager typed tree, against a generated writer that knows
every tag and a decode that materializes nothing until read. The model stays
the ergonomic path. The `tree` decode column is lazy (message/string/bytes
fields parse on first access); the `core` decode column is eager, so it
carries the materialization the `tree` column skips. `shim` for the JVM four:
`bindings/java/pbench/` (`jni/`, one crossing, no tree translation -- a
ceiling, not a binding cost).

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
compile time, which is why the full-task `tree` encode sits at 0.26x-0.34x:
that floor is structural, not tuning.

#### Node vs `google-protobuf` (jspb) and protobufjs

![Node vs google-protobuf (jspb) and protobufjs — MB/s by tier](docs/charts/bench-node-vs-google-protobuf-jspb-and-protobufjs.svg)

| payload | op | pbwire-node-tree (MB/s, x protobufjs) | pbwire-node-shim (MB/s, x protobufjs) | pbwire-node-core (MB/s, x protobufjs) | protobufjs (MB/s) | jspb (MB/s) |
|---|---|---|---|---|---|---|
| 136 B | encode | 38.5 (0.16x ❌) | 48.6 (0.20x ❌) | 64.1 (0.26x ❌) | 244.7 | 58.1 |
| 136 B | decode | 39.0 (0.10x ❌) | 61.5 (0.15x ❌) | 92.0 (0.23x ❌) | 397.8 | 116.2 |
| 174 B | encode | 41.9 (0.14x ❌) | 53.2 (0.18x ❌) | 70.7 (0.24x ❌) | 299.5 | 62.3 |
| 174 B | decode | 36.4 (0.09x ❌) | 50.2 (0.13x ❌) | 75.3 (0.19x ❌) | 390.2 | 112.1 |
| 2469 B | encode | 47.4 (0.11x ❌) | 55.7 (0.12x ❌) | 149.9 (0.33x ❌) | 450.2 | 57.1 |
| 2469 B | decode | 44.7 (0.12x ❌) | 49.4 (0.13x ❌) | 83.5 (0.22x ❌) | 381.2 | 82.3 |
| 74913 B | encode | 51.1 (0.10x ❌) | 58.9 (0.12x ❌) | 79.4 (0.16x ❌) | 511.4 | 58.0 |
| 74913 B | decode | 50.2 (0.12x ❌) | 53.4 (0.13x ❌) | 79.4 (0.19x ❌) | 423.1 | 83.9 |

All three pbwire columns are the same native-in/native-out full task
(`bindings/node/pbench/`): `tree` builds a fresh `Message` with one `set()`
per field, encodes and frees, then decodes and reads every field back with one
`get()` per field into a fresh object; `shim` does the same task in one
`encodeObject` / `decodeToObject` downcall; `core` runs the same task on the
pure-TypeScript codec (`bindings/node/lib/core/`, no native library). The
`proxy` rows in the same formal run time the prebuilt task instead (re-encode
a held handle, decode-and-free with no materialization; same-run timing only,
published, never the headline): encode 618.6 / 568.7 / 1514.7 / 1723.3 MB/s,
decode 373.9 / 427.7 / 965.2 / 1229.0 MB/s. Proxy/tree encode gap at xlarge:
33.7x -- the gap the old prebuilt `tree` row was hiding. Formal run
`results/bench-node-20261121T181411Z.json`.

The two pbwire columns are the arms (`bindings/node/pbench/`). `tree` is the
codec once the data is already in pbwire: a `Message` handle is built before the
timer, and `encode()`/`decode()` run against it (a decoded handle materializes a
field only when it is read). `shim` is the user-visible native-object task:
`type.encodeObject(obj)` and `type.decodeToObject(wire)` hand a plain JS object
or `Buffer` to the addon and get one back, one N-API downcall per op, with the
addon walking the schema's field table in C (nested messages and repeated
elements recurse in C, so no per-field JS round trip). `core` is the pure
TypeScript codec (`bindings/node/lib/core/`, `make conformance-node-core`): the
same native task in the same run, no native library and no boundary.

Against jspb (the legacy baseline) the native rows read: tree encode
0.66x / 0.67x / 0.83x / 0.88x and decode 0.34x / 0.32x / 0.54x / 0.60x; shim
encode 0.84x / 0.85x / 0.98x / 1.02x and decode 0.53x / 0.45x / 0.60x / 0.64x;
core encode 1.10x / 1.13x / 2.63x / 1.37x and decode 0.79x / 0.67x / 1.01x /
0.95x (small / medium / large / xlarge). So the native-object task beats jspb
only on encode (shim at xlarge, core everywhere) and stays below it on every
decode cell: each field of a native-object op is one N-API property read or
write (~60-70 ns) where jspb and protobufjs pay a few ns in their own heap.
The verdict (tree handle path stays the documented default, the one-downcall
`shim` is the recommended one-shot path, `core` is the toolchain-less mode)
is in `bindings/node/pbench/README.md` and the ledger; the old prebuilt-handle
`tree` headline is retired.

The pure-TypeScript `core` column above is the same native task in the same
run (no separate appendix table): an in-language codec that walks a schema
table beats jspb's generated reader/writer on encode at every tier
(1.10x-2.63x) and matches it on the large decodes (1.01x / 0.95x), because
jspb resolves a code/type table per field and hands every repeated element to
a closure. It stays ~5-8x behind protobufjs, which generates per-type code.

jspb's throughput does not move with payload size: its writer resolves a
code/type table per field and hands every repeated element to a closure, where
the C core writes one tag byte and one inline value, or memcpys an already
validated slice. protobufjs, the fastest widely used JS runtime, is 3-7x faster
than jspb on the same bytes (and the harness gates its bytes too).

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

| payload | op | pbwire-ruby-tree (MB/s, x gem) | pbwire-ruby-shim (MB/s, x gem) | gem (MB/s) |
|---|---|---|---|---|
| 136 B | encode | 38.8 (0.06x ❌) | 191.7 (0.28x ❌) | 686.4 |
| 136 B | decode | 26.6 (0.10x ❌) | 145.7 (0.56x ❌) | 259.3 |
| 174 B | encode | 36.2 (0.05x ❌) | 200.4 (0.25x ❌) | 799.3 |
| 174 B | decode | 30.2 (0.11x ❌) | 157.2 (0.58x ❌) | 270.9 |
| 2469 B | encode | 32.1 (0.03x ❌) | 227.6 (0.18x ❌) | 1235.8 |
| 2469 B | decode | 44.3 (0.09x ❌) | 150.4 (0.30x ❌) | 498.4 |
| 74913 B | encode | 35.9 (0.03x ❌) | 249.3 (0.19x ❌) | 1341.4 |
| 74913 B | decode | 51.8 (0.08x ❌) | 159.2 (0.23x ❌) | 686.0 |

Ruby is a C extension over the same core, so this is C extension against C
extension. The first version drove the same C ABI through stdlib Fiddle and lost
every row by an order of magnitude: a downcall costs ~3 µs, so a 136 B encode was
3.3 µs against the core's ~65 ns, and the tiers could only be compared by padding
`str` because the wrapper had no repeated fields. The rewrite removed the
boundary, added the repeated-field payload (so the xlarge tier is 3000 nested
messages, not one 74 KB string), and split the message into two representations.
Both pbwire columns are the native-in/native-out full task
(`bindings/ruby/pbench/`), formal run
`results/bench-ruby-20261122T015616Z.json`. The `proxy` rows in the same run
time the prebuilt task instead (held `Message` builder re-encode, wire-view
decode with no materialization; same-run timing only, published, never the
headline). The two native arms:

- **`tree` (per-field facade, the published reference).** Fresh generated model
  per iteration, one facade call per field, full read-back on decode. It is
  2.5x-3.1x the old per-field model path but below the gem (0.03x-0.11x): the
  gem's generated object already owns C-backed storage (and its decode stays
  lazy), while this row builds a C tree from Ruby ivars and materializes every
  model and array.
- **`shim` (native object in and out).** `type.encode_object(obj)` walks the
generated model inside C in one entry call and fills a pbw tree; `decode_object`
decodes once and materializes the generated model classes in C. It is the
fastest native-in/native-out arm on every tier on both ops (0.18x-0.28x /
0.23x-0.58x vs the gem) and the simplest -- one extension call removes the
Ruby per-field loop -- but it stays below the gem for the same reason.

The `proxy` row (prebuilt `Message` builder / wire view) is above the gem at
every tier (1.06x-1.33x encode, 1.41x-1.94x decode): that stays the fast path
for holders of a tree, published beside the native rows and never headlined.
The old prebuilt-`tree` headline is retired. The 174 B `tree` encode band cell
from the old table no longer applies: every native cell is below the line now,
and the finding is the boundary, not a band.

#### Objective-C vs the `protobuf` ObjC runtime

![Objective-C vs the protobuf ObjC runtime — MB/s by tier](docs/charts/bench-objective-c-vs-the-protobuf-objc-runtime.svg)

The `pbwire-objc` rows are the `tree` arm (`PBWMessage` handle). The direct
struct path is the same C code, measured by `make bench-c` (the `shim` rows);
there is no third mechanism to compare here.

| payload | op | pbwire-objc (MB/s, x protobuf-objc) | protobuf-objc (MB/s) |
|---|---|---|---|
| 136 B | encode | 957.4 (7.44x ✅) | 128.7 |
| 136 B | decode | 658.6 (2.76x ✅) | 239.0 |
| 174 B | encode | 1161.0 (5.62x ✅) | 206.5 |
| 174 B | decode | 803.0 (2.83x ✅) | 283.7 |
| 2469 B | encode | 12031.0 (4.64x ✅) | 2590.3 |
| 2469 B | decode | 8797.9 (2.43x ✅) | 3614.4 |
| 74913 B | encode | 62089.8 (1.72x ✅) | 36094.9 |
| 74913 B | decode | 29297.5 (1.18x ✅) | 24919.0 |

The ARC wrapper adds one Objective-C object per call and no copy: `encode` hands
the encoder's own malloc'd buffer to `NSData` with
`dataWithBytesNoCopy:length:freeWhenDone:`, and `decode` wraps the decoder's
arena in one `PBWMessage`. That stays ahead of the generated runtime on all four
tiers and both ops (6.6x / 2.7x at 136 B; the 74 KB tier is 1.9x / 1.2x, the
closest row). The xlarge tier is one 74 KB string, so it is a memcpy race: the
UTF-8 validation that runs at parse is the one walk over those bytes that is
not a copy, which is why it now uses the 16-byte ASCII scan (see the ledger);
the generated encoder
writes it into an `NSMutableData` of the final size, and the wrapper writes it
into its own buffer, which `NSData` then takes over.

#### Kotlin vs `protoc --kotlin_out` over `protobuf-java`

![Kotlin vs protoc --kotlin_out over protobuf-java — MB/s by tier](docs/charts/bench-kotlin-vs-protoc-kotlin-out-over-protobuf-java.svg)

| payload | op | pbwire-kotlin-tree (MB/s, x protobuf-kotlin) | pbwire-kotlin-core (MB/s) | protobuf-kotlin (MB/s) |
|---|---|---|---|---|
| 136 B | encode | 1554.4 (0.71x ❌) | 628.8 | 2202.7 |
| 136 B | decode | 890.4 (1.41x ✅) | 317.0 | 632.2 |
| 174 B | encode | 1491.6 (1.20x ✅) | 624.4 | 1242.6 |
| 174 B | decode | 722.0 (1.24x ✅) | 309.7 | 582.9 |
| 2469 B | encode | 996.9 (1.16x ✅) | 643.4 | 862.5 |
| 2469 B | decode | 709.0 (1.54x ✅) | 298.8 | 459.6 |
| 74913 B | encode | 990.3 (1.11x ✅) | 705.6 | 890.0 |
| 74913 B | decode | 772.9 (1.77x ✅) | 333.1 | 436.1 |

Kotlin reuses the Java runtime (`bindings/java/runtime`), so these numbers are
`pbwire-java`'s: one workspace lookup plus one `byte[]` copy per call. `core`
is the same committed generated Java model the Kotlin DSL builds (the
`tests/gen/matrix3.kt` factories are inline sugar over it), so it repeats
Java's loss (0.29x at 136 B, 0.79x at 74 KB encode, 0.50x-0.76x decode) for the
same reason; the `tree` row stays the headline. The `tree` decode column is
lazy, the `core` column eager (the materialization footnote is in the Java
section). `shim` for the JVM four: `bindings/java/pbench/` (`jni/`, one
crossing, no tree translation -- a ceiling, not a binding cost). The 136 B encode
is the case the Java section explains (a generated writer knows its tags, the
dynamic tree walk derives them), 0.71x in the latest run against a baseline
that reaches the same protobuf-java encoder through its builder/DSL path. The
174 B encode used to swing between 0.44x and 1.20x across runs on the same
codec and bytes (the codec-level A/B shows no codec-side cause: harness
allocation/GC sensitivity on a loaded host, not the codec); it reads 1.20x in
the latest run, but treat that cell as a tie that moves with the baseline.
The `Message`-wrapper cut (no per-decode `AtomicBoolean`, no volatile open
flag) removes the binding's own per-call overhead on top, so both narrow tiers
track the codec instead of the wrapper. From 2.5
KB up both ops are ahead at every tier.

#### PHP vs the official PHP runtime (pure + C extension)

![PHP vs the official PHP runtime (pure + C extension) — MB/s by tier](docs/charts/bench-php-vs-the-official-php-runtime-pure-c-extension.svg)

The native C extension publishes three native rows on the same payload: the
per-field full-task handle (`pbwire-php-tree`: fresh `Message` per call, one
`set`/`get` per field, `close` inside the timer), the native-array entry
(`pbwire-php-shim`, one extension entry that walks the array in C), and the
prebuilt `proxy` row (held-handle re-encode, bare parse; same-run timing only,
published, never the headline). The `ext-ffi` wrapper is the no-compiler
deployment and gets its own row (`pbwire-php-shim-ffi`). Formal run
`results/bench-php-20261122T022541Z.json`: the C-extension pass first, then
the `ext-ffi` pass against the same C-extension baseline, then the `ext-ffi`
pass against the pure-PHP baseline.

| payload | op | pbwire-php-tree (MB/s, x protobuf-php-c) | pbwire-php-shim (MB/s, x) | protobuf-php-c (MB/s) |
|---|---|---|---|---|
| 136 B | encode | 73.9 (0.09x ❌) | 173.8 (0.21x ❌) | 830.2 |
| 136 B | decode | 117.0 (0.29x ❌) | 159.0 (0.39x ❌) | 404.0 |
| 174 B | encode | 83.1 (0.09x ❌) | 197.9 (0.21x ❌) | 963.2 |
| 174 B | decode | 103.4 (0.24x ❌) | 176.1 (0.41x ❌) | 430.0 |
| 2469 B | encode | 82.2 (0.06x ❌) | 196.7 (0.15x ❌) | 1308.3 |
| 2469 B | decode | 54.0 (0.11x ❌) | 187.9 (0.37x ❌) | 503.9 |
| 74913 B | encode | 84.7 (0.07x ❌) | 211.7 (0.17x ❌) | 1282.2 |
| 74913 B | decode | 53.3 (0.09x ❌) | 212.5 (0.37x ❌) | 568.9 |

The `proxy` rows in the same C-extension pass stay above the official C
extension at every tier (encode 1387.6 / 1465.5 / 1672.0 / 1612.8 MB/s, 1.67x /
1.52x / 1.28x / 1.26x; decode 747.3 / 739.2 / 674.8 / 713.2 MB/s, 1.85x /
1.72x / 1.34x / 1.25x; same-run timing only, never the headline): the held
form remains the hot loop while the per-field walk costs ~19x the re-encode at
xlarge, which is the gap the old prebuilt `tree` row was hiding.
| 74913 B | decode | 750.8 (1.29x ✅) | 206.9 (0.36x ❌) | 580.5 |

`pbwire-php-tree` is now the per-field full task, not the shipped fast path:
built messages still live in a per-message arena and decoded trees keep the
core's single-malloc arena, but every call pays one `set`/`get` per field
(encode 0.06x-0.09x, decode 0.09x-0.29x of the official C extension).
`pbwire-php-shim` is the task a PHP application runs when its
data is an array: one extension entry walks the array in C
(`zend_hash_str_find` per field, recursion) and fills the tree, or materializes
the array on decode. It is the fastest native-in/native-out arm on every tier
on both ops (0.15x-0.21x encode, 0.37x-0.41x decode), but below the official C
extension at every tier because every field still costs a hashtable lookup plus
a C tree store, and the decode materializes the array eagerly.

The `ext-ffi` wrapper runs the same array API through one FFI trampoline per
field (the no-compiler deployment), so its row is interpreter-scale; the
official C extension is the baseline in that same run:

| payload | op | pbwire-php-tree (MB/s, x protobuf-php-c) | pbwire-php-shim-ffi (MB/s, x) | protobuf-php-c (MB/s) |
|---|---|---|---|---|
| 136 B | encode | 13.8 | 5.0 | 863.8 |
| 136 B | decode | 14.3 | 5.7 | 425.5 |
| 174 B | encode | 13.5 | 5.2 | 971.4 |
| 174 B | decode | 11.8 | 5.7 | 433.1 |
| 2469 B | encode | 8.7 | 5.0 | 1298.5 |
| 2469 B | decode | 5.0 | 4.3 | 503.6 |
| 74913 B | encode | 9.0 | 5.5 | 1265.0 |
| 74913 B | decode | 4.9 | 4.4 | 549.6 |

The `tree` column here is the same per-field API through the FFI trampoline
(one FFI call per `set`/`get`), which is why it reads lower than the C-entry
`tree` row above.

The pure-PHP runtime, timed by a second `php -n` run in the same harness, is a
different race; the FFI tree handle is the pbwire side there:

| payload | op | pbwire-php-tree (MB/s, x protobuf-php) | pbwire-php-shim-ffi (MB/s) | protobuf-php (MB/s) |
|---|---|---|---|---|
| 136 B | encode | 13.5 (3.55x ✅) | 4.8 | 3.8 |
| 136 B | decode | 13.7 (2.14x ✅) | 5.6 | 6.4 |
| 174 B | encode | 13.9 (3.66x ✅) | 5.2 | 3.8 |
| 174 B | decode | 11.9 (1.95x ✅) | 5.5 | 6.1 |
| 2469 B | encode | 8.8 (2.38x ✅) | 5.2 | 3.7 |
| 2469 B | decode | 5.0 (0.94x ❌) | 4.5 | 5.3 |
| 74913 B | encode | 9.4 (2.54x ✅) | 5.5 | 3.7 |
| 74913 B | decode | 5.0 (0.89x ❌) | 4.6 | 5.6 |

The pure-PHP runtime spends its time on per-field bookkeeping (~4-6 MB/s flat
across tiers). Against it the FFI-run `tree` row -- the same per-field API,
no C extension -- is 2.4x-3.7x on encode and 0.9x-2.1x on decode across the
tiers (2.54x / 0.89x at xlarge), and the `proxy` row in this pass is 428x /
136x there (1582.3 / 763.3 MB/s): quoting the held-handle number here would be
quoting the C core through FFI, not the interpreter, so the native `tree` row
stays the comparison. Both shim builds (C extension and
`ext-ffi`) produce identical bytes and pass the pbench gate on all four tiers;
the gap between them is the per-field PHP loop plus the FFI trampoline, not a
different codec.

#### Dart vs the `protobuf` Dart package

![Dart vs the protobuf Dart package — MB/s by tier](docs/charts/bench-dart-vs-the-protobuf-dart-package.svg)

| payload | op | pbwire-dart-tree (MB/s, x protobuf-dart) | pbwire-dart-shim (MB/s, x protobuf-dart) | pbwire-dart-core (MB/s, x protobuf-dart) | protobuf-dart (MB/s) |
|---|---|---|---|---|---|
| 136 B | encode | 91.8 (0.46x ❌) | 1336.5 (6.64x ✅) | 64.2 (0.32x ❌) | 201.4 |
| 136 B | decode | 87.7 (0.45x ❌) | 972.6 (4.97x ✅) | 72.1 (0.37x ❌) | 195.8 |
| 174 B | encode | 85.9 (0.49x ❌) | 1217.1 (6.97x ✅) | 69.3 (0.40x ❌) | 174.6 |
| 174 B | decode | 90.7 (0.52x ❌) | 859.9 (4.93x ✅) | 79.2 (0.45x ❌) | 174.6 |
| 2469 B | encode | 48.6 (0.39x ❌) | 1095.5 (8.77x ✅) | 313.9 (2.51x ✅) | 124.9 |
| 2469 B | decode | 446.0 (3.19x ✅) | 684.6 (4.89x ✅) | 180.5 (1.29x ✅) | 140.0 |
| 74913 B | encode | 52.4 (0.50x ❌) | 1152.1 (10.99x ✅) | 501.8 (4.78x ✅) | 104.9 |
| 74913 B | decode | 715.6 (4.57x ✅) | 752.9 (4.81x ✅) | 222.3 (1.42x ✅) | 156.4 |

All three pbwire columns are the native-in/native-out full task
(`bindings/dart/pbench/`, formal run
`results/bench-dart-20261121T185927Z.json`): `tree` is the per-field walk
(one `set` per field, `encode()`, `close()` per iteration; decode plus a full
read-back), `shim` is the same task in one FFI call over the flat
`<Message>Layout`, and `core` runs the same task on the pure-Dart codec. The
`proxy` rows in the same run time the prebuilt task instead (held-handle
re-encode, bare decode-and-close; same-run timing only, published, never the
headline): encode 808.4 / 888.9 / 1315.3 / 1616.4 MB/s
(4.01x / 5.09x / 10.53x / 15.41x), decode 365.6 / 407.4 / 634.8 / 729.5 MB/s
(1.87x / 2.33x / 4.53x / 4.66x).

`pbwire-dart-tree` is the per-field full task, published as the reference: one
FFI `set` per field plus `encode`/`close` per iteration, so it pays the
per-field crossing on encode at every tier (0.39x-0.50x); decode wins once the
payload amortizes the crossings (0.45x / 0.52x at 136/174 B, 3.19x / 4.57x at
2469 B / 74 KB). `pbwire-dart-shim` is the shipped native-object path: the
generated `<Message>Layout` is a `dart:ffi` `Struct`, and
`bindings/dart/native/pbw_dart_shim.c` walks that image in place through the C
binding's `pbw_struct` codec -- one FFI call per op, no per-field crossing. It
is above the baseline at every tier (6.6x-11.0x encode, 4.8x-5.0x decode)
because
the codec runs in C and the boundary is paid once; only the `Uint8List` copy of
the result remains. The shim row is the headline for native objects; the
`proxy` row beside it (15.41x / 4.66x at 74 KB) is the prebuilt shape for
applications that already hold a tree, never the headline.

`pbwire-dart-core` is the pure-Dart core (`bindings/dart/lib/core/`, no
`dart:ffi`, no `dart:io`): a `Msg` tree with BigInt for the 64-bit kinds, so it
also runs on dart2js and dart2wasm where the shared library cannot be reached.
On encode it beats the generated encoder from 2469 B up and is 4.78x the
baseline at 74 KB; it loses the two small tiers (0.32x, 0.40x) and the small
decodes (0.37x, 0.45x) to per-call allocation -- one `Msg` with a 22-slot list per message,
one growable list per repeated field, one dynamic slot lookup per field where
the generated reader writes a compiled field. It is (b) in the pbench contract
(a host with no native library), never a replacement for the FFI rows.


#### Rust vs prost (and `rust-protobuf` 3.x)

![Rust vs prost (and rust-protobuf 3.x) — MB/s by tier](docs/charts/bench-rust-vs-prost-and-rust-protobuf-3-x.svg)

| payload | op | pbwire-rust-tree (MB/s, x prost) | pbwire-rust-shim (MB/s, x prost) | pbwire-rust-core (MB/s, x prost) | prost (MB/s) | protobuf-rust (MB/s) |
|---|---|---|---|---|---|---|
| 136 B | encode | 153.9 (0.09x ❌) | 1818.1 (1.11x ✅) | 1012.2 (0.62x ❌) | 1631.8 | 1308.0 |
| 136 B | decode | 727.4 (0.82x ❌) | 1220.5 (1.38x ✅) | 418.9 (0.47x ❌) | 885.6 | 753.1 |
| 174 B | encode | 140.8 (0.09x ❌) | 1645.8 (1.05x ✅) | 1013.2 (0.64x ❌) | 1572.4 | 1325.4 |
| 174 B | decode | 640.6 (1.04x ✅) | 1050.5 (1.70x ✅) | 356.3 (0.58x ❌) | 618.9 | 587.9 |
| 2469 B | encode | 82.1 (0.06x ❌) | 1161.4 (0.87x ❌) | 901.2 (0.67x ❌) | 1337.2 | 807.0 |
| 2469 B | decode | 666.8 (1.89x ✅) | 681.8 (1.93x ✅) | 231.2 (0.65x ❌) | 353.4 | 367.7 |
| 74913 B | encode | 79.3 (0.06x ❌) | 1192.2 (0.88x ❌) | 951.3 (0.70x ❌) | 1350.6 | 877.5 |
| 74913 B | decode | 753.3 (1.99x ✅) | 762.4 (2.01x ✅) | 240.1 (0.63x ❌) | 378.4 | 390.0 |

The archive is linked in, so a call is a direct C call with no `dlopen` lookup.
All three pbwire columns are the native-in/native-out full task (formal run
`results/bench-rust-20261121T184711Z.json`). The `proxy` rows in the same run
time the prebuilt task instead (held-handle `encode_to`, bare `decode` with no
read-back; same-run timing only, published, never the headline): encode
2029.7 / 1884.6 / 1745.2 / 1764.5 MB/s (1.24x / 1.20x / 1.31x / 1.31x of
prost), decode 1088.0 / 914.9 / 687.4 / 748.6 MB/s
(1.23x / 1.48x / 1.95x / 1.98x).
`pbwire-rust-tree` is the per-field full task (fresh `fill` build per
iteration through the handle API, then `encode`; decode plus a full read-back),
so its encode is the per-field build price (0.06x-0.09x of prost) while its
decode stays the application path and the headline (0.82x at 136 B, then
1.04x / 1.89x / 1.99x). `pbwire-rust-shim` is the
generated `#[repr(C)]` mirror read and written by the C struct codec in one
call per op, with the image and its decode arena allocated once. prost, the
fastest widely used Rust runtime (generated at build time by build.rs, bytes
gated like the rest), is behind the `shim` decode on every cell and behind its
encode on the two small tiers; the prebuilt-handle `proxy` row still beats
prost on every encode tier (1.20x-1.31x), which is the price of the fresh
per-field `fill` build the `tree` encode pays.

`pbwire-rust-core` is the pure-Rust codec of `bindings/rust/src/core/` behind
the `core` cargo feature (off by default, so the crate keeps the static link):
one `Message` tree in and wire bytes out, wire bytes in and a fully readable
tree out, with no C library, no `unsafe` and no generated code, held to the same
official binary suite as the C core (`make conformance-rust`: 1412 successes,
1403 skipped, 2 expected, 0 unexpected). It is behind prost on every cell
(0.62-0.70x encode, 0.47-0.65x decode) and ahead of `rust-protobuf` on the two
large encodes (1.12x / 1.08x): encode allocates nothing, while
decode pays the boxed tree a schema-driven
codec needs -- a slot vector per message, a boxed sub-message and one owned byte
vector per string, ~12k allocations for the 74 KB payload. It is published as
the no-C-toolchain mode, not as a replacement for the static link: the native
`tree` decode (the shipped path) is 1.74x / 1.80x / 2.88x / 3.14x its decode
at the four tiers.

The shim's decode leads prost at every tier (1.38x at 136 B, 2.01x at 74 KB)
and its encode leads at 136 B (1.11x), is level at 174 B (1.05x), and trails at
2469 B/74913 B (0.87x/0.88x): that is the shared C struct encoder's
per-repeated-element reserve path (`make bench-c` measures the same codec at
1132.2 MB/s on 74913 B), while prost's generated writer is straight-line code
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

| payload | op | pbwire-csharp-tree (MB/s, x protobuf-csharp) | pbwire-csharp-shim (MB/s, x protobuf-csharp) | protobuf-csharp (MB/s) | protobuf-net (MB/s) |
|---|---|---|---|---|---|
| 136 B | encode | 171.0 (0.38x ❌) | 1162.7 (2.58x ✅) | 450.9 | 541.8 |
| 136 B | decode | 211.8 (0.45x ❌) | 728.7 (1.54x ✅) | 473.6 | 442.4 |
| 174 B | encode | 147.9 (0.36x ❌) | 985.7 (2.42x ✅) | 407.3 | — |
| 174 B | decode | 157.2 (0.39x ❌) | 594.0 (1.48x ✅) | 400.4 | — |
| 2469 B | encode | 83.1 (0.15x ❌) | 594.4 (1.05x ✅) | 564.6 | 397.2 |
| 2469 B | decode | 526.6 (1.11x ✅) | 345.0 (0.73x ❌) | 474.9 | 304.1 |
| 74913 B | encode | 81.1 (0.17x ❌) | 658.3 (1.36x ✅) | 483.7 | 370.7 |
| 74913 B | decode | 732.1 (1.49x ✅) | 346.8 (0.70x ❌) | 492.3 | 319.5 |

All three pbwire columns are the native-in/native-out full task
(`bindings/csharp/pbench/`, formal run
`results/bench-csharp-20261121T185300Z.json`). The `proxy` rows in the same run
time the prebuilt task instead (held-handle re-encode, bare decode with no
read-back; same-run timing only, published, never the headline): encode
1431.1 / 1459.1 / 1307.7 / 1675.2 MB/s (3.17x / 3.58x / 2.32x / 3.46x of
protobuf-csharp), decode 862.2 / 810.4 / 657.4 / 735.8 MB/s
(1.82x / 2.02x / 1.38x / 1.50x).

`pbwire-csharp-tree` is the per-field full task (fresh handle per iteration,
one shipped `Set()` per field, `Encode()`, `Dispose()` inside the timer; decode
reads every gate-checked field back through `ConsumeTree`), so its encode is
the per-field P/Invoke price (0.15x-0.38x) while its decode wins from 2469 B up
(1.11x / 1.49x), where one C decode beats the generated parser even with the
per-field read-back. `pbwire-csharp-shim` is the shipped native-object path:
the generated model is copied into one contiguous unmanaged image per op and
**one** `P/Invoke` (`bindings/csharp/native/pbw_cs_shim.c`, built by
`make csharp-shim-native`) walks it through the C layout codec, so no field
crosses the boundary. It is above Google.Protobuf on every encode tier
(1.05x-2.58x) and on the two small decode tiers (1.48x-1.54x). The 2469 B and
74913 B decode rows are below the baseline (0.73x, 0.70x): the C decode itself
is faster than the reference parser, but materialising 3000 ints, 3000 strings,
3000 sub-messages and 3000 bools into managed objects costs more than the
reference parser's whole run. protobuf-net, the other widely used C#
runtime (hand-written contracts in Pbn.cs, bytes gated like the rest), is
slower than Google.Protobuf on every tier here (xlarge 365.1 vs 484.3
encode): the official runtime is the fastest baseline, so the ratio column
is against it. Its medium rows are
absent on purpose: protobuf-net writes single-element packed fields in
singular form — valid protobuf, semantically equal (the harness checks that
via the Google parser), but not byte-identical, so there is nothing MB/s-fair
to time there.

The native `shim` rows land at ~660 MB/s encode and ~350 MB/s decode on the
xlarge tier: by 2.5 KB the per-call boundary is invisible and the C layout
codec plus the managed materialisation are what is being measured. That is the
point of the schema-at-runtime design -- the binding owns ergonomics, the core
owns the format.

The third arm is the pure-C# core (`bindings/csharp/Pbwire/Core/`,
`make conformance-csharp`): descriptor reader, tree, encoder and decoder in
managed code, no `DllImport`, no native library. Formal `make csharp-bench`,
same run (`results/bench-csharp-20261121T185300Z.json`, 2026-11-21), MB/s and
the ratio against protobuf-csharp in that run:

| payload | op | pbwire-csharp-core (MB/s) | x protobuf-csharp |
|---|---|---|---|
| 136 B | encode | 943.2 | 2.09x ✅ |
| 136 B | decode | 539.3 | 1.14x ✅ |
| 174 B | encode | 847.9 | 2.08x ✅ |
| 174 B | decode | 458.3 | 1.14x ✅ |
| 2469 B | encode | 586.5 | 1.04x ✅ |
| 2469 B | decode | 345.0 | 0.73x ❌ |
| 74913 B | encode | 649.1 | 1.34x ✅ |
| 74913 B | decode | 306.3 | 0.62x ❌ |

An in-language codec that walks a schema table beats the generated reader and
writer on every encode tier and on the two small decodes. It loses the two
large decodes to its own tree: every element is a boxed slot, a `Raw` slice or
a `Msg`, so 3000 ints + 3000 strings + 3000 sub-messages + 3000 bools cost
more than the reference parser's straight-line `ParseFrom`, while the native
`tree` decode pays the read-back but no image copy. The core is still close to
the shim's image round trip on both ops at 74 KB (306 / 347 decode, 649 / 658
encode), because nothing is copied twice. The official binary suite is green through it
(`make conformance-csharp`: 1412 successes, 1403 skipped for JSON/text, 2
expected failures, 0 unexpected), which is the bar for publishing this row.
The verdict and the Stage 6 decision are in `bindings/csharp/pbench/README.md`
and `docs/perf-attempts.md`: the core is the no-native-library arm (and F#'s
fallback), never the headline.

### Lua (C module, tables) vs `lua-protobuf`

![Lua (C module, tables) vs lua-protobuf — MB/s by tier](docs/charts/bench-lua-c-module-tables-vs-lua-protobuf.svg)

| payload | op | pbwire-lua (MB/s, x lua-protobuf) | lua-protobuf (MB/s) |
|---|---|---|---|
| 136 B | encode | 144.3 (1.19x ✅) | 121.6 |
| 136 B | encode_msg | 993.8 (8.17x ✅) | 121.6 |
| 136 B | decode | 148.7 (1.66x ✅) | 89.5 |
| 174 B | encode | 164.5 (1.22x ✅) | 135.3 |
| 174 B | encode_msg | 1043.4 (7.71x ✅) | 135.3 |
| 174 B | decode | 148.9 (1.66x ✅) | 89.9 |
| 2469 B | encode | 190.1 (1.13x ✅) | 168.5 |
| 2469 B | encode_msg | 1431.1 (8.49x ✅) | 168.5 |
| 2469 B | decode | 137.8 (1.53x ✅) | 90.2 |
| 74913 B | encode | 209.4 (1.12x ✅) | 186.3 |
| 74913 B | encode_msg | 1583.5 (8.50x ✅) | 186.3 |
| 74913 B | decode | 131.4 (1.40x ✅) | 93.8 |

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
way), the other columns are the two baselines side by side. The `pbwire-erlang`
rows are the `tree` arm (NIF handle); gpb, the pure-Erlang codec, **is** the
`core` comparison, so a second BEAM port would measure our code against the
competitor rather than a decision.

| payload | op | pbwire-erlang (MB/s, x best) | gpb (MB/s) | gpb-nif (MB/s) |
|---|---|---|---|---|
| 136 B | encode | 502.2 (2.43x ✅) | 206.3 | 85.1 |
| 136 B | decode | 541.7 (2.27x ✅) | 238.3 | 97.5 |
| 174 B | encode | 451.3 (3.14x ✅) | 143.7 | 101.5 |
| 174 B | decode | 486.6 (2.47x ✅) | 196.7 | 118.5 |
| 2469 B | encode | 415.4 (3.44x ✅) | 120.7 | 88.1 |
| 2469 B | decode | 325.0 (1.76x ✅) | 185.1 | 131.4 |
| 74913 B | encode | 483.3 (3.91x ✅) | 123.6 | 121.4 |
| 74913 B | decode | 356.5 (1.60x ✅) | 222.6 | 209.0 |

### Swift (SwiftPM, static link) vs `apple/swift-protobuf`

![Swift (SwiftPM, static link) vs apple/swift-protobuf — MB/s by tier](docs/charts/bench-swift-swiftpm-static-link-vs-apple-swift-protobuf.svg)

| payload | op | pbwire-swift-tree (MB/s, x swift-protobuf) | pbwire-swift-shim (MB/s, x swift-protobuf) | pbwire-swift-core (MB/s, x swift-protobuf) | swift-protobuf (MB/s) |
|---|---|---|---|---|---|
| 136 B | encode | 123.6 (0.28x ❌) | 974.9 (2.18x ✅) | 241.1 (0.54x ❌) | 448.0 |
| 136 B | decode | 311.4 (1.25x ✅) | 769.9 (3.09x ✅) | 98.6 (0.40x ❌) | 249.1 |
| 174 B | encode | 115.9 (0.35x ❌) | 966.5 (2.91x ✅) | 223.8 (0.67x ❌) | 331.8 |
| 174 B | decode | 274.5 (1.37x ✅) | 707.7 (3.53x ✅) | 91.2 (0.45x ❌) | 200.6 |
| 2469 B | encode | 69.1 (0.26x ❌) | 994.1 (3.80x ✅) | 247.3 (0.95x ❌) | 261.3 |
| 2469 B | decode | 614.5 (3.25x ✅) | 534.4 (2.83x ✅) | 63.9 (0.34x ❌) | 188.8 |
| 74913 B | encode | 66.3 (0.24x ❌) | 915.1 (3.31x ✅) | 247.7 (0.90x ❌) | 276.2 |
| 74913 B | decode | 578.9 (2.68x ✅) | 580.9 (2.69x ✅) | 65.9 (0.30x ❌) | 216.3 |

All three pbwire columns are the native-in/native-out full task (formal run
`results/bench-swift-20261121T193722Z.json`). The `proxy` rows in the same run
time the prebuilt task instead (held-handle `encode`, bare decode with no
read-back; same-run timing only, published, never the headline): encode
1047.5 / 1128.3 / 1457.9 / 1675.1 MB/s (2.34x / 3.40x / 5.58x / 6.07x),
decode 952.3 / 848.4 / 680.1 / 610.1 MB/s (3.82x / 4.23x / 3.52x / 2.82x).
Proxy/tree encode gap: 8.5x / 9.7x / 21.1x / 25.3x -- the gap the old prebuilt
`tree` row was hiding. Pairing note: the formal run prints the four native
rows without the baseline in its first case set and the `tree` row against
`swift-protobuf` in its second; the ratios above pair each four-arm MB/s with
the same run's own baseline MB/s (e.g. xlarge tree 66.3 vs 276.2 = 0.24x).

FFI host like Objective-C/Dart: `FileDescriptorSet` at run time, C ABI calls
only, no wire code, no plugin. SwiftPM links `build/libpbwire.a` via
`unsafeFlags` (so plain `swift test` works offline); the baseline is a
separate package using swift-protobuf's own build plugin, so no generated
code is checked in. The gate is byte parity per tier plus the pinned
`matrix3.bin` oracle.

`pbwire-swift-tree` is the per-field full task (fresh message plus `fillTree`
build per iteration, one `set` per field, byte count folded into a live sink;
decode plus `consumeTree`, which folds every gate-checked field into the sink),
so its encode is the per-field FFI price (0.24x-0.35x) while its decode beats
the baseline at every tier (1.25x-3.25x).
`pbwire-swift-shim` times the generated `@frozen <Message>Repr` mirror with one
`encodeInPlace`/`decodeInPlace` call into `bindings/c/runtime/pbw_struct.c`
(compiled into the package's `CPbwire` target), so no field and no ARC crosses
per field. The `proxy` row holds the prebuilt-handle price (6.07x / 2.82x at
74 KB), published beside the native rows and never headlined; the shim answers
the native-object question at 2.2-3.8x encode and 2.7-3.5x decode.

`pbwire-swift-core` is the pure-Swift `PbwireCore` product
(`bindings/swift/Sources/PbwireCore/`): a descriptor reader, value tree, encoder
and decoder in Swift, no C module, no `libpbwire.a` and no `unsafeFlags`, so it
can be a normal SwiftPM dependency and a build with no C toolchain can use it.
It is not the fast path — 0.54-0.95x encode and 0.30-0.45x decode of
swift-protobuf, the cost of the value tree's ARC — and it is published as the
no-native mode; `make conformance-swift` drives the official binary suite
through it (1412 successes, 1403 skipped, 2 expected, 0 unexpected, the same
failure set as the C core). Numbers are
`results/bench-swift-20261121T025449Z.json`; the `core` row's stage 6 round and
the remaining gap are recorded in `docs/perf-attempts.md`.

### Scala (Java runtime + DSL) vs ScalaPB

![Scala (Java runtime + DSL) vs ScalaPB — MB/s by tier](docs/charts/bench-scala-java-runtime-dsl-vs-scalapb.svg)

| payload | op | pbwire-scala-tree (MB/s, x scalapb) | pbwire-scala-core (MB/s) | scalapb (MB/s) |
|---|---|---|---|---|
| 136 B | encode | 1290.2 (0.81x ❌) | 587.1 | 1598.8 |
| 136 B | decode | 775.5 (1.34x ✅) | 336.6 | 578.1 |
| 174 B | encode | 1293.7 (1.37x ✅) | 494.2 | 942.0 |
| 174 B | decode | 702.3 (1.00x ✅) | 244.3 | 700.2 |
| 2469 B | encode | 1000.5 (1.44x ✅) | 310.6 | 694.6 |
| 2469 B | decode | 661.4 (1.22x ✅) | 160.9 | 542.7 |
| 74913 B | encode | 1050.5 (1.45x ✅) | 334.6 | 723.2 |
| 74913 B | decode | 707.4 (1.37x ✅) | 169.7 | 515.1 |

Same class as Kotlin: the JVM binding reuses the Java runtime and adds only
an idiomatic DSL — no second codec, no JNI, schema from a `FileDescriptorSet`
at run time. The baseline is `scalapbc`-generated code (needs `cs install
scalapbc` once); the jars land in `benchmarks/deps`. Byte parity per tier
before timing, like every other row. `core` is the committed generated
`matrix3Gen` case class; its `encode`/`decode` fill and load a `Message`
field by field by name, so it is the slowest JVM `core` row (0.37x at 136 B,
0.46x at 74 KB encode, 0.33x-0.58x decode): the translation is the cost, not
the wire engine. `tree` stays the headline. `shim` for the JVM four:
`bindings/java/pbench/` (`jni/`, one crossing, no tree translation -- a
ceiling, not a binding cost).

### Elixir (NIF over the shared core) vs `elixir-protobuf`

![Elixir (NIF over the shared core) vs elixir-protobuf — MB/s by tier](docs/charts/bench-elixir-nif-over-the-shared-core-vs-elixir-protobuf.svg)

| payload | op | pbwire-elixir (MB/s, x elixir-protobuf) | elixir-protobuf (MB/s) |
|---|---|---|---|
| 136 B | encode | 137.8 (3.06x ✅) | 45.0 |
| 136 B | decode | 100.7 (1.80x ✅) | 56.0 |
| 174 B | encode | 141.0 (3.06x ✅) | 46.1 |
| 174 B | decode | 104.3 (1.78x ✅) | 58.5 |
| 2469 B | encode | 144.5 (2.58x ✅) | 55.9 |
| 2469 B | decode | 106.4 (1.99x ✅) | 53.4 |
| 74913 B | encode | 152.6 (2.56x ✅) | 59.6 |
| 74913 B | decode | 105.7 (1.90x ✅) | 55.6 |

Same class as Erlang: the BEAM binding reuses the published `:pbwire` Hex
NIF (the same C core as `bindings/erlang`) and owns only the struct↔tuple
translation; `elixir-protobuf` is the schema plugin, like gpb is for Erlang, and
its pure-Elixir rows **are** the `core` comparison for this binding.
Every encode is byte-identical to `Protobuf.encode/1` before timing. Known
deliberate divergences from the oracle: NaN bit identity, unknown-field drop
(the NIF tuple form has no storage), raise on missing `required`.

The struct build is one pass over the NIF's record: the per-position field
tables are built once per module at schema load, so a decode indexes them
instead of walking two lists through `Tuple.to_list` + `Enum.zip` + `Enum.map`
+ a list concat. That is what lifted the xlarge decode row off the 0.99x floor
(see the ledger).

### Zig (static link) vs Arwalk/zig-protobuf

![Zig (static link) vs Arwalk/zig-protobuf — MB/s by tier](docs/charts/bench-zig-static-link-vs-arwalk-zig-protobuf.svg)

| payload | op | pbwire-zig-tree (MB/s, x zig-protobuf) | pbwire-zig-shim (MB/s, x zig-protobuf) | pbwire-zig-core (MB/s, x zig-protobuf) | zig-protobuf (MB/s) |
|---|---|---|---|---|---|
| 136 B | encode | 324.9 (0.24x ❌) | 1912.4 (1.41x ✅) | 574.2 (0.42x ❌) | 1357.5 |
| 136 B | decode | 766.0 (0.88x ❌) | 1257.7 (1.44x ✅) | 816.4 (0.93x ❌) | 875.0 |
| 174 B | encode | 287.0 (0.27x ❌) | 1656.2 (1.55x ✅) | 571.4 (0.54x ❌) | 1066.1 |
| 174 B | decode | 626.6 (0.85x ❌) | 1076.2 (1.46x ✅) | 667.0 (0.91x ❌) | 735.0 |
| 2469 B | encode | 183.2 (0.21x ❌) | 1166.7 (1.33x ✅) | 674.4 (0.77x ❌) | 878.0 |
| 2469 B | decode | 664.9 (1.08x ✅) | 699.8 (1.14x ✅) | 335.0 (0.54x ❌) | 615.9 |
| 74913 B | encode | 164.0 (0.19x ❌) | 1227.4 (1.45x ✅) | 741.2 (0.88x ❌) | 844.0 |
| 74913 B | decode | 723.8 (1.34x ✅) | 788.6 (1.46x ✅) | 357.0 (0.66x ❌) | 540.5 |

All three pbwire columns are the native-in/native-out full task (formal run
`results/bench-zig-20261122T014913Z.json`). The `proxy` rows in the same run
time the prebuilt task instead (held-handle `encodeInto`, bare `decode` with
no read-back; same-run timing only, published, never the headline): encode
2099.8 / 1969.4 / 1739.6 / 1643.2 MB/s, decode
1111.3 / 963.5 / 685.0 / 716.4 MB/s. Proxy/tree encode gap at xlarge: 10.0x
-- the gap the old prebuilt `tree` row was hiding.

Same class as Rust: the core sources compile into a static archive that links
into one self-contained binary, schema from a `FileDescriptorSet` at run
time. The baseline is Arwalk/zig-protobuf 7.0.0 generated code from the same
`matrix3.proto` (tarball pinned in `build.zig.zon`, fetched on first run and
cached; generation runs the system `protoc` against their plugin, and the
`test` / plain `bench` steps never resolve the dependency, so they stay
offline-safe — `bench.sh` runs pbwire-only by default, `--baseline` (what
`make zig-bench` passes) runs the comparison). Bytes are gated equal on all
four tiers before timing. `pbwire-zig-tree` is the per-field full task (fresh
handle per iteration, one `set` per field, `encodeInto`; decode plus a full
read-back), so its encode is the per-field build price (0.19x-0.27x) while its
decode wins from 2469 B up (1.08x / 1.34x). `pbwire-zig-shim` times the
generated
`extern struct <Message>Mirror` with one `encodeInPlace`/`decodeInPlace` call
into `bindings/c/runtime/pbw_struct.c` (compiled into the same archive), so no
field crosses per field. The shim is above the baseline on every cell; the `proxy` row holds
the prebuilt-handle price (10.0x the fresh native build on xlarge encode), published
beside the native rows and never headlined.

`pbwire-zig-core` is the pure-Zig codec (`bindings/zig/core/`, selected by
`-Dcore=true`): its own `FileDescriptorSet` reader, value tree and binary
encoder/decoder, with no C source, no translate-c and no `libpbwire.a`.
`make conformance-zig-core` gates it on the official binary suite (1412
successes, 1403 skipped, 2 expected, 0 unexpected). It is below the generated
baseline on every cell (0.42-0.88x encode, 0.54-0.93x decode): the encode cost
is the tag/length writer appending without the C encoder's reserve pass, and
the decode cost is the value tree's arena and per-field allocations, the same
shape the Rust and Swift cores report. The default artifacts keep linking the
C core; the extra `-Dcore=true` artifact is the no-C build.

The tree's encode lead is the generated straight-line writer versus one small
opcode dispatch per field, ~2x at most. The baseline's decode is structural:
it materializes every field with its own allocation (which is also what the
small tiers time most of), while the binding decodes into one arena; the lead
shrinks as the payload grows and per-byte work takes over. At
`-Doptimize=ReleaseFast` (what `bench.sh` runs) both sides sit near the core's
pace; the default debug build is ~20x slower and is not timed.

### F# (facade over the C# runtime) vs Google.Protobuf

![F# (facade over the C# runtime) vs Google.Protobuf — MB/s by tier](docs/charts/bench-f-facade-over-the-c-runtime-vs-google-protobuf.svg)

| payload | op | pbwire-fsharp-tree (MB/s, x Google.Protobuf) | pbwire-fsharp-shim (MB/s, x) | Google.Protobuf (MB/s) |
|---|---|---|---|---|
| 136 B | encode | 1442.0 (3.14x ✅) | 1180.6 (2.57x ✅) | 459.7 |
| 136 B | decode | 868.4 (1.73x ✅) | 760.2 (1.51x ✅) | 502.5 |
| 174 B | encode | 1422.3 (3.50x ✅) | 966.1 (2.37x ✅) | 406.8 |
| 174 B | decode | 787.0 (1.94x ✅) | 590.5 (1.45x ✅) | 406.0 |
| 2469 B | encode | 1461.5 (2.79x ✅) | 643.0 (1.23x ✅) | 523.6 |
| 2469 B | decode | 691.5 (3.62x ✅) | 320.8 (1.68x ✅) | 191.1 |
| 74913 B | encode | 671.2 (1.35x ✅) | 658.3 (1.33x ✅) | 496.1 |
| 74913 B | decode | 722.6 (1.40x ✅) | 354.6 (0.69x ❌) | 515.0 |

Same class as Kotlin: F# reuses the C# binding's project via
`ProjectReference` and adds only an F#-idiomatic facade — no second runtime,
no packages, schema from a `FileDescriptorSet` at run time. The baseline is
`protoc`-generated C# compiled in a sibling project (F# cannot compile
`.cs`). Byte parity per tier before timing. Both rows come from the C#
runtime: `pbwire-fsharp-tree` is the `Message` handle the facade exposes,
`pbwire-fsharp-shim` is the C# generated model's native-object entry
(`bindings/csharp/pbench/Shim.cs`), i.e. the same one-call C reader over the
flat image, so F# has no separate port and no second wire implementation. It is
above Google.Protobuf on every encode tier and on three of four decode tiers;
the xlarge decode row carries the managed materialisation cost the C# section
describes.

### Clojure (Java runtime + interop) vs `protobuf-java`

![Clojure (Java runtime + interop) vs protobuf-java — MB/s by tier](docs/charts/bench-clojure-java-runtime-interop-vs-protobuf-java.svg)

| payload | op | pbwire-clj-core (MB/s, x protobuf-java) | pbwire-clj-tree (MB/s) | protobuf-java (MB/s) |
|---|---|---|---|---|
| 136 B | encode | 1521.2 (0.68x ❌) | 21.0 | 2232.5 |
| 136 B | decode | 881.8 (1.35x ✅) | 21.1 | 651.3 |
| 174 B | encode | 1396.9 (1.12x ✅) | 19.9 | 1251.2 |
| 174 B | decode | 762.6 (1.34x ✅) | 16.2 | 568.6 |
| 2469 B | encode | 1156.9 (1.35x ✅) | 9.8 | 857.7 |
| 2469 B | decode | 703.4 (1.55x ✅) | 8.2 | 454.9 |
| 74913 B | encode | 1123.1 (1.26x ✅) | 10.0 | 893.1 |
| 74913 B | decode | 742.4 (1.70x ✅) | 8.5 | 436.6 |

`core` is the mutable-message fast path: the prebuilt `Message` (fill once,
encode many; decode plus close), the generated model's native object with no
map walk -- the plan's review focus says the core row must not be a per-call
map conversion. `tree` is the other shipped path, the persistent-map API the
generated model exposes (`matrix3`'s constructor and encode/decode fns): the
map is the Clojure tree, so its walk and materialization are that arm's cost,
~10-21 MB/s (`tree` ratio 0.01x-0.03x). The split is the one the README
documents: maps for convenience, the mutable path for hot loops. `shim` for
the JVM four: `bindings/java/pbench/` (`jni/`, one crossing, no tree
translation -- a ceiling, not a binding cost).

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

| payload | op | pbwire-julia-tree (MB/s, x ProtoBuf.jl) | pbwire-julia-shim (MB/s, x ProtoBuf.jl) | ProtoBuf.jl (MB/s) |
|---|---|---|---|---|
| 136 B | encode | 160.5 (0.42x ❌) | 1307.0 (3.39x ✅) | 385.6 |
| 136 B | decode | 212.5 (0.18x ❌) | 1102.5 (0.93x ❌) | 1181.2 |
| 136 B | dec+ctx | 1140.5 (0.97x ❌) | -- | 1181.2 |
| 174 B | encode | 144.4 (0.45x ❌) | 1243.1 (3.90x ✅) | 319.1 |
| 174 B | decode | 213.0 (0.30x ❌) | 980.5 (1.39x ✅) | 706.0 |
| 174 B | dec+ctx | 958.5 (1.36x ✅) | -- | 706.0 |
| 2469 B | encode | 87.3 (0.70x ❌) | 1040.5 (8.30x ✅) | 125.3 |
| 2469 B | decode | 576.1 (0.93x ❌) | 693.3 (1.12x ✅) | 617.0 |
| 2469 B | dec+ctx | 698.7 (1.13x ✅) | -- | 617.0 |
| 74913 B | encode | 87.9 (7.26x ✅) | 1122.2 (92.74x ✅) | 12.1 |
| 74913 B | decode | 739.8 (1.04x ✅) | 738.9 (1.04x ✅) | 709.1 |
| 74913 B | dec+ctx | 740.5 (1.04x ✅) | -- | 709.1 |

All three columns are the native-in/native-out full task (formal run
`results/bench-julia-20261121T190400Z.json`): `tree` builds a fresh `Message`
per iteration (one `set` per field, then `encode`, `close()` inside the timer;
decode plus a full read-back into a live sink), `shim` walks the same task
through one `ccall` over the generated layout image. The `proxy` rows in the
same run time the prebuilt task instead (held-handle re-encode, bare decode;
same-run timing only, published, never the headline): encode
1757.0 / 1644.6 / 1648.0 / 1694.4 MB/s, decode
1037.8 / 886.2 / 689.9 / 747.2 MB/s. The xlarge encode column is a baseline
collapse (ProtoBuf.jl's generated encoder falls to 12.1 MB/s there), so both
native ratios spike (tree 7.26x, shim 92.74x); the decode row is the honest
comparison (1.04x / 1.04x).

FFI host like Dart/C#: `Libdl.dlopen` + `ccall` into the C ABI (function
pointers resolved once via `dlsym`, since recent Julia needs a compile-time
library name), `Dict`-based messages convert through the C ABI, no wire
code, no `.proto` parser. The baseline is `protojl`-generated code
(`benchmarks/oracle/structtest/matrix3_pb.jl`, produced by
`protojl("matrix3.proto", "<proto dir>", "benchmarks/oracle")`). Its encode
collapses with size (pure-Julia per-field appends).

`pbwire-julia-tree` is the per-field full task: one `set` per field plus an
arena `malloc`/`free` pair per iteration, which is why it sits below the
baseline on the three smaller encode tiers (0.42x / 0.45x / 0.70x) and the
three smaller decode tiers (0.18x / 0.30x / 0.93x) -- that is the per-field
walk issue 41 corrects, not the codec. `pbwire-julia-shim` is the shipped
native-object path: a generated `mutable struct Scalars3Layout` is the object,
and `bindings/julia/native/pbw_julia_shim.c` walks it in place through
`pbw_struct_codec_*`, one `ccall` per op, with the image and caller-memory
arena allocated once. The shim beats the baseline on all four encode tiers
(3.39x at 136 B, 92.74x at 74 KB on the baseline collapse) and on
medium/large/xlarge decode (1.39x / 1.12x / 1.04x). The 136 B decode stays
below (0.93x shim, 0.18x tree): the small
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

The third arm is the pure-Julia core (`bindings/julia/runtime/src/core/`,
`make conformance-julia-core`): descriptor reader, tree, encoder and decoder in
stdlib-only Julia, no `ccall`, no native library, no generated code. Formal
`make julia-bench`, same formal run
(`results/bench-julia-20261121T190400Z.json`, 2026-11-21), MB/s and the ratio
against ProtoBuf.jl in that run:

| payload | op | pbwire-julia-core (MB/s) | x ProtoBuf.jl |
|---|---|---|---|
| 136 B | encode | 39.1 | 0.10x ❌ |
| 136 B | decode | 162.7 | 0.14x ❌ |
| 174 B | encode | 22.3 | 0.07x ❌ |
| 174 B | decode | 47.9 | 0.07x ❌ |
| 2469 B | encode | 28.6 | 0.23x ❌ |
| 2469 B | decode | 25.9 | 0.04x ❌ |
| 74913 B | encode | 29.3 | 2.42x ✅ |
| 74913 B | decode | 24.9 | 0.04x ❌ |

The in-language codec wins the one cell where the baseline wastes work: at
74 KB the generated Julia encoder re-derives every field's size before writing,
while this one writes fused tag+value pairs into one reusable buffer, so it is
2.42x the baseline encode (29.3 vs 12.1 MB/s on the same collapsed baseline
the native rows quote). Every other cell is the price of the portable path:
the published rows convert a plain `Dict` to the tree (encode) or materialize
the whole dict (decode), and that per-field conversion, not the wire walk, is
what an interpreter pays -- `from_dict`/`to_dict` walk a `Vector{Any}` slot
table, decoded strings stay `Raw` slices until read, and ProtoBuf.jl's
generated decoder writes straight into typed struct fields. The official binary
suite is green through it (`make conformance-julia-core`: 1412 successes, 1403
skipped for JSON/text, 2 expected failures, 0 unexpected), which is the bar for
publishing the row. The verdict and the Stage 6 decision are in
`bindings/julia/pbench/README.md` and `docs/perf-attempts.md`: the core is the
no-native-library arm, never the headline.

### R (C extension, named lists) vs RProtoBuf

![R (C extension, named lists) vs RProtoBuf — MB/s by tier](docs/charts/bench-r-c-extension-named-lists-vs-rprotobuf.svg)

| payload | op | pbwire-r-tree (MB/s, x RProtoBuf) | pbwire-r-shim (MB/s, x RProtoBuf) | RProtoBuf (MB/s) |
|---|---|---|---|---|
| 136 B | encode | 29.0 (11.60x ✅) | 56.3 (22.52x ✅) | 2.5 |
| 136 B | decode | 21.8 (8.07x ✅) | 74.7 (27.67x ✅) | 2.7 |
| 174 B | encode | 39.1 (12.61x ✅) | 108.1 (34.87x ✅) | 3.1 |
| 174 B | decode | 29.8 (9.61x ✅) | 101.2 (32.65x ✅) | 3.1 |
| 2469 B | encode | 89.9 (34.58x ✅) | 139.5 (53.65x ✅) | 2.6 |
| 2469 B | decode | 66.6 (22.97x ✅) | 136.8 (47.17x ✅) | 2.9 |
| 74913 B | encode | 108.4 (38.71x ✅) | 156.2 (55.79x ✅) | 2.8 |
| 74913 B | decode | 68.2 (25.26x ✅) | 158.7 (58.78x ✅) | 2.7 |

Same class as Lua/Ruby: a C extension over the core (named lists in/out
through one transient tree per call, statically linked), schema from
a `FileDescriptorSet` at run time. Both pbwire columns are the native-in/native-out
full task (formal run `results/bench-r-20261122T020549Z.json`). The `proxy`
rows in the same run time the prebuilt task instead (prebuilt builder
re-encode, bare parse; same-run timing only, published, never the headline):
encode 274.1 / 367.3 / 1045.1 / 1619.7 MB/s
(109.64x / 118.48x / 401.96x / 578.46x), decode 89.8 / 126.1 / 406.4 / 499.4
MB/s (33.26x / 40.68x / 140.14x / 184.96x).

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

Two rows, named by the arm (`bindings/r/pbench/README.md`):

- **`pbwire-r-shim`** is the task an R application runs: the generated model's
  named list in, wire bytes out. One `.Call` hands the list to C, which walks it
  and fills the tree inside C; the decode is the same one call in reverse and
  returns the fully materialized named list. The type is resolved once into a
  schema-owned message number, so the call does not look a name up per message.
  It is the fastest native-in/native-out arm on every tier, both ops
  (22.5x-55.8x / 27.7x-58.8x vs RProtoBuf).
- **`pbwire-r-tree`** is the per-field-API reference (fresh builder per encode
  plus one set per field, parse plus full read-back per decode). It skips
  nothing the list path skips, so it reads 1.4x-2.8x below the list row -- and
  still 8x-39x above RProtoBuf -- never replaced by `proxy`.

Encode is no contest at any tier: setting 3000 nested messages field by field
through S4 dispatch costs RProtoBuf ~27 ms where the C core walks the list once.
Decode is compared as the same user-visible task: `RProtoBuf::read` parses in C++
and materializes fields lazily on `$`, while `pbwire_decode` builds the full
named-list tree eagerly, so the baseline side reads every field into the same
plain-list shape (the table's `decode` rows). A bare `read()` alone is a few
times faster than `pbwire_decode`, but it hands back a Message, not R data; once
both sides produce plain lists, the list path leads 56x / 59x (encode / decode)
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
| x0 | 52 | 22.4 ns | 146.2 ns | 40.2 ns | 261.4 ns |
| x1 | 55 | 32.1 ns | 157.5 ns | 50.9 ns | 293.5 ns |
| x100 | 154 | 104.1 ns | 826.5 ns | 362.5 ns | 743.5 ns |
| x3000 | 5928 | 3181.5 ns | 25183.0 ns | 11124.0 ns | 19667.0 ns |

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
make go-bench     # Go thin model (shim runtime) vs protobuf-go generated code (needs Go only)
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

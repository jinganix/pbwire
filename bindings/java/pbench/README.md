# pbench: the pure-Java codec against the C core, kept runnable

The Java binding ships its own codec (`runtime/`), so it is the only binding
besides Go where the same payload has two real implementations. This directory
keeps both and times them in one process, so the question that produced it --
"would the binding be faster on the C core through JNI?" -- can be answered
again from numbers instead of memory.

The verdict recorded in [`docs/perf-attempts.md`](../../../docs/perf-attempts.md#rejected-jni-binding-for-java-2026-11-16)
is unchanged: keep the pure-Java codec. What is new is that the harness below is
committed, so the next person (or the next language's template) starts from a
re-runnable gate instead of a throwaway.

## Arms

The normative arm names are the contract's; this is where the Java rows live.

| arm | here | what it times |
|---|---|---|
| `tree` | [`../benchmarks/Bench.java`](../benchmarks/Bench.java), `GenericCodec`/`DMsg` full task | fresh `DMsg` built per field per iteration, encode; decode, every gate-checked field read back per iteration (Java has no `free`; GC owns it) |
| `core` | [`../benchmarks/Bench.java`](../benchmarks/Bench.java), the committed `Structtest_Scalars3` model | native object in and out through the shipped model |
| `proxy` | [`../benchmarks/Bench.java`](../benchmarks/Bench.java), `GenericCodec`/`DMsg` prebuilt | the old prebuilt task, relabelled not re-measured: re-encode a held tree, decode-and-drop with no materialization. Published, never the headline |
| `shim` | `jni/` (`pbwire_jni.c`) | the C core behind one JNI call; it never translates a tree, so it is a ceiling |

The four rows obey
[`../../README.md#pbench-three-arms-one-contract`](../../README.md#pbench-three-arms-one-contract).
`Bindings/README.md` publishes `tree` and `core` side by side and says that
`jni/` is the ceiling, not a binding cost.

## Contract

The normative contract is in
[`../../README.md#pbench-three-arms-one-contract`](../../README.md#pbench-three-arms-one-contract).
This section is the Java-specific shape of those arms.

Both sides measure the same shape, so the ratio is the codec and not the setup:

- **encode** -- re-encode the tree the candidate is holding, one `byte[]`
  allocated per call, one boundary crossing per call. The Java tree is the
  fully built one a caller would hold; the C tree is decoded once from the
  same bytes, because a C tree has no separate "built" state. That asymmetry
  flatters the C side -- use it as a ceiling, not as the binding's cost.
- **decode** -- bytes into the side's own materialized tree, the result read
  so the work cannot be eliminated, then released (`pbw_message_free` / GC),
  one boundary crossing per call. The C side reads the field and presence
  counts; the Java side reads the decoded arrays, because
  `GenericCodec.decode` is lazy on message/string/bytes fields and
  materializing them here would time work the binding never does.

`Compare.java` keeps this prebuilt shape on purpose: it prices the
JNI-ceiling question, not the application task. The application task
(fresh native per iteration, deep materialization) lives in
`../benchmarks/Bench.java` as the `tree` row, with the old prebuilt task
kept beside it as the `proxy` row.

Every tier is gated before any timing: both candidates must reproduce the
reference bytes exactly and both decodes must report a live tree. Timing is
`Compare.measureAll`, the same calibrate + interleaved best-of-3 loop as
`../benchmarks/Bench.java` (each round times all four ops back to back, so
machine drift hits both sides equally).

Rows print the [`BENCHMARKS.md` "Output format"](../../../BENCHMARKS.md#output-format)
block, so `scripts/bench-record.py` reads this log like any other harness.

## Layout

One directory per candidate, like [`bindings/go/pbench/`](../../go/pbench/README.md)
(Java's classes are all in the default package, so the directories are
organization only):

| dir | idea | status |
|---|---|---|
| `pure/` | adapter over [`../runtime/`](../runtime/): the shipped pure-Java codec -- `GenericCodec` tree, lazy decode, `Unsafe` stores | shipped (no copy of the codec: a copy would drift) |
| `jni/` | the C core behind four JNI methods in `pbwire_jni.c`, one call per op | reference: the **ceiling** a JNI binding would sit under |
| `Compare.java` | the candidate contract, the payload, the parity gates, the interleaved timer | harness |

The JNI row is deliberately flattering, and that is the point: it never
translates a Java tree into a C tree, and its decode never materializes Java
objects. A real JNI data binding pays both translations, so it measures
strictly worse than these numbers.

## Run

```shell
./pbench.sh              # all four tiers (or: make pbench-java)
TIER=xlarge ./pbench.sh  # one tier, for iteration
```

Needs JDK 17+ for `jni.h` and a C compiler. `pbench.sh` builds
`build/libpbwire.a` through the top-level Makefile (incremental, so a present
one costs one `make` call), compiles
the glue against `$JAVA_HOME/include`, and leaves everything in `out/`
(git-ignored -- no binaries are committed). It is **not** part of `make test`,
`make java-test` or CI: it needs a native toolchain, and the shipped binding's
"plain javac, no native library" property has to stay true.

## Last run (Apple M4 Pro, OpenJDK 26.0.1, same process)

`matrix3.proto`, the four standard tiers, `jni-c / pure-java` (higher means the
C core is faster; below 1.0x the pure-Java codec wins):

| tier | bytes | encode | decode |
|---|---|---|---|
| small | 136 | 0.50x | 0.59x |
| medium | 174 | 0.61x | 0.68x |
| large | 2469 | 1.90x | 0.90x |
| xlarge | 74913 | 1.87x | 1.01x |

Read it the way the ledger entry does: one JNI call plus the `byte[]` pin/copy
dominates an ~75-300 ns codec, so the C core loses small and medium; it wins
encode once the payload dominates the boundary, and loses decode everywhere
because `pbw_decode` eagerly materializes the whole tree plus a `malloc`/`free`
pair while `GenericCodec.decode` is lazy on message/string/bytes fields. Absolute
ns move with machine load and JDK (a loaded host puts the small tier at 0.57x
pure encode rather than 0.50x, and the large/xlarge encode ratio anywhere in
1.4x-2.0x); only the intra-run ratios are comparable.

## Verdict

Selected: the pure-Java runtime (`../runtime/`) stays the shipped codec. `jni/`
stays the `shim` ceiling and is not a candidate for the runtime: it never
translates a tree, so a real JNI binding pays strictly more than its numbers.
The ledger entry is
[`Rejected: JNI binding for Java`](../../../docs/perf-attempts.md#rejected-jni-binding-for-java-2026-11-16).

Go-parity re-time (`results/bench-java-20261121T183049Z.json`, xlarge
74913 B, throughput ratios = pbwire MB/s over `protobuf-java` MB/s):

| arm | encode ns/op | vs baseline | decode ns/op | vs baseline |
|---|---|---|---|---|
| `tree` (full task) | 303081.7 | 0.28x | 209700.8 | 0.87x |
| `core` (generated model) | 132921.2 | 0.63x | 227270.9 | 0.81x |
| `proxy` (prebuilt) | 75433.1 | 1.11x | 94570.1 | 1.94x |

Proxy/tree gap at xlarge: 4.0x encode, 2.2x decode -- the old headline
was the held handle, not the application. The `tree` row (fresh `DMsg`
per iteration, every gate-checked field read back) is the application
path and the headline; `proxy` is published beside it, never the
headline. Both native arms sit below the baseline on every tier
(`tree` encode 0.26x-0.34x, decode 0.72x-0.93x; `core` encode
0.30x-0.63x, decode 0.56x-0.81x), so both cells are flagged for the
Task 15 optimization pass -- no optimization is attempted here.

Selected: the pure-Java runtime (`../runtime/`) stays the shipped codec.
`jni/` stays the `shim` ceiling and is not a candidate for the runtime: it never
translates a tree, so a real JNI binding pays strictly more than its numbers.
The ledger entry is
[`Rejected: JNI binding for Java`](../../../docs/perf-attempts.md#rejected-jni-binding-for-java-2026-11-16).
The `core` arm (the committed generated model) is the native-shape
reference, not a second runtime. The prior per-arm numbers and reasons are in
[`Provisional: the JVM four, three arms`](../../../docs/perf-attempts.md#provisional-the-jvm-four-three-arms-2026-11-21-issue-41).

## Reuse in other languages

Copy the layout: one directory per candidate, the same native-in / native-out
contract, the same four tiers, one interleaved harness, the standard output
block, and a README table that says why each candidate lost.

Two axes exist, and only one of them is available everywhere:

- **Where the codec lives** -- the host language's own codec against the C core
  through that host's FFI. Only bindings that wrote their own codec have a
  second candidate: Java (here) and Go
  ([`bindings/go/pbench/`](../../go/pbench/README.md), `dyngo/` vs `dync/`).
  Everywhere else the C core *is* the only implementation, so this axis costs a
  whole second codec first.
- **How coarse the FFI crossing is** -- one crossing per field against one
  crossing per operation (a host value table, or a C shim that reads host
  memory). Go's `dync/` (0.05-0.10x encode, 0.11-0.13x decode) and `cshim/`
  (1.35-1.80x encode, 1.21-1.96x decode) priced this axis; it is copyable into
  every binding with a per-field FFI path, and it is the lesson worth carrying
  across languages.

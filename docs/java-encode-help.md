# Help wanted: Java 136 B encode

The pure-Java codec (`bindings/java/runtime`, shared by the Kotlin, Scala
and Clojure bindings) trails `protobuf-java` on every encode tier
(0.26x-0.44x), worst at the smallest: **136 B encode sits at ~0.26x** (see the
[Java section](../BENCHMARKS.md#java-vs-protobuf-java) and the
[at-a-glance table](../BENCHMARKS.md#at-a-glance-every-binding-one-tier)
in BENCHMARKS.md; the A/B probe measured ~224 ns vs ~59 ns per encode).
Tuning so far has hit what looks like a structural floor. I am a solo
developer. Ten years ago I worked at a mobile game company, and to speed up
the gateway I wrote `enif_protobuf`: a fast C core for Erlang (which has no
official protobuf runtime) that raised throughput several times over pure
Erlang code. With AI help I later ported that same dynamic design to 23 host
bindings, all measured against their official counterparts in BENCHMARKS.md.
If you know HotSpot and the Java memory model well, I would welcome your help.

> Note (2026-11-23 results slim): the published Java headline is now the
generated model (`core`, 0.72x/0.79x at xlarge) — the same-task comparison.
This page still tracks the per-field `tree` encoder below, now a `pbench`-only
arm (see `bindings/java/pbench/README.md`); the numbers are unchanged, only
the headline moved.

## Why I insist on dynamic (not generated)

I do not want a pile of generated static code: it is large, hard to read,
slow to compile, hard to annotate, hard to extend with custom field
behavior, and it cannot load or update a schema at runtime without a regen
and restart. My codegen layer therefore emits only thin models (field
names, numbers, types, presence) on top of the dynamic API, and the same
wire core serves every host language. Erlang is the clearest case for why:
there is no official Erlang runtime, so a shared dynamic core is the product
itself, not a shortcut.

## The constraint (non-negotiable)

The binding stays dynamic: the schema is built at runtime from a standard
`FileDescriptorSet`, values live in the columnar `DMsg` tree, and there is
no per-message generated `writeTo`, no `protoc` codegen step, and no JNI.
A fix that turns this binding into generated straight-line writers, or that
moves encoding into native code, will not be accepted -- that is my product
decision, not an oversight. The question is what a dynamic encoder can
still do within that shape. A well-evidenced "0.26x is the floor, here is
the proof" is also a welcome answer: it closes the issue instead of
leaving it open.

## Why it looks structural

Ablation on `GenericCodec.encodeInto()` (measurement only, bytes differ):
removing all byte writes still leaves **23.3 ns of per-field skeleton** --
22 iterations of `has[]` load, `Field` load, label/kind/presence tests and
dispatch, ~1.06 ns per field. Generated `protobuf-java` code has no such
loop: its `writeTo` is straight-line code with compile-time tag offsets.
The remaining gap (~26 ns) is roughly that skeleton plus tag loads, and
none of the tuning below moved it.

The second wall is **HotSpot's inlining ceiling**. A helper inlined into
`encodeInto()` must stay under ~325 bytecode bytes (`FreqInlineSize`); the
unrolled 64-bit varint writer reached 437 bytes and the call site reported
"hot method too big", which evaporated the win. Anything unrolled has to be
checked with `-XX:+PrintInlining`, not reasoned about. The
[xlarge row](../BENCHMARKS.md#java-vs-protobuf-java) shows the other side of
the same codec: decode 0.81x while encode trails at 0.29x even when the payload
dominates, so the per-field dispatch taxes every tier and this call is sharpest
at the 136 B cell. (Before the Go-parity re-time the `tree` arm held a prebuilt
handle and the same page read ~0.7x / 1.13x encode; the arm now times the full
native-in/native-out task, so the crossing cost is visible.)

## What has been tried (all neutral or worse)

Full numbers are in the
[Java 136 B encode cell](perf-attempts.md#cell-java-pure-java-codec-136-b-encode-073x)
of the attempts ledger; headlines only:

1. Unrolled branch-tree varint writers (`putVar32U`/`putVar64U`, the C
   core's shape) -- killed by the inlining ceiling above.
2. Plain `a[i++] = ...` stores instead of `Unsafe.putByte` -- at least as
   slow; the `Unsafe` store is already as cheap as a bounds-checked store.
3. Packed varint via one `Unsafe.putLong` -- won the microbenchmark, lost
   badly in the real encoder (register pressure in the already-large
   inlined `encodeInto`).
4. Primitive mirror arrays on `MessageDef` instead of `Field` loads -- four
   bases cost more loads than the two the JIT already keeps in L1.
5. Inlining the string/bytes/message path into `encodeInto()` -- removes
   call boundaries but grows the hottest method, which is the binding
   constraint.
6. Dropping the `repeated` / implicit-zero branches -- neutral; the
   branches are not the cost, so a precomputed action table was dropped
   before being built.
7. Splitting `encodeInto` (repeated-field branch and unknown-fields tail
   to helpers, the opposite of attempt 5) -- neutral on every shape.
8. Object dispatch for the numeric singular path (a `switch` replaced by a
   table of lambdas) -- slower on every encode shape, 136 B down to
   0.81-0.85x; a megamorphic call costs more than the branch-predicted
   switch.

A JNI port over the C core was also probed and rejected (small-tier
encode 0.54x of pure Java; see the
[JNI section](perf-attempts.md#rejected-jni-binding-for-java-2026-11-16)).

A 1-slot string memo replacing the 8-slot `StrCache` also regressed the
cell (0.82x -> 0.33x on stable-value traffic) and was reverted. So: please
do not retry micro-tuning of the writer loop without new evidence.

## How to contribute

1. The baseline is the standard `protobuf-java` jar: `bench.sh` downloads
   4.36.2 from Maven Central on first run, which matches the pinned
   `protoc` 36.2, so the default is fine. If you want a newer-than-Maven
   baseline, compile it from the pinned checkout (`make conformance-setup`
   clones `v36.2` into `.tmp/protobuf-v36.2`) and pass it as
   `PROTOBUF_JAVA_JAR=/path/to/protobuf-java.jar` -- every bench script
   honors that override.
2. Reproduce with the A/B probe (same session, alternating rounds, so
   machine drift hits both sides):
   `bindings/java/benchmarks/ab.sh <base-classes> <cand-classes>` -- best
   of passes ns/op for dyn-encode vs pbuf-encode, plus
   `-XX:+UnlockDiagnosticVMOptions -XX:+PrintInlining` to confirm the
   candidate still inlines.
3. Confirm the official cell with `make java-bench` (byte equality gates
   first; ablation-style runs that change bytes are measurement only).
3. Open an issue with the in-round A/B numbers and the `PrintInlining`
   note. Partial results are fine -- a clean negative with numbers saves
   the next person a week.

The codec source is `bindings/java/runtime/GenericCodec.java` (the floor
is noted at the top of that file); the benchmark is
`bindings/java/benchmarks` (harness details and the full tier tables are in
[Java vs protobuf-java](../BENCHMARKS.md#java-vs-protobuf-java)); the ledger
section linked above has the full ablation table and every rejected
candidate.

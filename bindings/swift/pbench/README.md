# Swift pbench: four arms, one contract

The arms, the gate and the timing loop from
[`bindings/README.md`](../../README.md#pbench-three-arms-one-contract), applied
to `structtest.Scalars3` from `bindings/c/tests/data/matrix3.proto` and the four
standard tiers. Every arm does the same user-visible task: a native object in
and wire bytes out; wire bytes in and a fully readable native object out.

| arm | file | mechanism | what it answers |
|---|---|---|---|
| `tree` | `Sources/Bench/main.swift` (timer) over `Sources/Pbench/Arms.swift` (handle) | the shipped `Message` handle full task: a fresh native object per iteration (one `set` per field, `encode`, deinit frees inside the timer); a fresh `decode` per iteration with every gate-checked field folded into a live sink, deinit frees inside the timer | what an application pays per message end to end |
| `proxy` | `Sources/Bench/main.swift` (timer) | the old prebuilt shape, relabelled not re-measured as a new task: a held handle's `encode`, bare decode with no materialization (same-run timing only, never the headline) | what the transcoder/hold-and-re-encode path costs |
| `shim` | `Sources/Pbench/Arms.swift` | the generated `@frozen <Message>Repr` is the native object; `InPlace.encodeInPlace` / `decodeInPlace` cross once and `bindings/swift/Sources/CPbwire/shim.c` walks that memory in place through `pbw_struct_codec_*` | is a boundary worth paying for at all |
| `core` | `Sources/Pbench/CoreArm.swift` | the pure-Swift `PbwireCore` product: a schema from the descriptor set, a value tree built with one `set` per field, then `encode`/`decode` | who walks the wire with no C library, no `unsafeFlags` link and no generated code |
`core` replaces the `n/a` plan 21 started from: the arm is `PbwireCore`
(`Sources/PbwireCore/`), a second, independent Swift implementation of the
binary format that builds without the C module.

## Layout table

`codegen/internal/gen/swift.go` emits, beside each message, a
`@frozen public struct <Message>Repr` and its `fields: [Pbwire.FieldPlan]` row
list. `@frozen` plus no references is what makes the mirror a known-layout
value type: every slot is a fixed-width scalar, an `UnsafePointer`, a
`Pbwire.PbwireBytes` (the core's inline pointer + length) or a pointer to a
child mirror, so the C walk pays no ARC (the idiomatic `String`/`Array`/`Data`
struct is the `tree` object, never the shim's).

Offsets are not assumed: each row's `offset`/`has`/`count`/`cs` is a
`MemoryLayout<Repr>.offset(of: \.slot)!`, so the Swift layout the compiler
chose and the table the C codec walks cannot disagree (Rust asserts
`offset_of!` instead; Julia uses `fieldoffset`). Slots are emitted values first
(with a repeated field's count behind its pointer), then has flags, then oneof
case words, so no presence slot can land at offset 0 — the codec reads 0 as
"no such slot".

Copy rules follow the C binding's struct ABI, with the deviations this binding
needs: a `string` is a NUL-terminated `UnsafePointer<CChar>` (UTF-8, no
embedded NUL — `bytes` is the field for those), `bytes` is the core's inline
`pbw_bytes_t`, a sub-message is a pointer to the child mirror (whose array
stride is emitted explicitly: a Swift stride can be wider than the child's
size), and a repeated field is a contiguous pointer + `Int` count. Encode
borrows the image; decode zeroes it and bump-allocates every non-blittable
value from the type's arena, so the decoded mirror is a view of memory the
`InPlace` type owns — readable until the type's next decode, released with it.
`Pbwire.ReprStorage` owns the buffers a *hand-built* mirror borrows.

`Sources/CPbwire/shim.c` builds one `pbw_struct_field_t` row per layout row,
resolves sub-message layouts through the schema's field descriptor (children
register first: `ReprMessage.children`), fills proto2 enum defaults from the
schema and compiles the codec once per type (`pbw_struct_codec_init`). It also
compiles `bindings/c/runtime/pbw_struct.c` (symlinked into the target) so the
package needs no second archive: the same C struct codec the C, Dart, Julia,
Rust and C# shim rows time. Encode and decode are each one downcall; the type
owns the reusable encode buffer and the decode arena.

## Gate

```
make swift-test                                   # includes the gate
(cd bindings/swift && swift test --filter InPlaceTest)   # PASS / exit 1
```

The gate asserts, on the pinned `matrix3` fixture, that the mirror's bytes
equal the protoc reference (`bindings/swift/tests/data/matrix3.bin`, the same
oracle `GeneratedTest` uses and the same bytes apple/swift-protobuf's generated
code writes for those values — `benchmarks/baseline/BenchBoth` asserts that
equality for the `tree` row), that a decode exposes every fixture value (`str`,
`inner.name`/`value`, the repeated counts and elements, the proto3 optionals
with their has flag, `u64`, `color`, the repeated bools), and that the decoded
mirror re-encodes to the same bytes. It also gates the layout rows themselves:
one row per field in descriptor order, every slot inside the mirror and no two
rows sharing an offset.

One shared rule is worth stating because the gate does not pretend otherwise:
a proto3 implicit scalar has no presence flag in the layout, and the C struct
codec writes such a slot even when it holds zero (the same rule the Rust,
Julia and Dart mirrors follow). The published payload sets every scalar, so the
arm's bytes equal the reference exactly; a *zero* mirror re-encodes to explicit
zero fields, which is valid proto3 that decodes to the same mirror.

`make conformance-swift` drives the official binary suite through
`bindings/swift/conformance/main.swift` (the same 4-byte-framed
`ConformanceRequest`/`ConformanceResponse` protocol every other binding's testee
speaks) over the pure core, so plan 09's deferred target exists and the
no-native path is checked by the suite itself, not only by `swift test`. Result:
1412 successes, 1403 skipped (JSON/text), 2 expected failures, 0 unexpected —
the same failure set as the C core and every other port.

The `core` arm has its own gate inside `Bench`: before timing anything it
encodes the tier through `PbwireCore` and requires the bytes to equal the
`tree` handle's reference image (which `InPlaceTest`/`GeneratedTest` pin to
protoc and swift-protobuf), then reads every payload field back off the decoded
core tree. A core row that prints is a row whose bytes and values a second,
independent wire implementation agrees with.

The `tree` full-task gate lives in `Bench` itself: per tier, before timing
anything, it builds a second fresh `Message` through the same
`schema.message` + `fillTree` path the timer uses and requires its bytes to
equal the reference image, decodes the reference fresh and compares every
gate-checked field (`TreeArm.readBack`, the same list the timer's
`consumeTree` folds into the sink), and re-encodes the decoded tree
byte-exactly. A tree row that prints is a row whose timer body is proven
byte- and field-correct.

## Provisional table (Stage 3, historical)

> Historical pre-parity definition: `tree` below is a prebuilt handle's `encode()`/`decode()`; the formal Go-parity definition is the Verdict table.

`cd bindings/swift && swift run -c release Bench compare`: interleaved rounds
(all six ops back to back), median of five. `tree` is a *prebuilt* handle's
`encode()`/`decode()` (the formal row's definition); `shim` is one
`encodeInPlace` / `decodeInPlace` call on the flat mirror, with the mirror and
arena allocated once; `core` is a prebuilt `PbwireCore` `Message` tree through
one `encode()` / one `decode()`. MB/s over wire bytes, 2026-11-21, before the
stage 6 rounds:

| tier | `tree` enc | `shim` enc | `core` enc | `tree` dec | `shim` dec | `core` dec |
|---|---|---|---|---|---|---|
| 136 B | 1023.0 | 914.0 | 163.3 | 836.1 | 717.9 | 75.0 |
| 174 B | 742.9 | 700.1 | 94.3 | 497.4 | 555.5 | 47.8 |
| 2469 B | 694.5 | 502.5 | 82.8 | 304.6 | 293.3 | 22.2 |
| 74913 B | 1518.8 | 854.1 | 185.3 | 592.1 | 560.0 | 51.2 |

## Verdict (Go-parity re-time)

**Ship the `tree` handle as the default runtime; publish the flat-mirror `shim`
row as the native-object mechanism; publish `core` and `proxy` beside them,
never headline `proxy`.** Formal run `results/bench-swift-20261121T193722Z.json`
(MB/s over wire bytes, ratio = pbwire / swift-protobuf, higher is faster):

| tier | `tree` enc | `tree` dec | `shim` enc | `shim` dec | `core` enc | `core` dec |
|---|---|---|---|---|---|---|
| 136 B | 123.6 (0.28x) | 311.4 (1.25x) | 974.9 (2.18x) | 769.9 (3.09x) | 241.1 (0.54x) | 98.6 (0.40x) |
| 174 B | 115.9 (0.35x) | 274.5 (1.37x) | 966.5 (2.91x) | 707.7 (3.53x) | 223.8 (0.67x) | 91.2 (0.45x) |
| 2469 B | 69.1 (0.26x) | 614.5 (3.25x) | 994.1 (3.80x) | 534.4 (2.83x) | 247.3 (0.95x) | 63.9 (0.34x) |
| 74913 B | 66.3 (0.24x) | 578.9 (2.68x) | 915.1 (3.31x) | 580.9 (2.69x) | 247.7 (0.90x) | 65.9 (0.30x) |

Proxy/tree encode gap: 8.5x/9.7x/21.1x/25.3x (small/medium/large/xlarge) —
the gap the old prebuilt `tree` row was hiding. Proxy decode stays near tree
decode (1.06x at xlarge): decode was always dominated by the C core walk, not
by materialization.

The `tree` row stays the default because it is the shipped path an
application that holds a `Message` uses, and its decode beats the baseline at
every tier (1.25x–3.25x). Its encode is below the baseline at every tier
(0.24x–0.35x): one downcall per `set`, plus a fresh 3000-child build at
xlarge. That encode gap is the per-field cost issue 41 named, and it is
exactly what the old prebuilt row hid. The `shim` row is the answer for an
application with its own object shape: one downcall per op, above the
baseline on both ops at every tier (2.18x–3.80x encode, 2.69x–3.53x decode),
no ARC per field. `core` stays the no-native, no-`unsafeFlags` path (see
below), below the baseline on decode at every tier. Below-baseline native
cells are flagged for the optimization pass: tree encode all tiers, core
encode 136/174 B, core decode all tiers; no optimization under this task.

## Verdict: the pure core (Stage 4)

**The FFI product stays the default; `PbwireCore` is the no-native,
no-`unsafeFlags` path.** The reason to ship a second Swift wire implementation
is not speed, it is reach: this package's `Pbwire`, `Bench` and test products
carry `LinkerSetting.unsafeFlags(["../../build/libpbwire.a"])`, which SwiftPM
refuses in a non-root package. A consumer who adds `pbwire` as a normal
dependency, or a build with no C toolchain, has nowhere to go. `PbwireCore`
answers that: pure Swift, no C module, no archive, no unsafe flags, and it
passes the official binary suite on its own (`make conformance-swift`).

It is not the fast path and does not pretend to be: the Go-parity formal run
above puts it at 0.54–0.95x of swift-protobuf on encode and 0.30–0.45x on
decode, against 0.24–0.35x / 1.25–3.25x for the full-task `tree` handle and
2.18–3.80x / 2.69–3.53x for the `shim` mirror. The gap is the value tree's ARC
traffic, not the wire walk: one class `Message` plus one slot array per
submessage, one `[UInt8]` per string, and a `Value`/`Field` copy per field, at
3000 sub-messages and 3000 strings in the 74 KB payload. Why the gap exists and
what was already fixed is in `docs/perf-attempts.md`.

## Formal rows (Go-parity re-time)

`make swift-bench` records `results/bench-swift-<stamp>.{log,json}` and prints
all four rows per tier plus the apple/swift-protobuf baseline
(`benchmarks/baseline`): `pbwire-swift-tree` (the shipped-handle full task),
`pbwire-swift-proxy` (the relabelled prebuilt hold-and-recode, never the
headline), `pbwire-swift-shim` (the flat mirror) and `pbwire-swift-core` (the
pure core's prebuilt tree), same harness, same payload, same bytes. The run
below is `results/bench-swift-20261121T193722Z.json` (Apple M4 Pro, arm64).
MB/s over wire bytes:

| tier | `tree` enc | `proxy` enc | `shim` enc | `core` enc | swift-protobuf enc | `tree` dec | `proxy` dec | `shim` dec | `core` dec | swift-protobuf dec |
|---|---|---|---|---|---|---|---|---|---|---|
| 136 B | 123.6 | 1047.5 | 974.9 | 241.1 | 448.0 | 311.4 | 952.3 | 769.9 | 98.6 | 249.1 |
| 174 B | 115.9 | 1128.3 | 966.5 | 223.8 | 331.8 | 274.5 | 848.4 | 707.7 | 91.2 | 200.6 |
| 2469 B | 69.1 | 1457.9 | 994.1 | 247.3 | 261.3 | 614.5 | 680.1 | 534.4 | 63.9 | 188.8 |
| 74913 B | 66.3 | 1675.1 | 915.1 | 247.7 | 276.2 | 578.9 | 610.1 | 580.9 | 65.9 | 216.3 |

Throughput ratios (pbwire / swift-protobuf, higher is faster): `tree` encode
0.28x / 0.35x / 0.26x / 0.24x and decode 1.25x / 1.37x / 3.25x / 2.68x; `shim`
encode 2.18x / 2.91x / 3.80x / 3.31x and decode 3.09x / 3.53x / 2.83x / 2.69x;
`core` encode 0.54x / 0.67x / 0.95x / 0.90x and decode 0.40x / 0.45x / 0.34x /
0.30x. The pre-Go-parity Stage 5 table (prebuilt `tree` at 2.20x–6.01x encode)
is superseded by the Verdict table above; the proxy column above carries the
old prebuilt shape forward for comparison.

Stage 6 (optimization): one round, kept. The first profile of the core arm was
dominated by `swift_beginAccess` (a class `Reader`'s stored-property mutations
cost a runtime exclusivity check **per byte read**), `outlined init with copy of
Tag` (an `[UInt8]` per tag = ARC traffic on every field write) and `MsgDef`
struct copies (`schema.messages[i]` copied two dictionaries and three arrays per
message). Making `Reader` a value type passed `inout`, `Tag` five inline bytes,
and `MsgDef` a class moved xlarge encode 185.3 -> 239.6 MB/s and decode 51.2 ->
60.2 MB/s (136 B: 163.3 -> 232.8 and 75.0 -> 93.0); the provisional table above
is the pre-round run. The remaining gap is the tree's ARC, whose fix is a
columnar/code-generated tree — a rewrite, recorded in `docs/perf-attempts.md`,
not chased.

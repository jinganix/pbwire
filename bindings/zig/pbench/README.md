# Zig pbench: four arms, one contract

The arms, the gate and the timing loop from
[`bindings/README.md`](../../README.md#pbench-three-arms-one-contract), applied
to `structtest.Scalars3` from `bindings/c/tests/data/matrix3.proto` and the four
standard tiers. Every arm does the same user-visible task: a native object in
and wire bytes out; wire bytes in and a fully readable native object out.

| arm | file | mechanism | what it answers |
|---|---|---|---|
| `tree` | `benchmarks/bench.zig` (`bench_vs_baseline.zig` for the baseline run) | the shipped dynamic handle, fresh per iteration: one `set` per field, `encodeInto` into a reused caller buffer, free; fresh `decode` then one `get` per field folded into a live sink, free | what the per-field native round trip costs the application |
| `proxy` | `benchmarks/bench.zig` (`bench_vs_baseline.zig`) | the old prebuilt shape: a held handle's `encodeInto`, decode-and-drop with no materialization; same-run timing only, never the headline | what the transcoder task costs without the native round trip |
| `shim` | `benchmarks/bench.zig` | the generated `extern struct <Message>Mirror` is the native object; `pbwire.InPlace.encodeInPlace` / `decodeInPlace` cross once and `bindings/c/runtime/pbw_struct.c` walks that memory in place | is a boundary worth paying for at all |
| `core` | `pbench/core_arm.zig` + `core/` | the pure-Zig wire core: its own descriptor reader, tree and binary codec, no C source and no translate-c | can a build that does not compile `src/*.c` still be the same binding |

## Layout table

`codegen/internal/gen/zig.go` emits, beside each message, a
`pub const <Message>Mirror = extern struct` whose slots follow the C struct
codec's ABI, plus its comptime `fields: [_]FieldPlan` row table. A string is a
NUL-terminated `?[*:0]const u8`, bytes are the core's inline `pbwire.Bytes`
(pointer + length), a sub-message is a pointer to its child mirror, and a
repeated field is a pointer to a contiguous array plus a `usize` count. A bool
is one `u8`, never a Zig `bool`, so a decoder byte can never be invalid.
Presence keeps the 4-byte has flag, and a oneof keeps its `u32` case word.

The emitter computes every slot offset with C's struct rules and emits a
`comptime` block asserting it with `@offsetOf` (plus the padded mirror size),
so an emitter/ABI mismatch fails the build instead of producing bytes. Child
plans are resolved recursively from the generated `ChildPlan` rows, with the
child's size as the repeated array stride.

`pbwire.InPlace` builds one `pbw_struct_field_t` row per plan, fills proto2
enum defaults from the schema and compiles the codec once per type. Encode and
decode are each one C call; the plan owns the reusable encode buffer and a
C-owned decode arena, so a decoded image is readable until the next decode on
that plan.

## Gate

```
make zig-test                                        # self-check + pure-core tests
cd bindings/zig && zig build generated-test          # mirror byte + field gate
make conformance-zig-core                            # pure-core binary suite
make zig-bench                                       # mirrors and core gated per tier before timing
```

The generated-model gate asserts, on the pinned `matrix3` fixture, that the
mirror's bytes equal the protoc reference (`bindings/c/tests/data/matrix3.bin`,
the same bytes Arwalk/zig-protobuf's generated code writes for those values),
that a decode exposes every fixture value (`str`, `inner.name`/`value`, the
repeated counts and last elements, the proto3 optionals with their has flag,
`u64`, `color`, the repeated bools), and that the decoded mirror re-encodes to
the same bytes. The bench repeats the byte gate for every tier before timing, plus a deep
field gate: decoding the reference must expose every fixture value (all
scalars, the nested message, both proto3 optionals, the counts and last
elements of all four repeated fields) and a second fresh-build message must
encode to the same bytes, so the timed per-iteration build is gated too.

## Provisional table (Stage 3, historical)

Superseded: the `tree` column below is the pre-parity prebuilt-handle
definition (a held `Message` re-encoded, decode-and-drop). The formal
Go-parity definition is the Verdict table: fresh native `Message` in and out
through the shipped per-field API, alloc and free inside the timer, every
gate-checked field materialized.

`cd bindings/zig && zig build -Doptimize=ReleaseFast bench`: all three arms back
to back in one process, best of three passes. MB/s over wire bytes, 2026-11-21:

| tier | `tree` enc | `shim` enc | `core` enc | `tree` dec | `shim` dec | `core` dec |
|---|---|---|---|---|---|---|
| 136 B | 2017.8 | 1849.5 | 574.4 | 1081.4 | 1211.9 | 780.6 |
| 174 B | 1804.9 | 1515.0 | 565.9 | 907.9 | 1065.8 | 649.4 |
| 2469 B | 1687.4 | 1118.5 | 685.7 | 665.8 | 658.9 | 326.8 |
| 74913 B | 1641.3 | 1200.7 | 729.9 | 743.7 | 721.2 | 334.7 |

The core arm is the pure-Zig codec: the descriptor reader, the value tree and
the binary encoder/decoder, with no C source and no translate-c. It is the
`-Dcore=true` build in `build.zig`, and `make conformance-zig-core` gates it on
the official binary suite (1412 successes, 1403 skipped, 2 expected, 0
unexpected).

## Verdict

**Ship the dynamic-handle `tree` as the default runtime; publish the
flat-mirror `shim` row as the native-object fast path.** The `tree` arm is
the shipped API, and its decode side beats the Arwalk/zig-protobuf baseline
at large and xlarge (1.08x / 1.34x); its encode side trails everywhere
(0.19x-0.27x: a fresh per-field build plus the C encoder against generated
code that writes straight into the output). The `shim` arm is above the
baseline at every tier on both ops (encode 1.41x / 1.55x / 1.33x / 1.45x,
decode 1.44x / 1.46x / 1.14x / 1.46x), so an application that wants its own
`extern struct` object takes the one-downcall path and skips the per-field
crossing issue 41 is about. The pure-Zig `core` stays the no-C build, not
the fast path (encode 0.42x-0.88x, decode 0.54x-0.93x of the baseline). The
`proxy` row is published beside them, never the headline: at xlarge the held
handle re-encodes 10.0x faster than the fresh native build (45589.6 ns/op vs
456842.8 ns/op), which is the gap the old prebuilt `tree` row was hiding.
Proxy decode stays near tree decode (1.01x at xlarge): decode was always
dominated by the C core walk, not by materialization.

Formal Go-parity rows (`make zig-bench`, `results/bench-zig-20261122T014913Z.json`,
Apple M4 Pro arm64; ratios are THROUGHPUT = MB/s pbwire / MB/s baseline):

| tier | `tree` enc | `tree` dec | `shim` enc | `shim` dec | `core` enc | `core` dec | `proxy` enc | `proxy` dec |
|---|---|---|---|---|---|---|---|---|
| 136 B | 0.24x | 0.88x | 1.41x | 1.44x | 0.42x | 0.93x | 64.8 ns/op | 122.4 ns/op |
| 174 B | 0.27x | 0.85x | 1.55x | 1.46x | 0.54x | 0.91x | 88.4 ns/op | 180.6 ns/op |
| 2469 B | 0.21x | 1.08x | 1.33x | 1.14x | 0.77x | 0.54x | 1419.3 ns/op | 3604.6 ns/op |
| 74913 B | 0.19x | 1.34x | 1.45x | 1.46x | 0.88x | 0.66x | 45589.6 ns/op | 104567.9 ns/op |

Below-baseline native cells, flagged for the optimization pass: tree encode
all tiers, tree decode 136/174 B, core encode all tiers, core decode all
tiers. No optimization is attempted here.

## Formal rows (Stage 5)

`make zig-bench` records `results/bench-zig-<stamp>.{log,json}` and prints all
three rows per tier plus the Arwalk/zig-protobuf baseline. The run below is
`results/bench-zig-20261121T032028Z.json` (Apple M4 Pro, arm64), same harness,
same payload, bytes gated equal before timing. Both sides allocate through the
release allocator (`std.heap.smp_allocator`), so the zig-protobuf decode column
is its release cost, not a debug allocator's:

| tier | `tree` enc | `shim` enc | `core` enc | zig-protobuf enc | `tree` dec | `shim` dec | `core` dec | zig-protobuf dec |
|---|---|---|---|---|---|---|---|---|
| 136 B | 1986.1 | 1735.6 | 549.1 | 1279.4 | 1043.6 | 1224.9 | 771.4 | 847.2 |
| 174 B | 1861.7 | 1654.5 | 563.1 | 1040.3 | 910.6 | 1037.7 | 649.0 | 730.6 |
| 2469 B | 1658.6 | 1131.2 | 677.7 | 834.5 | 651.4 | 673.1 | 343.6 | 593.6 |
| 74913 B | 1604.0 | 1197.1 | 731.9 | 825.6 | 724.2 | 745.0 | 356.6 | 521.8 |

## Core verdict

**The static link stays the default; `-Dcore=true` is the no-C build.** The
pure-Zig core exists so a build that cannot compile `src/*.c` still encodes and
decodes the same bytes, and the official suite proves it
(`make conformance-zig-core`: 1412 successes, 1403 skipped, 2 expected, 0
unexpected). It is not the fast path: the formal run puts it at 0.28-0.46x of
`pbwire-zig-tree` on encode and 0.49-0.74x on decode, and 0.43-0.89x /
0.58-0.91x of zig-protobuf (worst at 136 B encode, best at 74 KB encode). The
encode gap is the tag/length writer appending without the C encoder's reserve
pass; the decode gap is the value tree's arena and per-field allocations, the
same shape the Rust and Swift cores report. The default artifacts keep linking
the C core, and `make conformance-zig` keeps proving that path.

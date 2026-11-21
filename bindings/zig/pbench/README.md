# Zig pbench: three arms, one contract

The arms, the gate and the timing loop from
[`bindings/README.md`](../../README.md#pbench-three-arms-one-contract), applied
to `structtest.Scalars3` from `bindings/c/tests/data/matrix3.proto` and the four
standard tiers. Every arm does the same user-visible task: a native object in
and wire bytes out; wire bytes in and a fully readable native object out.

| arm | file | mechanism | what it answers |
|---|---|---|---|
| `tree` | `benchmarks/bench.zig` | the shipped dynamic handle: one `set` per field, `encodeInto` into a reused buffer; `decode` then one `get` per field | what the per-field native call costs once the tree exists |
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
the same bytes. The bench repeats the byte gate for every tier before timing.

## Provisional table (Stage 3)

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

**Publish the flat-image `shim` row as the native-object path beside the
shipped dynamic handle.** With one C call per op the shim is above the
zig-protobuf baseline at every tier: 1.36x / 1.59x / 1.36x / 1.45x on encode
and 1.45x / 1.42x / 1.13x / 1.43x on decode. The tree row (a prebuilt
`pbwire.Message` handle) is still faster on encode because it is the core's own
encoder; the shim is the answer for an application that wants its own
`extern struct` object, and it removes the per-field crossing issue 41 is
about. There is no below-baseline shim cell, so Stage 6 is a recorded no-op.

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

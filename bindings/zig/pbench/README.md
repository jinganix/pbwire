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
| `core` | `n/a` | a pure-Zig port of the C core | plan 22 (`issue41-22-core-zig.md`) builds it |

`core` prints `n/a` until plan 22 lands; there is no second wire implementation
in this binding.

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
make zig-test                                        # self-check
cd bindings/zig && zig build generated-test          # mirror byte + field gate
make zig-bench                                       # mirrors gated per tier before timing
```

The generated-model gate asserts, on the pinned `matrix3` fixture, that the
mirror's bytes equal the protoc reference (`bindings/c/tests/data/matrix3.bin`,
the same bytes Arwalk/zig-protobuf's generated code writes for those values),
that a decode exposes every fixture value (`str`, `inner.name`/`value`, the
repeated counts and last elements, the proto3 optionals with their has flag,
`u64`, `color`, the repeated bools), and that the decoded mirror re-encodes to
the same bytes. The bench repeats the byte gate for every tier before timing.

## Provisional table (Stage 3)

`make zig-bench`: same harness, same payload, all four arms plus the
Arwalk/zig-protobuf baseline. MB/s over wire bytes, 2026-11-20:

| tier | `tree` enc | `shim` enc | zig-protobuf enc | `tree` dec | `shim` dec | zig-protobuf dec |
|---|---|---|---|---|---|---|
| 136 B | 2023.4 | 1954.2 | 1285.9 | 1137.8 | 1254.6 | 25.7 |
| 174 B | 1971.8 | 1706.2 | 1027.3 | 946.2 | 1057.7 | 22.7 |
| 2469 B | 1724.0 | 1148.5 | 879.7 | 693.3 | 680.7 | 71.2 |
| 74913 B | 1736.2 | 1202.7 | 837.3 | 741.8 | 751.0 | 332.3 |

## Verdict

**Publish the flat-image `shim` row as the native-object path beside the
shipped dynamic handle.** With one C call per op the shim is above the
zig-protobuf baseline at every tier: 1.52x / 1.66x / 1.31x / 1.44x on encode
and 48.8x / 46.5x / 9.55x / 2.26x on decode. The tree row (a prebuilt
`pbwire.Message` handle) is still faster on encode because it is the core's own
encoder; the shim is the answer for an application that wants its own
`extern struct` object, and it removes the per-field crossing issue 41 is
about. There is no below-baseline shim cell, so Stage 6 is a recorded no-op.

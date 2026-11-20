# Zig pbench: three arms, one contract

The arms, the gate and the timing loop from
[`bindings/README.md`](../../README.md#pbench-three-arms-one-contract), applied
to `structtest.Scalars3` from `bindings/c/tests/data/matrix3.proto` and the four
standard tiers. Every arm does the same user-visible task: a native object in
and wire bytes out; wire bytes in and a fully readable native object out.

| arm | file | mechanism | what it answers |
|---|---|---|---|
| `tree` | `benchmarks/bench.zig` | the shipped dynamic handle: one `set` per field, `encodeInto`; `decode` then one `get` per field | what the per-field native call costs once the tree exists |
| `shim` | `benchmarks/bench.zig` | the generated `extern struct <Message>Mirror` is the native object; `pbwire.InPlace.encodeInPlace` / `decodeInPlace` cross once and `bindings/c/runtime/pbw_struct.c` walks that memory in place | is a boundary worth paying for at all |
| `core` | `n/a` | a pure-Zig port of the C core | plan 22 (`issue41-22-core-zig.md`) builds it |

`core` prints `n/a` until plan 22 lands; there is no second wire implementation
in this binding.

## Layout table

Provisional: the emitter (`codegen/internal/gen/zig.go`) emits, beside each
message, a `pub const <Message>Mirror = extern struct` whose slots follow the C
struct codec's ABI, and its comptime `fields: [_]FieldPlan` row table. A string
is a NUL-terminated `?[*:0]const u8`, bytes are the core's inline
`pbwire.Bytes` (pointer + length), a sub-message is a pointer to its child
mirror, and a repeated field is a pointer plus a `usize` count. Presence keeps
the 4-byte has flag, a oneof its `u32` case word. Every slot offset is asserted
with `@offsetOf` in a `comptime` block, so an emitter/ABI mismatch fails the
build instead of producing bytes.

## Gate

The gate is the generated-model test and the bench's own pre-timing checks:
`make zig-test` and `zig build generated-test` assert the mirror's bytes equal
the pinned `matrix3.bin` oracle (the bytes Arwalk/zig-protobuf's generated code
writes for the same values) and that a decode exposes every fixture field. The
bench re-checks byte equality per tier before timing.

## Provisional table (Stage 3)

Provisional: `make zig-bench` records the formal run below; numbers land in
Stage 5.

## Verdict

Pending Stage 4.

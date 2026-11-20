# Rust pbench: three arms, one contract

The arms, the gate and the timing loop from
[`bindings/README.md`](../../README.md#pbench-three-arms-one-contract), applied
to `structtest.Scalars3` from `bindings/c/tests/data/matrix3.proto` and the four
standard tiers. Every arm does the same user-visible task: a native object in
and wire bytes out; wire bytes in and a fully readable native object out.

| arm | file | mechanism | what it answers |
|---|---|---|---|
| `tree` | `benchmarks/src/main.rs` | the shipped handle: `schema.message(name)`, one `set_*` per field, `encode_to()` into a reused buffer; `decode()` then one `get_*` per field | what the per-field native call costs once the tree exists |
| `shim` | `benchmarks/src/main.rs` | the generated `<Message>Flat` (`#[repr(C)]`) is the native object; `FlatType::encode_in_place` / `decode_in_place` cross once into `bindings/c/runtime/pbw_struct.c`, which walks that memory in place through `pbw_struct_codec_*` | is a boundary worth paying for at all |
| `core` | `n/a` | a pure-Rust port of the C core | plan 20 (`issue41-20-core-rust.md`) builds it |

`core` prints `n/a` until plan 20 lands; there is no second wire implementation
in this binding.

## Layout table

`codegen/internal/gen/rust.go` emits, beside each message, a `#[repr(C)] pub
struct <Message>Flat` and its `<Message>Flat::FIELDS` row list: `name`,
`offset`, `kind`, flags (`REPEATED`/`PACKED`/`REQUIRED`/`ONEOF`), the
`has`/`count`/oneof-case offsets and the child plan. Slots follow the C codec's
struct ABI: a string is a `*mut c_char` to NUL-terminated UTF-8, bytes are the
core's inline `PbwBytes` (pointer + length), a sub-message is a pointer to the
child image, a repeated field is a pointer to a contiguous array plus a `usize`
count, and a bool is one byte (`u8`), never a Rust `bool`. Offsets are computed
by the emitter with Rust's `repr(C)` rules and every one of them is asserted
with `core::mem::offset_of!` next to the table, so a wrong offset fails the
build instead of producing bytes.

`Schema::in_place::<T>()` converts the table into `pbw_struct_field_t` rows
(child rows resolved recursively, proto2 enum defaults read from the schema) and
compiles the C codec once. Encode is one call whose bytes are a view of the
plan's reusable buffer; decode is one call whose strings/arrays/sub-messages are
bump-allocated from the `FlatImage`'s arena, so the image is a view of an arena
the caller keeps alive (a repeat decode invalidates it).

## Gate

```
make rust-test                                        # includes the gate
cd bindings/rust && cargo test --test flat             # PASS / exit 1
```

The gate asserts, on the pinned `matrix3` fixture, that the flat image's bytes
equal both the official protoc reference (`bindings/c/tests/data/matrix3.bin`)
and the pinned prost oracle, that a decode exposes every fixture value (`str`,
`inner.name`/`value`, the repeated counts and last elements, the proto3
optionals and `u64`), and that the decoded image re-encodes to the same bytes.
`rust-protobuf` parity is covered by the same fixture in the bench parity check.

## Provisional table (Stage 3)

`make rust-bench` (best of three passes per op, one reused buffer per side).
`tree` is a prebuilt handle's `encode_to` / `decode`; `shim` is one
`encode_in_place` / `decode_in_place` on the flat image, with the image and
arena allocated once. MB/s over wire bytes, 2026-11-20, recorded mid-session:

| tier | `tree` enc | `shim` enc | prost enc | `tree` dec | `shim` dec | prost dec |
|---|---|---|---|---|---|---|
| 136 B | 1783.1 | 1613.5 | 1540.0 | 990.7 | 1122.8 | 783.0 |
| 174 B | 1673.7 | 1521.5 | 1533.3 | 847.6 | 995.6 | 611.1 |
| 2469 B | 1724.0 | 1088.7 | 1353.4 | 651.1 | 659.5 | 332.4 |
| 74913 B | 1713.7 | 1128.0 | 1358.4 | 715.1 | 706.4 | 370.0 |

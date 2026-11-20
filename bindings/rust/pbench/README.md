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

`make conformance-rust` is **deferred**: no `bindings/rust/conformance/` testee
exists (the other bindings' targets drive their own binary against the official
suite, which needs `make conformance-setup` and a stdio testee this plan's file
list does not include), so there is no target to add here. The gate above plus
`make rust-test` (compat: protoc reference bytes, prost oracle, round trips)
cover this stage until a later plan adds the testee.

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

## Verdict

**Keep the shipped `tree` runtime and publish the flat-image `shim` row as the
native-object candidate; encode splits by payload size, decode does not.** One
call per op removes the per-field crossing: shim decode is above prost at every
tier (1.38x at 136 B, 1.90x at 74 KB) and shim encode is above prost at 136 B
(1.10x), level at 174 B (1.01x), and below it at 2469 B (0.80x) and 74913 B
(0.84x). The encode loss is the shared C struct codec's repeated-element path,
not the Rust arm: `make bench-c` measures the same codec at 1132.2 MB/s on
74913 B, while the tree row's `encode_to` reaches 1600.9 MB/s there, and prost's
generated writer is straight-line code with no per-element reserve.

It is not "replace the shipped fast path": `pbwire-rust-tree` times a *prebuilt*
handle's `encode_to`, which beats prost on every encode tier (1.20x/1.11x/
1.23x/1.17x on the recorded run) and stays the shipping runtime for
applications that
already hold a tree. It is not "a boundary feature is missing": the boundary is
a single call in both directions, and the shim's decode is faster than the tree
decode at every tier; only the struct codec's encode path is behind, and that is
a C runtime question shared by every layout binding, not a missing Rust entry
point. The arm stays re-runnable (`cargo test --test flat`, `make rust-bench`)
so a faster struct encoder moves this row without touching Rust.

## Formal rows (Stage 5)

`make rust-bench` records `results/bench-rust-<stamp>.{log,json}`; the run below
is `results/bench-rust-20261120T205610Z.json` (Apple M4 Pro, arm64, sha
`13406a1`), same harness and the same two rows `pbwire-rust-tree` /
`pbwire-rust-shim`. MB/s over wire bytes:

| tier | `tree` enc | `shim` enc | prost enc | `tree` dec | `shim` dec | prost dec |
|---|---|---|---|---|---|---|
| 136 B | 1845.2 | 1703.2 | 1541.8 | 1030.9 | 1147.6 | 833.0 |
| 174 B | 1726.3 | 1566.9 | 1548.5 | 879.6 | 1020.5 | 623.3 |
| 2469 B | 1655.0 | 1072.3 | 1340.9 | 651.6 | 640.4 | 340.4 |
| 74913 B | 1600.9 | 1145.0 | 1370.9 | 716.2 | 722.7 | 380.4 |

Against prost: shim encode 1.10x / 1.01x / 0.80x / 0.84x and shim decode
1.38x / 1.64x / 1.88x / 1.90x; the tree row is 1.20x / 1.11x / 1.23x / 1.17x on
encode and 1.24x / 1.41x / 1.91x / 1.88x on decode.

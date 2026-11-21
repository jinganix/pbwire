# Rust pbench: three arms, one contract

The arms, the gate and the timing loop from
[`bindings/README.md`](../../README.md#pbench-three-arms-one-contract), applied
to `structtest.Scalars3` from `bindings/c/tests/data/matrix3.proto` and the four
standard tiers. Every arm does the same user-visible task: a native object in
and wire bytes out; wire bytes in and a fully readable native object out.

| arm | file | mechanism | what it answers |
|---|---|---|---|
| `tree` | `benchmarks/src/main.rs` | the shipped handle as an application uses it: a fresh `schema.message(name)` per encode (one `set_*` per field), `encode_to()` into a reused buffer, drop; `decode()`, one `get_*` per field into a sink, drop | what the per-field native round trip costs |
| `proxy` | `benchmarks/src/main.rs` | the prebuilt task: a held handle's `encode_to`, one-shot `decode`-and-drop with no materialization (relabelled, not re-measured) | what a held tree costs, never the headline |
| `shim` | `benchmarks/src/main.rs` | the generated `<Message>Flat` (`#[repr(C)]`) is the native object; `FlatType::encode_in_place` / `decode_in_place` cross once into `bindings/c/runtime/pbw_struct.c`, which walks that memory in place through `pbw_struct_codec_*` | is a boundary worth paying for at all |
| `core` | `pbench/core_arm.rs` | the pure-Rust port of the C core behind the `core` cargo feature (`src/core/`): one `Message` tree in and wire bytes out, wire bytes in and the same tree out | can the host language replace the C core for this binding |

The `core` arm is the one allowed second mechanism
([`bindings/README.md`](../../README.md#pbench-three-arms-one-contract)); it is
not on the default build path, so the crate keeps linking the C core and this
binding keeps one wire implementation there.

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
cd bindings/rust && cargo test --no-default-features --features core
make conformance-rust                                  # official binary suite
```

The gate asserts, on the pinned `matrix3` fixture, that the flat image's bytes
equal both the official protoc reference (`bindings/c/tests/data/matrix3.bin`)
and the pinned prost oracle, that a decode exposes every fixture value (`str`,
`inner.name`/`value`, the repeated counts and last elements, the proto3
optionals and `u64`), and that the decoded image re-encodes to the same bytes.
`rust-protobuf` parity is covered by the same fixture in the bench parity check.

The `core` arm has its own gates. `cargo test --no-default-features --features
core` runs the pure core's tests (`tests/core/`): the descriptor reader (22
fields of `structtest.Scalars3`, the four presence kinds, proto2 requiredness,
groups, oneofs), the codec (the `matrix3` fixture in both directions, unknown
retention, groups, packed, oneof, presence, the `u64`/`i64` edges, the limits)
and the wire primitives (masking, strict lengths and tags, the group skipper's
depth cap, a deterministic garbage-never-panics sweep).

`make conformance-rust` runs `bindings/rust/conformance/main.rs` -- the same
4-byte-length-framed testee the other bindings use -- against the official
suite, and it is the gate that admits the arm into `BENCHMARKS.md`. It is built
with `--no-default-features --features core`, so the binary links no C code:

```
PASSED: 1412 successes, 1403 skipped, 2 expected failures, 0 unexpected failures
PASSED: 0 successes, 445 skipped
```

Same failure set as the C core and the TypeScript, C#, Python, Dart and Julia
ports: the 2 expected are the list's `BadTag_FieldNumberSlightlyTooHigh` cases
and the skipped half is the JSON/text mapping, out of this core's (binary)
scope.

## Provisional table (Stage 3)

`make rust-bench` (best of three passes per op, one reused buffer per side).
`tree` is a prebuilt handle's `encode_to` / `decode`; `shim` is one
`encode_in_place` / `decode_in_place` on the flat image, with the image and
arena allocated once; `core` is the pure-Rust codec on its own prebuilt
`Message` tree (`pbench/core_arm.rs`), so it differs from `tree` only in who
walks the wire. MB/s over wire bytes, 2026-11-21:

| tier | `core` enc | prost enc | `core` dec | prost dec | rust-protobuf dec |
|---|---|---|---|---|---|
| 136 B | 1119.2 | 1654.1 | 433.3 | 916.3 | 769.0 |
| 174 B | 1084.8 | 1575.6 | 371.6 | 629.8 | 591.2 |
| 2469 B | 889.1 | 1259.0 | 236.2 | 335.8 | 378.6 |
| 74913 B | 895.2 | 1310.9 | 233.3 | 357.2 | 379.9 |

## Verdict

**Keep the shipped `tree` runtime and publish the flat-image `shim` row as the
native-object candidate; shim encode splits by payload size, decode does not.** One
call per op removes the per-field crossing: shim decode is above prost at every tier
(1.38x at 136 B, 2.01x at 74 KB) and shim encode is above prost at 136 B
(1.11x), level at 174 B (1.05x), and below it at 2469 B (0.87x) and 74913 B
(0.88x), formal run `results/bench-rust-20261121T184711Z.json`. The encode loss is
the shared C struct codec's repeated-element path, not the Rust arm: `make bench-c`
measures the same codec at 1132.2 MB/s on 74913 B, while the proxy row's `encode_to`
reaches 1764.4 MB/s there, and prost's generated writer is straight-line code with
no per-element reserve.

The `tree` row now times the Go-parity full task, so its encode is the per-field
build price, not the prebuilt price: 0.06x-0.09x of prost on every tier (the old
prebuilt number lives on the `proxy` row, 13.2x-22.2x above the tree on encode, never
the headline). Tree decode stays the application path and the headline: 0.82x of
prost at 136 B, then 1.04x / 1.89x / 1.99x at 174 / 2469 / 74913 B. Below-baseline
native cells are flagged for the optimization pass: tree encode on every tier, tree
decode at 136 B, shim encode at 2469 B and above, core everywhere against prost;
no optimization here.

The old prebuilt number now lives on the `proxy` row, published beside the
native rows and never the headline: a held handle's `encode_to` still beats
prost on every encode tier, but no application pays only that, so it does not
decide the default path. It is not "a boundary feature is missing": the boundary is
a single call in both directions, and the shim's decode is faster than the tree
decode at every tier; only the struct codec's encode path is behind, and that is
a C runtime question shared by every layout binding, not a missing Rust entry
point. The arm stays re-runnable (`cargo test --test flat`, `make rust-bench`)
so a faster struct encoder moves this row without touching Rust.

### The `core` arm: keep the static link as the default

**The shipped default stays the static link to the C core; the `core` feature
serves builds that cannot link C.** The pure-Rust core is not a replacement for
`pbwire-rust-tree` and it is not faster than it: on the provisional run the core
is 0.45-0.54x of the tree arm's encode (1119.2 against 1876.8 MB/s at 136 B,
895.2 against 1740.2 at 74 KB) and 0.22-0.40x of its decode. Against prost -- the
baseline that matters, because prost is pure Rust too -- it is 0.68-0.69x on
every encode and 0.47-0.70x on decode, while it beats `rust-protobuf`'s
generated writer on the two large encode tiers (1.04x) and loses every decode
(0.56-0.63x).

The reason is the tree, not the wire: prost's generated writer and reader are
straight-line code over typed struct fields, while this arm keeps the C core's
shape -- a slot vector of a `Value` enum, a boxed `Message` per submessage and a
byte vector per string -- because that is what a schema-driven codec must do when
the message type is only known at run time. On decode those allocations are what
the 74 KB payload pays (3000 `inners`, 3000 `tags`); encode allocates nothing and
lands within 4% of `rust-protobuf`.

So the arm is published for what it is: the no-C-toolchain, no-`unsafe`,
no-generated-code mode, held to the same official binary suite as the C core
(1412 successes, 1403 skipped, 2 expected, 0 unexpected), re-runnable with `make
conformance-rust` and `make rust-bench`. Anyone who can link the static library
keeps the faster handle; a cross-compiled or sandboxed build that cannot gets
the same bytes at 0.5-0.7x of prost.

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

`pbwire-rust-core` in the same run (`results/bench-rust-20261121T022430Z.json`):
MB/s over wire bytes, a prebuilt core `Message` through `encode_to` into a
reused buffer and one one-shot `decode`.

| tier | `core` enc | `core` dec | prost enc | prost dec | rust-protobuf enc | rust-protobuf dec |
|---|---|---|---|---|---|---|
| 136 B | 1008.3 | 413.0 | 1563.4 | 884.6 | 1282.8 | 731.4 |
| 174 B | 978.2 | 348.4 | 1516.8 | 612.7 | 1317.3 | 571.2 |
| 2469 B | 869.0 | 237.1 | 1242.7 | 326.2 | 831.4 | 368.7 |
| 74913 B | 923.2 | 238.4 | 1348.3 | 363.3 | 886.2 | 400.7 |

Against prost that is 0.64x / 0.64x / 0.70x / 0.68x on encode and 0.47x / 0.57x
/ 0.73x / 0.66x on decode; against `rust-protobuf` 0.79x / 0.74x / 1.05x / 1.04x
on encode and 0.56x / 0.61x / 0.64x / 0.59x on decode. The tree arm (the
shipped static link) stays ahead of it everywhere: 1.79x / 1.66x / 1.88x / 1.86x
on encode and 2.50x / 2.64x / 2.82x / 3.14x on decode.

## Go-parity re-time (Task 6)

`make rust-bench` records `results/bench-rust-<stamp>.{log,json}`; the run below
is `results/bench-rust-20261121T184711Z.json` (same machine class, sha
recorded in the JSON). The `tree` rows now time the full task: a fresh native
`Message` per iteration (one `set_*` per field, the same field list the gate
checks), `encode_to` into the reused buffer, drop; `decode`, one `get_*` per
field into a sink, drop. Allocation and free (Rust `drop`, which frees the
root message) both sit inside the timer; the encode buffer stays reused exactly
as the bench header always said. The `proxy` rows are the old prebuilt shape
relabelled, not re-measured as a new task. `shim`/`core` timer bodies are
unchanged (the app-owned flat image / core message is the steady-state hot
loop, like the reused encode buffers); their numbers below come from the same
binary run.

xlarge ns/op (74913 B) plus THROUGHPUT ratios (pbwire MB/s over baseline MB/s,
higher is faster, `✅>=1.0x`):

| arm | encode ns/op | vs prost enc | vs rust-protobuf enc | decode ns/op | vs prost dec | vs rust-protobuf dec |
|---|---|---|---|---|---|---|
| tree (full task) | 944227.0 | 0.06x | 0.09x | 99447.5 | 1.99x | 1.93x |
| shim (one downcall) | 62835.6 | 0.88x | 1.36x | 98258.1 | 2.01x | 1.95x |
| core (pure Rust) | 78749.6 | 0.70x | 1.08x | 311998.7 | 0.63x | 0.62x |
| proxy (prebuilt) | 42456.5 | 1.31x | 2.01x | 100066.6 | 1.98x | 1.92x |

Proxy/tree gap at xlarge: 22.2x encode, 0.99x decode (decode is dominated by
the C decode itself, so the per-field consume adds almost nothing there). Tree
vs prost across tiers: encode 0.09x / 0.09x / 0.06x / 0.06x, decode 0.82x /
1.04x / 1.89x / 1.99x; shim vs prost: encode 1.11x / 1.05x / 0.87x / 0.88x,
decode 1.38x / 1.70x / 1.93x / 2.01x.

Verdict: the shipped default stays the static link to the C core with the
`tree` handle API (the only dynamic path; its decode is the headline
application number and beats both baselines above 136 B). The `Flat` shim is
recommended for one-shot native traffic (fastest native-in/native-out on 6 of
8 cells against prost, best decode everywhere). `core` stays the no-C-toolchain
mode, published never headlined. `proxy` is published with the held-tree price,
never the headline.

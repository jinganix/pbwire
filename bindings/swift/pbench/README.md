# Swift pbench: three arms, one contract

The arms, the gate and the timing loop from
[`bindings/README.md`](../../README.md#pbench-three-arms-one-contract), applied
to `structtest.Scalars3` from `bindings/c/tests/data/matrix3.proto` and the four
standard tiers. Every arm does the same user-visible task: a native object in
and wire bytes out; wire bytes in and a fully readable native object out.

| arm | file | mechanism | what it answers |
|---|---|---|---|
| `tree` | `Sources/Pbench/Arms.swift` | the shipped `Message` handle: one `set` per field, then `encode()`; `decode()` then one `get` per field | what the per-field downcall costs once the tree exists |
| `shim` | `Sources/Pbench/Arms.swift` | the generated `@frozen <Message>Repr` is the native object; `InPlace.encodeInPlace` / `decodeInPlace` cross once and `bindings/swift/Sources/CPbwire/shim.c` walks that memory in place through `pbw_struct_codec_*` | is a boundary worth paying for at all |
| `core` | `n/a` | a pure-Swift port of the C core | plan 21 (`issue41-21-core-swift.md`) builds it |

`core` prints `n/a` until plan 21 lands; there is no second wire
implementation in this binding.

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

`make conformance-swift` is **deferred**: this plan's file list does not add a
conformance testee (the Swift binding's `conformance/README.md` records that the
binary/stdin testee does not exist yet, and the other bindings' targets drive
their own testee against the official suite), so there is no target to wire up
here. The gate above plus `make swift-test` cover this stage.

## Provisional table (Stage 3)

`cd bindings/swift && swift run -c release Bench compare`: interleaved rounds
(all four ops back to back), median of five. `tree` is a *prebuilt* handle's
`encode()`/`decode()` (the formal row's definition); `shim` is one
`encodeInPlace` / `decodeInPlace` call on the flat mirror, with the mirror and
arena allocated once. MB/s over wire bytes, 2026-11-21, recorded mid-session:

| tier | `tree` enc | `shim` enc | `tree` dec | `shim` dec |
|---|---|---|---|---|
| 136 B | 1073.9 | 919.3 | 879.6 | 717.3 |
| 174 B | 1115.9 | 937.2 | 801.9 | 672.4 |
| 2469 B | 1492.8 | 1004.7 | 689.3 | 534.4 |
| 74913 B | 1675.8 | 917.0 | 642.6 | 592.4 |

## Verdict

**Keep the shipped `tree` runtime and publish the flat-mirror `shim` row as the
native-object mechanism.** With one downcall per op the shim is above the
swift-protobuf baseline at every tier on both ops: 2.3x encode and 3.1x decode
at 136 B, 3.4x and 2.6x at 74 KB (formal run, `results/bench-swift-*`). The
`tree` row — a *prebuilt* handle's `encode()`/`decode()`, no per-field work in
the timed loop — is faster than the shim everywhere, because on Swift the
boundary was never per-field in that row: the handle is already one call into
the C core's own encoder, while the shim runs the generic C struct codec over
the mirror (the same codec the C, Dart, Julia, Rust and C# shim rows time).

So it is not (a) — "replace the shipped fast path": `pbwire-swift-tree` stays
the runtime an application that holds a `Message` should use, and it is the
fastest row here. It is not (c) — "a boundary feature is missing": the shim
needs no new entry point, it is above the baseline from 136 B to 74 KB. What it
answers is (b): a native `@frozen` value type can be encoded and decoded with
one downcall and no ARC per field, at 3.1-3.9x the official runtime, so a Swift
application that wants its own object shape never has to build a `Message`
tree to get there. The gap to the handle is the shared struct codec's encode
path (it writes proto3 implicit scalars unconditionally and reserves per
repeated element), a C runtime question that moves every layout row at once —
recorded in `docs/perf-attempts.md`, not patched under this plan.

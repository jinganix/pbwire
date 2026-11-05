# Protobuf pain points — which ones pbwire solves

Developers complain about two different things under the name "Protobuf
problems": the **protocol design** (field compatibility, schema evolution,
what the wire format can express) and the **official implementation**
(code generation, build burden, `DynamicMessage` speed, API complexity).
pbwire only addresses the second class. Protocol semantics are intentionally
identical to the reference implementation — byte-identical encodings, same
wire compatibility rules — and proven by `make difftest` and
`make conformance` rather than by reasoning. If you are searching for how to
avoid a specific official-library pain, the table below is the index; each
row links to the section with the fix and its limits.

- [Summary table](#summary-which-complaint-pbwire-answers)
- [How to avoid Protobuf code generation bloat?](#how-to-avoid-protobuf-code-generation-bloat)
- [How to simplify the protoc toolchain?](#how-to-simplify-the-protoc-toolchain)
- [Why is DynamicMessage slow, and what is faster?](#why-is-dynamicmessage-slow-and-what-is-faster)
- [Why do Protobuf APIs differ per language?](#why-do-protobuf-apis-differ-per-language)
- [How to avoid the Protobuf C++ dependency and build burden?](#how-to-avoid-the-protobuf-c-dependency-and-build-burden)
- [How to survive Protobuf version upgrades?](#how-to-survive-protobuf-version-upgrades)
- [How to handle Protobuf JSON, int64, Any and special fields?](#how-to-handle-protobuf-json-int64-any-and-special-fields)
- [How to forward messages without losing unknown fields?](#how-to-forward-messages-without-losing-unknown-fields)
- [How to keep Protobuf memory management simple?](#how-to-keep-protobuf-memory-management-simple)
- [What pbwire deliberately does not change](#what-pbwire-deliberately-does-not-change)

## Summary: which complaint pbwire answers

| Complaint about the official library | Status in pbwire | Where |
|---|---|---|
| Generated `.pb.cc` / `.pb.go` bloat, slow builds, big binaries | Solved: no generated code, schema loads at runtime | [codegen](#how-to-avoid-protobuf-code-generation-bloat) |
| `protoc` + per-language plugins + include paths in every CI job | Eased: one `FileDescriptorSet` built once, no `protoc` at runtime | [toolchain](#how-to-simplify-the-protoc-toolchain) |
| `DynamicMessage` much slower than generated code | Solved: `pbw_ctx` beats generated C++ 1.1–1.7x, `DynamicMessage` 8–15x on encode | [dynamic](#why-is-dynamicmessage-slow-and-what-is-faster) |
| A different API, defaults and habits in every language | Unified semantics, idiomatic APIs per language — by design, not one API everywhere | [consistency](#why-do-protobuf-apis-differ-per-language) |
| libprotobuf / Abseil weight, Bazel–CMake friction, C++ upgrade churn | Solved: pure C99 core, zero runtime dependencies | [dependencies](#how-to-avoid-the-protobuf-c-dependency-and-build-burden) |
| Runtime upgrades break generated code and plugins | Mitigated: stable C ABI (`PBW_ABI_VERSION`), bindings decoupled from core | [upgrades](#how-to-survive-protobuf-version-upgrades) |
| JSON mapping edge cases: int64 precision, enums, `Any`, `Timestamp`, defaults vs presence | Solved: full JSON + text format, official conformance green | [JSON](#how-to-handle-protobuf-json-int64-any-and-special-fields) |
| Gateways drop unknown fields when converting to plain objects | Solved: unknown fields preserved as raw bytes, re-emitted in order | [unknowns](#how-to-forward-messages-without-losing-unknown-fields) |
| Arena lifetimes are easy to misuse; per-message allocation is wasteful | Simplified: two strategies (reusable `pbw_ctx` vs owning plain API), one rule each | [memory](#how-to-keep-protobuf-memory-management-simple) |
| Protobuf reflection API is complex and slow | Simplified: small C API, precomputed field lookup | [memory](#how-to-keep-protobuf-memory-management-simple) |
| Schema evolution / field compatibility rules | Inherited, not changed: same wire rules as the reference | [not changed](#what-pbwire-deliberately-does-not-change) |
| Stable serialization for hashing across versions | Intentionally not canonical: deterministic mode sorts maps only, like the reference | [not changed](#what-pbwire-deliberately-does-not-change) |

## How to avoid Protobuf code generation bloat?

The classic complaint: 191 `.proto` files cost ~5 minutes of codegen
compilation and hundreds of MB of objects; a Go developer wrote easyproto
because `*.pb.go` added tens of MB of unneeded runtime to the binary.

pbwire generates nothing at runtime. Descriptors go in (hand-built or a
standard `FileDescriptorSet`), dynamic messages come out — adding a message
type never requires a rebuild unless you embedded the descriptor bytes in the
binary. Generated models exist (`codegen`, plus the C struct plugin) for
ergonomics, but they are thin wrappers over the dynamic API, never a second
wire implementation. See [Is pbwire a Protobuf code generator?](faq.md#is-pbwire-a-protobuf-code-generator)
and [Do I have to recompile when a `.proto` changes?](faq.md#do-i-have-to-recompile-when-a-proto-changes).

## How to simplify the protoc toolchain?

`protoc`, one plugin per language, version pinning, include paths, and a
per-language generation step in CI — Buf exists largely because of this pain.

pbwire moves `protoc` from "every build, every language" to "once, to bytes":
`protoc --descriptor_set_out`, `buf build -o`, or the optional
`pbwire-protoc` importer produces a `FileDescriptorSet` a single time, and
every binding loads those bytes at runtime with `pbw_descriptor_set_load()`.
No plugin versions to pin per language, no include paths at runtime. Honest
limit: the descriptor set still has to be produced somewhere — pbwire removes
the per-language codegen pipeline, not the schema authoring step.

## Why is DynamicMessage slow, and what is faster?

Official `DynamicMessage` is descriptor-driven interpretation: field lookup
per access, extra allocations, typically far behind generated code (Java
developers report exactly this on Stack Overflow).

pbwire is descriptor-driven too, but built for it: precomputed lookup tables,
precomputed tag bytes, single-pass writes (the C++ serializer sizes first,
then writes), one bump arena plus one reusable encode buffer per `pbw_ctx`.
Measured on Apple M4 Pro (`protoc 36.2`, `make bench`, byte-equality checked
before timing): the in-process `pbw_ctx` API runs 1.1–1.7x the generated C++
codec on every payload tier and 8–15x `DynamicMessage` on encode, 2–4x on
decode. The two places generic dispatch still shows are documented, not
hidden: the plain one-`malloc`-per-call API near-ties generated code on small
messages, and tiny-message straight-line writes in some host bindings. Full
tables per language are in [BENCHMARKS.md](../BENCHMARKS.md).

## Why do Protobuf APIs differ per language?

C++ `Reflection` + arenas, Java builders, Python metaclasses, Go structs +
reflection, PHP dynamic objects — same `.proto`, different mental models,
occasionally different default/presence behavior.

pbwire unifies the semantics, not the spelling: one descriptor model, one
parse/encode path, one unknown-field rule, one repeated/map model, one JSON
mapping — in the shared C core — with each binding exposing it the way its
language expects (CPython extension, N-API addon, pure-Java codec, NIF,
`dart:ffi`, …). The per-language pain each binding removes is the table in
[bindings/README.md](../bindings/README.md#what-each-binding-solves).

## How to avoid the Protobuf C++ dependency and build burden?

libprotobuf plus Abseil, Bazel/CMake compatibility, Autotools removal in 4.22
— for a small C/C++ project the build can cost more than the codec.

The core is C99, one public header, zero runtime dependencies: `make` and
`make test` need nothing but a compiler. No globals, no locks, no
thread-local state, so embedding (Erlang NIF, Python C extension, Go cgo) is
a link step, not a runtime to adopt. The optional `.proto` importer is a
separate library (`pbwire-protoc`, C++17 + libprotobuf) that meets the core
only at serialized `FileDescriptorSet` bytes.

## How to survive Protobuf version upgrades?

The wire format is stable, but runtime APIs break across versions (C++ API
removals, Python runtime upgrades, Bazel rule migrations — the official
migration guide keeps growing).

pbwire keeps the C ABI versioned (`PBW_ABI_VERSION` + `pbw_abi_version()` +
SONAME move together; additive changes keep the number) and the bindings
check it at load time, so an incompatible mix fails fast instead of
corrupting memory. Caveat stated plainly: the project is young (`0.1.0`), so
the stability promise is a mechanism plus a policy, not a decade of history.

## How to handle Protobuf JSON, int64, Any and special fields?

int64-as-string precision, enum name/number conversion, `Any` resolution,
`Timestamp`/`Duration` bounds, presence vs defaults — the official issue
tracker still collects these.

pbwire implements the full JSON mapping and text format with no dependencies
(`pbw_json_print`/`pbw_json_parse`, `pbw_text_print`/`pbw_text_parse`),
including all well-known types, extensions as `[full.name]`, strict
base64/number rules, and closed-enum unknowns kept as unknown fields.
Verdict: same results as the reference parser on 40,000 mutated inputs;
official conformance binary + JSON 2745/2745 runnable (2 expected failures
the reference C++ testee fails identically; 4 JSON cases pass that the
reference itself fails) and text 445/445 (including 4 the reference fails).
Details in [testing.md](testing.md) and [guide.md](guide.md#json-and-text-format).

## How to forward messages without losing unknown fields?

The well-known gateway bug: convert to a plain object, rebuild, and fields
from a newer schema silently disappear.

In pbwire unknown fields are data, not noise: anything the schema does not
declare — including a known field with the wrong wire type and legacy groups
— is stored as raw wire bytes and re-emitted after the known fields in input
order. A proxy can forward a schema it only partially knows, and 7
conformance tests depend on exactly this. See [Unknown fields](guide.md#unknown-fields).

## How to keep Protobuf memory management simple?

Two recurring complaints: arenas with lifetimes that are easy to get wrong,
and per-message allocation storms (or hidden copies) in hot paths.

pbwire offers exactly two strategies with the same bytes — [guide.md](guide.md#reuse-one-arena-in-a-loop)
has the one-page version:

- Hot loop: one `pbw_ctx` per thread (reusable arena + encode buffer, no
  `malloc` after warm-up, `pbw_ctx_reset()` frees a whole tree by moving a
  pointer). Messages are read-only; a 1 MiB retain cap (tunable) bounds what
  stays resident.
- One-off or long-lived: plain `pbw_decode`/`pbw_encode` — one arena
  allocation per tree, caller-owned, mutable.

Every failure path returns a `pbw_status` (never `abort`), wire input is
bounds-checked with nesting capped at 256, and `pbw_limits` lowers the caps
per call. What is *not* claimed: decode still materializes a tree (it is not
a zero-copy view like FlatBuffers/Cap'n Proto); the win is one allocation
per tree instead of one per field, plus buffer reuse on the hot path.

## What pbwire deliberately does not change

- **Protocol design.** Field compatibility, schema evolution, proto2 vs
  proto3 semantics: inherited from the reference, byte for byte. If your
  problem is "the protocol cannot express X", pbwire is the wrong fix.
- **Canonical encoding.** Deterministic mode sorts map entries and is
  otherwise byte-identical to normal encode — matching the reference, which
  explicitly does not promise cross-version stable bytes for hashing.
- **Descriptors are trusted input.** Wire bytes are validated as hostile;
  descriptor bytes are not. Do not load descriptor sets from untrusted
  sources without your own validation ([design.md](design.md#scope-and-boundaries)).

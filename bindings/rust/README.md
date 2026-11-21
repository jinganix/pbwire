# pbwire Rust binding

`bindings/rust` is the Rust binding: a safe wrapper over the C ABI in
[`include/pbwire.h`](../../include/pbwire.h), linked as `libpbwire.a`. A
`FileDescriptorSet` (`protoc --descriptor_set_out`) becomes a schema at run
time; there is no generated code, no `build.rs` codegen and no bindgen step.
Wire bytes are the core's, so a Rust message and a C message are the same bytes
by construction.

```rust
use pbwire::Schema;

let schema = Schema::from_file("tests/data/matrix3.desc")?;
let ty = schema.message("structtest.Scalars3")?;
let inner_ty = schema.message("structtest.Inner3")?;

let mut msg = ty.new_message();
msg.set_i64(ty.field("i32")?, -42)
    .set_str(ty.field("str")?, "alice");
for tag in ["a", "bb"] {
    msg.set_str(ty.field("tags")?, tag);          // repeated: appends
}
msg.set_message(ty.field("inner")?)              // nested: created in place
    .set_str(inner_ty.field("name")?, "kid")
    .set_i64(inner_ty.field("value")?, 42);

let wire = msg.encode()?;
let decoded = ty.decode(&wire)?;                 // unknown fields preserved
assert_eq!(decoded.encode()?, wire);             // byte-exact round trip
# Ok::<(), pbwire::Error>(())
```

A `Field` is resolved once (`ty.field("i32")`) and reused, like the Java
binding's cached `FieldInfo`: a `set_*`/`get_*` call is one native call, not a
name lookup.

## Build and run

```shell
make -C ../..            # build/libpbwire.a, the archive the crate links
cargo test               # self-check against the pinned protoc bytes
cargo run --example roundtrip -- ../c/tests/data/matrix3.desc
```

`build.rs` copies `build/libpbwire.a` into `OUT_DIR` and links it statically, so
the resulting binary has no runtime dependency on the library. It also compiles
`bindings/c/runtime/pbw_struct.c` into a second archive (through `CC`/`AR`,
default `cc`/`ar`): that is the layout codec the in-place arm calls.
`PBWIRE_LIB_DIR` points at a different build directory. Everything is
`cdylib`-free on purpose: the FFI hosts (Java, Node, Objective-C, PHP, Dart, C#)
`dlopen` the shared library, while Rust compiles the archive in.

The `core` cargo feature adds a second, independent codec: the pure-Rust port of
the C core in `src/core/` (descriptor reader, value tree, encoder, decoder).
It is off by default -- the default build above is unchanged -- and it is the
only mode that needs no C toolchain, no `unsafe` and no generated code:

```shell
cargo test --no-default-features --features core   # descriptor + codec + wire tests
make -C ../.. conformance-rust                     # official binary suite (1412/2)
cargo run --no-default-features --features core --bin pbwire-conformance -- ../../build/conformance.desc
```

It is the pbench `core` arm, published as the no-native mode
(`pbench/README.md`): the shipped handle (the static link) stays 1.66-1.88x its
encode and 2.50-3.14x its decode, and prost is 1.4-1.5x its encode.

## Benchmarks

```shell
./benchmarks/bench.sh    # vs rust-protobuf 3.x generated code + prost, four tiers
```

Needs `cargo install protobuf-codegen` (it provides `protoc-gen-rs` and the
runtime the generated code uses); the prost baseline generates at build time
from crates.io. The four tiers are small / medium / large /
xlarge (the repeated fields scaled x0 / x1 / x100 / x3000). Copied from
[BENCHMARKS.md](../../BENCHMARKS.md), which is the single source for all
performance numbers; higher is better, MB/s over wire bytes:

| payload | op | pbwire-rust-tree (MB/s, x prost) | pbwire-rust-shim (MB/s, x prost) | prost (MB/s) | protobuf-rust (MB/s) |
|---|---|---|---|---|---|
| 136 B | encode | 1845.2 (1.20x) | 1703.2 (1.10x) | 1541.8 | 1248.5 |
| 136 B | decode | 1030.9 (1.24x) | 1147.6 (1.38x) | 833.0 | 722.5 |
| 174 B | encode | 1726.3 (1.11x) | 1566.9 (1.01x) | 1548.5 | 1336.8 |
| 174 B | decode | 879.6 (1.41x) | 1020.5 (1.64x) | 623.3 | 567.6 |
| 2469 B | encode | 1655.0 (1.23x) | 1072.3 (0.80x) | 1340.9 | 822.6 |
| 2469 B | decode | 651.6 (1.91x) | 640.4 (1.88x) | 340.4 | 344.3 |
| 74913 B | encode | 1600.9 (1.17x) | 1145.0 (0.84x) | 1370.9 | 845.1 |
| 74913 B | decode | 716.2 (1.88x) | 722.7 (1.90x) | 380.4 | 381.6 |

`set_str` rejects a string containing a NUL byte (it would truncate on the way
to C); use `set_bytes` for binary payloads, including on a `string` field.

### The native-object arm

`pbwire-rust-tree` times a prebuilt `Message`; `pbwire-rust-shim` times the
generated `#[repr(C)]` mirror with one call per op:

```rust
let mut plan = schema.in_place::<Scalars3Flat>()?;   // compiles the rows once
let mut buf = plan.encode_in_place(&image)?;         // bytes borrowed from the plan
let mut decoded = FlatImage::<Scalars3Flat>::new();  // mirror + reusable arena
plan.decode_in_place(&buf, &mut decoded)?;
```

The generated `<Message>Flat::FIELDS` rows carry the offsets and wire facts;
`build.rs` compiles the C codec they address. Decode allocates strings, bytes,
arrays and sub-messages from the image's arena, so the image must not outlive
it (decoding again into the same image invalidates the previous view). The arm,
its gate (`cargo test --test flat`) and the verdict live in
[`pbench/README.md`](pbench/README.md).

## Threads

A `Schema` is immutable once loaded, so `&Schema` can be shared. A `Message` is
not `Send`/`Sync` on purpose: like the C core, one tree belongs to one thread.

## Generated models

`protoc-gen-pbwire` (in `../../codegen`) generates prost-shaped structs:
the same field names, `Option` for optional/message/oneof fields, `i32` for
enums, nested modules, and `encode_to_vec` / `decode` over the embedded
schema — no schema argument at use sites. Each file splits in two:
`<mod>.rs` (models, including the sibling file as `pub mod meta`) and
`<mod>_meta.rs` (the embedded `DESC` plus the per-thread lazy `SCHEMA`).
`--pbwire_opt=lang=rust[,model=bool][,register=bool][,registry=bool]`
(all default `true`): `model=false` leaves the schema file for
dynamic-only users; `register=false` replaces the implicit schema with an
explicit per-thread `init()` (loud otherwise); `registry=false` drops
`MESSAGE_NAMES` + `preload()`. External descriptor sets keep working
through the raw runtime API (`Schema::from_file`); `Schema` stays
`!Sync` by design (one thread per schema, like the C core), so models
follow the embedded set. The committed output is
`tests/gen/` and `examples/gen/`; `examples/roundtrip.rs` assigns typed
fields only, and `tests/compat.rs` proves byte and field parity with the
pinned prost oracle (T1–T6). See `../../codegen/README.md`.

## Core coverage

The safe wrapper exposes the same surface as the C ABI, still over the statically
linked core:

| member | what it does |
|---|---|
| `Type::parse_json(text, ignore_unknown)` / `Message::to_json()` | JSON mapping |
| `Type::parse_text(text)` / `Message::to_text(print_unknown)` | text format |
| `Message::encode_deterministic()` | map entries sorted by key |
| `Message::encode_delimited()` / `Type::decode_delimited(wire)` | framing, returns `consumed` |
| `Message::encoded_size()` | exact size without writing |
| `Type::parse_json_limits(text, ignore_unknown, max_bytes, max_depth)`, `parse_text_limits(...)` | bounds on the JSON/text parsers |
| `Type::visit(wire, &mut cb)` | single-level scan; a panicking callback is caught, never unwinds into C |
| `Type::any_pack(url, payload)`, `Message::any_type_url()`/`any_payload()`, `Type::timestamp_from_millis(ms)`/`duration_from_millis(ms)`, `Message::timestamp_millis()`/`duration_millis()`, `Message::fieldmask_path_count()`/`fieldmask_path_at(i)` | WKT helpers |
| `Schema::service(..)`/`service_at`/`service_count`, `Schema::resolve_method(service, method)` | routing metadata |

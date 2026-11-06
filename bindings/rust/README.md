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
the resulting binary has no runtime dependency on the library. `PBWIRE_LIB_DIR`
points at a different build directory. Everything is `cdylib`-free on purpose:
the FFI hosts (Java, Node, Objective-C, PHP, Dart, C#) `dlopen` the shared
library, while Rust compiles the archive in.

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

| payload | op | pbwire-rust (MB/s, x protobuf-rust) | protobuf-rust (MB/s) | prost (MB/s) |
|---|---|---|---|---|
| 136 B | encode | 1962.9 (1.5x) | 1288.1 | 1601.8 |
| 136 B | decode | 1169.9 (1.5x) | 775.2 | 941.0 |
| 174 B | encode | 1870.2 (1.4x) | 1338.7 | 1470.4 |
| 174 B | decode | 963.1 (1.7x) | 570.7 | 655.5 |
| 2469 B | encode | 1532.9 (1.8x) | 838.7 | 1248.4 |
| 2469 B | decode | 682.0 (1.8x) | 370.2 | 365.5 |
| 74913 B | encode | 1398.4 (1.6x) | 849.5 | 1296.6 |
| 74913 B | decode | 729.1 (1.8x) | 406.1 | 390.3 |

`set_str` rejects a string containing a NUL byte (it would truncate on the way
to C); use `set_bytes` for binary payloads, including on a `string` field.

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

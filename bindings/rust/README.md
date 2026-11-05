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
| 136 B | encode | 1830.6 (1.39x) | 1319.7 | 1592.0 |
| 136 B | decode | 943.2 (1.24x) | 760.7 | 924.6 |
| 174 B | encode | 1736.3 (1.28x) | 1358.7 | 1545.9 |
| 174 B | decode | 821.0 (1.40x) | 587.5 | 657.6 |
| 2469 B | encode | 1696.1 (2.05x) | 828.5 | 1470.1 |
| 2469 B | decode | 637.1 (1.70x) | 374.0 | 355.6 |
| 74913 B | encode | 1596.5 (1.84x) | 866.2 | 1468.1 |
| 74913 B | decode | 669.0 (1.69x) | 396.4 | 407.7 |

`set_str` rejects a string containing a NUL byte (it would truncate on the way
to C); use `set_bytes` for binary payloads, including on a `string` field.

## Threads

A `Schema` is immutable once loaded, so `&Schema` can be shared. A `Message` is
not `Send`/`Sync` on purpose: like the C core, one tree belongs to one thread.

## Generated models

`protoc-gen-pbwire` (in `../../codegen`) generates prost-shaped structs:
the same field names, `Option` for optional/message/oneof fields, `i32` for
enums, nested modules, and `encode_to_vec` / `decode` over the embedded
schema — no schema argument at use sites. The committed output is
`tests/gen/` and `examples/gen/`; `examples/roundtrip.rs` assigns typed
fields only, and `tests/compat.rs` proves byte and field parity with the
pinned prost oracle (T1–T6). See `../../codegen/README.md`.

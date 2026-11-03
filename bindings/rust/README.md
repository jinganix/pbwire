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
./benchmarks/bench.sh    # vs rust-protobuf 3.x generated code, four tiers
```

Needs `cargo install protobuf-codegen` (it provides `protoc-gen-rs` and the
runtime the generated code uses). Numbers live in
[BENCHMARKS.md](../../BENCHMARKS.md).

`set_str` rejects a string containing a NUL byte (it would truncate on the way
to C); use `set_bytes` for binary payloads, including on a `string` field.

## Threads

A `Schema` is immutable once loaded, so `&Schema` can be shared. A `Message` is
not `Send`/`Sync` on purpose: like the C core, one tree belongs to one thread.

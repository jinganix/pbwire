# pbwire Zig binding

Zig binding: a safe wrapper over the C ABI in
[`include/pbwire.h`](../../include/pbwire.h), linked as a static library. A
`FileDescriptorSet` (`protoc --descriptor_set_out`) becomes a schema at run
time; there is no generated code, no build-time codegen and no bindgen step.
Wire bytes are the core's, so a Zig message and a C message are the same bytes
by construction.

```zig
const pbwire = @import("pbwire");

var schema = try pbwire.Schema.fromFile(allocator, "matrix3.desc");
defer schema.deinit();
const ty = try schema.messageType("structtest.Scalars3");
const inner_ty = try schema.messageType("structtest.Inner3");

var msg = try ty.newMessage();
defer msg.deinit();
try msg.setI64(try ty.field("i32"), -42);
try msg.setStr(try ty.field("str"), "alice");
for ([_][]const u8{ "a", "bb" }) |tag| try msg.setStr(try ty.field("tags"), tag);
var inner = try msg.setMessage(try ty.field("inner")); // nested: created in place
try inner.setStr(try inner_ty.field("name"), "kid");
try inner.setI64(try inner_ty.field("value"), 42);

const wire = try msg.encode(allocator);
defer allocator.free(wire);
var decoded = try ty.decode(wire); // unknown fields preserved
defer decoded.deinit();
const back = try decoded.encode(allocator); // byte-exact round trip
defer allocator.free(back);
```

A `Field` is resolved once (`ty.field("i32")`) and reused: a `set*`/`get*`
call is one native call, not a name lookup.

## Build and run

```shell
zig build test     # self-check against the pinned protoc bytes
zig build example  # runnable round trip (takes an optional <matrix3.desc>)
zig build bench -- matrix3.desc   # the four standard tiers
zig build testee   # conformance testee -> zig-out/bin/testee-zig
```

`build.zig` compiles the core sources (`../../src/*.c`) into a static library
and links it into every artifact, so the result has no runtime dependency on
the library. There is no `dlopen`, no `bindgen`, and no second wire
implementation. `setStr` rejects a string containing a NUL byte (it would
truncate on the way to C); use `setBytes` for binary payloads, including on a
`string` field.

## Benchmarks

```shell
./benchmarks/bench.sh [matrix3.desc]             # pbwire column only, offline-safe
./benchmarks/bench.sh --baseline [matrix3.desc]  # vs Arwalk/zig-protobuf (network once)
```

The four tiers are small / medium / large / xlarge (the repeated fields scaled
x0 / x1 / x100 / x3000, landing on 136 / 174 / 2469 / 74913 bytes), the same
payload as every other binding (see `BENCHMARKS.md`). The baseline is
Arwalk/zig-protobuf generated code from the same `matrix3.proto`: the tarball
is pinned in `build.zig.zon` and fetched on first run (then cached), the
generator runs under the system `protoc`, and `zig build test` plus the plain
`bench` step never resolve the dependency, so they stay offline-safe. The
comparison harness (`benchmarks/bench_vs_baseline.zig`, `zig build
-Dbaseline=true bench-baseline`) times both sides with shared iteration
counts and gates every tier on byte-identical output before timing.

## Threads

A `Schema` is immutable once loaded, so it can be shared between threads. A
`Message` is not thread safe on purpose: like the C core, one tree belongs to
one thread.

## Conformance

`conformance/testee.zig` speaks the runner's wire protocol and covers binary,
JSON and text (JSPB answers `skipped`); see `conformance/README.md`.
Error-result strings carry the Zig error name rather than the core's
`strerror` text — the runner only checks which result field is set, not the
message. The two binary tag-truncation cases the reference C++ testee fails
identically apply here too (same core decoder), so the shared
`failure_list_pbwire.txt` covers this testee as well.

## Layout

| path | contents |
|------|----------|
| `runtime/pbwire.zig` | the binding: `Schema`, `MessageType`, `Field`, `Message`, `Value` over `@cImport` |
| `build.zig` / `build.zig.zon` | static-libpbwire step plus `test` / `example` / `bench` / `testee` steps |
| `examples/roundtrip.zig` | runnable round trip |
| `tests/selftest.zig` | self-check (`zig build test`): oracle bytes, round trip, rejects, text/JSON |
| `benchmarks/bench.zig` + `bench.sh` | the four standard tiers |
| `conformance/testee.zig` | official-suite testee (binary + JSON + text, JSPB skipped) |

# pbwire C binding

Three pieces, like every language binding in this repo:

| Piece | Path | Role |
|---|---|---|
| 1. Binding runtime | [`runtime/`](runtime/) | generic struct engine: layout table + schema walk (`pbw_struct_encode/decode/free`) |
| 2. Compiler plugin | [`plugin/`](plugin/) | `protoc-gen-pbwire-c`: `.proto` -> structs + layout tables |
| 3. Example | [`example/`](example/) | addressbook round-trip on plain structs |

The C binding is the core library itself plus this thin struct layer: no
wrapper objects, no code generation at build time, no runtime dependencies
beyond a C99 compiler (the plugin needs Go, but only maintainers run it).

## 1. Runtime

[`runtime/pbw_struct.h`](runtime/pbw_struct.h) declares the layout row
(field number -> struct offset) and the engine entry points. Application
structs stay plain data; the engine walks the `pbw_schema` (the truth for
field numbers, wire types and proto2/proto3 semantics) and reads/writes
struct memory through the rows:

```c
static const pbw_struct_field_t person_fields[] = {
    {1, PBW_SF_STRING, 0, offsetof(Person, name), 0, 0, 0, 0, NULL, 0},
    ...
};
pbw_struct_encode(schema, "tutorial.Person", person_fields, 4, &person, &wire, &len);
```

`decode` allocates (strings, arrays, sub-messages) into a zeroed struct;
release with `pbw_struct_free()`. Every schema field needs a row: a missing
row is `PBW_ERR_INVALID`, so a wrong field number fails loudly instead of
dropping data silently. Unsupported kinds (bytes, groups, maps, oneofs)
fail the same way; extend the engine when a user needs them.

## 2. Plugin

`plugin/` is a self-contained Go module (only dependency:
`google.golang.org/protobuf`). Its harness (`internal/pluginutil`,
`internal/core`) is ported from webpb's protoc plugin framework; the
generator (`internal/cgen`) is new.

```shell
make -C bindings/c/plugin test    # golden tests, protoc-free (replay dumps)
make -C bindings/c/plugin regen    # needs protoc: rebuild the pinned headers + descriptor sets
make -C bindings/c/plugin dump     # needs protoc: re-capture testdata dumps
```

Golden tests compare generator output byte-for-byte against
[`example/gen/addressbook.h`](example/gen/addressbook.h) and
[`tests/gen/matrix{2,3}.h`](../tests/gen/matrix3.h),
which are also the files the example and the self-check compile against:
the test and the build can never disagree. Each `.proto` has a single
source (`examples/addressbook.proto`, `tests/data/matrix{2,3}.proto`);
`testdata/` stores only the captured `CodeGeneratorRequest` dumps plus the
regen commands, so `make test` at the repo root never needs protoc.

```shell
make test    # from the repo root: also runs bindings/c/tests/test_struct
```

[`tests/test_struct.c`](../tests/test_struct.c) is the binding self-check
(no framework, no dependencies): the generated structs encode to the exact
bytes `protoc --encode` produces (pinned `.bin` next to each `.proto`, from
the checked-in `.txt`), decode recovers every field, and layout mismatches,
missing required fields, truncated input and bad arguments all fail loudly.

## 3. Example

[`example/addressbook.c`](example/addressbook.c) mirrors the shared
[`examples/addressbook.proto`](../../examples/addressbook.proto) (the
classic tutorial schema) with plain structs and the generated wrappers:

```c
tutorial_person_t people[] = {
    { .name = "Alice", .id = 1, .phones = alice_phones, .n_phones = 1 },
    ...
};
tutorial_address_book_t book = { .people = people, .n_people = 2 };
tutorial_address_book_encode(schema, &book, &wire, &wire_len);
tutorial_address_book_decode(schema, wire, wire_len, &got);
tutorial_address_book_free(&got);
```

The schema comes from [`examples/addressbook.desc`](../../examples/addressbook.desc)
(written by `protoc --descriptor_set_out`, never embedded): the generated
header only provides its path (`ADDRESSBOOK_DESC_PATH`), and one
`pbw_struct_schema_load()` call loads every message in the file. No
per-message setup and no hand-written schema code remain in the example.
Run from the repo root so the relative path resolves.

Run it: `make examples` from the repo root.

Every binding example in this repo follows the same shape (the C++ one
in [`bindings/cpp/`](../../cpp/) mirrors it line for line):

1. Fill the generated structs with literals (the language's own
   assignment: designated initializers in C, aggregate init in C++).
2. Load the schema once (`pbw_struct_schema_load` reads the whole
   `FileDescriptorSet`, so every message in the file comes with it).
3. `encode` the struct, `decode` the bytes back, compare the two structs
   field by field.
4. Silent on success: no output, exit code only (`0` = equal).

Two things to notice when reading the example:

- The `.proto` is proto2 (`required` fields), so the schema switches
  dialect with `pbw_schema_set_proto3(schema, 0)`.
- Absent enums decode to their proto default (`type = 2 [default = HOME]`
  yields `HOME` with `has_type == 0`); the layout row carries it.

## Benchmark

[`benchmarks/bench_struct.cc`](benchmarks/bench_struct.cc) measures the
generated structs against the official C++ generated code (`protoc
--cpp_out`) on the same `tests/data/matrix3.proto` message, at the four
standard tiers from [`../../BENCHMARKS.md`](../../BENCHMARKS.md) (small /
medium / large / xlarge scale the repeated fields x0 / x1 / x100 / x3000).
Tables use the same `impl op ns/op MB/s` columns as `bench/bench.cc`, so the
root document can aggregate them.

```shell
make bench-c    # from the repo root; needs protoc + libprotobuf like `make bench`
```

It checks struct encode == `protoc` bytes (plus a decode→encode round trip)
before timing. Decode rows include the mandatory `free`: the struct decoder
allocates into the output, so releasing is part of every decode.

# pbwire C binding

Three pieces, like every language binding in this repo:

| Piece | Path | Role |
|---|---|---|
| 1. Binding runtime | [`runtime/`](runtime/) | generic struct codec: layout table, direct wire <-> struct (`pbw_struct_encode/decode/free`) |
| 2. Compiler plugin | [`plugin/`](plugin/) | `protoc-gen-pbwire-c`: `.proto` -> structs + layout tables |
| 3. Example | [`example/`](example/) | addressbook round-trip on plain structs |

The C binding is the core library itself plus this thin struct layer: no
wrapper objects, no code generation at build time, no runtime dependencies
beyond a C99 compiler (the plugin needs Go, but only maintainers run it).

## 1. Runtime

[`runtime/pbw_struct.h`](runtime/pbw_struct.h) declares the layout row
(field number -> struct offset) and the codec entry points. Application
structs stay plain data; the codec walks the layout rows and reads/writes
wire bytes directly — no `pbw_message` tree in between, which is what keeps
it within reach of generated code (the same design as the Erlang binding's
hand-written `ep_encoder.c` / `ep_decoder.c`):

```c
static const pbw_struct_field_t person_fields[] = {
    {1, PBW_SF_STRING, 0, offsetof(Person, name), 0, 0, 0, 0, NULL, 0,
     PBW_TYPE_STRING, 2, 0, 0},
    ...
};
pbw_struct_encode(schema, "tutorial.Person", person_fields, 4, &person, &wire, &len);
```

The last four members (`type`, `wire`, `packed`, `required`) are the field's
wire facts, emitted by the plugin so the codec needs no schema lookup on the
hot path. A row that leaves `type` at 0 makes the codec resolve the field
through the schema instead, which is slower but identical — that is how
hand-written tables (and the tests) keep working. The `pbw_schema` stays the
truth for field numbers and proto2/proto3 semantics: the row count must match
the descriptor, and a row whose kind/wire type disagrees with its own `type`
is `PBW_ERR_INVALID`.

`decode` allocates (strings, arrays, sub-messages) into a zeroed struct;
release with `pbw_struct_free()`. Repeated fields grow by doubling from the
count member, so a message with empty repeated fields allocates nothing.
Unsupported kinds (bytes, groups, maps, oneofs) fail with `PBW_ERR_INVALID`;
extend the codec when a user needs them.

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

Since the codec went direct (2026-11), the numbers are in the same league as
generated code rather than 6x behind it: on the 2.5 KB and 75 KB tiers the
struct codec matches or beats `protoc --cpp_out` (encode 1.11x, decode 1.20x
on 75 KB), and it is ~1.7x slower on the 136 B / 174 B tiers, where generated
straight-line code wins because it has no per-row dispatch at all. Generated
codecs (the plugin emitting `encode`/`decode` per message instead of tables)
are the way to close that last gap.

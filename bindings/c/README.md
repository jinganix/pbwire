# pbwire C binding

Three pieces, like every language binding in this repo:

| Piece | Path | Role |
|---|---|---|
| 1. Binding runtime | [`runtime/`](runtime/) | generic struct codec: layout table, direct wire <-> struct (`pbw_struct_encode/decode/free`, plus the compiled `pbw_struct_codec`) |
| 2. Compiler plugin | [`plugin/`](plugin/) | `protoc-gen-pbwire-c`: `.proto` -> structs + layout tables |
| 3. Example | [`examples/`](examples/) | addressbook round-trip on plain structs |

The C binding is the core library itself plus this thin struct layer: no
wrapper objects, no code generation at build time, no runtime dependencies
beyond a C99 compiler (the plugin needs Go, but only maintainers run it).

## Layout

| Piece | Path | Role |
|---|---|---|
| runtime | [`runtime/`](runtime/) | the struct codec (`pbw_struct.{h,c}`) |
| plugin | [`plugin/`](plugin/) | `protoc-gen-pbwire-c` (Go): `.proto` -> structs + layout tables |
| examples | [`examples/`](examples/) | addressbook round trip on plain structs |
| tests | [`tests/`](tests/) | byte-parity and failure-path self-checks (`matrix2`, `matrix3`, addressbook) |
| benchmarks | [`benchmarks/`](benchmarks/) | structs vs `protoc --cpp_out` on four tiers |
| conformance | [`conformance/`](conformance/) | how the official suite covers this binding |

## 1. Runtime

[`runtime/pbw_struct.h`](runtime/pbw_struct.h) declares the layout row
(field number -> struct offset) and the codec entry points. Application
structs stay plain data; the codec walks the layout rows and reads/writes
wire bytes directly — no `pbw_message` tree in between, which is what beats
generated code instead of merely keeping up with it (the same design as the
Erlang binding's hand-written `ep_encoder.c` / `ep_decoder.c`):

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

`decode` allocates (strings, arrays, sub-messages) into a zeroed struct.
Repeated fields grow by doubling from the count member, so a message with
empty repeated fields allocates nothing. Unsupported kinds (bytes, groups,
maps, oneofs) fail with `PBW_ERR_INVALID`; extend the codec when a user
needs them.

### Two levels: one-shot calls and a compiled codec

The calls above resolve the schema and re-derive the wire facts on every
invocation. That is fine for a single message, but a hot loop wants the
layout compiled once:

```c
pbw_struct_codec *codec = NULL;
pbw_struct_buf out = {0};              /* reusable output buffer */
pbw_struct_arena *arena = NULL;        /* owns decoded memory */

pbw_struct_codec_init(&codec, schema, "tutorial.Person", person_fields, 4);
pbw_struct_codec_encode(codec, &person, &out);   /* out.data / out.len */
pbw_struct_codec_decode(codec, out.data, out.len, &arena, &got);
...
pbw_struct_arena_free(arena);          /* also invalidates `got` */
pbw_struct_buf_free(&out);
pbw_struct_codec_free(codec);
```

`pbw_struct_codec_init` resolves the message once and precomputes, per field,
an opcode (which value transform applies), the tag bytes, the struct offsets
and the sub-message plan. Encode and decode then dispatch on one small
opcode per field instead of looking facts up every time — that single change
is what closes the gap to `protoc --cpp_out`'s straight-line code.

Two buffers make the loop allocation-free:

- `pbw_struct_buf` is the encode output. It keeps its capacity, so repeated
  encodes reuse the same allocation. `pbw_struct_buf_free` releases it.
- `pbw_struct_arena` owns everything a decode allocates. Each decode resets
  it (so the previous tree becomes invalid), the blocks themselves are
  reused, and `pbw_struct_arena_free` releases them. Because the arena never
  moves a block, the pointers stored in the decoded struct stay valid.

With `arena == NULL` the decoder instead mallocs every string, array and
sub-message, exactly like the one-shot `pbw_struct_decode`; that tree is
released by `pbw_struct_free(fields, nfields, base)`. Never pass an
arena-decoded struct to `pbw_struct_free`: its pointers do not point at
individual allocations. Use the one-shot calls (or a NULL arena) when a
single message should own its memory.

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
[`example/gen/addressbook.h`](examples/gen/addressbook.h) and
[`tests/gen/matrix{2,3}.h`](tests/gen/matrix3.h),
which are also the files the example and the self-check compile against:
the test and the build can never disagree. Each `.proto` has a single
source (`examples/addressbook.proto`, `tests/data/matrix{2,3}.proto`);
`testdata/` stores only the captured `CodeGeneratorRequest` dumps plus the
regen commands, so `make test` at the repo root never needs protoc.

```shell
make test    # from the repo root: also runs bindings/c/tests/test_struct
```

[`tests/test_struct.c`](tests/test_struct.c) is the binding self-check
(no framework, no dependencies): the generated structs encode to the exact
bytes `protoc --encode` produces (pinned `.bin` next to each `.proto`, from
the checked-in `.txt`), decode recovers every field, and layout mismatches,
missing required fields, truncated input and bad arguments all fail loudly.

## 3. Example

[`example/addressbook.c`](examples/addressbook.c) mirrors the shared
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
in [`bindings/cpp/`](../cpp/) mirrors it line for line):

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
before timing. Both sides reuse their output storage: generated code an
`std::string` that keeps its capacity, the struct codec one `pbw_struct_buf`
and one `pbw_struct_arena`.

With the compiled codec the struct binding is ahead of `protoc --cpp_out`
from the 174 B tier up, and ahead on every decode tier; only the 136 B encode
is at parity, where generated straight-line code has so little work per field
that the per-field dispatch is all that is left to pay. The gap grows with
message size because `SerializeToString` walks the message twice (size, then
write) while the struct encoder writes in one pass:

| payload | op | pbwire-c (MB/s, x gen) | protoc gen (MB/s) |
|---|---|---|---|
| 136 B | encode | 1431.7 (0.98x) | 1464.3 |
| 136 B | decode | 1138.7 (1.28x) | 888.0 |
| 174 B | encode | 1466.4 (1.05x) | 1392.8 |
| 174 B | decode | 1056.0 (1.40x) | 755.8 |
| 2469 B | encode | 1083.6 (1.22x) | 884.7 |
| 2469 B | decode | 810.6 (2.10x) | 386.5 |
| 74913 B | encode | 1318.8 (1.47x) | 898.2 |
| 74913 B | decode | 910.3 (2.42x) | 375.5 |

The harness also prints a `pbwire-c1` block: the one-shot API, which compiles
the layout on every call and mallocs every value. It is 3-5x slower, which is
the price of that convenience — the point of the extra block is to keep the
trade-off visible rather than hidden.

## Typed models

The struct binding *is* the typed model: `plugin/` generates the C structs, the
enum typedefs and the layout tables, with the `.proto` type named in each field
comment (`int32_t color; /* optional, structtest.Color3 */`). `tests/gen/` and
`examples/gen/` hold the pinned output; `examples/addressbook.c` assigns struct
literals and calls the generated `*_encode`/`*_decode` wrappers only.

For every other language, `../typed/` generates the same kind of typed layer;
see `../typed/README.md`.

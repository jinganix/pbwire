# C API guide

The public C API, in the order a program needs it. Semantics behind these calls
are in [design.md](design.md); the full contracts are in
[include/pbwire.h](../include/pbwire.h); a runnable project is in
[`examples/`](../examples/).

- [The tutorial in three commands](#the-tutorial-in-three-commands)
- [Build a schema by hand](#build-a-schema-by-hand)
- [Load a schema at runtime](#load-a-schema-at-runtime)
- [Reuse one arena in a loop](#reuse-one-arena-in-a-loop)
- [Ownership rules](#ownership-rules)
- [Stream, bound and sort](#stream-bound-and-sort)
- [Unknown fields](#unknown-fields)
- [JSON and text format](#json-and-text-format)
- [ABI, shared library and versioning](#abi-shared-library-and-versioning)

## The tutorial in three commands

```bash
make examples                     # build the C and C++ address-book examples
./build/example_addressbook       # C structs -> wire bytes -> decode -> compare
```

The example loads `examples/addressbook.desc` (a `FileDescriptorSet`) at
runtime and round-trips a proto2 address book through the generated C structs.
It is the smallest complete program in the repo, and `make test` runs it on
every commit. `examples/README.md` also holds the same address book as JSON
(`addressbook.json`) and as pinned `protoc --encode` bytes (`addressbook.bin`),
which other bindings use as test data.

## Build a schema by hand

```c
#include "pbwire.h"

pbw_schema *schema = pbw_schema_new();
pbw_descriptor *person = pbw_schema_add_message(schema, "Person");
pbw_message_add_field(person, "name", 1, PBW_TYPE_STRING, PBW_LABEL_OPTIONAL);
pbw_message_add_field(person, "id", 2, PBW_TYPE_INT32, PBW_LABEL_OPTIONAL);
pbw_schema_link(schema);

pbw_message *msg = pbw_message_new(person);
pbw_message_set(msg, 1, pbw_value_new_string("abc"));
pbw_message_set(msg, 2, pbw_value_new_int(-1));

uint8_t *buf;
size_t len;
pbw_encode(msg, &buf, &len);   /* 0a 03 61 62 63 10 ff ff ff ff ff ff ff ff ff 01 */

pbw_message *decoded;
pbw_decode(person, buf, len, &decoded);
const char *name = pbw_value_get_string(pbw_message_get(decoded, 1));

pbw_free(buf);
pbw_message_free(decoded);
pbw_message_free(msg);
pbw_schema_free(schema);
```

Hand-built schemas default to proto3; `pbw_schema_set_proto3(schema, 0)` makes
one proto2. `pbw_schema_link()` resolves type names and builds the lookup
tables, so call it once after adding fields, and treat the schema as read-only
afterwards. Field setters (`pbw_field_set_type_name/oneof/packed`) run before
the next `pbw_message_add_field`: each add reallocates the field array, so a
`pbw_field *` held across adds dangles.

## Load a schema at runtime

This is the feature the library exists for: the message types do not have to be
known when the program is compiled. Any process that can read bytes can pick up
a schema later — after a config reload, a tenant connect, or a plugin install —
and immediately read, write and re-emit messages of that type.

```c
uint8_t *set;
size_t set_len;
read_file("schema.pb", &set, &set_len);     /* protoc --include_imports --descriptor_set_out */

pbw_schema *schema;
if (pbw_descriptor_set_load(set, set_len, &schema) != PBW_OK) {
    log("%s", pbw_schema_error(schema));    /* *out is set even on failure */
    pbw_schema_free(schema);
    return;
}
/* names are fully qualified without the leading dot */
const pbw_descriptor *person = pbw_schema_find(schema, "example.Person");
```

- **Three producers, one format.** `protoc --include_imports
  --descriptor_set_out=schema.pb` at build time, `buf build -o schema.pb`
  (imports included), or `pbwire-protoc` at runtime. pbwire only ever reads the
  resulting bytes.
- **File or embedded, same bytes.** The loader takes bytes, not a path: read a
  sidecar file at startup for dynamic updates, or bake the set into the binary
  (`xxd -i schema.pb`, Rust `include_bytes!`, app resources) for a single
  artifact. Every binding exposes both forms (`from_file` / `from_bytes`,
  `fromFile` / `fromBytes`, `load(bytes)`); the trade-off is in the
  [FAQ](faq.md#do-i-have-to-recompile-when-a-proto-changes).
- **Ship every imported file.** Types are resolved by name inside the set, so a
  field typed `other.Thing` needs `other.Thing` in the same set: pass
  `--include_imports` (`buf build` already does) or the load fails with
  `unknown type ... for field ...` in `pbw_schema_error()`.
- **What the loader reads:** names, field numbers, types, labels, oneofs, the
  `packed` option, proto2 defaults and the file syntax (proto2, proto3, or an
  edition), plus extensions with their extendee and services with their
  methods (`pbw_schema_service_*`, query-only; the core does no RPC).
  `pbw_service_resolve()` maps a method name to its input/output descriptors
  so a transport can decode/encode; the socket bytes stay official gRPC/Twirp.
  Well-known-type helpers (`pbw_any_pack`, `pbw_any_type_url/payload`,
  `pbw_timestamp/duration_to_millis`) cover the Any/Timestamp/Duration shapes.
  Reserved ranges and source code info are ignored.
- **Dialect defaults are applied:** proto3 and edition-2023 sets get packed
  repetition by default, proto2 sets keep explicit `packed`. Editions files
  additionally resolve their FeatureSet per file, message and field
  (packed/required/closed-enum/delimited overrides). Presence is
  explicit either way — nothing is defaulted in.
- **Failure is not fatal and not silent:** `*out` is always written, so free it
  on both paths; the reason lands in `pbw_schema_error()`.
- **The schema outlives its messages.** Messages, values and context trees keep
  pointers into the schema's descriptors; free the schema last, and treat a
  linked schema as read-only while other threads use it.

`pbwire-protoc` turns `.proto` text into exactly those bytes at runtime
(`#include "pbwire-protoc/importer.h"`, C++17 + libprotobuf):

```cpp
pbwp::Options options;
options.import_paths = {"./proto", "./third_party"};

pbwp::Result result = pbwp::Importer(options).import({"user.proto", "order.proto"});
if (result.ok()) {
    result.value().save("schema.pb");            /* or hand data()/size() to pbw_descriptor_set_load */
} else {
    for (const pbwp::Diagnostic &d : result.diagnostics()) log("%s", d.ToString().c_str());
}
```

## Reuse one arena in a loop

Hand the library a workspace instead of relying on per-message ownership:

```c
pbw_ctx *ctx = pbw_ctx_new();          /* one per thread / goroutine */

for (;;) {
    pbw_message *msg;
    const uint8_t *out;
    size_t len;

    pbw_decode_ctx(ctx, person, in, in_len, &msg);   /* no allocation per field */
    /* ... read msg ... */
    pbw_encode_ctx(ctx, msg, &out, &len);            /* buffer reused */
    send(out, len);
    pbw_ctx_reset(ctx);                              /* everything freed at once */
}
pbw_ctx_free(ctx);
```

Messages decoded through a context belong to it: they are read-only,
`pbw_message_free` on them is a no-op, and `pbw_ctx_reset` invalidates them. The
plain functions share all the code underneath and give every decoded tree its own
arena, so a decode is still one `malloc`; `pbw_message_free` releases the whole
tree and everything reachable from it.

A context keeps at most `pbw_ctx_set_retain_cap` bytes resident across resets
(1 MiB by default; single allocations above it bypass the reusable chunk and are
freed on reset, so one outlier never pins the context). `0` keeps nothing. Pick
a small cap for many-goroutine hosts, a larger one for a few scheduler threads.

`pbw_pool` is a bounded freelist of contexts for short-lived workers:
`pbw_pool_acquire` instead of `pbw_ctx_new`, `pbw_pool_release` instead of reset
+ free. One pool per worker thread (it holds no lock); `pbw_pool_idle` reports
the idle count for your own logging. `pbw_ctx_set_user`/`pbw_ctx_user` carry one
opaque binding pointer per context (untouched by resets and pooling).

Which API to use per call site is a table in
[design.md](design.md#two-allocation-strategies).

## Ownership rules

Five rules cover every pointer the API hands out or takes:

1. **`pbw_message_set()` takes ownership on success only.** If it returns an
error, the value is still the caller's to free. Values already owned by another
message are rejected (`PBW_ERR_INVALID`): build a fresh value instead of moving
one across messages.
2. **A decoded plain tree owns everything under it.** `pbw_decode()` allocates
one arena for the whole tree, so `pbw_message_free()` releases the tree, its
values, submessages and payloads at once; `pbw_value_free()` on a value taken
from a decoded tree is a no-op on purpose (the `PBW_VF_ARENA` flag).
3. **Context messages belong to the context.** `pbw_decode_ctx` trees are
read-only and die with the next `pbw_ctx_reset()`/`pbw_ctx_free()`, never with
`pbw_message_free` (that is a no-op there too). A tree built with
`pbw_message_new_ctx()` is mutable until a decode seals it, but it only takes
values built in that same arena (`pbw_value_new_*_ctx`): a heap value
(`pbw_value_new_string()`, ...) is rejected with `PBW_ERR_INVALID`, because the
reset could not free it.
4. **The schema outlives every message built from it.** Messages, values and
context trees hold pointers into the schema's descriptors; free the schema last
and treat a linked schema as read-only while other threads use it.
5. **Encoder output is a plain `malloc`.** `pbw_encode()`, `pbw_json_print()`,
`pbw_text_print()` and `pbw_text_print_opts()` hand you a buffer owned by the
caller (`pbw_free`); `pbw_encode_ctx()` output is borrowed from the context and
valid until the next encode/reset/free on it.

The same rules with the reasoning are in [design.md](design.md#two-allocation-strategies)
and the hard invariants in [AGENTS.md](../AGENTS.md).

## Stream, bound and sort

Length-delimited framing (`varint length + message`) is the standard way to put
messages on a stream. Feed bytes as they arrive; each `next` call decodes one
message, or reports nothing yet:

```c
pbw_istream *s = pbw_istream_new(NULL);   /* optional pbw_limits */

pbw_istream_feed(s, chunk, chunk_len);     /* append, any split */
for (;;) {
    pbw_message *msg = NULL;
    pbw_istream_next(s, person, &msg);     /* NULL + PBW_OK = need more data */
    if (msg == NULL)
        break;
    /* ... read msg (owned, pbw_message_free) ... */
    pbw_message_free(msg);
}
```

`pbw_encode_delimited` writes one frame; `pbw_encode_delimited_to` writes the
same frame into your own buffer (no allocation, `size + prefix +
PBW_ENCODE_SLACK` is the guaranteed cap). A complete frame already in hand
takes one `pbw_decode_delimited` per message, `*consumed` being the prefix plus
the frame, so concatenated frames advance through the buffer; add `_ctx`
(`pbw_decode_delimited_ctx`, `pbw_decode_delimited_ctx_limits`) to decode into
a workspace arena instead of one `malloc` per frame. `pbw_encode_sink(msg, sink, opaque)`
hands the bytes to a caller callback in 8 KiB chunks instead of a `malloc`'d
buffer. Untrusted input takes explicit bounds (`0` means the default):

```c
pbw_limits limits = {.max_bytes = 1 << 20, .max_depth = 32};

pbw_decode_limits(person, buf, len, &limits, &msg);      /* PBW_ERR_LIMIT past them */
pbw_decode_ctx_limits(ctx, person, buf, len, &limits, &msg);
```

`pbw_encode_deterministic` sorts map entries by key so equal messages encode to
equal bytes; everything else encodes exactly as `pbw_encode` writes it.

## Scan without a tree

When a look is enough — counting fields, filtering, pre-checking a large
payload — `pbw_visit` walks one message level and calls you back per field,
with no message tree and no allocation. Scalars arrive as raw wire bits (no
zigzag or sign fold), nested messages and packed runs as `LEN` payloads to
recurse into, groups as raw bytes. `field` is `NULL` for unknown numbers; a
known field carrying another wire type still resolves, so the callback can
tell a mismatch from an unknown. `LEN` data points into `buf` (copy it if it
must outlive the buffer); `fail_off` reports the failing field's byte offset:

```c
static pbw_status count_inner(void *ud, const pbw_field *field, uint32_t number,
                              uint32_t wire_type, const pbw_visit_value *v)
{
    (*(size_t *)ud)++;
    return PBW_OK;
}

static pbw_status count_outer(void *ud, const pbw_field *field, uint32_t number,
                              uint32_t wire_type, const pbw_visit_value *v)
{
    if (number == 1 && wire_type == 2)   /* a nested message: recurse */
        return pbw_visit(field->sub, v->data, v->len, count_inner, ud, NULL);
    (*(size_t *)ud)++;
    return PBW_OK;                       /* anything else aborts the scan */
}

size_t n = 0, off = 0;

pbw_visit(person, buf, len, count_outer, &n, &off);   /* off == len on success */
```

`pbw_visit_limits` takes the same `pbw_limits` as the decoders (only
`max_bytes` and the group-nesting cap apply: the scan itself never recurses).
No required/enum checks run — values are raw wire bits. Reach for `pbw_decode`
when the tree must be kept or mutated; reach for `visit` when a single pass
answers the question (about 2.8x a decode+free on a repeated-submessage
payload).

## Unknown fields

Fields the schema does not declare — and known fields arriving with the wrong
wire type, and legacy groups — are kept as raw wire bytes and re-emitted after
the known fields in input order. That is what lets a gateway forward messages
for a schema it only partly knows:

```c
size_t n = pbw_message_unknown_len(msg);
const uint8_t *raw = pbw_message_unknown_bytes(msg);  /* tag + payload per item,
                                                         owned by the message */
pbw_message_add_unknown_bytes(msg, raw, n);  /* on a plain, mutable message */
```

Messages from `pbw_decode_ctx` are read-only, so `add_unknown_bytes` fails on
them. The cost is one buffer per message that actually contains unknowns; a
message decoded with none pays nothing.

## JSON and text format

Both directions are in the core, no dependency and no generated code. *out is
malloc'd, NUL-terminated and owned by the caller (`pbw_free`), *len excludes
the NUL:

```c
/* JSON mapping: well-known types, extensions as [full.name], quoted 64-bit
 * integers, standard base64, enums by name, no unknown fields. */
pbw_status pbw_json_print(const pbw_message *msg, char **out, size_t *len);
/* pbw_json_parse_opts takes pbw_json_parse_options{ignore_unknown_fields}. */
pbw_status pbw_json_parse(const pbw_descriptor *desc, const char *text,
                          size_t len, pbw_message **out);

/* text format: reference-identical printer (opts: show/hide unknown fields),
 * extensions, groups, Any. Reserved names are skipped by the parser, anything
 * else unknown fails. */
pbw_status pbw_text_print(const pbw_message *msg, char **out, size_t *len);
pbw_status pbw_text_print_opts(const pbw_message *msg, int print_unknown_fields,
                               char **out, size_t *len);
pbw_status pbw_text_parse(const pbw_descriptor *desc, const char *text,
                          size_t len, pbw_message **out);
```

Both parsers reject a missing proto2 `required` field with `PBW_ERR_REQUIRED`,
like the binary decoder.

Both printers emit the reference implementation's exact spelling, and both
parsers return the reference's verdict on the official conformance inputs — see
[testing.md](testing.md) for what pins that.

Limits apply to the parsers too (the JSON and text parsers share the binary
decoder's `PBW_MAX_DEPTH` nesting cap and its bounds checks), and their verdicts
are fuzzed against the reference parser — 40,000 JSON and 60,000 text mutants in
`make difftest` must be accepted or rejected exactly like libprotobuf. See
[testing.md](testing.md).

## ABI, shared library and versioning

The C ABI is the product, so it is versioned explicitly:

- `PBW_ABI_VERSION` (currently `1`) is in the header; `pbw_abi_version()` returns
the same number at runtime, so a binding that `dlopen`s the library can refuse a
mismatch instead of guessing. The Python extension, the Node addon,
Node, Ruby and PHP compare the two at load time.
- `make shared` produces `libpbwire.so.1` / `libpbwire.1.dylib`; the CMake
`SOVERSION` carries the same number, so the SONAME and the macro always agree.
- Additive API changes keep the number; a breaking change bumps both it and the
SONAME together.

A C++ consumer includes the same `pbwire.h` and links `pbwire::pbwire`; the
header is C99 and compiles as C++ unchanged.

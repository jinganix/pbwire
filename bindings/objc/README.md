# pbwire Objective-C binding

A thin ARC wrapper over the C ABI: a `FileDescriptorSet`
(`protoc --descriptor_set_out`) becomes a schema at runtime, and messages are
`PBWMessage` objects with field access by name. No code generation, no
protoc plugin, Foundation only.

```objc
NSError *error = nil;
PBWSchema *schema = [PBWSchema schemaFromFile:@"tests/data/probe.desc" error:&error];
PBWMessage *message = [schema messageByName:@"pbwobjc.Probe"];

[message setValue:@(1234) forField:@"i32" error:&error];
[message setValue:@"alice" forField:@"text" error:&error];
[[message addMessageForField:@"inner" error:&error] setValue:@"child" forField:@"label" error:&error];

NSData *wire = [message encode:&error];          // proto3 bytes
PBWMessage *decoded = [message decode:wire error:&error];

assert([[decoded valueForField:@"text"] isEqual:@"alice"]);
assert([[[decoded messageForField:@"inner"] valueForField:@"label"] isEqual:@"child"]);
assert([[decoded encode:&error] isEqualToData:wire]);   // byte-exact re-encode
```

## Build and run

From the repo root (macOS, Apple clang; `build/libpbwire.a` comes from `make all`,
which `build.sh` runs itself when it is missing):

```sh
make all                        # or: bindings/objc/build.sh does it for you
bindings/objc/build.sh          # self-check + example
bindings/objc/build.sh test     # self-check only
bindings/objc/build.sh example  # example only
```

`build.sh` compiles `PBWSchema.m`, `PBWMessage.m` and the driver of your choice
with `-fobjc-arc -I<repo>/include`, links `<repo>/build/libpbwire.a` with
`-framework Foundation -lpthread`, and writes the binaries to
`bindings/objc/build/` (git-ignored). `CC=... build.sh` picks another compiler.
The self-check prints `test_objc: all checks passed` and exits 0; it takes an
optional descriptor-set path as its first argument.

## API

### `PBWSchema`

| member | meaning |
|---|---|
| `+ schemaFromFile:error:` | load a serialized `FileDescriptorSet`; Foundation handles the file read (`NSCocoaErrorDomain`), the schema error is `PBWErrorDomain` |
| `+ schemaFromData:error:` | same from bytes, e.g. embedded in the app bundle |
| `- messageByName:` | an empty `PBWMessage` of that fully qualified type (`"pkg.Outer.Inner"`), or `nil` for an unknown name or an enum |

Use `protoc --include_imports` so every referenced type is in the set, and hold
nothing else: a message retains its schema, so keeping a message alive keeps the
descriptors valid.

### `PBWMessage`

| member | meaning |
|---|---|
| `messageName` | the message's full type name |
| `hasField:` | YES when the field has a value (set here or on the wire) |
| `valueForField:` | `NSNumber` (numeric, bool, enum), `NSString`, `NSData`, or a nested `PBWMessage`; `nil` when absent, unknown or repeated |
| `setValue:forField:error:` | store a singular field; `nil` clears it |
| `messageForField:` | the nested message of a singular message field, or `nil` |
| `addMessageForField:error:` | get-or-create the nested message and fill it in place |
| `encode:` | wire bytes |
| `decode:error:` | decode bytes as a message of the same type |

Value mapping (the field's schema type decides the wire type, so an `NSNumber`
is written as whatever the `.proto` declared):

| protobuf | Objective-C |
|---|---|
| int32/int64/sint\*/sfixed\*/fixed\*/uint\* | `NSNumber` (`longLongValue` / `unsignedLongLongValue`) |
| bool | `NSNumber` (`boolValue`) |
| float/double | `NSNumber` (`floatValue` / `doubleValue`) |
| string | `NSString`, written as UTF-8 (embedded NULs survive) |
| bytes | `NSData` |
| enum | `NSNumber` (the enum number, not the name) |
| message | `PBWMessage` |

Errors never cross the C
boundary as exceptions: every `pbw_status` becomes an `NSError` in
`PBWErrorDomain` whose `code` *is* the `pbw_status`
(`PBW_ERR_INVALID`, `PBW_ERR_NOT_FOUND`, `PBW_ERR_TYPE`, `PBW_ERR_NOMEM`, ...)
and whose `localizedDescription` is `pbw_strerror()` plus the schema reason for
a bad descriptor set.

### Ownership

A message from `messageByName:` or `decode:` owns its C tree and frees it in
`-dealloc` (`pbw_message_free`, which releases the whole decoded tree, so a
double release cannot happen: a nested message is a view that frees nothing).
A nested message view retains its parent, so it cannot outlive the storage it
points into, and editing a view edits the parent's tree. ARC is required.

## Files

| Piece | Path | Role |
|---|---|---|
| runtime | `PBWSchema.h/.m`, `PBWMessage.h/.m`, `PBWError.h` | the wrapper classes and the `NSError` helper |
| example | [`examples/roundtrip.m`](examples/roundtrip.m) | build, encode, decode, assert, print |
| tests | [`tests/test_objc.m`](tests/test_objc.m) | assert-based self-check: descriptor load, full round trip, byte-exact re-encode, absent and zero fields, nested messages, schema/API errors, lifetime loop |
| benchmark | [`benchmarks/bench_objc.m`](benchmarks/bench_objc.m) | the four standard tiers against the protobuf ObjC runtime |
| fixture | [`tests/data/probe.proto`](tests/data/probe.proto) + committed `probe.desc` | regenerate with `protoc -Ibindings/objc/tests/data --descriptor_set_out=bindings/objc/tests/data/probe.desc bindings/objc/tests/data/probe.proto` |

## Benchmark

`benchmarks/bench.sh` generates `Matrix3.pbobjc.{h,m}` with `--objc_out`, builds
the official `objectivec/GPBProtocolBuffers.m` runtime from the protobuf source
checkout (`make conformance-setup` caches it; `PBWIRE_PROTOBUF_SRC` overrides),
and runs the four standard tiers from [`BENCHMARKS.md`](../../BENCHMARKS.md):

```sh
./benchmarks/bench.sh      # or: make objc-bench
```

The wrapper adds one Objective-C object per call and no copy: `encode` hands
the malloc'd buffer the C encoder already filled to `NSData`
(`dataWithBytesNoCopy:length:freeWhenDone:`), and `decode` wraps the arena the C
decoder produced in one `PBWMessage`. `pbwire-objc` therefore beats the
generated runtime on every tier and both ops: 5.9x / 2.4x at 136 B, 6.4x / 2.8x
at 174 B, 4.7x / 2.7x at 2469 B and 1.9x / 2.7x at 74913 B (encode / decode). The
xlarge tier is one 74 KB string, i.e. a memcpy race: the generated encoder writes
it into an `NSMutableData` of the final size, and the wrapper writes it into its
own buffer, which `NSData` then takes over; neither side copies the payload
twice.

## Deliberately skipped

- **No protoc plugin or generated code.** Compiled languages bake a schema in at
  build time, but this binding follows the dynamic path the C ABI and the Python
  binding already take: a `FileDescriptorSet` is the runtime schema, so there is
  no second schema source to keep in sync and no build step to run.
- **No conformance testee.** The official suite drives the C core through
  `conformance/testee.c`; an Objective-C testee would test the same core code and
  the wrapper adds no wire behavior of its own.
- **No repeated/map field helpers.** `valueForField:` returns `nil` and
  `setValue:forField:` fails with `PBW_ERR_INVALID` for a repeated field; the C
  ABI's `pbw_value_at()` list walk is what a caller needs when it wants the whole
  list, and no caller here asked for it yet.
- **No JSON/text format or context/pool APIs** on the wrapper; call
  `pbw_json_print` / `pbw_decode_ctx` from Objective-C if you need them.

Known limits:

- A string field whose bytes are not valid UTF-8 reads as `nil` (the C payload
  is kept, so re-encoding is still byte-exact).
- A 32-bit numeric field set out of range fails with `PBW_ERR_TYPE` instead of
  being truncated.
- `float` values are stored as 32-bit floats, `double` as 64-bit; nothing is
  widened silently.

## Typed models

`protoc-gen-pbwire-typed` (in `../typed`) generates `NSObject` subclasses with
typed properties (including `NSArray` for repeated fields), enum constants and
`-encodeWithSchema:error:` / `+decodeWithSchema:data:error:`. The committed
output is `tests/gen/` and `examples/gen/`; `examples/roundtrip.m` assigns typed
properties only. See `../typed/README.md`.

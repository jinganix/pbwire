# pbwire Swift binding

A SwiftPM package over the C ABI: a `FileDescriptorSet`
(`protoc --descriptor_set_out`) becomes a schema at runtime, and messages are
`Message` objects with field access by name. No code generation, no protoc
plugin, no wire code — the format stays in the C core, like the Objective-C
and Dart bindings.

```swift
import Pbwire

let schema = try Schema(contentsOfFile: "tests/data/probe.desc")
let message = schema.message("pbwswift.Probe")!

try message.set(.int(1234), "i32")
try message.set(.string("alice"), "text")
try message.makeNested("inner").set(.string("child"), "label")

let wire = try message.encode() // proto3 bytes
let decoded = try message.decode(wire)

assert(decoded.get("text") == .string("alice"))
assert(decoded.nested("inner")?.get("label") == .string("child"))
assert(try decoded.encode() == wire) // byte-exact re-encode
```

## Build and run

From `bindings/swift` (macOS, Swift 6.x; `build/libpbwire.a` comes from
`make` at the repo root, which `build.sh` runs itself when it is missing):

```sh
./build.sh          # self-check + example
./build.sh test     # self-check only
./build.sh example  # example only
```

or plain SwiftPM (no dependencies to fetch, works offline):

```sh
swift test            # self-check, 8 tests
swift run Roundtrip   # example
swift run -c release Bench   # the four standard tiers, pbwire-swift only
```

`Package.swift` links `../../build/libpbwire.a` with a relative path, so the
default commands work with no environment setup. A package with unsafe linker
flags cannot itself be a dependency; that is fine, this binding is an
end-user FFI host. The `ld` warning that the archive "was built for newer
'macOS' version" is benign (static link, no runtime dependency).

## API

### `Schema`

| member | meaning |
|---|---|
| `init(contentsOfFile:)` | load a serialized `FileDescriptorSet`; throws `PbwireError(-1)` when the file cannot be read, a `pbw_status` error (with the `pbw_schema_error()` reason) for a bad set |
| `init(bytes:)` | same from bytes, e.g. embedded in the app bundle |
| `message(_:)` | an empty `Message` of that fully qualified type (`"pkg.Outer.Inner"`), or `nil` for an unknown name or an enum |

Use `protoc --include_imports` so every referenced type is in the set. A
message retains its schema, so keeping a message alive keeps the descriptors
valid.

### `Message`

| member | meaning |
|---|---|
| `typeName` | the message's full type name |
| `has(_:)` | true when the field has a value (set here or on the wire) |
| `get(_:)` | present value of a singular field, or nil when absent, unknown or repeated |
| `set(_:_:)` | store a singular field, replacing the previous one; nil clears it |
| `add(_:_:)` | append one item to a repeated field (or set a singular one) |
| `count(_:)` / `at(_:_:)` | items in a repeated field / one item by index |
| `nested(_:)` | the nested message of a singular message field, or nil |
| `makeNested(_:)` | get-or-create the nested message, filling it in place |
| `encode()` | wire bytes |
| `decode(_:)` | decode bytes as a message of the same type |

Value mapping (the field's schema type decides the wire type):

| protobuf | Swift |
|---|---|
| int32/int64/sint\*/sfixed\*/enum | `.int(Int64)` |
| uint\*/fixed\* | `.uint(UInt64)` |
| bool | `.bool(Bool)` |
| float/double | `.float(Float)` / `.double(Double)` |
| string | `.string(String)`, written as UTF-8 |
| bytes | `.bytes(Data)` |
| message/group | `.message(Message)` |

Errors never cross the C boundary as anything but values: every failure
throws `PbwireError` whose `status` is the `pbw_status` of the failing call
(`3` not-found, `4` type mismatch, ...) and whose `message` is
`pbw_strerror()` plus the field name.

### Ownership

A message from `message(_:)` or `decode(_:)` owns its C tree and frees it on
release (`pbw_message_free`, which releases the whole decoded tree, so a
double release cannot happen: a nested message is a view that frees nothing).
A nested message view retains its parent, so it cannot outlive the storage it
points into, and editing a view edits the parent's tree. One tree belongs to
one thread, like the C core.

## Files

| Piece | Path | Role |
|---|---|---|
| runtime | `Sources/CPbwire/` (C header re-export), `Sources/Pbwire/Pbwire.swift` | the `CPbwire` module shim and the `Schema`/`Message` wrapper |
| example | `Sources/Roundtrip/main.swift` | build, encode, decode, assert, print |
| tests | `Tests/PbwireTests/PbwireTests.swift` | XCTest self-check: descriptor load, full round trip, pinned bytes, presence, repeated fields, errors, lifetime loop |
| benchmark | `Sources/Bench/main.swift`, `benchmarks/baseline/` | the four standard tiers vs apple/swift-protobuf (`benchmarks/bench.sh`) |
| fixture | `tests/data/probe.proto` + committed `probe.desc`, `tests/data/matrix3.desc` | regenerate with `protoc -Itests/data --descriptor_set_out=tests/data/probe.desc tests/data/probe.proto` |
| conformance | `conformance/README.md` | how the official suite covers this binding (via the C testee) |

## Benchmark

`benchmarks/bench.sh` runs the four standard tiers from
[`BENCHMARKS.md`](../../BENCHMARKS.md), then the side-by-side comparison in
`benchmarks/baseline/` (a second package: this binding by path plus
apple/swift-protobuf with the matrix3 model from its own build plugin, so no
generated code is checked in and no protoc plugin is installed — needs
network once and `PROTOC_PATH`, which `bench.sh` exports):

```sh
./benchmarks/bench.sh          # tiers + baseline
./benchmarks/bench.sh tiers    # tiers only, no network
```

Both sides must produce identical bytes before anything is timed, and both
allocate per call. Copied from the run on Apple M4 Pro / Swift 6.4.2 /
swift-protobuf 1.39.0 (per-run noise is a few percent); higher is better,
MB/s over wire bytes:

| payload | op | pbwire-swift (MB/s, x swift-protobuf) | swift-protobuf (MB/s) |
|---|---|---|---|
| 136 B | encode | 974.5 (2.3x) | 430.5 |
| 136 B | decode | 788.7 (3.3x) | 241.9 |
| 174 B | encode | 1051.0 (3.3x) | 316.5 |
| 174 B | decode | 717.3 (3.7x) | 195.8 |
| 2469 B | encode | 1044.9 (4.0x) | 264.5 |
| 2469 B | decode | 622.1 (3.2x) | 191.9 |
| 74913 B | encode | 1272.7 (4.6x) | 277.5 |
| 74913 B | decode | 537.9 (2.5x) | 217.1 |

The wrapper adds one Swift object per call and no copy: `encode` hands the
malloc'd buffer the C encoder already filled to `Data` with a free
deallocator, and `decode` wraps the decoder's arena in one `Message`.

## Deliberately skipped

- **No protoc plugin or generated code.** Swift is a compiled language, but
  this binding follows the dynamic path the C ABI and the Objective-C binding
  already take: a `FileDescriptorSet` is the runtime schema, so there is no
  second schema source to keep in sync and no build step to run.
- **No conformance testee.** The official suite drives the C core through
  `conformance/testee.c`; a Swift testee would test the same core code, and
  the wrapper adds no wire behavior of its own. See `conformance/README.md`.
- **No JSON/text format wrappers.** The C ABI's `pbw_json_*` / `pbw_text_*`
  entry points are one import away (`import CPbwire`); no caller here asked
  for them yet.
- **No generated model.** `protoc-gen-pbwire` (in `../../codegen`) emits no
  Swift output today; the dynamic API above is the whole surface.

Known limits (same as the Objective-C wrapper):

- A string field whose bytes are not valid UTF-8 reads as nil (the C payload
  is kept, so re-encoding is still byte-exact).
- A 32-bit numeric field set out of range fails with a type error instead of
  being truncated.
- `float` values are stored as 32-bit floats, `double` as 64-bit; assigning a
  `.double` to a `float` field (or `.int`/`.uint` to either) converts the way
  a narrowing Swift initializer does.

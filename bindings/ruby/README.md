# pbwire Ruby binding

`lib/pbwire.rb` loads a compiled C extension (`ext/pbwire_ext/`) that links the
pbwire C core statically. No gems at run time.

Most code never touches the dynamic surface: generate a gem-shaped facade
once (`make -C codegen gen-ruby`) and use it like the library it
mirrors — same `new`/`encode`/`decode`/`to_h` calls, same observable
behavior, but the wire format stays in the C core:

```ruby
require 'addressbook'   # generated; embeds its own FileDescriptorSet

person = Pbwire::Gen::Tutorial_Person.new(name: 'alice', id: 7)
Pbwire::Gen::Tutorial_Person.encode(person)  # => binary String, no schema
back = Pbwire::Gen::Tutorial_Person.decode(wire)
back.name                                     # => "alice"
back.to_h                                     # => {name: "alice", id: 7, ...}
back.has_email?                               # => false
```

Presence follows the value: a non-nil assignment marks the field, so
`new(opt_i32: 0)` is written while `new` leaves it absent; oneof keeps
only its last member. `to_h` uses proto-name symbols and renders enums
as name symbols, exactly like the gem. The one deliberate difference:
validation happens at the encode boundary (plain Ruby objects until
then), where a mistyped field raises `TypeError`.

The dynamic API is still there for gateways and schemas known only at
runtime:

```ruby
require 'pbwire'

schema = Pbwire::Schema.from_file("sample.desc")
sample = schema.message("pbwire.ruby.Sample")   # a builder

sample["i32"] = -7
sample["name"] = "alice"
sample["nums"] = [1, 2, 3]                      # repeated: assign the whole Array
inner = schema.message("pbwire.ruby.Inner")
inner["s"] = "nested"
sample["inner"] = inner                         # hands the tree to the parent

wire = sample.encode                            # binary String
back = schema.message("pbwire.ruby.Sample").decode(wire)

back["name"]                      # => "alice"
back["nums"]                      # => [1, 2, 3]
back["inner"]["s"]                # => "nested"
back.encode == wire               # => true: byte-exact re-encode
```

`Pbwire::Type` is not part of the Ruby API: it is the C-side holder for one
message's field table (memoized per schema and kept alive by the messages that
reference it). Reach a type through the schema — `schema.message("pkg.Msg")`
returns a builder of that type, and `Message#type` returns its full name.

## Build and run

The extension is compiled once per interpreter with `sh build.sh` (or
`make ruby-bind`), which uses that Ruby's own headers and links `build/libpbwire.a`
into `lib/pbwire_ext.<DLEXT>`. Nothing else is needed at run time -- no shared
library path, no gem:

```shell
make ruby-bind                     # compiles lib/pbwire_ext.bundle for $(RUBY)
make ruby-test                     # builds, then runs test/test_pbwire.rb
ruby -Ibindings/ruby/lib bindings/ruby/examples/roundtrip.rb
```

`RUBY=` selects the interpreter, both for the build and the benchmark:

```shell
RUBY=/opt/homebrew/bin/ruby make ruby-test
```

The test prints one `ok`/`FAIL` line per check and exits non-zero on a failure;
`test/data/sample.desc` is checked in, so no `protoc` is needed at test time.

## Two message representations

Both are `Pbwire::Message`; the class of the object tells you which one you have.

- **A builder** comes from `schema.message(name)`. It is mutable: `set`/`[]=` store
  into a C message tree, `get`/`[]` read it back, and `encode` walks it with the C
  core, writing straight into the returned String (one pass, no intermediate
  buffer).
- **A wire view** comes from `msg.decode(bytes)`. It is a read-only view over the
  bytes: one validating tag scan records where each wire field lives, `get`
  materializes only the field asked for, and `encode` hands the exact bytes back,
  so unknown fields, field order and non-canonical encodings survive a
  decode/encode round trip. `set` on a wire view raises: build a message and
  encode it to change data.

That split is why the binding is fast: decoding 3000 repeated items costs one
scan rather than 3000 message trees, and encoding a builder is one pass into the
caller's string. Field facts (number, kind, wire type, packed/required/oneof,
sub type, name lookup) are resolved once per message type and cached per schema.

## API

| member | meaning |
|---|---|
| `Pbwire::Schema.from_file(path)` / `.from_bytes(string)` | load a `FileDescriptorSet`; raises `Pbwire::Error` with the core's `pbw_schema_error()` text when the set is rejected |
| `Pbwire::Schema.from_file(path) { \|schema\| ... }` (same for `.from_bytes`) | block form: the schema closes itself after the block, even on exception, like `File.open`; without a block the caller owns it and closes explicitly |
| `schema.message("pkg.Name")` | a new builder message; raises `Pbwire::Error` for an unknown name |
| `msg.set(name_or_number_symbol, value)` / `msg[...] = value` | set a field; returns the message |
| `msg.get(name_or_number_symbol)` / `msg[...]` | field value; `nil` when a singular field is absent, `[]` for an empty repeated field |
| `msg.type` | full message name |
| `msg.encode` | wire bytes as a binary `String` |
| `msg.decode(bytes)` | a wire view of the bytes; unknown fields and field order survive, so `decode(wire).encode == wire` |
| `msg.close` / `schema.close` | free the C object now; idempotent, also `release`. A schema is freed once its last message is gone |
| `msg.closed?` / `schema.closed?` | whether it was released |

Every `pbw_status` is checked and raised as `Pbwire::Error`; a wrong Ruby type
for a value raises `ArgumentError`/`TypeError`. Nothing aborts or segfaults on a
bad call. Malformed wire data is rejected by `decode` (every tag, length and
packed run is checked, nesting is capped, and proto2 required fields are
enforced).

### Value mapping

| protobuf | Ruby |
|---|---|
| int32/int64/sint*/sfixed*/enum | Integer (signed 64-bit range) |
| uint32/uint64/fixed* | Integer (unsigned 64-bit range) |
| bool | `true` / `false` (also accepts `nil` as `false`) |
| float/double | Float (Integers are accepted) |
| string | String, UTF-8 tagged (undecodable bytes stay as-is and are not lost) |
| bytes | String, `Encoding::BINARY` |
| message | `Pbwire::Message`, set by handing a builder over |
| repeated field | Array; assign a new Array to replace it, read a new Array back |

A field that was not set or not on the wire reads as `nil`, and a field set to
`false`/`0` is present and round trips as such (explicit presence, like the C
API). Assigning a repeated field replaces its contents (`[]` clears it); reading
one returns a copy, so `msg["nums"] << 1` does not modify the message.

A builder handed to a message field moves into that message and is closed
afterwards (`closed?` is true). The same builder cannot be handed over twice, and
a message cannot contain itself.

### Memory

A `Schema` owns one C schema and a message owns its C tree (a builder) or its
slice index (a wire view); an abandoned object is released by a GC finalizer,
and `#close` releases it deterministically. Closing twice cannot double free.

`pbw_message_free()` reads the descriptor, so the schema's C memory has to
outlive its last message -- and Ruby promises no finalizer order. A refcounted
holder therefore reference-counts the schema's users: whichever side is released
last frees the schema, and `schema.close` with messages still alive frees it when
the last one goes instead of leaving the messages dangling.

A wire view keeps a frozen shared copy of the bytes it was decoded from (Ruby's
copy-on-write makes this free for large strings), so later writes to the input
String do not change the message. A borrowed sub-message returned by `get` is
owned by its parent: it keeps the parent alive and refuses to be used after the
parent is closed.

### Contexts and pools

`Pbwire::Context` is the C `pbw_ctx` workspace (an arena plus a reusable encode
buffer), and `Pbwire::Pool` is a bounded freelist of them, for a server that
decodes in a tight loop:

```ruby
ctx = Pbwire::Context.new
ctx.decode(schema, "pkg.Msg", wire)   # read-only view, dies with the next reset
ctx.encode(msg)                       # into the context's buffer
ctx.used                              # bytes held now
ctx.retain_cap = 64 * 1024            # resident ceiling (1 MiB default)
ctx.reset                             # frees everything since the last reset

pool = Pbwire::Pool.new(max_idle: 8, retain_cap: 64 * 1024)
ctx = pool.acquire
# ... use ctx ...
pool.release(ctx)                     # already reset for the next acquire
```

A message decoded through a context borrows it: after `reset` (or a pool
`release`) it raises `"context was reset"` instead of reading freed arena
memory.

## Repeated fields, maps, groups

Repeated scalars (packed or unpacked on the wire), repeated strings/bytes and
repeated messages are supported; a map field reads as an Array of entry
messages. Groups (proto2 legacy) map like messages (START_GROUP framing on
the wire). Extensions are read by number, not name.

## Deliberately skipped

- **No `protoc` plugin, no codegen.** The descriptor set is the schema; nothing
  is generated and there is no hand-built schema path.
- **Formats beyond binary/JSON/text.** `to_json`/`from_json` (with an
  optional ignore-unknown flag), `to_text`/`from_text`, deterministic
  encoding (`encode_deterministic`, map entries sorted by key) and delimited
  framing (`encode_delimited`, `decode_delimited` returning `[message, read]`)
  wrap the C API; `to_builder` materializes a decoded view into a canonical
  tree. Unknown fields round-trip byte for byte in every form.

## Benchmark

`benchmarks/bench.sh` runs the four standard tiers from
[`BENCHMARKS.md`](../../BENCHMARKS.md) against the `google-protobuf` gem on the
same `matrix3.proto` (small / medium / large / xlarge scale the repeated fields
x0 / x1 / x100 / x3000). Both sides build the same message and must produce
identical bytes before anything is timed; the benchmark
interleaves the implementations and keeps the best of several runs. It needs
`protoc`, a C compiler and the gem. Copied from that file, which is the single
source for all performance numbers; higher is better, MB/s over wire bytes:

| payload | op | pbwire-ruby (MB/s, x gem) | gem (MB/s) |
|---|---|---|---|
| 136 B | encode | 1015.0 (1.4x) | 741.1 |
| 136 B | decode | 462.9 (1.8x) | 263.5 |
| 174 B | encode | 956.9 (1.2x) | 803.2 |
| 174 B | decode | 514.3 (1.9x) | 267.4 |
| 2469 B | encode | 1473.1 (1.2x) | 1256.4 |
| 2469 B | decode | 970.6 (1.8x) | 528.9 |
| 74913 B | encode | 1582.4 (1.2x) | 1310.6 |
| 74913 B | decode | 1170.0 (1.7x) | 694.7 |

```shell
gem install google-protobuf
RUBY=$(command -v ruby) make ruby-bench
```

The win is architectural, not a micro-optimization:

- **Encoding** writes one pass from the C tree straight into the String that is
  returned; there is no intermediate buffer and no per-field Ruby call.
- **Decoding** is one validating scan that records where each wire field lives;
  the gem materializes every field.

## Conformance

`conformance/testee.rb` drives the binary, JSON and text formats of the
official `conformance_test_runner` through this binding
(`make conformance-ruby`, reusing the cached protobuf checkout from
`make conformance-setup`):

```
CONFORMANCE SUITE PASSED: 2747 successes, 0 skipped, 0 expected failures,
0 unexpected failures.
CONFORMANCE SUITE PASSED: 443 successes, 0 skipped, 0 expected failures,
0 unexpected failures.
```

Only JSPB answers `skipped`; every other verdict matches the reference
implementation exactly. (The binary suite runs without the shared failure
list: the wire reader's validation rejects the overlong tag varints the
core tree decoder truncates, so the two listed cases parse-fail like the
reference. Text output honors the runner's `print_unknown_fields` flag.)

## Layout

| Piece | Path |
|---|---|
| the binding | [`lib/pbwire.rb`](lib/pbwire.rb) loads the extension |
| the extension | [`ext/pbwire_ext/pbwire_ext.c`](ext/pbwire_ext/pbwire_ext.c) |
| build | [`build.sh`](build.sh) |
| example | [`examples/roundtrip.rb`](examples/roundtrip.rb) |
| self-check | [`test/test_pbwire.rb`](test/test_pbwire.rb) |
| benchmark | [`benchmarks/bench.rb`](benchmarks/bench.rb) | the four standard tiers against the `google-protobuf` gem |
| conformance | [`conformance/testee.rb`](conformance/testee.rb) | binary, JSON and text through the official runner |
| fixture | [`test/data/sample.proto`](test/data/sample.proto) and the checked-in `sample.desc` |

Regenerate the descriptor set after editing the fixture (it is checked in, so the
tests need no `protoc`):

```shell
(cd bindings/ruby/test/data && protoc --descriptor_set_out=sample.desc sample.proto)
```

## Generated models

`protoc-gen-pbwire` (in `../../codegen`) generates classes with keyword-argument
constructors and accessors, `#encode(schema)` and `.decode(schema, wire)`. The
committed output is `test/gen/` and `examples/gen/`; `examples/roundtrip.rb`
uses typed assignments only. See `../../codegen/README.md`.

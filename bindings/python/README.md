# pbwire Python binding

`bindings/python/runtime/pbwire.c` is a CPython extension and the Python
binding: a `FileDescriptorSet` (`protoc --descriptor_set_out`) becomes a schema
at runtime, and messages go between wire bytes and plain Python data with no
generated code and no intermediate message tree.

```python
import pbwire

schema = pbwire.Schema.from_file("examples/addressbook.desc")
person = schema.message("tutorial.Person")

wire = person.encode({"name": "alice", "id": 1,
                      "phones": [{"number": "555", "type": 1}]})
message = person.decode(wire)         # parsed once, fields read on demand
assert message.name == "alice"
assert message.phones[0].number == "555"
assert message.to_dict() == {"name": "alice", "id": 1,
                             "phones": [{"number": "555", "type": 1}]}
assert person.encode(message) == wire  # byte-exact round trip
```

## Two representations

| | path | cost |
|---|---|---|
| `dict` | `encode(dict)` / `Message.to_dict()` | one pass, plain CPython objects |
| wire | `decode(bytes)` → `Message` | one tag scan, a slice per wire field |
| `Message` | `encode(message)` | the bytes it was parsed from |

`decode()` returns a lazily read message rather than a dict tree, because a
dict tree has to build every dict, string and int before the caller can look at
any of them. On a 75 KB message that is ~400 µs of `malloc`-bound object
creation; `decode()` instead walks the wire once (58 µs) and records where each
field lives. Field values materialize when they are read, `to_dict()` builds
the whole tree when it is actually wanted.

`decode()` validates everything it walks: every tag, field number, wire type,
length, nesting depth, sub-message and packed run. A message you get back is
well-formed; field values (and proto2 `required` presence) are checked when
they are materialized or written.

## `Message`

A `Message` is immutable and keeps the buffer it was parsed from, so sub
messages share the bytes and `encode(message)` is a copy of them. That makes a
`decode`/`encode` round trip exact: unknown fields, field order and
non-canonical encodings survive, which a `to_dict()`/`encode(dict)` round trip
cannot do. Use `Message` to inspect or pass through payloads, and
`to_dict()`/`encode()` to work with plain data.

| member | meaning |
|---|---|
| `msg.name` / `msg["name"]` | field value; attribute access gives `None` for an absent field, item access raises `KeyError` |
| `msg.nums` | list for a repeated field, built when read |
| `msg.inner` | sub `Message`; repeated sub-messages come back as a list of `Message` |
| `msg.get(name, default=None)` | value, or `default` when the field is absent |
| `msg.keys()` / `values()` / `items()` | fields present on the wire, in wire order |
| `"nums" in msg`, `len(msg)` | presence and field count |
| `msg.to_dict()` | the eager form: plain dicts and lists all the way down |
| `msg.type` | message full name |
| `encode(msg)` | the message's wire bytes |

A singular message field that appears more than once on the wire merges (the
payloads concatenate), and a oneof member set later clears the earlier ones, in
both representations.

## Value mapping

| protobuf | Python |
|---|---|
| int32/int64/sint*/sfixed* | int (signed, range-checked) |
| uint32/uint64/fixed* | int (range-checked) |
| bool | bool |
| float/double | float |
| string | str (utf-8; undecodable bytes come back as `bytes`) |
| bytes | bytes |
| enum | int |
| message | dict / `Message` |
| repeated | list (any sequence is accepted on encode) |

A field missing from the dict is absent from the message and vice versa, which
matches proto2 presence and proto3 implicit presence. Values written into the
dict are emitted as given, including zeros — that is a valid but not the
shortest encoding of a proto3 message. Unknown fields in the wire are kept by a
`Message` and skipped by `to_dict()`; an unknown *key* on encode raises
`KeyError` instead of being dropped silently. Encoding a proto2 message whose
`required` field is missing raises `ValueError`, and so does materializing one.

## Layout

| Piece | Path | Role |
|---|---|---|
| runtime | [`runtime/pbwire.c`](runtime/pbwire.c) | the CPython extension: schema, message types, dict <-> wire, parsed messages |
| examples | [`examples/addressbook.py`](examples/addressbook.py) | schema at runtime, dict round trip + parsed message |
| tests | [`tests/test_pbwire.py`](tests/test_pbwire.py) | byte parity with the `protoc` reference, oneof/merge/required failure paths |
| benchmarks | [`benchmarks/bench.py`](benchmarks/bench.py) | vs `google.protobuf` on four tiers |
| conformance | [`conformance/`](conformance/) | official runner testee, `make conformance-python` |

## Build

```shell
make python-bind    # builds the extension next to the sources
make python-test    # self-check (no dependencies beyond Python)
make python-bench   # benchmark vs google.protobuf (needs protoc + protobuf)
```

The extension statically links the core (`build/libpbwire.a`), so it needs a C
compiler and the Python headers (`python3 -c "import sysconfig; ..."`), but no
setuptools, no wheel, no generated Python. `make python-bind` writes
`bindings/python/pbwire<EXT_SUFFIX>`; add that directory to `PYTHONPATH` (the
tests and the benchmark do it themselves).

## Benchmark

[`benchmarks/bench.py`](benchmarks/bench.py) compares against `google.protobuf`
(the upb C extension) on the same `matrix3.proto` message at the four standard
tiers from [`../../BENCHMARKS.md`](../../BENCHMARKS.md) (small / medium / large /
xlarge scale the repeated fields x0 / x1 / x100 / x3000), printing the same
`impl op ns/op MB/s` table as every other harness. Copied from that file, which
is the single source for all performance numbers; higher is better, MB/s over
wire bytes.

```shell
make python-bench
```

It verifies byte-for-byte parity with `protobuf`'s own encoding and
both dict representations before timing.

| payload | op | pbwire-py (MB/s, x upb) | upb (MB/s) |
|---|---|---|---|
| 136 B | encode(data) | 291.4 (5.7x) | 51.5 |
| 136 B | decode(bytes) | 663.7 (1.2x) | 568.6 |
| 136 B | to_dict(bytes) | 161.4 (4.8x) | 33.3 |
| 136 B | encode(parsed) | 3087.4 (4.1x) | 751.3 |
| 174 B | encode(data) | 334.7 (5.7x) | 58.4 |
| 174 B | decode(bytes) | 727.6 (1.3x) | 573.3 |
| 174 B | to_dict(bytes) | 154.5 (5.6x) | 27.6 |
| 174 B | encode(parsed) | 3897.8 (4.8x) | 819.8 |
| 2469 B | encode(data) | 497.8 (6.6x) | 75.2 |
| 2469 B | decode(bytes) | 1127.3 (1.6x) | 717.7 |
| 2469 B | to_dict(bytes) | 200.3 (4.8x) | 41.4 |
| 2469 B | encode(parsed) | 31535.5 (22x) | 1430.8 |
| 74913 B | encode(data) | 593.1 (6.9x) | 85.8 |
| 74913 B | decode(bytes) | 1376.6 (1.7x) | 810.5 |
| 74913 B | to_dict(bytes) | 173.2 (4.0x) | 43.5 |
| 74913 B | encode(parsed) | 94176.6 (63x) | 1500.0 |

Rows are the same user-visible task on both sides: `encode(data)` starts from
the same dict, `decode(bytes)` ends with an in-memory message, `to_dict(bytes)`
ends with plain Python data, and `encode(parsed)` starts from an already parsed
message (a `Message` hands back the bytes it was parsed from, so the round trip
is byte-exact).

upb wins nothing: it spends 890 µs filling a message field by field from the
dict, it parses into its arena and materializes a proxy per field access, and
its serialization re-encodes every field from its own structs where a parsed
`Message` just hands back the bytes. The `to_dict()` row is the one where both
sides pay for every dict, string and int; that cost is `malloc`-bound and
nobody can avoid it.

## Generated models

`protoc-gen-pbwire` (in `../../codegen`) generates dataclasses with type
annotations plus `to_dict()`/`from_dict()`, and `encode(msg_type)` /
`decode(msg_type, wire)` on top of the dict API. Each file splits in two:
`<base>_pbwire.py` (dataclasses) and `<base>_pbwire_meta.py` (the embedded
descriptor set plus its loader, imported by the models file).
`--pbwire_opt=lang=python[,model=bool][,register=bool][,registry=bool]`
(all default `true`): `model=false` leaves the schema module for
dynamic-only users; `register=false` drops the lazy load so
`init_schema()` (embedded bytes) or `use_schema()` (an external schema,
dual registration) must run first; `registry=false` drops
`MESSAGE_NAMES` + `preload()`. The committed output is
`tests/gen/` and `examples/gen/`; `examples/addressbook.py` uses the dataclasses
only. See `../../codegen/README.md`.

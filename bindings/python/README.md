# pbwire Python binding

`bindings/python/runtime/pbwire.c` is a CPython extension and the Python binding:
a `FileDescriptorSet` (`protoc --descriptor_set_out`) becomes a schema at
runtime, and messages go in and out as plain dicts. The C code walks the
pbwire schema and converts dict <-> wire directly — no generated Python code,
no intermediate message tree, no cffi.

```python
import pbwire

schema = pbwire.Schema.from_file("examples/addressbook.desc")
person = schema.message("tutorial.Person")
wire = person.encode({"name": "alice", "id": 1,
                      "phones": [{"number": "555", "type": 1}]})
assert person.decode(wire) == {"name": "alice", "id": 1,
                               "phones": [{"number": "555", "type": 1}]}
```

## Layout

| Piece | Path | Role |
|---|---|---|
| runtime | [`runtime/pbwire.c`](runtime/pbwire.c) | the CPython extension: schema, message types, dict <-> wire, lazy views |
| examples | [`examples/addressbook.py`](examples/addressbook.py) | schema at runtime, dict round trip + lazy view |
| tests | [`tests/test_pbwire.py`](tests/test_pbwire.py) | byte parity with the `protoc` reference, failure paths |
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

## Lazy views

`decode()` builds the whole dict tree. When only a few fields are needed,
`parse()` returns a view that scans the wire once (one slice per wire field) and
builds Python objects on demand:

```python
msg = person.parse(wire)      # 23 us for a 75 KB message (upb: 95 us)
msg.name                      # materializes just this field
msg.phones[0].number          # sub-messages are views too
person.decode(wire)           # plain dicts, ~370 us for the same message
msg.to_dict()                 # the eager form of a view (== decode)
dict(msg)                     # same, deep
```

A view supports attribute and `msg["field"]` access (absent field raises
`KeyError`), `in`, `len()`, iteration over `(name, value)` pairs, and
`to_dict()`. Field reads are cached per view. Note that a view re-reads the
wire: use `decode()` when you want every field as a dict, `parse()` when you
want a few.

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
| message | dict |
| repeated | list (any sequence is accepted on encode) |

A field missing from the dict is absent from the message and vice versa, which
matches proto2 presence and proto3 implicit presence. Values written into the
dict are emitted as given, including zeros — that is a valid but not the
shortest encoding of a proto3 message. Unknown fields in the wire are skipped;
an unknown *key* on encode raises `KeyError` instead of being dropped silently.
Encoding a proto2 message whose `required` field is missing raises
`ValueError`, and so does decoding one.

## Tests

[`tests/test_pbwire.py`](tests/test_pbwire.py) is the self-check (plain
asserts, no framework). Its oracle is the same one the C binding uses:
`bindings/c/tests/data/matrix3.bin`, the bytes `protoc --encode` produced from
`matrix3.txt`, covering every scalar kind, string, enum, nested message, three
repeated kinds and proto3 optional. It also checks the addressbook example and
the failure cases (unknown key, truncated wire, missing required, unknown wire
fields skipped).

## Benchmark

[`benchmarks/bench.py`](benchmarks/bench.py) compares against `google.protobuf`
(the upb C extension) on the same `matrix3.proto` message at the four standard
tiers from [`../../BENCHMARKS.md`](../../BENCHMARKS.md), printing the same
`impl op ns/op MB/s` table as every other harness:

```shell
make python-bench
```

It verifies byte-for-byte parity with `protobuf`'s own encoding before timing.

Current standing on Apple M4 Pro (see `BENCHMARKS.md` for the table):

| path | 75 KB message | relative to upb |
|---|---|---|
| `encode()` | 592 MB/s | 0.40x |
| `decode()` (dicts) | 202 MB/s | 0.26x |
| `parse()` (lazy view) | 3228 MB/s | 4.1x faster than `upb.FromString` |

The eager dict path stays behind upb because it must build every dict, string
and int; the lazy path beats it because it only records slices and defers those
objects until a field is read. `parse()` + `to_dict()` (~145 MB/s) is slower
than `decode()`: it walks the wire twice, so pick the API that matches the
access pattern.

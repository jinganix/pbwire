# pbwire Python binding

`bindings/python/csrc/pbwire.c` is a CPython extension and the Python binding:
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

Current standing on Apple M4 Pro (see `BENCHMARKS.md` for the table): the
extension reaches ~0.25-0.4x of upb, i.e. 12-15x faster than the cffi version
it replaced (which was 0.02x). Two reasons it does not go further:

- upb parses into its own arena and materializes Python objects lazily; a
  decoder that promises plain dicts has to build every dict, string and int
  eagerly.
- upb's Python interop is hand-tuned Cython; a table-driven walker pays a
  per-field dispatch that generated code would not.

Closing the gap would mean lazy message views (a different API) or emitting
straight-line C per message from the plugin.

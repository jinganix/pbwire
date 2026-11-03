# pbwire Python binding

`pbwire_cffi.py` is the Python binding: cffi in ABI mode, no generated Python
code, no build step of its own. A `FileDescriptorSet` (`protoc
--descriptor_set_out`) becomes a schema at runtime and messages go in and out
as plain dicts.

```python
import pbwire_cffi as pbw

lib = pbw.load()                                     # opens libpbwire
schema = pbw.Schema.from_file(lib, "examples/addressbook.desc")
person = schema.message("tutorial.Person")
wire = person.encode({"name": "alice", "id": 1,
                      "phones": [{"number": "555", "type": 1}]})
assert person.decode(wire) == {"name": "alice", "id": 1,
                               "phones": [{"number": "555", "type": 1}]}
```

## Build

```shell
make shared         # build/libpbwire.so.1 | build/libpbwire.1.dylib
make python-bind    # optional: bulk helpers (build/libpbwirepy.*)
make python-test    # self-check (needs cffi)
make python-bench   # benchmark vs google.protobuf (needs protoc + protobuf)
```

The binding needs `cffi`. `libpbwirepy` is a ~200-line bridge
([csrc/pbw_py.c](csrc/pbw_py.c)) that fills or drains every scalar of a whole
message tree from flat arrays; without it the binding still works, one cffi
call per value (same results, ~2x the time on large messages). `make
python-test` asserts both paths produce identical bytes.

## Value mapping

| protobuf | Python |
|---|---|
| int32/int64/sint*/sfixed* | int (signed) |
| uint32/uint64/fixed* | int |
| bool | bool |
| float/double | float |
| string | str (utf-8; undecodable bytes come back as `bytes`) |
| bytes | bytes |
| enum | int |
| message | dict |
| repeated | list |

A field missing from the dict is absent from the message and vice versa, which
matches proto2 presence and proto3 implicit presence. Unknown fields survive a
decode→encode round trip. Encoding a proto2 message whose `required` field is
missing raises `ValueError`, exactly like `pbw_struct_encode` does in C.

## Tests

[`tests/test_pbwire.py`](tests/test_pbwire.py) is the self-check (plain
asserts, no framework). Its oracle is the same one the C binding uses:
`bindings/c/tests/data/matrix3.bin`, the bytes `protoc --encode` produced from
`matrix3.txt`, covering every scalar kind, string, enum, nested message, three
repeated kinds and proto3 optional. It also checks the fast and slow paths
against each other, the addressbook example, and the failure cases (unknown
field, truncated wire, missing required).

## Benchmark

[`benchmarks/bench.py`](benchmarks/bench.py) compares against the official
`google.protobuf` runtime on the same `matrix3.proto` message at the four
standard tiers from [`../../BENCHMARKS.md`](../../BENCHMARKS.md), printing the
same `impl op ns/op MB/s` table as every other harness:

```shell
make python-bench                       # pbwire-py vs the official runtime
python3 benchmarks/bench.py bench --slow # also measure the no-shim fallback
```

It verifies byte-for-byte parity with the reference bytes before timing. The
baseline is the upb (C) implementation that ships in the `protobuf` wheel; a
cffi bridge that builds Python objects per value cannot match a C++ extension
on large payloads, so read the table as "what the no-generated-code path
costs", not as a like-for-like codec comparison.

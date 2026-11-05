# C conformance

The C binding has two layers, and the official suite covers the first one
directly:

| layer | covered by |
|---|---|
| wire codec (`src/*.c`, the library both layers compile against) | `make conformance` — Google's `conformance_test_runner`, binary + text + JSON |
| struct codec (`runtime/pbw_struct.c`) | `make test` — `tests/test_struct.c` compares struct bytes with the `protoc --encode` reference for `matrix2` (proto2: required/optional/defaults, packed and unpacked) and `matrix3` (proto3: every scalar kind, string, enum, nested message, proto3 optional) |

Why the struct codec is not the testee itself (yet): it is driven by a layout
table per message, and the official conformance messages need extensions and
MessageSet members the binding has not proven yet. Bytes, maps, groups and
oneofs all ride `pbw_struct_kind_t` rows now (`PBW_SF_BYTES`, repeated entry
structs, `START_GROUP`/`END_GROUP` tags, union + case); a struct testee can be
generated with `protoc-gen-pbwire-c` from
`google/protobuf/test_messages_proto3.proto` and wired into the existing
dispatch in `conformance/testee.c`.

```shell
make conformance        # core (dynamic) codec, official runner
make test               # includes the struct byte-parity checks
```

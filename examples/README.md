# examples

Two self-contained tutorial schemas plus the data that goes with them.

## addressbook (proto2, structs)

All three describe the same address book (Alice + Bob); the JSON and
the wire bytes match what `build/example_addressbook` encodes from its
demo struct literals.

| File | Role |
|---|---|
| `addressbook.proto` | schema (proto2, no imports): `tutorial.AddressBook` |
| `addressbook.desc` | the schema as a `FileDescriptorSet` (`protoc --descriptor_set_out`); loaded at runtime, never embedded |
| `addressbook.json` | the same address book as JSON (test data, e.g. for `pbw_json_parse`) |
| `addressbook.bin` | the same address book as wire bytes (pinned `protoc --encode` output) |

## gateway (proto3, dynamic API)

The gateway loop — decode bytes from a newer producer, inspect the known
fields, print JSON for the log, forward everything losslessly — through
the dynamic API, no structs. `build/example_gateway` asserts each step:
unknown field 99 survives decoding and re-encoding byte-identically.

| File | Role |
|---|---|
| `gateway.proto` | schema (proto3, no imports): `shop.Order` (nested message, map, enum) |
| `gateway.desc` | the schema as a `FileDescriptorSet`; loaded at runtime |

```sh
# run the examples: structs (C + C++) and the gateway loop
make examples
```

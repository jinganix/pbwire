# examples

One self-contained tutorial schema plus the data that goes with it.
All three describe the same address book (Alice + Bob); the JSON and
the wire bytes match what `build/example_addressbook` encodes from its
demo struct literals.

| File | Role |
|---|---|
| `addressbook.proto` | schema (proto2, no imports): `tutorial.AddressBook` |
| `addressbook.desc` | the schema as a `FileDescriptorSet` (`protoc --descriptor_set_out`); loaded at runtime, never embedded |
| `addressbook.json` | the same address book as JSON (test data, e.g. for `pbw_json_parse`) |
| `addressbook.bin` | the same address book as wire bytes (pinned `protoc --encode` output) |

```sh
# run the examples (C + C++): struct literals -> wire bytes -> decode -> compare
make examples
```

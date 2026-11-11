# Shared fixtures

| file | used by |
|---|---|
| `desc_proto2.proto`, `desc_proto3.proto` | `make desc-fixture` → the pinned descriptor set in `tests/test_descriptor.c` |
| `adv.proto`, `adv.desc` | Node and PHP self-checks: one service with all four streaming shapes plus a holder for the WKT helpers (`Any`, Timestamp, Duration, FieldMask) |
| `edge.proto`, `edge.txt`, `edge.desc`, `edge.bin` | the host bindings' self-checks (Kotlin, PHP, Dart, Rust, C#): payloads with embedded NULs and invalid UTF-8, non-ASCII text, a negative enum, an empty repeated element, proto3 optional presence with an empty value, and oneof switching |

`edge.desc` and `edge.bin` are pinned so the binding tests need no `protoc` at
run time. Regenerate both after editing `edge.proto` or `edge.txt`:

```shell
protoc -Itests/data --descriptor_set_out=tests/data/edge.desc \
    --include_imports tests/data/edge.proto
protoc -Itests/data --encode=edge.Edge tests/data/edge.proto \
    < tests/data/edge.txt > tests/data/edge.bin
```

The bindings rebuild the same message field by field and require their bytes to
equal `edge.bin`, then decode it back and check every value plus a byte-exact
re-encode.

`adv.desc` is pinned for the same reason (the tests must not need `protoc`).
Regenerate it after editing `adv.proto`:

The second `-I` must hold `google/protobuf/any.proto` and the rest of the
well-known types (Homebrew: `/opt/homebrew/include`, Debian: `/usr/include`).

```shell
protoc -Itests/data -I/opt/homebrew/include --include_imports \
    --descriptor_set_out=tests/data/adv.desc tests/data/adv.proto
```

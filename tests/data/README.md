# Shared fixtures

| file | used by |
|---|---|
| `desc_proto2.proto`, `desc_proto3.proto` | `make desc-fixture` → the pinned descriptor set in `tests/test_descriptor.c` |
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

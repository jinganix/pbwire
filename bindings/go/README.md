# pbwire Go binding

A cgo cover of the C ABI. The schema always comes from a `FileDescriptorSet`
(`pbw.Load` / `pbw.LoadFile`), the same artifact the C, C++ and Python bindings
load; nothing is generated for Go. Messages travel as a map of field number to
scalar value.

| Piece | Path | Role |
|---|---|---|
| runtime | [`runtime/`](runtime/) | package `pbwire`: schema, message, encode/decode (tests next to the code, as Go expects) |
| examples | [`examples/addressbook/`](examples/addressbook/) | schema + round trip, `go run ./examples/addressbook` |
| benchmarks | [`benchmarks/`](benchmarks/) | `go test -bench . ./benchmarks` (four tiers of scalar fields) |
| conformance | [`conformance/`](conformance/) | not runnable yet: see the README there |

```shell
make shared                                    # build/libpbwire.so.1 (or .dylib)
cd bindings/go
go test ./...                                  # descriptor loading + round trip
go run ./examples/addressbook                  # 10 bytes, name=alice id=1234
go test -bench . -benchmem ./benchmarks        # cgo overhead per op
```

The module lives at `bindings/go` (`module pbwire`), the package at
`pbwire/runtime`, so the import is `pbwire "pbwire/runtime"`.

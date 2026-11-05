# Go oracle pin

`TestT4Oracle` (in `../compat_test.go`) diffs the binding's generated code
against `protoc-gen-go` output for the same `matrix3.proto` / `oneof1.proto`.
The oracle files live in `../gen_oracle/` and are produced by the upstream
generator, not by `protoc-gen-pbwire`:

```bash
protoc -I<proto dir> --go_out=<gen_oracle dir> --go_opt=paths=source_relative \
  --go_opt=Mmatrix3.proto=<module>/matrix3pb <proto dir>/matrix3.proto
```

(`codegen/Makefile`, `gen-go` target; `Mxfile.proto=...` style `M` flags map
each import to its sibling package, same as the pbwire files.)

`oraclePin` in `compat_test.go` must equal the
`google.golang.org/protobuf` version in `bindings/go/go.mod`: the oracle
`.pb.go` files embed that runtime's API, so regenerating them with a
different `protoc-gen-go` without moving both pins breaks the byte gate.
The test reads the version from `go.mod` itself (not from the test binary's
build info, which older toolchains omit), so the check behaves the same on
every Go version.

To move the pin: install the matching `protoc-gen-go`, regenerate the four
oracle files, bump `go.mod` (`go get google.golang.org/protobuf@<v>`), set
`oraclePin` to the same `<v>`, and run `go test ./...`.

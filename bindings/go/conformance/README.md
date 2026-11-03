# Go conformance

The Go binding has no conformance testee, by design.

Google's `conformance_test_runner` drives one binary with arbitrary
`ConformanceRequest`/`ConformanceResponse` messages and asks it to encode and
decode any of the test schemas. A testee therefore needs a *dynamic* runtime: a
schema loaded at run time, a generic message representation, and text/JSON
mappings for the non-binary suites.

The Go binding is the opposite model. `protoc-gen-pbwire-go` compiles one
`.proto` file into one Go package of plain structs, and the generated code
contains no descriptor, no field lookup and no generic value type. A struct
cannot represent a message it was not generated for, so a testee would have to
be generated per test schema and could not answer a request for an arbitrary
message name.

What covers the binding instead:

- `bindings/go/tests` compares the generated codecs byte for byte against
  `protoc --encode` output for matrix2 (proto2 presence, packed/unpacked,
  required fields, negative enum), matrix3 (every scalar kind, enums, nested
  messages, proto3 optional) and the Go-only edge fixture (bytes presence,
  `-0.0`, empty repeated entries, recursion).
- `bindings/go/runtime` is the only wire-reading code and is unit-tested for
  truncated varints, overlong tags, malformed lengths, group skipping and the
  nesting cap.
- The binary wire format itself, including the spec corners the generated codec
  relies on, is pinned by the C core's conformance run (`make conformance`),
  which runs the dynamic testee on the same reference implementation.

A Go testee would mean adding a dynamic message API on top of a descriptor-set
loader in Go -- a second implementation of `src/descriptor_set.c` and
`src/message.c`. That is possible, but it is a different binding, and it would
not exercise the generated code path these tests do.

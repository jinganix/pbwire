# Go conformance

Not runnable yet, and the reason is the binding, not the harness.

`conformance/testee.c` (repo root) passes Google's suite because it drives the
dynamic core, which accepts any `FileDescriptorSet`. The Go binding is a thin
cgo cover with a hand-built scalar schema (`runtime/pbwire.go`): it cannot
express nested messages, repeated fields, oneofs, maps and well-known types, so
a testee built on it would fail nearly every case and prove nothing.

How to get there, in order:

1. ~~add descriptor-set loading~~ done: `pbwire.Load` / `LoadFile` take a
   `FileDescriptorSet`, and `Schema.Message(name)` reads the fields from it;
2. add message/repeated accessors over `pbw_message_get` / `pbw_value_at` (the
   binding decodes scalars only today);
3. then port `conformance/testee.c`: read the length-prefixed
   `ConformanceRequest` (decoded with the binding itself, from
   `conformance.proto` in the descriptor set), pick the message by name, decode
   and re-encode, answer `skipped` for JSON and text.

Until then, the Go binding is covered by
[`runtime/pbwire_test.go`](../runtime/pbwire_test.go) (round trip + ABI check)
and by `make conformance`, which pins the C ABI the binding compiles against.

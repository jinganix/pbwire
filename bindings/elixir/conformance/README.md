# Elixir conformance

Not runnable yet; the blocker is how the binding learns a schema.

`conformance/testee.c` (repo root) passes Google's suite because the dynamic
core loads any `FileDescriptorSet`. `Pbwire` instead takes `elixir-protobuf`
generated modules (`load_schema/1` consumes `__message_props__/0`), so a
testee needs the conformance protos compiled by `protoc-gen-elixir` — and one
app cannot hold both
`protobuf_test_messages.proto2.TestAllTypesProto2` and
`...proto3.TestAllTypesProto3` under distinct Elixir aliases without a
naming pass, the same collision the Erlang binding documents for gpb
record names.

Plan, in order:

1. compile each test-messages proto with a distinct Elixir prefix (e.g.
   `P2`, `P3`, `Ed2023`) so message and enum aliases stay unique;
2. load the union with one `Pbwire.load_schema/1` call;
3. port `conformance/testee.c`: read the length-prefixed `ConformanceRequest`
   (decoded with `Pbwire` itself, from the same modules), pick the message by
   the suffix of `message_type`, decode and re-encode, answer `skipped` for
   JSON, JSPB and text.

Until then, correctness is covered by the ExUnit suite (`make test`):
byte-identity with `Protobuf.encode/1` and struct equality with
`Protobuf.decode/2` on proto2, proto3, maps, oneofs, packed fields, explicit
presence and float specials — and the wire codec it wraps is covered by
`make conformance`.

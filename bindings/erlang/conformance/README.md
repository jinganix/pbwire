# Erlang conformance

Not runnable yet; the blocker is how epb learns a schema.

`conformance/testee.c` (repo root) passes Google's suite because the dynamic
core loads any `FileDescriptorSet`. `pbwire` instead takes gpb-style
definitions (`load_cache(Defs)`) from a generated module, so a testee needs the
conformance protos compiled by gpb — and one module cannot hold both
`protobuf_test_messages.proto2.TestAllTypesProto2` and
`...proto3.TestAllTypesProto3` because gpb names records by their unqualified
message name, so the two collide.

Plan, in order:

1. compile each test-messages proto with a distinct
   `{msg_name_prefix, "P2" | "P3" | "Ed2023"}` so records stay unique;
2. load the union of their `get_msg_defs()` into one cache;
3. port `conformance/testee.c`: read the length-prefixed `ConformanceRequest`
   (decoded with epb itself, from the same defs), pick the message by the
   suffix of `message_type`, decode and re-encode, answer `skipped` for JSON,
   JSPB and text.

Until then, the binding's correctness is covered by its own suites
(`make erlang-test`: EUnit + the NIF C tests, including gpb parity), and the
wire codec it wraps is covered by `make conformance`.

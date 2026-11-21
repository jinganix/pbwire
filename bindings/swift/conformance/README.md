# Swift binding conformance

`main.swift` here is the official testee, driving the **pure-Swift core**
(`Sources/PbwireCore/`, the `PbwireCore` product) rather than the C core:

```
make conformance-swift        # from the repo root
```

It speaks the same protocol as every other binding's testee: 4-byte
little-endian length prefix, then a `ConformanceRequest`; the answer is a
`ConformanceResponse` framed the same way. Requests name a message type and the
descriptor set (`build/conformance.desc`) answers with the schema, so there is
no per-message code and no C library in the testee at all — `swift build
--product pbwire-conformance` links only `PbwireCore`, which has no `unsafeFlags`
and no `libpbwire.a`.

Binary in, binary out is what the core implements. JSON and text requests answer
`skipped`, which the runner counts as a skip rather than a failure, so the pass
set is the binary suite; the JSON/text mapping is out of this core's scope.

Result (2026-11-21, protobuf v36.2 runner): 1412 successes, 1403 skipped, 2
expected failures, 0 unexpected — the same failure set as `conformance/testee.c`
and every other port (`conformance/failure_list_pbwire.txt`). The two expected
cases are the `BadTag_FieldNumberSlightlyTooHigh` pair: a 5-byte tag varint above
UINT32_MAX truncates instead of failing, exactly as the reference C++
`CodedInputStream` does.

The FFI path needs no testee of its own: every byte it encodes or decodes goes
through the same `pbw_encode` / `pbw_decode` that `conformance/testee.c`
exercises. What this file adds is the check for the *other* product — the pure
Swift wire implementation — which the C testee cannot cover.

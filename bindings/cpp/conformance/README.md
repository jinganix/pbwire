# C++ conformance

The C++ binding is a thin layer over the C struct runtime
(`bindings/c/runtime/pbw_struct.c`) and shares its generated headers, so it
inherits exactly the C binding's conformance position:

- wire format: `make conformance` (Google's runner against the core codec, the
  same `src/*.c` the C++ code links);
- struct mapping: `make test` (byte parity with the `protoc` reference) and
  `make test-cpp` (the C++ round trip and failure paths).

There is no C++-specific codec to test separately — see
[`../../c/conformance/README.md`](../../c/conformance/README.md) for why the
struct layer cannot be the conformance testee yet.

# C++ benchmarks

Same codec as the C binding: `bindings/cpp` adds no encoder of its own, it uses
`bindings/c/runtime/pbw_struct.{h,c}` directly. So the C struct numbers *are*
the C++ numbers, and there is no second harness to keep in sync:

```shell
make bench-c     # from the repo root: structs vs protoc --cpp_out on matrix3.proto
```

Results for the four standard tiers (135 B / 174 B / 2469 B / 74913 B) live in
[`../../../BENCHMARKS.md`](../../../BENCHMARKS.md). Add a C++-specific harness
here only if a real C++ API (classes, `std::string`, RAII views) is added on
top — that API, not the C one, would be what needs measuring.

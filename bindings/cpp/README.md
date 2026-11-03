# pbwire C++ binding

[`examples/addressbook.cc`](examples/addressbook.cc) is the C++ twin of the
C sample ([`bindings/c/examples/`](../c/examples/)): the same addressbook
round-trip (struct literals -> wire bytes -> decode -> compare, silent,
exit code only), written with aggregate initialization and `nullptr`.

There is no separate C++ runtime or plugin: the C runtime
(`../c/runtime/pbw_struct.{h,c}`, `extern "C"`-clean) and the generated
header (`../c/examples/gen/addressbook.h`) are shared unchanged. A real C++
binding would add classes/RAII on top; the example stays on plain structs
so the wire behavior it pins is exactly the C one.

Build and run (from the repo root; needs a C++17 compiler):

```sh
make examples
```

Stays out of `make test`: that gate is C99-only. `make test-cpp` runs the
C++ self-check (round trip and the required-field failure path).

## Layout

| Piece | Path | Role |
|---|---|---|
| runtime | the C runtime, shared unchanged | `bindings/c/runtime/pbw_struct.{h,c}`, `extern "C"`-clean |
| plugin | the C plugin, shared unchanged | [`bindings/c/plugin/`](../c/plugin/) |
| examples | [`examples/`](examples/) | addressbook round trip, written the C++ way |
| tests | [`tests/`](tests/) | `make test-cpp`: round trip + required-field failure |
| benchmarks | [`benchmarks/`](benchmarks/) | the C numbers (same codec): `make bench-c` |
| conformance | [`conformance/`](conformance/) | inherits the C position: official suite + struct parity |



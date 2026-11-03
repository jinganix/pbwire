# pbwire C++ binding example

[`example/addressbook.cc`](example/addressbook.cc) is the C++ twin of the
C sample ([`bindings/c/example/`](../c/example/)): the same addressbook
round-trip (struct literals -> wire bytes -> decode -> compare, silent,
exit code only), written with aggregate initialization and `nullptr`.

There is no separate C++ runtime or plugin: the C runtime
(`../c/runtime/pbw_struct.{h,c}`, `extern "C"`-clean) and the generated
header (`../c/example/gen/addressbook.h`) are shared unchanged. A real C++
binding would add classes/RAII on top; the example stays on plain structs
so the wire behavior it pins is exactly the C one.

Build and run (from the repo root; needs a C++17 compiler):

```sh
make examples
```

Stays out of `make test`: that gate is C99-only.

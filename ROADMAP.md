# Roadmap

Where pbwire is going, and what it will never become. Dates are not promised;
order is by value per effort. Anything marked **never** is also marked in
[design.md](docs/design.md#scope-and-boundaries) so the decision survives.

## Done (this tree already)

- Byte-identical binary codec, JSON mapping and text format vs `protoc 36.2`
  (official conformance green, `make difftest` verdict parity).
- Dynamic schema from `FileDescriptorSet`, 23 host bindings, per-binding
  benchmark vs the official runtime ([BENCHMARKS.md](BENCHMARKS.md)).
- Bounded inputs (`pbw_limits` on wire bytes, `pbw_descriptor_set_load_limits`
  on sets), service routing (`pbw_service_resolve`), Any/Timestamp/Duration
  helpers (`src/wkt.c`).
- Release pipeline with provenance attestations; nightly difftest-full,
  conformance, x86_64 bench baseline, every ubuntu-capable binding test and
  the generated-model tests split by toolchain family.
- Environment gates in PR CI: ASan over the Python/Ruby/Node extensions,
  ThreadSanitizer over `test_threads`, 32-bit (ILP32) and big-endian (s390x
  under qemu) runs, and a Doxygen render of the public headers.

## Never

- `.proto` text parsing in the core (the `FileDescriptorSet` is the schema
  format; `pbwire-protoc` stays the optional bridge).
- A pbwire CLI (`protoc --decode/--encode` covers inspection).
- RPC transports/stubs in the core (routing helpers only; bytes on the
  socket stay official gRPC/Twirp).
- Canonical cross-version bytes (deterministic mode sorts maps only, like
  the reference).

## Next

- **Coverage floor ratchet.** The merged line coverage is ~90%; the floor sits
  at 85 (Linux gcov). Raise it only with a green CI run.
- **Binding sanitizer gates beyond address.** ASan covers the three C
  extensions in PR CI; a UBSan/TSan pass over the Python and Ruby suites is
  the next layer.
- **Per-binding `pbw_pool_*` exposure.** `retain_cap` is now reachable from
  Java/Kotlin/Scala/Clojure, Erlang and Elixir; the pool API (a bounded
  freelist of contexts) is still core-only.
- **Generated-model compat modes (T1-T6) in the newer bindings.** Swift, Zig,
  Scala, Clojure, F#, Elixir, Julia and R run the byte-equality generated
  test; the T1-T6 compat suite still stops at Node and Python.
- **Windows as a first-class build.** `ctest` already runs the full gate on
  `windows-latest` (MSVC `/W4 /WX`); what is missing is a Windows-specific
  note in the docs and a wheel/MSVC consumer test.
- **Publish the API reference.** `make api-docs` renders it in CI and
  uploads an artifact; a public site waits on Pages being enabled, not on
  content.

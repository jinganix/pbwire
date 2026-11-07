# Security policy

## Reporting a vulnerability

Email the maintainers with:

- the bytes (or `.proto`) that trigger it,
- the entry point (`pbw_decode`, `pbw_json_parse`, `pbw_descriptor_set_load`,
  ...),
- whether it came from `make difftest` / `make conformance` / `tests/fuzz_decode.c`,
- the commit hash and platform.

Do **not** open a public issue for a suspected memory-safety bug. We aim to
acknowledge within 72 hours and to ship a fix plus a regression test before
disclosing.

## Scope and premises

- **Wire data is untrusted.** The decoder bounds-checks every length, rejects
  field number 0 and wire types above 5, caps nesting at `PBW_MAX_DEPTH`
  (lowerable per call via `pbw_limits`), and returns `pbw_status` instead of
  aborting. CIP covers this with `test_malformed`, a fixed-seed differential
  run, and the stdin fuzz harness (`make fuzz`).
- **Descriptors are trusted input.** Do not load a `FileDescriptorSet` from
  an untrusted source without validation: a hostile descriptor set is a
  resource-consumption (memory/CPU) vector, like loading an untrusted schema
  in any dynamic runtime. See `docs/design.md` (Scope and boundaries).
- **Supported versions.** Only the latest `vX.Y.Z` release line receives
  security fixes. Binding runtimes (NIF, C extensions, FFI hosts) inherit the
  core's guarantees only through the documented C ABI; hosts must still treat
  their own object-graph construction as untrusted-input handling.

## Hardening checklist for embedders

1. Set `pbw_limits.max_bytes` / `max_depth` at every untrusted boundary.
2. One `pbw_ctx` per thread; never share schemas or messages across threads
   without caller-side confinement.
3. Check `PBW_ABI_VERSION` at load time when `dlopen`ing `libpbwire`.

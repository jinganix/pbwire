# Changelog

All notable changes to pbwire. Versions follow SemVer; `VERSION` is the
source of truth and `scripts/sync-versions.sh` propagates it to every
binding manifest.

## [Unreleased]

### Added

- `SECURITY.md`: vulnerability reporting policy and supported versions.
- `NOTICE`: third-party attributions (`descriptor.proto` wire shape, test
  baselines).
- `scripts/release-check.sh` (`make release-check`): release dry-run — version
  consistency, tag-vs-`VERSION` match, changelog entry, artifact presence.
- `tests/fuzz_decode.c` (`make fuzz`): stdin-fed decode/encode/visit fuzz
  harness; CI runs it on a fixed corpus plus a short deterministic mutation
  burst.
- Editions `FeatureSet` support in `pbw_descriptor_set_load()`: file-level
  and per-field `field_presence` / `repeated_field_encoding` are honored
  instead of treating every edition as proto3 defaults.
- Service descriptors: `FileDescriptorSet` services are loaded and queryable
  (`pbw_schema_service_*`, `pbw_service_*`); the core still does no RPC.
- MessageSet wire format: messages with `message_set_wire_format` decode
  group items into their extension fields and re-encode them byte-identically.
- `bindings/MIGRATING.md`: per-language migration notes from the official
  runtimes.
- `examples/gateway/`: proto3 + JSON + unknown-field passthrough tutorial.

### Changed

- CI: added `sanitizers` (ASan+UBSan `make test`), `difftest-short`
  (fixed-seed differential run), `fuzz-short`, and `coverage` (core-only
  `gcov` floor) jobs; conformance also runs on a nightly schedule.

## [0.1.12] - 2026-11-06

- Erlang Hex package vendors the C core.
- Full bench sweep refresh (R/Zig baselines).

## [0.1.11] - 2026-11-06

- Core fast paths; C# holder fix; all-green baselines.

## [0.1.8] - 2026-11

- Core fast paths (link-time decode actions, packed encode); first release
  line where every binding beats its baseline on xlarge encode.

<!--
Release process: bump VERSION, run scripts/sync-versions.sh, add the entry
above (date = tag date), run ./scripts/release-check.sh, then tag vX.Y.Z.
-->

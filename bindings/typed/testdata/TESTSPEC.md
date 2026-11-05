# Typed-model test spec (all bindings)

Every binding's typed test implements modes T1–T6 against the shared
fixtures in this directory. A new language copies the pattern, it does not
re-invent what to test.

## Fixtures

| file | meaning |
|---|---|
| `vectors.json` | canonical `Scalars3` values; the single source of truth |
| `../../c/tests/data/matrix3.{proto,desc,bin,txt}` | schema + pinned `protoc` reference bytes (the wire oracle) |
| `ORACLES.md` | each language's对照 library, pinned version, update command |
| `errors/` | invalid `CodeGeneratorRequest` samples + expected plugin error/skip |

Value encodings in `vectors.json`: int64 as decimal strings, non-finite
floats as `"Infinity"`/`"-Infinity"`/`"NaN"`, bytes as base64, enums as the
`NAME` string. Each adapter converts to its own precise types.

## Modes

- **T1 wire**: build the message from `vectors.json` through the generated
  model only, encode, require byte equality with `matrix3.bin`.
- **T2 decode**: decode `matrix3.bin` through the generated model, check
  every field (including presence) against `vectors.json`.
- **T3 empty/zero**: an empty message encodes to empty bytes; an explicit
  zero with presence (`opt_i32: 0`) round-trips as present.
- **T4 differential**: same object through the对照 library (protobufjs,
  `protobuf`, …): `verify` agrees (both accept / both reject), encoded
  bytes are equal, `toObject`/`MessageToDict` agree after canonicalization
  (int64→string, bytes→base64, enum→number).
- **T5 switch**: one business-logic snippet written against the对照
  library's static API runs unchanged against our generated code
  (import path is the only difference) with identical observable results.
- **T6 errors**: `verify()` (or the language's equivalent) rejects bad
  objects with a message, not a crash: wrong field type, out-of-range
  integer, unknown enum value, missing `required`, several oneof members
  set. Plugin-level malformed input is covered by `errors/` golden tests.

## Scope notes

- The official conformance proto is generation-compile only
  (`make typed-conformance`): the wire is already proven at the C core,
  the typed layer proves delegation (T1), not a second full run.
- `map` fields and cross-file message references stay outside the typed
  model (`// skipped …`); the dynamic API covers them.

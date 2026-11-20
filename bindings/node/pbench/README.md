# Node pbench: three arms, one contract

The arms, the gate and the timing loop from
[`bindings/README.md`](../../README.md#pbench-three-arms-one-contract), applied
to `structtest.Scalars3` from `bindings/c/tests/data/matrix3.proto` and the four
standard tiers. Every arm does the same user-visible task: a native object (a
plain JS object, proto field names, `BigInt` for 64-bit fields) in and wire
bytes out; wire bytes in and a fully readable native object out.

| arm | file | mechanism | what it answers |
|---|---|---|---|
| `tree` | `a-tree.js` | the shipped handle path: `newMessage()` plus one `set()` per field, `encode()`; decode into a `Message`, one `get()` per field | what the per-field downcall API costs for the whole task |
| `shim` | `b-shim.js` | `type.encodeObject(obj)` / `type.decodeToObject(wire)`: one N-API downcall, the addon walks the JS object against the schema's field table in C | is the boundary worth paying for at all |
| `core` | `n/a` | a TS/JS port of the C core | plan 15 (`issue41-15-core-ts.md`) builds it; until then the arm prints `n/a` |

`Payload` is `payload.js` (one object shared by the gate and the timing loop),
the oracle is google-protobuf (jspb) generated code from the same `.proto`.

## Gate

```
bindings/node/pbench/setup.sh        # addon + benchmarks/gen (jspb + .desc)
node bindings/node/pbench/gate.js    # gate: PASS / exit 1
```

The gate asserts, per tier, that `a-tree` bytes and `b-shim` bytes equal jspb's
bytes, that each arm's decode reads the fields `benchmarks/bench.js` checks
(`str`, `inner.name`, `inner.value`, the repeated lengths and last element, the
proto3 optionals, `u64`, `color`), and that the decoded object re-encodes to the
same bytes. It fails loudly while an arm or the `encodeObject` /
`decodeToObject` surface is missing, so no row is published before the arm
exists.

## Timing

`compare.js` interleaves `a-tree`, `b-shim`, jspb and protobufjs in one loop per
tier and reports the median of five rounds (`node pbench/compare.js`). The
formal, recorded run is `make node-bench`, which drives
`benchmarks/bench.js` and prints `pbwire-node-tree` (the prebuilt handle row)
beside `pbwire-node-shim` (the native-object row).

## Run table

Provisional numbers land here from Stage 3; the recorded Stage 5 rows live in
`BENCHMARKS.md`.

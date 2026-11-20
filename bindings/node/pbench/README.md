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

Provisional (Stage 3, `node pbench/compare.js`, median of five interleaved
rounds, 2026-11-21 under load average ~33): `tree` is the whole user task
(build the handle with one `set()` per field, then encode; decode, then read
every field back), `shim` is one `encodeObject` / `decodeToObject` call. The
prebuilt-handle row `make node-bench` publishes as `pbwire-node-tree` is a
different op and is not in this table.

| tier | arm | encode ns/op | encode MB/s | decode ns/op | decode MB/s |
|---|---|---|---|---|---|
| 136 B | `pbwire-node-tree` | 3566.5 | 38.1 | 3551.3 | 38.3 |
| 136 B | `pbwire-node-shim` | 3048.5 | 44.6 | 2241.6 | 60.7 |
| 136 B | `google-protobuf` | 2398.3 | 56.7 | 1183.5 | 114.9 |
| 136 B | `protobufjs` | 564.6 | 240.9 | 355.2 | 382.9 |
| 174 B | `pbwire-node-tree` | 4349.8 | 40.0 | 4907.6 | 35.5 |
| 174 B | `pbwire-node-shim` | 3727.4 | 46.7 | 3632.3 | 47.9 |
| 174 B | `google-protobuf` | 3001.4 | 58.0 | 1641.1 | 106.0 |
| 174 B | `protobufjs` | 671.3 | 259.2 | 456.4 | 381.3 |
| 2469 B | `pbwire-node-tree` | 55984.8 | 44.1 | 55996.4 | 44.1 |
| 2469 B | `pbwire-node-shim` | 54599.0 | 45.2 | 50813.3 | 48.6 |
| 2469 B | `google-protobuf` | 45947.6 | 53.7 | 31715.9 | 77.8 |
| 2469 B | `protobufjs` | 6567.9 | 375.9 | 6129.9 | 402.8 |
| 74913 B | `pbwire-node-tree` | 1583318.7 | 47.3 | 1534439.4 | 48.8 |
| 74913 B | `pbwire-node-shim` | 1571778.1 | 47.7 | 1426569.4 | 52.5 |
| 74913 B | `google-protobuf` | 1382696.4 | 54.2 | 922986.7 | 81.2 |
| 74913 B | `protobufjs` | 175388.5 | 427.1 | 176945.4 | 423.4 |

The reading: building a `Message` through `set()` and reading one back through
`get()` is ~15x the codec's own cost at the small tiers and ~30x at 74 KB
(1583 µs against the prebuilt handle's ~50 µs), so the boundary, not the codec,
is what the native-object task pays. One downcall removes the JS side of it
(3.0 against 3.6 µs at 136 B, 1.42 against 1.53 ms at 74 KB, so the shim beats
the per-field assembly at every tier), but the addon still pays one N-API
property read or write per field (~60-70 ns) where jspb and protobufjs pay a
few ns in their own heap. That is why every tree and shim cell sits below both
baselines while the prebuilt handle row still beats them.

## Verdict

**Keep the handle path as the runtime; the shim arm stays the native-object
path and a re-runnable candidate.** The shim replaces the JS per-field assembly
(it is 1.17x at 136 B and 1.05x at 74 KB) but not the `Message` tree, and it
does not close the gap to the official runtimes: each field of the
native-object task crosses N-API once (an object property read on encode, an
object property write on decode) at ~60-70 ns, against jspb's and protobufjs's
per-field cost inside their own heap (~5 ns). One downcall per op already
removes what can be removed; Stage 6 records the two boundary changes that were
tried and why they do not close it. Publishing both rows (Stage 5) keeps the
codec number and the native-object number visible instead of quoting one for the
other.

It is not (a) because "replace the harness path" cannot mean replacing the
binding's shipped fast path: a prebuilt `Message` encodes 74 KB in ~50 µs and
decodes it in ~60 µs, 15-30x faster than either native-object row, and anyone
who re-encodes the same message keeps that path. It is not (c) because there is
no payload-size crossover: the shim is below both baselines at every tier, small
and xlarge alike.

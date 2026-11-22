# Node pbench: four arms, one contract

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
| `core` | `c-core.js` | the pure-TypeScript core (`../lib/core/`): the descriptor reader, tree and codec in JS, so the task runs in the JS heap with no boundary at all | what the wire format costs when there is no native library to call |

`Payload` is `payload.js` (one object shared by the gate and the timing loop),
the oracle is google-protobuf (jspb) generated code from the same `.proto`.

## Gate

```
bindings/node/pbench/setup.sh        # addon + benchmarks/gen (jspb + .desc)
node bindings/node/pbench/gate.js    # gate: PASS / exit 1
```

The gate asserts, per tier, that `a-tree`, `b-shim` and `core` bytes equal
jspb's bytes, that each arm's decode reads the fields `benchmarks/bench.js` checks
(`str`, `inner.name`, `inner.value`, the repeated lengths and last element, the
proto3 optionals, `u64`, `color`), and that the decoded object re-encodes to the
same bytes. It fails loudly while an arm or the `encodeObject` /
`decodeToObject` surface is missing, so no row is published before the arm
exists.

## Timing

`compare.js` interleaves `a-tree`, `b-shim`, `core`, jspb and protobufjs in one
loop per tier and reports the median of five rounds (`node pbench/compare.js`). The
formal, recorded run is `make node-bench`, which drives
`benchmarks/bench.js` and prints `pbwire-node-tree` (the full per-field task:
build a fresh handle with one `set()` per field, encode, free; decode, read
every field back with one `get()` per field, free) beside `pbwire-node-shim`
(the same task in one downcall), `pbwire-node-core` (the same task on the
pure-TS core) and `pbwire-node-proxy` (the prebuilt hold-and-recode row:
re-encode a held handle, decode-and-free with no materialization; published,
never the headline).

## Run table

Provisional (Stage 3, `node pbench/compare.js`, median of five interleaved
rounds, 2026-11-21 under load average ~33): `tree` is the whole user task
(build the handle with one `set()` per field, then encode; decode, then read
every field back), `shim` is one `encodeObject` / `decodeToObject` call. The
prebuilt-handle row `make node-bench` publishes as `pbwire-node-tree` is a
different op and is not in this table.

| tier | arm | encode ns/op | encode MB/s | decode ns/op | decode MB/s |
|---|---|---|---|---|---|
| 136 B | `pbwire-node-core` | 2049.4 | 66.4 | 1449.5 | 93.8 |
| 136 B | `pbwire-node-tree` | 3485.0 | 39.0 | 3493.8 | 38.9 |
| 136 B | `pbwire-node-shim` | 2695.0 | 50.5 | 2190.3 | 62.1 |
| 136 B | `google-protobuf` | 2321.6 | 58.6 | 1134.5 | 119.9 |
| 136 B | `protobufjs` | 550.4 | 247.1 | 347.4 | 391.4 |
| 174 B | `pbwire-node-core` | 2401.9 | 72.4 | 2290.7 | 76.0 |
| 174 B | `pbwire-node-tree` | 4048.3 | 43.0 | 4737.7 | 36.7 |
| 174 B | `pbwire-node-shim` | 3262.4 | 53.3 | 3528.0 | 49.3 |
| 174 B | `google-protobuf` | 2735.6 | 63.6 | 1563.6 | 111.3 |
| 174 B | `protobufjs` | 641.3 | 271.3 | 435.3 | 399.7 |
| 2469 B | `pbwire-node-core` | 16830.9 | 146.7 | 32100.0 | 76.9 |
| 2469 B | `pbwire-node-tree` | 51871.2 | 47.6 | 55436.1 | 44.5 |
| 2469 B | `pbwire-node-shim` | 44551.4 | 55.4 | 49946.0 | 49.4 |
| 2469 B | `google-protobuf` | 42264.5 | 58.4 | 30766.9 | 80.2 |
| 2469 B | `protobufjs` | 6546.5 | 377.2 | 5949.2 | 415.0 |
| 74913 B | `pbwire-node-core` | 434607.8 | 172.4 | 853442.5 | 87.8 |
| 74913 B | `pbwire-node-tree` | 1484171.4 | 50.5 | 1480953.8 | 50.6 |
| 74913 B | `pbwire-node-shim` | 1253680.9 | 59.8 | 1376278.4 | 54.4 |
| 74913 B | `google-protobuf` | 1276967.8 | 58.7 | 893244.5 | 83.9 |
| 74913 B | `protobufjs` | 172487.7 | 434.3 | 171038.0 | 438.0 |
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

The 2026-11-21 plan-15 rows above are the same run (`compare.js`, five
interleaved rounds, load average ~22); the plan-04 rows from the first table
run are kept below for the tree/shim history. The reading: building a
`Message` through `set()` and reading one back through
`get()` is ~15x the codec's own cost at the small tiers and ~30x at 74 KB
(1583 µs against the prebuilt handle's ~50 µs), so the boundary, not the codec,
is what the native-object task pays. One downcall removes the JS side of it
(3.0 against 3.6 µs at 136 B, 1.42 against 1.53 ms at 74 KB, so the shim beats
the per-field assembly at every tier), but the addon still pays one N-API
property read or write per field (~60-70 ns) where jspb and protobufjs pay a
few ns in their own heap. That is why every tree and shim cell sits below both
baselines while the prebuilt handle row still beats them.

## Verdict

**Ships: the addon. The tree arm (shipped per-field handle API) stays the
documented default path; the shim arm (`encodeObject` / `decodeToObject`) is
the recommended one-shot path for fresh native objects.** Formal run
`results/bench-node-20261121T181411Z.json` (`make node-bench`, Go-parity
timers: fresh handle per iteration, freed inside the timer, every gate-checked
field read back; xlarge = 74913 B):

| xlarge arm | encode MB/s (x jspb / x protobufjs) | decode MB/s (x jspb / x protobufjs) |
|---|---|---|
| `tree` | 51.1 (0.88x / 0.10x) | 50.2 (0.60x / 0.12x) |
| `shim` | 58.9 (1.02x / 0.12x) | 53.4 (0.64x / 0.13x) |
| `core` | 79.4 (1.37x / 0.16x) | 79.4 (0.95x / 0.19x) |
| `proxy` | 1723.3 (prebuilt, never headline) | 1229.0 (prebuilt, never headline) |

Ratios are THROUGHPUT (MB/s pbwire / MB/s baseline, higher is faster), formal
run `results/bench-node-20261121T181411Z.json`.

Why tree stays the default: it is the only arm with JSON/text mappings, the
only one whose decode is the C core's single implementation, and the only path
that reuses a held tree (the proxy row: 43.5 us encode at xlarge, 33.7x the
tree encode). The shim beats per-field assembly at every tier (1.27x at 136 B
encode,
1.15x at xlarge encode) and beats jspb on xlarge encode (1.02x), so one-shot
encode/decode of a fresh object should go through the one-downcall surface; it
stays a recommendation rather than the default because each field still crosses
N-API once (~60-70 ns per property read/write) where jspb and protobufjs pay a
few ns in their own heap, leaving every native row below both baselines.

Why not core as the default: the pure-TS core beats jspb on xlarge encode
(1.37x) and stays just below it on decode (0.95x), but shipping it as the
second wire implementation on the default path, which `AGENTS.md` rules out. It
is what ships to a host with no toolchain to build the addon (plain Node with
no C compiler, an electron/bundled runtime, or a non-Node JS engine), where the
alternative is not the addon but nothing.

Why not proxy as the headline: it times data already in pbwire, not what an
application pays. It is published (the transcoder task is real) and never
quoted against a native row.

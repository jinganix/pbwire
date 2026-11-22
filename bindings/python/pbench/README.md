# Python pbench: four arms, one contract

The arms, the gate and the timing loop from
[`bindings/README.md`](../../README.md#pbench-three-arms-one-contract), applied
to `structtest.Scalars3` from `bindings/c/tests/data/matrix3.proto` and the four
standard tiers. Every arm does the same user-visible task: a native object (a
plain `dict`, proto field names, Python `int` for every integer width) in and
wire bytes out; wire bytes in and a fully readable `dict` out.

| arm | entry point | mechanism | what it answers |
|---|---|---|---|
| `tree` | `pbench/tree.py`: `marshal(dict)` / `unmarshal(wire)` | the shipped per-field full task: `encode(dict)`; `decode(bytes)`, then one `get()` per field into a fresh dict (nested message and repeated messages read back field by field). No prebuilt handle before the timer; CPython's GC owns the handle (no manual free exists on this path) | what an application pays through the per-field API |
| `shim` | `type.encode(dict)` / `type.decode(bytes).to_dict()` | one CPython downcall per op, the C extension walks the dict against the schema's field table (encode) and the tree (decode) | is the boundary worth paying for at all |
| `core` | `type.encode_object(dict)` / `type.decode_object(bytes)` | the pure-Python core (`../pure/`): descriptor reader, tree and codec in Python, so the task runs in the interpreter with no native library at all | what the wire format costs when the extension cannot be loaded |
| `proxy` | `type.encode(message)` / `type.decode(bytes)` on a handle built before the timer | re-encode a held tree, decode-and-free with no materialization | the codec's own cost once the tree exists -- published, never the headline |

The native object is the same dict `benchmarks/bench.py` already builds
(`matrix3.txt` values), the oracle is `google.protobuf` generated code from the
same `.proto`, and the reference bytes are protoc's.

## Gate

```
make python-test        # the extension's own check, unchanged
make python-bench       # gate runs first: bytes equality + field compare, per tier
```

`bench.py` refuses to print a row before the arm behind it is correct: every
arm's bytes must equal protoc's on all four tiers, every decode must read the
fields the harness checks (`str`, `inner`, `inner.name`, `inner.value`, the
repeated lengths and their last elements, the proto3 optionals, `u64`,
`color`), and a decoded tree must re-encode byte-exactly -- including the
tree arm's own materialized dict (`encode(unmarshal(wire)) == wire`), so no
row is published before the full task round-trips. A mismatch prints `FAIL`
and exits 1. The same core is what `make conformance-python-pure` runs the
official binary suite through.

## Run table

Formal (`make python-bench`, `results/bench-python-20261121T183716Z.json`,
74913 B xlarge tier, throughput ratios = pbwire MB/s over `pb upb` MB/s):

| arm | encode ns/op | vs upb enc | decode ns/op | vs upb dec |
|---|---|---|---|---|
| `tree` (full task) | 131381.6 | 7.19x | 1168445.8 | 1.57x |
| `shim` (one downcall) | 145161.0 | 6.51x | 501795.4 | 3.65x |
| `core` (pure Python) | 7282435.5 | 0.13x | 11544691.3 | 0.16x |
| `proxy` (prebuilt) | 840.0 | 60.92x | 82215.8 | 1.15x |

Decode compares against the matching upb task: `tree`/`shim` `to_dict(bytes)`
against upb `to_dict(bytes)` (parse plus full materialization), `proxy`
`decode(bytes)` against upb `decode(bytes)` (parse only). Proxy/tree gap at
xlarge: 156.4x encode, 14.2x decode on the full-task side (proxy `encode(parsed)`
vs tree `encode(data)`; proxy bare decode vs tree `to_dict`). Tree/shim encode
converge (0.90x-1.10x across tiers): encode is one C downcall either way, so
the per-field shape costs nothing extra on the way in; the tree tax is all on
decode (2.3x at xlarge), one boundary crossing per `get()`.

## Timing

`make python-bench` (`bindings/python/benchmarks/bench.py bench`) is the formal,
recorded run: it prints `pbwire-py-tree` (the per-field full task from
`pbench/tree.py`, which `bench.py` reuses directly), `pbwire-py-shim` (one
downcall per op), `pbwire-py-proxy` (the old prebuilt rows, relabelled not
re-measured as a new task) and `pbwire-py-core` next to upb (`pb upb`).
`results/` keeps the JSON the harness writes; it is not committed.

The pure core is the no-extension path (pure-Python mode, PyPy, a sandboxed
interpreter), not the fast one: the extension stays the shipped codec. Its
numbers are published beside the extension's so the cost of dropping the native
library is visible, never as a headline.

## Verdict

The CPython extension stays the shipped codec for the Python binding, and the
`shim` shape (one downcall per op: `encode(dict)` / `decode(bytes).to_dict()`)
is the recommended application path: it is the fastest native-in/native-out
shape on every tier (3.65x upb on xlarge decode; tree pays 2.3x for per-field
`get()` crossings), and it is also the simplest. The `tree` arm stays published
beside it as the per-field-API reference -- it is the headline-adjacent
application row, never replaced by `proxy`. The pure core ships as the
no-extension mode (pure-Python interpreters, PyPy, sandboxed runtimes), correct
at `make conformance-python-pure` (1412 successes, 1403 JSON/text skipped,
2 expected failures, 0 unexpected -- rerun on this task's tree), published
never headlined: it is 0.13x-0.16x upb on the 74 KB task because every value
becomes a Python object in the interpreter, not because the wire walk is wrong.
`proxy` is published with the held-tree price (156.4x encode gap at xlarge),
never the headline.

No optimization in this task: every native arm (`tree`, `shim`) is above
baseline on every tier, so Task 15 has no Python cell.

The reason it is not the default is arithmetic, not bugs. On the same 74 KB
payload the `proxy` path (prebuilt handle, formal
`results/bench-python-20261121T183716Z.json` xlarge) is 911.2 MB/s decode /
89182.7 MB/s `encode(parsed)`, and upb 793.9 / 1463.9 on the matching ops,
while the pure core is 6.5 / 10.3 (`to_dict` / `encode(data)`): every value becomes a
Python object (`int`, `str`, `Raw`, `dict` slot), and that materialization, not
the wire walk, is what costs. Against protobuf's *pure-Python* runtime, the
comparable alternative when no C extension can load, the core decodes 1.10x
faster (6.5 vs 6.2 MB/s) and is in the same class on encode (10.3 MB/s
`encode(dict)`, which builds the tree first, vs 15.0 MB/s
`SerializeToString` on an already-parsed message). So the pure core is a real
replacement for the no-extension case and no replacement for the extension.

Speed honesty: the row is printed as `pbwire-py-core` beside the extension rows
and never described as fast. `encode(parsed)` on the tree path is the one place
it wins outright (a decoded tree carries raw slices, so a string field is
copied, not re-encoded), which is why the handle path remains the shipped API.

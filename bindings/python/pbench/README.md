# Python pbench: three arms, one contract

The arms, the gate and the timing loop from
[`bindings/README.md`](../../README.md#pbench-three-arms-one-contract), applied
to `structtest.Scalars3` from `bindings/c/tests/data/matrix3.proto` and the four
standard tiers. Every arm does the same user-visible task: a native object (a
plain `dict`, proto field names, Python `int` for every integer width) in and
wire bytes out; wire bytes in and a fully readable `dict` out.

| arm | entry point | mechanism | what it answers |
|---|---|---|---|
| `tree` | `type.decode(bytes)` / `type.encode(message)` | the shipped handle path: one tag scan records a slice per wire field (`Message`), `encode(message)` writes the tree | what the codec costs once the tree exists |
| `shim` | `type.encode(dict)` / `type.decode(bytes).to_dict()` | one CPython downcall per op, the C extension walks the dict against the schema's field table | is the boundary worth paying for at all |
| `core` | `type.encode_object(dict)` / `type.decode_object(bytes)` | the pure-Python core (`../pure/`): descriptor reader, tree and codec in Python, so the task runs in the interpreter with no native library at all | what the wire format costs when the extension cannot be loaded |

The native object is the same dict `benchmarks/bench.py` already builds
(`matrix3.txt` values), the oracle is `google.protobuf` generated code from the
same `.proto`, and the reference bytes are protoc's.

## Gate

```
make python-test        # the extension's own check, unchanged
make python-bench       # gate runs first: bytes equality + field compare, per tier
```

`bench.py` refuses to print a `pbwire-py-core` row before the pure core
reproduces the reference bytes on all four tiers and its decode reads the fields
the harness checks (`str`, `inner`, `inner.name`, `inner.value`, the repeated
lengths and their last elements, the proto3 optionals, `u64`, `color`). A
mismatch prints `FAIL` and exits 1, so no row is published before the arm is
correct. The same core is what `make conformance-python-pure` runs the official
binary suite through.

## Run table

Provisional (Stage 3, `python3 bindings/python/pbench/compare.py --rounds 3`,
best of three interleaved rounds per cell, 2026-11-21, busy machine): the
`core` arm is the whole user task -- `encode_object(dict)` builds the tree and
writes it, `decode_object(bytes)` parses and reads every field into dicts.

| tier | `pbwire-py-core` encode ns/op | encode MB/s | decode ns/op | decode MB/s |
|---|---|---|---|---|
| 136 B | 15483.6 | 8.8 | 19186.4 | 7.1 |
| 174 B | 19670.1 | 8.8 | 24934.2 | 7.0 |
| 2469 B | 241020.3 | 10.2 | 360992.2 | 6.8 |
| 74913 B | 7167944.4 | 10.5 | 10980368.0 | 6.8 |

The same run, for scale: `pbwire-py-tree` (the extension's handle path)
953.8 MB/s decode / 96092.3 MB/s encode at 74 KB, `pbwire-py-shim` (one
downcall, dict in and out) 162.8 / 608.4, upb 806.7 / 514.9. The comparable
baseline for a pure-Python core is protobuf's own pure-Python runtime: same
74913 B payload in a separate process, 6.2 MB/s decode and 15.0 MB/s
serialize-from-parsed -- so the port is 1.10x that parser on decode and in the
same class, 100x behind the extension because the interpreter, not the wire
engine, is what it pays for.

## Timing

`make python-bench` (`bindings/python/benchmarks/bench.py bench`) is the formal,
recorded run: it prints `pbwire-py` (the extension's dict and message paths) and
`pb addressbook` (upb) as before, and `pbwire-py-core` next to them for the pure
core's encode(data) / decode to dict task. `results/` keeps the JSON the harness
writes; it is not committed.

The pure core is the no-extension path (pure-Python mode, PyPy, a sandboxed
interpreter), not the fast one: the extension stays the shipped codec. Its
numbers are published beside the extension's so the cost of dropping the native
library is visible, never as a headline.

## Verdict

The CPython extension stays the shipped codec for the Python binding; the pure core
ships as its no-extension mode (pure-Python interpreters, PyPy, sandboxed
runtimes), which is what the official runtime offers too. The pure core is
correct at the only contract that matters -- `make conformance-python-pure`:
1412 successes, 1403 JSON/text skipped, 2 expected failures, 0 unexpected, the
same failure set as the C core, the TS port and the C# port -- so it is
published, never headlined.

The reason it is not the default is arithmetic, not bugs. On the same 74 KB
payload the extension's handle path is 953.8 MB/s decode / 96092.3 MB/s encode
and upb 806.7 / 514.9, while the pure core is 6.8 / 10.5: every value becomes a
Python object (`int`, `str`, `Raw`, `dict` slot), and that materialization, not
the wire walk, is what costs. Against protobuf's *pure-Python* runtime, the
comparable alternative when no C extension can load, the core decodes 1.10x
faster (6.8 vs 6.2 MB/s) and is in the same class on encode (10.5 MB/s
`encode(dict)`, which builds the tree first, vs 15.0 MB/s
`SerializeToString` on an already-parsed message). So the pure core is a real
replacement for the no-extension case and no replacement for the extension.

Speed honesty: the row is printed as `pbwire-py-core` beside the extension rows
and never described as fast. `encode(parsed)` on the tree path is the one place
it wins outright (a decoded tree carries raw slices, so a string field is
copied, not re-encoded), which is why the handle path remains the shipped API.

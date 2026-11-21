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

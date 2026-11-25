"""Python binding benchmark: the pbwire CPython extension vs google.protobuf.

Both sides use the same `bindings/c/tests/data/matrix3.proto` message (every
scalar kind, string, enum, nested message, three repeated kinds, proto3
optional) and the four standard tiers from BENCHMARKS.md: the repeated fields
scale x0 / x1 / x100 / x3000.

The rows compare the same user-visible task, not the same internal call.
Four pbwire rows, named by the mechanism they time. The first three time
the same native-in/native-out task; the fourth is the prebuilt handle:

    pbwire-py-tree   a native dict in and out through the shipped per-field
                     API: encode(dict); decode, then read every field back
                     with one get() per field into a fresh dict (the pbench
                     tree arm). No prebuilt handle before the timer.
    pbwire-py-shim   the same dict in and out in one downcall per op: the C
                     extension walks the dict (encode) and the tree
                     (decode().to_dict()).
    pbwire-py-core   the pure-Python core (bindings/python/pure/, no CPython
                     extension), timed on the same task (see below).
    pbwire-py-proxy  encode/decode of a Message handle built (and read)
                     before the timer: the codec's own cost once the tree
                     exists. "The data is already in pbwire." Published,
                     never the headline.

    encode(data)    Python values -> wire bytes
                    tree/shim: encode(dict)
                    upb:    populate a message from the dict, then serialize
    to_dict(bytes)  wire bytes -> plain Python data
                    tree: decode(bytes), then one get() per field
                    shim: decode(bytes).to_dict()
                    upb:    FromString, then read every field into dicts/lists
    decode(bytes)   wire bytes -> in-memory message (fields read on demand)
                    proxy: decode(bytes), no materialization
                    upb:    Scalars3.FromString(bytes)
    encode(parsed)  in-memory message -> wire bytes
                    proxy: encode(message)   (a parsed message keeps its bytes)
                    upb:    message.SerializeToString()

`to_dict(bytes)` deliberately does not use `json_format.MessageToDict`: that
goes through JSON-shaped intermediate values and is ~4x slower than reading the
fields, so it would not measure the same thing.

The `pbwire-py-core` rows are the pure-Python core (`bindings/python/pure/`, no
CPython extension, no `google.protobuf`), timed on the same task:

    encode(data)    encode_object(dict): build the tree, then write the wire
    to_dict(bytes)  decode_object(bytes): parse, then read every field
    encode(parsed)  encode(tree): the tree a decode already built

They are gated first -- the core's bytes must equal protoc's, its decode must
equal the same dict, and a decoded tree must re-encode byte-exactly -- so no
row is printed before the arm is correct. It is the no-extension path and is
never the fast one; see `bindings/python/pbench/README.md`.

The baseline row names the protobuf implementation that is actually loaded
(`pb upb`: protobuf's C extension). The generated `matrix3_pb2` module comes
from `protoc --python_out` (see the Makefile).

    make python-bench
"""

import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))          # pbwire extension
sys.path.insert(0, os.path.join(HERE, "gen"))         # matrix3_pb2

import pbwire as pbw
from google.protobuf.internal import api_implementation
import matrix3_pb2

sys.path.insert(0, os.path.join(HERE, "..", "pbench"))   # the pure core arm
import core as pure_core  # noqa: E402
import tree as tree_arm  # noqa: E402

TIERS = (("small", 0, 136), ("medium", 1, 174), ("large", 100, 2469), ("xlarge", 3000, 74913))
DESC = "bindings/c/tests/data/matrix3.desc"

REPEATED = ("nums", "tags", "flags")


def scalars3(repeats):
    """The populated matrix3 message as a dict (mirrors matrix3.txt)."""
    return {
        "i32": -2147483648,
        "i64": 9223372036854775807,
        "u32": 4294967295,
        "u64": 18446744073709551615,
        "s32": -2147483648,
        "s64": -9223372036854775808,
        "fx32": 3735928559,
        "fx64": 72623859790382856,
        "sfx32": -2147483648,
        "sfx64": -9223372036854775808,
        "f32": float("inf"),
        "f64": -123456.789,
        "flag": True,
        "str": "hello",
        "color": matrix3_pb2.COLOR3_GREEN,
        "inner": {"name": "kid", "value": 42},
        "nums": [i - 3 for i in range(repeats)],
        "tags": ["tag"] * repeats,
        "inners": [{"name": "grandkid", "value": i + 1} for i in range(repeats)],
        "opt_i32": -7,
        "opt_str": "opt",
        "flags": [i % 2 == 0 for i in range(repeats)],
    }


def pruned(values):
    """Empty repeated fields encode to nothing, so they decode to nothing."""
    return {name: value for name, value in values.items()
            if not (isinstance(value, list) and not value)}


def protobuf_of(values):
    """Fill a generated message from the same dict: the upb side of
    encode(data)."""
    message = matrix3_pb2.Scalars3()
    for name, value in values.items():
        if name in REPEATED:
            getattr(message, name).extend(value)
        elif name == "inners":
            for item in value:
                kid = message.inners.add()
                kid.name = item["name"]
                kid.value = item["value"]
        elif name == "inner":
            message.inner.name = value["name"]
            message.inner.value = value["value"]
        else:
            setattr(message, name, value)
    return message


def protobuf_data(message):
    """Every field of a generated message as plain dicts/lists/scalars: the upb
    side of to_dict(bytes)."""
    out = {}
    for field, value in message.ListFields():
        if field.is_repeated:
            if field.message_type is not None:
                out[field.name] = [protobuf_data(item) for item in value]
            else:
                out[field.name] = list(value)
        elif field.message_type is not None:
            out[field.name] = protobuf_data(value)
        else:
            out[field.name] = value
    return out


# Shared measurement standard (see BENCHMARKS.md): from the tier floor (which
# is 1 here: every op is calibrated), double until one pass takes at least
# 0.25 s, then report the best of 3 timed passes after one warm-up pass.
def time_it(fn, target=0.25, minimum=0.25):
    """ns per operation, with the iteration count calibrated to `target`."""
    iterations = 1
    while True:
        start = time.perf_counter()
        for _ in range(iterations):
            fn()
        elapsed = time.perf_counter() - start
        if elapsed >= minimum:
            break
        iterations *= 2
    iterations = max(1, int(target / elapsed * iterations))
    for _ in range(iterations):  # warm-up pass
        fn()
    best = float("inf")
    for _ in range(3):
        start = time.perf_counter()
        for _ in range(iterations):
            fn()
        elapsed = time.perf_counter() - start
        best = min(best, elapsed / iterations * 1e9)
    return best, iterations


def fail(what, label):
    print("FAIL %s: %s" % (label, what))
    return 1


def run():
    base_label = "pb %s" % api_implementation.Type()
    schema = pbw.Schema.from_file(DESC)
    scalars = schema.message("structtest.Scalars3")
    status = 0

    for label, repeats, want in TIERS:
        values = scalars3(repeats)
        want_dict = pruned(values)
        reference = protobuf_of(values).SerializeToString()

        if len(reference) != want:
            return fail("official encoder wrote %d bytes, expected %d" % (len(reference), want), label)
        if scalars.encode(values) != reference:
            return fail("pbwire-py-shim bytes differ from protobuf", label)
        if scalars.decode(reference).to_dict() != want_dict:
            return fail("pbwire-py-shim to_dict differs", label)
        if scalars.encode(scalars.decode(reference)) != reference:
            return fail("pbwire-py-proxy Message round trip differs", label)
        # The tree arm is gated before it is timed: same bytes through the
        # per-field full task, same fields back, byte-exact round trip of
        # the materialized dict.
        if tree_arm.marshal(values) != reference:
            return fail("pbwire-py-tree bytes differ from protobuf", label)
        if tree_arm.unmarshal(reference) != want_dict:
            return fail("pbwire-py-tree field compare differs", label)
        if scalars.encode(tree_arm.unmarshal(reference)) != reference:
            return fail("pbwire-py-tree full-task round trip differs", label)
        if protobuf_data(matrix3_pb2.Scalars3.FromString(reference)) != want_dict:
            return fail("protobuf data differs", label)

        # The pure core is gated before it is timed: same bytes, same data,
        # byte-exact round trip.
        if pure_core.marshal(values) != reference:
            return fail("pure core bytes differ from protobuf", label)
        if pure_core.unmarshal(reference) != want_dict:
            return fail("pure core decode differs", label)
        core_message = pure_core.TYPE.decode(reference)
        if pure_core.TYPE.encode(core_message) != reference:
            return fail("pure core tree round trip differs", label)

        message = scalars.decode(reference)
        pb_message = matrix3_pb2.Scalars3.FromString(reference)

        # Headline encode(data): fresh native dict per iteration inside the
        # timer (message preparation included), on both sides. Proxy /
        # parsed rows below stay on their prebuilt handles.
        rows = [
            ("pbwire-py-tree", "encode(data)",
             time_it(lambda: tree_arm.marshal(scalars3(repeats)))[0]),
            ("pbwire-py-shim", "encode(data)",
             time_it(lambda: scalars.encode(scalars3(repeats)))[0]),
            (base_label, "encode(data)",
             time_it(lambda: protobuf_of(scalars3(repeats)).SerializeToString())[0]),
            ("pbwire-py-proxy", "decode(bytes)",
             time_it(lambda: scalars.decode(reference))[0]),
            (base_label, "decode(bytes)",
             time_it(lambda: matrix3_pb2.Scalars3.FromString(reference))[0]),
            ("pbwire-py-tree", "to_dict(bytes)",
             time_it(lambda: tree_arm.unmarshal(reference))[0]),
            ("pbwire-py-shim", "to_dict(bytes)",
             time_it(lambda: scalars.decode(reference).to_dict())[0]),
            (pure_core.NAME, "encode(data)",
             time_it(lambda: pure_core.marshal(scalars3(repeats)))[0]),
            (pure_core.NAME, "to_dict(bytes)",
             time_it(lambda: pure_core.unmarshal(reference))[0]),
            (pure_core.NAME, "encode(parsed)",
             time_it(lambda: pure_core.TYPE.encode(core_message))[0]),
            (base_label, "to_dict(bytes)",
             time_it(lambda: protobuf_data(
                 matrix3_pb2.Scalars3.FromString(reference)))[0]),
            ("pbwire-py-proxy", "encode(parsed)",
             time_it(lambda: scalars.encode(message))[0]),
            (base_label, "encode(parsed)",
             time_it(lambda: pb_message.SerializeToString())[0]),
        ]

        print("\n%s: %d bytes, repeat field x%d" % (label, len(reference),
                                                    repeats))
        print("  %-12s %-15s %10s %10s" % ("impl", "op", "ns/op", "MB/s"))
        for impl, op, ns in rows:
            mbps = len(reference) / ns * 1e3
            print("  %-12s %-15s %10.1f %10.1f" % (impl, op, ns, mbps))
    return status


def load_core(path):
    """Imports one pure-core module by file path (the A/B switch)."""
    import importlib.util

    name = "pbwire_py_core_" + os.path.basename(os.path.dirname(path))
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def ab(argv):
    """A/B probe for the pure core: two module paths, interleaved rounds.

        bench.py ab bindings/python/pbench/core.py /tmp/core-v1.py

    Both modules must expose NAME, marshal(dict), unmarshal(wire) and a TYPE
    with encode/decode. Every round times both sides back to back, so a
    drifting machine hits them equally; a candidate is only kept when it wins
    in every round. Bytes must stay identical.
    """
    if len(argv) < 3:
        print("usage: ab <base.py> <cand.py> [rounds]")
        return 2
    base = load_core(argv[1])
    cand = load_core(argv[2])
    rounds = int(argv[3]) if len(argv) > 3 else 3
    tiers = TIERS[-2:]
    status = 0
    for label, repeats, want in tiers:
        values = scalars3(repeats)
        wire = base.marshal(values)
        if len(wire) != want or cand.marshal(values) != wire:
            print("FAIL %s: base and candidate bytes differ" % label)
            return 1
        base_message = base.TYPE.decode(wire)
        cand_message = cand.TYPE.decode(wire)
        ops = [
            ("encode(data)", lambda m: m.marshal(values)),
            ("to_dict(bytes)", lambda m: m.unmarshal(wire)),
            ("encode(parsed)", lambda m: m.TYPE.encode(m is base and base_message or cand_message)),
        ]
        print("\n%s: %d bytes, %d interleaved rounds" % (label, len(wire), rounds))
        print("  %-20s %-16s %10s %10s" % ("impl", "op", "ns/op", "MB/s"))
        for op, fn in ops:
            best = {base.NAME: float("inf"), cand.NAME: float("inf")}
            for _ in range(rounds):
                for module in (base, cand):
                    ns = time_it(lambda: fn(module))[0]
                    best[module.NAME] = min(best[module.NAME], ns)
            for module in (base, cand):
                ns = best[module.NAME]
                print("  %-20s %-16s %10.1f %10.1f" % (module.NAME, op, ns, len(wire) / ns * 1e3))
            ratio = best[base.NAME] / best[cand.NAME]
            print("  %-20s %-16s %10s %.3fx" % ("", "candidate", "", ratio))
    return status


def main(argv):
    if len(argv) >= 2 and argv[1] == "ab":
        return ab(argv[1:])
    if len(argv) < 2 or argv[1] != "bench":
        print("usage: %s bench | ab <base.py> <cand.py> [rounds]" % argv[0])
        return 2
    return run()


if __name__ == "__main__":
    sys.exit(main(sys.argv))

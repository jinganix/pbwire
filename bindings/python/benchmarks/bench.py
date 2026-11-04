"""Python binding benchmark: the pbwire CPython extension vs google.protobuf.

Both sides use the same `bindings/c/tests/data/matrix3.proto` message (every
scalar kind, string, enum, nested message, three repeated kinds, proto3
optional) and the four standard tiers from BENCHMARKS.md: the repeated fields
scale x0 / x1 / x100 / x3000.

The rows compare the same user-visible task, not the same internal call:

    encode(data)    Python values -> wire bytes
                    pbwire: encode(dict)
                    upb:    populate a message from the dict, then serialize
    decode(bytes)   wire bytes -> in-memory message (fields read on demand)
                    pbwire: decode(bytes)
                    upb:    Scalars3.FromString(bytes)
    to_dict(bytes)  wire bytes -> plain Python data
                    pbwire: decode(bytes).to_dict()
                    upb:    FromString, then read every field into dicts/lists
    encode(parsed)  in-memory message -> wire bytes
                    pbwire: encode(message)   (a parsed message keeps its bytes)
                    upb:    message.SerializeToString()

`to_dict(bytes)` deliberately does not use `json_format.MessageToDict`: that
goes through JSON-shaped intermediate values and is ~4x slower than reading the
fields, so it would not measure the same thing.

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
            return fail("pbwire-py bytes differ from protobuf", label)
        if scalars.decode(reference).to_dict() != want_dict:
            return fail("pbwire-py to_dict differs", label)
        if scalars.encode(scalars.decode(reference)) != reference:
            return fail("pbwire-py Message round trip differs", label)
        if protobuf_data(matrix3_pb2.Scalars3.FromString(reference)) != want_dict:
            return fail("protobuf data differs", label)

        message = scalars.decode(reference)
        pb_message = matrix3_pb2.Scalars3.FromString(reference)

        rows = [
            ("pbwire-py", "encode(data)",
             time_it(lambda: scalars.encode(values))[0]),
            (base_label, "encode(data)",
             time_it(lambda: protobuf_of(values).SerializeToString())[0]),
            ("pbwire-py", "decode(bytes)",
             time_it(lambda: scalars.decode(reference))[0]),
            (base_label, "decode(bytes)",
             time_it(lambda: matrix3_pb2.Scalars3.FromString(reference))[0]),
            ("pbwire-py", "to_dict(bytes)",
             time_it(lambda: scalars.decode(reference).to_dict())[0]),
            (base_label, "to_dict(bytes)",
             time_it(lambda: protobuf_data(
                 matrix3_pb2.Scalars3.FromString(reference)))[0]),
            ("pbwire-py", "encode(parsed)",
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


def main(argv):
    if len(argv) < 2 or argv[1] != "bench":
        print("usage: %s bench" % argv[0])
        return 2
    return run()


if __name__ == "__main__":
    sys.exit(main(sys.argv))

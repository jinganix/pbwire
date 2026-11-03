"""Python binding benchmark: the pbwire CPython extension vs google.protobuf.

Both sides use the same `bindings/c/tests/data/matrix3.proto` message (every
scalar kind, string, enum, nested message, three repeated kinds, proto3
optional) and the four standard tiers from BENCHMARKS.md: the repeated fields
scale x0 / x1 / x100 / x3000.

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

TIERS = (("small", 0), ("medium", 1), ("large", 100), ("xlarge", 3000))
DESC = "bindings/c/tests/data/matrix3.desc"


def scalars3(repeats):
    """The populated matrix3 message as a dict (mirrors matrix3.txt)."""
    values = {
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
    return values


def pruned(values):
    """Empty repeated fields encode to nothing, so they decode to nothing."""
    return {name: value for name, value in values.items()
            if not (isinstance(value, list) and not value)}


def protobuf_of(values):
    message = matrix3_pb2.Scalars3()
    for name, value in values.items():
        if name in ("nums", "tags", "flags"):
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


def time_it(fn, target=0.6, minimum=0.15):
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
    start = time.perf_counter()
    for _ in range(iterations):
        fn()
    elapsed = time.perf_counter() - start
    return elapsed / iterations * 1e9, iterations


def run():
    base_label = "pb %s" % api_implementation.Type()
    schema = pbw.Schema.from_file(DESC)
    scalars = schema.message("structtest.Scalars3")

    for label, repeats in TIERS:
        values = scalars3(repeats)
        reference_msg = protobuf_of(values)
        reference = reference_msg.SerializeToString()

        got = scalars.encode(values)
        if got != reference:
            print("FAIL %s: pbwire-py bytes differ from protobuf (%d vs %d)"
                  % (label, len(got), len(reference)))
            return 1
        if scalars.decode(reference) != pruned(values):
            print("FAIL %s: pbwire-py decode differs" % label)
            return 1

        rows = []
        ns, _ = time_it(lambda: scalars.encode(values))
        rows.append(("pbwire-py", "encode", ns))
        ns, _ = time_it(lambda: scalars.decode(reference))
        rows.append(("pbwire-py", "decode", ns))
        ns, _ = time_it(lambda: reference_msg.SerializeToString())
        rows.append((base_label, "encode", ns))
        ns, _ = time_it(lambda: matrix3_pb2.Scalars3.FromString(reference))
        rows.append((base_label, "decode", ns))

        print("\n%s: %d bytes, repeat field x%d" % (label, len(reference),
                                                    repeats))
        print("  %-10s %-7s %10s %10s" % ("impl", "op", "ns/op", "MB/s"))
        for impl, op, ns in rows:
            mbps = len(reference) / ns * 1e3
            print("  %-10s %-7s %10.1f %10.1f" % (impl, op, ns, mbps))
    schema.close()
    return 0


def main(argv):
    if len(argv) < 2 or argv[1] != "bench":
        print("usage: %s bench" % argv[0])
        return 2
    return run()


if __name__ == "__main__":
    sys.exit(main(sys.argv))

"""One payload for the Python pbench gate and timing loop: the four standard
`matrix3.proto` tiers from BENCHMARKS.md, as the plain dict every arm takes.

The values are `matrix3.txt` (the same dict `benchmarks/bench.py` builds), so
the reference bytes are protoc's and the oracle is `google.protobuf`.
"""

import os

TIERS = (
    ("small", 0, 136),
    ("medium", 1, 174),
    ("large", 100, 2469),
    ("xlarge", 3000, 74913),
)

HERE = os.path.dirname(os.path.abspath(__file__))
DESC = os.path.join(HERE, "..", "..", "..", "bindings", "c", "tests", "data", "matrix3.desc")


def scalars3(repeats):
    """The populated Scalars3 as a dict (mirrors matrix3.txt)."""
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
        "color": 2,
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


def check_fields(obj, repeats, where):
    """Field-by-field compare of a decoded dict against the payload: every arm
    is gated on this, not on bytes alone."""
    def want(name, expected):
        got = obj.get(name)
        assert got == expected, "%s: %s = %r, want %r" % (where, name, got, expected)

    want("i32", -2147483648)
    want("u64", 18446744073709551615)
    want("s64", -9223372036854775808)
    want("f32", float("inf"))
    want("str", "hello")
    want("color", 2)
    want("opt_i32", -7)
    want("opt_str", "opt")
    assert obj["inner"]["name"] == "kid" and obj["inner"]["value"] == 42
    # An empty repeated field encodes to nothing, so it decodes to absent.
    assert obj.get("nums", []) == [i - 3 for i in range(repeats)]
    assert obj.get("tags", []) == ["tag"] * repeats
    assert obj.get("flags", []) == [i % 2 == 0 for i in range(repeats)]
    inners = obj.get("inners", [])
    assert len(inners) == repeats
    if repeats:
        assert inners[-1] == {"name": "grandkid", "value": repeats}

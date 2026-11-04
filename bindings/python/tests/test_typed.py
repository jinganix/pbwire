"""Typed-conformance check for the Python binding: build matrix3.proto's
Scalars3 through the generated dataclasses, encode, and require the bytes to
equal the protoc reference (matrix3.bin). Then decode the reference and require
every field back. Same flow as every other binding's typed test.

Run from the repo root after `make python-bind`:
    python3 bindings/python/tests/test_typed.py
"""

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
sys.path.insert(0, os.path.join(HERE, "gen"))

import pbwire as pbw  # noqa: E402

import matrix3_pbwire as t3  # noqa: E402

DATA = os.path.join(HERE, "..", "..", "c", "tests", "data")
DESC = os.path.join(DATA, "matrix3.desc")
REFERENCE = os.path.join(DATA, "matrix3.bin")

checks = 0


def check(ok, what):
    global checks
    assert ok, what
    checks += 1


def build():
    kid = t3.Inner3(name="kid", value=42)
    return t3.Scalars3(
        i32=-2147483648,
        i64=9223372036854775807,
        u32=4294967295,
        u64=18446744073709551615,
        s32=-2147483648,
        s64=-9223372036854775808,
        fx32=3735928559,
        fx64=72623859790382856,
        sfx32=-2147483648,
        sfx64=-9223372036854775808,
        f32=float("inf"),
        f64=-123456.789,
        flag=True,
        str="hello",
        color=t3.Color3.COLOR3_GREEN,
        inner=kid,
        nums=[1, -5, 300],
        tags=["a", "bb"],
        inners=[t3.Inner3(name="x", value=1), t3.Inner3(name="y", value=-2)],
        opt_i32=-7,
        opt_str="opt",
        flags=[True, False, True],
    )


def main():
    with open(REFERENCE, "rb") as f:
        reference = f.read()
    with pbw.Schema.from_file(DESC) as schema:
        scalars = schema.message("structtest.Scalars3")
        inner = schema.message("structtest.Inner3")

        typed = build()
        wire = typed.encode(scalars)
        check(wire == reference, "typed encode equals the protoc reference bytes")

        back = t3.Scalars3.decode(scalars, reference)
        check(back == typed, "decode returns the same dataclass")

        check(t3.Scalars3().encode(scalars) == b"", "empty typed message is empty")

        zero = t3.Scalars3.decode(scalars, t3.Scalars3(opt_i32=0).encode(scalars))
        check(zero.opt_i32 == 0, "explicit zero presence round-trips")

        nested = t3.Scalars3(inner=t3.Inner3(name="kid", value=42))
        check(t3.Scalars3.decode(scalars, nested.encode(scalars)).inner.value == 42, "nested typed message")
        check(t3.Inner3.decode(inner, nested.inner.encode(inner)).name == "kid", "inner encodes standalone")

    print(f"test_typed: {checks} checks passed")


if __name__ == "__main__":
    main()

"""Generated-conformance check for the Python binding: build matrix3.proto's
Scalars3 through the generated dataclasses, encode, and require the bytes to
equal the protoc reference (matrix3.bin). Then decode the reference and require
every field back. Same flow as every other binding's generated test.

Run from the repo root after `make python-bind`:
    python3 bindings/python/tests/test_generated.py
"""

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
sys.path.insert(0, os.path.join(HERE, "gen"))

import pbwire as pbw  # noqa: E402

import matrix3_pbwire as t3  # noqa: E402
import matrix3_pbwire_meta as t3meta  # noqa: E402
import maptest_pbwire as tm  # noqa: E402
import oneof1_pbwire as t1  # noqa: E402
import oneof1_pbwire_meta as t1meta  # noqa: E402

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

        gen = build()
        wire = gen.encode(scalars)
        check(wire == reference, "generated encode equals the protoc reference bytes")

        back = t3.Scalars3.decode(scalars, reference)
        check(back == gen, "decode returns the same dataclass")

        check(t3.Scalars3().encode(scalars) == b"", "empty generated message is empty")

        zero = t3.Scalars3.decode(scalars, t3.Scalars3(opt_i32=0).encode(scalars))
        check(zero.opt_i32 == 0, "explicit zero presence round-trips")

        nested = t3.Scalars3(inner=t3.Inner3(name="kid", value=42))
        check(t3.Scalars3.decode(scalars, nested.encode(scalars)).inner.value == 42, "nested generated message")
        check(t3.Inner3.decode(inner, nested.inner.encode(inner)).name == "kid", "inner encodes standalone")

    # Maps ride as repeated entry structs: entry order is the wire order.
    mapdata = os.path.join(HERE, "..", "..", "..", "codegen", "testdata")
    with open(os.path.join(mapdata, "maptest.bin"), "rb") as f:
        map_reference = f.read()
    with pbw.Schema.from_file(os.path.join(mapdata, "maptest.desc")) as schema:
        maptest = schema.message("spectest.MapTest")
        gen_map = tm.MapTest(
            by_int=[tm.MapTest_ByIntEntry(key=1, value="one"),
                    tm.MapTest_ByIntEntry(key=-5, value="neg")],
            by_str=[tm.MapTest_ByStrEntry(key="a", value=10),
                    tm.MapTest_ByStrEntry(key="b", value=-3)],
            by_msg=[tm.MapTest_ByMsgEntry(key="k", value=tm.MapValue(name="n", value=42))],
        )
        wire = gen_map.encode(maptest)
        check(wire == map_reference, "generated map encode equals protoc reference")
        back = tm.MapTest.decode(maptest, map_reference)
        check(back == gen_map, "map decode returns the same dataclass")
        check(tm.MapTest().encode(maptest) == b"", "empty map message is empty")

    # Cross-file references import the sibling model instead of skipping.
    with open(os.path.join(mapdata, "xfile.bin"), "rb") as f:
        xfile_reference = f.read()
    with pbw.Schema.from_file(os.path.join(mapdata, "xfile.desc")) as schema:
        uses = schema.message("xfile.UsesDep")
        import xdep_pbwire as td  # noqa: E402
        import xfile_pbwire as tx  # noqa: E402
        gen_x = tx.UsesDep(
            single=td.DepMsg(name="one", value=7),
            many=[td.DepMsg(name="a"), td.DepMsg(name="b", value=-1)],
            note="hi",
        )
        wire = gen_x.encode(uses)
        check(wire == xfile_reference, "generated xfile encode equals protoc reference")
        back_x = tx.UsesDep.decode(uses, xfile_reference)
        check(back_x == gen_x, "xfile decode returns the same dataclass")

    # Dual registration: embedded set by default, external file via use_schema.
    t3meta.preload()
    check("structtest.Scalars3" in t3meta.MESSAGE_NAMES, "registry lists every message")
    testdata = os.path.join(HERE, "..", "..", "..", "codegen", "testdata")
    with open(os.path.join(testdata, "oneof1.bin"), "rb") as f:
        reference_oneof = f.read()
    with open(os.path.join(testdata, "oneof1.desc"), "rb") as f:
        ext = pbw.Schema.load(f.read())
    t1meta.use_schema(ext)
    try:
        back_one = t1.Oneof1.FromString(reference_oneof)
        check(back_one.name == "a" and back_one.text == "hi", "external schema decodes")
        check(back_one.SerializeToString() == reference_oneof, "external re-encodes")
    finally:
        t1meta.use_schema(None)
    check(t1.Oneof1.FromString(reference_oneof).name == "a", "cleared override restores embedded")
    t1meta.init_schema()
    check(t1.Oneof1.FromString(reference_oneof).name == "a", "init keeps embedded")

    print(f"test_generated: {checks} checks passed")


if __name__ == "__main__":
    main()

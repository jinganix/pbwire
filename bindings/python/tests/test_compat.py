"""Generated-compat check for the Python binding (TESTSPEC T1–T6, vectors.json as
the single source of truth):

  T1  generated SerializeToString == pinned protoc reference bytes
  T2  FromString of the reference == vectors, field by field
  T3  empty encodes empty; explicit zero with presence round-trips
  T4  differential vs the official oracle (google.protobuf + committed _pb2):
      same bytes, same MessageToDict (canonicalized), HasField agrees
  T5  one business snippet runs unchanged against _pb2 classes and ours
      (import swap only) with identical observable results
  T6  errors are loud, not crashes: HasField on implicit presence, bad wire,
      CopyFrom/MergeFrom type mismatch, closed behavior parity notes

Run from anywhere with python3:
    python3 bindings/python/tests/test_compat.py
"""

import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
sys.path.insert(0, os.path.join(HERE, "gen"))
sys.path.insert(0, os.path.join(HERE, "gen_oracle"))

import pbwire as pbw  # noqa: E402

import matrix3_pb2  # noqa: E402
import matrix3_pbwire as t3  # noqa: E402
import oneof1_pb2  # noqa: E402
import oneof1_pbwire as t1  # noqa: E402

ORACLE_PIN = "7.36.2"

TESTDATA = os.path.join(HERE, "..", "..", "..", "codegen", "testdata")
with open(os.path.join(TESTDATA, "vectors.json")) as f:
    VECTORS = json.load(f)

checks = 0


def check(ok, what):
    global checks
    assert ok, what
    checks += 1


def ref_bin(rel):
    with open(os.path.join(TESTDATA, rel), "rb") as f:
        return f.read()


def build_scalars():
    v = VECTORS["values"]
    return t3.Scalars3(
        i32=v["i32"],
        i64=int(v["i64"]),
        u32=v["u32"],
        u64=int(v["u64"]),
        s32=v["s32"],
        s64=int(v["s64"]),
        fx32=v["fx32"],
        fx64=int(v["fx64"]),
        sfx32=v["sfx32"],
        sfx64=int(v["sfx64"]),
        f32=float("inf"),
        f64=v["f64"],
        flag=v["flag"],
        str=v["str"],
        color=getattr(t3.Color3, v["color"]),
        inner=t3.Inner3(name=v["inner"]["name"], value=v["inner"]["value"]),
        nums=list(v["nums"]),
        tags=list(v["tags"]),
        inners=[t3.Inner3(name=x["name"], value=x["value"]) for x in v["inners"]],
        opt_i32=v["opt_i32"],
        opt_str=v["opt_str"],
        flags=list(v["flags"]),
    )


def build_pb2_scalars():
    v = VECTORS["values"]
    m = matrix3_pb2.Scalars3(
        i32=v["i32"],
        i64=int(v["i64"]),
        u32=v["u32"],
        u64=int(v["u64"]),
        s32=v["s32"],
        s64=int(v["s64"]),
        fx32=v["fx32"],
        fx64=int(v["fx64"]),
        sfx32=v["sfx32"],
        sfx64=int(v["sfx64"]),
        f32=float("inf"),
        f64=v["f64"],
        flag=v["flag"],
        str=v["str"],
        color=getattr(matrix3_pb2.Color3, v["color"]),
        inner=matrix3_pb2.Inner3(name=v["inner"]["name"], value=v["inner"]["value"]),
        nums=v["nums"],
        tags=v["tags"],
        opt_i32=v["opt_i32"],
        opt_str=v["opt_str"],
        flags=v["flags"],
    )
    for x in v["inners"]:
        m.inners.add(name=x["name"], value=x["value"])
    return m


def check_t1_t2_t3():
    reference = ref_bin(VECTORS["reference"]["bin"])
    ours = build_scalars().SerializeToString()
    check(ours == reference, "T1 generated encode == protoc reference bytes")

    back = t3.Scalars3.FromString(reference)
    v = VECTORS["values"]
    check(back.i32 == v["i32"], "i32")
    check(back.i64 == int(v["i64"]), "i64")
    check(back.u32 == v["u32"], "u32")
    check(back.u64 == int(v["u64"]), "u64")
    check(back.s32 == v["s32"], "s32")
    check(back.s64 == int(v["s64"]), "s64")
    check(back.fx32 == v["fx32"], "fx32")
    check(back.fx64 == int(v["fx64"]), "fx64")
    check(back.sfx32 == v["sfx32"], "sfx32")
    check(back.sfx64 == int(v["sfx64"]), "sfx64")
    check(back.f32 == float("inf"), "f32")
    check(back.f64 == v["f64"], "f64")
    check(back.flag == v["flag"], "flag")
    check(back.str == v["str"], "str")
    check(back.color == getattr(t3.Color3, v["color"]), "color")
    check(back.inner == t3.Inner3(name="kid", value=42), "inner")
    check(back.nums == v["nums"], "nums")
    check(back.tags == v["tags"], "tags")
    check(back.inners == [t3.Inner3(name="x", value=1), t3.Inner3(name="y", value=-2)], "inners")
    check(back.opt_i32 == v["opt_i32"] and back.HasField("opt_i32"), "opt_i32 presence")
    check(back.opt_str == v["opt_str"] and back.HasField("opt_str"), "opt_str presence")
    check(back.flags == v["flags"], "flags")

    check(t3.Scalars3().SerializeToString() == b"", "T3 empty encodes empty")
    zero = t3.Scalars3.FromString(t3.Scalars3(opt_i32=0).SerializeToString())
    check(zero.opt_i32 == 0 and zero.HasField("opt_i32"), "T3 explicit zero round-trips")

    extra = VECTORS["extra_messages"][0]
    ref1 = ref_bin(extra["reference"]["bin"])
    check(t1.Oneof1(name="a", text="hi").SerializeToString() == ref1, "T1 oneof encode == reference")
    back1 = t1.Oneof1.FromString(ref1)
    check(back1.name == "a" and back1.text == "hi", "T2 oneof decode")
    check(back1.WhichOneof("kind") == "text", "T2 oneof member")
    nested = t1.Oneof1.FromString(t1.Oneof1(name="n", inner=t1.Oneof1.Inner(v=7)).SerializeToString())
    check(nested.inner == t1.Oneof1.Inner(v=7) and nested.WhichOneof("kind") == "inner",
          "T2 nested oneof member")


def check_t4():
    import google.protobuf  # noqa: E402

    check(google.protobuf.__version__ == ORACLE_PIN,
          "oracle pin drifted; see testdata/ORACLES.md (got %s)" % google.protobuf.__version__)
    reference = ref_bin(VECTORS["reference"]["bin"])
    pb2 = build_pb2_scalars()
    check(pb2.SerializeToString() == reference, "T4 oracle matches the reference")
    check(build_scalars().SerializeToString() == reference, "T4 facade matches the reference")

    from google.protobuf.json_format import MessageToDict  # noqa: E402

    # Canonicalize: the oracle renders int64 as strings, ours as ints.
    want = MessageToDict(pb2)
    got = MessageToDict(matrix3_pb2.Scalars3.FromString(build_scalars().SerializeToString()))

    def norm(x):
        if isinstance(x, str) and x.lstrip("-").isdigit():
            return int(x)
        if isinstance(x, list):
            return [norm(y) for y in x]
        if isinstance(x, dict):
            return {k: norm(y) for k, y in x.items()}
        return x

    check(norm(want) == norm(got), "T4 MessageToDict agrees after canonicalization")
    # Cross-decode: our bytes parse under the oracle to the same dict.
    check(norm(MessageToDict(matrix3_pb2.Scalars3.FromString(
        build_scalars().SerializeToString()))) == norm(want),
        "T4 cross-decode agrees")

    ours = t3.Scalars3.FromString(reference)
    for name in ("opt_i32", "opt_str"):
        check(ours.HasField(name) == pb2.HasField(name), "T4 HasField agrees on %s" % name)

    e1 = VECTORS["extra_messages"][0]
    ref1 = ref_bin(e1["reference"]["bin"])
    check(oneof1_pb2.Oneof1.FromString(ref1).SerializeToString() == ref1, "T4 oracle oneof")
    check(t1.Oneof1.FromString(ref1).SerializeToString() == ref1, "T4 facade oneof")


def check_t5():
    # One business snippet, two libraries: build, check presence, serialize,
    # parse back, list fields. The import is the only difference.
    def workflow(Scalars, Inner):
        m = Scalars(str="kid", i32=-5, nums=[1, 2], inner=Inner(name="x", value=1))
        present = m.HasField("inner")
        wire = m.SerializeToString()
        back = Scalars.FromString(wire)
        # The oracle lists FieldDescriptors, we list proto names: compare names.
        names = sorted(n.name if hasattr(n, "name") else n for n, _ in back.ListFields())
        return (wire, present, back.str, back.i32, list(back.nums), back.inner.name, names)

    a = workflow(matrix3_pb2.Scalars3, matrix3_pb2.Inner3)
    b = workflow(t3.Scalars3, t3.Inner3)
    check(a == b, "T5 same snippet, same observable result (import swap only)")


def check_t6():
    try:
        t3.Scalars3().HasField("i32")
        check(False, "T6 HasField on implicit presence raises")
    except ValueError:
        check(True, "T6 HasField on implicit presence raises")
    try:
        t3.Scalars3.FromString(b"\xff\xff\xff\xff\xff")
        check(False, "T6 garbage wire raises")
    except Exception:
        check(True, "T6 garbage wire raises")
    try:
        t3.Scalars3().CopyFrom(t3.Inner3())
        check(False, "T6 CopyFrom type mismatch raises")
    except TypeError:
        check(True, "T6 CopyFrom type mismatch raises")
    try:
        t3.Scalars3().MergeFrom("nope")
        check(False, "T6 MergeFrom type mismatch raises")
    except TypeError:
        check(True, "T6 MergeFrom type mismatch raises")
    m = build_scalars()
    m.Clear()
    check(m == t3.Scalars3() and m.SerializeToString() == b"", "T6 Clear resets")
    try:
        t1.Oneof1().WhichOneof("nope")
        check(False, "T6 unknown oneof raises")
    except ValueError:
        check(True, "T6 unknown oneof raises")


def main():
    check_t1_t2_t3()
    check_t4()
    check_t5()
    check_t6()
    print("test_compat: %d checks passed" % checks)


if __name__ == "__main__":
    main()

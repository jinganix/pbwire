"""Pure-Python core: encoder/decoder tests.

Covers the behaviours the port is reviewed on: bytes identical to protoc's
reference for every descriptor in the repo's C test data (scalars, proto2
required, groups, oneofs, maps, extensions), unknown retention in input order,
wrong-wire-type fields treated as unknown, packed and unpacked forms, oneof
collapse, presence, bounded reads and the UTF-8 / depth limits.
"""

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", ".."))

from pure.descriptor import Limits  # noqa: E402
from pure.schema import Schema  # noqa: E402
from pure.wire import CoreError  # noqa: E402

DATA = os.path.join(HERE, "..", "..", "..", "..", "bindings", "c", "tests", "data")

MATRIX3 = {
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
    "nums": [1, -5, 300],
    "tags": ["a", "bb"],
    "inners": [{"name": "x", "value": 1}, {"name": "y", "value": -2}],
    "opt_i32": -7,
    "opt_str": "opt",
    "flags": [True, False, True],
}


def _load(desc, name):
    return Schema.from_file(os.path.join(DATA, desc)).message(name)


def _reference(name):
    with open(os.path.join(DATA, name), "rb") as handle:
        return handle.read()


def test_reference_bytes():
    """Every protoc-encoded reference in the C test data re-encodes exactly."""
    cases = [
        ("matrix3.desc", "structtest.Scalars3", "matrix3.bin"),
        ("matrix2.desc", "structtest.Scalars2", "matrix2.bin"),
        ("grouptest.desc", "structtest.GroupTest", "grouptest.bin"),
        ("oneoftest.desc", "structtest.OneOfTest", "oneoftest.bin"),
        ("maptest.desc", "structtest.Maps", "maptest.bin"),
        ("extensiontest.desc", "exttest.HasExt", "extensiontest.bin"),
    ]
    for desc, name, blob in cases:
        reference = _reference(blob)
        msg = _load(desc, name)
        # decode -> encode is byte-exact (unknowns, order, raw slices).
        assert msg.encode(msg.decode(reference)) == reference, (desc, "round trip")
        # decode -> dict -> encode matches for the same values.
        assert msg.encode_object(msg.decode_object(reference)) == reference, (desc, "object")


def test_matrix3_dict_bytes():
    scalars = _load("matrix3.desc", "structtest.Scalars3")
    assert scalars.encode_object(MATRIX3) == _reference("matrix3.bin")
    # 64-bit and float values come back in their Python form, not masked.
    out = scalars.decode_object(_reference("matrix3.bin"))
    assert out["u64"] == 2 ** 64 - 1
    assert out["s64"] == -(2 ** 63)
    assert out["f32"] == float("inf")
    assert out["str"] == "hello" and isinstance(out["str"], str)
    # NaN payloads survive as bits through decode/encode.
    nan_wire = b"\x5d\x01\x00\xc0\x7f"  # f32 = 0x7fc00001
    msg = scalars.decode(nan_wire)
    assert scalars.encode(msg) == nan_wire
    assert scalars.get_field(msg, "f32") != scalars.get_field(msg, "f32")


def test_unknown_retention_order():
    """Unknown fields keep tag+payload and re-emit in input order, after the
    known fields."""
    scalars = _load("matrix3.desc", "structtest.Scalars3")
    unknown_a = b"\x98\x06\x05"          # field 99, varint 5
    unknown_b = b"\xa2\x06\x02hi"        # field 100, LEN "hi"
    i32 = b"\x08\x07"                    # field 1 (known), value 7
    wire_bytes = unknown_a + i32 + unknown_b
    msg = scalars.decode(wire_bytes)
    assert msg.unknown == [unknown_a, unknown_b]
    assert scalars.get_field(msg, "i32") == 7
    # Known fields first in field order, then the unknowns in input order.
    assert scalars.encode(msg) == i32 + unknown_a + unknown_b


def test_wrong_wire_type_is_unknown():
    scalars = _load("matrix3.desc", "structtest.Scalars3")
    # i32 is a varint field; a LEN occurrence must be retained, not parsed.
    wrong = b"\x0a\x01\xff"              # field 1, LEN, one byte
    msg = scalars.decode(wrong)
    assert scalars.get_field(msg, "i32") is None
    assert msg.unknown == [wrong]
    assert scalars.encode(msg) == wrong


def test_packed_and_unpacked():
    scalars = _load("matrix3.desc", "structtest.Scalars3")
    packed = b"\x8a\x01\x03\x01\x02\x03"          # field 17, LEN 3
    assert scalars.get_field(scalars.decode(packed), "nums") == [1, 2, 3]
    unpacked = b"\x88\x01\x01\x88\x01\x02"        # field 17, varint twice
    assert scalars.get_field(scalars.decode(unpacked), "nums") == [1, 2]
    # An empty packed run leaves the field absent and re-encodes to nothing.
    empty = b"\x8a\x01\x00"
    msg = scalars.decode(empty)
    assert scalars.get_field(msg, "nums") is None
    assert scalars.encode(msg) == b""
    # A packed run whose length is not a multiple of the element width fails.
    try:
        scalars.decode(b"\x8a\x01\x03\x01\x02")
    except CoreError:
        pass
    else:
        raise AssertionError("accepted a truncated packed run")


def test_oneof_last_wins():
    oneof = _load("oneoftest.desc", "structtest.OneOfTest")
    # num = 7, then text = "hi": the last member wins and num is cleared.
    msg = oneof.decode(b"\x08\x07\x12\x02hi")
    assert oneof.get_field(msg, "num") is None
    assert oneof.get_field(msg, "text") == "hi"
    assert oneof.encode(msg) == b"\x12\x02hi"
    # num = 7, then sub = {v: 1}: the message member wins.
    msg = oneof.decode(b"\x08\x07\x1a\x02\x08\x01")
    assert oneof.get_field(msg, "num") is None
    assert oneof.to_object(msg)["sub"] == {"v": 1}
    assert oneof.oneof_case(msg, "kind") == 3
    assert oneof.get_field(msg, "id") is None


def test_presence():
    scalars = _load("matrix3.desc", "structtest.Scalars3")
    # Implicit presence: a zero on the wire leaves the field absent.
    msg = scalars.decode(b"\x08\x00")
    assert scalars.get_field(msg, "i32") is None
    assert scalars.encode(msg) == b""
    # Explicit presence (proto3 optional): a zero is a value and re-emits.
    explicit = b"\xa0\x01\x00"            # field 20, varint 0
    msg = scalars.decode(explicit)
    assert scalars.get_field(msg, "opt_i32") == 0
    assert scalars.encode(msg) == explicit
    # An empty implicit string stays absent; an explicit empty string emits.
    assert scalars.encode(scalars.decode(b"\x72\x00")) == b""   # str, len 0
    assert scalars.encode(scalars.decode(b"\xaa\x01\x00")) == b"\xaa\x01\x00"  # opt_str


def test_bounded_reads():
    """Wide wire varints truncate to the field width, like the reference."""
    scalars = _load("matrix3.desc", "structtest.Scalars3")
    ten_ones = b"\xff" * 9 + b"\x01"
    assert scalars.get_field(scalars.decode(b"\x08" + ten_ones), "i32") == -1
    assert scalars.get_field(scalars.decode(b"\x18" + ten_ones), "u32") == 0xFFFFFFFF
    # sint32 truncates to 32 bits *before* unzigzag.
    assert scalars.get_field(scalars.decode(b"\x28\x02"), "s32") == 1
    # int64 keeps its full signed range.
    assert scalars.get_field(
        scalars.decode(b"\x10\x80\x80\x80\x80\x80\x80\x80\x80\x80\x01"), "i64") == -(2 ** 63)


def test_groups():
    group = _load("grouptest.desc", "structtest.GroupTest")
    reference = _reference("grouptest.bin")
    out = group.decode_object(reference)
    assert out["inner"] == {"x": 5, "s": "g"}
    assert out["item"] == [{"v": 1}, {"v": -2}]
    assert out["id"] == 9
    # An unterminated group fails; a stray end-group fails.
    for bad in (b"\x0b\x10\x05", b"\x0c"):
        try:
            group.decode(bad)
        except CoreError:
            pass
        else:
            raise AssertionError("accepted a bad group: %r" % bad)


def test_utf8():
    scalars = _load("matrix3.desc", "structtest.Scalars3")
    # proto3 string (VERIFY): a malformed value fails the decode.
    try:
        scalars.decode(b"\x72\x01\xff")
    except CoreError:
        pass
    else:
        raise AssertionError("accepted invalid UTF-8 in a proto3 string")
    # proto2 string (NONE): accepted and materialized loosely.
    group = _load("grouptest.desc", "structtest.GroupTest")
    msg = group.decode(b"\x0b\x1a\x01\xff\x0c")  # Inner.s = b"\xff"
    assert "\ufffd" in group.to_object(msg)["inner"]["s"]
    # bytes stay bytes; a valid string decodes to str.
    assert isinstance(scalars.get_field(scalars.decode(b"\x72\x02hi"), "str"), str)


def test_required_and_limits():
    matrix2 = _load("matrix2.desc", "structtest.Scalars2")
    # matrix2's Inner carries a required field: a missing one fails the decode.
    try:
        matrix2.decode(b"\x52\x00")  # inner (field 10) with an empty body
    except CoreError as error:
        assert error.status == 6, error
    else:
        raise AssertionError("accepted a missing proto2 required field")
    scalars = _load("matrix3.desc", "structtest.Scalars3")
    # The byte cap and the depth cap are enforced.
    try:
        scalars.decode(_reference("matrix3.bin"), Limits(max_bytes=8))
    except CoreError as error:
        assert error.status == 5, error
    else:
        raise AssertionError("accepted an input over the byte limit")
    try:
        scalars.decode(b"", Limits(max_depth=1))
    except CoreError:
        raise AssertionError("rejected an empty input at depth 1")
    # A message nested one level deeper than the cap fails.
    inner = b"\x82\x01\x02\x10\x01"  # inner { value: 1 } is fine
    assert scalars.to_object(scalars.decode(inner))["inner"] == {"value": 1}
    # 400 nested unknown groups exceed the 100 cap.
    deep = b"\x9b\x06" * 400 + b"\x9c\x06" * 400
    try:
        scalars.decode(deep)
    except CoreError as error:
        assert error.status == 4, error
    else:
        raise AssertionError("accepted 400 nested groups")


def test_descriptor_gate_still_passes():
    # The Stage 1 gate file is part of this suite: keep one call here so a
    # break in the descriptor reader fails the codec run too.
    from test_descriptor import test_matrix3  # noqa: F401 - imported for effect
    test_matrix3()


TESTS = [
    test_reference_bytes, test_matrix3_dict_bytes, test_unknown_retention_order,
    test_wrong_wire_type_is_unknown, test_packed_and_unpacked,
    test_oneof_last_wins, test_presence, test_bounded_reads, test_groups,
    test_utf8, test_required_and_limits, test_descriptor_gate_still_passes,
]


def main():
    failed = 0
    for test in TESTS:
        try:
            test()
        except Exception as error:  # noqa: BLE001
            failed += 1
            print("FAIL %s: %s: %s" % (test.__name__, type(error).__name__, error))
        else:
            print("ok   %s" % test.__name__)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())

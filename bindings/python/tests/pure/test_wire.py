"""Pure-Python core: wire primitive tests.

The bounds are the point: Python ints are unbounded, the wire is not. Every
reader has to return the value the C core's fixed-width type would hold, and
every length/tag has to be rejected where the C core rejects it.
"""

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", ".."))

from pure import wire  # noqa: E402
from pure.wire import CoreError, Reader, Writer, zig32, zig64, unzig32, unzig64  # noqa: E402


def test_varint_widths():
    r = Reader(b"\xff\xff\xff\xff\xff\xff\xff\xff\xff\x01")  # 10 bytes: -1
    assert wire.r_var(r) == 0xFFFFFFFF  # low 32, unsigned
    r = Reader(b"\xff\xff\xff\xff\xff\xff\xff\xff\xff\x01")
    assert wire.r_var64(r) == 0xFFFFFFFFFFFFFFFF
    # int64 of 2^63 is the signed minimum, not a Python-sized 2**63.
    r = Reader(b"\x80\x80\x80\x80\x80\x80\x80\x80\x80\x01")
    assert wire.r_var64(r) == 0x8000000000000000
    # A 5-byte varint whose top bits exceed 32 truncate like uint32_t.
    r = Reader(b"\xff\xff\xff\xff\x7f")
    assert wire.r_var(r) == 0xFFFFFFFF
    # Truncated and overlong encodings fail.
    for bad in (b"", b"\x80", b"\xff" * 10 + b"\x01"):
        try:
            wire.r_var64(Reader(bad))
        except CoreError:
            pass
        else:
            raise AssertionError("accepted a bad varint: %r" % bad)


def test_varint_bool():
    assert wire.r_var_bool(Reader(b"\x00")) == 0
    assert wire.r_var_bool(Reader(b"\x01")) == 1
    # Any set payload bit means true, even in a wide encoding.
    assert wire.r_var_bool(Reader(b"\x80\x02")) == 1


def test_tag_and_length():
    # A 5-byte tag above UINT32_MAX truncates instead of failing (the
    # reference parser accepts the two BadTag_FieldNumberSlightlyTooHigh cases).
    r = Reader(b"\xff\xff\xff\xff\x7f")
    assert wire.r_tag(r) == 0xFFFFFFFF
    r = Reader(b"\x8a\x01")
    assert wire.r_tag(r) == 138
    # Strict length: the fifth byte only carries four bits.
    r = Reader(b"\x80\x80\x80\x80\x10")
    try:
        wire.r_len32(r)
    except CoreError:
        pass
    else:
        raise AssertionError("accepted a length above 32 bits")
    r = Reader(b"\x03abc")
    assert wire.r_len32(r) == 3
    assert r.pos == 1
    # A length past the slice end is truncated, not clamped.
    try:
        wire.r_len32(Reader(b"\x05ab"))
    except CoreError:
        pass
    else:
        raise AssertionError("accepted a length past the end")


def test_zigzag():
    for v in (0, 1, -1, 2147483647, -2147483648):
        assert unzig32(zig32(v)) == v
    for v in (0, 1, -1, 2 ** 63 - 1, -2 ** 63):
        assert unzig64(zig64(v)) == v
    # zigzag is the reference mapping, not just its inverse.
    assert zig32(-1) == 1 and zig32(1) == 2
    assert zig64(-1) == 1 and zig64(1) == 2


def test_write_read_roundtrip():
    cases = [
        ("w_var32", [0, 1, 127, 128, 300, 0xFFFFFFFF]),
        ("w_var64", [0, 1, 127, 128, 2 ** 64 - 1, 2 ** 63]),
        ("w_fixed32", [0, 1, 0xDEADBEEF]),
        ("w_fixed64", [0, 1, 0x0102030405060708]),
    ]
    for name, values in cases:
        for v in values:
            w = Writer()
            getattr(wire, name)(w, v)
            r = Reader(bytes(w.b[:w.pos]))
            read = wire.r_var(r) if name == "w_var32" else (
                wire.r_var64(r) if name == "w_var64" else (
                    wire.r_fixed32(r) if name == "w_fixed32" else wire.r_fixed64(r)))
            mask = 0xFFFFFFFF if name in ("w_var32", "w_fixed32") else 0xFFFFFFFFFFFFFFFF
            assert read == v & mask, (name, v, read)


def test_patch_len():
    w = Writer()
    for payload, tag in ((b"x" * 3, b"\x0a"), (b"y" * 200, b"\x0a"),
                         (b"z" * 300000, b"\x0a")):
        w.reset()
        w.b[w.pos:w.pos + 1] = tag
        w.pos += 1
        lp = w.pos
        w.b[w.pos:w.pos + 1] = b"\x00"
        w.pos += 1
        w.b[w.pos:w.pos + len(payload)] = payload
        w.pos += len(payload)
        wire.patch_len(w, lp)
        out = bytes(w.b[:w.pos])
        r = Reader(out)
        assert wire.r_tag(r) == 10
        n = wire.r_len32(r)
        assert n == len(payload) and out[r.pos:] == payload, (tag, len(payload))


def test_utf8_validation():
    assert wire.utf8_valid(b"hello", 0, 5)
    assert wire.utf8_valid("\u00e9\u4e16\U0001f600".encode(), 0, 9)
    # Overlong, surrogate, past U+10FFFF and bare continuation bytes.
    for bad in (b"\xc0\xaf", b"\xed\xa0\x80", b"\xf4\x90\x80\x80", b"\x80",
                b"\xe0\x80\x80", b"\xf0\x80\x80\x80", b"\xc2"):
        assert not wire.utf8_valid(bad, 0, len(bad)), bad
    # Valid prefix and suffix around a malformed middle byte.
    assert not wire.utf8_valid(b"ab\xffcd", 0, 5)


def test_float_bits():
    assert wire.bits_float(wire.float_bits(1.5)) == 1.5
    assert wire.bits_double(wire.double_bits(-123456.789)) == -123456.789
    # NaN payloads survive as bit patterns (that is why the tree keeps bits).
    nan = 0x7FC00001
    assert wire.float_bits(wire.bits_float(nan)) == nan
    assert wire.bits_float(nan) != wire.bits_float(nan)
    # An out-of-range float becomes an infinity instead of raising.
    assert wire.float_bits(1e40) == 0x7F800000
    assert wire.float_bits(-1e40) == 0xFF800000


def test_skip_group_depth():
    # Three nested start-group frames close cleanly.
    # Body of group 1: a nested group 2 then group 1's end tag.
    r = Reader(b"\x13\x14\x0c")
    wire.skip_group(r, 1, 0)
    assert r.pos == 3
    # A mismatched end tag is an error.
    try:
        wire.skip_group(Reader(b"\x0b\x14"), 1, 0)
    except CoreError:
        pass
    else:
        raise AssertionError("accepted a mismatched end-group")
    # Deep nesting past the cap fails instead of overflowing the stack.
    deep = b"\x0b" * 400 + b"\x0c" * 400
    r = Reader(deep)
    try:
        wire.skip_group(r, 1, 0)
    except CoreError as error:
        assert error.status == 4, error
    else:
        raise AssertionError("accepted 400 nested groups")


TESTS = [
    test_varint_widths, test_varint_bool, test_tag_and_length, test_zigzag,
    test_write_read_roundtrip, test_patch_len, test_utf8_validation,
    test_float_bits, test_skip_group_depth,
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

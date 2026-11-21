"""Protobuf wire primitives for the pure-Python core.

Varint/tag/little-endian read and write, zigzag, skipping (with group frames)
and the structural UTF-8 check. One home for everything the descriptor reader,
the encoder and the decoder do to bytes, so they cannot diverge.

The rules are the C core's (``src/wire.c``, ``src/decode.c``): strict tags and
lengths, unknown fields skippable by shape, groups validated to their matching
end tag, one nesting cap for groups and messages alike.

Python details this file owns:

* ``int`` is unbounded, so every reader masks to the width it serves: a 32-bit
  varint returns its low 32 bits (like the C core's ``uint32_t``), a 64-bit one
  its low 64. Nothing here returns a value wider than the field can hold.
* ``float``/``double`` values travel as their raw bit patterns (``int``), so a
  NaN payload survives a decode/encode round trip. ``float_bits``/``bits_float``
  are the only conversion points.
"""

import struct

WT_VARINT = 0
WT_FIXED64 = 1
WT_LEN = 2
WT_START_GROUP = 3
WT_END_GROUP = 4
WT_FIXED32 = 5

# Nesting cap, the same bound as the C core's PBW_MAX_DEPTH.
MAX_DEPTH = 256

MASK32 = 0xFFFFFFFF
MASK64 = 0xFFFFFFFFFFFFFFFF


class CoreError(Exception):
    """A pbwire failure: the C core's pbw_status where one exists, else -1."""

    def __init__(self, message, status=-1):
        super().__init__(message)
        self.status = status


class Writer:
    """Growable output buffer, reset per encode (the C core's pbw_ctx twin)."""

    __slots__ = ("b", "pos")

    def __init__(self):
        self.b = bytearray(256)
        self.pos = 0

    def reset(self):
        self.pos = 0


class Reader:
    """Cursor over an input slice. Reset per decode, never allocated per call."""

    __slots__ = ("b", "pos", "end", "cap")

    def __init__(self, b, pos=0, end=None):
        self.b = b
        self.pos = pos
        self.end = len(b) if end is None else end
        # Nesting cap for this decode; set from Limits at entry.
        self.cap = MAX_DEPTH


def ensure(w, extra):
    need = w.pos + extra
    if need > len(w.b):
        b = w.b
        size = len(b) * 2
        while size < need:
            size *= 2
        b.extend(b"\x00" * (size - len(b)))


def _tag_int(fnum, wt):
    return fnum * 8 + wt


def tag_bytes(fnum, wt):
    """The (field-number, wire-type) tag as bytes."""
    tag = fnum * 8 + wt
    out = bytearray(5)
    n = 0
    while tag > 0x7F:
        out[n] = (tag & 0x7F) | 0x80
        n += 1
        tag >>= 7
    out[n] = tag
    return bytes(out[:n + 1])


def w_tag(w, tag):
    ensure(w, len(tag))
    b = w.b
    p = w.pos
    for c in tag:
        b[p] = c
        p += 1
    w.pos = p


def w_var32(w, v):
    """varint32 write of an unsigned 32-bit value."""
    ensure(w, 5)
    b = w.b
    p = w.pos
    x = v & MASK32
    while x > 0x7F:
        b[p] = (x & 0x7F) | 0x80
        p += 1
        x >>= 7
    b[p] = x
    w.pos = p + 1


def w_var64(w, v):
    """varint64 write of an unsigned 64-bit value."""
    ensure(w, 10)
    b = w.b
    p = w.pos
    x = v & MASK64
    while x > 0x7F:
        b[p] = (x & 0x7F) | 0x80
        p += 1
        x >>= 7
    b[p] = x
    w.pos = p + 1


def w_fixed32(w, v):
    ensure(w, 4)
    b = w.b
    p = w.pos
    x = v & MASK32
    b[p] = x & 0xFF
    b[p + 1] = (x >> 8) & 0xFF
    b[p + 2] = (x >> 16) & 0xFF
    b[p + 3] = (x >> 24) & 0xFF
    w.pos = p + 4


def w_fixed64(w, v):
    ensure(w, 8)
    b = w.b
    p = w.pos
    x = v & MASK64
    for i in range(8):
        b[p + i] = (x >> (i * 8)) & 0xFF
    w.pos = p + 8


def w_tag_var32(w, tag, v):
    """Fused tag+varint32: one capacity check per field."""
    ensure(w, len(tag) + 5)
    b = w.b
    p = w.pos
    for c in tag:
        b[p] = c
        p += 1
    x = v & MASK32
    while x > 0x7F:
        b[p] = (x & 0x7F) | 0x80
        p += 1
        x >>= 7
    b[p] = x
    w.pos = p + 1


def w_tag_var64(w, tag, v):
    ensure(w, len(tag) + 10)
    b = w.b
    p = w.pos
    for c in tag:
        b[p] = c
        p += 1
    x = v & MASK64
    while x > 0x7F:
        b[p] = (x & 0x7F) | 0x80
        p += 1
        x >>= 7
    b[p] = x
    w.pos = p + 1


def w_tag_fixed32(w, tag, v):
    ensure(w, len(tag) + 4)
    b = w.b
    p = w.pos
    for c in tag:
        b[p] = c
        p += 1
    x = v & MASK32
    b[p] = x & 0xFF
    b[p + 1] = (x >> 8) & 0xFF
    b[p + 2] = (x >> 16) & 0xFF
    b[p + 3] = (x >> 24) & 0xFF
    w.pos = p + 4


def w_tag_fixed64(w, tag, v):
    ensure(w, len(tag) + 8)
    b = w.b
    p = w.pos
    for c in tag:
        b[p] = c
        p += 1
    x = v & MASK64
    for i in range(8):
        b[p + i] = (x >> (i * 8)) & 0xFF
    w.pos = p + 8


def w_len(w, n):
    ensure(w, 5)
    b = w.b
    p = w.pos
    x = n
    while x > 0x7F:
        b[p] = (x & 0x7F) | 0x80
        p += 1
        x >>= 7
    b[p] = x
    w.pos = p + 1


def w_bytes(w, tag, v):
    """Length-delimited payload under ``tag`` (string/bytes/message)."""
    n = len(v)
    ensure(w, (0 if tag is None else len(tag)) + 5 + n)
    b = w.b
    p = w.pos
    if tag is not None:
        for c in tag:
            b[p] = c
            p += 1
    x = n
    while x > 0x7F:
        b[p] = (x & 0x7F) | 0x80
        p += 1
        x >>= 7
    b[p] = x
    p += 1
    b[p:p + n] = v
    w.pos = p + n


def patch_len(w, lp):
    """Fills a 1-byte length placeholder written at ``lp``."""
    b = w.b
    length = w.pos - lp - 1
    if length < 128:
        b[lp] = length
        return
    n = size_var32(length)
    # Shift the body up by (n - 1) bytes.
    w.pos += n - 1
    ensure(w, 0)
    b = w.b
    b[lp + n:w.pos] = b[lp + 1:w.pos - n + 1]
    x = length
    p = lp
    while x > 0x7F:
        b[p] = (x & 0x7F) | 0x80
        p += 1
        x >>= 7
    b[p] = x


def size_var32(v):
    n = 1
    x = v & MASK32 if v < 0 else v
    while x > 0x7F:
        x >>= 7
        n += 1
    return n


def size_var64(v):
    n = 1
    x = v & MASK64 if v < 0 else v
    while x > 0x7F:
        x >>= 7
        n += 1
    return n


def size_int32(v):
    """int32: negatives sign-extend to 10 bytes, same as libprotobuf."""
    return size_var32(v) if v >= 0 else 10


def zig32(v):
    return ((v << 1) ^ (v >> 31)) & MASK32


def zig64(v):
    return ((v << 1) ^ (v >> 63)) & MASK64


def unzig32(v):
    return (v >> 1) ^ -(v & 1)


def unzig64(v):
    return (v >> 1) ^ -(v & 1)


# ---------------------------------------------------------------- reading

def r_var(r):
    """varint, low 32 bits (unsigned). Validates the encoding's length only."""
    b = r.b
    end = r.end
    p = r.pos
    lo = 0
    j = 0
    while True:
        if p >= end:
            raise CoreError("truncated varint")
        c = b[p]
        p += 1
        if j < 5:
            # The mask discards the high bits on purpose: only the low 32 are
            # ever visible for the 32-bit kinds, like the C core's uint32_t.
            lo |= (c & 0x7F) << (j * 7)
        if not c & 0x80:
            r.pos = p
            return lo & MASK32
        j += 1
        if j >= 10:
            raise CoreError("varint overflow")


def r_var64(r):
    """varint, full 64 bits (unsigned)."""
    b = r.b
    end = r.end
    p = r.pos
    v = 0
    shift = 0
    while True:
        if p >= end:
            raise CoreError("truncated varint")
        c = b[p]
        p += 1
        v |= (c & 0x7F) << shift
        if not c & 0x80:
            r.pos = p
            return v & MASK64
        shift += 7
        if shift >= 64:
            raise CoreError("varint overflow")


def r_var_bool(r):
    """varint, reduced to its bool value: any set payload bit means true."""
    b = r.b
    end = r.end
    p = r.pos
    nz = 0
    j = 0
    while True:
        if p >= end:
            raise CoreError("truncated varint")
        c = b[p]
        p += 1
        nz |= c & 0x7F
        if not c & 0x80:
            r.pos = p
            return 1 if nz else 0
        j += 1
        if j >= 10:
            raise CoreError("varint overflow")


def r_len32(r):
    """Strict length read, like the core's: at most five bytes, the fifth only
    carries four bits; an out-of-range length is rejected, not truncated.
    Leaves ``r.pos`` after the length."""
    b = r.b
    end = r.end
    p = r.pos
    if p >= end:
        raise CoreError("truncated length")
    c = b[p]
    p += 1
    v = c & 0x7F
    shift = 7
    while c & 0x80:
        if shift >= 35 or p >= end:
            raise CoreError("bad length")
        c = b[p]
        p += 1
        if shift == 28 and (c & 0x7F) > 0x0F:
            raise CoreError("length overflow")
        v |= (c & 0x7F) << shift
        shift += 7
    if p + v > end:
        raise CoreError("truncated length-delimited")
    r.pos = p
    return v


def r_len(r):
    """Loose length read for descriptor parsing: any varint up to 2^31-1,
    bounds checked."""
    v = r_var64(r)
    if v > 0x7FFFFFFF:
        raise CoreError("length overflow")
    if r.pos + v > r.end:
        raise CoreError("truncated length-delimited")
    return v


def r_tag(r):
    """Strict tag read: a varint32 of at most five bytes. A 5-byte tag above
    UINT32_MAX truncates instead of failing, exactly what the reference parser
    does (the official suite expects two such cases to parse)."""
    b = r.b
    end = r.end
    p = r.pos
    if p >= end:
        raise CoreError("truncated tag")
    c = b[p]
    p += 1
    v = c & 0x7F
    shift = 7
    while c & 0x80:
        if shift >= 35 or p >= end:
            raise CoreError("bad tag")
        c = b[p]
        p += 1
        v |= (c & 0x7F) << shift  # masked below: truncates like the reference
        shift += 7
    r.pos = p
    return v & MASK32


def r_fixed32(r):
    if r.pos + 4 > r.end:
        raise CoreError("truncated fixed32")
    b = r.b
    p = r.pos
    r.pos = p + 4
    return b[p] | (b[p + 1] << 8) | (b[p + 2] << 16) | (b[p + 3] << 24)


def r_fixed64(r):
    """Raw 64 bits, unsigned."""
    if r.pos + 8 > r.end:
        raise CoreError("truncated fixed64")
    b = r.b
    p = r.pos
    r.pos = p + 8
    lo = b[p] | (b[p + 1] << 8) | (b[p + 2] << 16) | (b[p + 3] << 24)
    hi = b[p + 4] | (b[p + 5] << 8) | (b[p + 6] << 16) | (b[p + 7] << 24)
    return lo | (hi << 32)


def skip(r, wt):
    """Skips one field value of a non-group wire type."""
    if wt == WT_VARINT:
        r_var_bool(r)
    elif wt == WT_FIXED64:
        if r.pos + 8 > r.end:
            raise CoreError("truncated fixed64")
        r.pos += 8
    elif wt == WT_LEN:
        n = r_len32(r)
        r.pos += n
    elif wt == WT_FIXED32:
        if r.pos + 4 > r.end:
            raise CoreError("truncated fixed32")
        r.pos += 4
    else:
        raise CoreError("bad wire type %d" % wt)


def skip_group(r, number, depth):
    """Validates a group body up to its matching end tag, like the core's group
    skipper: every tag is strict, field 0 and wire types above 5 are rejected,
    and an end tag only closes the group it names."""
    if depth > r.cap:
        raise CoreError("nesting exceeds %d" % r.cap, 4)
    while True:
        tag = r_tag(r)
        fnum = tag >> 3
        wt = tag & 7
        if fnum == 0 or wt > WT_FIXED32:
            raise CoreError("bad tag %d" % tag)
        if wt == WT_END_GROUP:
            if fnum != number:
                raise CoreError("mismatched end-group for %d, want %d" % (fnum, number))
            return
        if wt == WT_START_GROUP:
            skip_group(r, fnum, depth + 1)
        else:
            skip(r, wt)


def skip_unknown(r, fnum, wt, depth):
    """Skips one unknown field: groups validate to their matching end tag,
    anything else only checks value shape. End-group tags never reach here; the
    decode loop owns them."""
    if wt == WT_START_GROUP:
        skip_group(r, fnum, depth + 1)
        return
    skip(r, wt)


def utf8_valid(b, off, length):
    """Structural UTF-8 check: rejects overlong forms, surrogates and values
    past U+10FFFF, like the core's pbw_utf8_valid and the reference parsers."""
    i = off
    end = off + length
    while i < end:
        c = b[i]
        if c < 0x80:
            i += 1
        elif c & 0xE0 == 0xC0:
            if c < 0xC2 or i + 1 >= end or b[i + 1] & 0xC0 != 0x80:
                return False
            i += 2
        elif c & 0xF0 == 0xE0:
            if i + 2 >= end or b[i + 1] & 0xC0 != 0x80 or b[i + 2] & 0xC0 != 0x80:
                return False
            v = ((c & 15) << 12) | ((b[i + 1] & 63) << 6) | (b[i + 2] & 63)
            if v < 0x800 or 0xD800 <= v <= 0xDFFF:
                return False
            i += 3
        elif c & 0xF8 == 0xF0:
            if (c > 0xF4 or i + 3 >= end or b[i + 1] & 0xC0 != 0x80
                    or b[i + 2] & 0xC0 != 0x80 or b[i + 3] & 0xC0 != 0x80):
                return False
            v = ((c & 7) << 18) | ((b[i + 1] & 63) << 12) | ((b[i + 2] & 63) << 6) | (b[i + 3] & 63)
            if v < 0x10000 or v > 0x10FFFF:
                return False
            i += 4
        else:
            return False
    return True


def utf8_encode(s):
    """A Python str as UTF-8; a lone surrogate is not a character and encodes
    as U+FFFD, like the TS core and TextEncoder."""
    try:
        return s.encode("utf-8")
    except UnicodeEncodeError:
        return "".join(
            "\ufffd" if 0xD800 <= ord(ch) <= 0xDFFF else ch for ch in s
        ).encode("utf-8")


# ------------------------------------------------- float/double bit patterns

_F32 = struct.Struct("<f")
_F64 = struct.Struct("<d")
_U32 = struct.Struct("<I")
_U64 = struct.Struct("<Q")


def float_bits(v):
    """The float32 bit pattern of a Python float (round-to-nearest; an
    out-of-range value becomes an infinity, like the reference cast)."""
    try:
        return _U32.unpack(_F32.pack(v))[0]
    except OverflowError:
        return 0xFF800000 if v < 0 else 0x7F800000


def bits_float(bits):
    return _F32.unpack(_U32.pack(bits & MASK32))[0]


def double_bits(v):
    return _U64.unpack(_F64.pack(v))[0]


def bits_double(bits):
    return _F64.unpack(_U64.pack(bits & MASK64))[0]

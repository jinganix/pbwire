"""Encoder: one pass into a reusable buffer, fields in schema (field-number)
order, unknown fields appended verbatim in input order. Nested payloads use a
one-byte length placeholder patched afterwards, packed runs use a five-byte
placeholder shifted down once, and untouched Raw slices copy verbatim (tag
included), so a decode/encode round trip is byte-exact.

The Python twin of ``bindings/node/lib/core/encode.ts``.
"""

from .descriptor import (
    IMPLICIT,
    K_BOOL,
    K_BYTES,
    K_DOUBLE,
    K_ENUM,
    K_FIXED32,
    K_FIXED64,
    K_FLOAT,
    K_GROUP,
    K_INT32,
    K_INT64,
    K_MESSAGE,
    K_SFIXED32,
    K_SFIXED64,
    K_SINT32,
    K_SINT64,
    K_STRING,
    K_UINT32,
    K_UINT64,
    REQUIRED,
    is_packable,
)
from .tree import Raw
from .wire import (
    MAX_DEPTH,
    CoreError,
    Writer,
    ensure,
    patch_len,
    size_var32,
    utf8_encode,
    w_bytes,
    w_fixed32,
    w_fixed64,
    w_len,
    w_tag,
    w_tag_fixed32,
    w_tag_fixed64,
    w_tag_var32,
    w_tag_var64,
    w_var32,
    w_var64,
    zig32,
    zig64,
)

# One workspace buffer: an encode pays no allocation but the result copy.
_WORKSPACE = Writer()


def encode(m):
    w = _WORKSPACE
    w.reset()
    encode_into(m, w, 0)
    return bytes(w.b[:w.pos])


def encode_into(m, w, depth):
    if depth > MAX_DEPTH:
        raise CoreError("nesting exceeds %d" % MAX_DEPTH, 4)
    def_ = m.def_
    no_required = not def_.has_required  # proto3: skip the check
    values = m.v
    for slot, f in enumerate(def_.fields):
        v = values[slot]
        if v is None:
            if not no_required and f.presence == REQUIRED:
                raise CoreError(
                    "missing required field %s.%s" % (def_.full_name, f.name), 6)
            continue
        kind = f.kind
        if f.repeated:
            if not v:
                continue
            if is_packable(kind):
                encode_rep_num(w, f, v)
            else:
                for item in v:
                    write_value(w, f, item, depth)
            continue
        if is_packable(kind):
            # Only implicit defaults skip here; explicit defaults (and
            # required) emit once set.
            if v == 0 and f.presence == IMPLICIT:
                continue
            write_singular_num(w, f, v)
            continue
        # Implicit empty strings and bytes stay silent, everything else emits.
        if f.presence == IMPLICIT:
            if v == "" or (isinstance(v, (bytes, bytearray)) and not v):
                continue
        write_value(w, f, v, depth)
    unknown = m.unknown
    if unknown is not None:
        # Unknown fields last, in input order.
        for u in unknown:
            n = len(u)
            ensure(w, n)
            w.b[w.pos:w.pos + n] = u
            w.pos += n


def write_value(w, f, v, depth):
    if isinstance(v, Raw):
        # Tag included: a verbatim copy stays byte-exact.
        ensure(w, v.len)
        w.b[w.pos:w.pos + v.len] = v.b[v.off:v.off + v.len]
        w.pos += v.len
        return
    kind = f.kind
    if kind == K_STRING:
        data = utf8_encode(v)
        w_bytes(w, f.tag, data)
        return
    if kind == K_BYTES:
        w_bytes(w, f.tag, v)
        return
    if kind == K_MESSAGE:
        # Inlined tag + length placeholder + patch: a repeated message field is
        # the hottest nesting shape.
        tag = f.tag
        ensure(w, len(tag) + 1)
        b = w.b
        p = w.pos
        for c in tag:
            b[p] = c
            p += 1
        lp = p
        b[p] = 0
        w.pos = p + 1
        encode_into(v, w, depth + 1)
        patch_len(w, lp)
        return
    # K_GROUP: start tag, body, end tag.
    w_tag(w, f.tag)
    encode_into(v, w, depth + 1)
    w_tag(w, f.tag_end)


def write_singular_num(w, f, v):
    """Singular numeric write: one fused tag+value call per field."""
    kind = f.kind
    if kind == K_INT32 or kind == K_ENUM:
        # Encode the int32 interpretation, sign-extended: the reference
        # truncates a wide varint to int32 and re-emits a negative as 10 bytes.
        if v >= 0:
            w_tag_var32(w, f.tag, v)
        else:
            w_tag_var64(w, f.tag, v)
    elif kind == K_UINT32:
        w_tag_var32(w, f.tag, v)
    elif kind == K_SINT32:
        w_tag_var32(w, f.tag, zig32(v))
    elif kind == K_INT64 or kind == K_UINT64:
        w_tag_var64(w, f.tag, v)
    elif kind == K_SINT64:
        w_tag_var64(w, f.tag, zig64(v))
    elif kind == K_BOOL:
        w_tag_var32(w, f.tag, 0 if v == 0 else 1)
    elif kind == K_FIXED32 or kind == K_SFIXED32 or kind == K_FLOAT:
        w_tag_fixed32(w, f.tag, v)
    else:  # K_FIXED64, K_SFIXED64, K_DOUBLE
        w_tag_fixed64(w, f.tag, v)


def encode_rep_num(w, f, arr):
    """One dispatch per field; the loops below it are branch-free writes."""
    n = len(arr)
    kind = f.kind
    if f.packed:
        if kind == K_BOOL:
            w_tag(w, f.tag)
            w_len(w, n)
            ensure(w, n)
            b = w.b
            p = w.pos
            for v in arr:
                b[p] = 0 if v == 0 else 1
                p += 1
            w.pos = p
            return
        if kind == K_FIXED32 or kind == K_SFIXED32 or kind == K_FLOAT:
            w_tag(w, f.tag)
            w_len(w, n * 4)
            ensure(w, n * 4)
            b = w.b
            p = w.pos
            for v in arr:
                x = v & 0xFFFFFFFF
                b[p] = x & 0xFF
                b[p + 1] = (x >> 8) & 0xFF
                b[p + 2] = (x >> 16) & 0xFF
                b[p + 3] = (x >> 24) & 0xFF
                p += 4
            w.pos = p
            return
        if kind == K_FIXED64 or kind == K_SFIXED64 or kind == K_DOUBLE:
            w_tag(w, f.tag)
            w_len(w, n * 8)
            ensure(w, n * 8)
            for v in arr:
                w_fixed64(w, v)
            return
        # Varint kinds: write the payload over a 5-byte placeholder, then shift
        # it down: one memmove beats a size pre-pass over thousands of elements.
        w_tag(w, f.tag)
        lp = w.pos
        ensure(w, n * 10 + 5)
        w.pos += 5
        for v in arr:
            write_packed_var(w, kind, v)
        length = w.pos - lp - 5
        llen = size_var32(length)
        b = w.b
        if llen != 5:
            b[lp + llen:w.pos - 5 + llen] = b[lp + 5:w.pos]
        p = lp
        x = length
        while x > 0x7F:
            b[p] = (x & 0x7F) | 0x80
            p += 1
            x >>= 7
        b[p] = x
        w.pos = lp + llen + length
        return
    for v in arr:
        w_tag(w, f.tag)
        write_num_single(w, kind, v)


def write_packed_var(w, kind, v):
    if kind == K_INT32 or kind == K_ENUM:
        if v >= 0:
            w_var32(w, v)
        else:
            w_var64(w, v)
    elif kind == K_UINT32:
        w_var32(w, v)
    elif kind == K_SINT32:
        w_var32(w, zig32(v))
    elif kind == K_INT64 or kind == K_UINT64:
        w_var64(w, v)
    elif kind == K_SINT64:
        w_var64(w, zig64(v))
    else:
        raise CoreError("not packable")


def write_num_single(w, kind, v):
    if kind == K_INT32 or kind == K_ENUM:
        if v >= 0:
            w_var32(w, v)
        else:
            w_var64(w, v)
    elif kind == K_UINT32:
        w_var32(w, v)
    elif kind == K_SINT32:
        w_var32(w, zig32(v))
    elif kind == K_INT64 or kind == K_UINT64:
        w_var64(w, v)
    elif kind == K_SINT64:
        w_var64(w, zig64(v))
    elif kind == K_BOOL:
        ensure(w, 1)
        w.b[w.pos] = 0 if v == 0 else 1
        w.pos += 1
    elif kind == K_FIXED32 or kind == K_SFIXED32 or kind == K_FLOAT:
        w_fixed32(w, v)
    else:  # K_FIXED64, K_SFIXED64, K_DOUBLE
        w_fixed64(w, v)

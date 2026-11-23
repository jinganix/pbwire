"""Decoder: strict tags and lengths (like the C core), unknown field numbers
AND wrong-wire-type occurrences retained as raw tag+payload bytes in input
order, groups decoded to their matching end tag, packed runs accepted for every
numeric kind (and the unpacked element form for a packed field), oneofs
collapsed to the last member, presence following the field's own zero, proto2
required fields checked over the whole tree, and the nesting cap enforced on
every recursive path.

The Python twin of ``bindings/node/lib/core/decode.ts``.
"""

from .descriptor import (
    IMPLICIT,
    K_BOOL,
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
    K_UINT32,
    K_UINT64,
    REQUIRED,
    check_bytes,
    effective_depth,
    is_packable,
    slot_of,
)
from .tree import Message, Raw, set_slot
from .wire import (
    MASK32,
    MASK64,
    CoreError,
    Reader,
    WT_END_GROUP,
    WT_FIXED32,
    WT_LEN,
    WT_START_GROUP,
    r_fixed32,
    r_fixed64,
    r_len32,
    r_tag,
    r_var,
    r_var64,
    r_var_bool,
    skip_unknown,
    unzig32,
    unzig64,
    utf8_valid,
)


def _signed32(v):
    v &= MASK32
    return v - 0x100000000 if v & 0x80000000 else v


def _signed64(v):
    v &= MASK64
    return v - 0x10000000000000000 if v & 0x8000000000000000 else v


def decode(def_, data, limits=None):
    """Decodes a whole message, unknown fields retained."""
    check_bytes(limits, len(data), "message")
    r = Reader(data)
    r.cap = effective_depth(limits)
    m = Message(def_)
    decode_into(m, r, len(data), -1, 0)
    if def_.deep_required:
        check_required(m, r.cap, 0)
    return m


def decode_slice(def_, data, off, length, limits=None):
    """Decodes a message body that already starts at ``off`` and ends at
    ``off+length``."""
    check_bytes(limits, length, "message")
    r = Reader(data, off, off + length)
    r.cap = effective_depth(limits)
    m = Message(def_)
    decode_into(m, r, off + length, -1, 0)
    if def_.deep_required:
        check_required(m, r.cap, 0)
    return m


def new_message(def_):
    """An empty message of ``def_``."""
    return Message(def_)


def wt_incompatible(f, wt):
    """True when the field does not accept this wire type. A repeated numeric
    field always accepts the LEN form (a packed run, whatever the declared
    form) and a packed field also accepts its unpacked element form."""
    if f.kind == K_GROUP:
        return wt != WT_START_GROUP
    if f.repeated and is_packable(f.kind):
        if wt == WT_LEN:
            return False
        if f.packed:
            return wt != f.elem_wt
    return wt != f.wt


def decode_into(m, r, end, group_end, depth):
    """Decodes into m up to ``end`` (or the matching end-group when
    ``group_end`` >= 0). One shared reader threads through the recursion, so
    positions never need saving: a nested payload consumes exactly its slice."""
    if depth > r.cap:
        raise CoreError("nesting exceeds %d" % r.cap, 4)
    def_ = m.def_
    outer_end = r.end
    r.end = end
    closed = group_end < 0
    fields = def_.fields
    while r.pos < r.end:
        field_start = r.pos
        tag = r_tag(r)
        fnum = tag >> 3
        wt = tag & 7
        if fnum == 0 or wt > WT_FIXED32:
            raise CoreError("bad tag %d" % tag, 1)
        if wt == WT_END_GROUP:
            # Only our own number closes us; any other end tag is stray.
            if group_end < 0 or fnum != group_end:
                raise CoreError("stray end-group for %d" % fnum, 1)
            closed = True
            break
        slot = slot_of(def_, fnum)
        if slot < 0 or wt_incompatible(fields[slot], wt):
            retain_unknown(m, r, field_start, fnum, wt, depth)
            continue
        read_known(m, r, fields[slot], slot, wt, field_start, depth)
    r.end = outer_end
    if not closed:
        raise CoreError("missing end-group for %d" % group_end, 1)


def retain_unknown(m, r, field_start, fnum, wt, depth):
    """Wrong wire type (or unknown number) keeps the raw blob, like the core.
    Groups validate to their matching end tag; anything else only checks value
    shape -- unknown LEN payloads stay opaque."""
    skip_unknown(r, fnum, wt, depth)
    if m.unknown is None:
        m.unknown = []
    m.unknown.append(bytes(r.b[field_start:r.pos]))


def read_known(m, r, f, slot, wt, field_start, depth):
    kind = f.kind
    if f.repeated:
        arr = m.v[slot]
        if arr is None:
            arr = []
            m.v[slot] = arr
        if is_packable(kind):
            if wt == WT_LEN:
                read_packed(r, f, arr)
                if not arr:
                    # An empty packed run adds nothing: presence follows
                    # length, so drop the holder again.
                    m.v[slot] = None
                return
            arr.append(read_num_single(r, kind))
            return
        if kind == K_MESSAGE:
            length = r_len32(r)
            sub = Message(f.msg_type)
            decode_into(sub, r, r.pos + length, -1, depth + 1)
            arr.append(sub)
            return
        if kind == K_GROUP:
            sub = Message(f.msg_type)
            decode_into(sub, r, r.end, f.number, depth + 1)
            arr.append(sub)
            return
        # K_STRING / K_BYTES element: validate, then record one slice.
        length = r_len32(r)
        if f.utf8 and not utf8_valid(r.b, r.pos, length):
            raise CoreError("invalid UTF-8 in a string field", 1)
        poff = r.pos
        r.pos += length
        arr.append(Raw(r.b, field_start, r.pos - field_start, poff, length))
        return
    if is_packable(kind):
        v = read_num_single(r, kind)
        # Implicit presence is decided on the *field's* zero, not the raw wire
        # bits: an int32/uint32 varint wider than 32 bits truncates and the
        # reference then drops the field as an implicit zero.
        if f.presence == IMPLICIT and v == 0:
            return
        set_slot(m, slot, v)
        return
    # Ref-kind singular: a second occurrence merges (like the core).
    if kind == K_MESSAGE:
        length = r_len32(r)
        existing = merge_target(m, f, slot)
        if existing is not None:
            decode_into(existing, r, r.pos + length, -1, depth + 1)
        else:
            sub = Message(f.msg_type)
            decode_into(sub, r, r.pos + length, -1, depth + 1)
            set_slot(m, slot, sub)
        return
    if kind == K_GROUP:
        existing = merge_target(m, f, slot)
        if existing is not None:
            decode_into(existing, r, r.end, f.number, depth + 1)
        else:
            sub = Message(f.msg_type)
            decode_into(sub, r, r.end, f.number, depth + 1)
            set_slot(m, slot, sub)
        return
    # K_STRING / K_BYTES (an empty implicit string stays absent).
    length = r_len32(r)
    if length == 0 and f.presence == IMPLICIT:
        return
    # proto3/editions strings are validated here, at parse, like the reference:
    # a malformed value fails the decode instead of surfacing as U+FFFD when
    # the slice is materialized. proto2 keeps the loose mapping.
    if f.utf8 and not utf8_valid(r.b, r.pos, length):
        raise CoreError("invalid UTF-8 in a string field", 1)
    poff = r.pos
    r.pos += length
    set_slot(m, slot, Raw(r.b, field_start, r.pos - field_start, poff, length))


def merge_target(m, f, slot):
    """The submessage a repeated singular message/group field merges into, or
    None for the first occurrence."""
    v = m.v[slot]
    if v is None:
        return None
    if f.oneof >= 0 and m.oneof_case is not None and m.oneof_case[f.oneof] != slot + 1:
        return None
    return v if isinstance(v, Message) else None


def read_packed(r, f, arr):
    """One packed run of numeric elements into ``arr``."""
    length = r_len32(r)
    end = r.pos + length
    kind = f.kind
    if kind == K_BOOL:
        while r.pos < end:
            arr.append(r_var_bool(r))
    elif kind == K_FIXED32 or kind == K_SFIXED32 or kind == K_FLOAT:
        count = length // 4
        for _ in range(count):
            v = r_fixed32(r)
            arr.append(_signed32(v) if kind == K_SFIXED32 else v)
    elif kind == K_FIXED64 or kind == K_SFIXED64 or kind == K_DOUBLE:
        count = length // 8
        for _ in range(count):
            v = r_fixed64(r)
            arr.append(_signed64(v) if kind == K_SFIXED64 else v)
    else:
        while r.pos < end:
            arr.append(read_packed_var(r, kind))
    if r.pos != end:
        raise CoreError("packed overrun", 1)


def read_packed_var(r, kind):
    if kind == K_INT32 or kind == K_ENUM:
        return _signed32(r_var(r))
    if kind == K_UINT32:
        return r_var(r)
    if kind == K_SINT32:
        # Truncate before unzigzag: a 64-bit varint on a sint32 field decodes
        # its low 32 bits, like the reference.
        return unzig32(r_var(r))
    if kind == K_INT64:
        return _signed64(r_var64(r))
    if kind == K_UINT64:
        return r_var64(r)
    if kind == K_SINT64:
        return unzig64(r_var64(r))
    raise CoreError("not numeric", 1)


def read_num_single(r, kind):
    """One numeric element in its storage form (see tree.py)."""
    if kind == K_INT32 or kind == K_ENUM:
        return _signed32(r_var(r))
    if kind == K_UINT32:
        return r_var(r)
    if kind == K_BOOL:
        return r_var_bool(r)
    if kind == K_SINT32:
        return unzig32(r_var(r))
    if kind == K_INT64:
        return _signed64(r_var64(r))
    if kind == K_UINT64:
        return r_var64(r)
    if kind == K_SINT64:
        return unzig64(r_var64(r))
    if kind == K_FIXED32:
        return r_fixed32(r)
    if kind == K_SFIXED32:
        return _signed32(r_fixed32(r))
    if kind == K_FLOAT:
        return r_fixed32(r)
    if kind == K_FIXED64:
        return r_fixed64(r)
    if kind == K_SFIXED64:
        return _signed64(r_fixed64(r))
    if kind == K_DOUBLE:
        return r_fixed64(r)
    raise CoreError("not numeric", 1)


def check_required(m, cap, depth):
    """Whole-tree required check: every present nested message is checked too,
    so a missing proto2 required field fails the decode, like the core."""
    if depth > cap:
        raise CoreError("nesting exceeds %d" % cap, 4)
    def_ = m.def_
    if def_.has_required:
        for slot, f in enumerate(def_.fields):
            if f.presence == REQUIRED and m.v[slot] is None:
                raise CoreError(
                    "missing required field %s.%s" % (def_.full_name, f.name), 6)
    for slot, f in enumerate(def_.fields):
        if f.kind != K_MESSAGE and f.kind != K_GROUP:
            continue
        v = m.v[slot]
        if v is None:
            continue
        if f.repeated:
            for item in v:
                check_required(item, cap, depth + 1)
        elif isinstance(v, Message):
            check_required(v, cap, depth + 1)

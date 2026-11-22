"""The dynamic message tree: one slot per declared field, plus the oneof cases
and the retained unknown fields.

Scalars live in their wire-neutral Python form (``int`` for every integer
width -- 64-bit values stay in their signed/unsigned 64-bit range, so nothing
is truncated to what a host ``int`` could hold; raw bit patterns for
float/double so NaN payloads survive a round trip; 0/1 for bool); messages live
nested as ``Message``; strings and bytes decoded off the wire stay a ``Raw``
slice of the input until materialized, so invalid UTF-8 and non-minimal length
prefixes re-encode byte for byte.

This is the Python twin of ``bindings/node/lib/core/tree.ts`` (itself the twin
of ``bindings/java/runtime/GenericCodec.DMsg``).
"""

from .descriptor import (
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
    slot_of,
)
from .wire import (
    MASK32,
    MASK64,
    CoreError,
    bits_double,
    bits_float,
    double_bits,
    float_bits,
)

class Raw:
    """An undecoded field image (tag + length + payload) inside the input.

    ``off``/``length`` cover the whole image (tag included) so a re-encode can
    copy it verbatim; ``poff``/``plen`` point at the payload so materializing
    does not have to re-read the tag and length.
    """

    __slots__ = ("b", "off", "len", "poff", "plen")

    def __init__(self, b, off, length, poff, plen):
        self.b = b
        self.off = off
        self.len = length
        self.poff = poff
        self.plen = plen

    def __repr__(self):
        return "<Raw %d+%d>" % (self.off, self.len)


class Message:
    """One message value: per-slot values, the oneof cases and unknowns."""

    __slots__ = ("def_", "v", "oneof_case", "unknown")

    def __init__(self, def_):
        self.def_ = def_
        self.v = [None] * len(def_.fields)
        # Slot+1 per oneof, 0 = none set; None when the message has none.
        self.oneof_case = [0] * def_.oneof_count if def_.oneof_count else None
        # Retained unknown fields in input order; None until the first.
        self.unknown = None

    def __repr__(self):
        return "<Message %s>" % self.def_.full_name


def slot_by_name(def_, name):
    """Slot index of a field name, or -1."""
    slot = def_.by_name.get(name)
    return -1 if slot is None else slot


def require_slot(def_, name_or_number):
    if isinstance(name_or_number, int):
        slot = slot_of(def_, name_or_number)
    else:
        slot = slot_by_name(def_, name_or_number)
    if slot < 0:
        raise CoreError("%s has no field %s" % (def_.full_name, name_or_number))
    return slot


def set_slot(m, slot, value):
    """Stores a value in a slot, collapsing the oneof it belongs to."""
    f = m.def_.fields[slot]
    if f.oneof >= 0 and m.oneof_case is not None:
        cases = m.oneof_case
        prev = cases[f.oneof]
        if prev != 0 and prev != slot + 1:
            m.v[prev - 1] = None
        cases[f.oneof] = slot + 1
    m.v[slot] = value


def has_field(m, name_or_number):
    return m.v[require_slot(m.def_, name_or_number)] is not None


def get_field(m, name_or_number):
    """The value of a field: materialized, converted, or ``None``."""
    slot = require_slot(m.def_, name_or_number)
    f = m.def_.fields[slot]
    v = m.v[slot]
    if v is None:
        return None
    if f.repeated:
        return [to_py(f, item) for item in v]
    return to_py(f, v)


def oneof_case(m, oneof_name):
    """Which member of a oneof is set: its field number, or 0 for none."""
    idx = m.def_.oneof_by_name.get(oneof_name)
    if idx is None or m.oneof_case is None:
        raise CoreError("%s has no oneof %s" % (m.def_.full_name, oneof_name))
    case = m.oneof_case[idx]
    return 0 if case == 0 else m.def_.fields[case - 1].number


def raw_payload(raw):
    """The payload bytes of a Raw string/bytes field image."""
    return bytes(raw.b[raw.poff:raw.poff + raw.plen])


def materialize(f, raw):
    """Materializes a Raw slice into its Python value."""
    if f.kind == K_STRING:
        # proto3/editions strings were validated at parse; the loose mapping
        # (U+FFFD per malformed unit) only applies to proto2 strings.
        return bytes(raw.b[raw.poff:raw.poff + raw.plen]).decode("utf-8", "replace")
    return bytes(raw.b[raw.poff:raw.poff + raw.plen])


def to_py(f, v):
    """One value in its Python form (what get()/to_dict() expose)."""
    if isinstance(v, Raw):
        return materialize(f, v)
    kind = f.kind
    if kind == K_BOOL:
        return v != 0
    if kind == K_FLOAT:
        return bits_float(v)
    if kind == K_DOUBLE:
        return bits_double(v)
    if kind == K_MESSAGE or kind == K_GROUP:
        return v
    return v


def to_object(m):
    """A plain dict: proto field names, nested dicts, Python scalars."""
    def_ = m.def_
    out = {}
    for slot, f in enumerate(def_.fields):
        v = m.v[slot]
        if v is None:
            continue
        kind = f.kind
        if f.repeated:
            if not v:
                continue
            if kind == K_MESSAGE or kind == K_GROUP:
                out[f.name] = [to_object(item) for item in v]
            else:
                out[f.name] = [to_py(f, item) for item in v]
            continue
        if kind == K_MESSAGE or kind == K_GROUP:
            out[f.name] = to_object(v)
            continue
        out[f.name] = to_py(f, v)
    return out


def _as_int32(v):
    v &= MASK32
    return v - 0x100000000 if v & 0x80000000 else v


def _as_int64(v):
    v &= MASK64
    return v - 0x10000000000000000 if v & 0x8000000000000000 else v


def from_py(f, v):
    """One Python value in its storage form (the inverse of to_py)."""
    kind = f.kind
    if kind == K_INT32 or kind == K_SINT32 or kind == K_SFIXED32 or kind == K_ENUM:
        return _as_int32(int(v))
    if kind == K_UINT32 or kind == K_FIXED32:
        return int(v) & MASK32
    if kind == K_INT64 or kind == K_SINT64 or kind == K_SFIXED64:
        return _as_int64(int(v))
    if kind == K_UINT64 or kind == K_FIXED64:
        return int(v) & MASK64
    if kind == K_BOOL:
        return 1 if v else 0
    if kind == K_FLOAT:
        return float_bits(float(v))
    if kind == K_DOUBLE:
        return double_bits(float(v))
    if kind == K_STRING:
        return v if isinstance(v, str) else str(v)
    if kind == K_BYTES:
        return v if isinstance(v, (bytes, bytearray)) else bytes(v)
    # K_MESSAGE / K_GROUP
    if isinstance(v, Message):
        return v
    return from_object(f.msg_type, v)


def from_object(def_, obj):
    """Builds a tree from a plain dict (proto field names, absent/None keys stay
    absent)."""
    if not isinstance(obj, dict):
        raise CoreError(def_.full_name + ": needs a dict")
    m = Message(def_)
    for slot, f in enumerate(def_.fields):
        v = obj.get(f.name)
        if v is None:
            continue
        try:
            if f.repeated:
                if not isinstance(v, (list, tuple)):
                    raise CoreError("needs a list")
                m.v[slot] = [from_py(f, item) for item in v]
            else:
                set_slot(m, slot, from_py(f, v))
        except CoreError:
            raise
        except Exception as error:  # noqa: BLE001 - name the field, keep the type
            raise CoreError("%s.%s: %s" % (def_.full_name, f.name, error)) from None
    return m


def check_present_required(m, slot):
    """Throws when a REQUIRED field of this message is absent."""
    if m.v[slot] is None and m.def_.fields[slot].presence == REQUIRED:
        f = m.def_.fields[slot]
        raise CoreError("missing required field %s.%s" % (m.def_.full_name, f.name), 6)

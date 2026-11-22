"""FileDescriptorSet reader and schema builder for the pure-Python core: the
only reader of descriptor.proto the pure path needs. It parses just enough for
a binary codec (names, numbers, labels, types, type names, oneofs, extensions,
packedness, proto3 optional, editions features, syntax) and skips everything
else by wire type, so unknown descriptor.proto additions never break loading.

The wire layout knowledge lives here; ``encode``/``decode`` know the message
wire layout and ``schema`` is the public facade. This mirrors
``bindings/node/lib/core/descriptor.ts`` (itself the twin of
``bindings/java/runtime/Proto.java`` and ``GenericCodec.Schema``).
"""

from . import wire
from .wire import (
    MAX_DEPTH,
    WT_LEN,
    WT_VARINT,
    CoreError,
    Reader,
    r_len,
    r_var,
    skip,
    tag_bytes,
)

# ------------------------------------------------- compiled schema (what
# encode/decode read)

K_INT32 = 0
K_UINT32 = 1
K_SINT32 = 2
K_INT64 = 3
K_UINT64 = 4
K_SINT64 = 5
K_BOOL = 6
K_ENUM = 7
K_FIXED32 = 8
K_SFIXED32 = 9
K_FLOAT = 10
K_FIXED64 = 11
K_SFIXED64 = 12
K_DOUBLE = 13
K_STRING = 14
K_BYTES = 15
K_MESSAGE = 16
K_GROUP = 17


class K:
    """Wire kinds of the table engine (its own numbering, not protobuf's)."""

    INT32 = K_INT32
    UINT32 = K_UINT32
    SINT32 = K_SINT32
    INT64 = K_INT64
    UINT64 = K_UINT64
    SINT64 = K_SINT64
    BOOL = K_BOOL
    ENUM = K_ENUM
    FIXED32 = K_FIXED32
    SFIXED32 = K_SFIXED32
    FLOAT = K_FLOAT
    FIXED64 = K_FIXED64
    SFIXED64 = K_SFIXED64
    DOUBLE = K_DOUBLE
    STRING = K_STRING
    BYTES = K_BYTES
    MESSAGE = K_MESSAGE
    GROUP = K_GROUP


# Presence kinds. IMPLICIT = proto3 plain scalar (no has-bit).
IMPLICIT = 0
EXPLICIT = 1
REQUIRED = 2
MESSAGE_PRESENCE = 3


def is_packable(kind):
    """The packable numeric kinds: packed runs cross every one of them."""
    return kind <= K_DOUBLE


class EnumDef:
    __slots__ = ("full_name", "by_name", "by_number", "closed")

    def __init__(self, full_name, by_name, by_number, closed):
        self.full_name = full_name
        self.by_name = by_name
        self.by_number = by_number
        self.closed = closed


class Field:
    __slots__ = (
        "number", "name", "kind", "wt", "elem_wt", "repeated", "packed",
        "oneof", "presence", "msg_type", "enum_type", "enum_closed", "utf8",
        "full_name", "tag", "tag_end",
    )

    def __init__(self, number, name, kind, wt, elem_wt, repeated, packed,
                 oneof, presence, msg_type, enum_type, enum_closed, utf8,
                 full_name, tag, tag_end):
        self.number = number
        self.name = name
        self.kind = kind
        # Wire type of the declared form: LEN for packed numerics.
        self.wt = wt
        # Wire type of a single element (what an unpacked occurrence uses).
        self.elem_wt = elem_wt
        self.repeated = repeated
        self.packed = packed
        # Oneof index, -1 when the field is not in a oneof.
        self.oneof = oneof
        self.presence = presence
        self.msg_type = msg_type
        self.enum_type = enum_type
        self.enum_closed = enum_closed
        # A decoded string must be valid UTF-8 (proto3/editions VERIFY).
        self.utf8 = utf8
        # Extensions only: the global name ``[pkg.name]``.
        self.full_name = full_name
        # Precomputed tag bytes (start-group tag for a group).
        self.tag = tag
        # Groups only: the end-group tag.
        self.tag_end = tag_end

    def __repr__(self):
        return "<Field %s=%d kind=%d>" % (self.name, self.number, self.kind)


class MsgDef:
    __slots__ = (
        "full_name", "fields", "slot_by_fnum", "by_name", "oneof_by_name",
        "oneof_count", "has_required", "deep_required", "map_entry",
        "reserved_names",
    )

    def __init__(self, full_name, fields, slot_by_fnum, by_name, oneof_by_name,
                 oneof_count, has_required, deep_required, map_entry,
                 reserved_names):
        self.full_name = full_name
        self.fields = fields
        # Dense number-to-slot map, or None when field numbers are sparse.
        self.slot_by_fnum = slot_by_fnum
        self.by_name = by_name
        self.oneof_by_name = oneof_by_name
        self.oneof_count = oneof_count
        self.has_required = has_required
        # Some transitively reachable message has a REQUIRED field.
        self.deep_required = deep_required
        # protoc's synthesized map entry (field 1 key, field 2 value).
        self.map_entry = map_entry
        self.reserved_names = reserved_names

    def __repr__(self):
        return "<MsgDef %s fields=%d>" % (self.full_name, len(self.fields))


class CoreSchema:
    __slots__ = ("messages", "enums")

    def __init__(self):
        self.messages = {}
        self.enums = {}

    def message(self, full_name):
        def_ = self.messages.get(full_name)
        if def_ is None:
            raise CoreError("no such message type: " + full_name)
        return def_


class Limits:
    """Bounds for untrusted input; zero fields mean the defaults."""

    __slots__ = ("max_bytes", "max_depth")

    def __init__(self, max_bytes=0, max_depth=0):
        if max_bytes < 0 or max_depth < 0:
            raise CoreError("limits must not be negative")
        self.max_bytes = max_bytes
        self.max_depth = max_depth


DEFAULT_LIMITS = Limits()


def effective_depth(lim):
    if lim is None or lim.max_depth == 0 or lim.max_depth > MAX_DEPTH:
        return MAX_DEPTH
    return lim.max_depth


def check_bytes(lim, n, what):
    if lim is not None and lim.max_bytes > 0 and n > lim.max_bytes:
        raise CoreError("%s exceeds the %d-byte limit" % (what, lim.max_bytes), 5)


# --------------------------------------------- descriptor.proto model

T_DOUBLE = 1
T_FLOAT = 2
T_INT64 = 3
T_UINT64 = 4
T_INT32 = 5
T_FIXED64 = 6
T_FIXED32 = 7
T_BOOL = 8
T_STRING = 9
T_GROUP = 10
T_MESSAGE = 11
T_BYTES = 12
T_UINT32 = 13
T_ENUM = 14
T_SFIXED32 = 15
T_SFIXED64 = 16
T_SINT32 = 17
T_SINT64 = 18

L_OPTIONAL = 1
L_REQUIRED = 2
L_REPEATED = 3


class POneof:
    __slots__ = ("name",)

    def __init__(self):
        self.name = ""


class PField:
    __slots__ = (
        "name", "extendee", "number", "label", "type", "type_name",
        "oneof_index", "proto3_optional", "packed", "has_packed_option",
        "enum_open", "utf8", "message_type", "enum_type", "enum_closed",
        "oneof", "has_presence", "full_name",
    )

    def __init__(self):
        self.name = None
        self.extendee = None
        self.number = 0
        self.label = L_OPTIONAL
        self.type = 0
        self.type_name = None
        self.oneof_index = -1
        self.proto3_optional = False
        self.packed = False
        self.has_packed_option = False
        # Editions FeatureSet.enum_type: 0 unset, 1 OPEN, 2 CLOSED.
        self.enum_open = 0
        # Editions FeatureSet.utf8_validation: 0 unset, 2 VERIFY, 3 NONE.
        self.utf8 = 0
        # Linked: the parsed message/enum and the oneof.
        self.message_type = None
        self.enum_type = None
        self.enum_closed = False
        self.oneof = None
        # Linked: explicit presence (not a proto3 plain scalar).
        self.has_presence = False
        # Linked: the global extension name.
        self.full_name = None


class PEnumValue:
    __slots__ = ("name", "number")

    def __init__(self):
        self.name = None
        self.number = 0


class PEnum:
    __slots__ = ("name", "full_name", "values", "enum_open", "closed")

    def __init__(self):
        self.name = None
        self.full_name = ""
        self.values = []
        self.enum_open = 0
        self.closed = False


class PMessage:
    __slots__ = ("name", "full_name", "map_entry", "fields", "nested",
                 "extensions", "oneofs", "enums", "reserved_names")

    def __init__(self):
        self.name = None
        self.full_name = ""
        self.map_entry = False
        self.fields = []
        self.nested = []
        self.extensions = []
        self.oneofs = []
        self.enums = []
        self.reserved_names = []


class PFile:
    __slots__ = ("name", "pkg", "syntax", "edition", "enum_open", "utf8",
                 "messages", "extensions", "enums")

    def __init__(self):
        self.name = None
        self.pkg = ""
        self.syntax = "proto2"
        # Field 14: an editions file (behaves as proto3 here).
        self.edition = False
        self.enum_open = 0
        self.utf8 = 0
        self.messages = []
        self.extensions = []
        self.enums = []


# -------------------------------------------------------- wire parsing

def _reader(b, off, length):
    if length < 0 or off < 0 or off + length > len(b):
        raise CoreError("descriptor overrun")
    return Reader(b, off, off + length)


def _str(b, off, length):
    return b[off:off + length].decode("utf-8", "replace")


def parse_set(bytes_):
    r = Reader(bytes_)
    files = []
    while r.pos < r.end:
        tag = r_var(r)
        if tag >> 3 == 1 and tag & 7 == WT_LEN:
            n = r_len(r)
            files.append(parse_file(r.b, r.pos, n))
            r.pos += n
        else:
            skip(r, tag & 7)
    return files


def parse_file(b, off, length):
    r = _reader(b, off, length)
    f = PFile()
    while r.pos < r.end:
        tag = r_var(r)
        num = tag >> 3
        wt = tag & 7
        if wt == WT_VARINT:
            # Field 14 is the edition marker; its value is irrelevant here.
            if num == 14:
                r_var(r)
                f.edition = True
            else:
                skip(r, wt)
            continue
        if wt == WT_LEN:
            n = r_len(r)
            if num == 4:
                f.messages.append(parse_message(r.b, r.pos, n, 1))
            elif num == 5:
                f.enums.append(parse_enum(r.b, r.pos, n))
            elif num == 7:
                f.extensions.append(parse_field(r.b, r.pos, n))
            elif num == 8:
                f.enum_open = parse_features(r.b, r.pos, r.pos + n, 50)
                f.utf8 = parse_features_utf8(r.b, r.pos, r.pos + n, 50)
            elif num == 1:
                f.name = _str(r.b, r.pos, n)
            elif num == 2:
                f.pkg = _str(r.b, r.pos, n)
            elif num == 12:
                f.syntax = _str(r.b, r.pos, n)
            r.pos += n
        else:
            skip(r, wt)
    return f


def parse_message(b, off, length, depth):
    # Descriptor nesting is peer data too: a FileDescriptorSet a few tens of KB
    # can nest thousands of DescriptorProtos, so cap it instead of letting the
    # recursion overflow the stack.
    if depth > MAX_DEPTH:
        raise CoreError("descriptor nesting exceeds %d" % MAX_DEPTH)
    r = _reader(b, off, length)
    m = PMessage()
    while r.pos < r.end:
        tag = r_var(r)
        num = tag >> 3
        if tag & 7 != WT_LEN:
            skip(r, tag & 7)
            continue
        n = r_len(r)
        if num == 1:
            m.name = _str(r.b, r.pos, n)
        elif num == 2:
            m.fields.append(parse_field(r.b, r.pos, n))
        elif num == 3:
            m.nested.append(parse_message(r.b, r.pos, n, depth + 1))
        elif num == 4:
            m.enums.append(parse_enum(r.b, r.pos, n))
        elif num == 6:
            m.extensions.append(parse_field(r.b, r.pos, n))
        elif num == 7:
            m.map_entry = parse_map_entry(r.b, r.pos, n)
        elif num == 8:
            m.oneofs.append(parse_oneof(r.b, r.pos, n))
        elif num == 10:
            m.reserved_names.append(_str(r.b, r.pos, n))
        r.pos += n
    if m.name is None:
        raise CoreError("message without a name")
    return m


def parse_oneof(b, off, length):
    r = _reader(b, off, length)
    o = POneof()
    while r.pos < r.end:
        tag = r_var(r)
        if tag >> 3 == 1 and tag & 7 == WT_LEN:
            n = r_len(r)
            o.name = _str(r.b, r.pos, n)
            r.pos += n
        else:
            skip(r, tag & 7)
    return o


def parse_enum(b, off, length):
    r = _reader(b, off, length)
    e = PEnum()
    while r.pos < r.end:
        tag = r_var(r)
        num = tag >> 3
        if tag & 7 != WT_LEN:
            skip(r, tag & 7)
            continue
        n = r_len(r)
        if num == 1:
            e.name = _str(r.b, r.pos, n)
        elif num == 2:
            e.values.append(parse_enum_value(r.b, r.pos, n))
        elif num == 3:
            e.enum_open = parse_features(r.b, r.pos, r.pos + n, 7)
        r.pos += n
    return e


def parse_enum_value(b, off, length):
    r = _reader(b, off, length)
    v = PEnumValue()
    has_number = False
    while r.pos < r.end:
        tag = r_var(r)
        num = tag >> 3
        wt = tag & 7
        if wt == WT_VARINT:
            if num == 2:
                v.number = _as_int32(r_var(r))
                has_number = True
            else:
                skip(r, wt)
        elif wt == WT_LEN:
            n = r_len(r)
            if num == 1:
                v.name = _str(r.b, r.pos, n)
            r.pos += n
        else:
            skip(r, wt)
    if v.name is None or not has_number:
        raise CoreError("enum value without name/number")
    return v


def parse_map_entry(b, off, length):
    """MessageOptions.map_entry is field 7."""
    r = _reader(b, off, length)
    map_entry = False
    while r.pos < r.end:
        tag = r_var(r)
        if tag & 7 != WT_VARINT:
            skip(r, tag & 7)
            continue
        if tag >> 3 == 7:
            map_entry = r_var(r) != 0
        else:
            skip(r, WT_VARINT)
    return map_entry


def parse_features(b, off, end, option_number):
    """Reads one Options message and returns FeatureSet.enum_type (1 OPEN,
    2 CLOSED) when it carries one; 0 means unset."""
    r = _reader(b, off, end - off)
    open_ = 0
    while r.pos < r.end:
        tag = r_var(r)
        num = tag >> 3
        if tag & 7 != WT_LEN:
            skip(r, tag & 7)
            continue
        n = r_len(r)
        if num == option_number:
            open_ = parse_feature_set_enum_type(r.b, r.pos, r.pos + n)
        r.pos += n
    return open_


def parse_features_utf8(b, off, end, option_number):
    """FileOptions.features (50) -> FeatureSet.utf8_validation."""
    r = _reader(b, off, end - off)
    utf8 = 0
    while r.pos < r.end:
        tag = r_var(r)
        num = tag >> 3
        if tag & 7 != WT_LEN:
            skip(r, tag & 7)
            continue
        n = r_len(r)
        if num == option_number:
            utf8 = parse_feature_set_utf8(r.b, r.pos, r.pos + n)
        r.pos += n
    return utf8


def parse_feature_set_enum_type(b, off, end):
    """FeatureSet.enum_type = 2: 1 OPEN or 2 CLOSED."""
    r = _reader(b, off, end - off)
    open_ = 0
    while r.pos < r.end:
        tag = r_var(r)
        if tag >> 3 == 2 and tag & 7 == WT_VARINT:
            v = _as_int32(r_var(r))
            if v == 1 or v == 2:
                open_ = v
        else:
            skip(r, tag & 7)
    return open_


def parse_feature_set_utf8(b, off, end):
    """FeatureSet.utf8_validation = 4: 2 VERIFY or 3 NONE."""
    r = _reader(b, off, end - off)
    utf8 = 0
    while r.pos < r.end:
        tag = r_var(r)
        if tag >> 3 == 4 and tag & 7 == WT_VARINT:
            v = _as_int32(r_var(r))
            if v == 2 or v == 3:
                utf8 = v
        else:
            skip(r, tag & 7)
    return utf8


def parse_field(b, off, length):
    r = _reader(b, off, length)
    f = PField()
    while r.pos < r.end:
        tag = r_var(r)
        num = tag >> 3
        wt = tag & 7
        if wt == WT_VARINT:
            v = r_var(r)
            if num == 3:
                f.number = _as_int32(v)
            elif num == 4:
                f.label = _as_int32(v)
            elif num == 5:
                f.type = _as_int32(v)
            elif num == 9:
                f.oneof_index = _as_int32(v)
            elif num == 17:
                f.proto3_optional = v != 0
        elif wt == WT_LEN:
            n = r_len(r)
            if num == 1:
                f.name = _str(r.b, r.pos, n)
            elif num == 2:
                f.extendee = _str(r.b, r.pos, n)
            elif num == 6:
                f.type_name = _str(r.b, r.pos, n)
            elif num == 8:
                parse_field_options(r.b, r.pos, r.pos + n, f)
            r.pos += n
        else:
            skip(r, wt)
    if f.name is None or f.type == 0 or f.number == 0:
        raise CoreError("field without name/type/number")
    if f.type < T_DOUBLE or f.type > T_SINT64:
        raise CoreError("bad field type %d" % f.type)
    if f.label < L_OPTIONAL or f.label > L_REPEATED:
        raise CoreError("bad label %d" % f.label)
    return f


def parse_field_options(b, off, end, f):
    """FieldOptions: packed (2) and the editions FeatureSet (21)."""
    r = _reader(b, off, end - off)
    while r.pos < r.end:
        tag = r_var(r)
        num = tag >> 3
        if num == 2 and tag & 7 == WT_VARINT:
            f.packed = r_var(r) != 0
            f.has_packed_option = True
        elif num == 21 and tag & 7 == WT_LEN:
            n = r_len(r)
            f.enum_open = parse_feature_set_enum_type(r.b, r.pos, r.pos + n)
            f.utf8 = parse_feature_set_utf8(r.b, r.pos, r.pos + n)
            r.pos += n
        else:
            skip(r, tag & 7)


def _as_int32(v):
    v &= wire.MASK32
    return v - 0x100000000 if v & 0x80000000 else v


# ------------------------------------------------------------ linking

def _index_message(m, scope, by_name, enums):
    m.full_name = m.name if scope == "" else scope + "." + m.name
    if m.full_name in by_name:
        raise CoreError("duplicate message " + m.full_name)
    by_name[m.full_name] = m
    for e in m.enums:
        _index_enum(e, m.full_name, enums)
    for n in m.nested:
        _index_message(n, m.full_name, by_name, enums)


def _index_enum(e, scope, enums):
    e.full_name = e.name if scope == "" else scope + "." + e.name
    if e.full_name in enums:
        raise CoreError("duplicate enum " + e.full_name)
    enums[e.full_name] = e


def _link_enum(e, proto3, file_enum_open):
    """An enum is closed for proto2 files (and editions files asking for it)."""
    if e.name is None:
        raise CoreError("enum without a name")
    if e.enum_open != 0:
        e.closed = e.enum_open == 2
    elif file_enum_open != 0:
        e.closed = file_enum_open == 2
    else:
        e.closed = not proto3


def _link_enums(m, proto3, file_enum_open):
    for e in m.enums:
        _link_enum(e, proto3, file_enum_open)
    for n in m.nested:
        _link_enums(n, proto3, file_enum_open)


def _link_message(m, scope, proto3, file_enum_open, file_utf8, by_name, enums):
    for f in m.fields:
        _link_field(f, scope, proto3, file_utf8, by_name, enums, m)
    for n in m.nested:
        _link_message(n, m.full_name, proto3, file_enum_open, file_utf8,
                      by_name, enums)
    for e in m.extensions:
        _link_field(e, scope, proto3, file_utf8, by_name, enums, None)
        e.full_name = (None if e.name is None
                       else e.name if scope == "" else scope + "." + e.name)


def _is_packable_type(t):
    return not (t == T_STRING or t == T_GROUP or t == T_MESSAGE or t == T_BYTES)


def _link_field(f, scope, proto3, file_utf8, by_name, enums, parent):
    if f.oneof_index >= 0 and parent is not None:
        if f.oneof_index >= len(parent.oneofs):
            raise CoreError("field %s: oneof_index out of range" % f.name)
        f.oneof = parent.oneofs[f.oneof_index]
    if (f.type == T_MESSAGE or f.type == T_GROUP) and f.type_name is not None:
        f.message_type = _resolve(by_name, scope, f.type_name)
        if f.message_type is None:
            raise CoreError("field %s: no such type %s" % (f.name, f.type_name))
    if f.type == T_ENUM and f.type_name is not None:
        f.enum_type = _resolve_enum(enums, scope, f.type_name)
        if f.enum_type is None:
            raise CoreError("field %s: no such enum %s" % (f.name, f.type_name))
        # Editions may close one field in an open file; that wins over the enum
        # type, exactly like the core's per-field enum_open.
        f.enum_closed = (f.enum_open == 2 if f.enum_open != 0
                         else f.enum_type.closed)
    if f.label == L_REPEATED and _is_packable_type(f.type) and not f.has_packed_option:
        f.packed = proto3  # the syntax default
    # utf8_validation: field FeatureSet, then the file's, then the dialect
    # default (VERIFY for proto3/editions, NONE for proto2). Only `string`
    # fields carry it, and only they validate on parse.
    if f.type == T_STRING:
        utf8 = f.utf8 if f.utf8 != 0 else (file_utf8 if file_utf8 != 0
                                           else 2 if proto3 else 3)
        f.utf8 = 2 if utf8 == 2 else 3
    else:
        f.utf8 = 3
    f.has_presence = (
        f.label != L_REPEATED
        and (f.type == T_MESSAGE or f.type == T_GROUP or f.label == L_REQUIRED
             or not proto3 or f.proto3_optional)
    )


def _resolve(by_name, scope, name):
    """Leading-dot names resolve from the root; otherwise search the scope up."""
    if name.startswith("."):
        return by_name.get(name[1:])
    s = scope
    while True:
        m = by_name.get(name if s == "" else s + "." + name)
        if m is not None:
            return m
        # Nested scopes qualify from the inside out: C++ scoping.
        dot = name.find(".")
        if dot >= 0:
            head = name[:dot]
            rest = name[dot + 1:]
            prefix = head if s == "" else s + "." + head
            if prefix in by_name:
                return _resolve(by_name, prefix, rest)
        if s == "":
            return None
        cut = s.rfind(".")
        s = "" if cut < 0 else s[:cut]


def _resolve_enum(by_name, scope, name):
    """The enum counterpart of _resolve: same C++ scoping rules."""
    if name.startswith("."):
        return by_name.get(name[1:])
    s = scope
    while True:
        e = by_name.get(name if s == "" else s + "." + name)
        if e is not None:
            return e
        dot = name.find(".")
        if dot >= 0:
            head = name[:dot]
            rest = name[dot + 1:]
            prefix = head if s == "" else s + "." + head
            if prefix in by_name:
                return _resolve_enum(by_name, prefix, rest)
        if s == "":
            return None
        cut = s.rfind(".")
        s = "" if cut < 0 else s[:cut]


def _link_files(files):
    by_name = {}
    enums = {}
    for f in files:
        for m in f.messages:
            _index_message(m, f.pkg, by_name, enums)
        for e in f.enums:
            _index_enum(e, f.pkg, enums)
    for f in files:
        # Editions files carry syntax "editions" plus field 14: the core treats
        # them as proto3, so packed defaults and presence match.
        proto3 = f.syntax == "proto3" or f.edition
        for e in f.enums:
            _link_enum(e, proto3, f.enum_open)
        # Enums close/open before fields link: a field copies its enum's closed
        # flag, so the enum must be linked first.
        for m in f.messages:
            _link_enums(m, proto3, f.enum_open)
        for m in f.messages:
            _link_message(m, f.pkg, proto3, f.enum_open, f.utf8, by_name, enums)
        for e in f.extensions:
            _link_field(e, f.pkg, proto3, f.utf8, by_name, enums, None)
            e.full_name = (None if e.name is None
                           else e.name if f.pkg == "" else f.pkg + "." + e.name)


# --------------------------------------------- compiled schema building

def _kind_of(t):
    if t == T_INT32:
        return K_INT32
    if t == T_UINT32:
        return K_UINT32
    if t == T_SINT32:
        return K_SINT32
    if t == T_INT64:
        return K_INT64
    if t == T_UINT64:
        return K_UINT64
    if t == T_SINT64:
        return K_SINT64
    if t == T_BOOL:
        return K_BOOL
    if t == T_ENUM:
        return K_ENUM
    if t == T_FIXED32:
        return K_FIXED32
    if t == T_SFIXED32:
        return K_SFIXED32
    if t == T_FLOAT:
        return K_FLOAT
    if t == T_FIXED64:
        return K_FIXED64
    if t == T_SFIXED64:
        return K_SFIXED64
    if t == T_DOUBLE:
        return K_DOUBLE
    if t == T_STRING:
        return K_STRING
    if t == T_BYTES:
        return K_BYTES
    if t == T_MESSAGE:
        return K_MESSAGE
    return K_GROUP


def _wire_type(t, packed):
    if packed:
        return WT_LEN
    if t == T_DOUBLE or t == T_FIXED64 or t == T_SFIXED64:
        return 1
    if t == T_STRING or t == T_BYTES or t == T_MESSAGE:
        return WT_LEN
    if t == T_GROUP:
        return 3
    if t == T_FIXED32 or t == T_SFIXED32 or t == T_FLOAT:
        return 5
    return WT_VARINT


class _Builder:
    def __init__(self):
        self.schema = CoreSchema()

    def build(self, files):
        # Enums first: message fields link their element type while the defs
        # are built.
        for f in files:
            for e in f.enums:
                self._build_enum(e)
            for m in f.messages:
                self._build_enums(m, 1)
        for f in files:
            for m in f.messages:
                self._build_def(m, 1)
        # Extensions attach to their extendee after all defs exist.
        for f in files:
            for e in f.extensions:
                self._attach_extension(e)
            for m in f.messages:
                self._attach_message_extensions(m)
        _finish_transitive(self.schema)
        return self.schema

    def _build_enum(self, e):
        by_name = {}
        by_number = {}
        for v in e.values:
            if v.name in by_name:
                raise CoreError("duplicate enum value %s.%s" % (e.full_name, v.name))
            by_name[v.name] = v.number
            # Aliases repeat a number: the first declared name is printed.
            if v.number not in by_number:
                by_number[v.number] = v.name
        self.schema.enums[e.full_name] = EnumDef(e.full_name, by_name,
                                                 by_number, e.closed)

    def _build_enums(self, m, depth):
        if depth > MAX_DEPTH:
            raise CoreError("descriptor nesting exceeds %d" % MAX_DEPTH)
        for e in m.enums:
            self._build_enum(e)
        for n in m.nested:
            self._build_enums(n, depth + 1)

    def _build_def(self, m, depth):
        if depth > MAX_DEPTH:
            raise CoreError("descriptor nesting exceeds %d" % MAX_DEPTH)
        done = self.schema.messages.get(m.full_name)
        if done is not None:
            return done
        d = MsgDef(m.full_name, [], None, {}, {}, 0, False, False,
                   m.map_entry, list(m.reserved_names))
        self.schema.messages[m.full_name] = d  # before fields: recursion-safe
        fields = []
        oneofs = {}
        for f in m.fields:
            fields.append(self._build_field(f, oneofs))
        _finish_def(d, fields, oneofs)
        for n in m.nested:
            self._build_def(n, depth + 1)
        return d

    def _build_field(self, f, oneofs):
        kind = _kind_of(f.type)
        repeated = f.label == L_REPEATED
        oneof = -1
        if f.oneof is not None:
            idx = oneofs.get(f.oneof)
            if idx is None:
                oneof = len(oneofs)
                oneofs[f.oneof] = oneof
            else:
                oneof = idx
        if oneof >= 0:
            presence = EXPLICIT  # reads/writes go through the oneof case
        elif f.label == L_REQUIRED:
            presence = REQUIRED
        elif kind == K_MESSAGE or kind == K_GROUP:
            presence = MESSAGE_PRESENCE
        elif f.has_presence:
            presence = EXPLICIT
        else:
            presence = IMPLICIT
        packed = repeated and f.packed
        wt = _wire_type(f.type, packed)
        number = f.number
        return Field(
            number=number,
            name=f.name,
            kind=kind,
            wt=wt,
            elem_wt=_wire_type(f.type, False),
            repeated=repeated,
            packed=packed,
            oneof=oneof,
            presence=presence,
            msg_type=None if f.message_type is None else self._build_def(f.message_type, 1),
            enum_type=None if f.enum_type is None else self.schema.enums.get(f.enum_type.full_name),
            enum_closed=f.enum_closed,
            utf8=f.type == T_STRING and f.utf8 == 2,
            full_name=f.full_name,
            tag=tag_bytes(number, wt),
            tag_end=tag_bytes(number, 4) if kind == K_GROUP else None,
        )

    def _attach_message_extensions(self, m):
        for e in m.extensions:
            self._attach_extension(e)
        for n in m.nested:
            self._attach_message_extensions(n)

    def _attach_extension(self, e):
        extendee = e.extendee
        if extendee is None:
            raise CoreError("extension without extendee")
        if extendee.startswith("."):
            extendee = extendee[1:]
        d = self.schema.messages.get(extendee)
        if d is None:
            raise CoreError("extendee missing: " + e.extendee)
        s = self._build_field(e, {})
        s.full_name = e.full_name
        fields = list(d.fields)
        fields.append(s)
        oneof_by_name = d.oneof_by_name
        count = d.oneof_count
        _finish_def(d, fields, {})
        d.oneof_by_name = oneof_by_name  # extensions are never in a oneof
        d.oneof_count = count


def _finish_def(d, fields, oneofs):
    # Stable sort by field number: the slots decode into are field-number
    # ordered, like the C core's table.
    fields.sort(key=lambda f: f.number)
    max_number = 0
    has_required = False
    for f in fields:
        if f.number > max_number:
            max_number = f.number
        if f.presence == REQUIRED:
            has_required = True
    d.has_required = has_required
    d.fields = fields
    # Dense only when bounded, like the C core's fnum map: one field numbered
    # 536870911 must not allocate a 2 GiB list.
    if max_number <= 512 and max_number <= len(fields) * 8 + 8:
        slot_by_fnum = [-1] * (max_number + 1)
        for i, f in enumerate(fields):
            slot_by_fnum[f.number] = i
        d.slot_by_fnum = slot_by_fnum
    else:
        d.slot_by_fnum = None
    d.by_name = {}
    for i, f in enumerate(fields):
        d.by_name[f.name] = i
    d.oneof_by_name = {oneof.name: slot for oneof, slot in oneofs.items()}
    d.oneof_count = max(d.oneof_count, len(oneofs))


def slot_of(def_, fnum):
    """Slot of a field number, -1 when the message has no such field."""
    slots = def_.slot_by_fnum
    if slots is not None:
        if 0 <= fnum < len(slots):
            return slots[fnum]
        return -1
    for i, f in enumerate(def_.fields):
        if f.number == fnum:
            return i
    return -1


def _finish_transitive(schema):
    changed = True
    while changed:
        changed = False
        for d in schema.messages.values():
            if not d.deep_required and _needs_required(d):
                d.deep_required = True
                changed = True


def _needs_required(d):
    for f in d.fields:
        if f.presence == REQUIRED:
            return True
        if ((f.kind == K_MESSAGE or f.kind == K_GROUP)
                and f.msg_type is not None and f.msg_type.deep_required):
            return True
    return False


# ------------------------------------------------------ public entry

def load_schema(bytes_, limits=None):
    """Parses and links a FileDescriptorSet into a compiled schema."""
    check_bytes(limits, len(bytes_), "descriptor set")
    try:
        files = parse_set(bytes_)
        _link_files(files)
    except CoreError as e:
        raise CoreError("descriptor set: " + str(e), 1) from None
    return _Builder().build(files)


def field_count(def_):
    """Number of declared fields in a message (descriptor test surface)."""
    return len(def_.fields)


def field_by_name(def_, name):
    """The field named ``name``, or None."""
    slot = def_.by_name.get(name)
    return None if slot is None else def_.fields[slot]

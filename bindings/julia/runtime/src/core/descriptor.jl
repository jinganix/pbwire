# FileDescriptorSet reader and schema builder for the pure-Julia core: the
# only reader of descriptor.proto the pure path needs. It parses just enough
# for a binary codec (names, numbers, labels, types, type names, oneofs,
# extensions, packedness, proto3 optional, editions features, syntax) and
# skips everything else by wire type, so unknown descriptor.proto additions
# never break loading.
#
# The wire layout knowledge lives here; `encode.jl`/`decode.jl` know the
# message wire layout and `schema.jl` is the public facade.

# ------------------------------------------------- compiled schema (what the
# encoder and decoder read)

const K_INT32 = 0
const K_UINT32 = 1
const K_SINT32 = 2
const K_INT64 = 3
const K_UINT64 = 4
const K_SINT64 = 5
const K_BOOL = 6
const K_ENUM = 7
const K_FIXED32 = 8
const K_SFIXED32 = 9
const K_FLOAT = 10
const K_FIXED64 = 11
const K_SFIXED64 = 12
const K_DOUBLE = 13
const K_STRING = 14
const K_BYTES = 15
const K_MESSAGE = 16
const K_GROUP = 17

"""The packable numeric kinds: packed runs cross every one of them."""
@inline is_packable(kind::Int) = kind <= K_DOUBLE

# Presence kinds. IMPLICIT = proto3 plain scalar (no has-bit).
const IMPLICIT = 0
const EXPLICIT = 1
const REQUIRED = 2
const MESSAGE_PRESENCE = 3

mutable struct EnumDef
    full_name::String
    by_name::Dict{String,Int64}
    by_number::Dict{Int64,String}
    closed::Bool
end

# MsgDef is declared before Field because a message field points at a
# message definition and a message definition lists fields (a cycle), so the
# forward reference is `Any` on the message side and concrete on the field
# side (`Union{MsgDef,Nothing}`).
mutable struct MsgDef
    full_name::String
    fields::Vector{Any}
    slot_by_fnum::Union{Nothing,Vector{Int}}
    by_name::Dict{String,Int}
    oneof_by_name::Dict{String,Int}
    oneof_count::Int
    has_required::Bool
    deep_required::Bool
    map_entry::Bool
    reserved_names::Vector{String}
end

MsgDef(full_name::AbstractString, map_entry::Bool, reserved::Vector{String}) =
    MsgDef(String(full_name), Any[], nothing, Dict{String,Int}(),
        Dict{String,Int}(), 0, false, false, map_entry, reserved)

struct Field
    number::Int
    name::String
    kind::Int
    # Wire type of the declared form: LEN for packed numerics.
    wt::Int
    # Wire type of a single element (what an unpacked occurrence uses).
    elem_wt::Int
    repeated::Bool
    packed::Bool
    # Oneof index, -1 when the field is not in a oneof.
    oneof::Int
    presence::Int
    msg_type::Union{MsgDef,Nothing}
    enum_type::Union{EnumDef,Nothing}
    enum_closed::Bool
    # A decoded string must be valid UTF-8 (proto3/editions VERIFY).
    utf8::Bool
    # Extensions only: the global name `[pkg.name]`.
    full_name::Union{Nothing,String}
    # Precomputed tag bytes (the start-group tag for a group).
    tag::Vector{UInt8}
    # Groups only: the end-group tag.
    tag_end::Vector{UInt8}
end

function Field(number::Int, name::AbstractString, kind::Int, wt::Int,
        elem_wt::Int, repeated::Bool, packed::Bool, oneof::Int, presence::Int,
        msg_type::Union{MsgDef,Nothing}, enum_type::Union{EnumDef,Nothing},
        enum_closed::Bool, utf8::Bool, full_name::Union{Nothing,AbstractString})
    tag_end = kind == K_GROUP ? tag_bytes(number, WT_END_GROUP) : UInt8[]
    return Field(number, String(name), kind, wt, elem_wt, repeated, packed,
        oneof, presence, msg_type, enum_type, enum_closed, utf8,
        full_name === nothing ? nothing : String(full_name),
        tag_bytes(number, wt), tag_end)
end

Base.show(io::IO, f::Field) =
    print(io, "<Field ", f.name, "=", f.number, " kind=", f.kind, ">")

struct CoreSchema
    messages::Dict{String,MsgDef}
    enums::Dict{String,EnumDef}
end

CoreSchema() = CoreSchema(Dict{String,MsgDef}(), Dict{String,EnumDef}())

function message(s::CoreSchema, full_name::AbstractString)
    def = get(s.messages, String(full_name), nothing)
    def === nothing && throw(CoreError("no such message type: $full_name"))
    return def
end

function enum(s::CoreSchema, full_name::AbstractString)
    def = get(s.enums, String(full_name), nothing)
    def === nothing && throw(CoreError("no such enum type: $full_name"))
    return def
end

"""Bounds for untrusted input; zero fields mean the defaults."""
struct Limits
    max_bytes::Int
    max_depth::Int
    function Limits(max_bytes::Int = 0, max_depth::Int = 0)
        if max_bytes < 0 || max_depth < 0
            throw(CoreError("limits must not be negative"))
        end
        return new(max_bytes, max_depth)
    end
end

const DEFAULT_LIMITS = Limits()

function effective_depth(lim::Union{Nothing,Limits})
    if lim === nothing || lim.max_depth == 0 || lim.max_depth > MAX_DEPTH
        return MAX_DEPTH
    end
    return lim.max_depth
end

function check_bytes(lim::Union{Nothing,Limits}, n::Int, what::AbstractString)
    if lim !== nothing && lim.max_bytes > 0 && n > lim.max_bytes
        throw(CoreError("$what exceeds the $(lim.max_bytes)-byte limit", 5))
    end
    return nothing
end

# --------------------------------------------- descriptor.proto model

const T_DOUBLE = 1
const T_FLOAT = 2
const T_INT64 = 3
const T_UINT64 = 4
const T_INT32 = 5
const T_FIXED64 = 6
const T_FIXED32 = 7
const T_BOOL = 8
const T_STRING = 9
const T_GROUP = 10
const T_MESSAGE = 11
const T_BYTES = 12
const T_UINT32 = 13
const T_ENUM = 14
const T_SFIXED32 = 15
const T_SFIXED64 = 16
const T_SINT32 = 17
const T_SINT64 = 18

const L_OPTIONAL = 1
const L_REQUIRED = 2
const L_REPEATED = 3

mutable struct POneof
    name::Union{Nothing,String}
end
POneof() = POneof(nothing)

mutable struct PField
    name::Union{Nothing,String}
    extendee::Union{Nothing,String}
    number::Int
    label::Int
    type::Int
    type_name::Union{Nothing,String}
    oneof_index::Int
    proto3_optional::Bool
    packed::Bool
    has_packed_option::Bool
    # Editions FeatureSet.enum_type: 0 unset, 1 OPEN, 2 CLOSED.
    enum_open::Int
    # Editions FeatureSet.utf8_validation: 0 unset, 2 VERIFY, 3 NONE.
    utf8::Int
    # Linked types (`PMessage`/`PEnum`): forward references stay `Any` because
    # the parsed model is mutually recursive.
    message_type::Any
    enum_type::Any
    enum_closed::Bool
    oneof::Union{Nothing,POneof}
    has_presence::Bool
    full_name::Union{Nothing,String}
end

mutable struct PEnumValue
    name::Union{Nothing,String}
    number::Int
end
PEnumValue() = PEnumValue(nothing, 0)

mutable struct PEnum
    name::Union{Nothing,String}
    full_name::String
    values::Vector{PEnumValue}
    enum_open::Int
    closed::Bool
end
PEnum() = PEnum(nothing, "", PEnumValue[], 0, false)

mutable struct PMessage
    name::Union{Nothing,String}
    full_name::String
    map_entry::Bool
    fields::Vector{PField}
    nested::Vector{PMessage}
    extensions::Vector{PField}
    oneofs::Vector{POneof}
    enums::Vector{PEnum}
    reserved_names::Vector{String}
end

PMessage() = PMessage(nothing, "", false, PField[], PMessage[], PField[],
    POneof[], PEnum[], String[])

mutable struct PFile
    name::Union{Nothing,String}
    pkg::String
    syntax::String
    # Field 14: an editions file (behaves as proto3 here).
    edition::Bool
    enum_open::Int
    utf8::Int
    messages::Vector{PMessage}
    extensions::Vector{PField}
    enums::Vector{PEnum}
end

PFile() = PFile(nothing, "", "proto2", false, 0, 0, PMessage[], PField[],
    PEnum[])

PField() = PField(nothing, nothing, 0, L_OPTIONAL, 0, nothing, -1, false,
    false, false, 0, 0, nothing, nothing, false, nothing, false, nothing)

# -------------------------------------------------------- wire parsing

function _reader(b::Vector{UInt8}, off::Int, len::Int)
    if len < 0 || off < 1 || off + len - 1 > length(b)
        throw(CoreError("descriptor overrun"))
    end
    return Reader(b, off, off + len - 1)
end

_str(b::Vector{UInt8}, off::Int, len::Int) = String(b[off:(off + len - 1)])

function parse_set(bytes_::Vector{UInt8})
    r = Reader(bytes_)
    files = PFile[]
    while r.pos <= r.stop
        tag = r_var(r)
        if Int(tag >> 3) == 1 && Int(tag & 0x7) == WT_LEN
            n = r_len(r)
            push!(files, parse_file(r.b, r.pos, n))
            r.pos += n
        else
            skip(r, Int(tag & 0x7))
        end
    end
    return files
end

function parse_file(b::Vector{UInt8}, off::Int, len::Int)
    r = _reader(b, off, len)
    f = PFile()
    while r.pos <= r.stop
        tag = r_var(r)
        num = Int(tag >> 3)
        wt = Int(tag & 0x7)
        if wt == WT_VARINT
            # Field 14 is the edition marker; its value is irrelevant here.
            if num == 14
                r_var(r)
                f.edition = true
            else
                skip(r, wt)
            end
            continue
        end
        if wt == WT_LEN
            n = r_len(r)
            if num == 4
                push!(f.messages, parse_message(r.b, r.pos, n, 1))
            elseif num == 5
                push!(f.enums, parse_enum(r.b, r.pos, n))
            elseif num == 7
                push!(f.extensions, parse_field(r.b, r.pos, n))
            elseif num == 8
                f.enum_open = parse_features(r.b, r.pos, n, 50)
                f.utf8 = parse_features_utf8(r.b, r.pos, n, 50)
            elseif num == 1
                f.name = _str(r.b, r.pos, n)
            elseif num == 2
                f.pkg = _str(r.b, r.pos, n)
            elseif num == 12
                f.syntax = _str(r.b, r.pos, n)
            end
            r.pos += n
        else
            skip(r, wt)
        end
    end
    return f
end

function parse_message(b::Vector{UInt8}, off::Int, len::Int, depth::Int)
    # Descriptor nesting is peer data too: a FileDescriptorSet a few tens of KB
    # can nest thousands of DescriptorProtos, so cap it instead of letting the
    # recursion overflow the stack.
    depth > MAX_DEPTH &&
        throw(CoreError("descriptor nesting exceeds $MAX_DEPTH"))
    r = _reader(b, off, len)
    m = PMessage()
    while r.pos <= r.stop
        tag = r_var(r)
        num = Int(tag >> 3)
        if Int(tag & 0x7) != WT_LEN
            skip(r, Int(tag & 0x7))
            continue
        end
        n = r_len(r)
        if num == 1
            m.name = _str(r.b, r.pos, n)
        elseif num == 2
            push!(m.fields, parse_field(r.b, r.pos, n))
        elseif num == 3
            push!(m.nested, parse_message(r.b, r.pos, n, depth + 1))
        elseif num == 4
            push!(m.enums, parse_enum(r.b, r.pos, n))
        elseif num == 6
            push!(m.extensions, parse_field(r.b, r.pos, n))
        elseif num == 7
            m.map_entry = parse_map_entry(r.b, r.pos, n)
        elseif num == 8
            push!(m.oneofs, parse_oneof(r.b, r.pos, n))
        elseif num == 10
            push!(m.reserved_names, _str(r.b, r.pos, n))
        end
        r.pos += n
    end
    m.name === nothing && throw(CoreError("message without a name"))
    return m
end

function parse_oneof(b::Vector{UInt8}, off::Int, len::Int)
    r = _reader(b, off, len)
    o = POneof()
    while r.pos <= r.stop
        tag = r_var(r)
        if Int(tag >> 3) == 1 && Int(tag & 0x7) == WT_LEN
            n = r_len(r)
            o.name = _str(r.b, r.pos, n)
            r.pos += n
        else
            skip(r, Int(tag & 0x7))
        end
    end
    return o
end

function parse_enum(b::Vector{UInt8}, off::Int, len::Int)
    r = _reader(b, off, len)
    e = PEnum()
    while r.pos <= r.stop
        tag = r_var(r)
        num = Int(tag >> 3)
        if Int(tag & 0x7) != WT_LEN
            skip(r, Int(tag & 0x7))
            continue
        end
        n = r_len(r)
        if num == 1
            e.name = _str(r.b, r.pos, n)
        elseif num == 2
            push!(e.values, parse_enum_value(r.b, r.pos, n))
        elseif num == 3
            e.enum_open = parse_features(r.b, r.pos, n, 7)
        end
        r.pos += n
    end
    return e
end

function parse_enum_value(b::Vector{UInt8}, off::Int, len::Int)
    r = _reader(b, off, len)
    v = PEnumValue()
    has_number = false
    while r.pos <= r.stop
        tag = r_var(r)
        num = Int(tag >> 3)
        wt = Int(tag & 0x7)
        if wt == WT_VARINT
            if num == 2
                v.number = Int(signed32(r_var(r)))
                has_number = true
            else
                skip(r, wt)
            end
        elseif wt == WT_LEN
            n = r_len(r)
            if num == 1
                v.name = _str(r.b, r.pos, n)
            end
            r.pos += n
        else
            skip(r, wt)
        end
    end
    if v.name === nothing || !has_number
        throw(CoreError("enum value without name/number"))
    end
    return v
end

"""MessageOptions.map_entry is field 7."""
function parse_map_entry(b::Vector{UInt8}, off::Int, len::Int)
    r = _reader(b, off, len)
    map_entry = false
    while r.pos <= r.stop
        tag = r_var(r)
        if Int(tag & 0x7) != WT_VARINT
            skip(r, Int(tag & 0x7))
            continue
        end
        if Int(tag >> 3) == 7
            map_entry = r_var(r) != 0
        else
            skip(r, WT_VARINT)
        end
    end
    return map_entry
end

"""Reads one Options message and returns FeatureSet.enum_type (1 OPEN, 2
CLOSED) when it carries one; 0 means unset."""
function parse_features(b::Vector{UInt8}, off::Int, len::Int, option_number::Int)
    r = _reader(b, off, len)
    open_ = 0
    while r.pos <= r.stop
        tag = r_var(r)
        num = Int(tag >> 3)
        if Int(tag & 0x7) != WT_LEN
            skip(r, Int(tag & 0x7))
            continue
        end
        n = r_len(r)
        if num == option_number
            open_ = parse_feature_set_enum_type(r.b, r.pos, n)
        end
        r.pos += n
    end
    return open_
end

"""FileOptions.features (50) -> FeatureSet.utf8_validation."""
function parse_features_utf8(b::Vector{UInt8}, off::Int, len::Int,
        option_number::Int)
    r = _reader(b, off, len)
    utf8 = 0
    while r.pos <= r.stop
        tag = r_var(r)
        num = Int(tag >> 3)
        if Int(tag & 0x7) != WT_LEN
            skip(r, Int(tag & 0x7))
            continue
        end
        n = r_len(r)
        if num == option_number
            utf8 = parse_feature_set_utf8(r.b, r.pos, n)
        end
        r.pos += n
    end
    return utf8
end

"""FeatureSet.enum_type = 2: 1 OPEN or 2 CLOSED."""
function parse_feature_set_enum_type(b::Vector{UInt8}, off::Int, len::Int)
    r = _reader(b, off, len)
    open_ = 0
    while r.pos <= r.stop
        tag = r_var(r)
        if Int(tag >> 3) == 2 && Int(tag & 0x7) == WT_VARINT
            v = Int(signed32(r_var(r)))
            if v == 1 || v == 2
                open_ = v
            end
        else
            skip(r, Int(tag & 0x7))
        end
    end
    return open_
end

"""FeatureSet.utf8_validation = 4: 2 VERIFY or 3 NONE."""
function parse_feature_set_utf8(b::Vector{UInt8}, off::Int, len::Int)
    r = _reader(b, off, len)
    utf8 = 0
    while r.pos <= r.stop
        tag = r_var(r)
        if Int(tag >> 3) == 4 && Int(tag & 0x7) == WT_VARINT
            v = Int(signed32(r_var(r)))
            if v == 2 || v == 3
                utf8 = v
            end
        else
            skip(r, Int(tag & 0x7))
        end
    end
    return utf8
end

function parse_field(b::Vector{UInt8}, off::Int, len::Int)
    r = _reader(b, off, len)
    f = PField()
    while r.pos <= r.stop
        tag = r_var(r)
        num = Int(tag >> 3)
        wt = Int(tag & 0x7)
        if wt == WT_VARINT
            v = r_var(r)
            if num == 3
                f.number = Int(signed32(v))
            elseif num == 4
                f.label = Int(signed32(v))
            elseif num == 5
                f.type = Int(signed32(v))
            elseif num == 9
                f.oneof_index = Int(signed32(v))
            elseif num == 17
                f.proto3_optional = v != 0
            end
        elseif wt == WT_LEN
            n = r_len(r)
            if num == 1
                f.name = _str(r.b, r.pos, n)
            elseif num == 2
                f.extendee = _str(r.b, r.pos, n)
            elseif num == 6
                f.type_name = _str(r.b, r.pos, n)
            elseif num == 8
                parse_field_options(r.b, r.pos, n, f)
            end
            r.pos += n
        else
            skip(r, wt)
        end
    end
    if f.name === nothing || f.type == 0 || f.number == 0
        throw(CoreError("field without name/type/number"))
    end
    if f.type < T_DOUBLE || f.type > T_SINT64
        throw(CoreError("bad field type $(f.type)"))
    end
    if f.label < L_OPTIONAL || f.label > L_REPEATED
        throw(CoreError("bad label $(f.label)"))
    end
    return f
end

"""FieldOptions: packed (2) and the editions FeatureSet (21)."""
function parse_field_options(b::Vector{UInt8}, off::Int, len::Int, f::PField)
    r = _reader(b, off, len)
    while r.pos <= r.stop
        tag = r_var(r)
        num = Int(tag >> 3)
        if num == 2 && Int(tag & 0x7) == WT_VARINT
            f.packed = r_var(r) != 0
            f.has_packed_option = true
        elseif num == 21 && Int(tag & 0x7) == WT_LEN
            n = r_len(r)
            f.enum_open = parse_feature_set_enum_type(r.b, r.pos, n)
            f.utf8 = parse_feature_set_utf8(r.b, r.pos, n)
            r.pos += n
        else
            skip(r, Int(tag & 0x7))
        end
    end
    return nothing
end

# ------------------------------------------------------------ linking

function _index_message(m::PMessage, scope::String,
        by_name::Dict{String,PMessage}, enums::Dict{String,PEnum})
    m.full_name = scope == "" ? m.name : scope * "." * m.name
    haskey(by_name, m.full_name) &&
        throw(CoreError("duplicate message " * m.full_name))
    by_name[m.full_name] = m
    for e in m.enums
        _index_enum(e, m.full_name, enums)
    end
    for n in m.nested
        _index_message(n, m.full_name, by_name, enums)
    end
    return nothing
end

function _index_enum(e::PEnum, scope::String,
        enums::Dict{String,PEnum})
    e.full_name = scope == "" ? e.name : scope * "." * e.name
    haskey(enums, e.full_name) &&
        throw(CoreError("duplicate enum " * e.full_name))
    enums[e.full_name] = e
    return nothing
end

"""An enum is closed for proto2 files (and editions files asking for it)."""
function _link_enum(e::PEnum, proto3::Bool, file_enum_open::Int)
    e.name === nothing && throw(CoreError("enum without a name"))
    if e.enum_open != 0
        e.closed = e.enum_open == 2
    elseif file_enum_open != 0
        e.closed = file_enum_open == 2
    else
        e.closed = !proto3
    end
    return nothing
end

function _link_enums(m::PMessage, proto3::Bool, file_enum_open::Int)
    for e in m.enums
        _link_enum(e, proto3, file_enum_open)
    end
    for n in m.nested
        _link_enums(n, proto3, file_enum_open)
    end
    return nothing
end

function _link_message(m::PMessage, scope::String, proto3::Bool,
        file_enum_open::Int, file_utf8::Int,
        by_name::Dict{String,PMessage}, enums::Dict{String,PEnum})
    for f in m.fields
        _link_field(f, scope, proto3, file_utf8, by_name, enums, m)
    end
    for n in m.nested
        _link_message(n, m.full_name, proto3, file_enum_open, file_utf8,
            by_name, enums)
    end
    for e in m.extensions
        _link_field(e, scope, proto3, file_utf8, by_name, enums, nothing)
        e.full_name = (e.name === nothing ? nothing :
                       scope == "" ? e.name : scope * "." * e.name)
    end
    return nothing
end

@inline _is_packable_type(t::Int) =
    !(t == T_STRING || t == T_GROUP || t == T_MESSAGE || t == T_BYTES)

function _link_field(f::PField, scope::String, proto3::Bool, file_utf8::Int,
        by_name::Dict{String,PMessage}, enums::Dict{String,PEnum},
        parent::Union{Nothing,PMessage})
    if f.oneof_index >= 0 && parent !== nothing
        if f.oneof_index >= length(parent.oneofs)
            throw(CoreError("field $(f.name): oneof_index out of range"))
        end
        f.oneof = parent.oneofs[f.oneof_index + 1]
    end
    if (f.type == T_MESSAGE || f.type == T_GROUP) && f.type_name !== nothing
        f.message_type = _resolve(by_name, scope, f.type_name)
        if f.message_type === nothing
            throw(CoreError("field $(f.name): no such type $(f.type_name)"))
        end
    end
    if f.type == T_ENUM && f.type_name !== nothing
        f.enum_type = _resolve_enum(enums, scope, f.type_name)
        if f.enum_type === nothing
            throw(CoreError("field $(f.name): no such enum $(f.type_name)"))
        end
        # Editions may close one field in an open file; that wins over the enum
        # type, exactly like the core's per-field enum_open.
        f.enum_closed = f.enum_open != 0 ? f.enum_open == 2 : f.enum_type.closed
    end
    if f.label == L_REPEATED && _is_packable_type(f.type) &&
       !f.has_packed_option
        f.packed = proto3 # the syntax default
    end
    # utf8_validation: field FeatureSet, then the file's, then the dialect
    # default (VERIFY for proto3/editions, NONE for proto2). Only `string`
    # fields carry it, and only they validate on parse.
    if f.type == T_STRING
        utf8 = f.utf8 != 0 ? f.utf8 :
               (file_utf8 != 0 ? file_utf8 : (proto3 ? 2 : 3))
        f.utf8 = utf8 == 2 ? 2 : 3
    else
        f.utf8 = 3
    end
    f.has_presence = f.label != L_REPEATED &&
                     (f.type == T_MESSAGE || f.type == T_GROUP ||
                      f.label == L_REQUIRED || !proto3 || f.proto3_optional)
    return nothing
end

"""Leading-dot names resolve from the root; otherwise search the scope up."""
function _resolve(by_name::Dict{String,PMessage}, scope::String,
        name::String)
    if startswith(name, ".")
        return get(by_name, name[2:end], nothing)
    end
    s = scope
    while true
        m = get(by_name, s == "" ? name : s * "." * name, nothing)
        m !== nothing && return m
        # Nested scopes qualify from the inside out: C++ scoping.
        dot = findfirst('.', name)
        if dot !== nothing
            head = name[1:(dot - 1)]
            rest = name[(dot + 1):end]
            prefix = s == "" ? head : s * "." * head
            if haskey(by_name, prefix)
                return _resolve(by_name, prefix, rest)
            end
        end
        s == "" && return nothing
        cut = findlast('.', s)
        s = cut === nothing ? "" : s[1:(cut - 1)]
    end
end

"""The enum counterpart of `_resolve`: same C++ scoping rules."""
function _resolve_enum(by_name::Dict{String,PEnum}, scope::String,
        name::String)
    if startswith(name, ".")
        return get(by_name, name[2:end], nothing)
    end
    s = scope
    while true
        e = get(by_name, s == "" ? name : s * "." * name, nothing)
        e !== nothing && return e
        dot = findfirst('.', name)
        if dot !== nothing
            head = name[1:(dot - 1)]
            rest = name[(dot + 1):end]
            prefix = s == "" ? head : s * "." * head
            if haskey(by_name, prefix)
                return _resolve_enum(by_name, prefix, rest)
            end
        end
        s == "" && return nothing
        cut = findlast('.', s)
        s = cut === nothing ? "" : s[1:(cut - 1)]
    end
end

function _link_files(files::Vector{PFile})
    by_name = Dict{String,PMessage}()
    enums = Dict{String,PEnum}()
    for f in files
        for m in f.messages
            _index_message(m, f.pkg, by_name, enums)
        end
        for e in f.enums
            _index_enum(e, f.pkg, enums)
        end
    end
    for f in files
        # Editions files carry syntax "editions" plus field 14: the core treats
        # them as proto3, so packed defaults and presence match.
        proto3 = f.syntax == "proto3" || f.edition
        for e in f.enums
            _link_enum(e, proto3, f.enum_open)
        end
        # Enums close/open before fields link: a field copies its enum's closed
        # flag, so the enum must be linked first.
        for m in f.messages
            _link_enums(m, proto3, f.enum_open)
        end
        for m in f.messages
            _link_message(m, f.pkg, proto3, f.enum_open, f.utf8, by_name, enums)
        end
        for e in f.extensions
            _link_field(e, f.pkg, proto3, f.utf8, by_name, enums, nothing)
            e.full_name = (e.name === nothing ? nothing :
                           f.pkg == "" ? e.name : f.pkg * "." * e.name)
        end
    end
    return nothing
end

# --------------------------------------------- compiled schema building

function _kind_of(t::Int)
    t == T_INT32 && return K_INT32
    t == T_UINT32 && return K_UINT32
    t == T_SINT32 && return K_SINT32
    t == T_INT64 && return K_INT64
    t == T_UINT64 && return K_UINT64
    t == T_SINT64 && return K_SINT64
    t == T_BOOL && return K_BOOL
    t == T_ENUM && return K_ENUM
    t == T_FIXED32 && return K_FIXED32
    t == T_SFIXED32 && return K_SFIXED32
    t == T_FLOAT && return K_FLOAT
    t == T_FIXED64 && return K_FIXED64
    t == T_SFIXED64 && return K_SFIXED64
    t == T_DOUBLE && return K_DOUBLE
    t == T_STRING && return K_STRING
    t == T_BYTES && return K_BYTES
    t == T_MESSAGE && return K_MESSAGE
    return K_GROUP
end

function _wire_type(t::Int, packed::Bool)
    packed && return WT_LEN
    (t == T_DOUBLE || t == T_FIXED64 || t == T_SFIXED64) && return WT_FIXED64
    (t == T_STRING || t == T_BYTES || t == T_MESSAGE) && return WT_LEN
    t == T_GROUP && return WT_START_GROUP
    (t == T_FIXED32 || t == T_SFIXED32 || t == T_FLOAT) && return WT_FIXED32
    return WT_VARINT
end

mutable struct _Builder
    schema::CoreSchema
end

_Builder() = _Builder(CoreSchema())

function build(b::_Builder, files::Vector{PFile})
    # Enums first: message fields link their element type while the defs are
    # built.
    for f in files
        for e in f.enums
            _build_enum(b, e)
        end
        for m in f.messages
            _build_enums(b, m, 1)
        end
    end
    for f in files
        for m in f.messages
            _build_def(b, m, 1)
        end
    end
    # Extensions attach to their extendee after all defs exist.
    for f in files
        for e in f.extensions
            _attach_extension(b, e)
        end
        for m in f.messages
            _attach_message_extensions(b, m)
        end
    end
    _finish_transitive(b.schema)
    return b.schema
end

function _build_enum(b::_Builder, e::PEnum)
    by_name = Dict{String,Int64}()
    by_number = Dict{Int64,String}()
    for v in e.values
        if haskey(by_name, v.name)
            throw(CoreError("duplicate enum value $(e.full_name).$(v.name)"))
        end
        by_name[v.name] = v.number
        # Aliases repeat a number: the first declared name is printed.
        if !haskey(by_number, v.number)
            by_number[v.number] = v.name
        end
    end
    b.schema.enums[e.full_name] =
        EnumDef(e.full_name, by_name, by_number, e.closed)
    return nothing
end

function _build_enums(b::_Builder, m::PMessage, depth::Int)
    depth > MAX_DEPTH &&
        throw(CoreError("descriptor nesting exceeds $MAX_DEPTH"))
    for e in m.enums
        _build_enum(b, e)
    end
    for n in m.nested
        _build_enums(b, n, depth + 1)
    end
    return nothing
end

function _build_def(b::_Builder, m::PMessage, depth::Int)
    depth > MAX_DEPTH &&
        throw(CoreError("descriptor nesting exceeds $MAX_DEPTH"))
    done = get(b.schema.messages, m.full_name, nothing)
    done !== nothing && return done
    d = MsgDef(m.full_name, m.map_entry, copy(m.reserved_names))
    b.schema.messages[m.full_name] = d # before fields: recursion-safe
    fields = Any[]
    oneofs = Dict{POneof,Int}()
    for f in m.fields
        push!(fields, _build_field(b, f, oneofs))
    end
    _finish_def(d, fields, oneofs)
    for n in m.nested
        _build_def(b, n, depth + 1)
    end
    return d
end

function _build_field(b::_Builder, f::PField, oneofs::Dict{POneof,Int})
    kind = _kind_of(f.type)
    repeated = f.label == L_REPEATED
    oneof = -1
    if f.oneof !== nothing
        idx = get(oneofs, f.oneof, -1)
        if idx < 0
            oneof = length(oneofs)
            oneofs[f.oneof] = oneof
        else
            oneof = idx
        end
    end
    presence = if oneof >= 0
        EXPLICIT # reads/writes go through the oneof case
    elseif f.label == L_REQUIRED
        REQUIRED
    elseif kind == K_MESSAGE || kind == K_GROUP
        MESSAGE_PRESENCE
    elseif f.has_presence
        EXPLICIT
    else
        IMPLICIT
    end
    packed = repeated && f.packed
    wt = _wire_type(f.type, packed)
    return Field(f.number, something(f.name, ""), kind, wt,
        _wire_type(f.type, false), repeated, packed, oneof, presence,
        f.message_type === nothing ? nothing : _build_def(b, f.message_type, 1),
        f.enum_type === nothing ? nothing :
            get(b.schema.enums, f.enum_type.full_name, nothing),
        f.enum_closed, f.type == T_STRING && f.utf8 == 2, f.full_name)
end

function _attach_message_extensions(b::_Builder, m::PMessage)
    for e in m.extensions
        _attach_extension(b, e)
    end
    for n in m.nested
        _attach_message_extensions(b, n)
    end
    return nothing
end

function _attach_extension(b::_Builder, e::PField)
    extendee = e.extendee
    extendee === nothing && throw(CoreError("extension without extendee"))
    if startswith(extendee, ".")
        extendee = extendee[2:end]
    end
    d = get(b.schema.messages, extendee, nothing)
    d === nothing && throw(CoreError("extendee missing: $(e.extendee)"))
    s = _build_field(b, e, Dict{POneof,Int}())
    fields = copy(d.fields)
    push!(fields, s)
    oneof_by_name = d.oneof_by_name
    count = d.oneof_count
    _finish_def(d, fields, Dict{POneof,Int}())
    d.oneof_by_name = oneof_by_name # extensions are never in a oneof
    d.oneof_count = count
    return nothing
end

function _finish_def(d::MsgDef, fields::Vector{Any},
        oneofs::Dict{POneof,Int})
    # Stable sort by field number: the slots decode into are field-number
    # ordered, like the C core's table.
    sort!(fields; by = f -> (f::Field).number)
    max_number = 0
    has_required = false
    for f in fields
        f = f::Field
        f.number > max_number && (max_number = f.number)
        f.presence == REQUIRED && (has_required = true)
    end
    d.has_required = has_required
    d.fields = fields
    # Dense only when bounded, like the C core's fnum map: one field numbered
    # 536870911 must not allocate a 2 GiB vector.
    if max_number <= 512 && max_number <= length(fields) * 8 + 8
        slot_by_fnum = fill(-1, max_number + 1)
        for (i, f) in enumerate(fields)
            slot_by_fnum[(f::Field).number + 1] = i
        end
        d.slot_by_fnum = slot_by_fnum
    else
        d.slot_by_fnum = nothing
    end
    by_name = Dict{String,Int}()
    for (i, f) in enumerate(fields)
        by_name[(f::Field).name] = i
    end
    d.by_name = by_name
    oneof_by_name = Dict{String,Int}()
    for (oneof, slot) in oneofs
        oneof_by_name[something(oneof.name, "")] = slot
    end
    d.oneof_by_name = oneof_by_name
    d.oneof_count = max(d.oneof_count, length(oneofs))
    return nothing
end

"""Slot of a field number, -1 when the message has no such field."""
function slot_of(def::MsgDef, fnum::Int)
    slots = def.slot_by_fnum
    if slots !== nothing
        # Dense rows are stored at fnum + 1 (Julia is 1-based); slot 0 is no
        # field, and field number 0 never appears on the wire.
        return 0 <= fnum < length(slots) ? slots[fnum + 1] : -1
    end
    for (i, f) in enumerate(def.fields)
        (f::Field).number == fnum && return i
    end
    return -1
end

function _finish_transitive(schema::CoreSchema)
    changed = true
    while changed
        changed = false
        for d in values(schema.messages)
            if !d.deep_required && _needs_required(d)
                d.deep_required = true
                changed = true
            end
        end
    end
    return nothing
end

function _needs_required(d::MsgDef)
    for f in d.fields
        f = f::Field
        f.presence == REQUIRED && return true
        if (f.kind == K_MESSAGE || f.kind == K_GROUP) &&
           f.msg_type !== nothing && f.msg_type.deep_required
            return true
        end
    end
    return false
end

# ------------------------------------------------------ public entry

"""Parses and links a FileDescriptorSet into a compiled schema."""
function load_schema(bytes_::Vector{UInt8}; limits::Union{Nothing,Limits} = nothing)
    check_bytes(limits, length(bytes_), "descriptor set")
    local files
    try
        files = parse_set(bytes_)
        _link_files(files)
    catch e
        e isa CoreError ||
            throw(CoreError("descriptor set: $(sprint(showerror, e))", 1))
        throw(CoreError("descriptor set: $(e.msg)", 1))
    end
    return build(_Builder(), files)
end

"""Number of declared fields in a message (descriptor test surface)."""
field_count(def::MsgDef) = length(def.fields)

"""The field named `name`, or nothing."""
field_by_name(def::MsgDef, name::AbstractString) =
    (slot = get(def.by_name, String(name), 0); slot == 0 ? nothing :
        def.fields[slot]::Field)

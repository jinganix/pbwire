# Public surface of the pure-Julia core: a `Schema` loaded from a
# FileDescriptorSet, message types that decode and encode wire bytes, and the
# tree helpers the pbench `core` arm and the tests use. Stdlib only: no native
# library, no `ccall`, no generated code.
#
#     using PbwireCore
#     schema = from_file("matrix3.desc")
#     ty = msgtype(schema, "structtest.Scalars3")
#     m = decode(ty, bytes)      # unknown fields retained
#     encode(ty, m)              # byte-exact for a decoded tree
#
# This is the no-native mode (hosts without a C toolchain); the `ccall` runtime
# in `Pbwire.jl` stays the shipped fast path.

"""One message type: `new_message` plus binary decode/encode."""
struct MsgType
    def::MsgDef
end

name(ty::MsgType) = ty.def.full_name
Base.show(io::IO, ty::MsgType) = print(io, "<MsgType ", ty.def.full_name, ">")

"""A loaded FileDescriptorSet: the runtime schema every type comes from."""
struct Schema
    core::CoreSchema
    path::Union{Nothing,String}
end

function from_bytes(bytes::Vector{UInt8}; limits::Union{Nothing,Limits} = nothing,
        path::Union{Nothing,AbstractString} = nothing)
    return Schema(load_schema(bytes; limits = limits),
        path === nothing ? nothing : String(path))
end

function from_bytes(bytes::AbstractVector{UInt8};
        limits::Union{Nothing,Limits} = nothing,
        path::Union{Nothing,AbstractString} = nothing)
    return from_bytes(Vector{UInt8}(bytes); limits = limits, path = path)
end

"""Loads a descriptor set written by `protoc --descriptor_set_out`."""
function from_file(path::AbstractString; limits::Union{Nothing,Limits} = nothing)
    return from_bytes(read(path); limits = limits, path = path)
end

"""Looks up a message by its fully qualified name."""
function msgtype(schema::Schema, full_name::AbstractString)::MsgType
    return MsgType(message(schema.core, full_name))
end

"""An empty, mutable message of this type."""
new_message(ty::MsgType) = new_message(ty.def)

"""Decodes wire bytes; unknown fields are kept and re-encoded. The bytes are
the authority on presence, so a field absent on the wire stays absent."""
decode(ty::MsgType, data::AbstractVector{UInt8};
    limits::Union{Nothing,Limits} = nothing) =
    decode(ty.def, data; limits = limits)

decode_slice(ty::MsgType, data::AbstractVector{UInt8}, off::Int, len::Int;
    limits::Union{Nothing,Limits} = nothing) =
    decode_slice(ty.def, data, off, len; limits = limits)

"""Encodes a tree; a tree that came from `decode` re-emits byte-exactly."""
encode(ty::MsgType, m::Message) = encode(m)

"""Decodes straight into a plain dict (proto field names)."""
decode_dict(ty::MsgType, data::AbstractVector{UInt8};
    limits::Union{Nothing,Limits} = nothing) =
    to_dict(decode(ty.def, data; limits = limits))

"""Encodes a plain dict (the inverse of `decode_dict`)."""
encode_dict(ty::MsgType, obj::AbstractDict) = encode(from_dict(ty.def, obj))

to_dict(ty::MsgType, m::Message) = to_dict(m)
from_dict(ty::MsgType, obj::AbstractDict) = from_dict(ty.def, obj)

# ------------------------------------------------------------- field access

_field(m::Message, name_or_number) =
    m.def.fields[require_slot(m.def, name_or_number)]::Field

"""True when `m` carries that field (present on the wire)."""
has(m::Message, name_or_number) = has_field(m, name_or_number)
has_field(ty::MsgType, m::Message, name_or_number) =
    has_field(m, name_or_number)

"""A signed 64-bit view of a numeric field (an unsigned kind wraps, like the
`ccall` binding's `get_int`); 0 when absent."""
function get_int(m::Message, name_or_number)
    v = get_field(m, name_or_number)
    v === nothing && return Int64(0)
    v isa Int64 && return v
    v isa UInt64 && return reinterpret(Int64, v)
    v isa Bool && return v ? Int64(1) : Int64(0)
    v isa UInt32 && return Int64(v)
    v isa Int32 && return Int64(v)
    return Int64(v)
end

get_float(m::Message, name_or_number) = begin
    v = get_field(m, name_or_number)
    v === nothing ? Float32(0) : Float32(v)
end

get_double(m::Message, name_or_number) = begin
    v = get_field(m, name_or_number)
    v === nothing ? 0.0 : Float64(v)
end

get_bool(m::Message, name_or_number) = begin
    v = get_field(m, name_or_number)
    v === nothing ? false : v::Bool
end

get_string(m::Message, name_or_number) = begin
    v = get_field(m, name_or_number)
    v === nothing ? "" : String(v)
end

get_bytes(m::Message, name_or_number) = begin
    v = get_field(m, name_or_number)
    v === nothing ? UInt8[] : Vector{UInt8}(v)
end

get_message(m::Message, name_or_number) = begin
    v = get_field(m, name_or_number)
    v === nothing ? nothing : v::Message
end

"""Number of elements of a repeated field (0 when absent)."""
function count(m::Message, name_or_number)
    v = m.v[require_slot(m.def, name_or_number)]
    v === nothing && return 0
    return length(v)
end

"""Element `i` (1-based) of a repeated field, materialized."""
function at(m::Message, name_or_number, i::Int)
    slot = require_slot(m.def, name_or_number)
    f = m.def.fields[slot]::Field
    v = m.v[slot]
    (v === nothing || isempty(v)) && return nothing
    return to_value(f, v[i])
end

# ------------------------------------------------------------- field writes

as_int(m::Message, name_or_number) = get_int(m, name_or_number)
as_bool(m::Message, name_or_number) = get_bool(m, name_or_number)
as_string(m::Message, name_or_number) = get_string(m, name_or_number)
as_message(m::Message, name_or_number) = get_message(m, name_or_number)

function _store!(m::Message, f::Field, slot::Int, value)
    if f.repeated
        arr = m.v[slot]
        if arr === nothing
            arr = Any[]
            m.v[slot] = arr
        end
        push!(arr, value)
    else
        set_slot!(m, slot, value)
    end
    return value
end

function _set_scalar!(m::Message, name_or_number, value)
    slot = require_slot(m.def, name_or_number)
    f = m.def.fields[slot]::Field
    return _store!(m, f, slot, value)
end

set_int!(m::Message, name_or_number, v::Integer) =
    _set_scalar!(m, name_or_number, _int_storage(_field(m, name_or_number).kind, v))

function _int_storage(kind::Int, v::Integer)
    if kind == K_BOOL
        return v != 0
    elseif kind == K_UINT32 || kind == K_FIXED32
        return _ubits(v) & 0xFFFFFFFF
    elseif kind == K_UINT64 || kind == K_FIXED64
        return _ubits(v)
    elseif kind == K_INT32 || kind == K_SINT32 || kind == K_SFIXED32 ||
           kind == K_ENUM
        return Int64(reinterpret(Int32, UInt32(_ubits(v) & 0xFFFFFFFF)))
    end
    return reinterpret(Int64, _ubits(v))
end

set_float!(m::Message, name_or_number, v::Real) =
    _set_scalar!(m, name_or_number, float_bits(v))
set_double!(m::Message, name_or_number, v::Real) =
    _set_scalar!(m, name_or_number, double_bits(v))
set_bool!(m::Message, name_or_number, v::Bool) =
    _set_scalar!(m, name_or_number, v)
set_string!(m::Message, name_or_number, v::AbstractString) =
    _set_scalar!(m, name_or_number, String(v))
set_bytes!(m::Message, name_or_number, v::Vector{UInt8}) =
    _set_scalar!(m, name_or_number, v)

"""Creates (and stores, or appends for a repeated field) an empty submessage
of the field's type."""
function set_message!(m::Message, name_or_number)
    slot = require_slot(m.def, name_or_number)
    f = m.def.fields[slot]::Field
    sub = Message(f.msg_type::MsgDef)
    _store!(m, f, slot, sub)
    return sub
end

function close(x)
    return nothing
end

close(schema::Schema) = nothing
close(ty::MsgType) = nothing

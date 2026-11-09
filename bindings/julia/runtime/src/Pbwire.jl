# The Julia binding: `ccall` over the C ABI in `include/pbwire.h` (loaded
# with `Libdl.dlopen`, no C glue, no generated code). A `FileDescriptorSet`
# (`protoc --descriptor_set_out`) becomes the schema at run time; the wire
# format lives in the C core, so a Julia message and a C message are the same
# bytes by construction.
#
# Messages are `Dict`s at the boundary: `encode_dict` walks a `Dict` once and
# writes the wire directly, `decode` returns a `Message` handle and `to_dict`
# materialises plain Julia values. There is no `.proto` parser and no second
# wire implementation anywhere in this binding.
#
# Ownership: a `Schema` is freed by its GC finalizer once no message reaches
# it; a root `Message` must be closed explicitly (no finalizer on the hot
# path) and a nested message is a view of its parent that never frees anything. One message tree belongs
# to one thread, like the C core. A rejected value stays owned by the caller,
# so every setter frees it before throwing.
#
# The library is `ENV["PBWIRE_LIB"]`, else `build/libpbwire.{dylib,so}` found
# by walking up from the working directory or from this file.

module Pbwire

using Libdl

export PbwireError, Schema, MsgType, Field, Message, Value
export from_file, from_bytes, msgtype, field
export new_message, decode, encode, encode_dict, to_dict, decode_dict
export set_int!, set_bool!, set_float!, set_double!, set_string!, set_bytes!
export set_message!, clear!, has, get_int, get_bool, get_float, get_double
export get_string, get_bytes, get_message, count, at
export as_int, as_bool, as_float, as_double, as_string, as_bytes, as_message
export close

"""Anything the C core refused, plus I/O failures from [`from_file`](@ref)."""
struct PbwireError <: Exception
    status::Int
    message::String
end
Base.showerror(io::IO, e::PbwireError) = print(io, "PbwireError: ", e.message)

# --------------------------------------------------------------- the library

"""`ENV["PBWIRE_LIB"]`, else `build/libpbwire.{dylib,so}` by walking up."""
function _libpath()
    env = get(ENV, "PBWIRE_LIB", "")
    if !isempty(env)
        return env
    end
    for start in (pwd(), dirname(@__FILE__))
        dir = abspath(start)
        while true
            for suffix in ("dylib", "so")
                candidate = joinpath(dir, "build", "libpbwire.$suffix")
                if isfile(candidate)
                    return candidate
                end
            end
            parent = dirname(dir)
            if parent == dir
                break
            end
            dir = parent
        end
    end
    throw(PbwireError(-1, "no shared library found (set PBWIRE_LIB; run `make shared`)"))
end

# `ccall` in recent Julia needs a compile-time library name, so every C
# entry point is resolved once via `dlsym` into its own Ref and called
# through a raw pointer. One pointer load per call, no Dict hash.
const _handle = Ref{Ptr{Cvoid}}(C_NULL)
const _p_pbw_decode = Ref{Ptr{Cvoid}}(C_NULL)
const _p_pbw_descriptor_find_field_name = Ref{Ptr{Cvoid}}(C_NULL)
const _p_pbw_descriptor_name = Ref{Ptr{Cvoid}}(C_NULL)
const _p_pbw_descriptor_set_load = Ref{Ptr{Cvoid}}(C_NULL)
const _p_pbw_encode = Ref{Ptr{Cvoid}}(C_NULL)
const _p_pbw_field_label = Ref{Ptr{Cvoid}}(C_NULL)
const _p_pbw_field_message_type = Ref{Ptr{Cvoid}}(C_NULL)
const _p_pbw_field_name = Ref{Ptr{Cvoid}}(C_NULL)
const _p_pbw_field_number = Ref{Ptr{Cvoid}}(C_NULL)
const _p_pbw_field_type = Ref{Ptr{Cvoid}}(C_NULL)
const _p_pbw_free = Ref{Ptr{Cvoid}}(C_NULL)
const _p_pbw_message_clear = Ref{Ptr{Cvoid}}(C_NULL)
const _p_pbw_message_field_at = Ref{Ptr{Cvoid}}(C_NULL)
const _p_pbw_message_field_count = Ref{Ptr{Cvoid}}(C_NULL)
const _p_pbw_message_free = Ref{Ptr{Cvoid}}(C_NULL)
const _p_pbw_message_get_field = Ref{Ptr{Cvoid}}(C_NULL)
const _p_pbw_message_new = Ref{Ptr{Cvoid}}(C_NULL)
const _p_pbw_message_set_field = Ref{Ptr{Cvoid}}(C_NULL)
const _p_pbw_message_set_message = Ref{Ptr{Cvoid}}(C_NULL)
const _p_pbw_schema_error = Ref{Ptr{Cvoid}}(C_NULL)
const _p_pbw_schema_find = Ref{Ptr{Cvoid}}(C_NULL)
const _p_pbw_schema_free = Ref{Ptr{Cvoid}}(C_NULL)
const _p_pbw_strerror = Ref{Ptr{Cvoid}}(C_NULL)
const _p_pbw_value_at = Ref{Ptr{Cvoid}}(C_NULL)
const _p_pbw_value_count = Ref{Ptr{Cvoid}}(C_NULL)
const _p_pbw_value_free = Ref{Ptr{Cvoid}}(C_NULL)
const _p_pbw_value_get_bool = Ref{Ptr{Cvoid}}(C_NULL)
const _p_pbw_value_get_bytes = Ref{Ptr{Cvoid}}(C_NULL)
const _p_pbw_value_get_double = Ref{Ptr{Cvoid}}(C_NULL)
const _p_pbw_value_get_float = Ref{Ptr{Cvoid}}(C_NULL)
const _p_pbw_value_get_int = Ref{Ptr{Cvoid}}(C_NULL)
const _p_pbw_value_get_kind = Ref{Ptr{Cvoid}}(C_NULL)
const _p_pbw_value_get_message = Ref{Ptr{Cvoid}}(C_NULL)
const _p_pbw_value_get_string = Ref{Ptr{Cvoid}}(C_NULL)
const _p_pbw_value_new_bool = Ref{Ptr{Cvoid}}(C_NULL)
const _p_pbw_value_new_bytes = Ref{Ptr{Cvoid}}(C_NULL)
const _p_pbw_value_new_double = Ref{Ptr{Cvoid}}(C_NULL)
const _p_pbw_value_new_float = Ref{Ptr{Cvoid}}(C_NULL)
const _p_pbw_value_new_int = Ref{Ptr{Cvoid}}(C_NULL)
const _p_pbw_value_new_string = Ref{Ptr{Cvoid}}(C_NULL)

macro libcall(fname, Ret, ArgT, args...)
    if !(fname isa QuoteNode)
        error("@libcall function name must be a literal symbol")
    end
    ptr = Symbol("_p_", fname.value)
    :(ccall($(esc(ptr))[], $(esc(Ret)), $(esc(ArgT)), $(map(esc, args)...)))
end

_strerror(status::Integer) =
    unsafe_string(@libcall(:pbw_strerror, Ptr{UInt8}, (Cint,), Cint(status)))

function _check(status::Integer, what::String)
    if status != 0
        throw(PbwireError(Int(status), "$what: $(_strerror(status))"))
    end
    return nothing
end

# ---------------------------------------------------------------- the schema

"""A loaded `FileDescriptorSet`: the schema every [`MsgType`](@ref) comes from.

Native memory is owned by the GC finalizer, not by explicit `close`: a
`Message` keeps its `MsgType` (and through it the `Schema` object) reachable,
so the collector cannot free the schema while messages live. Explicit
`close(::Schema)` is a no-op kept for symmetry; prompt freeing would need a
live count on the hot path and is not worth it."""
mutable struct Schema
    handle::Ptr{Cvoid}
    function Schema(handle::Ptr{Cvoid})
        if handle == C_NULL
            throw(PbwireError(-1, "descriptor set: no schema returned"))
        end
        s = new(handle)
        finalizer(s) do x
            if x.handle != C_NULL
                @libcall(:pbw_schema_free, Cvoid, (Ptr{Cvoid},), x.handle)
                x.handle = C_NULL
            end
        end
        return s
    end
end

function Base.close(s::Schema)
    # No-op: freeing here would dangle live messages' descriptors. The
    # finalizer above releases native memory once no message reaches it.
    return nothing
end

"""Loads descriptor-set bytes; use `--include_imports` so every type resolves."""
function from_bytes(bytes::Vector{UInt8})
    out = Ref{Ptr{Cvoid}}(C_NULL)
    status = @libcall(:pbw_descriptor_set_load, Cint,
        (Ptr{UInt8}, Csize_t, Ptr{Ptr{Cvoid}}),
        isempty(bytes) ? C_NULL : bytes, length(bytes), out)
    if status != 0
        why = out[] == C_NULL ? "" : unsafe_string(
            @libcall(:pbw_schema_error, Ptr{UInt8}, (Ptr{Cvoid},), out[]))
        if out[] != C_NULL
            @libcall(:pbw_schema_free, Cvoid, (Ptr{Cvoid},), out[])
        end
        throw(PbwireError(Int(status), isempty(why) ? _strerror(status) : why))
    end
    return Schema(out[])
end

"""Loads a descriptor set written by `protoc --descriptor_set_out`."""
function from_file(path::String)
    if !isfile(path)
        throw(PbwireError(-1, "cannot read $path"))
    end
    return from_bytes(read(path))
end

# ------------------------------------------------------------------- the type

"""A resolved field handle; cached per [`MsgType`](@ref)."""
struct Field
    ptr::Ptr{Cvoid}
    number::UInt32
    type::Int
    label::Int
    schema::Schema # keeps the descriptor memory alive while the handle is used
end

function _field(ty, ptr::Ptr{Cvoid})
    number = @libcall(:pbw_field_number, UInt32, (Ptr{Cvoid},), ptr)
    type = Int(@libcall(:pbw_field_type, Cint, (Ptr{Cvoid},), ptr))
    label = Int(@libcall(:pbw_field_label, Cint, (Ptr{Cvoid},), ptr))
    return Field(ptr, number, type, label, ty.schema)
end

"""A message type from a [`Schema`](@ref)."""
mutable struct MsgType
    schema::Schema # keeps the schema alive while the type is in use
    desc::Ptr{Cvoid}
    fields::Dict{String,Field}
end

"""Looks up a message by fully qualified name, e.g. `structtest.Scalars3`."""
function msgtype(s::Schema, full_name::String)
    desc = @libcall(:pbw_schema_find, Ptr{Cvoid}, (Ptr{Cvoid}, Cstring),
        s.handle, full_name)
    if desc == C_NULL
        throw(PbwireError(-1, "no such message type: $full_name"))
    end
    return MsgType(s, desc, Dict{String,Field}())
end

"""Resolves a field once and caches it: a later set/get costs one native call."""
function field(ty::MsgType, name::String)
    cached = get(ty.fields, name, nothing)
    if cached !== nothing
        return cached
    end
    GC.@preserve ty begin
        ptr = @libcall(:pbw_descriptor_find_field_name, Ptr{Cvoid},
            (Ptr{Cvoid}, Cstring), ty.desc, name)
    end
    if ptr == C_NULL
        dname = unsafe_string(
            @libcall(:pbw_descriptor_name, Ptr{UInt8}, (Ptr{Cvoid},), ty.desc))
        throw(PbwireError(-1, "$name: no such field on $dname"))
    end
    f = _field(ty, ptr)
    ty.fields[name] = f
    return f
end

"""The message type of a MESSAGE/GROUP field, for nested access."""
function _nested_type(ty::MsgType, f::Field)
    GC.@preserve ty f begin
        desc = @libcall(:pbw_field_message_type, Ptr{Cvoid}, (Ptr{Cvoid},), f.ptr)
    end
    return desc == C_NULL ? nothing : MsgType(ty.schema, desc, Dict{String,Field}())
end

# ---------------------------------------------------------------- the message

"""One dynamic message. A root owns its tree and must be closed explicitly;
a nested message is a view of its parent and never frees anything."""
mutable struct Message
    ty::MsgType
    handle::Ptr{Cvoid}
    owned::Bool
    parent::Union{Message,Nothing} # keeps the parent (and its tree) alive
    function Message(ty::MsgType, handle::Ptr{Cvoid}, owned::Bool,
            parent::Union{Message,Nothing})
        if handle == C_NULL
            throw(PbwireError(-1, "pbw_message_new: out of memory"))
        end
        return new(ty, handle, owned, parent)
    end
end

function Base.close(m::Message)
    if m.handle != C_NULL && m.owned
        @libcall(:pbw_message_free, Cvoid, (Ptr{Cvoid},), m.handle)
        m.handle = C_NULL
    end
    return nothing
end

"""A new empty message; freed by [`close`](@ref)."""
function new_message(ty::MsgType)
    GC.@preserve ty begin
        return Message(ty,
            @libcall(:pbw_message_new, Ptr{Cvoid}, (Ptr{Cvoid},), ty.desc),
            true, nothing)
    end
end

"""Decodes wire bytes; unknown fields are preserved and re-encoded."""
function decode(ty::MsgType, wire::Vector{UInt8})
    out = Ref{Ptr{Cvoid}}(C_NULL)
    GC.@preserve ty begin
        status = @libcall(:pbw_decode, Cint,
            (Ptr{Cvoid}, Ptr{UInt8}, Csize_t, Ptr{Ptr{Cvoid}}),
            ty.desc, isempty(wire) ? C_NULL : wire, length(wire), out)
        _check(status, "decode")
    end
    return Message(ty, out[], true, nothing)
end

_resolve(ty::MsgType, f::Field) = f
_resolve(ty::MsgType, name::String) = field(ty, name)
_resolve(ty::MsgType, f) = throw(ArgumentError("Field or field name expected, got $f"))

# Any integer or enum field; the value is stored as raw 64-bit bits. A UInt64
# that does not fit Int64 keeps its bits via reinterpret.
_intbits(v::UInt64) = reinterpret(Int64, v)
_intbits(v::Integer) = Int64(v)

function _store(m::Message, f::Field, value::Ptr{Cvoid})
    if value == C_NULL
        throw(PbwireError(-1, "set $(_fname(f)): out of memory"))
    end
    GC.@preserve m f begin
        status = @libcall(:pbw_message_set_field, Cint,
            (Ptr{Cvoid}, Ptr{Cvoid}, Ptr{Cvoid}), m.handle, f.ptr, value)
    end
    if status != 0
        # A rejected value stays owned by the caller.
        @libcall(:pbw_value_free, Cvoid, (Ptr{Cvoid},), value)
        throw(PbwireError(Int(status), "set $(_fname(f)): $(_strerror(status))"))
    end
    return m
end

function _fname(f::Field)
    GC.@preserve f begin
        return unsafe_string(@libcall(:pbw_field_name, Ptr{UInt8}, (Ptr{Cvoid},), f.ptr))
    end
end

"""Appends to a repeated field or replaces a singular one; returns `m`."""
set_int!(m::Message, f::Union{Field,String}, v::Integer) =
    _store(m, _resolve(m.ty, f),
        @libcall(:pbw_value_new_int, Ptr{Cvoid}, (Int64,), _intbits(v)))
set_bool!(m::Message, f::Union{Field,String}, v::Bool) =
    _store(m, _resolve(m.ty, f),
        @libcall(:pbw_value_new_bool, Ptr{Cvoid}, (Cint,), v ? 1 : 0))
set_float!(m::Message, f::Union{Field,String}, v::Real) =
    _store(m, _resolve(m.ty, f),
        @libcall(:pbw_value_new_float, Ptr{Cvoid}, (Cfloat,), Float32(v)))
set_double!(m::Message, f::Union{Field,String}, v::Real) =
    _store(m, _resolve(m.ty, f),
        @libcall(:pbw_value_new_double, Ptr{Cvoid}, (Cdouble,), Float64(v)))
function set_string!(m::Message, f::Union{Field,String}, v::String)
    if occursin('\0', v)
        throw(PbwireError(-1,
            "set $(_fname(_resolve(m.ty, f))): string contains NUL (use set_bytes!)"))
    end
    return _store(m, _resolve(m.ty, f),
        @libcall(:pbw_value_new_string, Ptr{Cvoid}, (Cstring,), v))
end
function set_bytes!(m::Message, f::Union{Field,String}, v::Vector{UInt8})
    ff = _resolve(m.ty, f)
    ptr = @libcall(:pbw_value_new_bytes, Ptr{Cvoid},
        (Ptr{UInt8}, Csize_t), isempty(v) ? C_NULL : v, length(v))
    return _store(m, ff, ptr)
end

"""Nested message for a MESSAGE field, created in place (or returned as is)."""
function set_message!(m::Message, f::Union{Field,String})
    ff = _resolve(m.ty, f)
    ptr = @libcall(:pbw_message_set_message, Ptr{Cvoid},
        (Ptr{Cvoid}, UInt32), m.handle, ff.number)
    sub = _nested_type(m.ty, ff)
    if ptr == C_NULL || sub === nothing
        throw(PbwireError(-1, "set $(_fname(ff)): not a message field"))
    end
    return Message(sub, ptr, false, m)
end

clear!(m::Message, f::Union{Field,String}) =
    (@libcall(:pbw_message_clear, Cvoid, (Ptr{Cvoid}, UInt32),
        m.handle, _resolve(m.ty, f).number); m)

function _value(m::Message, f::Field)
    GC.@preserve m f begin
        return @libcall(:pbw_message_get_field, Ptr{Cvoid},
            (Ptr{Cvoid}, Ptr{Cvoid}), m.handle, f.ptr)
    end
end

has(m::Message, f::Union{Field,String}) = _value(m, _resolve(m.ty, f)) != C_NULL

get_int(m::Message, f::Union{Field,String}) = (v = _value(m, _resolve(m.ty, f));
    v == C_NULL ? Int64(0) : @libcall(:pbw_value_get_int, Int64, (Ptr{Cvoid},), v))
get_bool(m::Message, f::Union{Field,String}) = (v = _value(m, _resolve(m.ty, f));
    v != C_NULL && @libcall(:pbw_value_get_bool, Cint, (Ptr{Cvoid},), v) != 0)
get_float(m::Message, f::Union{Field,String}) = (v = _value(m, _resolve(m.ty, f));
    v == C_NULL ? 0.0f0 : @libcall(:pbw_value_get_float, Cfloat, (Ptr{Cvoid},), v))
get_double(m::Message, f::Union{Field,String}) = (v = _value(m, _resolve(m.ty, f));
    v == C_NULL ? 0.0 : @libcall(:pbw_value_get_double, Cdouble, (Ptr{Cvoid},), v))

function get_string(m::Message, f::Union{Field,String})
    v = _value(m, _resolve(m.ty, f))
    if v == C_NULL
        return nothing
    end
    ptr = @libcall(:pbw_value_get_string, Ptr{UInt8}, (Ptr{Cvoid},), v)
    return ptr == C_NULL ? nothing : unsafe_string(ptr)
end

function get_bytes(m::Message, f::Union{Field,String})
    v = _value(m, _resolve(m.ty, f))
    return v == C_NULL ? nothing : _load_bytes(v)
end

function _load_bytes(v::Ptr{Cvoid})
    len = Ref{Csize_t}(0)
    ptr = @libcall(:pbw_value_get_bytes, Ptr{UInt8},
        (Ptr{Cvoid}, Ptr{Csize_t}), v, len)
    if ptr == C_NULL
        return nothing
    end
    return copy(unsafe_wrap(Array, ptr, Int(len[]); own = false))
end

function get_message(m::Message, f::Union{Field,String})
    ff = _resolve(m.ty, f)
    v = _value(m, ff)
    if v == C_NULL
        return nothing
    end
    ptr = @libcall(:pbw_value_get_message, Ptr{Cvoid}, (Ptr{Cvoid},), v)
    sub = _nested_type(m.ty, ff)
    return (ptr == C_NULL || sub === nothing) ? nothing : Message(sub, ptr, false, m)
end

"""Items in a repeated field, or 0 when the field is absent."""
count(m::Message, f::Union{Field,String}) = (v = _value(m, _resolve(m.ty, f));
    v == C_NULL ? 0 : Int(@libcall(:pbw_value_count, Csize_t, (Ptr{Cvoid},), v)))

# -------------------------------------------------------------------- values

"""One item of a repeated field; only repeated items need it."""
struct Value
    ptr::Ptr{Cvoid}
    owner::Message # keeps the tree alive while the item is in use
    subtype::Union{MsgType,Nothing}
end

"""One item of a repeated field (`nothing` when absent or out of range)."""
function at(m::Message, f::Union{Field,String}, index::Integer)
    ff = _resolve(m.ty, f)
    v = _value(m, ff)
    if v == C_NULL
        return nothing
    end
    # Julia is 1-based; the core is 0-based.
    item = @libcall(:pbw_value_at, Ptr{Cvoid},
        (Ptr{Cvoid}, Csize_t), v, Csize_t(index - 1))
    return item == C_NULL ? nothing : Value(item, m, _nested_type(m.ty, ff))
end

as_int(v::Value) = @libcall(:pbw_value_get_int, Int64, (Ptr{Cvoid},), v.ptr)
as_bool(v::Value) =
    @libcall(:pbw_value_get_bool, Cint, (Ptr{Cvoid},), v.ptr) != 0
as_float(v::Value) = @libcall(:pbw_value_get_float, Cfloat, (Ptr{Cvoid},), v.ptr)
as_double(v::Value) = @libcall(:pbw_value_get_double, Cdouble, (Ptr{Cvoid},), v.ptr)
function as_string(v::Value)
    ptr = @libcall(:pbw_value_get_string, Ptr{UInt8}, (Ptr{Cvoid},), v.ptr)
    return ptr == C_NULL ? nothing : unsafe_string(ptr)
end
as_bytes(v::Value) = _load_bytes(v.ptr)
function as_message(v::Value)
    ptr = @libcall(:pbw_value_get_message, Ptr{Cvoid}, (Ptr{Cvoid},), v.ptr)
    return (ptr == C_NULL || v.subtype === nothing) ? nothing :
           Message(v.subtype, ptr, false, v.owner)
end

# ---------------------------------------------------------------------- wire

"""Encoded bytes (one allocation by the core, one copy into a Julia vector)."""
function encode(m::Message)
    out = Ref{Ptr{UInt8}}(C_NULL)
    len = Ref{Csize_t}(0)
    GC.@preserve m begin
        status = @libcall(:pbw_encode, Cint,
            (Ptr{Cvoid}, Ptr{Ptr{UInt8}}, Ptr{Csize_t}), m.handle, out, len)
        _check(status, "encode")
    end
    wire = copy(unsafe_wrap(Array, out[], Int(len[]); own = false))
    @libcall(:pbw_free, Cvoid, (Ptr{Cvoid},), out[])
    return wire
end

# ---------------------------------------------------------------------- dicts

# Field types from pbw_type; only the shapes the Dict path needs to tell apart.
const _T_DOUBLE = 1
const _T_FLOAT = 2
const _T_BOOL = 8
const _T_STRING = 9
const _T_GROUP = 10
const _T_MESSAGE = 11
const _T_BYTES = 12
const _LABEL_REPEATED = 3

function _store_scalar(m::Message, f::Field, v)
    t = f.type
    if t == _T_BOOL
        return set_bool!(m, f, v isa Bool ? v : throw(PbwireError(-1, "set $(_fname(f)): expected Bool")))
    elseif t == _T_FLOAT
        return set_float!(m, f, v)
    elseif t == _T_DOUBLE
        return set_double!(m, f, v)
    elseif t == _T_STRING
        if v isa Vector{UInt8}
            return set_bytes!(m, f, v) # a string field accepts bytes on the wire
        end
        return set_string!(m, f, String(v))
    elseif t == _T_BYTES
        return set_bytes!(m, f, v isa Vector{UInt8} ? v : Vector{UInt8}(String(v)))
    elseif t == _T_MESSAGE || t == _T_GROUP
        if !(v isa AbstractDict)
            throw(PbwireError(-1, "set $(_fname(f)): expected Dict"))
        end
        return encode_dict!(set_message!(m, f), v)
    else # every integer kind and enums: raw 64-bit bits
        return set_int!(m, f, v)
    end
end

"""Fills `m` from a `Dict{String,Any}`; repeated fields take vectors."""
function encode_dict!(m::Message, d::AbstractDict)
    for (k, v) in d
        f = field(m.ty, String(k))
        if f.label == _LABEL_REPEATED && v isa AbstractVector && !(v isa Vector{UInt8})
            for item in v
                _store_scalar(m, f, item)
            end
        else
            _store_scalar(m, f, v)
        end
    end
    return m
end

"""Builds a new message of type `ty` from a `Dict{String,Any}` and encodes it."""
function encode_dict(ty::MsgType, d::AbstractDict)
    m = new_message(ty)
    try
        return encode(encode_dict!(m, d))
    finally
        close(m)
    end
end

function _value_to_julia(f::Field, v::Ptr{Cvoid})
    t = f.type
    if t == _T_BOOL
        return @libcall(:pbw_value_get_bool, Cint, (Ptr{Cvoid},), v) != 0
    elseif t == _T_FLOAT
        return @libcall(:pbw_value_get_float, Cfloat, (Ptr{Cvoid},), v)
    elseif t == _T_DOUBLE
        return @libcall(:pbw_value_get_double, Cdouble, (Ptr{Cvoid},), v)
    elseif t == _T_STRING
        ptr = @libcall(:pbw_value_get_string, Ptr{UInt8}, (Ptr{Cvoid},), v)
        return ptr == C_NULL ? nothing : unsafe_string(ptr)
    elseif t == _T_BYTES
        return _load_bytes(v)
    elseif t == _T_MESSAGE || t == _T_GROUP
        # Nested messages convert through their owner (keeps the tree alive).
        throw(PbwireError(-1, "internal: message value needs its owner"))
    else
        return @libcall(:pbw_value_get_int, Int64, (Ptr{Cvoid},), v)
    end
end

# to_dict walks present fields only (field_at skips what the wire omitted),
# so absent proto3 scalars and empty repeated fields simply do not appear.

"""Materialises present fields as `Dict{String,Any}` (nested messages recurse)."""
function to_dict(m::Message)
    d = Dict{String,Any}()
    GC.@preserve m begin
    n = Int(@libcall(:pbw_message_field_count, Csize_t, (Ptr{Cvoid},), m.handle))
    for i in 0:(n - 1)
        vout = Ref{Ptr{Cvoid}}(C_NULL)
        fptr = @libcall(:pbw_message_field_at, Ptr{Cvoid},
            (Ptr{Cvoid}, Csize_t, Ptr{Ptr{Cvoid}}), m.handle, Csize_t(i), vout)
        if fptr == C_NULL || vout[] == C_NULL
            continue
        end
        f = _field(m.ty, fptr)
        name = _fname(f)
        kind = Int(@libcall(:pbw_value_get_kind, Cint, (Ptr{Cvoid},), vout[]))
        if kind == 3 # PBW_VALUE_LIST: repeated field storage
            items = Any[]
            len = Int(@libcall(:pbw_value_count, Csize_t, (Ptr{Cvoid},), vout[]))
            sub = _nested_type(m.ty, f)
            for j in 0:(len - 1)
                item = @libcall(:pbw_value_at, Ptr{Cvoid},
                    (Ptr{Cvoid}, Csize_t), vout[], Csize_t(j))
                push!(items, _item_to_julia(m, f, sub, item))
            end
            d[name] = items
        else
            d[name] = _scalar_to_julia(m, f, vout[])
        end
    end
    end
    return d
end

function _scalar_to_julia(m::Message, f::Field, v::Ptr{Cvoid})
    GC.@preserve m f begin
    t = f.type
    if t == _T_MESSAGE || t == _T_GROUP
        ptr = @libcall(:pbw_value_get_message, Ptr{Cvoid}, (Ptr{Cvoid},), v)
        sub = _nested_type(m.ty, f)
        return (ptr == C_NULL || sub === nothing) ? nothing :
               to_dict(Message(sub, ptr, false, m))
    end
    return _value_to_julia(f, v)
    end
end

function _item_to_julia(m::Message, f::Field, sub::Union{MsgType,Nothing}, item::Ptr{Cvoid})
    GC.@preserve m f begin
    t = f.type
    if t == _T_MESSAGE || t == _T_GROUP
        ptr = @libcall(:pbw_value_get_message, Ptr{Cvoid}, (Ptr{Cvoid},), item)
        return (ptr == C_NULL || sub === nothing) ? nothing :
               to_dict(Message(sub, ptr, false, m))
    end
    return _value_to_julia(f, item)
    end
end

"""Decodes wire bytes straight into a `Dict{String,Any}`."""
function decode_dict(ty::MsgType, wire::Vector{UInt8})
    m = decode(ty, wire)
    try
        return to_dict(m)
    finally
        close(m)
    end
end

function __init__()
    # Eagerly fail on a missing/unloadable library, then pin every symbol.
    _handle[] = Libdl.dlopen(_libpath())
    _p_pbw_decode[] = Libdl.dlsym(_handle[], :pbw_decode)
    _p_pbw_descriptor_find_field_name[] = Libdl.dlsym(_handle[], :pbw_descriptor_find_field_name)
    _p_pbw_descriptor_name[] = Libdl.dlsym(_handle[], :pbw_descriptor_name)
    _p_pbw_descriptor_set_load[] = Libdl.dlsym(_handle[], :pbw_descriptor_set_load)
    _p_pbw_encode[] = Libdl.dlsym(_handle[], :pbw_encode)
    _p_pbw_field_label[] = Libdl.dlsym(_handle[], :pbw_field_label)
    _p_pbw_field_message_type[] = Libdl.dlsym(_handle[], :pbw_field_message_type)
    _p_pbw_field_name[] = Libdl.dlsym(_handle[], :pbw_field_name)
    _p_pbw_field_number[] = Libdl.dlsym(_handle[], :pbw_field_number)
    _p_pbw_field_type[] = Libdl.dlsym(_handle[], :pbw_field_type)
    _p_pbw_free[] = Libdl.dlsym(_handle[], :pbw_free)
    _p_pbw_message_clear[] = Libdl.dlsym(_handle[], :pbw_message_clear)
    _p_pbw_message_field_at[] = Libdl.dlsym(_handle[], :pbw_message_field_at)
    _p_pbw_message_field_count[] = Libdl.dlsym(_handle[], :pbw_message_field_count)
    _p_pbw_message_free[] = Libdl.dlsym(_handle[], :pbw_message_free)
    _p_pbw_message_get_field[] = Libdl.dlsym(_handle[], :pbw_message_get_field)
    _p_pbw_message_new[] = Libdl.dlsym(_handle[], :pbw_message_new)
    _p_pbw_message_set_field[] = Libdl.dlsym(_handle[], :pbw_message_set_field)
    _p_pbw_message_set_message[] = Libdl.dlsym(_handle[], :pbw_message_set_message)
    _p_pbw_schema_error[] = Libdl.dlsym(_handle[], :pbw_schema_error)
    _p_pbw_schema_find[] = Libdl.dlsym(_handle[], :pbw_schema_find)
    _p_pbw_schema_free[] = Libdl.dlsym(_handle[], :pbw_schema_free)
    _p_pbw_strerror[] = Libdl.dlsym(_handle[], :pbw_strerror)
    _p_pbw_value_at[] = Libdl.dlsym(_handle[], :pbw_value_at)
    _p_pbw_value_count[] = Libdl.dlsym(_handle[], :pbw_value_count)
    _p_pbw_value_free[] = Libdl.dlsym(_handle[], :pbw_value_free)
    _p_pbw_value_get_bool[] = Libdl.dlsym(_handle[], :pbw_value_get_bool)
    _p_pbw_value_get_bytes[] = Libdl.dlsym(_handle[], :pbw_value_get_bytes)
    _p_pbw_value_get_double[] = Libdl.dlsym(_handle[], :pbw_value_get_double)
    _p_pbw_value_get_float[] = Libdl.dlsym(_handle[], :pbw_value_get_float)
    _p_pbw_value_get_int[] = Libdl.dlsym(_handle[], :pbw_value_get_int)
    _p_pbw_value_get_kind[] = Libdl.dlsym(_handle[], :pbw_value_get_kind)
    _p_pbw_value_get_message[] = Libdl.dlsym(_handle[], :pbw_value_get_message)
    _p_pbw_value_get_string[] = Libdl.dlsym(_handle[], :pbw_value_get_string)
    _p_pbw_value_new_bool[] = Libdl.dlsym(_handle[], :pbw_value_new_bool)
    _p_pbw_value_new_bytes[] = Libdl.dlsym(_handle[], :pbw_value_new_bytes)
    _p_pbw_value_new_double[] = Libdl.dlsym(_handle[], :pbw_value_new_double)
    _p_pbw_value_new_float[] = Libdl.dlsym(_handle[], :pbw_value_new_float)
    _p_pbw_value_new_int[] = Libdl.dlsym(_handle[], :pbw_value_new_int)
    _p_pbw_value_new_string[] = Libdl.dlsym(_handle[], :pbw_value_new_string)
end

end # module

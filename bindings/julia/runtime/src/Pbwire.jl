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
export encode_deterministic, encode_delimited, decode_delimited, decode_limits
export print_json, parse_json, print_text, parse_text
export encoded_size, is_initialized
export LayoutRow, LayoutSchema, layout_schema, LayoutType, layout_type
export LayoutPlan, layout_plan, encode_layout, decode_layout
export any_pack, any_type_url, any_payload
export timestamp_millis, duration_millis, timestamp_from_millis, duration_from_millis
export fieldmask_paths, visit
export Service, Method, service_count, service_at, service, service_name
export method_count, method_at, method, method_name, method_input, method_output
export client_streaming, server_streaming, resolve_method
export close, Ctx, reset!, decode_in

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

"""`ENV["PBWIRE_JULIA_LIB"]`, else `build/libpbwire_julia.{dylib,so}` by walking up."""
function _julia_libpath()
    env = get(ENV, "PBWIRE_JULIA_LIB", "")
    if !isempty(env)
        return env
    end
    for start in (pwd(), dirname(@__FILE__))
        dir = abspath(start)
        while true
            for suffix in ("dylib", "so")
                candidate = joinpath(dir, "build", "libpbwire_julia.$suffix")
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
    throw(PbwireError(-1,
        "no layout library found (run `make julia-shim-native`)"))
end

# `ccall` in recent Julia needs a compile-time library name, so every C
# entry point is resolved once via `dlsym` into its own Ref and called
# through a raw pointer. One pointer load per call, no Dict hash.
const _handle = Ref{Ptr{Cvoid}}(C_NULL)
const _p_pbw_ctx_free = Ref{Ptr{Cvoid}}(C_NULL)
const _p_pbw_ctx_new = Ref{Ptr{Cvoid}}(C_NULL)
const _p_pbw_ctx_reset = Ref{Ptr{Cvoid}}(C_NULL)
const _p_pbw_decode = Ref{Ptr{Cvoid}}(C_NULL)
const _p_pbw_decode_ctx = Ref{Ptr{Cvoid}}(C_NULL)
const _p_pbw_descriptor_find_field_name = Ref{Ptr{Cvoid}}(C_NULL)
const _p_pbw_descriptor_name = Ref{Ptr{Cvoid}}(C_NULL)
const _p_pbw_descriptor_set_load = Ref{Ptr{Cvoid}}(C_NULL)
const _p_pbw_encode = Ref{Ptr{Cvoid}}(C_NULL)
const _p_pbw_encode_to = Ref{Ptr{Cvoid}}(C_NULL)
const _p_pbw_field_label = Ref{Ptr{Cvoid}}(C_NULL)
const _p_pbw_field_message_type = Ref{Ptr{Cvoid}}(C_NULL)
const _p_pbw_field_name = Ref{Ptr{Cvoid}}(C_NULL)
const _p_pbw_field_number = Ref{Ptr{Cvoid}}(C_NULL)
const _p_pbw_field_type = Ref{Ptr{Cvoid}}(C_NULL)
const _p_pbw_free = Ref{Ptr{Cvoid}}(C_NULL)
const _p_pbw_any_pack = Ref{Ptr{Cvoid}}(C_NULL)
const _p_pbw_any_payload = Ref{Ptr{Cvoid}}(C_NULL)
const _p_pbw_any_type_url = Ref{Ptr{Cvoid}}(C_NULL)
const _p_pbw_decode_delimited = Ref{Ptr{Cvoid}}(C_NULL)
const _p_pbw_decode_limits = Ref{Ptr{Cvoid}}(C_NULL)
const _p_pbw_duration_from_millis = Ref{Ptr{Cvoid}}(C_NULL)
const _p_pbw_duration_to_millis = Ref{Ptr{Cvoid}}(C_NULL)
const _p_pbw_encode_delimited = Ref{Ptr{Cvoid}}(C_NULL)
const _p_pbw_encode_deterministic = Ref{Ptr{Cvoid}}(C_NULL)
const _p_pbw_fieldmask_path_at = Ref{Ptr{Cvoid}}(C_NULL)
const _p_pbw_fieldmask_path_count = Ref{Ptr{Cvoid}}(C_NULL)
const _p_pbw_json_parse_opts = Ref{Ptr{Cvoid}}(C_NULL)
const _p_pbw_json_parse_limits = Ref{Ptr{Cvoid}}(C_NULL)
const _p_pbw_json_print = Ref{Ptr{Cvoid}}(C_NULL)
const _p_pbw_method_client_streaming = Ref{Ptr{Cvoid}}(C_NULL)
const _p_pbw_method_input_type = Ref{Ptr{Cvoid}}(C_NULL)
const _p_pbw_method_name = Ref{Ptr{Cvoid}}(C_NULL)
const _p_pbw_method_output_type = Ref{Ptr{Cvoid}}(C_NULL)
const _p_pbw_method_server_streaming = Ref{Ptr{Cvoid}}(C_NULL)
const _p_pbw_message_clear = Ref{Ptr{Cvoid}}(C_NULL)
const _p_pbw_message_encoded_size = Ref{Ptr{Cvoid}}(C_NULL)
const _p_pbw_message_field_at = Ref{Ptr{Cvoid}}(C_NULL)
const _p_pbw_message_field_count = Ref{Ptr{Cvoid}}(C_NULL)
const _p_pbw_message_is_initialized = Ref{Ptr{Cvoid}}(C_NULL)
const _p_pbw_message_free = Ref{Ptr{Cvoid}}(C_NULL)
const _p_pbw_message_get_field = Ref{Ptr{Cvoid}}(C_NULL)
const _p_pbw_message_new = Ref{Ptr{Cvoid}}(C_NULL)
const _p_pbw_message_set_field = Ref{Ptr{Cvoid}}(C_NULL)
const _p_pbw_message_set_message = Ref{Ptr{Cvoid}}(C_NULL)
const _p_pbw_message_wire_size = Ref{Ptr{Cvoid}}(C_NULL)
const _p_pbw_schema_error = Ref{Ptr{Cvoid}}(C_NULL)
const _p_pbw_schema_find = Ref{Ptr{Cvoid}}(C_NULL)
const _p_pbw_schema_free = Ref{Ptr{Cvoid}}(C_NULL)
const _p_pbw_schema_service_at = Ref{Ptr{Cvoid}}(C_NULL)
const _p_pbw_schema_service_count = Ref{Ptr{Cvoid}}(C_NULL)
const _p_pbw_schema_service_find = Ref{Ptr{Cvoid}}(C_NULL)
const _p_pbw_service_method_at = Ref{Ptr{Cvoid}}(C_NULL)
const _p_pbw_service_method_count = Ref{Ptr{Cvoid}}(C_NULL)
const _p_pbw_service_method_find = Ref{Ptr{Cvoid}}(C_NULL)
const _p_pbw_service_name = Ref{Ptr{Cvoid}}(C_NULL)
const _p_pbw_service_resolve = Ref{Ptr{Cvoid}}(C_NULL)
const _p_pbw_text_parse = Ref{Ptr{Cvoid}}(C_NULL)
const _p_pbw_text_parse_limits = Ref{Ptr{Cvoid}}(C_NULL)
const _p_pbw_text_print = Ref{Ptr{Cvoid}}(C_NULL)
const _p_pbw_text_print_opts = Ref{Ptr{Cvoid}}(C_NULL)
const _p_pbw_timestamp_from_millis = Ref{Ptr{Cvoid}}(C_NULL)
const _p_pbw_timestamp_to_millis = Ref{Ptr{Cvoid}}(C_NULL)
const _p_pbw_visit = Ref{Ptr{Cvoid}}(C_NULL)
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
const _jl_handle = Ref{Ptr{Cvoid}}(C_NULL)
const _p_pbw_julia_schema_new = Ref{Ptr{Cvoid}}(C_NULL)
const _p_pbw_julia_schema_free = Ref{Ptr{Cvoid}}(C_NULL)
const _p_pbw_julia_type_new = Ref{Ptr{Cvoid}}(C_NULL)
const _p_pbw_julia_type_free = Ref{Ptr{Cvoid}}(C_NULL)
const _p_pbw_julia_encode = Ref{Ptr{Cvoid}}(C_NULL)
const _p_pbw_julia_decode = Ref{Ptr{Cvoid}}(C_NULL)

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
    nested::Dict{UInt32,MsgType} # per field number, so a nested walk reuses one cache
    lock::ReentrantLock # guards both caches: first-burst threads share one type
end

_msgtype(s::Schema, desc::Ptr{Cvoid}) =
    MsgType(s, desc, Dict{String,Field}(), Dict{UInt32,MsgType}(), ReentrantLock())

"""Looks up a message by fully qualified name, e.g. `structtest.Scalars3`."""
function msgtype(s::Schema, full_name::String)
    desc = @libcall(:pbw_schema_find, Ptr{Cvoid}, (Ptr{Cvoid}, Cstring),
        s.handle, full_name)
    if desc == C_NULL
        throw(PbwireError(-1, "no such message type: $full_name"))
    end
    return _msgtype(s, desc)
end

"""Resolves a field once and caches it: a later set/get costs one native call.

The lookup-plus-fill holds the type lock, so first-burst threads sharing one
type resolve concurrently without racing on the `Dict` (concurrent read+write
on a `Dict` is undefined); the native lookup inside is a pure descriptor read."""
function field(ty::MsgType, name::String)
    lock(ty.lock) do
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
end

"""The message type of a MESSAGE/GROUP field, for nested access.

Memoised per (type, field number): without the cache every nested access built
a fresh `MsgType` with an empty field cache, so each nested field lookup went
back to a native name search."""
function _nested_type(ty::MsgType, f::Field)
    lock(ty.lock) do
        cached = get(ty.nested, f.number, nothing)
        cached !== nothing && return cached
        GC.@preserve ty f begin
            desc = @libcall(:pbw_field_message_type, Ptr{Cvoid}, (Ptr{Cvoid},), f.ptr)
        end
        desc == C_NULL && return nothing
        sub = _msgtype(ty.schema, desc)
        ty.nested[f.number] = sub
        return sub
    end
end

# ---------------------------------------------------------------- the message

"""One dynamic message. A root owns its tree and must be closed explicitly;
a nested message is a view of its parent and never frees anything."""
mutable struct Message
    ty::MsgType
    handle::Ptr{Cvoid}
    owned::Bool
    parent::Any # keeps the tree alive: the parent Message, or the Ctx it lives in
    cap::Threads.Atomic{Int} # last wire length: the encode buffer hint (atomic,
    # so concurrent encodes of one shared message never race on the write-back)
    function Message(ty::MsgType, handle::Ptr{Cvoid}, owned::Bool,
            parent, cap::Int = 0)
        if handle == C_NULL
            throw(PbwireError(-1, "pbw_message_new: out of memory"))
        end
        return new(ty, handle, owned, parent, Threads.Atomic{Int}(cap))
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

"""A reusable decode workspace: a decode into it allocates nothing after the
first message. One context per task, never shared concurrently."""
mutable struct Ctx
    handle::Ptr{Cvoid}
    function Ctx()
        h = @libcall(:pbw_ctx_new, Ptr{Cvoid}, ())
        if h == C_NULL
            throw(PbwireError(-1, "pbw_ctx_new: out of memory"))
        end
        c = new(h)
        finalizer(c) do x
            if x.handle != C_NULL
                @libcall(:pbw_ctx_free, Cvoid, (Ptr{Cvoid},), x.handle)
                x.handle = C_NULL
            end
        end
        return c
    end
end

"""Drops every tree decoded with `c`, keeping its buffers for the next decode."""
function reset!(c::Ctx)
    @libcall(:pbw_ctx_reset, Cvoid, (Ptr{Cvoid},), c.handle)
    return c
end

"""Decodes wire bytes into `c` instead of a fresh allocation per value: the
allocator call drops out of steady-state decoding. The tree is read-only, and
`decode_in`, `reset!` or closing `c` invalidates it -- so copy out anything
you keep, or encode it while it is still live."""
function decode_in(c::Ctx, ty::MsgType, wire::Vector{UInt8})
    # No reset! here: the core seals each decode read-only and resets a sealed
    # arena on entry, so steady-state decoding is one ccall, not two.
    out = Ref{Ptr{Cvoid}}(C_NULL)
    GC.@preserve ty c begin
        status = @libcall(:pbw_decode_ctx, Cint,
            (Ptr{Cvoid}, Ptr{Cvoid}, Ptr{UInt8}, Csize_t, Ptr{Ptr{Cvoid}}),
            c.handle, ty.desc, isempty(wire) ? C_NULL : wire,
            length(wire), out)
    end
    _check(status, "decode")
    return Message(ty, out[], false, c, length(wire))
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
    return Message(ty, out[], true, nothing, length(wire))
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

const _ENCODE_SLACK = 12 # PBW_ENCODE_SLACK: the writer's guaranteed headroom
const _ERR_LIMIT = 5

"""Encoded bytes.

Written straight into the array this returns: no core buffer, no second copy,
no `pbw_free`. The size of the last encode (or the length of the wire the tree
was decoded from) is the buffer hint; a tree that grew past it falls back to
the allocating writer once and remembers the new size, so a re-encode loop
settles on the direct path."""
function encode(m::Message)
    len = Ref{Csize_t}(0)
    cap = m.cap[]
    if cap == 0 # hand-built tree: ask the decoder's hint (0 unless it was decoded)
        cap = Int(@libcall(:pbw_message_wire_size, Csize_t, (Ptr{Cvoid},), m.handle))
    end
    if cap > 0
        cap += _ENCODE_SLACK
        wire = Vector{UInt8}(undef, cap)
        GC.@preserve m wire begin
            status = @libcall(:pbw_encode_to, Cint,
                (Ptr{Cvoid}, Ptr{UInt8}, Csize_t, Ptr{Csize_t}),
                m.handle, wire, Csize_t(cap), len)
        end
        if status == 0
            n = Int(len[])
            resize!(wire, n)
            m.cap[] = n
            return wire
        end
        status == _ERR_LIMIT || _check(status, "encode")
    end
    out = Ref{Ptr{UInt8}}(C_NULL)
    GC.@preserve m begin
        status = @libcall(:pbw_encode, Cint,
            (Ptr{Cvoid}, Ptr{Ptr{UInt8}}, Ptr{Csize_t}), m.handle, out, len)
        _check(status, "encode")
    end
    n = Int(len[])
    wire = copy(unsafe_wrap(Array, out[], n; own = false))
    @libcall(:pbw_free, Cvoid, (Ptr{Cvoid},), out[])
    m.cap[] = n
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


# ------------------------------------------------------------- flat layout

"""One row of a generated layout: the C reader's field facts plus a slot
offset (computed with `fieldoffset` in the generated file, never hard-coded).
`child` is a message row's proto full name, empty otherwise."""
struct LayoutRow
    number::UInt64
    kind::UInt64
    repeated::UInt64
    offset::UInt64
    has::UInt64
    count::UInt64
    type::UInt64
    wire::UInt64
    packed::UInt64
    required::UInt64
    oneof::UInt64
    cs::UInt64
    child::String
end

# The core's inline pbw_bytes_t (data, len); embedded in a layout image.
struct _PbwBytes
    data::Ptr{UInt8}
    len::Csize_t
end

# The C row mirror: every field is 64-bit so the Julia vector needs no padding
# rules, plus the child's proto full name as a NUL-terminated pointer.
struct _PbwJuliaRow
    number::UInt64
    kind::UInt64
    repeated::UInt64
    offset::UInt64
    has::UInt64
    count::UInt64
    type::UInt64
    wire::UInt64
    packed::UInt64
    required::UInt64
    oneof::UInt64
    cs::UInt64
    child::Ptr{UInt8}
end

function _ensure_layout_lib()
    if _jl_handle[] != C_NULL
        return
    end
    h = Libdl.dlopen(_julia_libpath())
    _p_pbw_julia_schema_new[] = Libdl.dlsym(h, :pbw_julia_schema_new)
    _p_pbw_julia_schema_free[] = Libdl.dlsym(h, :pbw_julia_schema_free)
    _p_pbw_julia_type_new[] = Libdl.dlsym(h, :pbw_julia_type_new)
    _p_pbw_julia_type_free[] = Libdl.dlsym(h, :pbw_julia_type_free)
    _p_pbw_julia_encode[] = Libdl.dlsym(h, :pbw_julia_encode)
    _p_pbw_julia_decode[] = Libdl.dlsym(h, :pbw_julia_decode)
    _jl_handle[] = h
end

"""A registry for one core [`Schema`](@ref), owned by the layout library.
It keeps the types compiled from the generated rows; the schema itself stays
owned by the core wrapper."""
mutable struct LayoutSchema
    handle::Ptr{Cvoid}
    schema::Schema
    function LayoutSchema(schema::Schema)
        _ensure_layout_lib()
        out = Ref{Ptr{Cvoid}}(C_NULL)
        GC.@preserve schema begin
            status = @libcall(:pbw_julia_schema_new, Cint,
                (Ptr{Cvoid}, Ptr{Ptr{Cvoid}}), schema.handle, out)
        end
        _check(status, "layout schema")
        ls = new(out[], schema)
        finalizer(ls) do x
            if x.handle != C_NULL
                @libcall(:pbw_julia_schema_free, Cvoid, (Ptr{Cvoid},), x.handle)
                x.handle = C_NULL
            end
        end
        return ls
    end
end

"""Wraps a core schema for the layout reader (one registry per schema)."""
layout_schema(schema::Schema) = LayoutSchema(schema)

"""A compiled layout of one generated `mutable struct`: the native object plus
its reusable encode buffer. Child types must be registered before their parent;
the registry, kept alive here, owns and frees them all together."""
mutable struct LayoutType
    handle::Ptr{Cvoid}
    name::String
    size::Int
    schema::LayoutSchema # keeps the registry (and the core schema) alive
    function LayoutType(handle::Ptr{Cvoid}, name::String, size::Int,
            schema::LayoutSchema)
        if handle == C_NULL
            throw(PbwireError(-1, "layout type: out of memory"))
        end
        # The registry owns every type it compiled and frees them together,
        # so a type never frees itself (that would double-free a child that
        # outlived its parent's finalizer).
        return new(handle, name, size, schema)
    end
end

"""Registers one generated layout type. `rows` come from the generated
`<message>LayoutRows`; offsets are `fieldoffset` calls, so they cannot drift
from the Julia struct. The child row must be registered first."""
function layout_type(schema::LayoutSchema, name::String, ::Type{T},
        rows::Vector{LayoutRow}) where {T}
    crows = Vector{_PbwJuliaRow}(undef, length(rows))
    children = Vector{String}(undef, length(rows))
    for i in eachindex(rows)
        r = rows[i]
        children[i] = r.child
        crows[i] = _PbwJuliaRow(r.number, r.kind, r.repeated, r.offset,
            r.has, r.count, r.type, r.wire, r.packed, r.required,
            r.oneof, r.cs, Ptr{UInt8}(C_NULL))
    end
    # The C registration only needs the child name during the call; keeping
    # the strings in `children` alive here is what makes their pointers valid.
    GC.@preserve crows children schema begin
        for i in eachindex(rows)
            if !isempty(children[i])
                crows[i] = _PbwJuliaRow(rows[i].number, rows[i].kind,
                    rows[i].repeated, rows[i].offset, rows[i].has,
                    rows[i].count, rows[i].type, rows[i].wire,
                    rows[i].packed, rows[i].required, rows[i].oneof,
                    rows[i].cs, Base.unsafe_convert(Ptr{UInt8}, children[i]))
            end
        end
        out = Ref{Ptr{Cvoid}}(C_NULL)
        status = @libcall(:pbw_julia_type_new, Cint,
            (Ptr{Cvoid}, Cstring, Ptr{_PbwJuliaRow}, Csize_t, Csize_t,
             Ptr{Ptr{Cvoid}}),
            schema.handle, name, crows, length(crows), sizeof(T), out)
        _check(status, "layout type $name")
        return LayoutType(out[], name, Int(sizeof(T)), schema)
    end
end

"""A reusable layout call: the compiled type, one zeroed native image and one
call-owned arena. `decode_layout` fills `image` and bump-allocates every
string/bytes/child from `arena`, so the plan must outlive a decoded image."""
mutable struct LayoutPlan{T}
    layout::LayoutType
    image::T
    arena::Vector{UInt8}
end

"""Compiles a generated layout and allocates its image and arena once."""
function layout_plan(schema::LayoutSchema, name::String, ::Type{T},
        rows::Vector{LayoutRow}; arena_bytes::Integer = 1 << 20) where {T}
    lt = layout_type(schema, name, T, rows)
    return LayoutPlan{T}(lt, T(), Vector{UInt8}(undef, Int(arena_bytes)))
end

"""Native image -> wire bytes, one ccall. The image is borrowed: strings,
byte arrays, repeated arrays and sub-images must stay alive for the call."""
function encode_layout(::Type{T}, msg::T, plan::LayoutPlan{T}) where {T}
    out = Ref{Ptr{UInt8}}(C_NULL)
    len = Ref{Csize_t}(0)
    GC.@preserve msg plan begin
        status = @libcall(:pbw_julia_encode, Cint,
            (Ptr{Cvoid}, Ref{T}, Ptr{Ptr{UInt8}}, Ptr{Csize_t}),
            plan.layout.handle, msg, out, len)
    end
    _check(status, "layout encode")
    n = Int(len[])
    wire = copy(unsafe_wrap(Array, out[], n; own = false))
    return wire
end

"""Wire bytes -> the plan's native image, one ccall. The decoded pointers
live in `plan.arena`, so a later decode or freeing the plan invalidates it."""
function decode_layout(bytes::Vector{UInt8}, ::Type{T}, plan::LayoutPlan{T}) where {T}
    GC.@preserve bytes plan begin
        status = @libcall(:pbw_julia_decode, Cint,
            (Ptr{Cvoid}, Ptr{UInt8}, Csize_t, Ref{T}, Ptr{UInt8}, Csize_t),
            plan.layout.handle, isempty(bytes) ? C_NULL : bytes,
            length(bytes), plan.image, plan.arena, length(plan.arena))
    end
    _check(status, "layout decode")
    return plan.image
end

# ------------------------------------------------------------------ extras

# Bounds for decoding (zero fields = the defaults) and JSON parse options.
# Both are isbits with the same layout as the C structs.
struct _PbwLimits
    max_bytes::Csize_t
    max_depth::UInt32
end
struct _PbwJsonOptions
    ignore_unknown::Cint
end
# One scanned field, matching pbw_visit_value.
struct _PbwVisitValue
    u64::UInt64
    data::Ptr{Cvoid}
    len::Csize_t
end
const _p_visit_tramp = Ref{Ptr{Cvoid}}(C_NULL)

_bytesof(text::Union{String,AbstractVector{UInt8}}) =
    text isa String ? Vector{UInt8}(text) : text

# The allocating-writer postlude shared by the *_print/encode_* wrappers:
# copies the core buffer out and frees it.
function _take_alloc(out::Ref{Ptr{UInt8}}, len::Ref{Csize_t})
    n = Int(len[])
    if n == 0 || out[] == C_NULL
        out[] != C_NULL && @libcall(:pbw_free, Cvoid, (Ptr{Cvoid},), out[])
        return UInt8[]
    end
    wire = copy(unsafe_wrap(Array, out[], n; own = false))
    @libcall(:pbw_free, Cvoid, (Ptr{Cvoid},), out[])
    return wire
end

"""Canonical encoding: map entries sorted by key, like the reference."""
function encode_deterministic(m::Message)
    out = Ref{Ptr{UInt8}}(C_NULL)
    len = Ref{Csize_t}(0)
    GC.@preserve m begin
        status = @libcall(:pbw_encode_deterministic, Cint,
            (Ptr{Cvoid}, Ptr{Ptr{UInt8}}, Ptr{Csize_t}), m.handle, out, len)
        _check(status, "encode_deterministic")
    end
    return _take_alloc(out, len)
end

"""One length-delimited frame (varint length + message)."""
function encode_delimited(m::Message)
    out = Ref{Ptr{UInt8}}(C_NULL)
    len = Ref{Csize_t}(0)
    GC.@preserve m begin
        status = @libcall(:pbw_encode_delimited, Cint,
            (Ptr{Cvoid}, Ptr{Ptr{UInt8}}, Ptr{Csize_t}), m.handle, out, len)
        _check(status, "encode_delimited")
    end
    return _take_alloc(out, len)
end

"""Decodes one length-delimited frame, returning the message and how many
bytes the frame occupied."""
function decode_delimited(ty::MsgType, wire::Vector{UInt8})
    out = Ref{Ptr{Cvoid}}(C_NULL)
    consumed = Ref{Csize_t}(0)
    GC.@preserve ty begin
        status = @libcall(:pbw_decode_delimited, Cint,
            (Ptr{Cvoid}, Ptr{UInt8}, Csize_t, Ptr{Ptr{Cvoid}}, Ptr{Csize_t}),
            ty.desc, isempty(wire) ? C_NULL : wire, length(wire), out, consumed)
        _check(status, "decode_delimited")
    end
    return Message(ty, out[], true, nothing, length(wire)), Int(consumed[])
end

"""Decodes with explicit bounds: inputs larger than `max_bytes` fail, and
nesting past `max_depth` fails. Zero means the defaults."""
function decode_limits(ty::MsgType, wire::Vector{UInt8}, max_bytes::Integer = 0, max_depth::Integer = 0)
    limits = Ref(_PbwLimits(Csize_t(max_bytes), UInt32(max_depth)))
    out = Ref{Ptr{Cvoid}}(C_NULL)
    GC.@preserve ty limits begin
        status = @libcall(:pbw_decode_limits, Cint,
            (Ptr{Cvoid}, Ptr{UInt8}, Csize_t, Ptr{_PbwLimits}, Ptr{Ptr{Cvoid}}),
            ty.desc, isempty(wire) ? C_NULL : wire, length(wire), limits, out)
        _check(status, "decode_limits")
    end
    return Message(ty, out[], true, nothing, length(wire))
end

"""Protobuf JSON mapping. Unknown fields are not printed."""
function print_json(m::Message)
    out = Ref{Ptr{UInt8}}(C_NULL)
    len = Ref{Csize_t}(0)
    GC.@preserve m begin
        status = @libcall(:pbw_json_print, Cint,
            (Ptr{Cvoid}, Ptr{Ptr{UInt8}}, Ptr{Csize_t}), m.handle, out, len)
        _check(status, "print_json")
    end
    return String(_take_alloc(out, len))
end

"""Parses JSON mapping text. Unknown fields fail unless `ignore_unknown`;
`max_bytes` / `max_depth` bound the input (0 = the defaults)."""
function parse_json(ty::MsgType, text::Union{String,AbstractVector{UInt8}},
                    ignore_unknown::Bool = false, max_bytes::Integer = 0,
                    max_depth::Integer = 0)
    bytes = _bytesof(text)
    opts = Ref(_PbwJsonOptions(ignore_unknown ? 1 : 0))
    limits = Ref(_PbwLimits(Csize_t(max_bytes), UInt32(max_depth)))
    out = Ref{Ptr{Cvoid}}(C_NULL)
    GC.@preserve ty bytes opts limits begin
        status = @libcall(:pbw_json_parse_limits, Cint,
            (Ptr{Cvoid}, Ptr{UInt8}, Csize_t, Ptr{_PbwLimits},
             Ptr{_PbwJsonOptions}, Ptr{Ptr{Cvoid}}),
            ty.desc, isempty(bytes) ? C_NULL : bytes, length(bytes), limits,
            opts, out)
        _check(status, "parse_json")
    end
    return Message(ty, out[], true, nothing, 0)
end

"""Protobuf text format. Unknown fields print as numeric lines when
`print_unknown` is set, like the reference printer."""
function print_text(m::Message, print_unknown::Bool = false)
    out = Ref{Ptr{UInt8}}(C_NULL)
    len = Ref{Csize_t}(0)
    GC.@preserve m begin
        # pbw_text_print is the "print them" default; the opts entry is what
        # makes print_unknown false actually drop the unknown fields.
        status = @libcall(:pbw_text_print_opts, Cint,
            (Ptr{Cvoid}, Cint, Ptr{Ptr{UInt8}}, Ptr{Csize_t}), m.handle,
            print_unknown ? 1 : 0, out, len)
        _check(status, "print_text")
    end
    return String(_take_alloc(out, len))
end

"""Parses text format into a new message tree; `max_bytes` / `max_depth`
bound the input (0 = the defaults)."""
function parse_text(ty::MsgType, text::Union{String,AbstractVector{UInt8}},
                    max_bytes::Integer = 0, max_depth::Integer = 0)
    bytes = _bytesof(text)
    limits = Ref(_PbwLimits(Csize_t(max_bytes), UInt32(max_depth)))
    out = Ref{Ptr{Cvoid}}(C_NULL)
    GC.@preserve ty bytes limits begin
        status = @libcall(:pbw_text_parse_limits, Cint,
            (Ptr{Cvoid}, Ptr{UInt8}, Csize_t, Ptr{_PbwLimits},
             Ptr{Ptr{Cvoid}}),
            ty.desc, isempty(bytes) ? C_NULL : bytes, length(bytes), limits,
            out)
        _check(status, "parse_text")
    end
    return Message(ty, out[], true, nothing, 0)
end

"""Exact bytes [`encode`](@ref) would write, without writing them."""
function encoded_size(m::Message)
    size = Ref{Csize_t}(0)
    GC.@preserve m begin
        status = @libcall(:pbw_message_encoded_size, Cint,
            (Ptr{Cvoid}, Ptr{Csize_t}), m.handle, size)
        _check(status, "encoded_size")
    end
    return Int(size[])
end

"""True when every proto2 `required` field is present."""
is_initialized(m::Message) =
    (@libcall(:pbw_message_is_initialized, Cint, (Ptr{Cvoid},), m.handle) != 0)

"""Packs `payload` into an `Any` tree; `ty` is the Any descriptor."""
function any_pack(ty::MsgType, type_url::String, payload::Vector{UInt8})
    if occursin('\0', type_url)
        throw(PbwireError(-1, "any_pack: type url contains NUL"))
    end
    out = Ref{Ptr{Cvoid}}(C_NULL)
    GC.@preserve ty begin
        status = @libcall(:pbw_any_pack, Cint,
            (Ptr{Cvoid}, Cstring, Ptr{UInt8}, Csize_t, Ptr{Ptr{Cvoid}}),
            ty.desc, type_url, isempty(payload) ? C_NULL : payload, length(payload), out)
        _check(status, "any_pack")
    end
    return Message(ty, out[], true, nothing, 0)
end

"""Field 1 of a `google.protobuf.Any` tree, or `nothing` when absent."""
function any_type_url(m::Message)
    ptr = @libcall(:pbw_any_type_url, Ptr{UInt8}, (Ptr{Cvoid},), m.handle)
    return ptr == C_NULL ? nothing : unsafe_string(ptr)
end

"""Field 2 of a `google.protobuf.Any` tree, or `nothing` when absent."""
function any_payload(m::Message)
    len = Ref{Csize_t}(0)
    ptr = @libcall(:pbw_any_payload, Ptr{UInt8},
        (Ptr{Cvoid}, Ptr{Csize_t}), m.handle, len)
    return ptr == C_NULL ? nothing : copy(unsafe_wrap(Array, ptr, Int(len[]); own = false))
end

"""A `google.protobuf.Timestamp` tree as milliseconds."""
function timestamp_millis(m::Message)
    ms = Ref{Int64}(0)
    GC.@preserve m begin
        status = @libcall(:pbw_timestamp_to_millis, Cint,
            (Ptr{Cvoid}, Ptr{Int64}), m.handle, ms)
        _check(status, "timestamp_millis")
    end
    return ms[]
end

"""A `google.protobuf.Duration` tree as milliseconds."""
function duration_millis(m::Message)
    ms = Ref{Int64}(0)
    GC.@preserve m begin
        status = @libcall(:pbw_duration_to_millis, Cint,
            (Ptr{Cvoid}, Ptr{Int64}), m.handle, ms)
        _check(status, "duration_millis")
    end
    return ms[]
end

"""A new Timestamp tree holding `ms`; `ty` is the Timestamp descriptor."""
function timestamp_from_millis(ty::MsgType, ms::Integer)
    out = Ref{Ptr{Cvoid}}(C_NULL)
    GC.@preserve ty begin
        status = @libcall(:pbw_timestamp_from_millis, Cint,
            (Ptr{Cvoid}, Int64, Ptr{Ptr{Cvoid}}), ty.desc, Int64(ms), out)
        _check(status, "timestamp_from_millis")
    end
    return Message(ty, out[], true, nothing, 0)
end

"""A new Duration tree holding `ms`; `ty` is the Duration descriptor."""
function duration_from_millis(ty::MsgType, ms::Integer)
    out = Ref{Ptr{Cvoid}}(C_NULL)
    GC.@preserve ty begin
        status = @libcall(:pbw_duration_from_millis, Cint,
            (Ptr{Cvoid}, Int64, Ptr{Ptr{Cvoid}}), ty.desc, Int64(ms), out)
        _check(status, "duration_from_millis")
    end
    return Message(ty, out[], true, nothing, 0)
end

"""Every path of a `google.protobuf.FieldMask` tree."""
function fieldmask_paths(m::Message)
    n = Int(@libcall(:pbw_fieldmask_path_count, Csize_t, (Ptr{Cvoid},), m.handle))
    return [unsafe_string(@libcall(:pbw_fieldmask_path_at, Ptr{UInt8},
        (Ptr{Cvoid}, Csize_t), m.handle, Csize_t(i))) for i in 0:(n - 1)]
end

# --------------------------------------------------------------- services

"""A service from a [`Schema`](@ref): routing metadata, not RPC."""
struct Service
    schema::Schema # keeps the schema alive while the service is used
    ptr::Ptr{Cvoid}
end

"""One service method: fully qualified input/output type names."""
struct Method
    schema::Schema
    ptr::Ptr{Cvoid}
end

"""How many services the loaded files declare."""
service_count(s::Schema) =
    Int(@libcall(:pbw_schema_service_count, Csize_t, (Ptr{Cvoid},), s.handle))

"""One service by 1-based index."""
function service_at(s::Schema, i::Integer)
    ptr = @libcall(:pbw_schema_service_at, Ptr{Cvoid},
        (Ptr{Cvoid}, Csize_t), s.handle, Csize_t(i - 1))
    ptr == C_NULL && throw(PbwireError(-1, "no service at index $i"))
    return Service(s, ptr)
end

"""One service by fully qualified name, e.g. `pkg.Service`."""
function service(s::Schema, name::String)
    ptr = @libcall(:pbw_schema_service_find, Ptr{Cvoid},
        (Ptr{Cvoid}, Cstring), s.handle, name)
    ptr == C_NULL && throw(PbwireError(-1, "no such service: $name"))
    return Service(s, ptr)
end

"""Fully qualified service name, without the leading dot."""
service_name(svc::Service) =
    unsafe_string(@libcall(:pbw_service_name, Ptr{UInt8}, (Ptr{Cvoid},), svc.ptr))

"""How many methods a service declares."""
method_count(svc::Service) =
    Int(@libcall(:pbw_service_method_count, Csize_t, (Ptr{Cvoid},), svc.ptr))

"""One method by 1-based index."""
function method_at(svc::Service, i::Integer)
    ptr = @libcall(:pbw_service_method_at, Ptr{Cvoid},
        (Ptr{Cvoid}, Csize_t), svc.ptr, Csize_t(i - 1))
    ptr == C_NULL && throw(PbwireError(-1, "no method at index $i"))
    return Method(svc.schema, ptr)
end

"""One method by name."""
function method(svc::Service, name::String)
    ptr = @libcall(:pbw_service_method_find, Ptr{Cvoid},
        (Ptr{Cvoid}, Cstring), svc.ptr, name)
    ptr == C_NULL && throw(PbwireError(-1, "no such method: $name"))
    return Method(svc.schema, ptr)
end

method_name(m::Method) =
    unsafe_string(@libcall(:pbw_method_name, Ptr{UInt8}, (Ptr{Cvoid},), m.ptr))
method_input(m::Method) =
    unsafe_string(@libcall(:pbw_method_input_type, Ptr{UInt8}, (Ptr{Cvoid},), m.ptr))
method_output(m::Method) =
    unsafe_string(@libcall(:pbw_method_output_type, Ptr{UInt8}, (Ptr{Cvoid},), m.ptr))
client_streaming(m::Method) =
    @libcall(:pbw_method_client_streaming, Cint, (Ptr{Cvoid},), m.ptr) != 0
server_streaming(m::Method) =
    @libcall(:pbw_method_server_streaming, Cint, (Ptr{Cvoid},), m.ptr) != 0

"""Resolves `service`/`method` to its input/output message types: decode
with the first, encode with the second."""
function resolve_method(s::Schema, svc::String, meth::String)
    in_ptr = Ref{Ptr{Cvoid}}(C_NULL)
    out_ptr = Ref{Ptr{Cvoid}}(C_NULL)
    GC.@preserve s begin
        status = @libcall(:pbw_service_resolve, Cint,
            (Ptr{Cvoid}, Cstring, Cstring, Ptr{Ptr{Cvoid}}, Ptr{Ptr{Cvoid}}),
            s.handle, svc, meth, in_ptr, out_ptr)
        _check(status, "resolve_method")
    end
    return _msgtype(s, in_ptr[]), _msgtype(s, out_ptr[])
end

# ------------------------------------------------------------------ visit

function _visit_tramp(ud::Ptr{Cvoid}, fptr::Ptr{Cvoid}, number::UInt32,
        wire_type::UInt32, value::Ptr{Cvoid})::Cint
    box = unsafe_pointer_to_objref(ud)::Ref{Any}
    cb, ty, _ = box[]
    try
        f = fptr == C_NULL ? nothing : _field(ty, fptr)
        vv = unsafe_load(Ptr{_PbwVisitValue}(value))
        data = vv.data == C_NULL ? nothing :
               copy(unsafe_wrap(Array, Ptr{UInt8}(vv.data), Int(vv.len); own = false))
        sub = (f === nothing || (f.type != _T_MESSAGE && f.type != _T_GROUP)) ? nothing :
              _nested_type(ty, f)
        cont = cb((field = f, number = Int(number), wire_type = Int(wire_type),
            raw = vv.u64, data = data, subtype = sub))
        return cont === false ? Cint(1) : Cint(0)
    catch e
        box[] = (cb, ty, e)
        return Cint(2)
    end
end

"""Single-level scan of `wire` without building a message tree: one callback
per field, no allocation. Nested messages and packed runs arrive as byte
payloads (`data`) for the caller to recurse into. Return `false` from `cb` to
abort the scan (an error); a callback exception propagates."""
function visit(ty::MsgType, wire::Vector{UInt8}, cb::Function)
    box = Ref{Any}((cb, ty, nothing))
    fail_off = Ref{Csize_t}(0)
    status = GC.@preserve ty wire box begin
        ud = Base.pointer_from_objref(box)
        @libcall(:pbw_visit, Cint,
            (Ptr{Cvoid}, Ptr{UInt8}, Csize_t, Ptr{Cvoid}, Ptr{Cvoid}, Ptr{Csize_t}),
            ty.desc, isempty(wire) ? C_NULL : wire, length(wire),
            _p_visit_tramp[], ud, fail_off)
    end
    exc = box[][3]
    exc !== nothing && throw(exc)
    _check(status, "visit")
    return nothing
end

function __init__()
    # Eagerly fail on a missing/unloadable library, then pin every symbol.
    _handle[] = Libdl.dlopen(_libpath())
    _p_pbw_any_pack[] = Libdl.dlsym(_handle[], :pbw_any_pack)
    _p_pbw_any_payload[] = Libdl.dlsym(_handle[], :pbw_any_payload)
    _p_pbw_any_type_url[] = Libdl.dlsym(_handle[], :pbw_any_type_url)
    _p_pbw_ctx_free[] = Libdl.dlsym(_handle[], :pbw_ctx_free)
    _p_pbw_ctx_new[] = Libdl.dlsym(_handle[], :pbw_ctx_new)
    _p_pbw_ctx_reset[] = Libdl.dlsym(_handle[], :pbw_ctx_reset)
    _p_pbw_decode[] = Libdl.dlsym(_handle[], :pbw_decode)
    _p_pbw_decode_ctx[] = Libdl.dlsym(_handle[], :pbw_decode_ctx)
    _p_pbw_decode_delimited[] = Libdl.dlsym(_handle[], :pbw_decode_delimited)
    _p_pbw_decode_limits[] = Libdl.dlsym(_handle[], :pbw_decode_limits)
    _p_pbw_duration_from_millis[] = Libdl.dlsym(_handle[], :pbw_duration_from_millis)
    _p_pbw_duration_to_millis[] = Libdl.dlsym(_handle[], :pbw_duration_to_millis)
    _p_pbw_encode_delimited[] = Libdl.dlsym(_handle[], :pbw_encode_delimited)
    _p_pbw_encode_deterministic[] = Libdl.dlsym(_handle[], :pbw_encode_deterministic)
    _p_pbw_fieldmask_path_at[] = Libdl.dlsym(_handle[], :pbw_fieldmask_path_at)
    _p_pbw_fieldmask_path_count[] = Libdl.dlsym(_handle[], :pbw_fieldmask_path_count)
    _p_pbw_json_parse_opts[] = Libdl.dlsym(_handle[], :pbw_json_parse_opts)
    _p_pbw_json_parse_limits[] = Libdl.dlsym(_handle[], :pbw_json_parse_limits)
    _p_pbw_json_print[] = Libdl.dlsym(_handle[], :pbw_json_print)
    _p_pbw_method_client_streaming[] = Libdl.dlsym(_handle[], :pbw_method_client_streaming)
    _p_pbw_method_input_type[] = Libdl.dlsym(_handle[], :pbw_method_input_type)
    _p_pbw_method_name[] = Libdl.dlsym(_handle[], :pbw_method_name)
    _p_pbw_method_output_type[] = Libdl.dlsym(_handle[], :pbw_method_output_type)
    _p_pbw_method_server_streaming[] = Libdl.dlsym(_handle[], :pbw_method_server_streaming)
    _p_pbw_descriptor_find_field_name[] = Libdl.dlsym(_handle[], :pbw_descriptor_find_field_name)
    _p_pbw_descriptor_name[] = Libdl.dlsym(_handle[], :pbw_descriptor_name)
    _p_pbw_descriptor_set_load[] = Libdl.dlsym(_handle[], :pbw_descriptor_set_load)
    _p_pbw_encode[] = Libdl.dlsym(_handle[], :pbw_encode)
    _p_pbw_encode_to[] = Libdl.dlsym(_handle[], :pbw_encode_to)
    _p_pbw_field_label[] = Libdl.dlsym(_handle[], :pbw_field_label)
    _p_pbw_field_message_type[] = Libdl.dlsym(_handle[], :pbw_field_message_type)
    _p_pbw_field_name[] = Libdl.dlsym(_handle[], :pbw_field_name)
    _p_pbw_field_number[] = Libdl.dlsym(_handle[], :pbw_field_number)
    _p_pbw_field_type[] = Libdl.dlsym(_handle[], :pbw_field_type)
    _p_pbw_free[] = Libdl.dlsym(_handle[], :pbw_free)
    _p_pbw_message_clear[] = Libdl.dlsym(_handle[], :pbw_message_clear)
    _p_pbw_message_encoded_size[] = Libdl.dlsym(_handle[], :pbw_message_encoded_size)
    _p_pbw_message_field_at[] = Libdl.dlsym(_handle[], :pbw_message_field_at)
    _p_pbw_message_field_count[] = Libdl.dlsym(_handle[], :pbw_message_field_count)
    _p_pbw_message_is_initialized[] = Libdl.dlsym(_handle[], :pbw_message_is_initialized)
    _p_pbw_message_free[] = Libdl.dlsym(_handle[], :pbw_message_free)
    _p_pbw_message_get_field[] = Libdl.dlsym(_handle[], :pbw_message_get_field)
    _p_pbw_message_new[] = Libdl.dlsym(_handle[], :pbw_message_new)
    _p_pbw_message_set_field[] = Libdl.dlsym(_handle[], :pbw_message_set_field)
    _p_pbw_message_set_message[] = Libdl.dlsym(_handle[], :pbw_message_set_message)
    _p_pbw_message_wire_size[] = Libdl.dlsym(_handle[], :pbw_message_wire_size)
    _p_pbw_schema_error[] = Libdl.dlsym(_handle[], :pbw_schema_error)
    _p_pbw_schema_find[] = Libdl.dlsym(_handle[], :pbw_schema_find)
    _p_pbw_schema_free[] = Libdl.dlsym(_handle[], :pbw_schema_free)
    _p_pbw_schema_service_at[] = Libdl.dlsym(_handle[], :pbw_schema_service_at)
    _p_pbw_schema_service_count[] = Libdl.dlsym(_handle[], :pbw_schema_service_count)
    _p_pbw_schema_service_find[] = Libdl.dlsym(_handle[], :pbw_schema_service_find)
    _p_pbw_service_method_at[] = Libdl.dlsym(_handle[], :pbw_service_method_at)
    _p_pbw_service_method_count[] = Libdl.dlsym(_handle[], :pbw_service_method_count)
    _p_pbw_service_method_find[] = Libdl.dlsym(_handle[], :pbw_service_method_find)
    _p_pbw_service_name[] = Libdl.dlsym(_handle[], :pbw_service_name)
    _p_pbw_service_resolve[] = Libdl.dlsym(_handle[], :pbw_service_resolve)
    _p_pbw_text_parse[] = Libdl.dlsym(_handle[], :pbw_text_parse)
    _p_pbw_text_parse_limits[] = Libdl.dlsym(_handle[], :pbw_text_parse_limits)
    _p_pbw_text_print[] = Libdl.dlsym(_handle[], :pbw_text_print)
    _p_pbw_text_print_opts[] = Libdl.dlsym(_handle[], :pbw_text_print_opts)
    _p_pbw_timestamp_from_millis[] = Libdl.dlsym(_handle[], :pbw_timestamp_from_millis)
    _p_pbw_timestamp_to_millis[] = Libdl.dlsym(_handle[], :pbw_timestamp_to_millis)
    _p_pbw_visit[] = Libdl.dlsym(_handle[], :pbw_visit)
    _p_visit_tramp[] = @cfunction(_visit_tramp, Cint,
        (Ptr{Cvoid}, Ptr{Cvoid}, UInt32, UInt32, Ptr{Cvoid}))
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

# The dynamic message tree: one slot per declared field, plus the oneof cases
# and the retained unknown fields.
#
# Scalars live in their wire-neutral Julia form:
#
#   * signed 32/64-bit kinds (int32/enum/sint32/sfixed32, int64/sint64/
#     sfixed64) in an `Int64`, unsigned kinds (uint32/fixed32, uint64/fixed64)
#     in a `UInt64` -- never truncated to what a signed host integer could
#     hold;
#   * float/double as their raw bit patterns (`UInt32`/`UInt64`), so a NaN
#     payload survives a round trip;
#   * `Bool` for bool;
#   * strings and bytes decoded off the wire stay a `Raw` slice of the input
#     until materialized, so invalid UTF-8 and non-minimal length prefixes
#     re-encode byte for byte;
#   * messages nested as `Message`.
#
# `Raw` never wraps memory it does not own: it holds the input
# `Vector{UInt8}` itself, so the garbage collector keeps the buffer alive and
# no `unsafe_wrap` is involved.

"""An undecoded field image (tag + length + payload) inside the input.

`off`/`len` cover the whole image (tag included) so a re-encode can copy it
verbatim; `poff`/`plen` point at the payload so materializing does not have to
re-read the tag and length."""
struct Raw
    b::Vector{UInt8}
    off::Int
    len::Int
    poff::Int
    plen::Int
end

function Base.show(io::IO, r::Raw)
    print(io, "<Raw ", r.off, "+", r.len, ">")
end

"""One message value: per-slot values, the oneof cases and unknowns."""
mutable struct Message
    def::MsgDef
    v::Vector{Any}
    # Slot+1 per oneof, 0 = none set; `nothing` when the message has none.
    oneof_case::Union{Nothing,Vector{Int}}
    # Retained unknown fields in input order; `nothing` until the first.
    unknown::Union{Nothing,Vector{Raw}}
end

function Message(def::MsgDef)
    n = length(def.fields)
    cases = def.oneof_count > 0 ? zeros(Int, def.oneof_count) : nothing
    return Message(def, fill!(Vector{Any}(undef, n), nothing), cases, nothing)
end

Base.show(io::IO, m::Message) = print(io, "<Message ", m.def.full_name, ">")

"""Slot index of a field name, or 0."""
function slot_by_name(def::MsgDef, name::AbstractString)
    return get(def.by_name, String(name), 0)
end

function require_slot(def::MsgDef, name_or_number)
    slot = name_or_number isa Integer ?
        slot_of(def, Int(name_or_number)) :
        slot_by_name(def, String(name_or_number))
    if slot < 1
        throw(CoreError("$(def.full_name) has no field $name_or_number"))
    end
    return slot
end

"""Stores a value in a slot, collapsing the oneof it belongs to."""
function set_slot!(m::Message, slot::Int, value)
    f = m.def.fields[slot]::Field
    if f.oneof >= 0 && m.oneof_case !== nothing
        cases = m.oneof_case
        prev = cases[f.oneof + 1]
        if prev != 0 && prev != slot + 1
            m.v[prev - 1] = nothing # prev is slot + 1; slots are 1-based
        end
        cases[f.oneof + 1] = slot + 1
    end
    m.v[slot] = value
    return nothing
end

has_field(m::Message, name_or_number) =
    m.v[require_slot(m.def, name_or_number)] !== nothing

"""The value of a field: materialized, converted, or `nothing`."""
function get_field(m::Message, name_or_number)
    slot = require_slot(m.def, name_or_number)
    f = m.def.fields[slot]::Field
    v = m.v[slot]
    v === nothing && return nothing
    if f.repeated
        return Any[to_value(f, item) for item in v]
    end
    return to_value(f, v)
end

"""Which member of a oneof is set: its field number, or 0 for none."""
function oneof_case(m::Message, oneof_name::AbstractString)
    idx = get(m.def.oneof_by_name, String(oneof_name), -1)
    if idx < 0 || m.oneof_case === nothing
        throw(CoreError("$(m.def.full_name) has no oneof $oneof_name"))
    end
    case = m.oneof_case[idx + 1]
    # A case word holds slot + 1 (so 0 means "none"); slots are 1-based here.
    return case == 0 ? 0 : (m.def.fields[case - 1]::Field).number
end

"""The payload bytes of a Raw string/bytes field image."""
raw_payload(raw::Raw) = Vector{UInt8}(@view raw.b[raw.poff:(raw.poff + raw.plen - 1)])

"""Materializes a Raw slice into its Julia value."""
function materialize(f::Field, raw::Raw)
    off = raw.poff
    last = raw.poff + raw.plen - 1
    if f.kind == K_STRING
        # proto3/editions strings were validated at parse; the loose mapping
        # (U+FFFD per malformed unit) only applies to proto2 strings.
        return f.utf8 ? String(raw.b[off:last]) : utf8_lossy(raw.b, off, raw.plen)
    end
    return Vector{UInt8}(@view raw.b[off:last])
end

"""One value in its Julia form (what `get_field`/`to_dict` expose)."""
function to_value(f::Field, v)
    if v isa Raw
        return materialize(f, v)
    end
    if f.kind == K_FLOAT
        return bits_float(v::UInt32)
    elseif f.kind == K_DOUBLE
        return bits_double(v::UInt64)
    end
    return v
end

"""A plain `Dict`: proto field names, nested dicts, Julia scalars."""
function to_dict(m::Message)
    def = m.def
    out = Dict{String,Any}()
    for (slot, f) in enumerate(def.fields)
        f = f::Field
        v = m.v[slot]
        v === nothing && continue
        if f.repeated
            isempty(v) && continue
            if f.kind == K_MESSAGE || f.kind == K_GROUP
                out[f.name] = Any[to_dict(item::Message) for item in v]
            else
                out[f.name] = Any[to_value(f, item) for item in v]
            end
            continue
        end
        if f.kind == K_MESSAGE || f.kind == K_GROUP
            out[f.name] = to_dict(v::Message)
            continue
        end
        out[f.name] = to_value(f, v)
    end
    return out
end

"""One Julia value in its storage form (the inverse of `to_value`)."""
function from_value(f::Field, v)
    k = f.kind
    if k == K_INT32 || k == K_SINT32 || k == K_SFIXED32 || k == K_ENUM
        return Int64(reinterpret(Int32, UInt32(_ubits(v) & 0xFFFFFFFF)))
    elseif k == K_UINT32 || k == K_FIXED32
        return _ubits(v) & 0xFFFFFFFF
    elseif k == K_INT64 || k == K_SINT64 || k == K_SFIXED64
        return reinterpret(Int64, _ubits(v))
    elseif k == K_UINT64 || k == K_FIXED64
        return _ubits(v)
    elseif k == K_BOOL
        return v isa Bool ? v : v != 0
    elseif k == K_FLOAT
        return v isa UInt32 ? v : float_bits(v)
    elseif k == K_DOUBLE
        return v isa UInt64 ? v : double_bits(v)
    elseif k == K_STRING
        return v isa Raw ? v : String(v)
    elseif k == K_BYTES
        return v isa Raw ? v : Vector{UInt8}(v)
    end
    # K_MESSAGE / K_GROUP
    v isa Message && return v
    return from_dict(f.msg_type, v)
end

"""Builds a tree from a plain dict (proto field names, absent/`nothing` keys
stay absent)."""
function from_dict(def::MsgDef, obj::AbstractDict)
    m = Message(def)
    for (slot, f) in enumerate(def.fields)
        f = f::Field
        v = get(obj, f.name, nothing)
        v === nothing && continue
        try
            if f.repeated
                if !(v isa AbstractVector || v isa Tuple)
                    throw(CoreError("needs a vector"))
                end
                m.v[slot] = Any[from_value(f, item) for item in v]
            else
                set_slot!(m, slot, from_value(f, v))
            end
        catch e
            e isa CoreError && rethrow()
            throw(CoreError("$(def.full_name).$(f.name): $(sprint(showerror, e))"))
        end
    end
    return m
end

from_dict(def::MsgDef, obj) =
    throw(CoreError("$(def.full_name): needs a dict"))

"""Throws when a REQUIRED field of this message is absent."""
function check_present_required(m::Message, slot::Int)
    if m.v[slot] === nothing && (m.def.fields[slot]::Field).presence == REQUIRED
        f = m.def.fields[slot]::Field
        throw(CoreError("missing required field $(m.def.full_name).$(f.name)", 6))
    end
    return nothing
end

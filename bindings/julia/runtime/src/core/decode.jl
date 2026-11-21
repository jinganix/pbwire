# Decoder: strict tags and lengths (like the C core), unknown field numbers
# AND wrong-wire-type occurrences retained as raw tag+payload bytes in input
# order, groups decoded to their matching end tag, packed runs accepted for
# every numeric kind (and the unpacked element form for a packed field), oneofs
# collapsed to the last member, presence following the field's own zero, proto2
# required fields checked over the whole tree, and the nesting cap enforced on
# every recursive path.

"""Decodes a whole message, unknown fields retained."""
function decode(def::MsgDef, data::AbstractVector{UInt8};
        limits::Union{Nothing,Limits} = nothing)
    bytes = _owned(data)
    check_bytes(limits, length(bytes), "message")
    r = Reader(bytes)
    r.cap = effective_depth(limits)
    m = Message(def)
    decode_into(m, r, length(bytes), -1, 0)
    if def.deep_required
        check_required(m, r.cap, 0)
    end
    return m
end

"""Decodes a message body that already starts at `off` (1-based) and is
`len` bytes long."""
function decode_slice(def::MsgDef, data::AbstractVector{UInt8}, off::Int,
        len::Int; limits::Union{Nothing,Limits} = nothing)
    bytes = _owned(data)
    check_bytes(limits, len, "message")
    r = Reader(bytes, off, off + len - 1)
    r.cap = effective_depth(limits)
    m = Message(def)
    decode_into(m, r, off + len - 1, -1, 0)
    if def.deep_required
        check_required(m, r.cap, 0)
    end
    return m
end

# The tree keeps `Raw` slices of the input, so the input must be a
# `Vector{UInt8}` this core holds: a different array type is copied once, never
# wrapped.
_owned(data::Vector{UInt8}) = data
_owned(data::AbstractVector{UInt8}) = Vector{UInt8}(data)

"""An empty message of `def`."""
new_message(def::MsgDef) = Message(def)

"""True when the field does not accept this wire type. A repeated numeric
field always accepts the LEN form (a packed run, whatever the declared form)
and a packed field also accepts its unpacked element form."""
function wt_incompatible(f::Field, wt::Int)
    if f.kind == K_GROUP
        return wt != WT_START_GROUP
    end
    if f.repeated && is_packable(f.kind)
        wt == WT_LEN && return false
        f.packed && return wt != f.elem_wt
    end
    return wt != f.wt
end

"""Decodes into `m` up to `stop` (inclusive; or the matching end-group when
`group_end` >= 0). One shared reader threads through the recursion, so
positions never need saving: a nested payload consumes exactly its slice."""
function decode_into(m::Message, r::Reader, stop::Int, group_end::Int,
        depth::Int)
    depth > r.cap && throw(CoreError("nesting exceeds $(r.cap)", 4))
    def = m.def
    outer_stop = r.stop
    r.stop = stop
    closed = group_end < 0
    fields = def.fields
    while r.pos <= r.stop
        field_start = r.pos
        tag = r_tag(r)
        fnum = Int(tag >> 3)
        wt = Int(tag & 0x7)
        if fnum == 0 || wt > WT_FIXED32
            throw(CoreError("bad tag $tag", 1))
        end
        if wt == WT_END_GROUP
            # Only our own number closes us; any other end tag is stray.
            if group_end < 0 || fnum != group_end
                throw(CoreError("stray end-group for $fnum", 1))
            end
            closed = true
            break
        end
        slot = slot_of(def, fnum)
        if slot < 0 || wt_incompatible(fields[slot]::Field, wt)
            retain_unknown(m, r, field_start, fnum, wt, depth)
            continue
        end
        read_known(m, r, fields[slot]::Field, slot, wt, field_start, depth)
    end
    r.stop = outer_stop
    if !closed
        throw(CoreError("missing end-group for $group_end", 1))
    end
    return nothing
end

"""Wrong wire type (or unknown number) keeps the raw blob, like the core.
Groups validate to their matching end tag; anything else only checks value
shape -- unknown LEN payloads stay opaque."""
function retain_unknown(m::Message, r::Reader, field_start::Int, fnum::Int,
        wt::Int, depth::Int)
    skip_unknown(r, fnum, wt, depth)
    if m.unknown === nothing
        m.unknown = Raw[]
    end
    push!(m.unknown, Raw(r.b, field_start, r.pos - field_start, 0, 0))
    return nothing
end

function read_known(m::Message, r::Reader, f::Field, slot::Int, wt::Int,
        field_start::Int, depth::Int)
    kind = f.kind
    if f.repeated
        arr = m.v[slot]
        if arr === nothing
            arr = Any[]
            m.v[slot] = arr
        end
        if is_packable(kind)
            if wt == WT_LEN
                read_packed(r, f, arr)
                if isempty(arr)
                    # An empty packed run adds nothing: presence follows
                    # length, so drop the holder again.
                    m.v[slot] = nothing
                end
                return nothing
            end
            push!(arr, read_num_single(r, kind))
            return nothing
        end
        if kind == K_MESSAGE
            length_ = r_len32(r)
            sub = Message(f.msg_type)
            decode_into(sub, r, r.pos + length_ - 1, -1, depth + 1)
            push!(arr, sub)
            return nothing
        end
        if kind == K_GROUP
            sub = Message(f.msg_type)
            decode_into(sub, r, r.stop, f.number, depth + 1)
            push!(arr, sub)
            return nothing
        end
        # K_STRING / K_BYTES element: validate, then record one slice.
        length_ = r_len32(r)
        if f.utf8 && !utf8_valid(r.b, r.pos, length_)
            throw(CoreError("invalid UTF-8 in a string field", 1))
        end
        poff = r.pos
        r.pos += length_
        push!(arr, Raw(r.b, field_start, r.pos - field_start, poff, length_))
        return nothing
    end
    if is_packable(kind)
        v = read_num_single(r, kind)
        # Implicit presence is decided on the *field's* zero, not the raw wire
        # bits: an int32/uint32 varint wider than 32 bits truncates and the
        # reference then drops the field as an implicit zero.
        if f.presence == IMPLICIT && v == 0
            return nothing
        end
        set_slot!(m, slot, v)
        return nothing
    end
    # Ref-kind singular: a second occurrence merges (like the core).
    if kind == K_MESSAGE
        length_ = r_len32(r)
        existing = merge_target(m, f, slot)
        if existing !== nothing
            decode_into(existing, r, r.pos + length_ - 1, -1, depth + 1)
        else
            sub = Message(f.msg_type)
            decode_into(sub, r, r.pos + length_ - 1, -1, depth + 1)
            set_slot!(m, slot, sub)
        end
        return nothing
    end
    if kind == K_GROUP
        existing = merge_target(m, f, slot)
        if existing !== nothing
            decode_into(existing, r, r.stop, f.number, depth + 1)
        else
            sub = Message(f.msg_type)
            decode_into(sub, r, r.stop, f.number, depth + 1)
            set_slot!(m, slot, sub)
        end
        return nothing
    end
    # K_STRING / K_BYTES (an empty implicit string stays absent).
    length_ = r_len32(r)
    if length_ == 0 && f.presence == IMPLICIT
        return nothing
    end
    # proto3/editions strings are validated here, at parse, like the reference:
    # a malformed value fails the decode instead of surfacing as U+FFFD when
    # the slice is materialized. proto2 keeps the loose mapping.
    if f.utf8 && !utf8_valid(r.b, r.pos, length_)
        throw(CoreError("invalid UTF-8 in a string field", 1))
    end
    poff = r.pos
    r.pos += length_
    set_slot!(m, slot, Raw(r.b, field_start, r.pos - field_start, poff, length_))
    return nothing
end

"""The submessage a repeated singular message/group field merges into, or
`nothing` for the first occurrence."""
function merge_target(m::Message, f::Field, slot::Int)
    v = m.v[slot]
    v === nothing && return nothing
    if f.oneof >= 0 && m.oneof_case !== nothing &&
       m.oneof_case[f.oneof + 1] != slot + 1
        return nothing
    end
    return v isa Message ? v : nothing
end

"""One packed run of numeric elements into `arr`."""
function read_packed(r::Reader, f::Field, arr)
    length_ = r_len32(r)
    last = r.pos + length_ - 1
    kind = f.kind
    if kind == K_BOOL
        while r.pos <= last
            push!(arr, r_var_bool(r))
        end
    elseif kind == K_FIXED32 || kind == K_SFIXED32 || kind == K_FLOAT
        count = length_ ÷ 4
        for _ in 1:count
            v = r_fixed32(r)
            push!(arr, kind == K_SFIXED32 ? signed32(v) : v)
        end
    elseif kind == K_FIXED64 || kind == K_SFIXED64 || kind == K_DOUBLE
        count = length_ ÷ 8
        for _ in 1:count
            v = r_fixed64(r)
            push!(arr, kind == K_SFIXED64 ? signed64(v) : v)
        end
    else
        while r.pos <= last
            push!(arr, read_packed_var(r, kind))
        end
    end
    if r.pos != last + 1
        throw(CoreError("packed overrun", 1))
    end
    return nothing
end

function read_packed_var(r::Reader, kind::Int)
    kind == K_INT32 && return signed32(r_var(r))
    kind == K_ENUM && return signed32(r_var(r))
    kind == K_UINT32 && return UInt64(r_var(r))
    if kind == K_SINT32
        # Truncate before unzigzag: a 64-bit varint on a sint32 field decodes
        # its low 32 bits, like the reference.
        return unzig32(r_var(r))
    end
    kind == K_INT64 && return signed64(r_var64(r))
    kind == K_UINT64 && return r_var64(r)
    kind == K_SINT64 && return unzig64(r_var64(r))
    throw(CoreError("not numeric", 1))
end

"""One numeric element in its storage form (see tree.jl)."""
function read_num_single(r::Reader, kind::Int)
    kind == K_INT32 && return signed32(r_var(r))
    kind == K_ENUM && return signed32(r_var(r))
    kind == K_UINT32 && return UInt64(r_var(r))
    kind == K_BOOL && return r_var_bool(r)
    kind == K_SINT32 && return unzig32(r_var(r))
    kind == K_INT64 && return signed64(r_var64(r))
    kind == K_UINT64 && return r_var64(r)
    kind == K_SINT64 && return unzig64(r_var64(r))
    kind == K_FIXED32 && return r_fixed32(r)
    kind == K_SFIXED32 && return signed32(r_fixed32(r))
    kind == K_FLOAT && return r_fixed32(r)
    kind == K_FIXED64 && return r_fixed64(r)
    kind == K_SFIXED64 && return signed64(r_fixed64(r))
    kind == K_DOUBLE && return r_fixed64(r)
    throw(CoreError("not numeric", 1))
end

"""Whole-tree required check: every present nested message is checked too, so
a missing proto2 required field fails the decode, like the core."""
function check_required(m::Message, cap::Int, depth::Int)
    depth > cap && throw(CoreError("nesting exceeds $cap", 4))
    def = m.def
    if def.has_required
        for (slot, f) in enumerate(def.fields)
            f = f::Field
            if f.presence == REQUIRED && m.v[slot] === nothing
                throw(CoreError(
                    "missing required field $(def.full_name).$(f.name)", 6))
            end
        end
    end
    for (slot, f) in enumerate(def.fields)
        f = f::Field
        (f.kind == K_MESSAGE || f.kind == K_GROUP) || continue
        v = m.v[slot]
        v === nothing && continue
        if f.repeated
            for item in v
                check_required(item::Message, cap, depth + 1)
            end
        elseif v isa Message
            check_required(v, cap, depth + 1)
        end
    end
    return nothing
end

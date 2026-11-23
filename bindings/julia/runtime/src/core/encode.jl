# Encoder: one pass into a reusable buffer, fields in schema (field-number)
# order, unknown fields appended verbatim in input order. Nested payloads use a
# one-byte length placeholder patched afterwards, packed runs use a five-byte
# placeholder shifted down once, and untouched `Raw` slices copy verbatim (tag
# included), so a decode/encode round trip is byte-exact.

# One workspace buffer per task: an encode pays no allocation but the result
# copy. Not re-entrant; the core is one-thread-per-message by contract.
const _WORKSPACE = Writer()

"""Encodes a tree; a tree that came from `decode` re-emits byte-exactly."""
function encode(m::Message)
    w = _WORKSPACE
    reset!(w)
    encode_into(m, w, 0)
    return Vector{UInt8}(@view w.b[1:(w.pos - 1)])
end

function encode_into(m::Message, w::Writer, depth::Int)
    depth > MAX_DEPTH && throw(CoreError("nesting exceeds $MAX_DEPTH", 4))
    def = m.def
    no_required = !def.has_required # proto3: skip the check
    values = m.v
    for (slot, f) in enumerate(def.fields)
        f = f::Field
        v = values[slot]
        if v === nothing
            if !no_required && f.presence == REQUIRED
                throw(CoreError(
                    "missing required field $(def.full_name).$(f.name)", 6))
            end
            continue
        end
        kind = f.kind
        if f.repeated
            isempty(v) && continue
            if is_packable(kind)
                encode_rep_num(w, f, v)
            else
                for item in v
                    write_value(w, f, item, depth)
                end
            end
            continue
        end
        if is_packable(kind)
            # Only implicit defaults skip here; explicit defaults (and
            # required) emit once set.
            if f.presence == IMPLICIT && _is_zero(v)
                continue
            end
            write_singular_num(w, f, v)
            continue
        end
        # Implicit empty strings and bytes stay silent, everything else emits.
        if f.presence == IMPLICIT && _is_empty(v)
            continue
        end
        write_value(w, f, v, depth)
    end
    unknown = m.unknown
    if unknown !== nothing
        # Unknown fields last, in input order.
        b = w.b
        for u in unknown
            n = u.len
            ensure(w, n)
            copyto!(b, w.pos, u.b, u.off, n)
            w.pos += n
        end
    end
    return nothing
end

@inline _is_zero(v) = v == 0
@inline _is_empty(v) = !(v isa Raw) && isempty(v)

function write_value(w::Writer, f::Field, v, depth::Int)
    if v isa Raw
        # Tag included: a verbatim copy stays byte-exact.
        ensure(w, v.len)
        copyto!(w.b, w.pos, v.b, v.off, v.len)
        w.pos += v.len
        return nothing
    end
    kind = f.kind
    if kind == K_STRING
        w_bytes(w, f.tag, utf8_encode(v))
        return nothing
    end
    if kind == K_BYTES
        w_bytes(w, f.tag, v::Vector{UInt8})
        return nothing
    end
    if kind == K_MESSAGE
        # Inlined tag + length placeholder + patch: a repeated message field is
        # the hottest nesting shape.
        tag = f.tag
        ensure(w, length(tag) + 1)
        b = w.b
        p = w.pos
        @inbounds for c in tag
            b[p] = c
            p += 1
        end
        lp = p
        @inbounds b[p] = 0
        w.pos = p + 1
        encode_into(v::Message, w, depth + 1)
        patch_len(w, lp)
        return nothing
    end
    # K_GROUP: start tag, body, end tag.
    w_tag(w, f.tag)
    encode_into(v::Message, w, depth + 1)
    w_tag(w, f.tag_end)
    return nothing
end

"""Singular numeric write: one fused tag+value call per field."""
function write_singular_num(w::Writer, f::Field, v)
    kind = f.kind
    if kind == K_INT32 || kind == K_ENUM
        # Encode the int32 interpretation, sign-extended: the reference
        # truncates a wide varint to int32 and re-emits a negative as 10 bytes.
        v >= 0 ? w_tag_var32(w, f.tag, v) : w_tag_var64(w, f.tag, v)
    elseif kind == K_UINT32
        w_tag_var32(w, f.tag, v)
    elseif kind == K_SINT32
        w_tag_var32(w, f.tag, zig32(v))
    elseif kind == K_INT64 || kind == K_UINT64
        w_tag_var64(w, f.tag, v)
    elseif kind == K_SINT64
        w_tag_var64(w, f.tag, zig64(v))
    elseif kind == K_BOOL
        w_tag_var32(w, f.tag, v ? 1 : 0)
    elseif kind == K_FIXED32 || kind == K_SFIXED32 || kind == K_FLOAT
        w_tag_fixed32(w, f.tag, v)
    else # K_FIXED64, K_SFIXED64, K_DOUBLE
        w_tag_fixed64(w, f.tag, v)
    end
    return nothing
end

"""One dispatch per field; the loops below it are branch-free writes."""
function encode_rep_num(w::Writer, f::Field, arr)
    n = length(arr)
    kind = f.kind
    if f.packed
        if kind == K_BOOL
            w_tag(w, f.tag)
            w_len(w, n)
            ensure(w, n)
            b = w.b
            p = w.pos
            @inbounds for v in arr
                b[p] = v ? 0x01 : 0x00
                p += 1
            end
            w.pos = p
            return nothing
        end
        if kind == K_FIXED32 || kind == K_SFIXED32 || kind == K_FLOAT
            w_tag(w, f.tag)
            w_len(w, n * 4)
            ensure(w, n * 4)
            b = w.b
            p = w.pos
            @inbounds for v in arr
                x = UInt32(_ubits(v) & 0xFFFFFFFF)
                b[p] = UInt8(x & 0xFF)
                b[p + 1] = UInt8((x >> 8) & 0xFF)
                b[p + 2] = UInt8((x >> 16) & 0xFF)
                b[p + 3] = UInt8((x >> 24) & 0xFF)
                p += 4
            end
            w.pos = p
            return nothing
        end
        if kind == K_FIXED64 || kind == K_SFIXED64 || kind == K_DOUBLE
            w_tag(w, f.tag)
            w_len(w, n * 8)
            ensure(w, n * 8)
            for v in arr
                w_fixed64(w, v)
            end
            return nothing
        end
        # Varint kinds: write the payload over a 5-byte placeholder, then shift
        # it down: one memmove beats a size pre-pass over thousands of elements.
        w_tag(w, f.tag)
        lp = w.pos
        ensure(w, n * 10 + 5)
        w.pos += 5
        for v in arr
            write_packed_var(w, kind, v)
        end
        length_ = w.pos - lp - 5
        llen = size_var32(length_)
        b = w.b
        if llen != 5
            copyto!(b, lp + llen, b, lp + 5, length_)
        end
        p = lp
        x = length_
        @inbounds while x > 0x7F
            b[p] = UInt8((x & 0x7F) | 0x80)
            p += 1
            x >>= 7
        end
        @inbounds b[p] = UInt8(x)
        w.pos = lp + llen + length_
        return nothing
    end
    for v in arr
        w_tag(w, f.tag)
        write_num_single(w, kind, v)
    end
    return nothing
end

function write_packed_var(w::Writer, kind::Int, v)
    if kind == K_INT32 || kind == K_ENUM
        v >= 0 ? w_var32(w, v) : w_var64(w, v)
    elseif kind == K_UINT32
        w_var32(w, v)
    elseif kind == K_SINT32
        w_var32(w, zig32(v))
    elseif kind == K_INT64 || kind == K_UINT64
        w_var64(w, v)
    elseif kind == K_SINT64
        w_var64(w, zig64(v))
    else
        throw(CoreError("not packable"))
    end
    return nothing
end

function write_num_single(w::Writer, kind::Int, v)
    if kind == K_INT32 || kind == K_ENUM
        v >= 0 ? w_var32(w, v) : w_var64(w, v)
    elseif kind == K_UINT32
        w_var32(w, v)
    elseif kind == K_SINT32
        w_var32(w, zig32(v))
    elseif kind == K_INT64 || kind == K_UINT64
        w_var64(w, v)
    elseif kind == K_SINT64
        w_var64(w, zig64(v))
    elseif kind == K_BOOL
        ensure(w, 1)
        w.b[w.pos] = v ? 0x01 : 0x00
        w.pos += 1
    elseif kind == K_FIXED32 || kind == K_SFIXED32 || kind == K_FLOAT
        w_fixed32(w, v)
    else # K_FIXED64, K_SFIXED64, K_DOUBLE
        w_fixed64(w, v)
    end
    return nothing
end

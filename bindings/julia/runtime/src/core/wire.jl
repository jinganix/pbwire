# Protobuf wire primitives for the pure-Julia core.
#
# Varint/tag/little-endian read and write, zigzag, skipping (with group
# frames) and the structural UTF-8 check. One home for everything the
# descriptor reader, the encoder and the decoder do to bytes, so they cannot
# diverge. The rules are the C core's (`src/wire.c`, `src/decode.c`): strict
# tags and lengths, unknown fields skippable by shape, groups validated to
# their matching end tag, one nesting cap for groups and messages alike.
#
# Julia details this file owns:
#
#   * `UInt64` is the widest integer, and every reader masks to the width it
#     serves: a 32-bit varint returns its low 32 bits, a 64-bit one its low 64.
#     Nothing here returns a value wider than the field can hold.
#   * A `Reader` is a cursor over a `Vector{UInt8}` the caller owns; nothing
#     here copies input bytes and `unsafe_wrap` is never used, so decoded
#     `Raw` slices keep the buffer alive through an ordinary reference.
#   * float/double values travel as their raw bit patterns, so a NaN payload
#     survives a decode/encode round trip.

const WT_VARINT = 0
const WT_FIXED64 = 1
const WT_LEN = 2
const WT_START_GROUP = 3
const WT_END_GROUP = 4
const WT_FIXED32 = 5

# Nesting cap, the same bound as the C core's PBW_MAX_DEPTH.
const MAX_DEPTH = 256

const MASK32 = UInt32(0xFFFFFFFF)
const MASK64 = UInt64(0xFFFFFFFFFFFFFFFF)

"""A pbwire failure: the C core's `pbw_status` where one exists, else -1."""
struct CoreError <: Exception
    msg::String
    status::Int
end

CoreError(msg::AbstractString) = CoreError(String(msg), -1)

Base.showerror(io::IO, e::CoreError) = print(io, e.msg)

"""Growable output buffer, reset per encode (the C core's `pbw_ctx` twin)."""
mutable struct Writer
    b::Vector{UInt8}
    pos::Int # 1-based index of the next byte to write
end

Writer() = Writer(Vector{UInt8}(undef, 256), 1)

@inline function reset!(w::Writer)
    w.pos = 1
    return w
end

@inline function ensure(w::Writer, extra::Int)
    need = w.pos - 1 + extra
    if need > length(w.b)
        size = length(w.b) * 2
        while size < need
            size *= 2
        end
        resize!(w.b, size)
    end
    return nothing
end

"""Cursor over an input slice. `pos` is 1-based, `stop` the inclusive last
index; `cap` is the nesting cap for this decode."""
mutable struct Reader
    b::Vector{UInt8}
    pos::Int
    stop::Int
    cap::Int
end

Reader(b::Vector{UInt8}, pos::Int = 1, stop::Int = length(b)) =
    Reader(b, pos, stop, MAX_DEPTH)

# ----------------------------------------------------------------- writing

@inline function tag_bytes(fnum::Integer, wt::Integer)
    tag = fnum * 8 + wt
    out = Vector{UInt8}(undef, 5)
    n = 0
    while tag > 0x7F
        out[n + 1] = UInt8((tag & 0x7F) | 0x80)
        n += 1
        tag >>= 7
    end
    out[n + 1] = UInt8(tag)
    resize!(out, n + 1)
    return out
end

@inline function put_tag!(w::Writer, tag::Vector{UInt8})
    p = w.pos
    @inbounds for c in tag
        w.b[p] = c
        p += 1
    end
    w.pos = p
    return nothing
end

@inline function w_tag(w::Writer, tag::Vector{UInt8})
    ensure(w, length(tag))
    put_tag!(w, tag)
end

"""varint32 write of an unsigned 32-bit value."""
@inline function w_var32(w::Writer, v)
    ensure(w, 5)
    b = w.b
    p = w.pos
    x = _ubits(v) & 0xFFFFFFFF
    @inbounds while x > 0x7F
        b[p] = UInt8((x & 0x7F) | 0x80)
        p += 1
        x >>= 7
    end
    @inbounds b[p] = UInt8(x)
    w.pos = p + 1
    return nothing
end

"""varint64 write of an unsigned 64-bit value."""
@inline function w_var64(w::Writer, v)
    ensure(w, 10)
    b = w.b
    p = w.pos
    x = _ubits(v)
    @inbounds while x > 0x7F
        b[p] = UInt8((x & 0x7F) | 0x80)
        p += 1
        x >>= 7
    end
    @inbounds b[p] = UInt8(x)
    w.pos = p + 1
    return nothing
end

@inline function w_fixed32(w::Writer, v)
    ensure(w, 4)
    b = w.b
    p = w.pos
    x = UInt32(_ubits(v) & 0xFFFFFFFF)
    @inbounds begin
        b[p] = UInt8(x & 0xFF)
        b[p + 1] = UInt8((x >> 8) & 0xFF)
        b[p + 2] = UInt8((x >> 16) & 0xFF)
        b[p + 3] = UInt8((x >> 24) & 0xFF)
    end
    w.pos = p + 4
    return nothing
end

@inline function w_fixed64(w::Writer, v)
    ensure(w, 8)
    b = w.b
    p = w.pos
    x = _ubits(v)
    @inbounds for i in 0:7
        b[p + i] = UInt8((x >> (i * 8)) & 0xFF)
    end
    w.pos = p + 8
    return nothing
end

# Fused tag + value: one capacity check per field.

@inline function w_tag_var32(w::Writer, tag::Vector{UInt8}, v)
    ensure(w, length(tag) + 5)
    put_tag!(w, tag)
    b = w.b
    p = w.pos
    x = _ubits(v) & 0xFFFFFFFF
    @inbounds while x > 0x7F
        b[p] = UInt8((x & 0x7F) | 0x80)
        p += 1
        x >>= 7
    end
    @inbounds b[p] = UInt8(x)
    w.pos = p + 1
    return nothing
end

@inline function w_tag_var64(w::Writer, tag::Vector{UInt8}, v)
    ensure(w, length(tag) + 10)
    put_tag!(w, tag)
    b = w.b
    p = w.pos
    x = _ubits(v)
    @inbounds while x > 0x7F
        b[p] = UInt8((x & 0x7F) | 0x80)
        p += 1
        x >>= 7
    end
    @inbounds b[p] = UInt8(x)
    w.pos = p + 1
    return nothing
end

@inline function w_tag_fixed32(w::Writer, tag::Vector{UInt8}, v)
    ensure(w, length(tag) + 4)
    put_tag!(w, tag)
    b = w.b
    p = w.pos
    x = UInt32(_ubits(v) & 0xFFFFFFFF)
    @inbounds begin
        b[p] = UInt8(x & 0xFF)
        b[p + 1] = UInt8((x >> 8) & 0xFF)
        b[p + 2] = UInt8((x >> 16) & 0xFF)
        b[p + 3] = UInt8((x >> 24) & 0xFF)
    end
    w.pos = p + 4
    return nothing
end

@inline function w_tag_fixed64(w::Writer, tag::Vector{UInt8}, v)
    ensure(w, length(tag) + 8)
    put_tag!(w, tag)
    b = w.b
    p = w.pos
    x = _ubits(v)
    @inbounds for i in 0:7
        b[p + i] = UInt8((x >> (i * 8)) & 0xFF)
    end
    w.pos = p + 8
    return nothing
end

@inline function w_len(w::Writer, n::Int)
    ensure(w, 5)
    b = w.b
    p = w.pos
    x = n
    @inbounds while x > 0x7F
        b[p] = UInt8((x & 0x7F) | 0x80)
        p += 1
        x >>= 7
    end
    @inbounds b[p] = UInt8(x)
    w.pos = p + 1
    return nothing
end

"""Length-delimited payload under `tag` (string/bytes/message)."""
@inline function w_bytes(w::Writer, tag::Vector{UInt8}, v)
    n = length(v)
    ensure(w, length(tag) + 5 + n)
    put_tag!(w, tag)
    b = w.b
    p = w.pos
    x = n
    @inbounds while x > 0x7F
        b[p] = UInt8((x & 0x7F) | 0x80)
        p += 1
        x >>= 7
    end
    @inbounds b[p] = UInt8(x)
    w.pos = p + 1 + n
    copyto!(b, p + 1, v, 1, n)
    return nothing
end

"""Fills a one-byte length placeholder written at `lp` (1-based)."""
function patch_len(w::Writer, lp::Int)
    length_ = w.pos - lp - 1
    if length_ < 128
        w.b[lp] = UInt8(length_)
        return nothing
    end
    n = size_var32(length_)
    # Shift the body up by (n - 1) bytes; `copyto!` is a memmove.
    w.pos += n - 1
    ensure(w, 0)
    copyto!(w.b, lp + n, w.b, lp + 1, length_)
    b = w.b
    x = length_
    p = lp
    @inbounds while x > 0x7F
        b[p] = UInt8((x & 0x7F) | 0x80)
        p += 1
        x >>= 7
    end
    @inbounds b[p] = UInt8(x)
    return nothing
end

@inline function size_var32(v::Integer)
    n = 1
    x = v < 0 ? (_ubits(v) & 0xFFFFFFFF) : UInt64(v)
    while x > 0x7F
        x >>= 7
        n += 1
    end
    return n
end

@inline function size_var64(v::Integer)
    n = 1
    x = _ubits(v)
    while x > 0x7F
        x >>= 7
        n += 1
    end
    return n
end

"""int32: negatives sign-extend to 10 bytes, same as libprotobuf."""
@inline size_int32(v::Integer) = v >= 0 ? size_var32(v) : 10

@inline zig32(v)::UInt32 =
    reinterpret(UInt32, (Int32(v) << 1) ⊻ (Int32(v) >> 31))
@inline zig64(v)::UInt64 =
    reinterpret(UInt64, (Int64(v) << 1) ⊻ (Int64(v) >> 63))
@inline unzig32(v::UInt32)::Int64 =
    Int64(reinterpret(Int32, v >> 1) ⊻ -Int32(v & 0x1))
@inline unzig64(v::UInt64)::Int64 =
    reinterpret(Int64, v >> 1) ⊻ -Int64(v & 0x1)

# ---------------------------------------------------------------- reading

"""varint, low 32 bits (unsigned). Validates the encoding's length only."""
@inline function r_var(r::Reader)::UInt32
    b = r.b
    stop = r.stop
    p = r.pos
    lo = UInt32(0)
    j = 0
    while true
        p > stop && throw(CoreError("truncated varint"))
        c = @inbounds b[p]
        p += 1
        if j < 5
            lo |= UInt32(c & 0x7F) << (7 * j)
        end
        if (c & 0x80) == 0
            r.pos = p
            return lo
        end
        j += 1
        j >= 10 && throw(CoreError("varint overflow"))
    end
end

"""varint, full 64 bits (unsigned)."""
@inline function r_var64(r::Reader)::UInt64
    b = r.b
    stop = r.stop
    p = r.pos
    v = UInt64(0)
    shift = 0
    while true
        p > stop && throw(CoreError("truncated varint"))
        c = @inbounds b[p]
        p += 1
        v |= UInt64(c & 0x7F) << shift
        if (c & 0x80) == 0
            r.pos = p
            return v
        end
        shift += 7
        shift >= 64 && throw(CoreError("varint overflow"))
    end
end

"""varint, reduced to its bool value: any set payload bit means true."""
@inline function r_var_bool(r::Reader)::Bool
    b = r.b
    stop = r.stop
    p = r.pos
    nz = 0
    j = 0
    while true
        p > stop && throw(CoreError("truncated varint"))
        c = @inbounds b[p]
        p += 1
        nz |= c & 0x7F
        if (c & 0x80) == 0
            r.pos = p
            return nz != 0
        end
        j += 1
        j >= 10 && throw(CoreError("varint overflow"))
    end
end

"""Strict length read, like the core's: at most five bytes, the fifth only
carries four bits; an out-of-range length is rejected, not truncated."""
@inline function r_len32(r::Reader)::Int
    b = r.b
    stop = r.stop
    p = r.pos
    p > stop && throw(CoreError("truncated length"))
    c = @inbounds b[p]
    p += 1
    v = Int(c & 0x7F)
    shift = 7
    while (c & 0x80) != 0
        (shift >= 35 || p > stop) && throw(CoreError("bad length"))
        c = @inbounds b[p]
        p += 1
        if shift == 28 && (c & 0x7F) > 0x0F
            throw(CoreError("length overflow"))
        end
        v |= Int(c & 0x7F) << shift
        shift += 7
    end
    if v > 0 && p - 1 + v > stop
        throw(CoreError("truncated length-delimited"))
    end
    r.pos = p
    return v
end

"""Loose length read for descriptor parsing: any varint up to 2^31-1,
bounds checked."""
function r_len(r::Reader)::Int
    v = r_var64(r)
    v > 0x7FFFFFFF && throw(CoreError("length overflow"))
    if v > 0 && r.pos - 1 + Int(v) > r.stop
        throw(CoreError("truncated length-delimited"))
    end
    return Int(v)
end

"""Strict tag read: a varint32 of at most five bytes. A 5-byte tag above
UINT32_MAX truncates instead of failing, exactly what the reference parser
does (the official suite expects two such cases to parse)."""
@inline function r_tag(r::Reader)::UInt32
    b = r.b
    stop = r.stop
    p = r.pos
    p > stop && throw(CoreError("truncated tag"))
    c = @inbounds b[p]
    p += 1
    v = UInt64(c & 0x7F)
    shift = 7
    while (c & 0x80) != 0
        (shift >= 35 || p > stop) && throw(CoreError("bad tag"))
        c = @inbounds b[p]
        p += 1
        v |= UInt64(c & 0x7F) << shift
        shift += 7
    end
    r.pos = p
    return UInt32(v & 0xFFFFFFFF)
end

@inline function r_fixed32(r::Reader)::UInt32
    p = r.pos
    p + 3 > r.stop && throw(CoreError("truncated fixed32"))
    b = r.b
    r.pos = p + 4
    @inbounds return UInt32(b[p]) | (UInt32(b[p + 1]) << 8) |
                    (UInt32(b[p + 2]) << 16) | (UInt32(b[p + 3]) << 24)
end

"""Raw 64 bits, unsigned."""
@inline function r_fixed64(r::Reader)::UInt64
    p = r.pos
    p + 7 > r.stop && throw(CoreError("truncated fixed64"))
    b = r.b
    r.pos = p + 8
    @inbounds lo = UInt64(b[p]) | (UInt64(b[p + 1]) << 8) |
                       (UInt64(b[p + 2]) << 16) | (UInt64(b[p + 3]) << 24)
    @inbounds hi = UInt64(b[p + 4]) | (UInt64(b[p + 5]) << 8) |
                       (UInt64(b[p + 6]) << 16) | (UInt64(b[p + 7]) << 24)
    return lo | (hi << 32)
end

"""Skips one field value of a non-group wire type."""
function skip(r::Reader, wt::Integer)
    if wt == WT_VARINT
        r_var_bool(r)
    elseif wt == WT_FIXED64
        r.pos + 7 > r.stop && throw(CoreError("truncated fixed64"))
        r.pos += 8
    elseif wt == WT_LEN
        n = r_len32(r)
        r.pos + n - 1 > r.stop && throw(CoreError("truncated length-delimited"))
        r.pos += n
    elseif wt == WT_FIXED32
        r.pos + 3 > r.stop && throw(CoreError("truncated fixed32"))
        r.pos += 4
    else
        throw(CoreError("bad wire type $wt"))
    end
    return nothing
end

"""Validates a group body up to its matching end tag, like the core's group
skipper: every tag is strict, field 0 and wire types above 5 are rejected,
and an end tag only closes the group it names."""
function skip_group(r::Reader, number::Int, depth::Int)
    depth > r.cap && throw(CoreError("nesting exceeds $(r.cap)", 4))
    while true
        tag = r_tag(r)
        fnum = Int(tag >> 3)
        wt = Int(tag & 0x7)
        if fnum == 0 || wt > WT_FIXED32
            throw(CoreError("bad tag $tag"))
        end
        if wt == WT_END_GROUP
            fnum == number ||
                throw(CoreError("mismatched end-group for $fnum, want $number"))
            return nothing
        end
        if wt == WT_START_GROUP
            skip_group(r, fnum, depth + 1)
        else
            skip(r, wt)
        end
    end
end

"""Skips one unknown field: groups validate to their matching end tag,
anything else only checks value shape."""
function skip_unknown(r::Reader, fnum::Int, wt::Int, depth::Int)
    if wt == WT_START_GROUP
        skip_group(r, fnum, depth + 1)
        return nothing
    end
    skip(r, wt)
end

"""Structural UTF-8 check: rejects overlong forms, surrogates and values past
U+10FFFF, like the core's `pbw_utf8_valid` and the reference parsers."""
function utf8_valid(b::Vector{UInt8}, off::Int, len::Int)
    i = off
    last = off + len - 1
    while i <= last
        c = @inbounds b[i]
        if c < 0x80
            i += 1
        elseif (c & 0xE0) == 0xC0
            if c < 0xC2 || i + 1 > last || (@inbounds b[i + 1] & 0xC0) != 0x80
                return false
            end
            i += 2
        elseif (c & 0xF0) == 0xE0
            if i + 2 > last || (@inbounds b[i + 1] & 0xC0) != 0x80 ||
               (@inbounds b[i + 2] & 0xC0) != 0x80
                return false
            end
            v = (Int(c & 15) << 12) | (Int(b[i + 1] & 63) << 6) |
                Int(b[i + 2] & 63)
            if v < 0x800 || (0xD800 <= v <= 0xDFFF)
                return false
            end
            i += 3
        elseif (c & 0xF8) == 0xF0
            if c > 0xF4 || i + 3 > last || (@inbounds b[i + 1] & 0xC0) != 0x80 ||
               (@inbounds b[i + 2] & 0xC0) != 0x80 ||
               (@inbounds b[i + 3] & 0xC0) != 0x80
                return false
            end
            v = (Int(c & 7) << 18) | (Int(b[i + 1] & 63) << 12) |
                (Int(b[i + 2] & 63) << 6) | Int(b[i + 3] & 63)
            if v < 0x10000 || v > 0x10FFFF
                return false
            end
            i += 4
        else
            return false
        end
    end
    return true
end

"""Loose UTF-8 decode (proto2 strings): every malformed unit becomes U+FFFD,
like the reference's permissive mapping. Valid input is copied verbatim."""
function utf8_lossy(b::Vector{UInt8}, off::Int, len::Int)
    last = off + len - 1
    i = off
    out = IOBuffer()
    masks = (0x00, 0x1F, 0x0F, 0x07)
    mins = (0x00, 0x80, 0x800, 0x10000)
    while i <= last
        c = @inbounds b[i]
        if c < 0x80
            write(out, c)
            i += 1
            continue
        end
        n = (c & 0xE0) == 0xC0 ? 1 :
            (c & 0xF0) == 0xE0 ? 2 :
            (c & 0xF8) == 0xF0 ? 3 : 0
        ok = n > 0 && i + n <= last
        if ok
            @inbounds for k in 1:n
                if (b[i + k] & 0xC0) != 0x80
                    ok = false
                    break
                end
            end
        end
        v = 0
        if ok
            v = Int(c & masks[n + 1])
            @inbounds for k in 1:n
                v = (v << 6) | Int(b[i + k] & 0x3F)
            end
            ok = v >= mins[n + 1] && v <= 0x10FFFF &&
                 !(0xD800 <= v <= 0xDFFF)
        end
        if ok
            write(out, @view b[i:(i + n)])
            i += n + 1
        else
            write(out, "\ufffd")
            i += 1
        end
    end
    return String(take!(out))
end

"""A `String` as UTF-8 bytes. Julia strings may hold invalid UTF-8 (a lone
surrogate built through unsafe means); those bytes are copied verbatim."""
utf8_encode(s::AbstractString) = Vector{UInt8}(codeunits(String(s)))

# ------------------------------------------------- float/double bit patterns

@inline bits_float(bits::UInt32)::Float32 = reinterpret(Float32, bits)
@inline bits_double(bits::UInt64)::Float64 = reinterpret(Float64, bits)
@inline float_bits(v::Float32)::UInt32 = reinterpret(UInt32, v)
@inline double_bits(v::Float64)::UInt64 = reinterpret(UInt64, v)
@inline float_bits(v::Real)::UInt32 = float_bits(Float32(v))
@inline double_bits(v::Real)::UInt64 = double_bits(Float64(v))

# Unsigned reinterpretation of any integer width (Julia throws on UInt64(-1)).
@inline function _ubits(v::Integer)::UInt64
    if v isa UInt64
        return v
    elseif v isa UInt
        return UInt64(v)
    elseif v < 0
        return reinterpret(UInt64, Int64(v))
    else
        return UInt64(v)
    end
end

@inline signed32(v::UInt32)::Int64 = Int64(reinterpret(Int32, v))
@inline signed64(v::UInt64)::Int64 = reinterpret(Int64, v)

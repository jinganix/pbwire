# The one payload every Julia pbench arm times, plus the native layout image,
# the tree handle and the ProtoBuf.jl oracle. Values are
# bindings/c/tests/data/matrix3.txt; the repeats scale the three repeated
# fields so the four tiers land on the standard wire sizes.

push!(LOAD_PATH, joinpath(@__DIR__, "..", "runtime", "src"))
using Pbwire

const _GEN = isfile(joinpath(@__DIR__, "gen", "matrix3_pbwire.jl")) ?
    joinpath(@__DIR__, "gen", "matrix3_pbwire.jl") :
    joinpath(@__DIR__, "..", "tests", "gen", "matrix3_pbwire.jl")
module G end
Base.include(G, _GEN)
const Scalars3Layout = G.Scalars3Layout
const scalars3LayoutRows = G.scalars3LayoutRows
const Inner3Layout = G.Inner3Layout
const inner3LayoutRows = G.inner3LayoutRows

struct Tier
    label::String
    repeats::Int
    bytes::Int
    iterations::Int
end

const tiers = [
    Tier("small", 0, 136, 50000),
    Tier("medium", 1, 174, 50000),
    Tier("large", 100, 2469, 5000),
    Tier("xlarge", 3000, 74913, 500),
]

const DESC = joinpath(@__DIR__, "..", "..", "c", "tests", "data", "matrix3.desc")

_nul(s::AbstractString) = (b = Vector{UInt8}(codeunits(s)); push!(b, 0x00); b)

struct LayoutPayload
    root::Scalars3Layout
    plan::LayoutPlan{Scalars3Layout}
    keep::Vector{Any}
end

const _REGISTERED = IdDict{Any,Bool}()
const _ROOT_TYPES = IdDict{Any,LayoutType}()

"""Builds a native layout image with every string/array/sub-image in `keep`."""
function layout_payload(ls::LayoutSchema, repeats::Int)
    # A child layout registers before its parent; one compiled type per schema,
    # then a fresh image/arena for each tier.
    if !haskey(_REGISTERED, ls)
        layout_type(ls, "structtest.Inner3", Inner3Layout, inner3LayoutRows)
        _REGISTERED[ls] = true
    end
    lt = get(_ROOT_TYPES, ls, nothing)
    if lt === nothing
        lt = layout_type(ls, "structtest.Scalars3", Scalars3Layout,
            scalars3LayoutRows)
        _ROOT_TYPES[ls] = lt
    end
    plan = LayoutPlan{Scalars3Layout}(lt, Scalars3Layout(),
        Vector{UInt8}(undef, 1 << 21))
    root = Scalars3Layout()
    keep = Any[]
    kp(x) = (push!(keep, x); x)

    root.i32 = Int32(-2147483648)
    root.i64 = Int64(9223372036854775807)
    root.u32 = UInt32(0xffffffff)
    root.u64 = typemax(UInt64)
    root.s32 = Int32(-2147483648)
    root.s64 = typemin(Int64)
    root.fx32 = UInt32(0xdeadbeef)
    root.fx64 = UInt64(72623859790382856)
    root.sfx32 = Int32(-2147483648)
    root.sfx64 = typemin(Int64)
    root.f32 = Float32(Inf)
    root.f64 = -123456.789
    root.flag = true

    root.str = pointer(kp(_nul("hello")))
    root.color = Int32(2)

    inner = kp(Ref{Inner3Layout}(Inner3Layout()))
    inner[].name = pointer(kp(_nul("kid")))
    inner[].value = Int32(42)
    root.inner = pointer_from_objref(inner[])

    root.nums = pointer(kp(Int32[i - 3 for i in 0:(repeats - 1)]))
    root.numsCount = Csize_t(repeats)

    tags = [kp(_nul("tag")) for _ in 1:repeats]
    tagptrs = kp(Ptr{UInt8}[pointer(t) for t in tags])
    root.tags = pointer(tagptrs)
    root.tagsCount = Csize_t(repeats)

    inraw = kp(Vector{UInt8}(undef, repeats * sizeof(Inner3Layout)))
    ip = Ptr{Inner3Layout}(pointer(inraw))
    for i in 0:(repeats - 1)
        child = Inner3Layout()
        child.name = pointer(kp(_nul("grandkid")))
        child.value = Int32(i + 1)
        unsafe_store!(ip + i * sizeof(Inner3Layout), child)
    end
    root.inners = ip
    root.innersCount = Csize_t(repeats)

    root.opt_i32 = Int32(-7)
    root.has_opt_i32 = Int32(1)
    root.opt_str = pointer(kp(_nul("opt")))

    root.flags = pointer(kp(Bool[i % 2 == 0 for i in 0:(repeats - 1)]))
    root.flagsCount = Csize_t(repeats)

    return LayoutPayload(root, plan, keep)
end

"""Reads every field of a decoded layout image; the gate compares it with the
tree arm and the official decoder. Returns true when all match."""
function layout_reads(back::Scalars3Layout, repeats::Int)
    ok = back.i32 == Int32(-2147483648) &&
        back.i64 == Int64(9223372036854775807) &&
        back.u32 == UInt32(0xffffffff) &&
        back.u64 == typemax(UInt64) &&
        back.s32 == Int32(-2147483648) &&
        back.s64 == typemin(Int64) &&
        back.fx32 == UInt32(0xdeadbeef) &&
        back.fx64 == UInt64(72623859790382856) &&
        back.sfx32 == Int32(-2147483648) &&
        back.sfx64 == typemin(Int64) &&
        back.f32 == Float32(Inf) &&
        back.f64 == -123456.789 &&
        back.flag &&
        unsafe_string(back.str) == "hello" &&
        back.color == Int32(2) &&
        back.numsCount == repeats &&
        back.tagsCount == repeats &&
        back.innersCount == repeats &&
        back.flagsCount == repeats &&
        back.has_opt_i32 == 1 && back.opt_i32 == Int32(-7) &&
        unsafe_string(back.opt_str) == "opt"
    if !ok
        return false
    end
    inner = unsafe_load(back.inner)
    ok &= unsafe_string(inner.name) == "kid" && inner.value == Int32(42)
    for i in 0:(repeats - 1)
        ok &= unsafe_load(back.nums, i + 1) == Int32(i - 3) &&
            unsafe_string(unsafe_load(back.tags, i + 1)) == "tag" &&
            unsafe_load(back.flags, i + 1) == (i % 2 == 0)
        sub = unsafe_load(back.inners + i * sizeof(Inner3Layout))
        ok &= unsafe_string(sub.name) == "grandkid" &&
            sub.value == Int32(i + 1)
    end
    return ok
end

# Optional official baseline: ProtoBuf.jl generated code. Included in a named
# module so its PB encoder/decoder and message types stay separate.
const ORACLE_OLD = joinpath(@__DIR__, "..", "benchmarks", "oracle", "matrix3_pb.jl")
const ORACLE_NEW = joinpath(@__DIR__, "..", "benchmarks", "oracle", "structtest",
    "matrix3_pb.jl")
const HAS_ORACLE = isfile(ORACLE_OLD) || isfile(ORACLE_NEW)

module Oracle
using ProtoBuf
const _PB = ProtoBuf
end

if HAS_ORACLE
    Base.include(Oracle, isfile(ORACLE_NEW) ? ORACLE_NEW : ORACLE_OLD)
end

function baseline_message(repeats::Int)
    o = Oracle
    return o.Scalars3(Int32(-2147483648), Int64(9223372036854775807),
        UInt32(0xffffffff), typemax(UInt64), Int32(-2147483648),
        typemin(Int64), UInt32(0xdeadbeef), UInt64(72623859790382856),
        Int32(-2147483648), typemin(Int64), Float32(Inf), -123456.789, true,
        "hello", o.Color3.COLOR3_GREEN, o.Inner3("kid", Int32(42)),
        Int32[i - 3 for i in 0:(repeats - 1)], fill("tag", repeats),
        [o.Inner3("grandkid", Int32(i + 1)) for i in 0:(repeats - 1)],
        Int32(-7), "opt", Bool[i % 2 == 0 for i in 0:(repeats - 1)])
end

function baseline_encode(m)
    io = IOBuffer()
    Oracle.PB.encode(Oracle.PB.ProtoEncoder(io), m)
    return take!(io)
end

function baseline_decode(bytes::Vector{UInt8})
    return Oracle.PB.decode(Oracle.PB.ProtoDecoder(IOBuffer(bytes)),
        Oracle.Scalars3)
end

function baseline_reads(m, repeats::Int)
    return m.i32 == Int32(-2147483648) &&
        m.i64 == Int64(9223372036854775807) &&
        m.u32 == UInt32(0xffffffff) &&
        m.u64 == typemax(UInt64) &&
        m.s32 == Int32(-2147483648) &&
        m.s64 == typemin(Int64) &&
        m.fx32 == UInt32(0xdeadbeef) &&
        m.fx64 == UInt64(72623859790382856) &&
        m.sfx32 == Int32(-2147483648) &&
        m.sfx64 == typemin(Int64) &&
        m.f32 == Float32(Inf) && m.f64 == -123456.789 && m.flag &&
        m.str == "hello" && m.color == Oracle.Color3.COLOR3_GREEN &&
        m.inner !== nothing && m.inner.name == "kid" &&
        m.inner.value == Int32(42) && length(m.nums) == repeats &&
        length(m.tags) == repeats && length(m.inners) == repeats &&
        length(m.flags) == repeats && m.opt_i32 == Int32(-7) &&
        m.opt_str == "opt" &&
        (repeats == 0 || (m.nums[end] == Int32(repeats - 4) &&
            m.tags[end] == "tag" && m.flags[end] == ((repeats - 1) % 2 == 0) &&
            m.inners[end].value == Int32(repeats)))
end

# Tree arm: build the shipped dynamic Message with one set per field, then
# encode; decode and read every field back. This is the whole user task, not
# the prebuilt-handle op `make julia-bench` prints as `pbwire-julia-tree`.
function fill_tree!(ty::MsgType, repeats::Int)
    m = new_message(ty)
    set_int!(m, "i32", -2147483648)
    set_int!(m, "i64", 9223372036854775807)
    set_int!(m, "u32", 4294967295)
    set_int!(m, "u64", -1)
    set_int!(m, "s32", -2147483648)
    set_int!(m, "s64", -9223372036854775808)
    set_int!(m, "fx32", 0xdeadbeef)
    set_int!(m, "fx64", 72623859790382856)
    set_int!(m, "sfx32", -2147483648)
    set_int!(m, "sfx64", -9223372036854775808)
    set_float!(m, "f32", Inf)
    set_double!(m, "f64", -123456.789)
    set_bool!(m, "flag", true)
    set_string!(m, "str", "hello")
    set_int!(m, "color", 2)
    set_int!(m, "opt_i32", -7)
    set_string!(m, "opt_str", "opt")
    kid = set_message!(m, "inner")
    set_string!(kid, "name", "kid")
    set_int!(kid, "value", 42)
    for i in 0:(repeats - 1)
        set_int!(m, "nums", i - 3)
        set_string!(m, "tags", "tag")
        set_bool!(m, "flags", i % 2 == 0)
        sub = set_message!(m, "inners")
        set_string!(sub, "name", "grandkid")
        set_int!(sub, "value", i + 1)
    end
    return m
end

function tree_reads(m::Message, repeats::Int)
    return get_int(m, "i32") == -2147483648 &&
        get_int(m, "i64") == 9223372036854775807 &&
        get_int(m, "u32") == 4294967295 && get_int(m, "u64") == -1 &&
        get_int(m, "s32") == -2147483648 &&
        get_int(m, "s64") == -9223372036854775808 &&
        get_int(m, "fx32") == 3735928559 &&
        get_int(m, "fx64") == 72623859790382856 &&
        get_int(m, "sfx32") == -2147483648 &&
        get_int(m, "sfx64") == -9223372036854775808 &&
        get_float(m, "f32") == Inf && get_double(m, "f64") == -123456.789 &&
        get_bool(m, "flag") && get_string(m, "str") == "hello" &&
        get_int(m, "color") == 2 && has(m, "opt_i32") &&
        get_int(m, "opt_i32") == -7 && has(m, "opt_str") &&
        get_string(m, "opt_str") == "opt" &&
        Pbwire.count(m, "nums") == repeats &&
        Pbwire.count(m, "tags") == repeats &&
        Pbwire.count(m, "inners") == repeats &&
        Pbwire.count(m, "flags") == repeats &&
        (repeats == 0 || (as_int(at(m, "nums", repeats)) == repeats - 4 &&
            as_string(at(m, "tags", repeats)) == "tag" &&
            as_bool(at(m, "flags", repeats)) == ((repeats - 1) % 2 == 0) &&
            get_int(as_message(at(m, "inners", repeats)), "value") == repeats)) &&
        get_string(get_message(m, "inner"), "name") == "kid" &&
        get_int(get_message(m, "inner"), "value") == 42
end

# Shared bootstrap for the pure-core tests: load `PbwireCore` (once) and the
# fixture directory the C binding already pins.
#
# Plain functions and an explicit check counter -- no test framework -- so the
# same files run standalone (`julia --project=runtime tests/core/<file>.jl`) and
# through `tests/runtests.jl`.

if !isdefined(@__MODULE__, :PbwireCore)
    include(joinpath(@__DIR__, "..", "..", "runtime", "src", "PbwireCore.jl"))
    using .PbwireCore
end

const CORE_DATA = abspath(joinpath(@__DIR__, "..", "..", "..", "c", "tests",
    "data"))

"""The status of the CoreError `f` throws, or `nothing` when it does not."""
function status_of(f)
    try
        f()
        return nothing
    catch e
        return e isa CoreError ? e.status : -99
    end
end

"True when `f` throws any CoreError."
throws_core_error(f) = status_of(f) !== nothing

function fixture(name::AbstractString, data::AbstractString = CORE_DATA)
    return read(joinpath(data, name))
end

"""One message type from one fixture descriptor file."""
function type_of(file::AbstractString, name::AbstractString,
        data::AbstractString = CORE_DATA)
    return msgtype(from_file(joinpath(data, file)), name)
end

# ------------------------------------------------- synthetic descriptors

"""A hand-built FileDescriptorSet for a self-nesting proto3 message:
`syntax = "proto3"; package deep; message Node { Node kid = 1; }`."""
function deep_type()
    varint(v::Int) = begin
        out = UInt8[]
        while v > 0x7f
            push!(out, UInt8((v % 128) | 0x80))
            v ÷= 128
        end
        push!(out, UInt8(v))
        out
    end
    ld(fnum::Int, payload::Vector{UInt8}) = UInt8[fnum * 8 + 2;
        varint(length(payload)); payload]
    vint(fnum::Int, v::Int) = UInt8[fnum * 8; varint(v)]
    units(s::AbstractString) = Vector{UInt8}(codeunits(s))

    field = UInt8[ld(1, units("kid"))...; vint(3, 1)...; vint(4, 1)...;
        vint(5, 11)...; ld(6, units(".deep.Node"))...]
    message = UInt8[ld(1, units("Node"))...; ld(2, field)...]
    file = UInt8[ld(1, units("deep.proto"))...; ld(2, units("deep"))...;
        ld(4, message)...; ld(12, units("proto3"))...]
    return msgtype(from_bytes(ld(1, file)), "deep.Node")
end

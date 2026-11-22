# The `tree` arm: the shipped dynamic Message handle. The whole user task is
# timed -- one `set` per field while building, `encode()`; decode, then one
# `get` per field while reading -- so it is not the prebuilt-handle op
# `make julia-bench` prints as `pbwire-julia-tree`.

include(joinpath(@__DIR__, "payload.jl"))

function tree_encode(ty::MsgType, repeats::Int)
    m = fill_tree!(ty, repeats)
    try
        return Pbwire.encode(m)
    finally
        close(m)
    end
end

function tree_decode(ty::MsgType, wire::Vector{UInt8}, repeats::Int)
    m = Pbwire.decode(ty, wire)
    try
        tree_reads(m, repeats) || return nothing
        return Pbwire.encode(m)
    finally
        close(m)
    end
end

# The `shim` arm: the generated Scalars3Layout image is the native object;
# `encode_layout` / `decode_layout` cross once and the C reader
# (bindings/julia/native/pbw_julia_shim.c) walks that memory in place.

include(joinpath(@__DIR__, "payload.jl"))

shim_encode(p::LayoutPayload) =
    encode_layout(Scalars3Layout, p.root, p.plan)

shim_decode(wire::Vector{UInt8}, p::LayoutPayload) =
    decode_layout(wire, Scalars3Layout, p.plan)

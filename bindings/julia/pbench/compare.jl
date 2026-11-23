# Interleaved timing for the Julia arms: per round `tree`, `shim`, `core` and
# the ProtoBuf.jl baseline run back to back on the same payload; per tier the
# median of five rounds is printed. The gate runs first in spirit: every
# round re-encodes with all arms and refuses a row whose bytes differ.
#
#   julia --project=runtime pbench/compare.jl

include(joinpath(@__DIR__, "a-tree.jl"))
include(joinpath(@__DIR__, "b-shim.jl"))
include(joinpath(@__DIR__, "core_arm.jl"))
using .CoreArm

const ROUNDS = 5

function measure(iterations::Int, op)
    best = Inf
    for _ in 1:ROUNDS
        t0 = time_ns()
        for _ in 1:iterations
            op()
        end
        best = min(best, (time_ns() - t0) / iterations)
    end
    return best
end

function row(tier, arm, bytes, enc, dec)
    println(rpad(tier, 7), " ", rpad(arm, 14),
        lpad(round(Int, enc), 8), " ", lpad(round(bytes / enc * 1000.0; digits = 1), 8),
        " ", lpad(round(Int, dec), 8), " ", lpad(round(bytes / dec * 1000.0; digits = 1), 8))
end

println("tier    arm              encode ns      MB/s  decode ns      MB/s")
schema = from_file(DESC)
ty = msgtype(schema, "structtest.Scalars3")
ls = layout_schema(schema)

for tier in tiers
    payload = layout_payload(ls, tier.repeats)
    ref = baseline_message(tier.repeats)
    wire = baseline_encode(ref)
    if length(wire) != tier.bytes
        error("$(tier.label): $(length(wire)) bytes, want $(tier.bytes)")
    end
    core_obj = core_object(tier.repeats)
    if shim_encode(payload) != wire || tree_encode(ty, tier.repeats) != wire ||
       core_marshal(core_obj) != wire
        error("$(tier.label): an arm disagrees with the official bytes")
    end
    tree_enc = measure(tier.iterations, () -> tree_encode(ty, tier.repeats))
    shim_enc = measure(tier.iterations, () -> shim_encode(payload))
    core_enc = measure(tier.iterations, () -> core_marshal(core_obj))
    official_enc = measure(tier.iterations, () -> baseline_encode(ref))
    tree_dec = measure(tier.iterations, () -> tree_decode(ty, wire, tier.repeats))
    shim_dec = measure(tier.iterations, () -> shim_decode(wire, payload))
    core_dec = measure(tier.iterations, () -> core_unmarshal(wire))
    official_dec = measure(tier.iterations, () -> baseline_decode(wire))
    row(tier.label, "tree", length(wire), tree_enc, tree_dec)
    row(tier.label, "shim", length(wire), shim_enc, shim_dec)
    row(tier.label, "core", length(wire), core_enc, core_dec)
    row(tier.label, "protobuf-jl", length(wire), official_enc, official_dec)
    if !layout_reads(shim_decode(wire, payload), tier.repeats)
        error("$(tier.label): shim decode did not expose the fields")
    end
    if core_unmarshal(wire) != core_obj
        error("$(tier.label): core decode did not expose the fields")
    end
end

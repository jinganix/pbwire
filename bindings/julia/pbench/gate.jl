# The pbench gate: every arm must produce the official ProtoBuf.jl bytes on
# all four tiers, and every arm's decode must expose the fields payload.jl
# lists, before any number is printed.
#
#   julia --project=runtime pbench/gate.jl

include(joinpath(@__DIR__, "a-tree.jl"))
include(joinpath(@__DIR__, "b-shim.jl"))
include(joinpath(@__DIR__, "core_arm.jl"))
using .CoreArm

HAS_ORACLE || error("no ProtoBuf.jl oracle; run pbench/setup.sh")

checks = 0
failures = 0
function check(ok, what)
    global checks += 1
    if !ok
        global failures += 1
        println("FAIL ", what)
    end
end

schema = from_file(DESC)
ty = msgtype(schema, "structtest.Scalars3")
ls = layout_schema(schema)

for tier in tiers
    ref = baseline_message(tier.repeats)
    wire = baseline_encode(ref)
    check(length(wire) == tier.bytes, "$(tier.label): $(length(wire)) bytes")
    tree_wire = tree_encode(ty, tier.repeats)
    check(tree_wire == wire, "$(tier.label): tree bytes differ")
    check(tree_decode(ty, wire, tier.repeats) == wire,
        "$(tier.label): tree decode/re-encode differs")

    payload = layout_payload(ls, tier.repeats)
    shim_wire = shim_encode(payload)
    check(shim_wire == wire, "$(tier.label): shim bytes differ")
    back = shim_decode(wire, payload)
    check(layout_reads(back, tier.repeats), "$(tier.label): shim fields differ")
    again = shim_encode(payload)
    check(again == wire, "$(tier.label): shim decode/re-encode differs")

    core_obj = core_object(tier.repeats)
    check(core_marshal(core_obj) == wire, "$(tier.label): core bytes differ")
    check(core_roundtrip(wire) == wire,
        "$(tier.label): core decode/re-encode differs")
    check(core_unmarshal(wire) == core_obj, "$(tier.label): core fields differ")

    official = baseline_decode(wire)
    check(baseline_reads(official, tier.repeats),
        "$(tier.label): official fields differ")
    println("OK   $(tier.label): $(length(wire)) bytes, tree/shim/core equal, x$(tier.repeats)")
end

if failures != 0
    error("gate: $failures of $checks checks failed")
end
println("gate: PASS ($checks checks)")

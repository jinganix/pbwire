# Julia binding benchmark on the same matrix3.proto and the four standard tiers

using Printf
# from BENCHMARKS.md: every scalar kind, the nested message, the two proto3
# optionals, and the repeated fields scaled x0 / x1 / x100 / x3000, which lands
# the tiers on 136 / 174 / 2469 / 74913 bytes.
#
# The rows are the published mechanisms: `pbwire-julia-tree` is the prebuilt
# dynamic Message handle (plain encode/decode plus the reusable Ctx
# workspace), `pbwire-julia-shim` is the generated flat layout image read and
# written in place by the C reader (one ccall per op), and
# `pbwire-julia-core` is the pure-Julia core (plan 19): a plain Dict in,
# encode_dict out; wire bytes in, fully materialized decode_dict out. All
# sides must produce identical bytes before anything is timed.
#
# The baseline is JuliaIO/ProtoBuf.jl with generated code
# (`benchmarks/oracle/.../matrix3_pb.jl`, produced by `protojl("matrix3.proto",
# "<proto dir>", "benchmarks/oracle")`); when it is absent the script times
# pbwire only and says so.
#
#   julia --project=runtime benchmarks/bench.jl [matrix3.desc]

include(joinpath(@__DIR__, "..", "pbench", "b-shim.jl"))
include(joinpath(@__DIR__, "..", "pbench", "core_arm.jl"))
using .CoreArm

const FLOORS = (50000, 50000, 5000, 500)
const MIN_PASS_SECONDS = 0.25
const PASSES = 3

function calibrate(op, floor::Int)
    n = max(floor, 1)
    while true
        t0 = time_ns()
        for _ in 1:n
            op()
        end
        if (time_ns() - t0) / 1e9 >= MIN_PASS_SECONDS || n >= 100_000_000
            return n
        end
        n *= 2
    end
end

function measure(floor::Int, op)
    n = calibrate(op, floor)
    for _ in 1:n # warm up
        op()
    end
    best = Inf
    for _ in 1:PASSES
        t0 = time_ns()
        for _ in 1:n
            op()
        end
        best = min(best, (time_ns() - t0) / n)
    end
    return best
end

function row(impl::String, op::String, bytes::Int, ns::Float64)
    @printf("  %-18s %-7s %12.1f %10.1f\n", impl, op, ns, bytes / ns * 1000.0)
end

function main(args::Vector{String})
    desc = isempty(args) ? DESC : args[1]
    schema = from_file(desc)
    ty = msgtype(schema, "structtest.Scalars3")
    ls = layout_schema(schema)

    if !HAS_ORACLE
        println("note: no ProtoBuf.jl baseline under benchmarks/oracle;")
        println("      timing pbwire only. Generate it with pbench/setup.sh or protojl.")
    end

    failures = 0
    for (tier, floor) in zip(tiers, FLOORS)
        label, n, want = tier.label, tier.repeats, tier.bytes
        tmp = fill_tree!(ty, n)
        got = Pbwire.encode(tmp)
        close(tmp)
        if length(got) != want
            println("FAIL $label: pbwire wrote $(length(got)) bytes, expected $want")
            failures += 1
            continue
        end
        back = Pbwire.decode(ty, got)
        ok = tree_reads(back, n)
        close(back)
        if !ok
            println("FAIL $label: pbwire decode differs")
            failures += 1
            continue
        end

        payload = layout_payload(ls, n)
        shim_got = shim_encode(payload)
        if shim_got != got
            println("FAIL $label: shim bytes differ")
            failures += 1
            continue
        end
        if !layout_reads(shim_decode(got, payload), n)
            println("FAIL $label: shim decode differs")
            failures += 1
            continue
        end
        core_obj = core_object(n)
        if core_marshal(core_obj) != got || core_roundtrip(got) != got ||
           core_unmarshal(got) != core_obj
            println("FAIL $label: core bytes or fields differ")
            failures += 1
            continue
        end
        println("OK   $label encoding parity, x$n ($want bytes)")

        probe = fill_tree!(ty, n)
        tier_n = calibrate(() -> Pbwire.encode(probe), floor)
        enc = measure(floor, () -> Pbwire.encode(probe))
        dec = measure(floor, () -> let m = Pbwire.decode(ty, got)
            close(m)
        end)
        # The workspace path: the same decode into a reusable context, which
        # drops the per-message arena allocation. Both tree rows are the
        # binding's own API.
        ctx = Ctx()
        dec_ctx = measure(floor, () -> decode_in(ctx, ty, got))
        close(probe)

        shim_enc = measure(floor, () -> shim_encode(payload))
        shim_dec = measure(floor, () -> shim_decode(got, payload))
        core_enc = measure(floor, () -> core_marshal(core_obj))
        core_dec = measure(floor, () -> core_unmarshal(got))

        println("$label: $want bytes, $tier_n iterations (repeat field x$n)")
        println("  $(rpad("impl", 18)) $(rpad("op", 7)) $(lpad("ns/op", 12)) $(lpad("MB/s", 10))")
        row("pbwire-julia-tree", "encode", want, enc)
        row("pbwire-julia-tree", "decode", want, dec)
        row("pbwire-julia-tree", "dec+ctx", want, dec_ctx)
        row("pbwire-julia-shim", "encode", want, shim_enc)
        row("pbwire-julia-shim", "decode", want, shim_dec)
        row("pbwire-julia-core", "encode", want, core_enc)
        row("pbwire-julia-core", "decode", want, core_dec)
        if HAS_ORACLE
            ref = baseline_message(n)
            ref_bytes = baseline_encode(ref)
            if ref_bytes != got
                println("FAIL $label: baseline differs from pbwire bytes")
                failures += 1
                println()
                continue
            end
            benc = measure(floor, () -> baseline_encode(ref))
            bdec = measure(floor, () -> baseline_decode(got))
            row("protobuf-jl", "encode", want, benc)
            row("protobuf-jl", "decode", want, bdec)
        end
        println()
    end
    close(schema)
    if failures != 0
        error("$failures check(s) failed")
    end
    return nothing
end

main(ARGS)

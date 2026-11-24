# Julia binding benchmark on the same matrix3.proto and the four standard tiers

using Printf
# from BENCHMARKS.md: every scalar kind, the nested message, the two proto3
# optionals, and the repeated fields scaled x0 / x1 / x100 / x3000, which lands
# the tiers on 136 / 174 / 2469 / 74913 bytes.
#
# The rows are the published mechanisms: `pbwire-julia-tree` is the Go-parity
# full task (a fresh dynamic Message per iteration through the shipped
# per-field API: one set per field, encode, close; decode, one get per field
# folded into a live sink, close), `pbwire-julia-proxy` is the old prebuilt
# shape (a held handle's encode, bare decode-and-close, and the reusable Ctx
# workspace decode, relabelled not re-measured, never the headline),
# `pbwire-julia-shim` is the generated flat layout image read and written in
# place by the C reader (one ccall per op), and `pbwire-julia-core` is the
# pure-Julia core (plan 19): a plain Dict in, encode_dict out; wire bytes in,
# fully materialized decode_dict out. All sides must produce identical bytes
# before anything is timed.
#
# Fresh-task headline (disclosed): `shim` encode builds a fresh layout image
# per iteration inside the timer, like `tree` rebuilds its Message; the
# baseline encode builds a fresh ProtoBuf.jl message per iteration. `core`
# keeps the steady-state shape (one-downcall op on the held Dict) beside the
# fresh rows; `proxy`/`dec+ctx` stay prebuilt, never the headline.
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

# Folds every gate-checked field (same list `tree_reads` asserts) so the tree
# decode timer pays the read, not just the parse. Returns an Int the caller
# adds to a live sink.
function consume_tree!(m::Message, repeats::Int)
    acc = Int64(0)
    acc += get_int(m, "i32")
    acc += get_int(m, "i64")
    acc += get_int(m, "u32")
    acc += get_int(m, "u64")
    acc += get_int(m, "s32")
    acc += get_int(m, "s64")
    acc += get_int(m, "fx32")
    acc += get_int(m, "fx64")
    acc += get_int(m, "sfx32")
    acc += get_int(m, "sfx64")
    acc += get_float(m, "f32") > 0 ? Int64(1) : Int64(0)
    acc += trunc(Int64, get_double(m, "f64"))
    acc += get_bool(m, "flag") ? Int64(1) : Int64(0)
    acc += Int64(length(get_string(m, "str")))
    acc += get_int(m, "color")
    acc += has(m, "opt_i32") ? get_int(m, "opt_i32") : Int64(0)
    acc += has(m, "opt_str") ? Int64(length(get_string(m, "opt_str"))) : Int64(0)
    inner = get_message(m, "inner")
    acc += Int64(length(get_string(inner, "name")))
    acc += get_int(inner, "value")
    acc += Int64(Pbwire.count(m, "nums"))
    acc += Int64(Pbwire.count(m, "tags"))
    acc += Int64(Pbwire.count(m, "inners"))
    acc += Int64(Pbwire.count(m, "flags"))
    if repeats > 0
        acc += as_int(at(m, "nums", repeats))
        acc += Int64(length(as_string(at(m, "tags", repeats))))
        acc += as_bool(at(m, "flags", repeats)) ? Int64(1) : Int64(0)
        acc += get_int(as_message(at(m, "inners", repeats)), "value")
    end
    return acc
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
        # The tree full task, gated before it is timed: a second fresh
        # native build encodes to the same bytes, a fresh decode exposes
        # every gate-checked field, and the decoded tree re-encodes
        # byte-exactly. The prebuilt rows below are the proxy arm.
        fresh = fill_tree!(ty, n)
        fresh_got = Pbwire.encode(fresh)
        close(fresh)
        if fresh_got != got
            println("FAIL $label: pbwire-julia-tree fresh-build bytes differ")
            failures += 1
            continue
        end
        back = Pbwire.decode(ty, got)
        ok = tree_reads(back, n)
        again = ok ? Pbwire.encode(back) : UInt8[]
        close(back)
        if !ok
            println("FAIL $label: pbwire-julia-tree field compare differs")
            failures += 1
            continue
        end
        if again != got
            println("FAIL $label: pbwire-julia-tree re-encode differs")
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
        # Floor calibration stays on the prebuilt op so the slower tree op
        # never inflates any row's iteration count.
        tier_n = calibrate(() -> Pbwire.encode(probe), floor)
        sink = Ref{Int64}(0)
        enc = measure(floor, () -> begin
            built = fill_tree!(ty, n)
            try
                sink[] += Int64(length(Pbwire.encode(built)))
            finally
                close(built)
            end
        end)
        dec = measure(floor, () -> begin
            m = Pbwire.decode(ty, got)
            try
                sink[] += consume_tree!(m, n)
            finally
                close(m)
            end
        end)
        # The proxy arm: the old prebuilt shape (a held handle's encode,
        # bare decode-and-close with no materialization), relabelled not
        # re-measured as a new task, never the headline.
        proxy_enc = measure(floor, () -> Pbwire.encode(probe))
        proxy_dec = measure(floor, () -> let m = Pbwire.decode(ty, got)
            close(m)
        end)
        # The workspace path: the same prebuilt decode into a reusable
        # context, which drops the per-message arena allocation. A proxy-row
        # variant of the bare decode, kept beside it.
        ctx = Ctx()
        dec_ctx = measure(floor, () -> decode_in(ctx, ty, got))
        close(probe)
        sink[] == typemax(Int64) && error("unreachable sink guard")

        shim_enc = measure(floor, () -> shim_encode(layout_payload(ls, n)))
        shim_dec = measure(floor, () -> shim_decode(got, payload))
        core_enc = measure(floor, () -> core_marshal(core_obj))
        core_dec = measure(floor, () -> core_unmarshal(got))

        println("$label: $want bytes, $tier_n iterations (repeat field x$n)")
        println("  $(rpad("impl", 18)) $(rpad("op", 7)) $(lpad("ns/op", 12)) $(lpad("MB/s", 10))")
        row("pbwire-julia-tree", "encode", want, enc)
        row("pbwire-julia-tree", "decode", want, dec)
        row("pbwire-julia-proxy", "encode", want, proxy_enc)
        row("pbwire-julia-proxy", "decode", want, proxy_dec)
        row("pbwire-julia-proxy", "dec+ctx", want, dec_ctx)
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
            benc = measure(floor, () -> baseline_encode(baseline_message(n)))
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

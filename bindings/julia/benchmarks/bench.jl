# Julia binding benchmark on the same matrix3.proto and the four standard tiers
# from BENCHMARKS.md: every scalar kind, the nested message, the two proto3
# optionals, and the repeated fields scaled x0 / x1 / x100 / x3000, which lands
# the tiers on 136 / 174 / 2469 / 74913 bytes.
#
# Both sides must produce identical bytes before anything is timed. The
# baseline is JuliaIO/ProtoBuf.jl with generated code (`oracle/matrix3_pb.jl`,
# produced by `protojl("matrix3.proto", "<proto dir>", "benchmarks/oracle")`);
# when it is absent the script times pbwire only and says so.
#
#   julia --project=runtime benchmarks/bench.jl [matrix3.desc]

push!(LOAD_PATH, joinpath(@__DIR__, "..", "runtime"))
using Pbwire
using Printf

const CASES = ("small", "medium", "large", "xlarge")
const REPEATS = (0, 1, 100, 3000)
const WANT = (136, 174, 2469, 74913)
const FLOORS = (50000, 50000, 5000, 500)
const MIN_PASS_SECONDS = 0.25
const PASSES = 3

function fill_pbwire!(ty::MsgType, n::Int)
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
    for i in 0:(n - 1)
        set_int!(m, "nums", i - 3)
        set_string!(m, "tags", "tag")
        set_bool!(m, "flags", i % 2 == 0)
        inner = set_message!(m, "inners")
        set_string!(inner, "name", "grandkid")
        set_int!(inner, "value", i + 1)
    end
    return m
end

function fill_baseline(n::Int)
    Main.Scalars3(Int32(-2147483648), 9223372036854775807, UInt32(4294967295),
        typemax(UInt64), Int32(-2147483648), typemin(Int64),
        UInt32(0xdeadbeef), UInt64(72623859790382856),
        typemin(Int32), typemin(Int64), Inf32, -123456.789, true, "hello",
        Main.Color3.COLOR3_GREEN, Main.Inner3("kid", Int32(42)),
        Int32[i - 3 for i in 0:(n - 1)], fill("tag", n),
        [Main.Inner3("grandkid", Int32(i + 1)) for i in 0:(n - 1)],
        Int32(-7), "opt", Bool[i % 2 == 0 for i in 0:(n - 1)])
end

function baseline_encode(m)
    io = IOBuffer()
    Main.PB.encode(Main.PB.ProtoEncoder(io), m)
    return take!(io)
end

function baseline_decode(::Type{T}, bytes::Vector{UInt8}) where {T}
    return Main.PB.decode(Main.PB.ProtoDecoder(IOBuffer(bytes)), T)
end

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
    @printf("  %-12s %-7s %12.1f %10.1f\n", impl, op, ns, bytes / ns * 1000.0)
end

# Optional baseline: generated ProtoBuf.jl code next to this script, included
# at top level so its methods are visible to everything below (including
# fill_baseline) without world-age issues.
const ORACLE_PATH = joinpath(@__DIR__, "oracle", "matrix3_pb.jl")
const HAS_BASELINE = let ok = false
    if isfile(ORACLE_PATH)
        try
            include(ORACLE_PATH)
            ok = true
        catch e
            @warn "baseline oracle present but not loadable; timing pbwire only" exception = e
        end
    end
    ok
end

function main(args::Vector{String})
    desc = isempty(args) ?
           joinpath(@__DIR__, "..", "..", "c", "tests", "data", "matrix3.desc") : args[1]
    schema = from_file(desc)
    ty = msgtype(schema, "structtest.Scalars3")

    # Optional baseline: generated ProtoBuf.jl code next to this script.
    baseline = HAS_BASELINE || nothing
    if baseline === nothing
        println("note: no ProtoBuf.jl baseline (benchmarks/oracle/matrix3_pb.jl absent);")
        println("      timing pbwire only. Generate it with protojl and")
        println("      rerun to compare against JuliaIO/ProtoBuf.jl.")
    end

    failures = 0
    for tier in 1:4
        label, n, want, floor = CASES[tier], REPEATS[tier], WANT[tier], FLOORS[tier]
        tmp = fill_pbwire!(ty, n)
        got = encode(tmp)
        close(tmp)
        if length(got) != want
            println("FAIL $label: pbwire wrote $(length(got)) bytes, expected $want")
            failures += 1
            continue
        end
        back = decode(ty, got)
        ok = encode(back) == got && get_string(back, "str") == "hello" &&
             Pbwire.count(back, "nums") == n &&
             (n == 0 || as_int(at(back, "nums", n)) == n - 4)
        close(back)
        if !ok
            println("FAIL $label: pbwire decode differs")
            failures += 1
            continue
        end
        println("OK   $label encoding parity, x$n ($want bytes)")

        probe = fill_pbwire!(ty, n)
        tier_n = calibrate(() -> encode(probe), floor)
        enc = measure(floor, () -> encode(probe))
        dec = measure(floor, () -> let m = decode(ty, got)
            close(m)
        end)
        close(probe)
        println("$label: $want bytes, $tier_n iterations (repeat field x$n)")
        println("  $(rpad("impl", 12)) $(rpad("op", 7)) $(lpad("ns/op", 12)) $(lpad("MB/s", 10))")
        row("pbwire-julia", "encode", want, enc)
        row("pbwire-julia", "decode", want, dec)
        if baseline !== nothing
            ref = fill_baseline(n)
            ref_bytes = baseline_encode(ref)
            if ref_bytes != got
                println("FAIL $label: baseline differs from pbwire bytes")
                failures += 1
                println()
                continue
            end
            benc = measure(floor, () -> baseline_encode(ref))
            bdec = measure(floor, () -> baseline_decode(Main.Scalars3, got))
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

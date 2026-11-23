# Self-check against the pinned protoc bytes: build structtest.Scalars3 from
# bindings/c/tests/data/matrix3.txt, encode, compare with matrix3.bin (the same
# oracle the C binding uses), then decode it back and check the values.
#
#   julia --project=runtime tests/runtests.jl

push!(LOAD_PATH, joinpath(@__DIR__, "..", "runtime"))
using Pbwire
using Test

const C_DATA = joinpath(@__DIR__, "..", "..", "c", "tests", "data")
const CORE_DATA = joinpath(@__DIR__, "..", "..", "..", "tests", "data")

function fill_scalars!(m::Message)
    Pbwire.set_int!(m, "i32", -2147483648)
    Pbwire.set_int!(m, "i64", 9223372036854775807)
    Pbwire.set_int!(m, "u32", 4294967295)
    Pbwire.set_int!(m, "u64", -1) # u64::MAX bits
    Pbwire.set_int!(m, "s32", -2147483648)
    Pbwire.set_int!(m, "s64", -9223372036854775808)
    Pbwire.set_int!(m, "fx32", 0xdeadbeef)
    Pbwire.set_int!(m, "fx64", 72623859790382856)
    Pbwire.set_int!(m, "sfx32", -2147483648)
    Pbwire.set_int!(m, "sfx64", -9223372036854775808)
    Pbwire.set_float!(m, "f32", Inf)
    Pbwire.set_double!(m, "f64", -123456.789)
    Pbwire.set_bool!(m, "flag", true)
    Pbwire.set_string!(m, "str", "hello")
    Pbwire.set_int!(m, "color", 2)
    Pbwire.set_int!(m, "opt_i32", -7)
    Pbwire.set_string!(m, "opt_str", "opt")
    for n in (1, -5, 300)
        Pbwire.set_int!(m, "nums", n)
    end
    for tag in ("a", "bb")
        Pbwire.set_string!(m, "tags", tag)
    end
    for (x, v) in (("x", 1), ("y", -2))
        inner = Pbwire.set_message!(m, "inners")
        Pbwire.set_string!(inner, "name", x)
        Pbwire.set_int!(inner, "value", v)
    end
    for flag in (true, false, true)
        Pbwire.set_bool!(m, "flags", flag)
    end
    kid = Pbwire.set_message!(m, "inner")
    Pbwire.set_string!(kid, "name", "kid")
    Pbwire.set_int!(kid, "value", 42)
    return m
end

@testset "pbwire julia" begin
    oracle = read(joinpath(C_DATA, "matrix3.bin"))
    schema = from_file(joinpath(C_DATA, "matrix3.desc"))
    ty = msgtype(schema, "structtest.Scalars3")
    inner_ty = msgtype(schema, "structtest.Inner3")
    name = field(inner_ty, "name")
    value = field(inner_ty, "value")

    msg = fill_scalars!(new_message(ty))
    wire = encode(msg)
    @test wire == oracle # encoded bytes match protoc
    close(msg)

    decoded = decode(ty, Vector{UInt8}(oracle))
    @test get_int(decoded, "i32") == -2147483648
    @test get_bool(decoded, "flag")
    @test get_string(decoded, "str") == "hello"
    @test get_double(decoded, "f64") == -123456.789
    @test get_int(decoded, "color") == 2
    @test get_int(decoded, "opt_i32") == -7
    @test has(decoded, "opt_str")
    let tmp = new_message(ty)
        @test !has(tmp, "i32")
        close(tmp)
    end
    @test Pbwire.count(decoded, "nums") == 3
    @test as_int(at(decoded, "nums", 2)) == -5
    @test as_string(at(decoded, "tags", 2)) == "bb"
    @test get_int(as_message(at(decoded, "inners", 2)), value) == -2
    @test as_bool(at(decoded, "flags", 2)) == false
    @test get_string(get_message(decoded, "inner"), name) == "kid"
    @test encode(decoded) == oracle # decode -> encode is byte-exact
    close(decoded)

    # A field the schema does not declare (99, varint 7) survives re-encoding.
    inner_only = decode(inner_ty, UInt8[0x98, 0x06, 0x07])
    @test encode(inner_only) == UInt8[0x98, 0x06, 0x07]
    close(inner_only)

    # Bad input is an error, not a crash.
    @test_throws PbwireError decode(ty, UInt8[0x80])
    @test_throws PbwireError msgtype(schema, "structtest.Nope")
    @test_throws PbwireError field(ty, "nope")

    # The cases a binding can get wrong even when the core is right: payloads
    # with embedded NULs / invalid UTF-8, non-ASCII text, a negative enum, an
    # empty repeated element, proto3 optional presence with an empty value,
    # and oneof switching. tests/data/edge.bin is protoc --encode.
    edge_oracle = read(joinpath(CORE_DATA, "edge.bin"))
    edge_schema = from_file(joinpath(CORE_DATA, "edge.desc"))
    edge_ty = msgtype(edge_schema, "edge.Edge")

    edge = new_message(edge_ty)
    set_bytes!(edge, "raw", UInt8[0x00, 0x01, 0xff])
    set_string!(edge, "text", "héllo ☃")
    set_int!(edge, "sign", -3)
    set_string!(edge, "maybe", "")
    set_int!(edge, "pick_num", 5) # oneof: replaced below
    set_string!(edge, "pick_str", "one")
    for tag in ("a", "", "b")
        set_string!(edge, "tags", tag)
    end
    set_bytes!(edge, "blobs", UInt8[0x00])
    set_bytes!(edge, "blobs", UInt8[])
    sub = set_message!(edge, "inner")
    set_string!(sub, "tag", "kid")
    set_int!(sub, "n", -1)
    @test encode(edge) == edge_oracle
    close(edge)

    back = decode(edge_ty, Vector{UInt8}(edge_oracle))
    @test get_bytes(back, "raw") == UInt8[0x00, 0x01, 0xff]
    @test get_string(back, "text") == "héllo ☃"
    @test get_int(back, "sign") == -3
    @test Pbwire.count(back, "tags") == 3
    @test as_string(at(back, "tags", 2)) == ""
    @test as_bytes(at(back, "blobs", 1)) == UInt8[0x00]
    @test as_bytes(at(back, "blobs", 2)) == UInt8[]
    @test get_int(get_message(back, "inner"), "n") == -1
    @test has(back, "maybe") && get_string(back, "maybe") == ""
    @test has(back, "pick_str") && !has(back, "pick_num")
    @test encode(back) == edge_oracle
    close(back)

    # The Dict path round-trips through the same bytes.
    d = decode_dict(ty, Vector{UInt8}(oracle))
    @test d["str"] == "hello"
    @test d["nums"] == Any[1, -5, 300]
    @test d["inners"][2]["value"] == -2
    @test encode_dict(ty, d) == oracle
    small = Dict{String,Any}("name" => "kid", "value" => 42)
    @test decode_dict(inner_ty, encode_dict(inner_ty, small)) == small

    # encode() sizes its buffer from the last wire length; a tree that grew past
    # that hint must fall back to the allocating writer, then use the new hint.
    let m = decode(edge_ty, Vector{UInt8}(edge_oracle))
        set_string!(m, "text", "y"^600)
        @test get_string(m, "text") == "y"^600
        first = encode(m)
        @test length(first) > 600
        @test encode(m) == first # the hint now fits: direct path, same bytes
        close(m)
    end

    # A default message is zero bytes both ways.
    let tmp = new_message(edge_ty)
        @test isempty(encode(tmp))
        close(tmp)
    end
    empty = decode(edge_ty, UInt8[])
    @test isempty(encode(empty))
    @test !has(empty, "text")
    close(empty)

    # A wrong value kind is refused, not written.
    let m = new_message(edge_ty)
        @test_throws PbwireError set_string!(m, "pick_num", "not a number")
        @test_throws PbwireError set_message!(m, "text")
        # An embedded NUL would be silently truncated by the C string
        # constructor, so the binding refuses it instead.
        @test_throws PbwireError set_string!(m, "text", "a\0b")
        close(m)
    end

    # encode_dict closes its tree even when the conversion throws (no finalizer
    # on Message by design, so a throw must not leak the native tree).
    @test_throws PbwireError encode_dict(ty, Dict("nope" => 1))

    # The extended surface: deterministic/delimited encode, JSON and text
    # print/parse, bounded decode, the visit scan, and service metadata.
    let m = new_message(ty)
        set_string!(m, "str", "hello")
        w = encode(m)
        @test encoded_size(m) == length(w)
        @test is_initialized(m)
        @test encode_deterministic(m) == w
        framed = encode_delimited(m)
        back, consumed = decode_delimited(ty, vcat(framed, framed))
        @test encode(back) == w
        @test consumed == length(framed)
        close(back)
        js = print_json(m)
        from_json = parse_json(ty, js)
        @test encode(from_json) == w
        close(from_json)
        @test_throws PbwireError parse_json(ty, "{\"nope\":1}")
        ignored = parse_json(ty, "{\"nope\":1}", true)
        @test encode(ignored) == UInt8[]
        close(ignored)
        kept = decode(ty, vcat(w, UInt8[0xf8, 0x06, 0x02])) # unknown field 111
        @test occursin("111: 2", print_text(kept, true))
        @test !occursin("111", print_text(kept, false))
        close(kept)
        tx = print_text(m)
        from_text = parse_text(ty, tx)
        @test encode(from_text) == w
        close(from_text)
        limited = decode_limits(ty, w)
        @test encode(limited) == w
        close(limited)
        @test_throws PbwireError decode_limits(ty, w, 1, 0)
        @test_throws PbwireError parse_json(ty, js, false, 1, 0)
        @test_throws PbwireError parse_text(ty, tx, 1, 0)
        @test parse_json(ty, js, false, length(js) + 1, 0) isa Message
        # A workspace decode: read-only tree, byte-identical re-encode, and the
        # next decode on the same context replaces it.
        ctx = Ctx()
        in_ctx = decode_in(ctx, ty, w)
        @test encode(in_ctx) == w
        @test_throws PbwireError set_int!(in_ctx, "i32", 1)
        @test encode(decode_in(ctx, ty, w)) == w
        seen = Ref(0)
        visit(ty, w, (f) -> begin
            seen[] += 1
            @test f.number != 0
            return true
        end)
        @test seen[] > 0
        @test service_count(schema) == 0
        @test_throws PbwireError resolve_method(schema, "no.Such", "Nope")
        close(m)
    end

    close(edge_schema)
    close(schema)
end

# The pure-Julia core (plan 19) is a second, independent codec: same wire, same
# official conformance suite, no ccall. Its checks are plain assertions, not
# `@test`, so they read the same standalone and here.
for file in ("descriptor_test.jl", "wire_test.jl", "codec_test.jl")
    include(joinpath(@__DIR__, "core", file))
end

@testset "pbwire julia core" begin
    for (file, run) in (("descriptor_test", descriptor_tests),
                        ("wire_test", wire_tests),
                        ("codec_test", codec_tests))
        checks, failures = run()
        @test failures == 0
        println("  $(file): $(checks - failures)/$(checks) checks")
    end
end

using Base.Threads # @threads / Atomic for the testset below

# Threading proof over the C-core path: 8 workers x 200 iters x three phases,
# mirroring the other bindings' shape:
#
#   (a) independent-message encode == want bytes,
#   (b) SHARED read-only message concurrent encode == want bytes,
#   (c) fresh-target decode of want with field checks (NOT re-encode),
#
# plus a cold-cache phase first: fresh types shared across workers behind a
# start gate, so every worker hits the field + nested-type caches in the same
# instant (without the gate startup stagger could serialize the first fill
# and the race window would be timing luck). The MsgType lock makes cold
# resolution safe; with the lock reverted this phase fails, so it is not a
# vacuous green.
#
# The schema, the warmed type, the want bytes and (for the cold phase) the
# fresh types are the only cross-thread state, all read-only after warmup.
# Every message a worker fills/encodes/decodes is worker-owned and closed;
# the shared message is encode-only (its `cap` hint is atomic, so concurrent
# encodes never race on the write-back). With `-t1` the same body runs as a
# serial loop with the same asserts. Speed prints only, never asserted.
@testset "concurrent" begin
    schema = Pbwire.from_file(joinpath(C_DATA, "matrix3.desc"))
    ty = Pbwire.msgtype(schema, "structtest.Scalars3")
    oracle = read(joinpath(C_DATA, "matrix3.bin"))

    # Serial oracle pin: one encode must equal the protoc bytes before any
    # worker starts, so a mapping bug fails here and not in a thread. This
    # also warms ty's field + nested-type caches, so the phases below share
    # only read-only state.
    probe = fill_scalars!(Pbwire.new_message(ty))
    want = Pbwire.encode(probe)
    @test want == oracle
    Pbwire.close(probe)

    # Shared read-only fixture for phase (b): built once, never mutated.
    shared = fill_scalars!(Pbwire.new_message(ty))

    problems = String[]
    problems_lock = ReentrantLock()
    record(w, phase) = lock(problems_lock) do
        push!(problems, "worker $w: $phase")
    end

    # Phase (c) field checks, the same values the main suite pins.
    function check_decoded(w, d)
        ok = Pbwire.get_int(d, "i32") == -2147483648 &&
            Pbwire.get_int(d, "u64") == -1 && # u64::MAX bits, kept as Int64
            isinf(Pbwire.get_float(d, "f32")) &&
            Pbwire.get_double(d, "f64") == -123456.789 &&
            Pbwire.get_bool(d, "flag") &&
            Pbwire.get_string(d, "str") == "hello" &&
            Pbwire.get_int(d, "color") == 2 &&
            Pbwire.get_int(Pbwire.get_message(d, "inner"), "value") == 42 &&
            Pbwire.get_string(Pbwire.get_message(d, "inner"), "name") == "kid" &&
            Pbwire.count(d, "nums") == 3 &&
            Pbwire.as_int(Pbwire.at(d, "nums", 2)) == -5 &&
            Pbwire.as_string(Pbwire.at(d, "tags", 2)) == "bb" &&
            Pbwire.get_int(Pbwire.as_message(Pbwire.at(d, "inners", 2)), "value") == -2 &&
            Pbwire.get_int(d, "opt_i32") == -7 &&
            Pbwire.get_string(d, "opt_str") == "opt" &&
            Pbwire.as_bool(Pbwire.at(d, "flags", 2)) == false
        ok || record(w, "phase (c) fresh-target decode field mismatch")
    end

    function run_worker(w)
        for _ in 1:200
            # (a) independent-message encode == want bytes.
            m = fill_scalars!(Pbwire.new_message(ty))
            try
                Pbwire.encode(m) == want ||
                    record(w, "phase (a) independent-encode != want bytes")
            catch e
                record(w, "phase (a) independent-encode threw $e")
            end
            Pbwire.close(m)
            # (b) SHARED read-only message concurrent encode == want bytes.
            try
                Pbwire.encode(shared) == want ||
                    record(w, "phase (b) shared-read-only-encode != want bytes")
            catch e
                record(w, "phase (b) shared-read-only-encode threw $e")
            end
            # (c) fresh-target decode with field checks, NOT re-encode.
            d = nothing
            try
                d = Pbwire.decode(ty, want)
                check_decoded(w, d)
            catch e
                record(w, "phase (c) fresh-target decode threw $e")
            end
            d !== nothing && Pbwire.close(d)
        end
    end

    # One serial worker first: the 1-thread reference for the timing print,
    # with the same asserts as every other worker.
    t_serial = @elapsed run_worker(0)
    println("  concurrent serial reference: 600 encodes+decodes in " *
        "$(round(t_serial, digits=3)) s ($(round(600 / t_serial)) ops/s)")

    # Cold-cache phase on fresh types: string setters resolve every field
    # name (warming `fields`), nested setters resolve the message types
    # (warming `nested`), all under contention behind the start gate.
    cold_ty = Pbwire.msgtype(schema, "structtest.Scalars3")
    nstart = min(8, nthreads())
    cold_body(w) = begin
        for _ in 1:200
            m = fill_scalars!(Pbwire.new_message(cold_ty))
            try
                Pbwire.encode(m) == want ||
                    record(w, "phase (cold) cold-resolve encode != want bytes")
            catch e
                record(w, "phase (cold) cold-resolve threw $e")
            end
            Pbwire.close(m)
        end
    end
    if nthreads() > 1
        gate = Atomic{Int}(0)
        t_cold = @elapsed @threads for w in 1:8
            atomic_add!(gate, 1)
            while gate[] < nstart
                yield()
            end
            cold_body(w)
        end
        println("  concurrent cold-cache: 1600 resolves+encodes in " *
            "$(round(t_cold, digits=3)) s ($(round(1600 / t_cold)) resolves/s)")
    else
        t_cold = @elapsed for w in 1:8
            cold_body(w)
        end
        println("  concurrent cold-cache (serial fallback, 1 thread): " *
            "1600 resolves+encodes in $(round(t_cold, digits=3)) s")
    end

    if nthreads() > 1
        t = @elapsed @threads for w in 1:8
            run_worker(w)
        end
        println("  concurrent: 4800 encodes+decodes in " *
            "$(round(t, digits=3)) s ($(round(4800 / t)) ops/s)")
    else
        t = @elapsed for w in 1:8
            run_worker(w)
        end
        println("  concurrent (serial fallback, 1 thread): " *
            "4800 encodes+decodes in $(round(t, digits=3)) s")
    end

    if !isempty(problems)
        println("  failures: " * join(Iterators.take(problems, 8), "; "))
    end
    @test isempty(problems)
    Pbwire.close(shared)
end

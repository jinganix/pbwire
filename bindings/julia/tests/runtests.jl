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
    set_int!(m, "i32", -2147483648)
    set_int!(m, "i64", 9223372036854775807)
    set_int!(m, "u32", 4294967295)
    set_int!(m, "u64", -1) # u64::MAX bits
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
    for n in (1, -5, 300)
        set_int!(m, "nums", n)
    end
    for tag in ("a", "bb")
        set_string!(m, "tags", tag)
    end
    for (x, v) in (("x", 1), ("y", -2))
        inner = set_message!(m, "inners")
        set_string!(inner, "name", x)
        set_int!(inner, "value", v)
    end
    for flag in (true, false, true)
        set_bool!(m, "flags", flag)
    end
    kid = set_message!(m, "inner")
    set_string!(kid, "name", "kid")
    set_int!(kid, "value", 42)
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

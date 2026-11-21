# Codec behaviour: byte equality against the fixtures the C tests pin,
# decode/encode round trips, unknown retention (unknown numbers and
# wrong-wire-type occurrences, input order), groups, packed runs, oneof
# last-wins, presence, Int64/UInt64 edges, proto3 UTF-8 validation, proto2
# required fields and the byte/depth limits.
#
#   julia --project=runtime tests/core/codec_test.jl [data dir]

include(joinpath(@__DIR__, "harness.jl"))

"""The matrix3 fixture as a plain dict; `n` scales the repeated fields."""
function matrix3_object(n::Int)
    obj = Dict{String,Any}(
        "i32" => Int64(-2147483648),
        "i64" => Int64(9223372036854775807),
        "u32" => UInt64(4294967295),
        "u64" => UInt64(18446744073709551615),
        "s32" => Int64(-2147483648),
        "s64" => Int64(-9223372036854775808),
        "fx32" => UInt64(3735928559),
        "fx64" => UInt64(72623859790382856),
        "sfx32" => Int64(-2147483648),
        "sfx64" => Int64(-9223372036854775808),
        "f32" => Float32(Inf),
        "f64" => -123456.789,
        "flag" => true,
        "str" => "hello",
        "color" => Int64(2),
        "inner" => Dict{String,Any}("name" => "kid", "value" => Int64(42)),
        "nums" => Int64[1, -5, 300][1:min(3, n)],
        "tags" => Any["a", "bb"][1:min(2, n)],
        "inners" => Any[
            Dict{String,Any}("name" => "x", "value" => Int64(1)),
            Dict{String,Any}("name" => "y", "value" => Int64(-2))][1:min(2, n)],
        "flags" => Bool[true, false, true][1:min(3, n)],
        "opt_i32" => Int64(-7),
        "opt_str" => "opt",
    )
    n == 0 && for key in ("nums", "tags", "inners", "flags")
        delete!(obj, key)
    end
    return obj
end

codec_tests(data::AbstractString = CORE_FIXTURES) = begin
    checks = 0
    failures = 0
    function check(ok::Bool, what::AbstractString)
        checks += 1
        if !ok
            failures += 1
            println("FAIL ", what)
        end
    end
    type_of_here(file, name) = type_of(file, name, data)
    fixture_here(name) = fixture(name, data)

    scalars = type_of_here("matrix3.desc", "structtest.Scalars3")
    bin = fixture_here("matrix3.bin")

    # ------------------------------------------------- object -> wire
    check(encode_dict(scalars, matrix3_object(3)) == bin,
        "matrix3: dict -> wire is byte-identical to protoc")

    # ----------------------------------------------------- decode
    m = decode(scalars, bin)
    check(get_int(m, "i32") == -2147483648, "decode i32")
    check(get_field(m, "u32") == UInt64(4294967295), "decode u32")
    check(get_field(m, "i64") == 9223372036854775807, "decode i64")
    check(get_field(m, "u64") == UInt64(18446744073709551615), "decode u64")
    check(get_field(m, "s64") == -9223372036854775808, "decode s64")
    check(get_field(m, "fx64") == UInt64(72623859790382856), "decode fx64")
    check(get_field(m, "sfx64") == -9223372036854775808, "decode sfx64")
    check(get_field(m, "f32") == Float32(Inf), "decode f32")
    check(get_field(m, "f64") == -123456.789, "decode f64")
    check(get_field(m, "flag") === true, "decode flag")
    check(get_string(m, "str") == "hello", "decode str")
    check(get_int(m, "color") == 2, "decode color")
    check(get_field(m, "nums") == Any[1, -5, 300], "decode nums")
    check(get_field(m, "tags") == Any["a", "bb"], "decode tags")
    check(get_field(m, "flags") == Any[true, false, true], "decode flags")
    check(get_int(m, "opt_i32") == -7, "decode opt_i32")
    check(get_string(m, "opt_str") == "opt", "decode opt_str")
    inner = get_message(m, "inner")
    check(get_string(inner, "name") == "kid", "decode inner.name")
    check(get_int(inner, "value") == 42, "decode inner.value")
    check(to_dict(m) == matrix3_object(3), "to_dict matches the input object")
    check(encode(m) == bin, "matrix3: decode/re-encode is byte-exact")

    # 64-bit edges stay exact through a round trip (no float, no Int32).
    for (name, value) in (("i64", Int64(9223372036854775807)),
                          ("s64", Int64(-9223372036854775808)),
                          ("u64", UInt64(18446744073709551615)),
                          ("fx64", UInt64(18235189155122922741)))
        one = new_message(scalars)
        set_int!(one, name, value)
        back = decode(scalars, encode(one))
        check(get_field(back, name) == value, "64-bit edge $name survives")
    end
    one = new_message(scalars)
    set_int!(one, "i64", Int64(2^53 + 1)) # not representable as a double
    check(get_field(decode(scalars, encode(one)), "i64") == 2^53 + 1,
        "2^53+1 stays exact")

    # ---------------------------------------------------------- proto2
    proto2 = type_of_here("matrix2.desc", "structtest.Scalars2")
    bin2 = fixture_here("matrix2.bin")
    m2 = decode(proto2, bin2)
    check(get_int(m2, "id") == 99, "proto2 id")
    check(get_field(m2, "s64") == -1, "proto2 s64")
    check(get_int(m2, "shade") == -2, "proto2 shade")
    check(get_field(m2, "nums") == Any[4, 5], "proto2 unpacked nums")
    check(get_field(m2, "packed_nums") == Any[6, 7, 8], "proto2 packed run")
    check(get_string(get_message(m2, "inner"), "name") == "req",
        "proto2 inner.name")
    check(encode(m2) == bin2, "proto2 decode/re-encode is byte-exact")
    # Dropping the first field (`id`, required) fails with status 6.
    check(status_of(() -> decode(proto2, bin2[3:end])) == 6,
        "a missing required field fails with status 6")

    # ---------------------------------------------------------- groups
    groups = type_of_here("grouptest.desc", "structtest.GroupTest")
    gbin = fixture_here("grouptest.bin")
    gm = decode(groups, gbin)
    ginner = get_message(gm, "inner")
    check(ginner.def.full_name == "structtest.GroupTest.Inner", "group type")
    check(get_int(ginner, "x") == 5, "group inner.x")
    check(get_string(ginner, "s") == "g", "group inner.s")
    items = get_field(gm, "item")
    check([get_int(item, "v") for item in items] == [1, -2], "group items")
    check(get_int(gm, "id") == 9, "group id")
    check(encode(gm) == gbin, "groups decode/re-encode byte-exactly")
    check(status_of(() -> decode(groups, gbin[1:(end - 1)])) !== nothing,
        "a group that never closes fails")

    # ---------------------------------------------------------- oneof
    oneofs = type_of_here("oneoftest.desc", "structtest.OneOfTest")
    obin = fixture_here("oneoftest.bin")
    om = decode(oneofs, obin)
    check(oneof_case(om, "kind") == 2, "oneof case follows the last member")
    check(get_string(om, "text") == "hi", "oneof text")
    check(!has(om, "num"), "oneof num stays clear")
    check(encode(om) == obin, "oneof decode/re-encode byte-exactly")
    # num then text: the text member replaces the number, not merges.
    both = UInt8[0x08, 0x03, 0x12, 0x02, 0x68, 0x69, 0x28, 0x01]
    last = decode(oneofs, both)
    check(oneof_case(last, "kind") == 2, "last oneof member wins")
    check(!has(last, "num"), "the cleared member is gone")
    check(encode(last) == UInt8[0x12, 0x02, 0x68, 0x69, 0x28, 0x01],
        "only the set oneof member is emitted")

    # -------------------------------------------------------- unknowns
    unknown_a = UInt8[0x98, 0x06, 0x05]             # field 99, varint 5
    unknown_b = UInt8[0xa2, 0x06, 0x02, 0x7a, 0x7a] # field 100, "zz"
    wire = UInt8[unknown_a; bin; unknown_b]
    um = decode(scalars, wire)
    check(get_string(um, "str") == "hello", "a known field survives an unknown")
    check(encode(um) == UInt8[bin; unknown_a; unknown_b],
        "unknowns are retained after the known fields, in input order")
    only = decode(scalars, UInt8[unknown_a; unknown_b])
    check(encode(only) == UInt8[unknown_a; unknown_b],
        "a message of pure unknowns re-encodes to itself")

    # -------------------------------------------------- wrong wire type
    wrong = UInt8[0x0a, 0x01, 0x00] # i32 (varint) sent length-delimited
    wm = decode(scalars, wrong)
    check(!has(wm, "i32"), "a wrong wire type leaves the field absent")
    check(encode(wm) == wrong, "a wrong wire type is retained verbatim")
    group_wire = UInt8[0x0b, 0x0c] # field 1, empty group
    check(encode(decode(scalars, group_wire)) == group_wire,
        "an unknown group is validated and retained")

    # ------------------------------------------------------ packed forms
    unpacked = UInt8[0x88, 0x01, 0x01, # nums +1
        0x88, 0x01, 0xfb, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01]
    pm = decode(scalars, unpacked)
    check(get_field(pm, "nums") == Any[1, -5], "unpacked runs are accepted")
    check(encode(pm) == UInt8[0x8a, 0x01, 0x0b, 0x01,
        0xfb, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01],
        "a decoded unpacked run re-encodes packed")
    check(status_of(() -> decode(scalars, UInt8[0x8a, 0x01, 0x02, 0x01])) !==
          nothing, "a short packed run fails")
    empty = decode(scalars, UInt8[0x8a, 0x01, 0x00])
    check(!has(empty, "nums"), "an empty packed run stays absent")
    check(isempty(encode(empty)), "an empty packed run encodes to nothing")

    # ------------------------------------------------------ implicit zero
    # i32 with the varint 2^32 truncates to int32 0 and is dropped.
    zero = decode(scalars, UInt8[0x08, 0x80, 0x80, 0x80, 0x80, 0x10])
    check(!has(zero, "i32"), "a wide varint truncates to an implicit zero")
    check(isempty(encode(zero)), "the implicit zero is not emitted")

    # ---------------------------------------------------------- utf8
    check(status_of(() -> decode(scalars, UInt8[0x72, 0x01, 0xff])) !== nothing,
        "proto3 strings must be valid UTF-8")
    id = UInt8[0x08, 0x01]
    str = UInt8[0x3a, 0x02, 0xff, 0xfe] # matrix2 str is proto2
    pm2 = decode(proto2, UInt8[id; str])
    check(encode(pm2) == UInt8[id; str],
        "proto2 strings are passed through byte for byte")
    check(occursin("\ufffd", get_string(pm2, "str")),
        "proto2 strings materialize loosely")

    # --------------------------------------------------------- limits
    check(status_of(() -> decode(scalars, bin;
        limits = Limits(10, 0))) == 5, "the byte cap fails with status 5")
    check(encode(decode(scalars, bin;
        limits = Limits(length(bin), 0))) == bin,
        "a cap at the exact size still decodes")
    # Eight levels of the synthetic self-nesting message.
    body = UInt8[]
    for _ in 1:8
        body = UInt8[0x0a; UInt8(length(body)); body]
    end
    deep = deep_type()
    check(status_of(() -> decode(deep, body; limits = Limits(0, 4))) !== nothing,
        "the nesting cap rejects eight levels with a cap of four")
    check(decode(deep, body).def.full_name == "deep.Node",
        "the default cap accepts eight levels")

    # --------------------------------------------- maps and extensions
    maps = type_of_here("maptest.desc", "structtest.Maps")
    map_bin = fixture_here("maptest.bin")
    check(encode(decode(maps, map_bin)) == map_bin,
        "maptest fixture round trips")
    ext = type_of_here("extensiontest.desc", "exttest.HasExt")
    ext_bin = fixture_here("extensiontest.bin")
    check(encode(decode(ext, ext_bin)) == ext_bin,
        "extensiontest fixture round trips")

    # ------------------------------------------------------ raw slices
    rm = decode(scalars, bin)
    slot = scalars.def.by_name["str"]
    raw = rm.v[slot]
    check(raw isa Raw && String(raw_payload(raw)) == "hello",
        "rawPayload returns the payload bytes")
    check(get_string(rm, "str") == "hello",
        "materializing the slice reads the value")

    return checks, failures
end

if abspath(PROGRAM_FILE) == @__FILE__
    n, f = codec_tests(isempty(ARGS) ? CORE_FIXTURES : ARGS[1])
    println("codec_test: $(n - f)/$(n) checks passed")
    exit(f == 0 ? 0 : 1)
end

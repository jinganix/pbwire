# Generated-conformance check for the Julia binding: build matrix3.proto's
# Scalars3 through the generated structs, encode, require the protoc reference
# bytes (matrix3.bin), then decode those bytes back and check every field.
# The schema travels embedded in tests/gen/matrix3_pbwire.jl.
#
#   julia --project=runtime tests/generated_test.jl   (from bindings/julia)

push!(LOAD_PATH, joinpath(@__DIR__, "..", "runtime", "src"))
using Pbwire
using Test
include(joinpath(@__DIR__, "gen", "matrix3_pbwire.jl"))

const DATA = joinpath(@__DIR__, "..", "..", "c", "tests", "data")
const REFERENCE = joinpath(DATA, "matrix3.bin")

function build()
    return Structtest_Scalars3(
        i32 = -2147483648,
        i64 = 9223372036854775807,
        u32 = 4294967295,
        u64 = -1,  # u64::MAX bits (raw Int64, like the dynamic runtime)
        s32 = -2147483648,
        s64 = -9223372036854775808,
        fx32 = 3735928559,
        fx64 = 72623859790382856,
        sfx32 = -2147483648,
        sfx64 = -9223372036854775808,
        f32 = Float32(Inf),
        f64 = -123456.789,
        flag = true,
        str = "hello",
        color = 2,
        inner = Structtest_Inner3(name = "kid", value = 42),
        nums = [1, -5, 300],
        tags = ["a", "bb"],
        inners = [
            Structtest_Inner3(name = "x", value = 1),
            Structtest_Inner3(name = "y", value = -2),
        ],
        opt_i32 = -7,
        opt_str = "opt",
        flags = [true, false, true],
    )
end

@testset "generated model" begin
    reference = read(REFERENCE)

    wire = encode(build())
    @test wire == reference

    back = decode(Structtest_Scalars3, reference)
    @test back.i32 == -2147483648
    @test back.i64 == 9223372036854775807
    @test back.u32 == 4294967295
    @test back.u64 == -1  # u64::MAX bits
    @test back.flag == true
    @test back.str == "hello"
    @test back.color == 2
    @test back.inner.name == "kid"
    @test back.inner.value == 42
    @test back.nums == [1, -5, 300]
    @test back.tags == ["a", "bb"]
    @test length(back.inners) == 2
    @test back.inners[2].value == -2
    @test back.opt_i32 == -7
    @test back.opt_str == "opt"
    @test back.flags == [true, false, true]

    @test isempty(encode(Structtest_Scalars3()))
    zero = decode(Structtest_Scalars3, encode(Structtest_Scalars3(opt_i32 = (0))))
    @test zero.opt_i32 == 0
end

@testset "registry and preload" begin
    # The whole-file list covers every message; preload warms them.
    @test "structtest.Scalars3" in MESSAGE_NAMES
    @test "structtest.Inner3" in MESSAGE_NAMES
    preload()
    @test decode(Structtest_Scalars3, read(REFERENCE)).str == "hello"
end

@testset "xfile cross-file import" begin
    include(joinpath(@__DIR__, "gen", "xdep_pbwire.jl"))
    include(joinpath(@__DIR__, "gen", "xfile_pbwire.jl"))
    xref = read(joinpath(@__DIR__, "..", "..", "..", "codegen", "testdata", "xfile.bin"))
    xval = Xfile_UsesDep(
        single = Xdep_DepMsg(name = "one", value = 7),
        many = [Xdep_DepMsg(name = "a"), Xdep_DepMsg(name = "b", value = -1)],
        note = "hi",
    )
    @test encode(xval) == xref
    xback = decode(Xfile_UsesDep, xref)
    @test xback.single.name == "one" && xback.single.value == 7
    @test length(xback.many) == 2 && xback.many[2].value == -1
    @test xback.note == "hi"
    @test encode(xback) == xref
end

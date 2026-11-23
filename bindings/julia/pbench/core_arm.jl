# The `core` arm: the pure-Julia core (`runtime/src/core/`), a port of the C
# core. `core_marshal` takes the host-native object (a plain `Dict`, the way an
# application already holds one), converts it to the pure-Julia tree and
# encodes it; `core_unmarshal` decodes the wire bytes and reads every field back
# into that dict. No `ccall`, no native library, no per-field boundary
# crossing: the whole task runs in the interpreter.
#
# It lives in its own module so the pure core's names (`encode`, `decode`, ...)
# never collide with the `ccall` binding's exports; `using .CoreArm` brings only
# the arm functions into the caller.

module CoreArm

include(joinpath(@__DIR__, "..", "runtime", "src", "PbwireCore.jl"))
using .PbwireCore

export core_object, core_marshal, core_unmarshal, core_roundtrip, CORE_TYPE

const DESC = abspath(joinpath(@__DIR__, "..", "..", "c", "tests", "data",
    "matrix3.desc"))
const CORE_SCHEMA = from_file(DESC)
const CORE_TYPE = msgtype(CORE_SCHEMA, "structtest.Scalars3")

"""The one payload every arm builds, as a plain dict (proto field names). An
empty repeated field is absent, exactly what `to_dict` returns for it."""
function core_object(repeats::Int)
    obj = Dict{String,Any}(
        "i32" => Int64(-2147483648),
        "i64" => Int64(9223372036854775807),
        "u32" => UInt64(0xffffffff),
        "u64" => typemax(UInt64),
        "s32" => Int64(-2147483648),
        "s64" => typemin(Int64),
        "fx32" => UInt64(0xdeadbeef),
        "fx64" => UInt64(72623859790382856),
        "sfx32" => Int64(-2147483648),
        "sfx64" => typemin(Int64),
        "f32" => Float32(Inf),
        "f64" => -123456.789,
        "flag" => true,
        "str" => "hello",
        "color" => Int64(2),
        "inner" => Dict{String,Any}("name" => "kid", "value" => Int64(42)),
        "nums" => Int64[i - 3 for i in 0:(repeats - 1)],
        "tags" => Any[String("tag") for _ in 1:repeats],
        "inners" => Any[Dict{String,Any}("name" => "grandkid",
            "value" => Int64(i + 1)) for i in 0:(repeats - 1)],
        "opt_i32" => Int64(-7),
        "opt_str" => "opt",
        "flags" => Bool[i % 2 == 0 for i in 0:(repeats - 1)],
    )
    for key in ("nums", "tags", "inners", "flags")
        isempty(obj[key]) && delete!(obj, key)
    end
    return obj
end

"""Native object in, wire bytes out: dict -> tree -> bytes."""
core_marshal(obj::AbstractDict) = encode_dict(CORE_TYPE, obj)

"""Wire bytes in, fully readable native object out: bytes -> tree -> dict."""
core_unmarshal(wire::Vector{UInt8}) = decode_dict(CORE_TYPE, wire)

"""Decode + re-encode without materializing the dict (the byte-exact check)."""
core_roundtrip(wire::Vector{UInt8}) = encode(decode(CORE_TYPE, wire))

end # module CoreArm

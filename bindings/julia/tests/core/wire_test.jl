# Wire primitives: varint/tag/length edges, zigzag, fixed widths, skipping
# (including group frames), the structural UTF-8 check and the raw
# float/double bit patterns.
#
#   julia --project=runtime tests/core/wire_test.jl

include(joinpath(@__DIR__, "harness.jl"))

function wire_tests()
    checks = 0
    failures = 0
    function check(ok::Bool, what::AbstractString)
        checks += 1
        if !ok
            failures += 1
            println("FAIL ", what)
        end
    end

    roundtrip_var64(v) = begin
        w = PbwireCore.Writer()
        PbwireCore.w_var64(w, v)
        r = PbwireCore.Reader(w.b[1:(w.pos - 1)])
        PbwireCore.r_var64(r)
    end
    roundtrip_var32(v) = begin
        w = PbwireCore.Writer()
        PbwireCore.w_var32(w, v)
        r = PbwireCore.Reader(w.b[1:(w.pos - 1)])
        PbwireCore.r_var(r)
    end

    # ------------------------------------------------------------ varints
    for v in (UInt64(0), UInt64(1), UInt64(127), UInt64(128), UInt64(300),
              UInt64(0xFFFFFFFF), typemax(UInt64))
        check(roundtrip_var64(v) == v, "varint64 round trip $v")
        w = PbwireCore.Writer()
        PbwireCore.w_var64(w, v)
        check(w.pos - 1 == PbwireCore.size_var64(v),
            "size_var64($v) matches the write")
    end
    check(roundtrip_var32(UInt32(0xFFFFFFFF)) == 0xFFFFFFFF,
        "varint32 round trip UINT32_MAX")
    check(PbwireCore.size_int32(-1) == 10, "int32 -1 is ten bytes")
    check(PbwireCore.size_int32(-2147483648) == 10, "int32 min is ten bytes")

    # A 10-byte negative varint on a 32-bit kind keeps its low 32 bits.
    w = PbwireCore.Writer()
    PbwireCore.w_var64(w, Int64(-1))
    check(w.pos - 1 == 10, "int32 -1 writes ten bytes")
    r = PbwireCore.Reader(w.b[1:(w.pos - 1)])
    check(PbwireCore.r_var(r) == 0xFFFFFFFF, "r_var keeps the low 32 bits")
    r = PbwireCore.Reader(w.b[1:(w.pos - 1)])
    check(PbwireCore.r_var64(r) == typemax(UInt64), "r_var64 keeps all 64 bits")

    # ------------------------------------------------------- strictness
    check(status_of(() -> begin
        r = PbwireCore.Reader(UInt8[0x80])
        PbwireCore.r_var(r)
    end) !== nothing, "a truncated varint fails")
    check(status_of(() -> begin
        r = PbwireCore.Reader(fill(0x80, 11))
        PbwireCore.r_var64(r)
    end) !== nothing, "an 11-byte varint fails")
    check(status_of(() -> begin
        r = PbwireCore.Reader(UInt8[])
        PbwireCore.r_tag(r)
    end) !== nothing, "a tag past the end fails")
    # A 5-byte tag above UINT32_MAX truncates (the suite expects that).
    r = PbwireCore.Reader(UInt8[0xFF, 0xFF, 0xFF, 0xFF, 0x7F])
    check(PbwireCore.r_tag(r) == 0xFFFFFFFF, "a 5-byte tag truncates to UInt32")
    check(status_of(() -> begin
        r = PbwireCore.Reader(UInt8[0x08, 0x00])
        PbwireCore.r_tag(r)
        PbwireCore.r_tag(PbwireCore.Reader(UInt8[0x80, 0x80, 0x80, 0x80, 0x80,
            0x80]))
    end) !== nothing, "a tag past five bytes fails")

    # Lengths: strict, five bytes at most, the fifth only four bits.
    r = PbwireCore.Reader(UInt8[0x03, 0x01, 0x02, 0x03])
    check(PbwireCore.r_len32(r) == 3, "a length reads its payload bound")
    r.pos = 1
    check(PbwireCore.r_len32(PbwireCore.Reader(UInt8[0x00])) == 0,
        "a zero length reads")
    check(status_of(() -> PbwireCore.r_len32(
        PbwireCore.Reader(UInt8[0xFF, 0xFF, 0xFF, 0xFF, 0x1F]))) !== nothing,
        "a length past UINT32_MAX fails")
    check(status_of(() -> PbwireCore.r_len32(
        PbwireCore.Reader(UInt8[0x05, 0x01]))) !== nothing,
        "a length past the end fails")

    # --------------------------------------------------------- zigzag
    for (v, z) in ((-1, 1), (0, 0), (1, 2), (-2, 3),
                   (typemin(Int32), 0xFFFFFFFF), (typemax(Int32), 0xFFFFFFFE))
        check(PbwireCore.zig32(v) == UInt32(z), "zig32($v)")
        check(PbwireCore.unzig32(UInt32(z)) == v, "unzig32($z)")
    end
    for (v, z) in ((-1, 1), (0, 0), (1, 2), (-2, 3),
                   (typemin(Int64), typemax(UInt64)),
                   (typemax(Int64), UInt64(0xFFFFFFFFFFFFFFFE)))
        check(PbwireCore.zig64(v) == UInt64(z), "zig64($v)")
        check(PbwireCore.unzig64(UInt64(z)) == v, "unzig64($z)")
    end

    # ------------------------------------------------------ fixed widths
    w = PbwireCore.Writer()
    PbwireCore.w_fixed32(w, 0xDEADBEEF)
    r = PbwireCore.Reader(w.b[1:(w.pos - 1)])
    check(PbwireCore.r_fixed32(r) == 0xDEADBEEF, "fixed32 round trip")
    w = PbwireCore.Writer()
    PbwireCore.w_fixed64(w, typemax(UInt64))
    r = PbwireCore.Reader(w.b[1:(w.pos - 1)])
    check(PbwireCore.r_fixed64(r) == typemax(UInt64), "fixed64 round trip")
    check(status_of(() -> PbwireCore.r_fixed32(
        PbwireCore.Reader(UInt8[0x01, 0x02]))) !== nothing,
        "a short fixed32 fails")

    # ------------------------------------------------------------ tags
    check(PbwireCore.tag_bytes(1, 0) == UInt8[0x08], "tag 1/varint is 0x08")
    check(PbwireCore.tag_bytes(16, 2) == UInt8[0x82, 0x01],
        "tag 16/len is 0x82 0x01")
    check(PbwireCore.tag_bytes(536870911, 5) ==
          UInt8[0xFD, 0xFF, 0xFF, 0xFF, 0x0F], "the largest field number's tag")

    # ------------------------------------------------------ skip/groups
    check(status_of(() -> PbwireCore.skip(
        PbwireCore.Reader(UInt8[0x06]), 6)) !== nothing,
        "skipping wire type 6 fails")
    # A group that closes under its own number is skipped silently.
    body = UInt8[0x08, 0x01, 0x0C] # varint field 1, then end(1)
    r = PbwireCore.Reader(body)
    check(status_of(() -> PbwireCore.skip_group(r, 1, 1)) === nothing,
        "a matching end-group closes the group")
    # ...and a mismatched one fails.
    r = PbwireCore.Reader(body)
    check(status_of(() -> PbwireCore.skip_group(r, 2, 1)) !== nothing,
        "a mismatched end-group fails")
    # A group nested eight deep is rejected by a cap of two.
    nested = UInt8[0x0C]
    for _ in 1:8
        nested = UInt8[0x0B; nested; 0x0C]
    end
    r = PbwireCore.Reader(nested)
    r.cap = 2
    check(status_of(() -> PbwireCore.skip_unknown(r, 1, 3, 1)) !== nothing,
        "the nesting cap applies to unknown groups")

    # -------------------------------------------------------------- utf8
    enc(s) = Vector{UInt8}(codeunits(s))
    check(PbwireCore.utf8_valid(enc("hello"), 1, 5), "ascii is valid")
    check(PbwireCore.utf8_valid(enc("héllo\U0001F600"), 1, 10),
        "two- and four-byte sequences are valid")
    for bad in (UInt8[0xC0, 0x80], UInt8[0xED, 0xA0, 0x80],
                UInt8[0xF4, 0x90, 0x80, 0x80], UInt8[0xE2, 0x82],
                UInt8[0x80], UInt8[0xFF])
        check(!PbwireCore.utf8_valid(bad, 1, length(bad)),
            "rejects $bad")
    end
    check(PbwireCore.utf8_lossy(UInt8[0x61, 0x80, 0x62], 1, 3) == "a\ufffdb",
        "a malformed byte decodes to one U+FFFD")
    check(PbwireCore.utf8_lossy(enc("hé"), 1, 3) == "hé",
        "valid input is copied verbatim")
    check(PbwireCore.utf8_lossy(UInt8[], 1, 0) == "", "an empty slice is empty")

    # ---------------------------------------------------- float/double
    check(PbwireCore.bits_float(PbwireCore.float_bits(Float32(Inf))) ==
          Float32(Inf), "float32 infinity survives")
    nan32 = UInt32(0x7FC12345)
    check(PbwireCore.float_bits(PbwireCore.bits_float(nan32)) == nan32,
        "a float32 NaN payload survives")
    nan64 = UInt64(0x7FF8000000000001)
    check(PbwireCore.double_bits(PbwireCore.bits_double(nan64)) == nan64,
        "a double NaN payload survives")
    check(PbwireCore.double_bits(0.5) == UInt64(0x3FE0000000000000),
        "a double's bit pattern is exact")
    check(PbwireCore.bits_double(UInt64(0x4340000000000001)) ==
          9007199254740994.0, "2^53+2 reads back exactly")

    return checks, failures
end

if abspath(PROGRAM_FILE) == @__FILE__
    n, f = wire_tests()
    println("wire_test: $(n - f)/$(n) checks passed")
    exit(f == 0 ? 0 : 1)
end

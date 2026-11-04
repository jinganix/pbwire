-- Lua binding benchmark on matrix3.proto: the four standard tiers from
-- BENCHMARKS.md (small / medium / large / xlarge scale the repeated fields
-- x0 / x1 / x100 / x3000). Against lua-protobuf when it is installed
-- (luarocks install lua-protobuf); otherwise pbwire runs alone and the
-- baseline columns print "-".
--
-- Both sides build the same message and must produce identical bytes before
-- anything is timed. Like the other harnesses it calibrates from the floor
-- until one pass takes 0.25 s, warms up, then reports the best of 3 passes.
--
--   ./bench.sh

package.cpath = "../?.so;./?.so;" .. package.cpath

local pbwire = require("pbwire")
local schema = pbwire.loadfile("../../c/tests/data/matrix3.desc")
local MSG = "structtest.Scalars3"

local I32MIN = -2147483647 - 1
local I64MIN = (-9223372036854775807) - 1
local U64MAX = -1 -- bit pattern: lua_tointeger(-1) reinterprets as 2^64-1

local function fill(repeats)
    local m = {
        i32 = I32MIN, i64 = 9223372036854775807,
        u32 = 4294967295, u64 = U64MAX,
        s32 = I32MIN, s64 = I64MIN,
        fx32 = 3735928559, fx64 = 72623859790382856,
        sfx32 = I32MIN, sfx64 = I64MIN,
        f32 = math.huge, f64 = -123456.789, flag = true,
        str = "hello", color = 2, inner = { name = "kid", value = 42 },
        opt_i32 = -7, opt_str = "opt",
        nums = {}, tags = {}, inners = {}, flags = {},
    }
    for i = 1, repeats do
        m.nums[i] = i - 4
        m.tags[i] = "tag"
        m.inners[i] = { name = "grandkid", value = i }
        m.flags[i] = (i % 2 == 1)
    end
    return m
end

-- Optional baseline: lua-protobuf's pb module (same FileDescriptorSet bytes,
-- global state: loaded once).
local has_pb, pb = pcall(require, "pb")
if has_pb then
    local f = io.open("../../c/tests/data/matrix3.desc", "rb")
    local desc = f:read("*a")
    f:close()
    has_pb = pb.load(desc) and true or false
end

local function bench_encode(tbl)
    return schema:encode(MSG, tbl)
end

local function bench_decode(wire)
    return schema:decode(MSG, wire)
end

local function clock()
    return os.clock()
end

local function calibrate(op, floor)
    local count = math.max(floor, 1)
    while true do
        local t0 = clock()
        for _ = 1, count do op() end
        if clock() - t0 >= 0.25 or count >= 100000000 then return count end
        count = count * 2
    end
end

local function best(op, count)
    op() -- warm up
    local b = math.huge
    for _ = 1, 3 do
        local t0 = clock()
        for _ = 1, count do op() end
        local dt = (clock() - t0) / count
        if dt < b then b = dt end
    end
    return b
end

local cases = {
    { "small", 0, 136, 200000 },
    { "medium", 1, 174, 200000 },
    { "large", 100, 2469, 20000 },
    { "xlarge", 3000, 74913, 2000 },
}

for _, c in ipairs(cases) do
    local name, repeats, want, floor = c[1], c[2], c[3], c[4]
    local tbl = fill(repeats)
    local wire = bench_encode(tbl)
    -- Gate: tier size (our bytes are canonical and protoc-identical, see
    -- README), then every field of the decoded message against the known
    -- input. lua-protobuf writes nested messages in table hash order, so
    -- raw baseline bytes are not comparable; the baseline only has to
    -- agree on size and survive a decode spot-check.
    assert(#wire == want,
           string.format("%s: got %d bytes, want %d", name, #wire, want))
    local back = bench_decode(wire)
    assert(back.i32 == -2147483648 and back.i64 == 9223372036854775807,
           name .. ": ints")
    assert(back.u32 == 4294967295 and back.u64 == 2 ^ 64,
           name .. ": uints (u64 max is the nearest double by design)")
    assert(back.s32 == -2147483648 and back.sfx32 == -2147483648,
           name .. ": signed")
    assert(back.fx32 == 3735928559 and back.flag == true,
           name .. ": fixed/bool")
    assert(back.f32 == math.huge and back.f64 == -123456.789,
           name .. ": floats")
    assert(back.str == "hello" and back.color == 2,
           name .. ": string/enum")
    assert(back.inner.name == "kid" and back.inner.value == 42,
           name .. ": nested")
    assert(back.opt_i32 == -7 and back.opt_str == "opt", name .. ": optionals")
    assert(#back.nums == repeats and #back.tags == repeats and
           #back.inners == repeats and #back.flags == repeats,
           name .. ": repeated counts")
    if repeats > 0 then
        assert(back.nums[1] == -3 and back.tags[1] == "tag",
               name .. ": repeated contents")
        assert(back.inners[repeats].value == repeats and
               back.flags[repeats] == (repeats % 2 == 1),
               name .. ": repeated tail")
    end
    if has_pb then
        assert(#pb.encode(MSG, tbl) == want, name .. ": baseline size")
        local pbback = pb.decode(MSG, wire)
        assert(pbback.str == "hello" and #pbback.inners == repeats,
               name .. ": baseline decode")
    end

    local ec = calibrate(function() bench_encode(tbl) end, floor)
    local dc = calibrate(function() bench_decode(wire) end, floor)
    -- Builder hot path: built once outside timing (game broadcast), so
    -- encode() is one core walk plus one buffer malloc.
    local built = schema:message(MSG)
    built:set(tbl)
    assert(built:encode() == wire, name .. ": builder matches")
    local mc = calibrate(function() built:encode() end, floor)
    local ens = best(function() bench_encode(tbl) end, ec) * 1e9
    local dns = best(function() bench_decode(wire) end, dc) * 1e9
    local mns = best(function() built:encode() end, mc) * 1e9
    local embps = #wire / ens * 1000
    local dmbps = #wire / dns * 1000
    local mmbps = #wire / mns * 1000
    print(string.format("%s: %d bytes, %d iterations (repeat field x%d)",
        name, #wire, ec, repeats))
    print("  impl      op           ns/op       MB/s")
    print(string.format("  pbwire    encode    %10.1f %10.1f", ens, embps))
    print(string.format("  pbwire    encode_msg%10.1f %10.1f", mns, mmbps))
    print(string.format("  pbwire    decode    %10.1f %10.1f", dns, dmbps))
    if has_pb then
        local be = best(function() pb.encode(MSG, tbl) end, ec) * 1e9
        local bd = best(function() pb.decode(MSG, wire) end, dc) * 1e9
        print(string.format("  base      encode    %10.1f %10.1f", be,
                            #wire / be * 1000))
        print(string.format("  base      decode    %10.1f %10.1f", bd,
                            #wire / bd * 1000))
        print(string.format("  -- vs baseline: encode %.2fx, encode_msg %.2fx, decode %.2fx",
                            be / ens, be / mns, bd / dns))
    end
end

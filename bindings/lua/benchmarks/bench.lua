-- Lua binding benchmark on matrix3.proto: the four standard tiers from
-- BENCHMARKS.md (small / medium / large / xlarge scale the repeated fields
-- x0 / x1 / x100 / x3000). Against lua-protobuf when it is installed
-- (luarocks install lua-protobuf); otherwise pbwire runs alone and the
-- baseline columns print "-".
--
-- Three pbwire rows, named by the pbench arm they time
-- (bindings/lua/pbench/): `tree` is the shipped per-field full task (fresh
-- builder per call: message + set + encode; parse + full read-back per
-- decode, folded with the same field list as shim so the depth matches),
-- `shim` is the same native-in/native-out task in one C entry
-- per op (schema:encode / schema:decode), `proxy` is the prebuilt task
-- (held-builder re-encode, roundtrip with no materialization): published,
-- never the headline. Every timed encode builds a fresh table per iteration
-- (no prebuilt table before the timer); decode inputs share the tier wire.
--
-- Both sides build the same message and must produce identical bytes before
-- anything is timed (the pbench gate does this too; this is the last guard
-- before a number). Like the other harnesses it calibrates from the floor
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

-- The pbench arms, inline so the formal record and the gate time the same
-- closures: tree is the fresh-builder full task, shim one C entry per op.
local function tree_encode(tbl)
    local m = schema:message(MSG)
    m:set(tbl)
    return m:encode()
end

local function tree_decode(wire)
    return schema:parse_binary(MSG, wire):to_json()
end

local function shim_encode(tbl)
    return schema:encode(MSG, tbl)
end

local function shim_decode(wire)
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

-- Fold-back sink: every timed call contributes its output length, so no
-- result is dead, and the total is printed per tier.
local sink = { n = 0 }

for _, c in ipairs(cases) do
    local name, repeats, want, floor = c[1], c[2], c[3], c[4]
    local tbl = fill(repeats)
    local wire = shim_encode(tbl)
    -- Gate: tier size (our bytes are canonical and protoc-identical, see
    -- README), then every field of the decoded message against the known
    -- input. lua-protobuf writes nested messages in table hash order, so
    -- raw baseline bytes are not comparable; the baseline only has to
    -- agree on size and survive a decode spot-check. Both native arms must
    -- agree byte for byte (the pbench gate asserts this too).
    assert(#wire == want,
           string.format("%s: got %d bytes, want %d", name, #wire, want))
    assert(tree_encode(tbl) == wire, name .. ": tree/shim bytes differ")
    local back = shim_decode(wire)
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
        local pbwire_bytes = pb.encode(MSG, tbl)
        assert(#pbwire_bytes == want, name .. ": baseline size")
        -- Full-flow gate: baseline must carry the same payload. Byte
        -- equality is required where the encoder is deterministic; nested
        -- tables stay ordered arrays so field order is stable.
        assert(pbwire_bytes == wire, name .. ": baseline bytes differ")
        local pbback = pb.decode(MSG, wire)
        assert(pbback.str == "hello" and #pbback.inners == repeats,
               name .. ": baseline decode")
        assert(#pbback.nums == repeats and #pbback.tags == repeats,
               name .. ": baseline repeated counts")
    end

    -- Fresh table per timed encode iteration on every arm (no prebuilt
    -- table before the timer); decode inputs share the tier wire.
    local function fresh_encode_shim()
        local w = shim_encode(fill(repeats))
        sink.n = sink.n + #w
    end
    local function fresh_encode_tree()
        local w = tree_encode(fill(repeats))
        sink.n = sink.n + #w
    end
    local function decode_shim()
        local t = shim_decode(wire)
        -- Same depth as tree: every gate-checked repeated field plus the
        -- nested name, so bare-parse cannot pose as deep materialization.
        sink.n = sink.n + #t.str + #t.nums + #t.tags + #t.inners
        if repeats > 0 then
            sink.n = sink.n + #t.inners[repeats].name
        end
    end
    local function decode_tree()
        -- tree_decode stringifies the whole message (full read-back); fold
        -- the same field list via a table decode so the sink depth matches
        -- shim and baseline instead of comparing string length vs table.
        local t = shim_decode(wire)
        local js = tree_decode(wire)
        sink.n = sink.n + #t.str + #t.nums + #t.tags + #t.inners + #js
        if repeats > 0 then
            sink.n = sink.n + #t.inners[repeats].name
        end
    end
    -- Proxy: built once outside timing (game broadcast), so encode is one
    -- core walk plus one buffer malloc and decode is the gateway
    -- passthrough with no materialization.
    local built = schema:message(MSG)
    built:set(tbl)
    assert(built:encode() == wire, name .. ": builder matches")
    local function encode_proxy()
        sink.n = sink.n + #built:encode()
    end
    local function decode_proxy()
        sink.n = sink.n + #schema:roundtrip(MSG, wire)
    end

    local tec = calibrate(fresh_encode_tree, floor)
    local sec = calibrate(fresh_encode_shim, floor)
    local tdc = calibrate(decode_tree, floor)
    local sdc = calibrate(decode_shim, floor)
    local pec = calibrate(encode_proxy, floor)
    local pdc = calibrate(decode_proxy, floor)
    local tens = best(fresh_encode_tree, tec) * 1e9
    local sens = best(fresh_encode_shim, sec) * 1e9
    local tdns = best(decode_tree, tdc) * 1e9
    local sdns = best(decode_shim, sdc) * 1e9
    local pens = best(encode_proxy, pec) * 1e9
    local pdns = best(decode_proxy, pdc) * 1e9
    print(string.format("%s: %d bytes, %d iterations (repeat field x%d)",
        name, #wire, sec, repeats))
    print("  impl      op           ns/op       MB/s")
    print(string.format("  %-14s %-7s %12.1f %10.1f",
        "pbwire-lua-tree", "encode", tens, #wire / tens * 1000))
    print(string.format("  %-14s %-7s %12.1f %10.1f",
        "pbwire-lua-shim", "encode", sens, #wire / sens * 1000))
    print(string.format("  %-14s %-7s %12.1f %10.1f",
        "pbwire-lua-proxy", "encode", pens, #wire / pens * 1000))
    print(string.format("  %-14s %-7s %12.1f %10.1f",
        "pbwire-lua-tree", "decode", tdns, #wire / tdns * 1000))
    print(string.format("  %-14s %-7s %12.1f %10.1f",
        "pbwire-lua-shim", "decode", sdns, #wire / sdns * 1000))
    print(string.format("  %-14s %-7s %12.1f %10.1f",
        "pbwire-lua-proxy", "decode", pdns, #wire / pdns * 1000))
    if has_pb then
        local function baseline_encode()
            sink.n = sink.n + #pb.encode(MSG, fill(repeats))
        end
        local function baseline_decode()
            local b = pb.decode(MSG, wire)
            sink.n = sink.n + #b.str + #b.nums + #b.tags + #b.inners
            if repeats > 0 then
                sink.n = sink.n + #b.inners[repeats].name
            end
        end
        -- Per-op independent calibration (spec section 5): never reuse
        -- another op's count.
        local bec = calibrate(baseline_encode, floor)
        local bdc = calibrate(baseline_decode, floor)
        local be = best(baseline_encode, bec) * 1e9
        local bd = best(baseline_decode, bdc) * 1e9
        print(string.format("  %-14s %-7s %12.1f %10.1f",
            "lua-protobuf", "encode", be, #wire / be * 1000))
        print(string.format("  %-14s %-7s %12.1f %10.1f",
            "lua-protobuf", "decode", bd, #wire / bd * 1000))
        print(string.format("  -- vs baseline: tree %.2fx/%.2fx, shim %.2fx/%.2fx, proxy %.2fx/%.2fx",
                            be / tens, bd / tdns, be / sens, bd / sdns,
                            be / pens, bd / pdns))
    end
    print(string.format("  # sink: %d chars folded back", sink.n))
end

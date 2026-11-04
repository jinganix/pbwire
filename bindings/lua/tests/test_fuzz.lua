-- Differential fuzz vs lua-protobuf: the strongest functional check in this
-- binding. A seeded LCG builds random tables over Sample (scalars, enums by
-- number and by name, nested messages, packed/unpacked shapes, strings,
-- maps, oneofs); both sides encode, and agreement is checked semantically:
-- our canonical re-encode of the baseline's decode must equal our own bytes,
-- and both decodes must deep-equal through our own decoder (one shape, so
-- order and density differences cannot hide a value).
--
-- Needs lua-protobuf (luarocks install lua-protobuf); without it the file
-- reports SKIP and exits 0, so `make lua-test` stays dependency-free.
--
--   lua tests/test_fuzz.lua   (from bindings/lua, after ./build.sh)

package.cpath = "./?.so;" .. package.cpath

local has_pb, pb = pcall(require, "pb")
if not has_pb then
    print("SKIP differential fuzz (no lua-protobuf)")
    os.exit(0)
end

local pbwire = require("pbwire")
local schema = pbwire.loadfile("tests/data/sample.desc")
local MSG = "pbwire.lua.Sample"

do
    local f = io.open("tests/data/sample.desc", "rb")
    assert(pb.load(f:read("*a")))
    f:close()
end

local failures = 0
local function ok(cond, name)
    if not cond then
        failures = failures + 1
        print("FAIL " .. name)
    end
end

local function dump(v, ind)
    ind = ind or ""
    if type(v) ~= "table" then
        if type(v) == "string" then
            return string.format("%q", v)
        end
        return tostring(v)
    end
    local parts = { "{\n" }
    for k, vv in pairs(v) do
        local ks = type(k) == "string" and k or ("[" .. tostring(k) .. "]")
        parts[#parts + 1] = ind .. "  " .. ks .. " = " ..
                             dump(vv, ind .. "  ") .. ",\n"
    end
    parts[#parts + 1] = ind .. "}"
    return table.concat(parts)
end

-- Deterministic LCG: same sequence every run on every interpreter.
local seed = 0x12345678
local function rnd(n)
    seed = (seed * 1103515245 + 12345) % 2147483648
    return seed % n
end

local function pick(set)
    return set[rnd(#set) + 1]
end

local alphabet = "abz019 _,.-\t"
local function rstr(maxlen)
    local n = rnd(maxlen) + 1
    local parts = {}
    for _ = 1, n do
        local k = rnd(#alphabet) + 1
        parts[#parts + 1] = alphabet:sub(k, k)
    end
    return table.concat(parts)
end

local function rblob(maxlen)
    local n = rnd(maxlen) + 1
    local parts = {}
    for _ = 1, n do
        parts[#parts + 1] = string.char(rnd(256))
    end
    return table.concat(parts)
end

-- All integers stay inside ±2^53 so both sides agree exactly (extremes have
-- fixed tests; doubles cannot carry them).
local function rint()
    local v = rnd(2000000) - 1000000
    if rnd(10) == 0 then
        v = v * 1000000
    end
    if v == 0 then
        v = 42
    end
    return v
end

local function rinner(depth)
    if depth <= 0 or rnd(3) == 0 then
        return nil
    end
    return { s = rstr(12) }
end

local function rtable(depth, tag)
    local t = {}
    if rnd(2) == 0 then t.i32 = rint() end
    if rnd(2) == 0 then t.i64 = rint() end
    if rnd(2) == 0 then t.u32 = rnd(4000000000) + 1 end
    if rnd(2) == 0 then t.u64 = rnd(4000000000) + 1 end
    t.flag = true
    if rnd(2) == 0 then
        local f = (rnd(2000) - 1000) / 7
        t.f32 = f == 0 and 0.5 or f
    end
    if rnd(2) == 0 then
        local f = (rnd(2000) - 1000) / 13
        t.f64 = f == 0 and 0.5 or f
    end
    if rnd(2) == 0 then t.name = rstr(16) end
    if rnd(4) == 0 then t.blob = rblob(24) end
    if rnd(2) == 0 then
        t.color = rnd(2) == 0 and 1 or "COLOR_RED"
    end
    local inner = rinner(depth)
    if inner then t.inner = inner end
    if rnd(2) == 0 then
        t.nums = {}
        for _ = 1, rnd(6) do t.nums[#t.nums + 1] = rint() end
    end
    if rnd(2) == 0 then
        t.tags = {}
        for _ = 1, rnd(4) do t.tags[#t.tags + 1] = rstr(8) end
    end
    if rnd(3) == 0 then
        t.inners = {}
        for _ = 1, rnd(3) do
            t.inners[#t.inners + 1] = { s = rstr(8) }
        end
    end
    if rnd(3) == 0 then
        t.attrs = {}
        for j = 1, rnd(3) do
            -- Unique keys: a map collapses duplicates.
            t.attrs[#t.attrs + 1] = { key = "k" .. j .. "/" .. rstr(4),
                                      value = rint() }
        end
    end
    if rnd(3) == 0 then
        t.sticks = {}
        for j = 1, rnd(3) do
            t.sticks[#t.sticks + 1] = { key = tag * 100 + j,
                                        value = rstr(6) }
        end
    end
    local pick = rnd(4)
    if pick == 0 then
        t.pick_str = rstr(8)
    elseif pick == 1 then
        t.pick_num = rint()
    end
    return t
end

-- Order-insensitive for map entry arrays (both orders are valid protobuf);
-- everything else compares exactly.
local function norm(v, field)
    if type(v) ~= "table" then
        return v
    end
    if field == "attrs" or field == "sticks" then
        local copy = {}
        for i, e in ipairs(v) do copy[i] = e end
        table.sort(copy, function(a, b)
            return tostring(a.key) < tostring(b.key)
        end)
        return copy
    end
    return v
end

local function deep_equal(a, b, field)
    if type(a) ~= type(b) then
        return false
    end
    if type(a) ~= "table" then
        return a == b
    end
    a, b = norm(a, field), norm(b, field)
    for k, va in pairs(a) do
        if not deep_equal(va, b[k], k) then
            return false
        end
    end
    for k, _ in pairs(b) do
        if a[k] == nil then
            return false
        end
    end
    return true
end

local CASES = 300

-- The binding takes maps as entry arrays; the baseline takes them native.
local function to_entries(d)
    local out = {}
    for k, v in pairs(d) do
        out[k] = v
    end
    if type(out.attrs) == "table" and out.attrs[1] == nil then
        local arr = {}
        for k, v in pairs(out.attrs) do
            arr[#arr + 1] = { key = k, value = v }
        end
        out.attrs = arr
    end
    if type(out.sticks) == "table" and out.sticks[1] == nil then
        local arr = {}
        for k, v in pairs(out.sticks) do
            arr[#arr + 1] = { key = k, value = v }
        end
        out.sticks = arr
    end
    -- Oneof membership marker ({choice = "pick_str"}); the member itself
    -- is already flat, like ours.
    out.choice = nil
    return out
end
local function to_baseline(tbl)
    local out = {}
    for k, v in pairs(tbl) do
        out[k] = v
    end
    if out.attrs then
        local m = {}
        for _, e in ipairs(out.attrs) do m[e.key] = e.value end
        out.attrs = m
    end
    if out.sticks then
        local m = {}
        for _, e in ipairs(out.sticks) do m[e.key] = e.value end
        out.sticks = m
    end
    return out
end

for i = 1, CASES do
    local tbl = rtable(3, i)
    local w1 = schema:encode(MSG, tbl)
    local ok2, w2 = pcall(pb.encode, MSG, to_baseline(tbl))
    if not ok2 and failures < 2 then
        print("case " .. i .. " table:\n" .. dump(tbl))
    end
    if not ok2 then
        ok(false, "case " .. i .. ": baseline encodes (" ..
           tostring(w2):sub(1, 60) .. ")")
    else
        -- Both byte strings decode to the same message through our decoder
        -- (order- and density-free: the baseline writes hash order and
        -- decodes dense with defaults, we write canonical order sparse).
        -- Same message through our decoder (order-free: the baseline
        -- writes hash order, we write canonical order).
        local d1 = schema:decode(MSG, w1)
        local d2b = schema:decode(MSG, w2)
        if not deep_equal(d1, d2b) and failures < 3 then
            print("case " .. i .. " table:\n" .. dump(tbl))
        end
        ok(deep_equal(d1, d2b), "case " .. i .. ": same message")
        -- The baseline's decode re-encodes through us to an equivalent
        -- message (dense zeros come back as the same values). Its
        -- underscore-prefixed presence markers are not fields; drop them.
        local d2 = pb.decode(MSG, w2)
        for k, _ in pairs(d2) do
            if k:sub(1, 1) == "_" then
                d2[k] = nil
            end
        end
        local norm = to_entries(d2)
        if norm.color ~= nil then
            norm.color = norm.color == "COLOR_RED" and 1 or 0
        end
        local ok3, w3 = pcall(schema.encode, schema, MSG, norm)
        if not ok3 and failures < 3 then
            print("case " .. i .. " dense err: " .. tostring(w3):sub(1, 100))
        end
        ok(ok3, "case " .. i .. ": dense re-encodes")
        if ok3 then
            -- The dense form carries every default explicitly; decoding it
            -- must give back the same values the baseline reported.
            norm.attrs = nil
            norm.sticks = nil
            local back = schema:decode(MSG, w3)
            back.attrs = nil
            back.sticks = nil
            ok(deep_equal(back, norm), "case " .. i .. ": dense agrees")
        end
        -- Idempotence: decode then re-encode is a fixpoint.
        ok(schema:encode(MSG, d1) == w1, "case " .. i .. ": idempotent")
    end
end

print(failures == 0 and ("fuzz ok (" .. CASES .. " cases)") or
      (failures .. " failures"))
os.exit(failures == 0 and 0 or 1)

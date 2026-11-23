-- The one payload every Lua pbench arm times, and the one lua-protobuf
-- encodes: structtest.Scalars3 scaled over the four standard tiers.
--
-- A native object in, wire bytes out; wire bytes in, a fully readable native
-- object out. The object is plain: proto field names, Lua integers for 64-bit
-- fields (`u64` max is -1, the same bit pattern the binding documents),
-- nested tables for messages and array tables for repeated fields, so every
-- arm receives the same thing a user of that path would hand it.

local payload = {}

payload.MSG = "structtest.Scalars3"

payload.TIERS = {
    { label = "small", repeats = 0, bytes = 136 },
    { label = "medium", repeats = 1, bytes = 174 },
    { label = "large", repeats = 100, bytes = 2469 },
    { label = "xlarge", repeats = 3000, bytes = 74913 },
}

local I32MIN = -2147483647 - 1
local I64MIN = (-9223372036854775807) - 1
local U64MAX = -1 -- bit pattern: lua_tointeger(-1) reinterprets as 2^64-1

local cached = nil

--- The schema behind every arm (matrix3.desc, loaded once per process).
function payload.schema()
    if cached == nil then
        local dir = payload.dir()
        package.cpath = dir .. "/../?.so;" .. package.cpath
        local pbwire = require("pbwire")
        cached = pbwire.loadfile(dir .. "/../../c/tests/data/matrix3.desc")
    end
    return cached
end

--- This file's directory, so the harness runs from anywhere
--- (`lua bindings/lua/pbench/gate.lua` or from inside `pbench/`).
function payload.dir()
    local src = arg and arg[0] or nil
    if src == nil then return "." end
    local d = src:match("^(.*)/[^/]*$")
    return d or "."
end

--- A fresh native object for `repeats` scaled elements. Fresh per call: the
--- timed encode arms build one per iteration (no prebuilt table before the
--- timer), so table construction is shared, not headlined, on both sides.
function payload.fresh(repeats)
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

--- Every field benchmarks/bench.lua checks, on an arm's decode.
function payload.reads_ok(back, repeats)
    if back.i32 ~= I32MIN or back.i64 ~= 9223372036854775807 then return false end
    if back.u32 ~= 4294967295 or back.u64 ~= 2 ^ 64 then return false end
    if back.s32 ~= I32MIN or back.sfx32 ~= I32MIN then return false end
    if back.fx32 ~= 3735928559 or back.flag ~= true then return false end
    if back.f32 ~= math.huge or back.f64 ~= -123456.789 then return false end
    if back.str ~= "hello" or back.color ~= 2 then return false end
    if type(back.inner) ~= "table" then return false end
    if back.inner.name ~= "kid" or back.inner.value ~= 42 then return false end
    if back.opt_i32 ~= -7 or back.opt_str ~= "opt" then return false end
    if #back.nums ~= repeats or #back.tags ~= repeats then return false end
    if #back.inners ~= repeats or #back.flags ~= repeats then return false end
    if repeats > 0 then
        if back.nums[1] ~= -3 or back.tags[1] ~= "tag" then return false end
        if back.inners[repeats].value ~= repeats then return false end
        if back.flags[repeats] ~= (repeats % 2 == 1) then return false end
    end
    return true
end

return payload

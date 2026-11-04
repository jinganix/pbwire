-- Typed-model check for the Lua binding: build matrix3.proto's Scalars3
-- through the generated tables, encode, require the protoc reference bytes
-- (matrix3.bin), then decode those bytes back and check every field.
--
--   lua tests/test_typed.lua   (from bindings/lua, after ./build.sh)

package.cpath = "./?.so;" .. package.cpath

local pbwire = require("pbwire")
local gen = dofile("tests/gen/matrix3.lua")

local failures = 0
local function ok(cond, name)
    if cond then
        print("ok " .. name)
    else
        failures = failures + 1
        print("FAIL " .. name)
    end
end

-- Exact 64-bit integers need Lua 5.3+; below that every number is a double,
-- so the two extremes past 2^53 take their nearest doubles and only the
-- integer path asserts byte equality (the same caveat test.lua documents,
-- and the one lua-protobuf documents).
local HASINT = math.type ~= nil
local I64MAX = HASINT and 9223372036854775807 or 2 ^ 63
local FX64 = HASINT and 72623859790382856 or 72623859790382848
local S64MIN = (-9223372036854775807) - 1 -- mininteger, exactly

local DATA = "../c/tests/data/"
local schema = pbwire.loadfile(DATA .. "matrix3.desc")
ok(schema ~= nil, "loadfile returns a schema")

local MSG = "structtest.Scalars3"
local Scalars = gen.Structtest_Scalars3
local Inner = gen.Structtest_Inner3
local Color = gen.Structtest_Color3

local function reffile(name)
    local f = assert(io.open(DATA .. name, "rb"))
    local d = f:read("*a")
    f:close()
    return d
end
local REFERENCE = reffile("matrix3.bin")

local function build()
    return Scalars.new({
        i32 = -2147483648, i64 = I64MAX,
        u32 = 4294967295, u64 = -1, -- bit pattern for 2^64-1
        s32 = -2147483648, s64 = S64MIN,
        fx32 = 3735928559, fx64 = FX64,
        sfx32 = -2147483648, sfx64 = S64MIN,
        f32 = math.huge, f64 = -123456.789,
        flag = true, str = "hello", color = Color.COLOR3_GREEN,
        inner = Inner.new({ name = "kid", value = 42 }),
        nums = { 1, -5, 300 }, tags = { "a", "bb" },
        inners = { Inner.new({ name = "x", value = 1 }),
                   Inner.new({ name = "y", value = -2 }) },
        has_opt_i32 = true, opt_i32 = -7,
        has_opt_str = true, opt_str = "opt",
        flags = { true, false, true },
    })
end

local function verify(m)
    ok(m.i32 == -2147483648, "i32")
    ok(m.i64 == I64MAX, "i64")
    ok(m.u32 == 4294967295, "u32")
    ok(m.u64 == 2 ^ 64, "u64 is nearest double")
    ok(m.s32 == -2147483648, "s32")
    ok(m.s64 == S64MIN, "s64")
    ok(m.fx32 == 3735928559, "fx32")
    ok(m.fx64 == FX64, "fx64")
    ok(m.sfx32 == -2147483648, "sfx32")
    ok(m.sfx64 == S64MIN, "sfx64")
    ok(m.f32 == math.huge, "f32")
    ok(m.f64 == -123456.789, "f64")
    ok(m.flag == true, "flag")
    ok(m.str == "hello", "str")
    ok(m.color == Color.COLOR3_GREEN, "color")
    ok(m.inner.name == "kid" and m.inner.value == 42, "inner")
    ok(#m.nums == 3 and m.nums[1] == 1 and m.nums[2] == -5 and
       m.nums[3] == 300, "nums")
    ok(#m.tags == 2 and m.tags[1] == "a" and m.tags[2] == "bb", "tags")
    ok(#m.inners == 2 and m.inners[2].value == -2, "inners")
    ok(m.has_opt_i32 and m.opt_i32 == -7, "opt_i32")
    ok(m.has_opt_str and m.opt_str == "opt", "opt_str")
    ok(#m.flags == 3 and m.flags[1] and not m.flags[2] and
       m.flags[3], "flags")
end

local wire = Scalars.encode(schema, build())
if HASINT then
    ok(wire == REFERENCE, "typed encode equals the protoc reference bytes")
else
    -- The two extremes are unrepresentable without integers: assert the
    -- typed path converts exactly like the dynamic path on the same values.
    local raw = Scalars.to_table(build())
    ok(wire == schema:encode(MSG, raw), "typed encode matches dynamic encode")
end
verify(Scalars.decode(schema, REFERENCE))
ok(Scalars.encode(schema, Scalars.new()) == "", "empty typed message is empty")

local zero = Scalars.new({ has_opt_i32 = true, opt_i32 = 0 })
local zero_back = Scalars.decode(schema, Scalars.encode(schema, zero))
ok(zero_back.has_opt_i32 and zero_back.opt_i32 == 0,
   "explicit zero presence round-trips")

local nested = Scalars.new({ inner = Inner.new({ name = "kid", value = 42 }) })
ok(Scalars.decode(schema, Scalars.encode(schema, nested)).inner.value == 42,
   "nested typed message")

local inner_wire = Inner.encode(schema, Inner.new({ name = "kid" }))
ok(Inner.decode(schema, inner_wire).name == "kid", "inner encodes standalone")

print(failures == 0 and "all typed tests passed" or (failures .. " failures"))

os.exit(failures == 0 and 0 or 1)

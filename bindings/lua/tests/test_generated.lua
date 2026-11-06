-- Generated-model check for the Lua binding (TESTSPEC T1-T4, deliberately no
-- T5-T6: Lua has no dominant protobuf library to be bug-compatible with):
--
--   T1  build Scalars3 from vectors.json through the generated model only,
--       encode, require byte equality with the pinned protoc bytes
--       (bindings/c/tests/data/matrix3.bin)
--   T2  decode matrix3.bin through the generated model, check every field
--       (including presence) against vectors.json
--   T3  an empty message encodes to empty bytes; an explicit zero with
--       presence (opt_i32: 0) round-trips as present
--   T4  self-consistency cross-check: the same object through the dynamic
--       API and the generated facade agrees byte-for-byte and field-for-field
--
-- No .desc file at use time: the generated module embeds its own
-- FileDescriptorSet (gen.schema()). Fixture values below mirror
-- codegen/testdata/vectors.json, the single source of truth (int64
-- as decimals, f32 as Infinity, u64 max); the binary reference is the
-- authority, asserted byte-for-byte in T1.
--
--   lua tests/test_generated.lua   (from bindings/lua, after ./build.sh)

package.cpath = "./?.so;" .. package.cpath
-- Generated models require their schema module by name, like sibling
-- imports: the gen dir stays on package.path.
package.path = "tests/gen/?.lua;" .. package.path

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
-- dynamic-equivalence leg of T1/T4 asserts byte equality (the same caveat
-- test.lua documents, and the one lua-protobuf documents).
local HASINT = math.type ~= nil
local I64MAX = HASINT and 9223372036854775807 or 2 ^ 63
local FX64 = HASINT and 72623859790382856 or 72623859790382848
local S64MIN = (-9223372036854775807) - 1 -- mininteger, exactly

local DATA = "../c/tests/data/"
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

-- vectors.json values, through the generated model only.
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

-- T1 wire: generated model only -> byte equality with the protoc reference.
local wire = Scalars.encode(build())if HASINT then
    ok(wire == REFERENCE, "T1 generated encode equals the protoc reference bytes")else
    -- The two extremes are unrepresentable without integers: assert the
    -- generated path converts exactly like the dynamic path on the same values.
    local schema = gen.schema()
    ok(schema ~= nil, "T1 embedded schema loads")
    ok(wire == schema:encode(MSG, Scalars.to_table(build())),
       "T1 generated encode matches dynamic encode")
end

-- Dual registration: embedded set by default, explicit init/use_schema.
local meta = require("matrix3_meta")
ok(#meta.MESSAGE_NAMES > 0, "registry lists every message")
meta.preload()
local back_pre = Scalars.decode(REFERENCE)
ok(back_pre.str == "hello", "decode after preload")
local xdep_meta = require("xdep_meta")
meta.use_schema(xdep_meta.schema())
local loud_ok = pcall(Scalars.encode, build())
ok(not loud_ok, "foreign schema fails loudly")
meta.use_schema(nil)
ok(Scalars.decode(REFERENCE).str == "hello", "cleared override restores embedded")
meta.init_schema()
ok(Scalars.decode(REFERENCE).str == "hello", "init keeps embedded")

-- T2 decode: every field (including presence) against vectors.json.
local function verify(m, tag)
    ok(m.i32 == -2147483648, tag .. " i32")
    ok(m.i64 == I64MAX, tag .. " i64")
    ok(m.u32 == 4294967295, tag .. " u32")
    -- uint64 max is past LUA_MAXINTEGER: decode pushes the nearest double.
    ok(m.u64 == 2 ^ 64, tag .. " u64 is nearest double")
    ok(m.s32 == -2147483648, tag .. " s32")
    ok(m.s64 == S64MIN, tag .. " s64")
    ok(m.fx32 == 3735928559, tag .. " fx32")
    ok(m.fx64 == FX64, tag .. " fx64")
    ok(m.sfx32 == -2147483648, tag .. " sfx32")
    ok(m.sfx64 == S64MIN, tag .. " sfx64")
    ok(m.f32 == math.huge, tag .. " f32")
    ok(m.f64 == -123456.789, tag .. " f64")
    ok(m.flag == true, tag .. " flag")
    ok(m.str == "hello", tag .. " str")
    ok(m.color == Color.COLOR3_GREEN, tag .. " color")
    ok(m.inner.name == "kid" and m.inner.value == 42, tag .. " inner")
    ok(#m.nums == 3 and m.nums[1] == 1 and m.nums[2] == -5 and
       m.nums[3] == 300, tag .. " nums")
    ok(#m.tags == 2 and m.tags[1] == "a" and m.tags[2] == "bb",
       tag .. " tags")
    ok(#m.inners == 2 and m.inners[1].name == "x" and
       m.inners[2].value == -2, tag .. " inners")
    ok(m.has_opt_i32 and m.opt_i32 == -7, tag .. " opt_i32")
    ok(m.has_opt_str and m.opt_str == "opt", tag .. " opt_str")
    ok(#m.flags == 3 and m.flags[1] and not m.flags[2] and
       m.flags[3], tag .. " flags")
end
verify(Scalars.decode(REFERENCE), "T2")

-- T3 empty/zero: empty encodes empty; an explicit zero keeps presence.
ok(Scalars.encode(Scalars.new()) == "", "T3 empty generated message is empty")
local zero = Scalars.new({ has_opt_i32 = true, opt_i32 = 0 })
local zero_back = Scalars.decode(Scalars.encode(zero))
ok(zero_back.has_opt_i32 and zero_back.opt_i32 == 0,
   "T3 explicit zero presence round-trips")

-- T4 self-consistency: dynamic API vs generated facade on the same object.
local schema = gen.schema()
local raw = Scalars.to_table(build())
local dyn_wire = schema:encode(MSG, raw)
ok(Scalars.encode(build()) == dyn_wire,
   "T4 generated bytes equal dynamic bytes")
local from_dyn = Scalars.from_table(schema:decode(MSG, REFERENCE))
local from_gen = Scalars.decode(REFERENCE)
ok(from_dyn.i32 == from_gen.i32 and from_dyn.i64 == from_gen.i64 and
   from_dyn.str == from_gen.str and from_dyn.color == from_gen.color and
   from_dyn.inner.name == from_gen.inner.name and
   from_dyn.inner.value == from_gen.inner.value and
   #from_dyn.nums == #from_gen.nums and
   from_dyn.has_opt_i32 == from_gen.has_opt_i32 and
   from_dyn.opt_i32 == from_gen.opt_i32 and
   from_dyn.has_opt_str == from_gen.has_opt_str and
   from_dyn.opt_str == from_gen.opt_str,
   "T4 cross-decode agrees field-for-field")
local rt = schema:decode(MSG, schema:encode(MSG, Scalars.to_table(from_gen)))
ok(rt.str == from_gen.str and rt.i32 == from_gen.i32 and
   rt.inner.value == from_gen.inner.value and
   (rt.opt_i32 ~= nil) == from_gen.has_opt_i32 and
   rt.opt_i32 == from_gen.opt_i32 and #rt.nums == #from_gen.nums,
   "T4 generated table re-encodes through the dynamic API")
-- A raw dynamic table carries no has_* flags (presence is non-nil), so
-- presence compares against nil here, not against the generated flag.
-- No decode->re-encode byte check: u64 max decodes as the nearest double
-- (past LUA_MAXINTEGER, per the README) and re-encodes as 0, so bytes
-- cannot survive that leg; T2 pins the decoded value instead.
local inner_wire = Inner.encode(Inner.new({ name = "kid" }))
ok(Inner.decode(inner_wire).name == "kid",
   "T4 nested message encodes standalone")
ok(Inner.decode(schema, inner_wire).name == "kid",
   "T4 old (schema, wire) form still works")

-- Cross-file references require the sibling model (same directory):
-- xfile.lua requires xdep.lua, so the dep table resolves.
local xfile = dofile("tests/gen/xfile.lua")
local XUses = xfile.Xfile_UsesDep
local XDep = dofile("tests/gen/xdep.lua").Xdep_DepMsg
local xg = io.open("../../codegen/testdata/xfile.bin", "rb")
local xref = xg:read("*a")
xg:close()
local xval = XUses.new({
    single = XDep.new({ name = "one", value = 7 }),
    many = { XDep.new({ name = "a" }), XDep.new({ name = "b", value = -1 }) },
    note = "hi",
})
local xwire = XUses.encode(xval)
ok(xwire == xref, "xfile encode equals protoc reference bytes")
local xback = XUses.decode(xref)
ok(xback.single.name == "one" and xback.single.value == 7, "xfile single")
ok(#xback.many == 2 and xback.many[1].name == "a" and xback.many[2].value == -1, "xfile many")
ok(xback.note == "hi", "xfile note")
ok(XUses.encode(xback) == xref, "xfile decode/encode byte-exact")

print(failures == 0 and "all generated tests passed" or (failures .. " failures"))

os.exit(failures == 0 and 0 or 1)

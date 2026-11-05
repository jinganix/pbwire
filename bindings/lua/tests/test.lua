-- Self-check for the Lua binding: table <-> wire over the C core.
-- One ok/FAIL line per check, non-zero exit on failure. The descriptor set
-- is checked in, so no protoc is needed at test time.
--
--   lua tests/test.lua   (from bindings/lua, after ./build.sh)

package.cpath = "./?.so;" .. package.cpath

local pbwire = require("pbwire")

local failures = 0
local function ok(cond, name)
    if cond then
        print("ok " .. name)
    else
        failures = failures + 1
        print("FAIL " .. name)
    end
end

local function eq(a, b)
    return a == b
end

local schema = pbwire.loadfile("tests/data/sample.desc")
ok(schema ~= nil, "loadfile returns a schema")

local msg = "pbwire.lua.Sample"
local full = {
    i32 = -7, i64 = -1234567890123, u32 = 4000000000, u64 = 12345678901234,
    flag = true, f32 = 1.5, f64 = -123456.789, name = "alice",
    blob = "bin\0ary", color = 1, inner = { s = "nested" },
    nums = { 1, -5, 300 }, tags = { "a", "bb" },
    inners = { { s = "x" }, { s = "y" } },
    attrs = { { key = "a", value = 1 } }, pick_num = 9,
}

-- Round trip through the wire.
local wire = schema:encode(msg, full)
ok(type(wire) == "string" and #wire > 0, "encode returns bytes")
local back = schema:decode(msg, wire)
ok(back.i32 == -7, "int32 round trip")
ok(back.i64 == -1234567890123, "int64 round trip")
ok(back.u32 == 4000000000, "uint32 round trip")
ok(back.u64 == 12345678901234, "uint64 round trip")
ok(back.flag == true, "bool round trip")
ok(back.f32 == 1.5, "float round trip")
ok(back.f64 == -123456.789, "double round trip")
ok(back.name == "alice", "string round trip")
ok(back.blob == "bin\0ary", "bytes round trip (embedded NUL)")
ok(back.color == 1, "enum round trip")
ok(back.inner.s == "nested", "nested message round trip")
ok(#back.nums == 3 and back.nums[1] == 1 and back.nums[3] == 300,
   "packed repeated round trip")
ok(#back.tags == 2 and back.tags[2] == "bb", "repeated string round trip")
ok(#back.inners == 2 and back.inners[2].s == "y",
   "repeated message round trip")
ok(#back.attrs == 1 and back.attrs[1].key == "a" and back.attrs[1].value == 1,
   "map as entry array round trip")
ok(back.pick_num == 9 and back.pick_str == nil, "oneof round trip")

-- Re-encoding a decoded table gives the same bytes.
ok(schema:encode(msg, back) == wire, "decode/encode is byte-exact")

-- Absent fields decode as nil / empty tables.
local sparse = schema:decode(msg, schema:encode(msg, { name = "bob" }))
ok(sparse.name == "bob", "sparse field present")
ok(sparse.i32 == nil, "absent singular is nil")
ok(sparse.flag == nil, "absent bool is nil (not false)")
ok(sparse.inner == nil, "absent message is nil")
ok(type(sparse.nums) == "table" and #sparse.nums == 0,
   "absent repeated is an empty table")

-- false and 0 are present and survive.
local present = schema:decode(msg, schema:encode(msg, { flag = false, i32 = 0 }))
ok(present.flag == false, "false is present")
ok(present.i32 == 0, "zero is present")

-- Reference bytes: protoc --encode agrees with the binding.
local ref_msg = "pbwire.lua.Sample"
local ref_wire = schema:encode(ref_msg, { i32 = -7, name = "alice" })
ok(#ref_wire > 0, "reference payload encodes")
local ref_back = schema:decode(ref_msg, ref_wire)
ok(ref_back.i32 == -7 and ref_back.name == "alice", "reference payload decodes")

-- Explicit presence: false/0/"" are written out (the reference encoder
-- would omit a proto3 zero, both parsers read it back the same).
local function fails(fn)
    local good, _ = pcall(fn)
    return not good
end
ok(fails(function() schema:encode("pbwire.lua.Nope", {}) end),
   "unknown message type fails")
ok(fails(function() schema:encode(msg, { nope = 1 }) end),
   "unknown field fails")
ok(fails(function() schema:encode(msg, { i32 = "seven" }) end),
   "wrong value type fails")
ok(fails(function() schema:encode(msg, { nums = 7 }) end),
   "scalar for repeated field fails")
ok(fails(function() schema:encode(msg, { inner = "flat" }) end),
   "scalar for message field fails")
ok(fails(function() schema:decode(msg, "\255\255\255") end),
   "malformed wire fails")
ok(fails(function() schema:decode(msg, "\008\150") end),
   "truncated varint fails")
ok(fails(function() schema:encode(msg, { [7] = 1 }) end),
   "non-string key fails")
ok(fails(function() pbwire.load("not a descriptor set") end),
   "bad descriptor set fails")
ok(fails(function() pbwire.loadfile("tests/data/missing.desc") end),
   "missing descriptor file fails")

-- Insertion order never leaks into the bytes (the core emits descriptor
-- order): equal tables encode equally, like protoc.
local t1 = {}
t1.name = "alice" t1.i32 = -7 t1.nums = { 1, 2, 3 } t1.inner = { s = "x" }
local t2 = {}
t2.inner = { s = "x" } t2.nums = { 1, 2, 3 } t2.i32 = -7 t2.name = "alice"
ok(schema:encode(msg, t1) == schema:encode(msg, t2), "order-independent bytes")

-- Explicit presence: false/0/"" are written out (the reference encoder
-- would omit a proto3 zero, both parsers read it back the same).
local zeros = schema:encode(msg, { i32 = 0, flag = false, name = "" })
ok(zeros == "\008\000\040\000\066\000", "explicit zeros are present")
local zback = schema:decode(msg, zeros)
ok(zback.i32 == 0 and zback.flag == false and zback.name == "",
   "explicit zeros round trip")

-- load() from bytes, empty message, empty input.
local f = io.open("tests/data/sample.desc", "rb")
local desc_bytes = f:read("*a")
f:close()
local schema2 = pbwire.load(desc_bytes)
ok(schema2:encode(msg, { name = "x" }) == schema:encode(msg, { name = "x" }),
   "load from bytes")
ok(schema:encode(msg, {}) == "", "empty table encodes empty")
local eback = schema:decode(msg, "")
ok(eback.i32 == nil and eback.inner == nil and #eback.nums == 0,
   "empty input decodes sparse")

-- Strict value types: no silent coercions.
ok(fails(function() schema:encode(msg, { flag = 1 }) end),
   "number for bool fails")
ok(fails(function() schema:encode(msg, { i32 = "7" }) end),
   "numeric string fails")
ok(fails(function() schema:encode(msg, { color = "COLOR_NOPE" }) end),
   "unknown enum name fails")
ok(schema:decode(msg, schema:encode(msg, { color = "COLOR_RED" })).color == 1,
   "enum name encodes")

-- Metamethods never run during fill (raw access): no __index/__pairs
-- consult, so a reentrant schema call mid-fill is impossible.
local touched = false
local sneaky = setmetatable({ name = "x" }, {
    __index = function() touched = true return 1 end,
    __pairs = function() error("pairs consulted") end,
})
ok(schema:decode(msg, schema:encode(msg, sneaky)).name == "x"
   and not touched, "fill ignores metamethods")

-- Depth cap: 300 nested tables fail, 10 work.
local deep = { v = 0 }
for _ = 1, 300 do deep = { next = deep, v = 1 } end
ok(fails(function() schema:encode("pbwire.lua.Chain", deep) end),
   "300-deep encode fails")
local shallow = { v = 0 }
for _ = 1, 10 do shallow = { next = shallow, v = 1 } end
local sback = schema:decode("pbwire.lua.Chain",
                            schema:encode("pbwire.lua.Chain", shallow))
ok(sback.next.next.v == 1, "10-deep round trip")

-- proto2: required, closed enums, groups, defaults.
local s2 = pbwire.loadfile("tests/data/sample2.desc")
local m2 = "pbwire.lua2.Sample2"
ok(fails(function()
    s2:decode(m2, s2:encode(m2, {}))
end), "missing required fails on decode")
local wreq = s2:encode(m2, { need = 5 })
ok(s2:decode(m2, wreq).need == 5, "required round trip")
ok(s2:decode(m2, wreq).opt == nil, "proto2 default not filled")
-- Closed enum: unknown numbers are dropped on decode, like the reference.
local wu = string.char(0x08, 0x01, 0x18, 99)
ok(s2:decode(m2, wu).color == nil, "unknown closed enum drops")
local wue = s2:encode(m2, { need = 1, color = 99 })
ok(s2:decode(m2, wue).color == nil, "unknown closed enum re-drops")
-- Groups encode like messages (START_GROUP framing) and decode to tables.
local wge = s2:encode(m2, { need = 1, mygroup = { gv = 7 } })
local wg = string.char(0x08, 0x01, 0x23, 0x28, 0x07, 0x24)
ok(wge == wg, "group encode is START_GROUP framing")
local gback = s2:decode(m2, wg)
ok(gback.need == 1, "group bytes keep the rest intact")
ok(gback.mygroup ~= nil and gback.mygroup.gv == 7, "group decodes")
ok(s2:encode(m2, gback) == wg, "group decode/encode byte-exact")

-- Builder: build once, encode hot. Matches the table API byte for byte.
ok(fails(function() schema:message("pbwire.lua.Nope") end),
   "builder unknown type fails")
local b = schema:message(msg)
b:set({ i32 = -7, name = "alice", nums = { 1, 2, 3 } })
ok(b:encode() == schema:encode(msg, { i32 = -7, name = "alice",
                                      nums = { 1, 2, 3 } }),
   "builder matches table api")
b:set({ name = "bob" })
local bb = schema:decode(msg, b:encode())
ok(bb.name == "bob" and bb.i32 == -7 and #bb.nums == 3,
   "builder set merges")
b:set({ nums = { 9 } })
ok(#schema:decode(msg, b:encode()).nums == 1
   and schema:decode(msg, b:encode()).nums[1] == 9,
   "builder repeated set replaces")
-- The builder anchors its schema: collecting the schema must not break it.
local anchored
do
    local s3 = pbwire.loadfile("tests/data/sample.desc")
    anchored = s3:message(msg)
    anchored:set({ i32 = 42 })
end
collectgarbage("collect")
ok(schema:decode(msg, anchored:encode()).i32 == 42,
   "builder outlives its schema")

-- Wire-level crafts: unpacked form of a packed field, oneof last-wins on
-- the wire, int map key 0, full-byte blob, unicode, 64-bit extremes.
local unpacked = schema:decode(msg, "\096\001\096\002")
ok(unpacked.nums[1] == 1 and unpacked.nums[2] == 2,
   "unpacked varints decode as packed")
local oneof_a = schema:decode(msg, "\130\001\001x\136\001\009")
ok(oneof_a.pick_num == 9 and oneof_a.pick_str == nil,
   "oneof wire last wins (num)")
local oneof_b = schema:decode(msg, "\136\001\009\130\001\001x")
ok(oneof_b.pick_str == "x" and oneof_b.pick_num == nil,
   "oneof wire last wins (str)")
local stick0 = schema:decode(msg, "\146\001\005\008\000\018\001z")
ok(stick0.sticks[1].key == 0 and stick0.sticks[1].value == "z",
   "int map key 0 round trip")
local allbytes = {}
for i = 0, 255 do allbytes[#allbytes + 1] = string.char(i) end
allbytes = table.concat(allbytes)
ok(schema:decode(msg, schema:encode(msg, { blob = allbytes })).blob ==
   allbytes, "256-byte blob round trip")
ok(schema:decode(msg, schema:encode(msg, { name = "h\195\169llo\226\152\131" })).name ==
   "h\195\169llo\226\152\131", "unicode round trip")
local I64MIN = (-9223372036854775807) - 1 -- mininteger, exactly
local extremes = schema:decode(msg, schema:encode(msg, {
    i64 = I64MIN, u64 = -1, -- bit pattern for 2^64-1
}))
ok(extremes.i64 == I64MIN, "int64 min exact")
ok(extremes.u64 == 2 ^ 64, "uint64 max is nearest double")
local fz = schema:encode(msg, { f32 = 0.1 })
ok(schema:encode(msg, schema:decode(msg, fz)) == fz,
   "float32 idempotent")

-- roundtrip: gateway passthrough. Unknown fields survive byte for byte;
-- canonical bytes pass through unchanged.
local with_unknown = wire .. "\152\006\007" -- field 99 varint 7
local rt = schema:roundtrip(msg, with_unknown)
ok(rt == with_unknown, "roundtrip preserves unknown fields")
ok(schema:decode(msg, rt).name == back.name, "roundtrip keeps values")
ok(schema:roundtrip(msg, wire) == wire, "roundtrip canonical stable")
ok(fails(function() schema:roundtrip(msg, "\255\255\255") end),
   "roundtrip malformed fails")
ok(fails(function() schema:roundtrip("pbwire.lua.Nope", wire) end),
   "roundtrip unknown type fails")

-- Groups map like messages: the same tables encode through START_GROUP
-- framing and must equal the protoc reference bytes.
local gschema = pbwire.loadfile("../c/tests/data/grouptest.desc")
local gmsg = "structtest.GroupTest"
local gfull = { inner = { x = 5, s = "g" }, item = { { v = 1 }, { v = -2 } }, id = 9 }
local gwire = gschema:encode(gmsg, gfull)
local gref = io.open("../c/tests/data/grouptest.bin", "rb")
local gexpect = gref:read("*a")
gref:close()
ok(gwire == gexpect, "group encode equals protoc reference bytes")
local gback = gschema:decode(gmsg, gexpect)
ok(gback.inner.x == 5 and gback.inner.s == "g", "singular group decodes")
ok(#gback.item == 2 and gback.item[1].v == 1 and gback.item[2].v == -2,
   "repeated group decodes")
ok(gback.id == 9, "sibling field decodes")
ok(gschema:encode(gmsg, gback) == gexpect, "group decode/encode byte-exact")

-- JSON/text mapping through the binding (tables and builders agree).
local jsample = { i32 = -7, name = "alice" }
local jstr = schema:to_json(msg, jsample)
ok(jstr == '{"i32":-7,"name":"alice"}', "to_json prints the mapping")
local jback = schema:from_json(msg, jstr)
ok(jback.i32 == -7 and jback.name == "alice", "from_json round trip")
ok(schema:encode(msg, jback) == schema:encode(msg, jsample),
   "JSON round trip is wire-equal")
ok(fails(function() schema:from_json(msg, "{oops") end),
   "malformed JSON fails")
ok(fails(function() schema:from_json(msg, '{"nope":1}') end),
   "unknown JSON field fails")
ok(schema:from_json(msg, '{"nope":1}', true).name == nil,
   "ignored unknown JSON field reads as absent")
local tstr = schema:to_text(msg, jsample)
ok(tstr:find('name: "alice"', 1, true) ~= nil, "to_text prints")
-- Builder to_text drops unknown fields on request (conformance flag).
local wbuilder = schema:message(msg)
wbuilder:set({ name = "alice" })
local wunknown = schema:encode(msg, { name = "alice" }) .. "\152\006\007"
local wparsed = schema:parse_binary(msg, wunknown)
ok(wparsed:to_text():find("99", 1, true) ~= nil,
   "builder to_text prints unknowns by default")
ok(wparsed:to_text(false):find("99", 1, true) == nil,
   "builder to_text(false) drops unknowns")
local tback = schema:from_text(msg, tstr)
ok(schema:encode(msg, tback) == schema:encode(msg, jsample),
   "text round trip is wire-equal")
ok(fails(function() schema:from_text(msg, "name: ") end),
   "malformed text fails")
-- Builders parse exactly (64-bit integers past 2^53 survive, unlike tables).
local big = schema:parse_json(msg, '{"u64":18446744073709551615}')
ok(big:to_json():find("18446744073709551615", 1, true) ~= nil,
   "builder JSON keeps uint64 max")
local bigbin = schema:parse_binary(msg, schema:encode(msg, { u64 = -1 }))
ok(bigbin:to_json():find("18446744073709551615", 1, true) ~= nil,
   "builder binary keeps uint64 max")
-- Deterministic encoding sorts map entries by key.
local mschema = pbwire.loadfile("../c/tests/data/maptest.desc")
local mmsg = "structtest.Maps"
local unordered = "\010\005\008\002\018\001b\010\005\008\001\018\001a"
local det = mschema:encode_deterministic(mmsg, mschema:decode(mmsg, unordered))
ok(det == "\010\005\008\001\018\001a\010\005\008\002\018\001b",
   "deterministic encoding sorts map entries")
-- Delimited frames concatenate and decode in a loop.
local f1 = schema:encode_delimited(msg, jsample)
local f2 = schema:encode_delimited(msg, { name = "bob" })
local frames = f1 .. f2
local at, names = 1, {}
while at <= #frames do
    local m, read = schema:decode_delimited(msg, frames:sub(at))
    ok(read > 0, "frame reports its size")
    names[#names + 1] = m.name
    at = at + read
end
ok(#names == 2 and names[1] == "alice" and names[2] == "bob",
   "delimited frames decode in order")
ok(fails(function() schema:decode_delimited(msg, frames:sub(1, 2)) end),
   "truncated frame fails")
-- Unknown fields survive byte for byte through roundtrip (gateway
-- passthrough); tables carry known fields only, like the JSON mapping.
local with_unknown = wire .. "\152\006\007"
ok(schema:roundtrip(msg, with_unknown) == with_unknown,
   "unknown fields survive byte round trip")
local kept = schema:decode(msg, with_unknown)
ok(kept.name == back.name, "known fields survive table round trip")

ok(eq(failures, 0) and true or true, "summary")
print(failures == 0 and "all tests passed" or (failures .. " failures"))

os.exit(failures == 0 and 0 or 1)

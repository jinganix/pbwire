-- Runnable round trip through the Lua binding (uses the checked-in
-- descriptor set, so no protoc is needed):
--
--   lua examples/roundtrip.lua   (from bindings/lua, after ./build.sh)

package.cpath = "./?.so;" .. package.cpath

local pbwire = require("pbwire")
local schema = pbwire.loadfile("tests/data/sample.desc")

local wire = schema:encode("pbwire.lua.Sample", {
    i32 = -7, name = "alice", color = 1,
    inner = { s = "nested" }, nums = { 1, 2, 3 },
})
local back = schema:decode("pbwire.lua.Sample", wire)

assert(back.name == "alice" and back.i32 == -7)
assert(back.inner.s == "nested" and #back.nums == 3)
assert(schema:encode("pbwire.lua.Sample", back) == wire)
print(string.format("round trip ok (%d bytes)", #wire))

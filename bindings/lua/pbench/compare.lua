-- Provisional interleaved timing of the two Lua pbench arms against
-- lua-protobuf: one round runs every op back to back per tier, five rounds,
-- median reported.
--
--   lua bindings/lua/pbench/compare.lua
--
-- The ops are whole user tasks with a fresh native table per encode
-- iteration (no prebuilt table before the timer): `tree` encode builds a
-- fresh builder per call (message + set + encode) and `tree` decode parses
-- plus reads fully back (to_json, folded into a sink); `shim` encode/decode
-- is one C entry with the table in or out. `proxy` is the prebuilt task
-- (held-builder re-encode, roundtrip with no materialization): published,
-- never the headline. The formal, recorded run is `make lua-bench`.

local dir = (arg and arg[0] and arg[0]:match("^(.*)/[^/]*$")) or "."
package.path = dir .. "/?.lua;" .. package.path

local payload = require("payload")
local tree = require("a-tree")
local shim = require("b-shim")

local ROUNDS = 5
local PASS_SECONDS = 0.2

local has_pb, pb = pcall(require, "pb")
local pb_loaded = false
if has_pb then
    local f = io.open(dir .. "/../../c/tests/data/matrix3.desc", "rb")
    if f ~= nil then
        local desc = f:read("*a")
        f:close()
        pb_loaded = pb.load(desc) and true or false
    end
end

local function calibrate(op)
    local count = 1
    while true do
        local t0 = os.clock()
        for _ = 1, count do op() end
        if os.clock() - t0 >= PASS_SECONDS or count >= 100000000 then
            return count
        end
        count = count * 2
    end
end

local function pass_ns(op, count)
    local t0 = os.clock()
    for _ = 1, count do op() end
    return (os.clock() - t0) / count * 1e9
end

local function median(values)
    table.sort(values)
    return values[math.floor(#values / 2) + 1]
end

local function row(impl, op, bytes, ns)
    print(string.format("  %-16s %-7s %12.1f %10.1f",
                        impl, op, ns, bytes / ns * 1000))
end

for _, tier in ipairs(payload.TIERS) do
    local label, repeats, bytes = tier.label, tier.repeats, tier.bytes
    local wire = shim.marshal(payload.fresh(repeats))
    assert(#wire == bytes, label .. ": gate bytes moved")

    -- The prebuilt proxy input: built once outside the timer (game
    -- broadcast), so proxy encode is one core walk plus one buffer malloc.
    local held = tree.prepare(payload.fresh(repeats))
    assert(tree.encode_of(held) == wire, label .. ": builder mismatch")

    local sink = { n = 0 }
    local ops = {
        { "pbwire-lua-tree", "encode", function()
            sink.n = sink.n + #tree.marshal(payload.fresh(repeats))
        end },
        { "pbwire-lua-tree", "decode", function()
            sink.n = sink.n + #tree.unmarshal(wire)
        end },
        { "pbwire-lua-shim", "encode", function()
            sink.n = sink.n + #shim.marshal(payload.fresh(repeats))
        end },
        { "pbwire-lua-shim", "decode", function()
            local back = shim.unmarshal(wire)
            sink.n = sink.n + #back.str + #back.nums
        end },
        { "pbwire-lua-proxy", "encode", function()
            sink.n = sink.n + #tree.encode_of(held)
        end },
        { "pbwire-lua-proxy", "decode", function()
            sink.n = sink.n + #payload.schema():roundtrip(payload.MSG, wire)
        end },
    }
    if pb_loaded then
        ops[#ops + 1] = { "lua-protobuf", "encode", function()
            sink.n = sink.n + #pb.encode(payload.MSG, payload.fresh(repeats))
        end }
        ops[#ops + 1] = { "lua-protobuf", "decode", function()
            sink.n = sink.n + #pb.decode(payload.MSG, wire).str
        end }
    end

    local counts = {}
    for i, op in ipairs(ops) do counts[i] = calibrate(op[3]) end
    local samples = {}
    for i = 1, #ops do samples[i] = {} end
    for _ = 1, ROUNDS do
        for i, op in ipairs(ops) do
            samples[i][#samples[i] + 1] = pass_ns(op[3], counts[i])
        end
    end

    print(string.format("%s: %d bytes (repeat field x%d)",
                        label, bytes, repeats))
    print("  impl               op            ns/op       MB/s")
    for i, op in ipairs(ops) do
        row(op[1], op[2], bytes, median(samples[i]))
    end
    print(string.format("  # sink: %d chars folded back", sink.n))
    print()
end

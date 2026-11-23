-- The pbench gate: no arm is timed before it reproduces the canonical
-- bytes on all four tiers and reads its own decode back field by field.
-- Runs first and stays runnable:
--
--   lua bindings/lua/pbench/gate.lua
--
-- Writes 'gate: PASS' and exits 0, or names the failing arm/tier and exits 1.

local dir = (arg and arg[0] and arg[0]:match("^(.*)/[^/]*$")) or "."
package.path = dir .. "/?.lua;" .. package.path

local payload = require("payload")
local tree = require("a-tree")
local shim = require("b-shim")

local failures = 0

local function fail(what)
    print("FAIL " .. what)
    failures = failures + 1
end

-- Varint tag bytes for field `fnum`, wire type `wt`.
local function tag(fnum, wt)
    local v = fnum * 8 + wt
    local out = {}
    while v >= 128 do
        out[#out + 1] = string.char(v % 128 + 128)
        v = math.floor(v / 128)
    end
    out[#out + 1] = string.char(v)
    return table.concat(out)
end

local schema = payload.schema()

for _, tier in ipairs(payload.TIERS) do
    local label, repeats, bytes = tier.label, tier.repeats, tier.bytes
    local fix = payload.fresh(repeats)

    -- Both arms encode the fixed payload to identical canonical bytes.
    local a_wire = tree.marshal(fix)
    local b_wire = shim.marshal(fix)
    if #a_wire ~= bytes then
        fail(label .. ": tree wrote " .. #a_wire .. " bytes, want " .. bytes)
    elseif #b_wire ~= bytes then
        fail(label .. ": shim wrote " .. #b_wire .. " bytes, want " .. bytes)
    elseif a_wire ~= b_wire then
        fail(label .. ": tree and shim bytes differ")
    else
        print("OK   " .. label .. " tree/shim bytes (" .. bytes .. " bytes)")
    end
    local wire = a_wire

    -- Shim decode: field-by-field, then re-encode. The u64 max decodes
    -- to the nearest double by design (README value mapping), so it is
    -- normalized back to the -1 bit pattern before the byte compare; the
    -- approximation itself is pinned by reads_ok (== 2^64).
    local back = shim.unmarshal(wire)
    if not payload.reads_ok(back, repeats) then
        fail(label .. ": shim decode differs")
    else
        back.u64 = -1
        if shim.marshal(back) ~= wire then
            fail(label .. ": shim re-encode differs")
        else
            print("OK   " .. label .. " shim decode and re-encode")
        end
    end

    -- Tree decode: the full read-back (JSON) matches the source tree's
    -- own print, and the parsed builder re-encodes to the same bytes.
    local ref_json = tree.prepare(fix):to_json()
    if tree.unmarshal(wire) ~= ref_json then
        fail(label .. ": tree decode does not print like the source tree")
    elseif tree.encode_of(tree.parse(wire)) ~= wire then
        fail(label .. ": tree re-encode differs")
    else
        print("OK   " .. label .. " tree decode and re-encode")
    end

    -- Unknown fields: field 999 (len-delim "ABC") after the known bytes,
    -- top level and nested inside `inner`. The builder path preserves them
    -- (re-emitted after known fields in input order); the table path is
    -- canonical by design (unknowns skipped on decode); roundtrip preserves.
    local suffix = tag(999, 2) .. "\003ABC"
    do
        local top_unk = wire .. suffix
        local top_back = shim.unmarshal(top_unk)
        top_back.u64 = -1 -- see above: the documented u64 approximation
        if tree.encode_of(tree.parse(top_unk)) ~= top_unk then
            fail(label .. ": tree drops top-level unknown field 999")
        elseif shim.marshal(top_back) ~= wire then
            fail(label .. ": table path not canonical over unknown field 999")
        elseif schema:roundtrip(payload.MSG, top_unk) ~= top_unk then
            fail(label .. ": roundtrip drops top-level unknown field 999")
        else
            print("OK   " .. label .. " unknown field 999 (top level)")
        end
    end
    do
        -- The known inner-field encoding (tag 16 + length + payload) occurs
        -- once: splice the unknown field at the end of the inner payload and
        -- bump its length prefix (plain byte search, no patterns).
        local inner_field = schema:encode(payload.MSG,
                                          { inner = { name = "kid",
                                                      value = 42 } })
        local at = wire:find(inner_field, 1, true)
        if at == nil then
            fail(label .. ": nested-unknown construction missed inner bytes")
        else
            local before = wire:sub(1, at - 1)
            local after = wire:sub(at + #inner_field)
            -- inner_field = tag(16,2) .. lenprefix .. innerpayload: the
            -- tag is 2 bytes (130 -> 0x82 0x01), the rest is lenprefix +
            -- payload. Split them by decoding the length varint.
            local pos = 3
            local len = 0
            local shift = 0
            while true do
                local b = inner_field:byte(pos)
                len = len + (b % 128) * (2 ^ shift)
                pos = pos + 1
                if b < 128 then break end
                shift = shift + 7
            end
            local innerpayload = inner_field:sub(pos)
            assert(#innerpayload == len)
            local newpayload = innerpayload .. suffix
            local v = #newpayload
            local newlen = {}
            while v >= 128 do
                newlen[#newlen + 1] = string.char(v % 128 + 128)
                v = math.floor(v / 128)
            end
            newlen[#newlen + 1] = string.char(v)
            local nested_unk = before .. inner_field:sub(1, 2) ..
                               table.concat(newlen) .. newpayload .. after
            local nested_back = shim.unmarshal(nested_unk)
            nested_back.u64 = -1 -- see above
            if tree.encode_of(tree.parse(nested_unk)) ~= nested_unk then
                fail(label .. ": tree drops nested unknown field 999")
            elseif shim.marshal(nested_back) ~= wire then
                fail(label .. ": table path not canonical over nested unknown 999")
            else
                print("OK   " .. label .. " unknown field 999 (nested)")
            end
        end
    end
end

-- Baseline cross-check (informational): lua-protobuf writes nested messages
-- in table hash order, so raw bytes are not comparable; it only has to agree
-- on size and survive a decode spot-check, like benchmarks/bench.lua.
do
    local has_pb, pb = pcall(require, "pb")
    if has_pb then
        local f = io.open(dir .. "/../../c/tests/data/matrix3.desc", "rb")
        if f == nil then
            fail("baseline: cannot open matrix3.desc")
        else
            local desc = f:read("*a")
            f:close()
            if pb.load(desc) then
                for _, tier in ipairs(payload.TIERS) do
                    local fix = payload.fresh(tier.repeats)
                    local bw = pb.encode(payload.MSG, fix)
                    if #bw ~= tier.bytes then
                        fail(tier.label .. ": baseline size " .. #bw
                             .. ", want " .. tier.bytes)
                    else
                        local pbback = pb.decode(payload.MSG, bw)
                        if pbback.str ~= "hello" then
                            fail(tier.label .. ": baseline decode differs")
                        else
                            print("OK   " .. tier.label .. " baseline size+decode")
                        end
                    end
                end
            else
                fail("baseline: pb.load(matrix3.desc) failed")
            end
        end
    else
        print("--   baseline lua-protobuf not installed: size cross-check skipped")
    end
end

if failures ~= 0 then
    print("\n" .. failures .. " gate check(s) failed")
    os.exit(1)
end
print("\ngate: PASS")

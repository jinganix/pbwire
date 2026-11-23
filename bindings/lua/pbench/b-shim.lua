-- The `shim` arm: one C entry per op. `schema:encode` hands the native
-- table to C, which walks it against the schema's prepared field table and
-- writes straight into the wire buffer (no tree, no per-value allocation);
-- `schema:decode` validates and materializes straight into a fresh table.
-- No per-field set()/get() round trip either way.
--
--   shim.marshal(obj)   -> wire bytes (one C entry, table in)
--   shim.unmarshal(wire)-> a fresh Lua table (one C entry, wire in;
--                          documented-eager: the C walk in pbwlua_push_message
--                          materializes every field -- repeated tables and
--                          nested messages included -- before it returns, so
--                          the full table already exists when the timer drops
--                          the reference)

local payload = require("payload")

local MSG = payload.MSG

local shim = {}
shim.name = "b-shim"

function shim.marshal(obj)
    return payload.schema():encode(MSG, obj)
end

function shim.unmarshal(wire)
    return payload.schema():decode(MSG, wire)
end

return shim

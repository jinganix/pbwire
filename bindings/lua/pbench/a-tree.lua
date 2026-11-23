-- The `tree` arm: the shipped per-field full task through the builder
-- handle (the shipped runtime). No prebuilt tree before the timer.
--
--   tree.marshal(obj)   -> wire bytes through a fresh builder per call
--                          (message + set + encode: the per-field convert
--                          fills a malloc tree in C, then the core walks it)
--   tree.unmarshal(wire) -> the full read-back as JSON text (the shipped
--                          read-back path: builders have no per-field getters
--                          by design, so parse + to_json IS the whole task,
--                          the same split the R arm documents)
--   tree.prepare(obj)   -> a fresh builder (the encode half of the timer)
--   tree.encode_of(msg) -> wire bytes of a held builder (the proxy op)
--   tree.parse(wire)    -> a builder tree (the decode half of the timer)
--
-- The Lua table feeding the encode is app-owned steady-state input built
-- fresh per iteration (the honesty rule: no prebuilt table before the
-- timer). No manual free exists on this path: the builder is collected by
-- Lua's GC (same note as the Python/Ruby arms).

local payload = require("payload")

local MSG = payload.MSG

local tree = {}
tree.name = "a-tree"

function tree.prepare(obj)
    local msg = payload.schema():message(MSG)
    msg:set(obj)
    return msg
end

function tree.encode_of(msg)
    return msg:encode()
end

function tree.marshal(obj)
    return tree.encode_of(tree.prepare(obj))
end

function tree.parse(wire)
    return payload.schema():parse_binary(MSG, wire)
end

function tree.unmarshal(wire)
    return tree.parse(wire):to_json()
end

return tree

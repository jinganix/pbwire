#!/usr/bin/env lua
-- Google conformance testee driven by the pbwire Lua binding.
--
-- Speaks the runner's wire protocol (4-byte little-endian length prefix,
-- then a ConformanceRequest / ConformanceResponse) and, because the binding
-- is schema-driven, needs no per-message code: the request names the message
-- type and the descriptor set answers with the schema.
--
-- Supported: binary in -> binary out, through schema:roundtrip(), which
-- decodes and re-encodes in the C core: unknown fields survive, and a request
-- is accepted exactly when the core accepts it. JSON/text output needs
-- printers the binding does not expose, so those requests answer `skipped`.
--
--   lua testee.lua build/conformance.desc   # from the repo root
--   LUA=luajit lua testee.lua ...           # any 5.1+ interpreter works

-- The module lives one directory up from this script, whatever the
-- working directory is (the runner execs an absolute path).
do
    local src = debug.getinfo(1, "S").source:gsub("^@", "")
    local dir = src:match("^(.*/)") or "./"
    package.cpath = dir .. "../?.so;" .. package.cpath
end

local pbwire = require("pbwire")

local PROTOBUF = 1

if not arg[1] then
    io.stderr:write("usage: testee.lua <conformance.desc>\n")
    os.exit(2)
end
local schema = pbwire.loadfile(arg[1])

local function read_n(stream, n)
    local chunks = {}
    local got = 0
    while got < n do
        local piece = stream:read(n - got)
        if not piece or #piece == 0 then
            return nil
        end
        got = got + #piece
        chunks[#chunks + 1] = piece
    end
    return table.concat(chunks)
end

local function read_u32le(s)
    local a, b, c, d = s:byte(1, 4)
    return a + b * 256 + c * 65536 + d * 16777216
end

local function write_u32le(n)
    return string.char(n % 256, math.floor(n / 256) % 256,
                       math.floor(n / 65536) % 256,
                       math.floor(n / 16777216) % 256)
end

local function read_request(stream)
    local header = read_n(stream, 4)
    if not header then
        return nil
    end
    return read_n(stream, read_u32le(header))
end

local function write_response(stream, payload)
    stream:write(write_u32le(#payload))
    stream:write(payload)
    stream:flush()
end

io.stdin:setvbuf("no")
io.stdout:setvbuf("no")

while true do
    local body = read_request(io.stdin)
    if not body then
        break
    end

    local response
    local ok, request = pcall(schema.decode, schema,
                              "conformance.ConformanceRequest", body)
    if not ok then
        response = { runtime_error = "cannot decode request" }
    else
        local wanted = request.requested_output_format or 0
        local name = request.message_type
        if wanted ~= PROTOBUF then
            response = { skipped = "output format " .. tostring(wanted) ..
                                    " is not supported" }
        elseif request.protobuf_payload == nil then
            response = { skipped = "only binary input is supported" }
        else
            local ok2, out = pcall(schema.roundtrip, schema, name,
                                   request.protobuf_payload)
            if ok2 then
                response = { protobuf_payload = out }
            elseif out:match("^pbwire: no message ") then
                response = { runtime_error = "unknown message type " .. name }
            else
                response = { parse_error = out }
            end
        end
    end

    local ok3, payload = pcall(schema.encode, schema,
                               "conformance.ConformanceResponse", response)
    if not ok3 then
        payload = schema:encode("conformance.ConformanceResponse",
                                { runtime_error = "cannot encode response" })
    end
    write_response(io.stdout, payload)
end

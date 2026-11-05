#!/usr/bin/env lua
-- Google conformance testee driven by the pbwire Lua binding.
--
-- Speaks the runner's wire protocol (4-byte little-endian length prefix,
-- then a ConformanceRequest / ConformanceResponse) and, because the binding
-- is schema-driven, needs no per-message code: the request names the message
-- type and the descriptor set answers with the schema.
--
-- Supported: binary, JSON and text in -> binary, JSON or text out, all
-- through builder trees (exact, including 64-bit integers past 2^53 that
-- tables carry as doubles): binary parses with parse_binary, JSON with
-- parse_json, text with parse_text, and every output encodes or prints the
-- same tree. Unknown fields survive binary (the core keeps them); JSON
-- drops them per the mapping. JSPB answers `skipped`.
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
local JSON = 2
local JSPB = 3
local TEXT = 4

-- Test categories the runner sends: JSON input with unknown fields only
-- parses when the category asks to ignore them.
local JSON_IGNORE_UNKNOWN = 3

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
        if wanted ~= PROTOBUF and wanted ~= JSON and wanted ~= TEXT then
            response = { skipped = "output format " .. tostring(wanted) ..
                                    " is not supported" }
        else
            -- One input payload wins: binary beats JSON beats text. Every
            -- input parses into a builder tree; every output renders it.
            local t, perr
            if request.protobuf_payload ~= nil then
                local ok2, b = pcall(schema.parse_binary, schema, name,
                                      request.protobuf_payload)
                if ok2 then
                    t = b
                else
                    perr = b
                end
            elseif request.json_payload ~= nil then
                local ignore = request.test_category == JSON_IGNORE_UNKNOWN
                local ok2, b = pcall(schema.parse_json, schema, name,
                                      request.json_payload, ignore)
                if ok2 then
                    t = b
                else
                    perr = b
                end
            elseif request.text_payload ~= nil then
                local ok2, b = pcall(schema.parse_text, schema, name,
                                      request.text_payload)
                if ok2 then
                    t = b
                else
                    perr = b
                end
            else
                perr = "pbwire: no input payload in the request"
            end
            if t ~= nil then
                if wanted == PROTOBUF then
                    local ok3, out = pcall(function() return t:encode() end)
                    if ok3 then
                        response = { protobuf_payload = out }
                    else
                        response = { serialize_error = out }
                    end
                elseif wanted == JSON then
                    local ok3, out = pcall(function() return t:to_json() end)
                    if ok3 then
                        response = { json_payload = out }
                    else
                        response = { serialize_error = out }
                    end
                else
                    -- The runner's print_unknown_fields flag (field 9) decides
                    -- whether unknown fields print; absent means drop them.
                    local ok3, out = pcall(function()
                        return t:to_text(request.print_unknown_fields)
                    end)
                    if ok3 then
                        response = { text_payload = out }
                    else
                        response = { serialize_error = out }
                    end
                end
            elseif response == nil then
                if perr ~= nil and perr:match("^pbwire: no message ") then
                    response = { runtime_error = "unknown message type " .. name }
                else
                    response = { parse_error = perr or "unreachable" }
                end
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

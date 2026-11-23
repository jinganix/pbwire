# Google conformance testee driven by the pure-Julia core.
#
# Same wire protocol as the `ccall` testee (4-byte little-endian length prefix,
# then a ConformanceRequest / ConformanceResponse) and the same schema-driven
# shape: the request names the message type and the descriptor set answers with
# the schema. Nothing here loads the native library: the whole codec is
# `bindings/julia/runtime/src/PbwireCore.jl`.
#
# Binary in -> binary out is what this core implements. JSON and text requests
# answer `skipped` (the runner does not count a skip as a failure), so the pass
# set is the binary suite.
#
#   julia --project=bindings/julia/runtime \
#       bindings/julia/conformance/testee_core.jl build/conformance.desc

include(joinpath(@__DIR__, "..", "runtime", "src", "PbwireCore.jl"))
using .PbwireCore

const PROTOBUF = 1

function read_request(io::IO)
    header = read(io, 4)
    length(header) < 4 && return nothing
    size = Int(header[1]) | (Int(header[2]) << 8) | (Int(header[3]) << 16) |
           (Int(header[4]) << 24)
    body = read(io, size)
    length(body) < size && return nothing
    return body
end

function write_response(io::IO, payload::Vector{UInt8})
    n = length(payload)
    write(io, UInt8[n & 0xff, (n >> 8) & 0xff, (n >> 16) & 0xff, (n >> 24) & 0xff])
    write(io, payload)
    flush(io)
    return nothing
end

_errmsg(e) = e isa CoreError ? e.msg : sprint(showerror, e)

function respond(ty::MsgType, fields::Dict{String,Any})
    try
        m = from_dict(ty, fields)
        return encode(m)
    catch e
        # The response must always be sendable: report the failure in the
        # runtime_error case rather than dying on it.
        try
            m = from_dict(ty, Dict{String,Any}(
                "runtime_error" => "cannot encode response: $(_errmsg(e))"))
            return encode(m)
        catch
            return UInt8[]
        end
    end
end

function serve(schema::Schema, request_ty::MsgType, response_ty::MsgType,
        body::Vector{UInt8})
    local request::Message
    try
        request = decode(request_ty, body)
    catch e
        return respond(response_ty, Dict{String,Any}(
            "runtime_error" => "cannot parse ConformanceRequest: $(_errmsg(e))"))
    end
    wanted = has(request, "requested_output_format") ?
        get_int(request, "requested_output_format") : Int64(0)
    if wanted != PROTOBUF
        # No JSON or text printer in this core: let the runner skip.
        return respond(response_ty, Dict{String,Any}(
            "skipped" => "output format $wanted is not supported"))
    end
    if has(request, "json_payload") || has(request, "text_payload")
        return respond(response_ty, Dict{String,Any}(
            "skipped" => "JSON and text input are not supported by the pure core"))
    end
    if !has(request, "protobuf_payload")
        return respond(response_ty, Dict{String,Any}("skipped" => "no binary payload"))
    end
    name = has(request, "message_type") ? get_string(request, "message_type") : ""
    local ty::MsgType
    try
        ty = msgtype(schema, name)
    catch e
        return respond(response_ty, Dict{String,Any}(
            "runtime_error" => "unknown message type $(repr(name))"))
    end
    local message::Message
    try
        message = decode(ty, get_bytes(request, "protobuf_payload"))
    catch e
        return respond(response_ty,
            Dict{String,Any}("parse_error" => _errmsg(e)))
    end
    local payload::Vector{UInt8}
    try
        payload = encode(message)
    catch e
        return respond(response_ty,
            Dict{String,Any}("serialize_error" => _errmsg(e)))
    end
    return respond(response_ty, Dict{String,Any}("protobuf_payload" => payload))
end

function main(args::Vector{String})
    if isempty(args)
        println(stderr, "usage: testee_core.jl <conformance.desc>")
        exit(2)
    end
    schema = from_file(args[1])
    request_ty = msgtype(schema, "conformance.ConformanceRequest")
    response_ty = msgtype(schema, "conformance.ConformanceResponse")
    # Framed, one request at a time: never slurp stdin, the runner keeps the
    # pipe open while it waits for each response.
    while !eof(stdin)
        body = read_request(stdin)
        body === nothing && break
        write_response(stdout, serve(schema, request_ty, response_ty, body))
    end
    return nothing
end

main(ARGS)

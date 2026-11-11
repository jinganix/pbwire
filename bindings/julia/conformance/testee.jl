# Google conformance testee driven by the pbwire Julia binding.
#
# It speaks the runner's wire protocol (4-byte little-endian length prefix,
# then a ConformanceRequest / ConformanceResponse) and, because the binding is
# schema-driven, it needs no per-message code: the request names the message
# type, the descriptor set answers with the schema. Every field is addressed
# by name through the binding, so there is no wire code and no `.proto`
# parsing here either.
#
# Supported: binary, JSON and text in -> binary, JSON or text out. JSPB
# answers `skipped`, like every other testee here.
#
# `decode` walks and validates the whole payload before returning, and unknown
# fields are preserved and re-encoded byte-exactly, which is why this testee
# has no expected failures on the binary suite.
#
#   julia --project=runtime conformance/testee.jl build/conformance.desc

push!(LOAD_PATH, joinpath(@__DIR__, "..", "runtime"))
using Pbwire

const PROTOBUF = 1
const JSON_IN = 2
const JSPB = 3
const TEXT = 4
const JSON_IGNORE_UNKNOWN = 3

function read_request(io::IO)
    header = read(io, 4)
    if length(header) < 4
        return nothing
    end
    size = Int(header[1]) | (Int(header[2]) << 8) | (Int(header[3]) << 16) |
           (Int(header[4]) << 24)
    body = read(io, size)
    if length(body) < size
        return nothing
    end
    return body
end

function write_response(io::IO, payload::Vector{UInt8})
    n = length(payload)
    write(io, UInt8[n & 0xff, (n >> 8) & 0xff, (n >> 16) & 0xff, (n >> 24) & 0xff])
    write(io, payload)
    flush(io)
    return nothing
end

# First field name that exists on the response wins; the suite only needs one.
function set_first!(m::Message, names::Vector{String}, v)
    for name in names
        try
            f = field(m.ty, name)
            if f.type == 12 # bytes
                set_bytes!(m, f, v isa Vector{UInt8} ? v : Vector{UInt8}(codeunits(String(v))))
            else
                set_string!(m, f, String(v))
            end
            return true
        catch e
            e isa PbwireError || rethrow()
        end
    end
    return false
end

function handle(schema::Schema, body::Vector{UInt8})
    req_ty = msgtype(schema, "conformance.ConformanceRequest")
    res_ty = msgtype(schema, "conformance.ConformanceResponse")
    res = new_message(res_ty)
    try
        req = decode(req_ty, body)
        try
            want = has(req, "requested_output_format") ?
                Int(get_int(req, "requested_output_format")) : PROTOBUF
            category = has(req, "test_category") ?
                Int(get_int(req, "test_category")) : 0
            if want != PROTOBUF && want != JSON_IN && want != TEXT
                set_first!(res, ["skipped"], "output format $want is not supported")
                return encode(res)
            end
            if !has(req, "message_type")
                set_first!(res, ["runtime_error"], "request has no message_type")
                return encode(res)
            end
            ty = msgtype(schema, get_string(req, "message_type"))
            local parsed::Message
            try
                if has(req, "json_payload")
                    parsed = parse_json(ty, get_string(req, "json_payload"),
                        category == JSON_IGNORE_UNKNOWN)
                elseif has(req, "text_payload")
                    parsed = parse_text(ty, get_string(req, "text_payload"))
                elseif has(req, "protobuf_payload")
                    parsed = decode(ty, get_bytes(req, "protobuf_payload"))
                else
                    set_first!(res, ["skipped"], "unsupported payload")
                    return encode(res)
                end
            catch e
                set_first!(res, ["parse_error", "runtime_error"],
                    e isa PbwireError ? e.message : sprint(showerror, e))
                return encode(res)
            end
            local out::Union{Vector{UInt8},String}
            try
                if want == JSON_IN
                    out = print_json(parsed)
                elseif want == TEXT
                    out = print_text(parsed,
                        has(req, "print_unknown_fields") &&
                        get_int(req, "print_unknown_fields") != 0)
                else
                    out = encode(parsed)
                end
                close(parsed)
            catch e
                set_first!(res, ["serialize_error", "runtime_error"],
                    e isa PbwireError ? e.message : sprint(showerror, e))
                return encode(res)
            end
            name = want == JSON_IN ? "json_payload" :
                   want == TEXT ? "text_payload" : "protobuf_payload"
            set_first!(res, [name], out)
            return encode(res)
        finally
            close(req)
        end
    catch e
        set_first!(res, ["runtime_error", "skipped"],
            e isa PbwireError ? e.message : sprint(showerror, e))
        return encode(res)
    finally
        close(res)
    end
end

function main(args::Vector{String})
    if isempty(args)
        println(stderr, "usage: testee.jl <conformance.desc>")
        exit(2)
    end
    schema = from_file(args[1])
    # Framed, one request at a time: never slurp stdin, the runner keeps the
    # pipe open while it waits for each response.
    while !eof(stdin)
        body = read_request(stdin)
        body === nothing && break
        write_response(stdout, handle(schema, body))
    end
    close(schema)
    return nothing
end

main(ARGS)

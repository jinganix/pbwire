// Google conformance testee driven by the pure-C# core (bindings/csharp/Pbwire/Core).
//
// Same wire protocol as the F# testee (4-byte little-endian length prefix,
// then a ConformanceRequest / ConformanceResponse) and the same schema-driven
// shape: the request names the message type and the descriptor set answers
// with the schema. Nothing here touches libpbwire.
//
// Binary in -> binary out is what this core implements. JSON and text
// requests answer `skipped` (the runner does not count a skip as a failure),
// so the pass set is the binary suite, not the JSON/text mappings.
//
//   dotnet Testee.dll build/conformance.desc

using Pbwire.Core;

if (args.Length < 1)
{
    Console.Error.WriteLine("usage: Testee <conformance.desc>");
    return 2;
}

// ConformanceRequest.requested_output_format values.
const int protobuf = 1;

var schema = Schema.FromFile(args[0]);
var requestTy = schema.Message("conformance.ConformanceRequest");
var responseTy = schema.Message("conformance.ConformanceResponse");

using var stdin = Console.OpenStandardInput();
using var stdout = Console.OpenStandardOutput();

var body = ReadRequest(stdin);
while (body is not null)
{
    WriteResponse(stdout, Handle(body));
    body = ReadRequest(stdin);
}
return 0;

byte[] Handle(byte[] body)
{
    Msg request;
    try
    {
        request = requestTy.Decode(body);
    }
    catch (CoreException e)
    {
        return Answer("runtime_error", "cannot parse ConformanceRequest: " + e.Message);
    }

    var wanted = request.Get("requested_output_format") is int f ? f : 0;
    if (wanted != protobuf)
    {
        // No JSON or text printer in this core: let the runner skip.
        return Answer("skipped", "output format " + wanted + " is not supported");
    }
    if (request.Has("json_payload") || request.Has("text_payload"))
    {
        return Answer("skipped", "JSON and text input are not supported by the pure C# core");
    }
    if (request.Get("protobuf_payload") is not byte[] payload)
    {
        return Answer("skipped", "no binary payload");
    }
    MessageType type;
    try
    {
        type = schema.Message(request.Get("message_type") as string ?? "");
    }
    catch (CoreException)
    {
        return Answer("runtime_error", "unknown message type '" + request.Get("message_type") + "'");
    }

    Msg parsed;
    try
    {
        parsed = type.Decode(payload);
    }
    catch (CoreException e)
    {
        return Answer("parse_error", e.Message);
    }
    try
    {
        return Answer("protobuf_payload", type.Encode(parsed));
    }
    catch (CoreException e)
    {
        return Answer("serialize_error", e.Message);
    }
}

byte[] Answer(string field, object value)
{
    var response = responseTy.NewMessage();
    response.Set(field, value);
    return responseTy.Encode(response);
}

static byte[]? ReadRequest(Stream stdin)
{
    var header = new byte[4];
    if (!ReadExactly(stdin, header, 4))
    {
        return null; // clean end of input (or a truncated header)
    }
    var size = header[0] | (header[1] << 8) | (header[2] << 16) | (header[3] << 24);
    var body = new byte[size];
    if (!ReadExactly(stdin, body, size))
    {
        throw new InvalidOperationException("truncated conformance request");
    }
    return body;
}

static bool ReadExactly(Stream stream, byte[] buf, int count)
{
    var got = 0;
    while (got < count)
    {
        var n = stream.Read(buf, got, count - got);
        if (n == 0)
        {
            return false;
        }
        got += n;
    }
    return true;
}

static void WriteResponse(Stream stdout, byte[] payload)
{
    var len = payload.Length;
    var header = new byte[]
    {
        (byte)len, (byte)(len >> 8), (byte)(len >> 16), (byte)(len >> 24),
    };
    stdout.Write(header, 0, 4);
    stdout.Write(payload, 0, len);
    stdout.Flush();
}

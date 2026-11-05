// Google conformance testee driven by the F# binding's dynamic API.
//
// Part of the repo-wide conformance story: `conformance/testee.c` proves the
// dynamic core, this program proves the F# facade on the same requests (and,
// through it, the C# binding it wraps).
//
// It speaks the runner's wire protocol (4-byte little-endian length prefix,
// then a ConformanceRequest / ConformanceResponse) and, because the binding is
// schema-driven, it needs no per-message code: the request names the message
// type, the descriptor set answers with the schema.
//
// Supported: binary in -> binary out. JSON and text output need printers the
// binding does not expose, so those requests answer `skipped` and the runner
// reports them as skipped rather than failed.
//
// `parse` walks and validates the whole payload before returning, and a parsed
// message keeps the bytes it was parsed from, so `encode` hands them back: a
// round trip preserves unknown fields and repeated fields in wire order.
//
//   dotnet run --project conformance -- build/conformance.desc   # from bindings/fsharp

module Testee

open System
open System.Collections.Generic
open System.IO
open Pbwire
open Pbwire.FSharp

// ConformanceRequest.requested_output_format values.
let protobuf = 1

// ConformanceResponse field numbers: exactly one result field is set.
let parseError = "parse_error"
let runtimeError = "runtime_error"
let protobufPayload = "protobuf_payload"
let skipped = "skipped"
let serializeError = "serialize_error"

let types = Dictionary<string, MessageType>()

let messageOf (schema: Schema) (name: string) : MessageType =
    match types.TryGetValue name with
    | true, ty -> ty
    | _ ->
        let ty = Pbwire.messageType schema name
        types[name] <- ty
        ty

let answer (responseTy: MessageType) (field: string) (text: string) : byte[] =
    use response = responseTy.NewMessage()
    response.Set(field, text) |> ignore
    response.Encode()

let answerBytes (responseTy: MessageType) (payload: byte[]) : byte[] =
    use response = responseTy.NewMessage()
    response.Set(protobufPayload, ReadOnlySpan payload) |> ignore
    response.Encode()

let handle (schema: Schema) (requestTy: MessageType) (responseTy: MessageType) (body: byte[]) : byte[] =
    match Pbwire.tryParse requestTy body with
    | Error _ -> answer responseTy runtimeError "cannot parse ConformanceRequest"
    | Ok req ->
        use req = req
        let wanted = Pbwire.getInt64 req "requested_output_format"
        let name = Pbwire.tryString req "message_type" |> Option.defaultValue ""
        let payload = Pbwire.tryBytes req "protobuf_payload"
        if wanted <> int64 protobuf then
            answer responseTy skipped $"output format %d{wanted} is not supported"
        elif payload.IsNone then
            answer responseTy skipped "only binary input is supported"
        else
            let ty =
                try
                    messageOf schema name |> Some
                with :? PbwireException ->
                    None
            match ty with
            | None -> answer responseTy runtimeError $"unknown message type '%s{name}'"
            | Some ty ->
                match Pbwire.tryParse ty payload.Value with
                | Error e -> answer responseTy parseError e.Message
                | Ok parsed ->
                    use parsed = parsed
                    try
                        answerBytes responseTy (parsed.Encode())
                    with :? PbwireException as e ->
                        answer responseTy serializeError e.Message

let readExactly (stream: Stream) (buf: byte[]) off count =
    let mutable got = 0
    let mutable eof = false
    while got < count && not eof do
        let n = stream.Read(buf, off + got, count - got)
        if n = 0 then eof <- true else got <- got + n
    got, eof

let readRequest (stdin: Stream) : byte[] option =
    let header = Array.zeroCreate 4
    let got, eof = readExactly stdin header 0 4
    if got < 4 then
        None // clean end of input (or a truncated header)
    else
        let size =
            int header[0]
            ||| (int header[1] <<< 8)
            ||| (int header[2] <<< 16)
            ||| (int header[3] <<< 24)
        let body = Array.zeroCreate size
        let read, _ = readExactly stdin body 0 size
        if read < size then failwith "truncated conformance request"
        Some body

let writeResponse (stdout: Stream) (payload: byte[]) =
    let len = payload.Length
    let header =
        [| byte (len &&& 0xFF)
           byte ((len >>> 8) &&& 0xFF)
           byte ((len >>> 16) &&& 0xFF)
           byte ((len >>> 24) &&& 0xFF) |]
    stdout.Write(header, 0, 4)
    stdout.Write(payload, 0, len)
    stdout.Flush()

[<EntryPoint>]
let main argv =
    if argv.Length < 1 then
        eprintfn "usage: Testee <conformance.desc>"
        2
    else
        use schema = Pbwire.schema argv[0]
        let requestTy = Pbwire.messageType schema "conformance.ConformanceRequest"
        let responseTy = Pbwire.messageType schema "conformance.ConformanceResponse"
        use stdin = Console.OpenStandardInput()
        use stdout = Console.OpenStandardOutput()
        let mutable body = readRequest stdin
        while body.IsSome do
            writeResponse stdout (handle schema requestTy responseTy body.Value)
            body <- readRequest stdin
        0

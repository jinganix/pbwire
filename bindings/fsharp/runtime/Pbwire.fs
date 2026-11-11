// The F# binding: an idiomatic facade over the C# binding's dynamic API
// (bindings/csharp/Pbwire), which is P/Invoke over the C ABI in
// include/pbwire.h. Like the Kotlin binding over Java, this adds no wire code
// and no second schema path: a FileDescriptorSet
// (protoc --descriptor_set_out) becomes the schema at run time, and the wire
// format lives in the C core, so an F# message and a C message are the same
// bytes by construction.
//
// Ownership and threads follow the C# binding: a Schema and a root Message own
// native memory and Dispose() frees it, a nested message is a view of its
// parent, and one message belongs to one thread, like the C core.

namespace Pbwire.FSharp

open System
open Pbwire

/// A field value by name: the record-friendly write path for `setAll`.
type FieldValue =
    | Int of int64
    | Bool of bool
    | Float of float32
    | Double of float
    | Text of string
    | Bytes of byte[]

/// Thin idioms over the C# dynamic API: load, build, parse, read.
module Pbwire =

    /// Loads a descriptor set written by `protoc --descriptor_set_out`.
    let schema (path: string) : Schema = Schema.FromFile path

    /// Loads descriptor-set bytes; use `--include_imports` so every type resolves.
    let schemaBytes (bytes: byte[]) : Schema = Schema.FromBytes bytes

    /// Looks up a message by fully qualified name, e.g. `structtest.Scalars3`.
    let messageType (schema: Schema) (name: string) : MessageType = schema.Message name

    /// A new message, then `init` against it (the Kotlin `build` idiom).
    let build (ty: MessageType) (init: Message -> unit) : Message =
        let msg = ty.NewMessage()
        init msg
        msg

    /// Decodes wire bytes; unknown fields are preserved and re-encoded.
    let parse (ty: MessageType) (wire: byte[]) : Message = ty.Decode(ReadOnlySpan wire)

    /// Decodes wire bytes; refused input is an Error, not an exception.
    let tryParse (ty: MessageType) (wire: byte[]) : Result<Message, PbwireException> =
        try Ok(parse ty wire) with :? PbwireException as e -> Error e

    /// Encoded bytes (one allocation by the core, one copy into a managed array).
    let encode (msg: Message) : byte[] = msg.Encode()

    /// A nested message, created in place and passed to `init`; appends when
    /// the field is repeated. Returns the nested view, which borrows `msg`.
    let nested (msg: Message) (field: string) (init: Message -> unit) : Message =
        let inner = msg.SetMessage field
        init inner
        inner

    /// Sets several fields at once; repeated fields append, one per item.
    let setAll (msg: Message) (fields: (string * FieldValue) list) : Message =
        for name, value in fields do
            match value with
            | Int v -> msg.Set(name, v) |> ignore
            | Bool v -> msg.Set(name, v) |> ignore
            | Float v -> msg.Set(name, v) |> ignore
            | Double v -> msg.Set(name, v) |> ignore
            | Text v -> msg.Set(name, v) |> ignore
            | Bytes v -> msg.Set(name, ReadOnlySpan v) |> ignore
        msg

    /// True when a singular field is present (proto2 optional / proto3 optional / oneof member).
    let has (msg: Message) (field: string) : bool = msg.Has field

    /// Items in a repeated field, or 0 when the field is absent.
    let count (msg: Message) (field: string) : int = msg.Count field

    /// One item of a repeated field, or None when the field is absent or out of range.
    let at (msg: Message) (field: string) (index: int) : Value option =
        match msg.At(field, index) with
        | null -> None
        | v -> Some v

    /// Singular getters; strings, bytes and messages are None when absent.
    let getInt64 (msg: Message) (field: string) : int64 = msg.GetLong field
    let getInt (msg: Message) (field: string) : int = msg.GetInt field
    let getBool (msg: Message) (field: string) : bool = msg.GetBool field
    let getFloat32 (msg: Message) (field: string) : float32 = msg.GetFloat field
    let getFloat (msg: Message) (field: string) : float = msg.GetDouble field

    let tryString (msg: Message) (field: string) : string option =
        match msg.GetString field with
        | null -> None
        | s -> Some s

    let tryBytes (msg: Message) (field: string) : byte[] option =
        match msg.GetBytes field with
        | null -> None
        | b -> Some b

    let tryMessage (msg: Message) (field: string) : Message option =
        match msg.GetMessage field with
        | null -> None
        | m -> Some m

    /// Canonical encoding: map entries sorted by key, like the reference.
    let encodeDeterministic (msg: Message) : byte[] = msg.EncodeDeterministic()

    /// One length-delimited frame (varint length + message).
    let encodeDelimited (msg: Message) : byte[] = msg.EncodeDelimited()

    /// Decodes one length-delimited frame; returns the message and how many
    /// bytes the frame occupied.
    let decodeDelimited (ty: MessageType) (wire: byte[]) : Message * int =
        let struct (msg, consumed) = ty.DecodeDelimited(ReadOnlySpan<byte> wire)
        msg, consumed

    /// Decodes with explicit bounds (0 = the defaults).
    let decodeLimits (ty: MessageType) (wire: byte[]) (maxBytes: uint64) (maxDepth: uint32) : Message =
        ty.DecodeLimits(ReadOnlySpan<byte> wire, maxBytes, maxDepth)

    /// Protobuf JSON mapping. Unknown fields are not printed.
    let printJson (msg: Message) : string = msg.PrintJson()

    /// Parses JSON mapping text; unknown fields fail unless `ignoreUnknown`.
    let parseJson (ty: MessageType) (text: string) (ignoreUnknown: bool) : Message =
        ty.ParseJson(text, ignoreUnknown)

    /// Protobuf text format.
    let printText (msg: Message) (printUnknown: bool) : string = msg.PrintText(printUnknown)

    /// Parses text format.
    let parseText (ty: MessageType) (text: string) : Message = ty.ParseText text

    /// Exact bytes `encode` would write, without writing them.
    let encodedSize (msg: Message) : uint64 = msg.EncodedSize()

    /// True when every proto2 `required` field is present.
    let isInitialized (msg: Message) : bool = msg.IsInitialized()

    /// Field 1 of a `google.protobuf.Any` tree, or None when absent.
    let tryAnyTypeUrl (msg: Message) : string option =
        match msg.AnyTypeUrl() with
        | null -> None
        | s -> Some s

    /// Field 2 of a `google.protobuf.Any` tree, or None when absent.
    let tryAnyPayload (msg: Message) : byte[] option =
        match msg.AnyPayload() with
        | null -> None
        | b -> Some b

    /// A `google.protobuf.Timestamp` tree as milliseconds.
    let timestampMillis (msg: Message) : int64 = msg.TimestampMillis()

    /// A `google.protobuf.Duration` tree as milliseconds.
    let durationMillis (msg: Message) : int64 = msg.DurationMillis()

    /// Every path of a `google.protobuf.FieldMask` tree.
    let fieldMaskPaths (msg: Message) : string[] = msg.FieldMaskPaths()

    /// Packs `payload` into an `Any` tree; `ty` is the Any descriptor.
    let anyPack (ty: MessageType) (typeUrl: string) (payload: byte[]) : Message =
        ty.AnyPack(typeUrl, ReadOnlySpan<byte> payload)

    /// A new Timestamp tree holding `ms`; `ty` is the Timestamp descriptor.
    let timestampFromMillis (ty: MessageType) (ms: int64) : Message = ty.TimestampFromMillis ms

    /// A new Duration tree holding `ms`; `ty` is the Duration descriptor.
    let durationFromMillis (ty: MessageType) (ms: int64) : Message = ty.DurationFromMillis ms

    /// Single-level scan of the wire: one callback per field, no tree built.
    /// Return false from `cb` to abort the scan.
    let visit (ty: MessageType) (wire: byte[]) (cb: VisitField -> bool) : unit =
        ty.Visit(ReadOnlySpan<byte> wire, Func<VisitField, bool> cb)

    /// How many services the loaded files declare.
    let serviceCount (schema: Schema) : int = schema.ServiceCount()

    /// One service by fully qualified name, e.g. `pkg.Service`.
    let service (schema: Schema) (name: string) : Service = schema.Service name

    /// Resolves a method to its input/output message types for routing.
    let resolveMethod (schema: Schema) (svc: string) (meth: string) : MessageType * MessageType =
        let struct (input, output) = schema.ResolveMethod(svc, meth)
        input, output

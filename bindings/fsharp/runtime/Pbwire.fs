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

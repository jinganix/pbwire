// Self-check against the pinned protoc bytes: build structtest.Scalars3,
// encode, compare with matrix3.bin (the same oracle the C# binding uses),
// then decode it back and check the values.
//
//   dotnet run --project tests -- <descriptor-set>

module Tests

open System
open System.IO
open Pbwire
open Pbwire.FSharp

let mutable checks = 0

let check ok what =
    checks <- checks + 1
    if not ok then
        eprintfn $"FAIL %s{what}"
        exit 1

let throws op =
    try
        op ()
        false
    with :? PbwireException ->
        true

let rejects op (expected: string) =
    try
        op ()
        false
    with :? PbwireException as e ->
        e.Message.Contains expected

let asStringOpt (v: Value) : string option = v.AsString() |> Option.ofObj
let asBytesOpt (v: Value) : byte[] option = v.AsBytes() |> Option.ofObj
let asMessageOpt (v: Value) : Message option = v.AsMessage() |> Option.ofObj

let makeMessage (ty: MessageType) (innerTy: MessageType) : Message =
    let msg = ty.NewMessage()
    let name = innerTy.Field "name"
    let value = innerTy.Field "value"

    Pbwire.setAll
        msg
        [ "i32", Int (int64 Int32.MinValue)
          "i64", Int Int64.MaxValue
          "u32", Int 0xFFFFFFFFL
          "u64", Int -1L // u64::MAX bits
          "s32", Int (int64 Int32.MinValue)
          "s64", Int Int64.MinValue
          "fx32", Int 0xDEADBEEFL
          "fx64", Int 72623859790382856L
          "sfx32", Int (int64 Int32.MinValue)
          "sfx64", Int Int64.MinValue
          "f32", Float Single.PositiveInfinity
          "f64", Double -123456.789
          "flag", Bool true
          "str", Text "hello"
          "color", Int 2L
          "opt_i32", Int -7L
          "opt_str", Text "opt" ]
    |> ignore

    for n in [ 1L; -5L; 300L ] do
        msg.Set("nums", n) |> ignore
    for tag in [ "a"; "bb" ] do
        msg.Set("tags", tag) |> ignore
    for innerName, innerValue in [ "x", 1L; "y", -2L ] do
        Pbwire.nested msg "inners" (fun inner ->
            inner.Set(name, innerName) |> ignore
            inner.Set(value, innerValue) |> ignore)
        |> ignore
    for flag in [ true; false; true ] do
        msg.Set("flags", flag) |> ignore
    Pbwire.nested msg "inner" (fun inner ->
        inner.Set(name, "kid") |> ignore
        inner.Set(value, 42L) |> ignore)
    |> ignore
    msg

let fill (ty: MessageType) (innerTy: MessageType) : byte[] =
    use msg = makeMessage ty innerTy
    msg.Encode()

// Concurrent stress over the matrix3 oracle: 8 workers x 200 iters, three
// phases per worker mirroring the C/C++/Rust/Zig/Swift/C# shape:
//
//   (a) per-worker message encode == want bytes,
//   (b) SHARED read-only message concurrent encode == want bytes,
//   (c) fresh-target decode of want with field checks (NOT re-encode).
//
// Only the schema types and the oracle cross threads. MessageType.Field takes
// a lock around its cache fill (like LayoutSource.Plan in the C# binding this
// facades), so worker name lookups are safe even cold: the serial baseline
// below is the oracle pin only, not a cache warmer. Every other message is
// worker-owned. Concurrent encodes of one native tree are safe: the C core
// keeps no mutable encode state (proven by bindings/c/tests/test_concurrency.c).
// Speed prints only, never asserted.
let concurrency (schema: Schema) (ty: MessageType) (innerTy: MessageType) (oracle: byte[]) =
    // Cap tracks ACTUAL parallelism (Julia precedent `min(8, nthreads())`):
    // `Async.Parallel` over 8 blocking-gated workers does not guarantee 8
    // concurrent threads, so a `Barrier(8)` start gate can deadlock on hosts
    // with < 8 cores. The spin/wait start gate below waits on this same
    // count, so gate and parallelism cannot disagree. Blocking waits also
    // stay out of `async {}`: the cold phase runs on `Task.Run` threads.
    let workers = min 8 Environment.ProcessorCount
    let iters = 200
    // Every field the matrix3 fixture touches: resolving the full set per
    // iteration hammers the cache fill from all workers at once.
    let names =
        [| "i32"; "i64"; "u32"; "u64"; "s32"; "s64"; "fx32"; "fx64"
           "sfx32"; "sfx64"; "f32"; "f64"; "flag"; "str"; "color"
           "opt_i32"; "opt_str"; "nums"; "tags"; "inners"; "inner"; "flags" |]

    // COLD-cache phase first: one FRESH type pair with no prior Field lookups
    // on any thread, shared across workers that concurrently resolve the full
    // field-name set and encode 200 iters each. This is the real server pattern
    // (a singleton type shared across first-burst request threads); concurrent
    // cold fills used to race on the Dictionary, so this phase exercises the
    // lock. Resolution must match the warm pair field-for-field and the bytes
    // must equal the oracle.
    let coldTy = Pbwire.messageType schema "structtest.Scalars3"
    let coldInner = Pbwire.messageType schema "structtest.Inner3"
    // A spin/wait start gate so all workers hit the cold cache in the same
    // instant: without it, thread-startup stagger alone could serialize the
    // first fill and the race window would be timing luck instead of a real
    // hammering.
    let mutable arrived = 0
    let coldSw = System.Diagnostics.Stopwatch.StartNew()

    let coldWork (worker: int) : string list =
        System.Threading.Interlocked.Increment(&arrived) |> ignore
        let spin = System.Threading.SpinWait()
        while System.Threading.Volatile.Read(&arrived) < workers do
            spin.SpinOnce()
        let problems = ResizeArray<string>()
        for _ in 1..iters do
            try
                let mutable mismatch: string option = None
                for n in names do
                    if mismatch.IsNone && (coldTy.Field n).Number <> (ty.Field n).Number then
                        mismatch <- Some n
                match mismatch with
                | Some n -> problems.Add($"worker %d{worker}: cold field %s{n} resolved differently")
                | None -> ()
                if (coldInner.Field "name").Number <> (innerTy.Field "name").Number
                    || (coldInner.Field "value").Number <> (innerTy.Field "value").Number then
                    problems.Add($"worker %d{worker}: cold inner field resolved differently")
                System.Threading.Interlocked.Increment(&checks) |> ignore
                if fill coldTy coldInner <> oracle then
                    problems.Add($"worker %d{worker}: cold independent-encode != want bytes")
            with _ ->
                problems.Add($"worker %d{worker}: cold threw")
        List.ofSeq problems

    let coldResults =
        [| for w in 0 .. workers - 1 -> System.Threading.Tasks.Task.Run(fun () -> coldWork w) |]
        |> System.Threading.Tasks.Task.WhenAll
        |> fun t -> t.Result
        |> Array.toList
        |> List.concat

    coldSw.Stop()
    let coldTotal = workers * iters
    printfn $"OK   concurrent cold-cache: %d{coldTotal} resolves+encodes in %.3f{coldSw.Elapsed.TotalSeconds} s (%.0f{float coldTotal / coldSw.Elapsed.TotalSeconds}/s)"
    check (List.isEmpty coldResults) (sprintf "concurrency cold-cache: %s" (String.concat "; " (List.truncate 8 coldResults)))

    // Serial encode-once baseline: must equal the protoc oracle before any
    // worker starts, so a mapping bug fails here and not in a thread.
    check (fill ty innerTy = oracle) "concurrency serial encode equals the protoc oracle"

    // Shared read-only fixture for phase (b): never mutated while workers run.
    use shared = makeMessage ty innerTy

    let sw = System.Diagnostics.Stopwatch.StartNew()

    let work (worker: int) : Async<string list> =
        async {
            let problems = ResizeArray<string>()
            let record phase = problems.Add($"worker %d{worker}: %s{phase}")
            for _ in 1..iters do
                // (a) independent-message encode == want bytes.
                try
                    use m = makeMessage ty innerTy
                    System.Threading.Interlocked.Increment(&checks) |> ignore
                    if m.Encode() <> oracle then
                        record "phase (a) independent-encode != want bytes"
                with _ ->
                    record "phase (a) threw"
                // (b) SHARED read-only message concurrent encode == want bytes.
                try
                    System.Threading.Interlocked.Increment(&checks) |> ignore
                    if shared.Encode() <> oracle then
                        record "phase (b) shared-read-only-encode != want bytes"
                with _ ->
                    record "phase (b) threw"
                // (c) fresh-target decode with field checks, NOT re-encode.
                try
                    use back = Pbwire.parse ty oracle
                    System.Threading.Interlocked.Increment(&checks) |> ignore
                    let innerValue =
                        Pbwire.at back "inners" 1
                        |> Option.bind asMessageOpt
                        |> Option.map (fun m -> Pbwire.getInt64 m "value")
                    let ok =
                        Pbwire.getInt back "i32" = Int32.MinValue
                        && Pbwire.getBool back "flag"
                        && Pbwire.tryString back "str" = Some "hello"
                        && Pbwire.getFloat back "f64" = -123456.789
                        && Pbwire.getInt64 back "color" = 2L
                        && Pbwire.getInt64 back "opt_i32" = -7L
                        && Pbwire.count back "nums" = 3
                        && (Pbwire.at back "nums" 1 |> Option.map (fun v -> v.AsLong())) = Some -5L
                        && (Pbwire.at back "tags" 1 |> Option.bind asStringOpt) = Some "bb"
                        && innerValue = Some -2L
                        && (Pbwire.at back "flags" 1 |> Option.map (fun v -> v.AsBool())) = Some false
                        && (Pbwire.tryMessage back "inner" |> Option.bind (fun m -> Pbwire.tryString m "name")) = Some "kid"
                    if not ok then
                        record "phase (c) fresh-target decode field mismatch"
                with _ ->
                    record "phase (c) threw"
            return List.ofSeq problems
        }

    let results =
        [ for w in 0 .. workers - 1 -> work w ]
        |> Async.Parallel
        |> Async.RunSynchronously
        |> Array.toList
        |> List.concat

    sw.Stop()
    let total = workers * iters * 3
    printfn $"OK   concurrent: %d{total} encodes+decodes in %.3f{sw.Elapsed.TotalSeconds} s (%.0f{float total / sw.Elapsed.TotalSeconds}/s)"
    check (List.isEmpty results) (sprintf "concurrency: %s" (String.concat "; " (List.truncate 8 results)))

// Walk up to the checkout: `dotnet run` keeps the caller's working directory.
let findRepo () =
    let mutable current = Some(DirectoryInfo(Directory.GetCurrentDirectory()))
    let mutable found = ""
    while found = "" do
        match current with
        | None -> failwith "no pbwire checkout above the working directory"
        | Some d ->
            if File.Exists(Path.Combine(d.FullName, "tests/data/edge.desc")) then
                found <- d.FullName
            else
                current <- Option.ofObj d.Parent
    found

[<EntryPoint>]
let main argv =
    let desc =
        if argv.Length > 0 then
            argv[0]
        else
            "../../csharp/tests/data/matrix3.desc"

    let dir =
        Path.GetDirectoryName(Path.GetFullPath desc)
        |> Option.ofObj
        |> Option.defaultValue "."
    let oracle = File.ReadAllBytes(Path.Combine(dir, "matrix3.bin"))

    use schema = Pbwire.schema desc
    let ty = Pbwire.messageType schema "structtest.Scalars3"
    let innerTy = Pbwire.messageType schema "structtest.Inner3"

    let wire = fill ty innerTy
    check (wire = oracle) $"encoded bytes differ from protoc (%d{wire.Length} vs %d{oracle.Length})"
    printfn $"OK   encode matches protoc, %d{oracle.Length} bytes"

    use decoded = Pbwire.parse ty oracle
    check (Pbwire.getInt decoded "i32" = Int32.MinValue) "i32"
    check (Pbwire.getBool decoded "flag") "flag"
    check (Pbwire.tryString decoded "str" = Some "hello") "str"
    check (Pbwire.getFloat decoded "f64" = -123456.789) "f64"
    check (Pbwire.getInt64 decoded "color" = 2L) "color"
    check (Pbwire.getInt64 decoded "opt_i32" = -7L) "opt_i32"
    check (Pbwire.has decoded "opt_str" && not (ty.NewMessage().Has "i32")) "presence"
    check (Pbwire.count decoded "nums" = 3) "nums count"
    check ((Pbwire.at decoded "nums" 1 |> Option.map (fun v -> v.AsLong())) = Some -5L) "nums[1]"
    check ((Pbwire.at decoded "tags" 1 |> Option.bind asStringOpt) = Some "bb") "tags[1]"
    let innerValue =
        Pbwire.at decoded "inners" 1
        |> Option.bind asMessageOpt
        |> Option.map (fun m -> Pbwire.getInt64 m "value")
    check (innerValue = Some -2L) "inners[1].value"
    check ((Pbwire.at decoded "flags" 1 |> Option.map (fun v -> v.AsBool())) = Some false) "flags[1]"
    let innerName =
        Pbwire.tryMessage decoded "inner"
        |> Option.bind (fun m -> Pbwire.tryString m "name")
    check (innerName = Some "kid") "inner.name"
    check (decoded.Encode() = oracle) "decode -> encode is byte-exact"
    printfn "OK   decode round trip, repeated and nested fields"

    // A field the schema does not declare (99, varint 7) survives re-encoding.
    let unknown = [| 0x98uy; 0x06uy; 0x07uy |]
    use innerOnly = Pbwire.parse innerTy unknown
    check (innerOnly.Encode() = unknown) "unknown field preserved"

    // Bad input is an error, not a crash.
    check (throws (fun () -> Pbwire.parse ty [| 0x80uy |] |> ignore)) "truncated tag accepted"
    check (throws (fun () -> Pbwire.messageType schema "structtest.Nope" |> ignore)) "unknown message accepted"
    check (throws (fun () -> ty.Field "nope" |> ignore)) "unknown field accepted"

    // The cases a binding can get wrong even when the core is right: payloads
    // with embedded NULs / invalid UTF-8, non-ASCII text, a negative enum, an
    // empty repeated element, proto3 optional presence with an empty value,
    // and oneof switching. tests/data/edge.bin is protoc --encode.
    let root = findRepo ()
    let edgeOracle = File.ReadAllBytes(Path.Combine(root, "tests/data/edge.bin"))
    use edgeSchema = Pbwire.schema (Path.Combine(root, "tests/data/edge.desc"))
    let edgeTy = Pbwire.messageType edgeSchema "edge.Edge"

    use edge = edgeTy.NewMessage()
    Pbwire.setAll
        edge
        [ "raw", Bytes [| 0x00uy; 0x01uy; 0xFFuy |]
          "text", Text "héllo ☃"
          "sign", Int -3L
          "maybe", Text ""
          "pick_num", Int 5L // oneof: replaced below
          "pick_str", Text "one" ]
    |> ignore
    for tag in [ "a"; ""; "b" ] do
        edge.Set("tags", tag) |> ignore
    edge.Set("blobs", ReadOnlySpan [| 0x00uy |]) |> ignore
    edge.Set("blobs", ReadOnlySpan [||]) |> ignore
    Pbwire.nested edge "inner" (fun inner ->
        inner.Set("tag", "kid") |> ignore
        inner.Set("n", -1L) |> ignore)
    |> ignore
    check (edge.Encode() = edgeOracle) "edge payload differs from protoc"

    use edgeBack = Pbwire.parse edgeTy edgeOracle
    check (Pbwire.tryBytes edgeBack "raw" = Some [| 0x00uy; 0x01uy; 0xFFuy |]) "bytes with NUL and 0xff"
    check (Pbwire.tryString edgeBack "text" = Some "héllo ☃") "non-ASCII string"
    check (Pbwire.getInt64 edgeBack "sign" = -3L) "negative enum"
    check (Pbwire.count edgeBack "tags" = 3) "empty repeated element count"
    check ((Pbwire.at edgeBack "tags" 1 |> Option.bind asStringOpt) = Some "") "empty repeated element"
    check ((Pbwire.at edgeBack "blobs" 0 |> Option.bind asBytesOpt) = Some [| 0x00uy |]) "bytes element with NUL"
    let blobLen =
        Pbwire.at edgeBack "blobs" 1
        |> Option.bind asBytesOpt
        |> Option.map Array.length
    check (blobLen = Some 0) "empty bytes element"
    let nestedN =
        Pbwire.tryMessage edgeBack "inner"
        |> Option.map (fun m -> Pbwire.getInt64 m "n")
    check (nestedN = Some -1L) "nested negative"
    check (Pbwire.has edgeBack "maybe" && Pbwire.tryString edgeBack "maybe" = Some "") "optional present and empty"
    check (Pbwire.has edgeBack "pick_str" && not (Pbwire.has edgeBack "pick_num")) "oneof keeps only the last member"
    check (edgeBack.Encode() = edgeOracle) "edge decode -> encode is byte-exact"
    printfn "OK   edge payload: bytes, UTF-8, negative enum, empty elements, oneof"

    // A default message is zero bytes both ways.
    check (edgeTy.NewMessage().Encode().Length = 0) "empty message encodes to zero bytes"
    use emptyDecoded = Pbwire.parse edgeTy [||]
    check (emptyDecoded.Encode().Length = 0) "empty wire decodes and re-encodes to zero bytes"
    check (not (Pbwire.has emptyDecoded "text")) "empty message has no fields"

    // A wrong value kind is refused, not written.
    check (rejects (fun () -> edgeTy.NewMessage().Set("pick_num", "not a number") |> ignore) "type mismatch") "wrong-typed value rejected"
    check (rejects (fun () -> edgeTy.NewMessage().SetMessage "text" |> ignore) "not a message field") "setMessage on a scalar rejected"

    // The extended surface: deterministic/delimited encode, JSON and text
    // print/parse, bounded decode, visit, and service routing metadata.
    use m = ty.NewMessage()
    Pbwire.setAll m [ "str", Text "hello" ] |> ignore
    let w = Pbwire.encode m
    check (Pbwire.encodedSize m = uint64 w.Length) "encoded size"
    check (Pbwire.isInitialized m) "initialized"
    check (Pbwire.encodeDeterministic m = w) "deterministic"
    let framed = Pbwire.encodeDelimited m
    let back, consumed = Pbwire.decodeDelimited ty (Array.append framed framed)
    use back = back
    check (Pbwire.encode back = w) "delimited round trip"
    check (consumed = framed.Length) "delimited consumed"
    let js = Pbwire.printJson m
    use fromJson = Pbwire.parseJson ty js false
    check (Pbwire.encode fromJson = w) "json round trip"
    check (throws (fun () -> Pbwire.parseJson ty "{\"nope\":1}" false |> ignore)) "json unknown accepted"
    use ignored = Pbwire.parseJson ty "{\"nope\":1}" true
    check (Pbwire.encode ignored = [||]) "json ignore unknown"
    let tx = Pbwire.printText m false
    use fromText = Pbwire.parseText ty tx
    check (Pbwire.encode fromText = w) "text round trip"
    use limited = Pbwire.decodeLimits ty w 0UL 0u
    check (Pbwire.encode limited = w) "limits defaults"
    check (throws (fun () -> Pbwire.decodeLimits ty w 1UL 0u |> ignore)) "limits maxBytes ignored"
    let mutable seen = 0
    Pbwire.visit ty w (fun f -> seen <- seen + 1; f.Number <> 0u)
    check (seen > 0) "visit saw fields"
    check (Pbwire.serviceCount schema = 0) "no services in fixture"
    check (throws (fun () -> Pbwire.resolveMethod schema "no.Such" "Nope" |> ignore)) "unknown service accepted"

    concurrency schema ty innerTy oracle

    printfn $"OK   %d{checks} checks passed"
    0

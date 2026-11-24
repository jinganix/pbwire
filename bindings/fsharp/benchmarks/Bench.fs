// F# binding benchmark against the official Google.Protobuf C# runtime and
// generated code, on the same matrix3.proto and the four standard tiers from
// BENCHMARKS.md.
//
// The same payload as the C/Go/Java/Rust rows: every scalar kind, the nested
// message, the two proto3 optionals, and the repeated fields scaled x0 / x1 /
// x100 / x3000, which lands the tiers on 136 / 174 / 2469 / 74913 bytes. Both
// sides must produce identical bytes before anything is timed.
//
// Arms (bindings/README.md#pbench-three-arms-one-contract):
//   tree  the facade's Message handle, full task -- fresh build per field per
//               iteration, encode; decode, every gate-checked field read back
//               per iteration (mirrors bindings/csharp/benchmarks/Bench.cs)
//   shim  the C# generated model's native-object entry -- one C-reader call
//               over the flat image (bindings/csharp/pbench/Shim.cs)
//   proxy the held handle -- re-encode a prebuilt tree, decode-and-drop with
//               no read-back. Published, never the headline.
// (The C# harness also times protobuf-net; this one does not: it
// singular-encodes single-element packed fields, so two of the four tiers
// carry nothing MB/s-fair to time.)
//
//   ./bench.sh          generate, build, run

module Bench

open System
open System.Diagnostics
open Pbwire
open Pbwire.FSharp
open Pbwire.Pbench
open Google.Protobuf
open Structtest

let cases = [| "small"; "medium"; "large"; "xlarge" |]
let repeats = [| 0; 1; 100; 3000 |]
let want = [| 136; 174; 2469; 74913 |]
// Floor iterations per tier; doubled until one pass takes 0.25 s.
let floors = [| 50_000; 50_000; 5_000; 500 |]
let minPassSeconds = 0.25
let passes = 3
let mutable failures = 0

let mutable sink = 0L

/// The tree decode half of the timed full task: decode, then read back every
/// gate-checked field (the same field list the gate asserts). A bare
/// decode-and-drop is the proxy row, not this one.
let consumeTree (back: Message) n =
    let mutable acc = 0L
    acc <- acc + int64 (back.GetInt "i32")
    acc <- acc + back.GetLong "i64"
    acc <- acc + back.GetLong "u32"
    acc <- acc + back.GetLong "u64"
    acc <- acc + int64 (back.GetInt "s32")
    acc <- acc + back.GetLong "s64"
    acc <- acc + int64 (back.GetInt "fx32")
    acc <- acc + back.GetLong "fx64"
    acc <- acc + int64 (back.GetInt "sfx32")
    acc <- acc + back.GetLong "sfx64"
    acc <- acc + (if back.GetFloat "f32" > 0.0f then 1L else 0L)
    acc <- acc + int64 (back.GetDouble "f64")
    acc <- acc + (if back.GetBool "flag" then 1L else 0L)
    acc <- acc + int64 (Pbwire.tryString back "str" |> Option.map (fun s -> s.Length) |> Option.defaultValue 0)
    acc <- acc + int64 (back.GetInt "color")
    acc <- acc + (if back.Has "opt_i32" then int64 (back.GetInt "opt_i32") else 0L)
    acc <- acc + int64 (Pbwire.tryString back "opt_str" |> Option.map (fun s -> s.Length) |> Option.defaultValue 0)
    Pbwire.tryMessage back "inner"
    |> Option.iter (fun inner ->
        acc <- acc + int64 (match inner.GetString "name" with null -> 0 | s -> s.Length)
        acc <- acc + int64 (inner.GetInt "value"))
    acc <- acc + int64 (back.Count "nums" + back.Count "tags" + back.Count "inners" + back.Count "flags")
    if n > 0 then
        acc <- acc + (Pbwire.at back "nums" (n - 1) |> Option.map (fun v -> v.AsLong()) |> Option.defaultValue 0L)
        acc <- acc + (Pbwire.at back "nums" 0 |> Option.map (fun v -> v.AsLong()) |> Option.defaultValue 0L)
        acc <- acc + int64 (Pbwire.at back "tags" (n - 1) |> Option.bind (fun v -> match v.AsString() with null -> None | s -> Some s.Length) |> Option.defaultValue 0)
        acc <- acc + (if Pbwire.at back "flags" (n - 1) |> Option.map (fun v -> v.AsBool()) |> Option.defaultValue false then 1L else 0L)
        let kid = Pbwire.at back "inners" (n - 1) |> Option.bind (fun v -> match v.AsMessage() with null -> None | m -> Some m)
        match kid with
        | None -> ()
        | Some k ->
            acc <- acc + int64 (match k.GetString "name" with null -> 0 | s -> s.Length)
            acc <- acc + int64 (k.GetInt "value")
    acc

/// Full field-by-field gate for the tree arm (mirrors C# TreeFieldsOk): the
/// same field list consumeTree reads back on every timed decode.
let treeFieldsOk (back: Message) reference n =
    let innerOk =
        match Pbwire.tryMessage back "inner" with
        | None -> false
        | Some m -> m.GetString "name" = "kid" && m.GetInt "value" = 42
    let kid = if n = 0 then None else Pbwire.at back "inners" (n - 1) |> Option.bind (fun v -> match v.AsMessage() with null -> None | m -> Some m)
    back.Encode() = reference
    && Pbwire.tryString back "str" = Some "hello"
    && innerOk
    && back.Count "nums" = n && back.Count "tags" = n && back.Count "inners" = n && back.Count "flags" = n
    && (n = 0 || Pbwire.at back "nums" (n - 1) |> Option.map (fun v -> v.AsLong()) = Some (int64 (n - 4)))
    && (n = 0 || (kid |> Option.map (fun k -> k.GetInt "value") = Some n))
    && back.Has "opt_i32" && back.GetInt "opt_i32" = -7
    && back.Has "opt_str" && Pbwire.tryString back "opt_str" = Some "opt"
    && back.GetLong "u64" = -1L
    && back.GetInt "color" = 2

let fail label what =
    printfn $"FAIL %s{label}: %s{what}"
    failures <- failures + 1

let fillOfficial n =
    let m =
        Scalars3(
            I32 = Int32.MinValue,
            I64 = Int64.MaxValue,
            U32 = UInt32.MaxValue,
            U64 = UInt64.MaxValue,
            S32 = Int32.MinValue,
            S64 = Int64.MinValue,
            Fx32 = 0xDEADBEEFu,
            Fx64 = 72623859790382856UL,
            Sfx32 = Int32.MinValue,
            Sfx64 = Int64.MinValue,
            F32 = Single.PositiveInfinity,
            F64 = -123456.789,
            Flag = true,
            Str = "hello",
            Color = Color3.Green,
            Inner = Inner3(Name = "kid", Value = 42),
            OptI32 = -7,
            OptStr = "opt"
        )
    for i in 0 .. n - 1 do
        m.Nums.Add(i - 3)
        m.Tags.Add "tag"
        m.Inners.Add(Inner3(Name = "grandkid", Value = i + 1))
        m.Flags.Add(i % 2 = 0)
    m

let fillPbwire (ty: MessageType) (innerTy: MessageType) n =
    let name = innerTy.Field "name"
    let value = innerTy.Field "value"
    let msg =
        Pbwire.build ty (fun msg ->
            Pbwire.setAll
                msg
                [ "i32", Int (int64 Int32.MinValue)
                  "i64", Int Int64.MaxValue
                  "u32", Int (int64 UInt32.MaxValue)
                  "u64", Int -1L
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
            Pbwire.nested msg "inner" (fun inner ->
                inner.Set(name, "kid") |> ignore
                inner.Set(value, 42L) |> ignore)
            |> ignore
            for i in 0 .. n - 1 do
                msg.Set("nums", int64 (i - 3)) |> ignore
                msg.Set("tags", "tag") |> ignore
                msg.Set("flags", i % 2 = 0) |> ignore
                Pbwire.nested msg "inners" (fun inner ->
                    inner.Set(name, "grandkid") |> ignore
                    inner.Set(value, int64 (i + 1)) |> ignore)
                |> ignore)
    msg

let run iterations (op: unit -> unit) =
    let watch = Stopwatch.StartNew()
    for _ in 1..iterations do
        op ()
    watch.Elapsed.TotalNanoseconds / float iterations

// Double from the tier floor until one pass takes minPassSeconds.
let rec calibrate (op: unit -> unit) count =
    let watch = Stopwatch.StartNew()
    for _ in 1..count do
        op ()
    if watch.Elapsed.TotalSeconds >= minPassSeconds || count >= 100_000_000 then
        count
    else
        calibrate op (count * 2)

// Every op calibrates independently from the floor (a slow op already exceeds
// the minimum at the floor and keeps it), then one warm-up pass and the best
// of `passes`.
let measure floor (op: unit -> unit) =
    let count = calibrate op (max 1 floor)
    run count op |> ignore
    let mutable best = Double.MaxValue
    for _ in 1..passes do
        best <- min best (run count op)
    best, count

let row impl op (bytes: int) ns =
    printfn "  %-20s %-7s %12.1f %10.1f" impl op ns (float bytes / ns * 1000.0)

[<EntryPoint>]
let main argv =
    if argv.Length <> 1 then
        eprintfn "usage: Bench <descriptor-set>"
        2
    else
        use schema = Pbwire.schema argv[0]
        let ty = Pbwire.messageType schema "structtest.Scalars3"
        let innerTy = Pbwire.messageType schema "structtest.Inner3"

        for tier in 0 .. cases.Length - 1 do
            let label = cases[tier]
            let n = repeats[tier]
            let floor = floors[tier]

            let reference = (fillOfficial n).ToByteArray()
            if reference.Length <> want[tier] then
                fail label $"official encoder wrote %d{reference.Length} bytes, expected %d{want[tier]}"
            else
                use message = fillPbwire ty innerTy n
                let got = message.Encode()
                if got <> reference then
                    fail label $"pbwire-fsharp wrote %d{got.Length} bytes, protobuf-csharp %d{reference.Length}"
                else
                    printfn $"OK   %s{label} encoding parity, x{n} (%d{reference.Length} bytes)"

                    // The shim arm's native object: the committed generated model,
                    // filled before the timer; same two rows through the C# runtime.
                    let native = Matrix3Shim.New n
                    if Matrix3Shim.Marshal native <> reference then
                        fail label "pbwire-fsharp-shim encode differs"
                    elif Matrix3Shim.Marshal(Matrix3Shim.Unmarshal reference) <> reference then
                        fail label "pbwire-fsharp-shim decode differs"

                    // The tree arm's full task, gated before it is timed: a fresh
                    // per-field build per iteration must match the reference too
                    // (not just the held handle above), and the decode below must
                    // expose the same values the timer reads back.
                    use fresh = fillPbwire ty innerTy n
                    if fresh.Encode() <> reference then
                        fail label "pbwire-fsharp-tree fresh-build bytes differ"
                    else
                        use back = Pbwire.parse ty reference
                        if not (treeFieldsOk back reference n) then
                            fail label "pbwire-fsharp-tree field compare differs"
                        else
                            // Tier line shows the proxy op's count; the floor calibration
                            // stays on the prebuilt op so the slower tree op never
                            // inflates any row. Every op calibrates on its own count.
                            let proxyEncodeNs, tierCount = measure floor (fun () -> message.Encode() |> ignore)
                            let proxyDecodeNs, _ =
                                measure floor (fun () ->
                                    use d = Pbwire.parse ty reference
                                    ())
                            // Tree ops are the full task: fresh Message per iteration,
                            // deep read-back of every gate-checked field on decode.
                            let treeEncodeNs, _ =
                                measure floor (fun () ->
                                    use built = fillPbwire ty innerTy n
                                    built.Encode() |> ignore)
                            let treeDecodeNs, _ =
                                measure floor (fun () ->
                                    use d = Pbwire.parse ty reference
                                    sink <- sink + consumeTree d n)

                            // Headline encode rows are the fresh task: build a fresh native
                            // input per iteration inside the timer, encode, release inside
                            // the timer. Decode rows already allocate a fresh output per
                            // iteration; kept as is.
                            let shimEncodeNs, _ = measure floor (fun () -> Matrix3Shim.Marshal (Matrix3Shim.New n) |> ignore)
                            let shimDecodeNs, _ = measure floor (fun () -> Matrix3Shim.Unmarshal reference |> ignore)

                            let officialEncodeNs, _ = measure floor (fun () -> (fillOfficial n).ToByteArray() |> ignore)
                            let officialDecodeNs, _ =
                                measure floor (fun () -> Scalars3.Parser.ParseFrom reference |> ignore)

                            printfn $"%s{label}: %d{reference.Length} bytes, %d{tierCount} iterations (repeat field x{n})"
                            let hImpl: string = "impl"
                            let hOp: string = "op"
                            let hNs: string = "ns/op"
                            let hMb: string = "MB/s"
                            printfn "  %-20s %-7s %12s %10s" hImpl hOp hNs hMb
                            row "pbwire-fsharp-tree" "encode" reference.Length treeEncodeNs
                            row "pbwire-fsharp-proxy" "encode" reference.Length proxyEncodeNs
                            row "pbwire-fsharp-shim" "encode" reference.Length shimEncodeNs
                            row "protobuf-csharp" "encode" reference.Length officialEncodeNs
                            row "pbwire-fsharp-tree" "decode" reference.Length treeDecodeNs
                            row "pbwire-fsharp-proxy" "decode" reference.Length proxyDecodeNs
                            row "pbwire-fsharp-shim" "decode" reference.Length shimDecodeNs
                            row "protobuf-csharp" "decode" reference.Length officialDecodeNs
                            printfn ""

        if failures <> 0 then
            eprintfn $"%d{failures} check(s) failed"
            1
        else
            0

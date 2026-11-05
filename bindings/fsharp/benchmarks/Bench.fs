// F# binding benchmark against the official Google.Protobuf C# runtime and
// generated code, on the same matrix3.proto and the four standard tiers from
// BENCHMARKS.md.
//
// The same payload as the C/Go/Java/Rust rows: every scalar kind, the nested
// message, the two proto3 optionals, and the repeated fields scaled x0 / x1 /
// x100 / x3000, which lands the tiers on 136 / 174 / 2469 / 74913 bytes. Both
// sides must produce identical bytes before anything is timed, and both
// allocate per call (pbwire Encode()/Parse() against ToByteArray()/
// ParseFrom()), so the comparison is like for like. (The C# harness also times
// protobuf-net; this one does not: it singular-encodes single-element packed
// fields, so two of the four tiers carry nothing MB/s-fair to time.)
//
//   ./bench.sh          generate, build, run

module Bench

open System
open System.Diagnostics
open Pbwire
open Pbwire.FSharp
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
    printfn "  %-12s %-7s %12.1f %10.1f" impl op ns (float bytes / ns * 1000.0)

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

                    // Decode must round-trip byte-exactly and expose the same values.
                    use back = Pbwire.parse ty reference
                    let backInnerName =
                        Pbwire.tryMessage back "inner"
                        |> Option.bind (fun m -> Pbwire.tryString m "name")
                    let backOk =
                        back.Encode() = reference
                        && Pbwire.tryString back "str" = Some "hello"
                        && backInnerName = Some "kid"
                        && Pbwire.count back "nums" = n
                        && (n = 0
                            || Pbwire.at back "nums" (n - 1)
                               |> Option.map (fun v -> v.AsLong())
                               = Some (int64 (n - 4)))
                    if not backOk then
                        fail label "pbwire-fsharp decode differs"
                    else
                        let official = fillOfficial n
                        // Tier line shows the reference op's count; slower ops keep the floor.
                        let encodeNs, tierCount = measure floor (fun () -> message.Encode() |> ignore)
                        let decodeNs, _ =
                            measure floor (fun () ->
                                use d = Pbwire.parse ty reference
                                ())

                        let officialEncodeNs, _ = measure floor (fun () -> official.ToByteArray() |> ignore)
                        let officialDecodeNs, _ =
                            measure floor (fun () -> Scalars3.Parser.ParseFrom reference |> ignore)

                        printfn $"%s{label}: %d{reference.Length} bytes, %d{tierCount} iterations (repeat field x{n})"
                        let hImpl: string = "impl"
                        let hOp: string = "op"
                        let hNs: string = "ns/op"
                        let hMb: string = "MB/s"
                        printfn "  %-12s %-7s %12s %10s" hImpl hOp hNs hMb
                        row "pbwire-fsharp" "encode" reference.Length encodeNs
                        row "protobuf-csharp" "encode" reference.Length officialEncodeNs
                        row "pbwire-fsharp" "decode" reference.Length decodeNs
                        row "protobuf-csharp" "decode" reference.Length officialDecodeNs
                        printfn ""

        if failures <> 0 then
            eprintfn $"%d{failures} check(s) failed"
            1
        else
            0

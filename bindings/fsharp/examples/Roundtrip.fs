// A runnable round trip through the dynamic API: build structtest.Scalars3,
// encode it, decode it back, print both views.
//
//   dotnet run --project examples -- <descriptor-set>

module Roundtrip

open Pbwire.FSharp

[<EntryPoint>]
let main argv =
    let desc =
        if argv.Length > 0 then
            argv[0]
        else
            "../../csharp/tests/data/matrix3.desc"

    use schema = Pbwire.schema desc
    let ty = Pbwire.messageType schema "structtest.Scalars3"
    let innerTy = Pbwire.messageType schema "structtest.Inner3"
    let name = innerTy.Field "name"
    let value = innerTy.Field "value"

    use msg =
        Pbwire.build ty (fun msg ->
            Pbwire.setAll
                msg
                [ "i32", Int (int64 System.Int32.MinValue)
                  "str", Text "alice"
                  "flag", Bool true ]
            |> ignore
            msg.Set("tags", "a") |> ignore
            msg.Set("tags", "bb") |> ignore
            Pbwire.nested msg "inner" (fun inner ->
                inner.Set(name, "kid") |> ignore
                inner.Set(value, 42L) |> ignore)
            |> ignore)

    let wire = Pbwire.encode msg
    use back = Pbwire.parse ty wire
    let innerName =
        Pbwire.tryMessage back "inner"
        |> Option.bind (fun m -> Pbwire.tryString m "name")

    let str = Pbwire.tryString back "str" |> Option.defaultValue "<unset>"
    let kid = innerName |> Option.defaultValue "<unset>"
    printfn $"round trip: %d{wire.Length} bytes, str=%s{str}, inner=%s{kid}"

    if back.Encode() <> wire then
        eprintfn "FAIL round trip differs"
        1
    else
        0

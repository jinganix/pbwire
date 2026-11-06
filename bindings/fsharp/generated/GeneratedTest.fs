// Generated-conformance check for the F# binding: build matrix3.proto's
// Scalars3 through the generated records, encode, require the protoc
// reference bytes (matrix3.bin), then decode those bytes back and check
// every field. The schema travels embedded in tests/gen/matrix3.fs.
//
//   dotnet run --project generated -- <matrix3.bin>

module GeneratedTest

open System
open System.IO

let mutable checks = 0

let check ok what =
    checks <- checks + 1
    if not ok then
        eprintfn $"FAIL %s{what}"
        exit 1

let build () : Matrix3.Structtest_Scalars3 =
    let kid : Matrix3.Structtest_Inner3 = { name = "kid"; value = 42 }
    { i32 = Int32.MinValue
      i64 = Int64.MaxValue
      u32 = UInt32.MaxValue
      u64 = UInt64.MaxValue
      s32 = Int32.MinValue
      s64 = Int64.MinValue
      fx32 = 0xdeadbeefu
      fx64 = 72623859790382856UL
      sfx32 = Int32.MinValue
      sfx64 = Int64.MinValue
      f32 = Single.PositiveInfinity
      f64 = -123456.789
      flag = true
      str = "hello"
      color = 2
      inner = Some kid
      nums = [ 1; -5; 300 ]
      tags = [ "a"; "bb" ]
      inners =
        [ { name = "x"; value = 1 }
          { name = "y"; value = -2 } ]
      opt_i32 = Some -7
      opt_str = Some "opt"
      flags = [ true; false; true ] }

[<EntryPoint>]
let main argv =
    let bin =
        if argv.Length > 0 then argv[0]
        else "../../c/tests/data/matrix3.bin"

    let reference = File.ReadAllBytes bin
    let wire = Matrix3.Structtest_Scalars3.encode (build ())
    check (wire = reference) "generated encode equals the protoc reference bytes"

    let m = Matrix3.Structtest_Scalars3.decode reference
    check (m.i32 = Int32.MinValue) "i32"
    check (m.i64 = Int64.MaxValue) "i64"
    check (m.u32 = UInt32.MaxValue) "u32"
    check (m.u64 = UInt64.MaxValue) "u64"
    check (m.s32 = Int32.MinValue) "s32"
    check (m.s64 = Int64.MinValue) "s64"
    check (m.fx32 = 0xdeadbeefu) "fx32"
    check (m.fx64 = 72623859790382856UL) "fx64"
    check (m.f32 = Single.PositiveInfinity) "f32"
    check (m.f64 = -123456.789) "f64"
    check m.flag "flag"
    check (m.str = "hello") "str"
    check (m.color = 2) "color"
    check (m.inner = Some { name = "kid"; value = 42 }) "inner"
    check (m.nums = [ 1; -5; 300 ]) "nums"
    check (m.tags = [ "a"; "bb" ]) "tags"
    check (m.inners.Length = 2 && m.inners[1].value = -2) "inners"
    check (m.opt_i32 = Some -7) "opt_i32"
    check (m.opt_str = Some "opt") "opt_str"
    check (m.flags = [ true; false; true ]) "flags"

    let empty = Matrix3.Structtest_Scalars3.encode Matrix3.Structtest_Scalars3.defaultValue
    check (empty.Length = 0) "empty generated message encodes to nothing"
    let zero =
        Matrix3.Structtest_Scalars3.decode
            (Matrix3.Structtest_Scalars3.encode { Matrix3.Structtest_Scalars3.defaultValue with opt_i32 = Some 0 })
    check (zero.opt_i32 = Some 0) "explicit zero with presence survives"

    // Dual registration: embedded set by default, explicit init/override.
    Matrix3Meta.preload ()
    check (Matrix3Meta.messageNames.Length = 2) "registry lists every message"
    Matrix3Meta.useSchema null
    check (try Matrix3.Structtest_Scalars3.decode reference |> ignore; false with _ -> true) "cleared schema fails loudly"
    Matrix3Meta.initSchema Matrix3Meta.desc
    check ((Matrix3.Structtest_Scalars3.decode reference).str = "hello") "initSchema restores embedded"

    printfn $"generated test ok: %d{checks} checks"
    0

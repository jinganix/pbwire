// Generated-compat check for the C# binding (TESTSPEC T1–T6, vectors.json as the
// single source of truth):
//
//   T1  generated ToByteArray == pinned protoc reference bytes
//   T2  ParseFrom of the reference reads every fixture value back
//   T3  default() encodes empty; explicit optional zero round-trips
//   T4  differential vs the pinned Google.Protobuf oracle (extern alias
//       Oracle): same bytes, same fields, cross-decode agrees
//   T5  one generic workflow runs against both libraries' messages with
//       identical observable results (import swap only)
//   T6  errors are loud, not crashes: bad wire throws on both sides
//
//   dotnet run --project generated

extern alias Oracle;

using System.Text.Json;
using Google.Protobuf;
using Structtest;
using Spectest;

var root = Path.GetFullPath(Path.Combine(AppContext.BaseDirectory, "..", "..", "..", "..", "..", ".."));
var testdata = Path.Combine(root, "codegen", "testdata");
var cdata = Path.Combine(root, "bindings", "c", "tests", "data");
var reference = File.ReadAllBytes(Path.Combine(cdata, "matrix3.bin"));
var referenceOneof = File.ReadAllBytes(Path.Combine(testdata, "oneof1.bin"));
var vectors = JsonDocument.Parse(File.ReadAllText(Path.Combine(testdata, "vectors.json"))).RootElement.GetProperty("values");
var checks = 0;

void Check(bool ok, string what)
{
    if (!ok)
    {
        Console.Error.WriteLine($"FAIL {what}");
        Environment.Exit(1);
    }
    checks++;
}

long JsonI64(string name) => vectors.GetProperty(name).ValueKind == JsonValueKind.String
    ? long.Parse(vectors.GetProperty(name).GetString()!)
    : vectors.GetProperty(name).GetInt64();

Scalars3 Build() => new Scalars3
{
    I32 = int.MinValue,
    I64 = long.MaxValue,
    U32 = uint.MaxValue,
    U64 = ulong.MaxValue,
    S32 = int.MinValue,
    S64 = long.MinValue,
    Fx32 = 0xdeadbeef,
    Fx64 = 72623859790382856UL,
    Sfx32 = int.MinValue,
    Sfx64 = long.MinValue,
    F32 = float.PositiveInfinity,
    F64 = -123456.789,
    Flag = true,
    Str = "hello",
    Color = Color3.Green,
    Inner = new Inner3 { Name = "kid", Value = 42 },
    Nums = { 1, -5, 300 },
    Tags = { "a", "bb" },
    Inners = { new Inner3 { Name = "x", Value = 1 }, new Inner3 { Name = "y", Value = -2 } },
    OptI32 = -7,
    OptStr = "opt",
    Flags = { true, false, true },
};

Oracle::Structtest.Scalars3 BuildOracle() => new Oracle::Structtest.Scalars3
{
    I32 = int.MinValue,
    I64 = long.MaxValue,
    U32 = uint.MaxValue,
    U64 = ulong.MaxValue,
    S32 = int.MinValue,
    S64 = long.MinValue,
    Fx32 = 0xdeadbeef,
    Fx64 = 72623859790382856UL,
    Sfx32 = int.MinValue,
    Sfx64 = long.MinValue,
    F32 = float.PositiveInfinity,
    F64 = -123456.789,
    Flag = true,
    Str = "hello",
    Color = Oracle::Structtest.Color3.Green,
    Inner = new Oracle::Structtest.Inner3 { Name = "kid", Value = 42 },
    Nums = { 1, -5, 300 },
    Tags = { "a", "bb" },
    Inners = { new Oracle::Structtest.Inner3 { Name = "x", Value = 1 }, new Oracle::Structtest.Inner3 { Name = "y", Value = -2 } },
    OptI32 = -7,
    OptStr = "opt",
    Flags = { true, false, true },
};

// T1: encode equals the protoc reference bytes.
Check(Build().ToByteArray().SequenceEqual(reference), "T1 facade != reference");

// T2: decode reads every fixture value back.
{
    var back = Scalars3.Parser.ParseFrom(reference);
    Check(back.I32 == JsonI64("i32") && back.I64 == JsonI64("i64"), "T2 i32/i64");
    Check(back.U32 == 4294967295 && back.U64 == 18446744073709551615UL, "T2 u32/u64");
    Check(back.S32 == int.MinValue && back.S64 == long.MinValue, "T2 s32/s64");
    Check(back.Fx32 == 0xdeadbeef && back.Fx64 == 72623859790382856UL, "T2 fx32/fx64");
    Check(back.Sfx32 == int.MinValue && back.Sfx64 == long.MinValue, "T2 sfx32/sfx64");
    Check(float.IsPositiveInfinity(back.F32) && back.F64 == -123456.789, "T2 f32/f64");
    Check(back.Flag && back.Str == "hello" && back.Color == Color3.Green, "T2 flag/str/color");
    Check(back.Inner != null && back.Inner.Name == "kid" && back.Inner.Value == 42, "T2 inner");
    Check(back.Nums.Count == 3 && back.Nums[0] == 1 && back.Nums[1] == -5 && back.Nums[2] == 300, "T2 nums");
    Check(back.Tags.Count == 2 && back.Tags[0] == "a" && back.Tags[1] == "bb", "T2 tags");
    Check(back.Inners.Count == 2 && back.Inners[0].Name == "x" && back.Inners[1].Value == -2, "T2 inners");
    Check(back.HasOptI32 && back.OptI32 == -7 && back.HasOptStr && back.OptStr == "opt", "T2 optional");
    Check(back.Flags.Count == 3 && back.Flags[0] && !back.Flags[1] && back.Flags[2], "T2 flags");

    var one = new Oneof1 { Name = "a", Text = "hi" };
    Check(one.KindCase == Oneof1.KindOneofCase.Text, "T2 oneof set tracks case");
    Check(one.ToByteArray().SequenceEqual(referenceOneof), "T1 oneof != reference");
    var ob = Oneof1.Parser.ParseFrom(referenceOneof);
    Check(ob.Name == "a" && ob.Text == "hi" && ob.KindCase == Oneof1.KindOneofCase.Text, "T2 oneof decode");
    var nested = new Oneof1 { Name = "n", Inner = new Oneof1.Types.Inner { V = 7 } };
    var nb = Oneof1.Parser.ParseFrom(nested.ToByteArray());
    Check(nb.Inner != null && nb.Inner.V == 7, "T2 nested oneof member");
}

// T3: empty encodes empty; explicit optional zero round-trips.
Check(new Scalars3().ToByteArray().Length == 0, "T3 empty encodes empty");
{
    var zero = new Scalars3 { OptI32 = 0 };
    var back = Scalars3.Parser.ParseFrom(zero.ToByteArray());
    Check(back.HasOptI32 && back.OptI32 == 0 && !back.HasOptStr, "T3 explicit zero keeps presence");
}

// T4: differential vs the pinned oracle.
Check(BuildOracle().ToByteArray().SequenceEqual(reference), "T4 oracle != reference");
{
    var om = Oracle::Structtest.Scalars3.Parser.ParseFrom(Build().ToByteArray());
    var fm = Scalars3.Parser.ParseFrom(BuildOracle().ToByteArray());
    Check(om.I32 == fm.I32 && om.I64 == fm.I64 && om.U32 == fm.U32 && om.U64 == fm.U64, "T4 ints");
    Check(om.Flag == fm.Flag && om.Str == fm.Str && (int)om.Color == (int)fm.Color, "T4 flag/str/color");
    Check(om.Inner.Name == fm.Inner!.Name && om.OptI32 == fm.OptI32, "T4 inner/opt");
    Check(om.Nums.Count == fm.Nums.Count && om.Inners.Count == fm.Inners.Count, "T4 repeated");
    var po = new Oracle::Spectest.Oneof1 { Name = "a", Text = "hi" };
    Check(po.ToByteArray().SequenceEqual(referenceOneof), "T4 oracle oneof != reference");
}

// T5: one generic workflow against both libraries.
static (byte[] Wire, string Str, int I32, int Nums, string Inner, bool HasOpt) Workflow<T>(
    Func<T> @new, Action<T> set, Func<T, byte[]> enc, Func<byte[], T> dec, Func<T, (string, int, int, string, bool)> get)
{
    var m = @new();
    set(m);
    var wire = enc(m);
    var g = get(dec(wire));
    return (wire, g.Item1, g.Item2, g.Item3, g.Item4, g.Item5);
}

var a = Workflow(
    () => new Oracle::Structtest.Scalars3(),
    m => { m.Str = "kid"; m.I32 = -5; m.Nums.AddRange(new[] { 1, 2 }); m.Inner = new Oracle::Structtest.Inner3 { Name = "x", Value = 1 }; },
    m => m.ToByteArray(),
    b => Oracle::Structtest.Scalars3.Parser.ParseFrom(b),
    m => (m.Str, m.I32, m.Nums.Count, m.Inner.Name, m.HasOptI32));
var f = Workflow(
    () => new Scalars3(),
    m => { m.Str = "kid"; m.I32 = -5; m.Nums.AddRange(new[] { 1, 2 }); m.Inner = new Inner3 { Name = "x", Value = 1 }; },
    m => m.ToByteArray(),
    b => Scalars3.Parser.ParseFrom(b),
    m => (m.Str, m.I32, m.Nums.Count, m.Inner!.Name, m.HasOptI32));
Check(a.Wire.SequenceEqual(f.Wire) && a.Str == f.Str && a.I32 == f.I32 && a.Nums == f.Nums && a.Inner == f.Inner && a.HasOpt == f.HasOpt, "T5 mismatch");

// T6: errors are loud.
static bool Throws(Action fn)
{
    try { fn(); return false; }
    catch { return true; }
}
Check(Throws(() => Scalars3.Parser.ParseFrom(new byte[] { 0xff, 0xff, 0xff })), "T6 bad wire fails");
Check(Throws(() => Oracle::Structtest.Scalars3.Parser.ParseFrom(new byte[] { 0xff, 0xff, 0xff })), "T6 oracle bad wire fails");
Check(Scalars3.Parser.ParseFrom(Array.Empty<byte>()).Equals(new Scalars3()), "T6 empty decodes to default");
{
    var one = new Oneof1 { Name = "n", Num = 1 };
    Check(Oneof1.Parser.ParseFrom(one.ToByteArray()).Num == 1, "T6 oneof member round-trips");
}

// Dual registration: embedded set by default, external file via UseSchema.
Spectest.PbwireFile_Oneof1.Preload();
Check(Spectest.PbwireFile_Oneof1.MessageNames.Length == 2, "oneof registry lists every message");
{
    using var ext = Pbwire.Schema.FromFile(Path.Combine(testdata, "oneof1.desc"));
    Spectest.PbwireFile_Oneof1.UseSchema(ext);
    var back = Spectest.Oneof1.Parser.ParseFrom(referenceOneof);
    Check(back.Name == "a" && back.Text == "hi", "external schema decodes");
    Check(back.ToByteArray().SequenceEqual(referenceOneof), "external schema re-encodes");
    Spectest.PbwireFile_Oneof1.UseSchema(null);
    Check(Throws(() => Spectest.Oneof1.Parser.ParseFrom(referenceOneof)), "cleared override fails loudly");
    Spectest.PbwireFile_Oneof1.Init();
    Check(Spectest.Oneof1.Parser.ParseFrom(referenceOneof).Name == "a", "Init restores embedded");
}

Console.WriteLine($"compat: {checks} checks passed");

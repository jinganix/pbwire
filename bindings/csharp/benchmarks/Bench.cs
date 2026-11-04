// C# binding benchmark against the official Google.Protobuf C# runtime and
// generated code on the same matrix3.proto and the four standard tiers from
// BENCHMARKS.md.
//
// The same payload as the C/Go/Java/Rust rows: every scalar kind, the nested
// message, the two proto3 optionals, and the repeated fields scaled x0 / x1 /
// x100 / x3000, which lands the tiers on 136 / 174 / 2469 / 74913 bytes. Both
// sides must produce identical bytes before anything is timed, and both
// allocate per call (pbwire Encode()/Decode() against ToByteArray()/
// ParseFrom()), so the comparison is like for like.
//
//   ./bench.sh          generate, build, run

using System.Diagnostics;
using Google.Protobuf;
using Pbwire;
using Structtest;

var cases = new[] { "small", "medium", "large", "xlarge" };
var repeats = new[] { 0, 1, 100, 3000 };
var want = new[] { 136, 174, 2469, 74913 };
var iterations = new[] { 50_000, 50_000, 5_000, 500 };
const int passes = 5;
var failures = 0;

if (args.Length != 1)
{
    Console.Error.WriteLine("usage: Bench <descriptor-set>");
    return 2;
}

using var schema = Schema.FromFile(args[0]);
var ty = schema.Message("structtest.Scalars3");
var innerTy = schema.Message("structtest.Inner3");

for (var tier = 0; tier < cases.Length; tier++)
{
    var label = cases[tier];
    var n = repeats[tier];
    var count = iterations[tier];

    var reference = FillOfficial(n).ToByteArray();
    if (reference.Length != want[tier])
    {
        Fail(label, $"official encoder wrote {reference.Length} bytes, expected {want[tier]}");
        continue;
    }
    using var message = FillPbwire(ty, innerTy, n);
    var got = message.Encode();
    if (!got.SequenceEqual(reference))
    {
        Fail(label, $"pbwire-csharp wrote {got.Length} bytes, protobuf-csharp {reference.Length}");
        continue;
    }
    Console.WriteLine($"OK   {label} encoding parity, x{n} ({reference.Length} bytes)");

    var official = FillOfficial(n);
    var encode = Measure(count, () => message.Encode());
    var decode = Measure(count, () =>
    {
        using var decoded = ty.Decode(reference);
    });
    var officialEncode = Measure(count, () => official.ToByteArray());
    var officialDecode = Measure(count, () => Scalars3.Parser.ParseFrom(reference));

    Console.WriteLine($"{label}: {reference.Length} bytes, {count} iterations (repeat field x{n})");
    Console.WriteLine($"  {"impl",-12} {"op",-7} {"ns/op",12} {"MB/s",10}");
    Row("pbwire-csharp", "encode", reference.Length, encode);
    Row("protobuf-csharp", "encode", reference.Length, officialEncode);
    Row("pbwire-csharp", "decode", reference.Length, decode);
    Row("protobuf-csharp", "decode", reference.Length, officialDecode);
    Console.WriteLine();
}

if (failures != 0)
{
    Console.Error.WriteLine($"{failures} check(s) failed");
    return 1;
}
return 0;

static Scalars3 FillOfficial(int n)
{
    var m = new Scalars3
    {
        I32 = int.MinValue,
        I64 = long.MaxValue,
        U32 = uint.MaxValue,
        U64 = ulong.MaxValue,
        S32 = int.MinValue,
        S64 = long.MinValue,
        Fx32 = 0xdeadbeef,
        Fx64 = 72623859790382856,
        Sfx32 = int.MinValue,
        Sfx64 = long.MinValue,
        F32 = float.PositiveInfinity,
        F64 = -123456.789,
        Flag = true,
        Str = "hello",
        Color = Color3.Green,
        Inner = new Inner3 { Name = "kid", Value = 42 },
        OptI32 = -7,
        OptStr = "opt",
    };
    for (var i = 0; i < n; i++)
    {
        m.Nums.Add(i - 3);
        m.Tags.Add("tag");
        m.Inners.Add(new Inner3 { Name = "grandkid", Value = i + 1 });
        m.Flags.Add(i % 2 == 0);
    }
    return m;
}

static Message FillPbwire(MessageType ty, MessageType innerTy, int n)
{
    var name = innerTy.Field("name");
    var value = innerTy.Field("value");
    var msg = ty.NewMessage()
        .Set("i32", int.MinValue)
        .Set("i64", long.MaxValue)
        .Set("u32", uint.MaxValue)
        .Set("u64", -1L)
        .Set("s32", int.MinValue)
        .Set("s64", long.MinValue)
        .Set("fx32", 0xdeadbeefL)
        .Set("fx64", 72623859790382856L)
        .Set("sfx32", int.MinValue)
        .Set("sfx64", long.MinValue)
        .Set("f32", float.PositiveInfinity)
        .Set("f64", -123456.789)
        .Set("flag", true)
        .Set("str", "hello")
        .Set("color", 2L)
        .Set("opt_i32", -7L)
        .Set("opt_str", "opt");

    msg.SetMessage("inner").Set(name, "kid").Set(value, 42L);
    for (var i = 0; i < n; i++)
    {
        msg.Set("nums", (long)(i - 3)).Set("tags", "tag").Set("flags", i % 2 == 0);
        msg.SetMessage("inners").Set(name, "grandkid").Set(value, (long)(i + 1));
    }
    return msg;
}

static double Run(int iterations, Action op)
{
    var watch = Stopwatch.StartNew();
    for (var i = 0; i < iterations; i++)
    {
        op();
    }
    return watch.Elapsed.TotalNanoseconds / iterations;
}

// One warm-up pass, then the best of `passes`.
static double Measure(int iterations, Action op)
{
    Run(iterations, op);
    var best = double.MaxValue;
    for (var pass = 0; pass < passes; pass++)
    {
        best = Math.Min(best, Run(iterations, op));
    }
    return best;
}

static void Row(string impl, string op, int bytes, double ns) =>
    Console.WriteLine($"  {impl,-12} {op,-7} {ns,12:F1} {bytes / ns * 1000.0,10:F1}");

void Fail(string label, string what)
{
    Console.WriteLine($"FAIL {label}: {what}");
    failures++;
}

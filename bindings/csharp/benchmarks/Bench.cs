// C# binding benchmark against the official Google.Protobuf C# runtime and
// generated code, plus protobuf-net (the fastest widely used C# runtime),
// on the same matrix3.proto and the four standard tiers from BENCHMARKS.md.
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
// Floor iterations per tier; doubled until one pass takes 0.25 s.
var iterations = new[] { 50_000, 50_000, 5_000, 500 };
const double MinPassSeconds = 0.25;
const int passes = 3;
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

    var pbn = FillPbn(n);
    var pbnBytes = PbnEncode(pbn);
    // protobuf-net writes single-element packed fields in singular form: valid
    // protobuf and semantically equal, but not byte-identical to the canonical
    // packed encoding, so those tiers carry no protobuf-net rows.
    var pbnOk = pbnBytes.SequenceEqual(reference);
    if (!pbnOk && !Scalars3.Parser.ParseFrom(pbnBytes).Equals(FillOfficial(n)))
    {
        Fail(label, $"protobuf-net wrote {pbnBytes.Length} bytes, protobuf-csharp {reference.Length}");
        continue;
    }
    if (!pbnOk)
    {
        Console.WriteLine($"NOTE   {label}: protobuf-net singular-encodes single-element packed fields ({pbnBytes.Length} vs {reference.Length} bytes); its rows skipped");
    }

    var official = FillOfficial(n);
    // Tier line shows the reference op's count; slower ops keep the floor.
    var tierCount = Calibrate(() => message.Encode(), count);
    var encode = Measure(count, () => message.Encode());
    var decode = Measure(count, () =>
    {
        using var decoded = ty.Decode(reference);
    });
    var officialEncode = Measure(count, () => official.ToByteArray());
    var pbnEncode = pbnOk ? Measure(count, () => PbnEncode(pbn)) : 0;
    var officialDecode = Measure(count, () => Scalars3.Parser.ParseFrom(reference));
    var pbnDecode = pbnOk ? Measure(count, () => PbnDecode(reference)) : 0;

    Console.WriteLine($"{label}: {reference.Length} bytes, {tierCount} iterations (repeat field x{n})");
    Console.WriteLine($"  {"impl",-12} {"op",-7} {"ns/op",12} {"MB/s",10}");
    Row("pbwire-csharp", "encode", reference.Length, encode);
    Row("protobuf-csharp", "encode", reference.Length, officialEncode);
    if (pbnOk) Row("protobuf-net", "encode", reference.Length, pbnEncode);
    Row("pbwire-csharp", "decode", reference.Length, decode);
    Row("protobuf-csharp", "decode", reference.Length, officialDecode);
    if (pbnOk) Row("protobuf-net", "decode", reference.Length, pbnDecode);
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

static Pbn.Scalars3 FillPbn(int n)
{
    var m = new Pbn.Scalars3
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
        Color = 2,
        Inner = new Pbn.Inner3 { Name = "kid", Value = 42 },
        OptI32 = -7,
        OptStr = "opt",
    };
    // Null (not empty) repeated fields encode to nothing, matching the
    // reference bytes: protobuf-net writes a zero-length header for an
    // explicitly-packed empty list.
    m.Nums = n == 0 ? null : new List<int>(n);
    m.Flags = n == 0 ? null : new List<bool>(n);
    for (var i = 0; i < n; i++)
    {
        m.Nums!.Add(i - 3);
        m.Tags.Add("tag");
        m.Inners.Add(new Pbn.Inner3 { Name = "grandkid", Value = i + 1 });
        m.Flags!.Add(i % 2 == 0);
    }
    return m;
}

static byte[] PbnEncode(Pbn.Scalars3 m)
{
    using var ms = new MemoryStream();
    ProtoBuf.Serializer.Serialize(ms, m);
    return ms.ToArray();
}

static Pbn.Scalars3 PbnDecode(byte[] bytes)
{
    using var ms = new MemoryStream(bytes, false);
    return ProtoBuf.Serializer.Deserialize<Pbn.Scalars3>(ms);
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

// Double from the tier floor until one pass takes MinPassSeconds.
static int Calibrate(Action op, int floor)
{
    var count = Math.Max(1, floor);
    for (;;)
    {
        var watch = Stopwatch.StartNew();
        for (var i = 0; i < count; i++)
        {
            op();
        }
        if (watch.Elapsed.TotalSeconds >= MinPassSeconds || count >= 100_000_000)
        {
            return count;
        }
        count *= 2;
    }
}

// Every op calibrates independently from the floor (a slow op already exceeds
// the minimum at the floor and keeps it), then one warm-up pass and the best
// of `passes`.
static double Measure(int floor, Action op)
{
    var count = Calibrate(op, floor);
    Run(count, op);
    var best = double.MaxValue;
    for (var pass = 0; pass < passes; pass++)
    {
        best = Math.Min(best, Run(count, op));
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

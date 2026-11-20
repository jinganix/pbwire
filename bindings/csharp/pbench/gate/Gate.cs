extern alias Oracle;

// C# pbench: the gate (byte parity plus a field compare, all four tiers) and
// the provisional table (interleaved rounds, median of five). The arms are the
// ones in ../../README.md#pbench-three-arms-one-contract:
//
//   tree  the prebuilt Pbwire.Message handle: message.Encode(), type.Decode
//   shim  the generated model as the native object: Scalars3Layout.Marshal /
//         .Unmarshal, one call per op (see the README for the deviation: the
//         core has no offset reader, so the walk is the model's shipped one)
//   core  n/a: plan 16
//
//   dotnet run --project gate -c Release            gate, exit 0 or 1
//   dotnet run --project gate -c Release -- compare compare table after the gate

using System.Diagnostics;
using Google.Protobuf;
using Pbwire;
using Pbwire.Pbench;
using Structtest;
using Official = Oracle::Structtest;

var compare = args.Length > 0 && args[0] == "compare";
var desc = args.Length > (compare ? 1 : 0) ? args[compare ? 1 : 0] : FindDesc();
if (!File.Exists(desc))
{
    Console.Error.WriteLine($"descriptor set not found: {desc}");
    return 2;
}

var cases = new[] { "small", "medium", "large", "xlarge" };
var repeats = new[] { 0, 1, 100, 3000 };
var want = new[] { 136, 174, 2469, 74913 };

using var schema = Schema.FromFile(desc);
var scalars = schema.Message("structtest.Scalars3");
var inner = schema.Message("structtest.Inner3");

// A wrong offset table fails here instead of producing bad bytes.
Scalars3.Scalars3Layout.Table.Verify(scalars);
Inner3.Inner3Layout.Table.Verify(inner);

var failures = 0;
var refs = new byte[want.Length][];
for (var tier = 0; tier < cases.Length; tier++)
{
    var label = cases[tier];
    var n = repeats[tier];
    var reference = FillOfficial(n).ToByteArray();
    refs[tier] = reference;
    if (reference.Length != want[tier])
    {
        Fail(label, $"official wrote {reference.Length} bytes, expected {want[tier]}");
        continue;
    }

    // tree arm: prebuilt handle.
    using var message = FillPbwire(scalars, inner, n);
    if (!message.Encode().SequenceEqual(reference))
    {
        Fail(label, "tree encode differs from Google.Protobuf");
        continue;
    }

    // shim arm: the generated model is the native object.
    var native = Matrix3Shim.New(n);
    if (!Matrix3Shim.Marshal(native).SequenceEqual(reference))
    {
        Fail(label, "shim encode differs from Google.Protobuf");
        continue;
    }

    // decode: fields plus a byte-identical re-encode, both arms.
    var back = Matrix3Shim.Unmarshal(reference);
    var model = (Scalars3)back;
    var modelOk = model.Str == "hello"
        && model.Inner?.Name == "kid"
        && model.Inner?.Value == 42
        && model.Nums.Count == n
        && (n == 0 || model.Nums[n - 1] == n - 4)
        && model.Tags.Count == n
        && model.Flags.Count == n
        && model.HasOptI32 && model.OptI32 == -7
        && model.HasOptStr && model.OptStr == "opt"
        && model.U64 == ulong.MaxValue
        && model.Color == Color3.Green
        && Matrix3Shim.Marshal(back).SequenceEqual(reference);
    if (!modelOk)
    {
        Fail(label, "shim decode differs");
        continue;
    }

    using (var treeBack = scalars.Decode(reference))
    {
        var treeOk = treeBack.Encode().SequenceEqual(reference)
            && treeBack.GetString("str") == "hello"
            && treeBack.GetMessage("inner")?.GetString("name") == "kid"
            && treeBack.GetMessage("inner")?.GetInt("value") == 42
            && treeBack.Count("nums") == n
            && (n == 0 || treeBack.At("nums", n - 1)?.AsLong() == n - 4)
            && treeBack.Has("opt_i32") && treeBack.GetInt("opt_i32") == -7
            && treeBack.Has("opt_str") && treeBack.GetString("opt_str") == "opt"
            && treeBack.GetLong("u64") == -1
            && treeBack.GetInt("color") == 2;
        if (!treeOk)
        {
            Fail(label, "tree decode differs");
            continue;
        }
    }

    Console.WriteLine($"OK   {label} parity + field compare, x{n} ({reference.Length} bytes)");
}

if (failures != 0)
{
    Console.Error.WriteLine($"FAIL {failures} check(s) failed");
    return 1;
}
Console.WriteLine("PASS");
if (!compare)
{
    return 0;
}

// Interleaved rounds: every arm plus the baseline back to back, median of five.
var floors = new[] { 20_000, 20_000, 2_000, 200 };
for (var tier = 0; tier < cases.Length; tier++)
{
    var n = repeats[tier];
    var reference = refs[tier];
    var count = floors[tier];
    using var message = FillPbwire(scalars, inner, n);
    var native = Matrix3Shim.New(n);
    var official = FillOfficial(n);

    var ops = new (string Impl, string Op, Action Run)[]
    {
        ("pbwire-csharp-tree", "encode", () => message.Encode()),
        ("pbwire-csharp-shim", "encode", () => Matrix3Shim.Marshal(native)),
        ("protobuf-csharp", "encode", () => official.ToByteArray()),
        ("pbwire-csharp-tree", "decode", () => scalars.Decode(reference).Dispose()),
        ("pbwire-csharp-shim", "decode", () => Matrix3Shim.Unmarshal(reference)),
        ("protobuf-csharp", "decode", () => Official.Scalars3.Parser.ParseFrom(reference)),
    };
    var rounds = new double[ops.Length, 5];
    for (var round = 0; round < 5; round++)
    {
        for (var i = 0; i < ops.Length; i++)
        {
            var watch = Stopwatch.StartNew();
            for (var k = 0; k < count; k++)
            {
                ops[i].Run();
            }
            rounds[i, round] = watch.Elapsed.TotalNanoseconds / count;
        }
    }
    Console.WriteLine($"{cases[tier]}: {reference.Length} bytes, {count} iterations (repeat field x{n})");
    Console.WriteLine($"  {"impl",-20} {"op",-7} {"ns/op",12} {"MB/s",10} {"median of 5",12}");
    foreach (var (impl, op, _) in ops)
    {
        var i = Array.FindIndex(ops, o => o.Impl == impl && o.Op == op);
        var five = new double[5];
        for (var r = 0; r < 5; r++)
        {
            five[r] = rounds[i, r];
        }
        Array.Sort(five);
        var ns = five[2];
        Console.WriteLine($"  {impl,-20} {op,-7} {ns,12:F1} {reference.Length / ns * 1000.0,10:F1} {five[4] / five[0],12:F2}");
    }
    Console.WriteLine();
}
return 0;

void Fail(string label, string what)
{
    Console.WriteLine($"FAIL {label}: {what}");
    failures++;
}

static string FindDesc()
{
    for (var dir = new DirectoryInfo(Directory.GetCurrentDirectory()); dir != null; dir = dir.Parent)
    {
        var candidate = Path.Combine(dir.FullName, "bindings", "c", "tests", "data", "matrix3.desc");
        if (File.Exists(candidate))
        {
            return candidate;
        }
    }
    return "bindings/c/tests/data/matrix3.desc";
}

static Official.Scalars3 FillOfficial(int n)
{
    var m = new Official.Scalars3
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
        Color = Official.Color3.Green,
        Inner = new Official.Inner3 { Name = "kid", Value = 42 },
        OptI32 = -7,
        OptStr = "opt",
    };
    for (var i = 0; i < n; i++)
    {
        m.Nums.Add(i - 3);
        m.Tags.Add("tag");
        m.Inners.Add(new Official.Inner3 { Name = "grandkid", Value = i + 1 });
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

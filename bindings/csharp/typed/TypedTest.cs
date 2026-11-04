// Typed-conformance check: build structtest.Scalars3 through the generated
// classes, encode, require the protoc reference bytes (matrix3.bin), then
// decode those bytes back and check every field. Same flow as every binding.
//
//   dotnet run --project typed -- ../c/tests/data/matrix3.desc

using Pbwire;
using Pbwire.Generated;

var desc = args.Length > 0 ? args[0] : "bindings/c/tests/data/matrix3.desc";
var data = Path.GetDirectoryName(Path.GetFullPath(desc))!;
var reference = File.ReadAllBytes(Path.Combine(data, "matrix3.bin"));
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

Structtest_Scalars3 Build()
{
    var m = new Structtest_Scalars3
    {
        I32 = int.MinValue,
        I64 = long.MaxValue,
        U32 = -1,
        U64 = -1L,
        S32 = int.MinValue,
        S64 = long.MinValue,
        Fx32 = unchecked((int)0xdeadbeef),
        Fx64 = 72623859790382856L,
        Sfx32 = int.MinValue,
        Sfx64 = long.MinValue,
        F32 = float.PositiveInfinity,
        F64 = -123456.789,
        Flag = true,
        Str = "hello",
        Color = Structtest_Color3.COLOR3_GREEN,
        Inner = new Structtest_Inner3 { Name = "kid", Value = 42 },
        HasOptI32 = true,
        OptI32 = -7,
        HasOptStr = true,
        OptStr = "opt",
    };
    m.Nums.AddRange(new[] { 1, -5, 300 });
    m.Tags.AddRange(new[] { "a", "bb" });
    m.Inners.Add(new Structtest_Inner3 { Name = "x", Value = 1 });
    m.Inners.Add(new Structtest_Inner3 { Name = "y", Value = -2 });
    m.Flags.AddRange(new[] { true, false, true });
    return m;
}

void Verify(Structtest_Scalars3 m)
{
    Check(m.I32 == int.MinValue, "i32");
    Check(m.I64 == long.MaxValue, "i64");
    Check(m.U32 == -1, "u32");
    Check(m.U64 == -1L, "u64");
    Check(m.S32 == int.MinValue, "s32");
    Check(m.S64 == long.MinValue, "s64");
    Check(m.Fx32 == unchecked((int)0xdeadbeef), "fx32");
    Check(m.Fx64 == 72623859790382856L, "fx64");
    Check(m.Sfx32 == int.MinValue, "sfx32");
    Check(m.Sfx64 == long.MinValue, "sfx64");
    Check(float.IsPositiveInfinity(m.F32), "f32");
    Check(m.F64 == -123456.789, "f64");
    Check(m.Flag, "flag");
    Check(m.Str == "hello", "str");
    Check(m.Color == Structtest_Color3.COLOR3_GREEN, "color");
    Check(m.Inner is { Name: "kid", Value: 42 }, "inner");
    Check(m.Nums.SequenceEqual(new[] { 1, -5, 300 }), "nums");
    Check(m.Tags.SequenceEqual(new[] { "a", "bb" }), "tags");
    Check(m.Inners.Count == 2 && m.Inners[1].Value == -2, "inners");
    Check(m.HasOptI32 && m.OptI32 == -7, "opt_i32");
    Check(m.HasOptStr && m.OptStr == "opt", "opt_str");
    Check(m.Flags.SequenceEqual(new[] { true, false, true }), "flags");
}

using var schema = Schema.FromFile(desc);
var typed = Build();
var wire = typed.Encode(schema);
Check(wire.AsSpan().SequenceEqual(reference), "typed encode equals the protoc reference bytes");
Verify(Structtest_Scalars3.Decode(schema, reference));
Check(new Structtest_Scalars3().Encode(schema).Length == 0, "empty typed message is empty");

var zero = new Structtest_Scalars3 { HasOptI32 = true, OptI32 = 0 };
var zeroBack = Structtest_Scalars3.Decode(schema, zero.Encode(schema));
Check(zeroBack.HasOptI32 && zeroBack.OptI32 == 0, "explicit zero presence round-trips");

Console.WriteLine($"TypedTest: {checks} checks passed");

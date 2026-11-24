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
    // Clone/MergeFrom must copy the active oneof member (each setter resets
    // the case word, so assigning all members would clear it), and cloning a
    // message member must not alias it.
    var cloned = one.Clone();
    Check(cloned.KindCase == Oneof1.KindOneofCase.Text && cloned.Text == "hi",
          "T2 clone keeps the oneof member");
    var merged = new Oneof1();
    merged.MergeFrom(one);
    Check(merged.KindCase == Oneof1.KindOneofCase.Text && merged.Text == "hi",
          "T2 MergeFrom keeps the oneof member");
    var innerClone = nested.Clone();
    Check(innerClone.Inner != null && innerClone.Inner.V == 7
          && !ReferenceEquals(innerClone.Inner, nested.Inner),
          "T2 clone deep-copies a oneof message member");
    // An absent singular message field must hash without a NullReference.
    Check(new Scalars3().GetHashCode() != 0, "T2 hash of an absent message field");
    // Float equality is bitwise, like protoc: NaN equals NaN, +0.0 != -0.0.
    var nanA = new Scalars3 { F32 = float.NaN };
    var nanB = new Scalars3 { F32 = float.NaN };
    Check(nanA.Equals(nanB) && nanA.GetHashCode() == nanB.GetHashCode(),
          "T2 NaN equals and hashes");
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

// T7: struct-path (Layout.Unmarshal, the shim decode arm) edge semantics over
// hand-built wire. Same rules as the reference parser, pinned here because
// the image reader is a second decoder: last-wins, sub-message merge, oneof
// reset, packed/unpacked/mixed repetition, malformed input throwing loudly.
// Two deliberate divergences, both documented in pbench/README.md: a packed
// run for a singular scalar is skipped like the core decoder (PHP rule; Ruby
// rejects it instead), and unknown fields decode fine but do not survive a
// re-encode (the flat image keeps no unknown storage). Deep-nesting and
// missing-required have no fixture here (no recursive or proto2 message in
// the C# models): the limits live in the shared C codec, covered by its own
// tests, and every C# message here is proto3 without required fields.
static byte[] V7(uint v)
{
    var b = new List<byte>();
    while (v >= 0x80) { b.Add((byte)(v | 0x80)); v >>= 7; }
    b.Add((byte)v);
    return b.ToArray();
}
static byte[] Tag7(int fnum, int wt) => V7((uint)(fnum << 3 | wt));
static byte[] Len7(byte[] p) => V7((uint)p.Length);
static byte[] Cat7(params byte[][] parts) => parts.SelectMany(p => p).ToArray();
static byte[] LF7(int fnum, byte[] payload) => Cat7(Tag7(fnum, 2), Len7(payload), payload);
{
    // Last duplicate wins for a singular scalar.
    var w = Cat7(Tag7(1, 0), V7(1), Tag7(1, 0), V7(2));
    Check(Scalars3.Scalars3Layout.Unmarshal(w).I32 == 2, "T7 last duplicate wins");
    // A repeated singular sub-message merges field by field.
    var nm = new byte[] { (byte)'a' };
    var m1 = Cat7(Tag7(1, 2), Len7(nm), nm);
    var m2 = Cat7(Tag7(2, 0), V7(3));
    var both = Scalars3.Scalars3Layout.Unmarshal(Cat7(LF7(16, m1), LF7(16, m2)));
    Check(both.Inner?.Name == "a" && both.Inner?.Value == 3, "T7 sub-message merges");
    // Packed, unpacked and mixed repetition concatenate.
    var mixed = Scalars3.Scalars3Layout.Unmarshal(
        Cat7(LF7(17, new byte[] { 3, 4 }), Tag7(17, 0), V7(6), LF7(17, new byte[] { 7 })));
    Check(mixed.Nums.Count == 4 && mixed.Nums[3] == 7, "T7 packed/unpacked mix");
    var flags = Scalars3.Scalars3Layout.Unmarshal(Cat7(Tag7(22, 0), V7(1), LF7(22, new byte[] { 0 })));
    Check(flags.Flags.Count == 2 && flags.Flags[0] && !flags.Flags[1], "T7 unpacked/packed flags");
    // A packed run for a singular scalar is skipped (core parity: PHP skips
    // it too, Ruby rejects it; neither keeps the value).
    Check(Scalars3.Scalars3Layout.Unmarshal(LF7(1, V7(9))).I32 == 0, "T7 packed-for-singular skips");
    // Unknown fields read fine but do not survive a re-encode.
    var withUnknown = Cat7(Tag7(999, 0), V7(1), Tag7(1, 0), V7(5));
    var unk = Scalars3.Scalars3Layout.Unmarshal(withUnknown);
    Check(unk.I32 == 5, "T7 unknown field skips");
    Check(Scalars3.Scalars3Layout.Marshal(unk).Length == 2, "T7 unknown field drops on re-encode");
    // Truncated wire throws loudly instead of reading past the end.
    Check(Throws(() => Scalars3.Scalars3Layout.Unmarshal(new byte[] { 0x08, 0x80 })), "T7 truncated varint throws");
    var ab2 = new byte[] { (byte)'a', (byte)'b' };
    Check(Throws(() => Scalars3.Scalars3Layout.Unmarshal(Cat7(Tag7(14, 2), V7(10), ab2))), "T7 length overrun throws");
    // Oneof: the last member on the wire wins and the case follows it.
    var hi = new byte[] { (byte)'h', (byte)'i' };
    var numThenText = Oneof1.Oneof1Layout.Unmarshal(Cat7(Tag7(2, 0), V7(7), LF7(3, hi)));
    Check(numThenText.KindCase == Oneof1.KindOneofCase.Text && numThenText.Text == "hi", "T7 oneof last wins");
    var textThenNum = Oneof1.Oneof1Layout.Unmarshal(Cat7(LF7(3, hi), Tag7(2, 0), V7(7)));
    Check(textThenNum.KindCase == Oneof1.KindOneofCase.Num && textThenNum.Num == 7, "T7 oneof resets");
    // Non-ASCII strings take the ImageText fallback and round-trip exactly.
    var snow = new Scalars3 { Str = "h\u00e9llo \u2603" };
    var snowBack = Scalars3.Scalars3Layout.Unmarshal(Scalars3.Scalars3Layout.Marshal(snow));
    Check(snowBack.Str == snow.Str, "T7 non-ASCII string round-trips");
    // Invalid UTF-8 in a string field throws on both sides (parity: the C
    // struct codec validates proto3 strings like the reference parser, so
    // the ImageText fallback never sees these bytes from the wire).
    var badStr = new byte[] { 0xff, 0xfe };
    var badWire = Cat7(Tag7(14, 2), Len7(badStr), badStr);
    var shimThrew = false;
    try { Scalars3.Scalars3Layout.Unmarshal(badWire); } catch (Pbwire.PbwireException) { shimThrew = true; }
    var officialThrew = false;
    try { Oracle::Structtest.Scalars3.Parser.ParseFrom(badWire); } catch (InvalidProtocolBufferException) { officialThrew = true; }
    Check(shimThrew && officialThrew, "T7 invalid UTF-8 throws on both sides");
    // Empty wire is the default message.
    Check(Scalars3.Scalars3Layout.Unmarshal(Array.Empty<byte>()).Equals(new Scalars3()), "T7 empty decodes to default");
}

Console.WriteLine($"compat: {checks} checks passed");

// Self-check against the pinned protoc bytes: build structtest.Scalars3 from
// bindings/c/tests/data/matrix3.txt, encode, compare with matrix3.bin (the same
// oracle the C binding uses), then decode it back and check the values.
//
//   dotnet run --project tests -- <descriptor-set>

using Pbwire;

var desc = args.Length > 0 ? args[0] : "../../c/tests/data/matrix3.desc";
var oracle = File.ReadAllBytes(Path.Combine(Path.GetDirectoryName(Path.GetFullPath(desc))!, "matrix3.bin"));
var checks = 0;

void Check(bool ok, string what)
{
    checks++;
    if (!ok)
    {
        Console.Error.WriteLine($"FAIL {what}");
        Environment.Exit(1);
    }
}

byte[] Fill(MessageType ty, MessageType innerTy)
{
    using var msg = ty.NewMessage();
    var name = innerTy.Field("name");
    var value = innerTy.Field("value");

    msg.Set("i32", int.MinValue)
        .Set("i64", long.MaxValue)
        .Set("u32", uint.MaxValue)
        .Set("u64", -1L) // u64::MAX bits
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

    foreach (var n in new long[] { 1, -5, 300 })
    {
        msg.Set("nums", n);
    }
    foreach (var tag in new[] { "a", "bb" })
    {
        msg.Set("tags", tag);
    }
    foreach (var (innerName, innerValue) in new[] { ("x", 1L), ("y", -2L) })
    {
        msg.SetMessage("inners").Set(name, innerName).Set(value, innerValue);
    }
    foreach (var flag in new[] { true, false, true })
    {
        msg.Set("flags", flag);
    }
    msg.SetMessage("inner").Set(name, "kid").Set(value, 42L);
    return msg.Encode();
}

using var schema = Schema.FromFile(desc);
var ty = schema.Message("structtest.Scalars3");
var innerTy = schema.Message("structtest.Inner3");

var wire = Fill(ty, innerTy);
Check(wire.SequenceEqual(oracle), $"encoded bytes differ from protoc ({wire.Length} vs {oracle.Length})");
Console.WriteLine($"OK   encode matches protoc, {oracle.Length} bytes");

using (var decoded = ty.Decode(oracle))
{
    Check(decoded.GetInt("i32") == int.MinValue, "i32");
    Check(decoded.GetBool("flag"), "flag");
    Check(decoded.GetString("str") == "hello", "str");
    Check(decoded.GetDouble("f64") == -123456.789, "f64");
    Check(decoded.GetLong("color") == 2, "color");
    Check(decoded.GetLong("opt_i32") == -7, "opt_i32");
    Check(decoded.Has("opt_str") && !ty.NewMessage().Has("i32"), "presence");
    Check(decoded.Count("nums") == 3, "nums count");
    Check(decoded.At("nums", 1)?.AsLong() == -5, "nums[1]");
    Check(decoded.At("tags", 1)?.AsString() == "bb", "tags[1]");
    Check(decoded.At("inners", 1)?.AsMessage()?.GetLong("value") == -2, "inners[1].value");
    Check(decoded.At("flags", 1)?.AsBool() == false, "flags[1]");
    Check(decoded.GetMessage("inner")?.GetString("name") == "kid", "inner.name");
    Check(decoded.Encode().SequenceEqual(oracle), "decode -> encode is byte-exact");
    Console.WriteLine("OK   decode round trip, repeated and nested fields");
}

// A field the schema does not declare (99, varint 7) survives re-encoding.
var unknown = new byte[] { 0x98, 0x06, 0x07 };
using (var innerOnly = innerTy.Decode(unknown))
{
    Check(innerOnly.Encode().SequenceEqual(unknown), "unknown field preserved");
}

// Bad input is an error, not a crash.
Check(Throws(() => ty.Decode(new byte[] { 0x80 })), "truncated tag accepted");
Check(Throws(() => schema.Message("structtest.Nope")), "unknown message accepted");
Check(Throws(() => ty.Field("nope")), "unknown field accepted");

// The cases a binding can get wrong even when the core is right: payloads with
// embedded NULs / invalid UTF-8, non-ASCII text, a negative enum, an empty
// repeated element, proto3 optional presence with an empty value, and oneof
// switching. tests/data/edge.bin is protoc --encode.
// Walk up to the checkout: `dotnet run` keeps the caller's working directory.
static string FindRepo()
{
    for (var dir = new DirectoryInfo(Directory.GetCurrentDirectory()); dir != null; dir = dir.Parent)
    {
        if (File.Exists(Path.Combine(dir.FullName, "tests/data/edge.desc")))
        {
            return dir.FullName;
        }
    }
    throw new InvalidOperationException("no pbwire checkout above the working directory");
}

var root = FindRepo();
var edgeOracle = File.ReadAllBytes(Path.Combine(root, "tests/data/edge.bin"));
using var edgeSchema = Schema.FromFile(Path.Combine(root, "tests/data/edge.desc"));
var edgeTy = edgeSchema.Message("edge.Edge");
var edgeInner = edgeSchema.Message("edge.Inner");

using (var edge = edgeTy.NewMessage())
{
    edge.Set("raw", new byte[] { 0x00, 0x01, 0xff })
        .Set("text", "héllo ☃")
        .Set("sign", -3L)
        .Set("maybe", "")
        .Set("pick_num", 5L) // oneof: replaced below
        .Set("pick_str", "one");
    foreach (var tag in new[] { "a", "", "b" })
    {
        edge.Set("tags", tag);
    }
    edge.Set("blobs", new byte[] { 0x00 });
    edge.Set("blobs", Array.Empty<byte>());
    edge.SetMessage("inner").Set("tag", "kid").Set("n", -1L);

    Check(edge.Encode().SequenceEqual(edgeOracle), "edge payload differs from protoc");
}

using (var edge = edgeTy.Decode(edgeOracle))
{
    Check(edge.GetBytes("raw")!.SequenceEqual(new byte[] { 0x00, 0x01, 0xff }), "bytes with NUL and 0xff");
    Check(edge.GetString("text") == "héllo ☃", "non-ASCII string");
    Check(edge.GetLong("sign") == -3, "negative enum");
    Check(edge.Count("tags") == 3, "empty repeated element count");
    Check(edge.At("tags", 1)?.AsString() == "", "empty repeated element");
    Check(edge.At("blobs", 0)?.AsBytes()!.SequenceEqual(new byte[] { 0x00 }) == true, "bytes element with NUL");
    Check(edge.At("blobs", 1)?.AsBytes()!.Length == 0, "empty bytes element");
    Check(edge.GetMessage("inner")?.GetLong("n") == -1, "nested negative");
    Check(edge.Has("maybe") && edge.GetString("maybe") == "", "optional present and empty");
    Check(edge.Has("pick_str") && !edge.Has("pick_num"), "oneof keeps only the last member");
    Check(edge.Encode().SequenceEqual(edgeOracle), "edge decode -> encode is byte-exact");
}
Console.WriteLine("OK   edge payload: bytes, UTF-8, negative enum, empty elements, oneof");

// A default message is zero bytes both ways.
Check(edgeTy.NewMessage().Encode().Length == 0, "empty message encodes to zero bytes");
using (var emptyDecoded = edgeTy.Decode(Array.Empty<byte>()))
{
    Check(emptyDecoded.Encode().Length == 0, "empty wire decodes and re-encodes to zero bytes");
    Check(!emptyDecoded.Has("text"), "empty message has no fields");
}

// A wrong value kind is refused, not written.
Check(Rejects(() => edgeTy.NewMessage().Set("pick_num", "not a number"), "type mismatch"),
    "wrong-typed value rejected");
Check(Rejects(() => edgeTy.NewMessage().SetMessage("text"), "not a message field"),
    "setMessage on a scalar rejected");

Console.WriteLine($"OK   {checks} checks passed");

static bool Rejects(Action op, string expected)
{
    try
    {
        op();
        return false;
    }
    catch (PbwireException e)
    {
        return e.Message.Contains(expected);
    }
}

static bool Throws(Action op)
{
    try
    {
        op();
        return false;
    }
    catch (PbwireException)
    {
        return true;
    }
}

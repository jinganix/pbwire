// A round trip over the pinned matrix3.desc: build, encode, decode, read back.
//
//   dotnet run --project examples -- <descriptor-set>

using Pbwire;

var desc = args.Length > 0 ? args[0] : "../../c/tests/data/matrix3.desc";

using var schema = Schema.FromFile(desc);
var ty = schema.Message("structtest.Scalars3");
var innerTy = schema.Message("structtest.Inner3");

using var msg = ty.NewMessage()
    .Set("i32", -42L)
    .Set("str", "alice")
    .Set("flag", true);
foreach (var tag in new[] { "a", "bb" })
{
    msg.Set("tags", tag); // repeated: appends
}
msg.SetMessage("inner") // nested: created in place
    .Set(innerTy.Field("name"), "kid")
    .Set(innerTy.Field("value"), 42L);

var wire = msg.Encode();
Console.WriteLine($"{wire.Length} bytes: {Convert.ToHexString(wire).ToLowerInvariant()}");

using var decoded = ty.Decode(wire);
Console.WriteLine($"i32   = {decoded.GetLong("i32")}");
Console.WriteLine($"str   = {decoded.GetString("str")}");
Console.WriteLine($"inner = {decoded.GetMessage("inner")?.GetString("name")}");
Console.WriteLine($"tags  = [{string.Join(", ", Enumerable.Range(0, decoded.Count("tags")).Select(i => decoded.At("tags", i)?.AsString()))}]");

if (!decoded.Encode().SequenceEqual(wire))
{
    Console.Error.WriteLine("round trip differs");
    return 1;
}
Console.WriteLine("OK   byte-exact round trip");
return 0;

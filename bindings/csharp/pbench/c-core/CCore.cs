// The `core` arm: a pure-C# port of the C core (bindings/csharp/Pbwire/Core).
// The native object of this arm is the plain object (`Dictionary<string,
// object?>`, proto field names, long/ulong for 64-bit) -- the same prebuilt
// native input every arm encodes from (Go template: encode from the same
// prebuilt native struct; the conversion into the codec's tree runs inside
// the timer). `Marshal` converts and encodes per call (`EncodeObject`), and
// `Unmarshal` decodes wire bytes back into a fresh plain object per call
// (`DecodeObject`). No native library, no P/Invoke, no per-field boundary
// crossing: the whole task runs in the managed heap. Timing a prebuilt tree
// (`Msg`) instead would be the proxy arm, not the core arm.
//
// Kept in its own assembly (like the shim model) so both the gate and the
// formal benchmark can reference it without dragging in a second model.

namespace Pbwire.Pbench;

public static class CCore
{
    private static Pbwire.Core.MessageType? _type;

    /// <summary>Loads the descriptor once; every arm call after this is schema-free.</summary>
    public static void Use(string descriptorSet)
    {
        var schema = Pbwire.Core.Schema.FromFile(descriptorSet);
        _type = schema.Message("structtest.Scalars3");
    }

    public static Pbwire.Core.MessageType Type =>
        _type ?? throw new InvalidOperationException("CCore.Use(descriptorSet) was not called");

    public static Pbwire.Core.MsgDef InnerDef => Type.Field("inner")!.MsgType!;

    /// <summary>The payload every tier uses, as this arm's native object (prebuilt once, converted per call).</summary>
    public static Dictionary<string, object?> New(int n)
    {
        var inner = new Dictionary<string, object?> { ["name"] = "kid", ["value"] = 42 };
        var nums = new object?[n];
        var tags = new object?[n];
        var flags = new object?[n];
        var inners = new object?[n];
        for (var i = 0; i < n; i++)
        {
            nums[i] = i - 3;
            tags[i] = "tag";
            flags[i] = i % 2 == 0;
            inners[i] = new Dictionary<string, object?> { ["name"] = "grandkid", ["value"] = i + 1 };
        }
        return new Dictionary<string, object?>
        {
            ["i32"] = int.MinValue,
            ["i64"] = long.MaxValue,
            ["u32"] = uint.MaxValue,
            ["u64"] = ulong.MaxValue,
            ["s32"] = int.MinValue,
            ["s64"] = long.MinValue,
            ["fx32"] = 0xdeadbeef,
            ["fx64"] = 72623859790382856L,
            ["sfx32"] = int.MinValue,
            ["sfx64"] = long.MinValue,
            ["f32"] = float.PositiveInfinity,
            ["f64"] = -123456.789,
            ["flag"] = true,
            ["str"] = "hello",
            ["color"] = 2,
            ["inner"] = inner,
            ["nums"] = nums,
            ["tags"] = tags,
            ["inners"] = inners,
            ["flags"] = flags,
            ["opt_i32"] = -7,
            ["opt_str"] = "opt",
        };
    }

    /// <summary>One call per op: the plain object in, wire bytes out (convert + encode).</summary>
    public static byte[] Marshal(Dictionary<string, object?> native) => Type.EncodeObject(native);

    /// <summary>One call per op: wire bytes in, a fresh plain object out.</summary>
    public static Dictionary<string, object?> Unmarshal(byte[] wire) => Type.DecodeObject(wire);
}

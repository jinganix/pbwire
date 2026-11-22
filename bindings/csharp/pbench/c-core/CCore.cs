// The `core` arm: a pure-C# port of the C core (bindings/csharp/Pbwire/Core).
// `New` builds the native object of this arm once, before the timer -- the
// dynamic tree (`Pbwire.Core.Msg`) -- exactly like the `tree` arm builds its
// prebuilt handle; `Marshal` encodes it and `Unmarshal` decodes wire bytes
// back into a tree. No native library, no P/Invoke, no per-field boundary
// crossing: the whole task runs in the managed heap.
//
// Kept in its own assembly (like the shim model) so both the gate and the
// formal benchmark can reference it without dragging in a second model.

namespace Pbwire.Pbench;

using Pbwire.Core;

public static class CCore
{
    private static MessageType? _type;

    /// <summary>Loads the descriptor once; every arm call after this is schema-free.</summary>
    public static void Use(string descriptorSet)
    {
        var schema = Schema.FromFile(descriptorSet);
        _type = schema.Message("structtest.Scalars3");
    }

    public static MessageType Type =>
        _type ?? throw new InvalidOperationException("CCore.Use(descriptorSet) was not called");

    public static MsgDef InnerDef => Type.Field("inner")!.MsgType!;

    /// <summary>The payload every tier uses, as this arm's native object.</summary>
    public static Msg New(int n)
    {
        var type = Type;
        var msg = type.NewMessage();
        msg.Set("i32", int.MinValue)
            .Set("i64", long.MaxValue)
            .Set("u32", uint.MaxValue)
            .Set("u64", ulong.MaxValue)
            .Set("s32", int.MinValue)
            .Set("s64", long.MinValue)
            .Set("fx32", 0xdeadbeefu)
            .Set("fx64", 72623859790382856UL)
            .Set("sfx32", int.MinValue)
            .Set("sfx64", long.MinValue)
            .Set("f32", float.PositiveInfinity)
            .Set("f64", -123456.789)
            .Set("flag", true)
            .Set("str", "hello")
            .Set("color", 2)
            .Set("opt_i32", -7)
            .Set("opt_str", "opt");
        msg.NewMessage("inner").Set("name", "kid").Set("value", 42);
        for (var i = 0; i < n; i++)
        {
            msg.Add("nums", i - 3);
            msg.Add("tags", "tag");
            msg.Add("flags", i % 2 == 0);
            var kid = new Msg(InnerDef);
            kid.Set("name", "grandkid").Set("value", i + 1);
            msg.GetRep("inners").Add(kid);
        }
        return msg;
    }

    /// <summary>One call per op: the tree in, wire bytes out.</summary>
    public static byte[] Marshal(Msg native) => Type.Encode(native);

    /// <summary>One call per op: wire bytes in, a fully readable tree out.</summary>
    public static Msg Unmarshal(byte[] wire) => Type.Decode(wire);
}

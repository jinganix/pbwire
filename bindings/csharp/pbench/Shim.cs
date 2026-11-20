// The native-object arm's helper over the shipped generated model. Public API
// stays object-typed on purpose: the benchmark projects reference this
// assembly without the model assembly (disable transitive project references),
// so their own protoc-generated `Structtest` namespace stays unambiguous.
//
// Not test-only glue: each call is one shipped entry, `Scalars3Layout.Marshal`
// / `.Unmarshal` (Pbwire.Run.Encode/Decode), which is what a user of the
// generated model pays.

namespace Pbwire.Pbench;

using Structtest;

public static class Matrix3Shim
{
    /// <summary>The payload every tier uses, as the native object of the shim arm.</summary>
    public static object New(int n)
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

    /// <summary>One call per op: native object in, wire bytes out.</summary>
    public static byte[] Marshal(object native) => Scalars3.Scalars3Layout.Marshal((Scalars3)native);

    /// <summary>One call per op: wire bytes in, native object out.</summary>
    public static object Unmarshal(byte[] wire) => Scalars3.Scalars3Layout.Unmarshal(wire);
}

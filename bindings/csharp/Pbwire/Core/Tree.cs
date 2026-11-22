// The dynamic message tree: one slot per declared field, plus the oneof
// cases and the retained unknown fields. Scalars live in their wire-neutral
// .NET form (long/ulong for the 64-bit kinds, raw bits for float/double so NaN
// payloads survive a round trip, 0/1 for bool); messages live nested as Msg;
// strings and bytes decoded off the wire stay a Raw slice of the input until
// materialized, so invalid UTF-8 and non-minimal length prefixes re-encode
// byte for byte.
//
// This is the C# twin of bindings/node/lib/core/tree.ts and
// bindings/java/runtime/GenericCodec.DMsg.

namespace Pbwire.Core;

/// <summary>An undecoded field image (tag + length + payload) inside the input.</summary>
public sealed class Raw
{
    public Raw(byte[] b, int off, int len)
    {
        B = b;
        Off = off;
        Len = len;
    }

    public byte[] B { get; }

    public int Off { get; }

    public int Len { get; }
}

/// <summary>One decoded message: slots, oneof cases and retained unknown fields.</summary>
public sealed class Msg
{
    public Msg(MsgDef def)
    {
        Def = def;
        V = new object?[def.Fields.Length];
        OneofCase = def.OneofCount > 0 ? new int[def.OneofCount] : null;
    }

    public MsgDef Def { get; }

    /// <summary>Per slot: null = absent, an array for repeated fields.</summary>
    public object?[] V { get; }

    /// <summary>Slot+1 per oneof, 0 = none set; null when the message has none.</summary>
    public int[]? OneofCase { get; }

    /// <summary>Retained unknown fields in input order; null until the first.</summary>
    public List<byte[]>? Unknown { get; set; }

    public int Slot(string name) => Tree.RequireSlot(Def, name);

    public object? Get(string name) => Tree.GetField(this, name);

    public bool Has(string name) => Tree.HasField(this, name);

    public Msg Set(string name, object? value)
    {
        var s = Slot(name);
        Tree.SetSlot(this, s, Tree.FromJs(Def.Fields[s], value));
        return this;
    }

    /// <summary>Appends one converted value to a repeated field.</summary>
    public Msg Add(string name, object? value)
    {
        var s = Slot(name);
        var f = Def.Fields[s];
        if (!f.Repeated)
        {
            throw new CoreException(Def.FullName + "." + name + " is not repeated");
        }
        GetRep(name).Add(Tree.FromJs(f, value));
        return this;
    }

    /// <summary>The mutable list behind a repeated field (created on first use).</summary>
    public List<object?> GetRep(string name)
    {
        var s = Slot(name);
        if (!Def.Fields[s].Repeated)
        {
            throw new CoreException(Def.FullName + "." + name + " is not repeated");
        }
        if (V[s] is List<object?> list)
        {
            return list;
        }
        list = [];
        V[s] = list;
        return list;
    }

    /// <summary>Creates a child message for <paramref name="name"/>, stores it, returns it.</summary>
    public Msg NewMessage(string name)
    {
        var s = Slot(name);
        var child = new Msg(Def.Fields[s].MsgType ?? throw new CoreException(Def.FullName + "." + name + " is not a message"));
        Tree.SetSlot(this, s, child);
        return child;
    }

    /// <summary>Which member of a oneof is set: its field number, or 0 for none.</summary>
    public int OneofCaseOf(string oneofName)
    {
        if (!Def.OneofByName.TryGetValue(oneofName, out var idx) || OneofCase is null)
        {
            throw new CoreException(Def.FullName + " has no oneof " + oneofName);
        }
        var c = OneofCase[idx];
        return c == 0 ? 0 : Def.Fields[c - 1].Number;
    }
}

/// <summary>The tree helpers: slots, conversion to the public forms, plain objects.</summary>
public static class Tree
{
    /// <summary>Slot index of a field name, or -1.</summary>
    public static int SlotByName(MsgDef def, string name) => def.ByName.GetValueOrDefault(name, -1);

    public static int RequireSlot(MsgDef def, string name)
    {
        var s = SlotByName(def, name);
        return s >= 0 ? s : throw new CoreException(def.FullName + " has no field " + name);
    }

    /// <summary>Stores a value in a slot, collapsing the oneof it belongs to.</summary>
    public static void SetSlot(Msg m, int s, object? value)
    {
        var f = m.Def.Fields[s];
        if (f.Oneof >= 0 && m.OneofCase is not null)
        {
            var prev = m.OneofCase[f.Oneof];
            if (prev != 0 && prev != s + 1)
            {
                m.V[prev - 1] = null;
            }
            m.OneofCase[f.Oneof] = s + 1;
        }
        m.V[s] = value;
    }

    public static bool HasField(Msg m, string name) => m.V[RequireSlot(m.Def, name)] is not null;

    /// <summary>The singular value of a field: materialized, converted, or null.</summary>
    public static object? GetField(Msg m, string name)
    {
        var s = RequireSlot(m.Def, name);
        var f = m.Def.Fields[s];
        var v = m.V[s];
        if (v is null)
        {
            return null;
        }
        if (f.Repeated)
        {
            var arr = (List<object?>)v;
            var output = new object?[arr.Count];
            for (var i = 0; i < arr.Count; i++)
            {
                output[i] = ToJs(f, arr[i]);
            }
            return output;
        }
        return ToJs(f, v);
    }

    /// <summary>The payload bytes of a Raw string/bytes field image.</summary>
    public static byte[] RawPayload(Raw raw)
    {
        var r = new R(raw.B, raw.Off, raw.Off + raw.Len);
        Wire.RVar(r); // the tag, already validated at slice time
        var n = Wire.RLen(r);
        var payload = new byte[n];
        Array.Copy(r.B, r.Pos, payload, 0, n);
        return payload;
    }

    /// <summary>Materializes a Raw slice into its value.</summary>
    public static object Materialize(FieldDef f, Raw raw)
    {
        var r = new R(raw.B, raw.Off, raw.Off + raw.Len);
        Wire.RVar(r); // the tag
        var n = Wire.RLen(r);
        if (f.Kind == K.String)
        {
            // proto3/editions strings were validated at parse, so this is the
            // loose mapping only for proto2 strings.
            return Wire.Utf8Decode(r.B, r.Pos, n);
        }
        var payload = new byte[n];
        Array.Copy(r.B, r.Pos, payload, 0, n);
        return payload;
    }

    /// <summary>One value in its public form (what Get()/ToObject() expose).</summary>
    public static object? ToJs(FieldDef f, object? v)
    {
        if (v is Raw raw)
        {
            return Materialize(f, raw);
        }
        return f.Kind switch
        {
            K.Bool => (int)v! != 0,
            K.Float => Wire.BitsFloat((uint)v!),
            K.Double => Wire.BitsDouble((ulong)v!),
            _ => v,
        };
    }

    /// <summary>A plain object: proto field names, long/ulong for 64-bit, nested objects.</summary>
    public static Dictionary<string, object?> ToObject(Msg m)
    {
        var def = m.Def;
        var output = new Dictionary<string, object?>();
        for (var s = 0; s < def.Fields.Length; s++)
        {
            var v = m.V[s];
            if (v is null)
            {
                continue;
            }
            var f = def.Fields[s];
            if (f.Repeated)
            {
                var arr = (List<object?>)v;
                if (arr.Count == 0)
                {
                    continue;
                }
                var list = new object?[arr.Count];
                for (var i = 0; i < arr.Count; i++)
                {
                    var item = arr[i];
                    list[i] = f.Kind is K.Message or K.Group && item is Msg nested ? ToObject(nested) : ToJs(f, item);
                }
                output[f.Name] = list;
                continue;
            }
            if (f.Kind is K.Message or K.Group && v is Msg child)
            {
                output[f.Name] = ToObject(child);
                continue;
            }
            output[f.Name] = ToJs(f, v);
        }
        return output;
    }

    /// <summary>One value from its plain form (nested messages included).</summary>
    public static object? FromJs(FieldDef f, object? v) => f.Kind switch
    {
        K.Int32 or K.Sint32 or K.Sfixed32 or K.Enum => (int)ToInt64(v),
        K.Uint32 or K.Fixed32 => (uint)ToUInt64(v),
        K.Int64 or K.Sint64 or K.Sfixed64 => ToInt64(v),
        K.Uint64 or K.Fixed64 => ToUInt64(v),
        K.Bool => Truthy(v) ? 1 : 0,
        K.Float => Wire.FloatBits(Convert.ToSingle(v)),
        K.Double => Wire.DoubleBits(Convert.ToDouble(v)),
        K.String => v as string ?? throw new CoreException("needs a string"),
        K.Bytes => v switch
        {
            byte[] bytes => bytes,
            ReadOnlyMemory<byte> mem => mem.ToArray(),
            ArraySegment<byte> seg => seg.ToArray(),
            _ => throw new CoreException("needs bytes"),
        },
        _ => v is Msg m
            ? m
            : FromObject(f.MsgType ?? throw new CoreException("message field without a type"), AsObject(v)),
    };

    /// <summary>
    /// Builds a tree from a plain object (proto field names, long/ulong for
    /// 64-bit; absent or null keys stay absent).
    /// </summary>
    public static Msg FromObject(MsgDef def, IDictionary<string, object?> obj)
    {
        var m = new Msg(def);
        for (var s = 0; s < def.Fields.Length; s++)
        {
            var f = def.Fields[s];
            if (!obj.TryGetValue(f.Name, out var v) || v is null)
            {
                continue;
            }
            try
            {
                if (f.Repeated)
                {
                    var items = v switch
                    {
                        System.Collections.IEnumerable e and not string => e.Cast<object?>().ToList(),
                        _ => throw new CoreException("needs an array"),
                    };
                    var arr = new List<object?>(items.Count);
                    foreach (var item in items)
                    {
                        arr.Add(FromJs(f, item));
                    }
                    m.V[s] = arr;
                }
                else
                {
                    SetSlot(m, s, FromJs(f, v));
                }
            }
            catch (CoreException e)
            {
                throw new CoreException(def.FullName + "." + f.Name + ": " + e.Message);
            }
            catch (Exception e) when (e is InvalidCastException or FormatException or OverflowException)
            {
                throw new CoreException(def.FullName + "." + f.Name + ": " + e.Message);
            }
        }
        return m;
    }

    /// <summary>Throws when a Required field of this message is absent.</summary>
    public static void CheckPresentRequired(Msg m, int s)
    {
        if (m.V[s] is null && m.Def.Fields[s].Presence == Presence.Required)
        {
            var f = m.Def.Fields[s];
            throw new CoreException("missing required field " + m.Def.FullName + "." + f.Name, 6);
        }
    }

    private static IDictionary<string, object?> AsObject(object? v) =>
        v as IDictionary<string, object?>
        ?? throw new CoreException("needs an object");

    private static long ToInt64(object? v) => v switch
    {
        long l => l,
        int i => i,
        uint u => u,
        ulong ul => unchecked((long)ul),
        short s => s,
        ushort us => us,
        byte b => b,
        sbyte sb => sb,
        _ => Convert.ToInt64(v),
    };

    private static ulong ToUInt64(object? v) => v switch
    {
        ulong ul => ul,
        long l => unchecked((ulong)l),
        uint u => u,
        int i => unchecked((uint)i),
        _ => Convert.ToUInt64(v),
    };

    private static bool Truthy(object? v) => v switch
    {
        null => false,
        bool b => b,
        int i => i != 0,
        long l => l != 0,
        _ => Convert.ToBoolean(v),
    };
}

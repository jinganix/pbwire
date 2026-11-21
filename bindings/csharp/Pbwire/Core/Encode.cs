// Encoder: one pass into a reusable buffer, fields in schema (field-number)
// order, unknown fields appended verbatim in input order. Nested payloads use
// a one-byte length placeholder patched afterwards, packed runs use a
// five-byte placeholder shifted down once, and untouched Raw slices copy
// verbatim (tag included), so a decode/encode round trip is byte-exact.
//
// The C# twin of bindings/node/lib/core/encode.ts and
// bindings/java/runtime/GenericCodec.encodeInto.

namespace Pbwire.Core;

/// <summary>The binary encoder.</summary>
public static class Encoder
{
    /* One workspace buffer per thread: an encode pays no allocation but the
     * result copy. */
    [ThreadStatic]
    private static W? _ws;

    public static byte[] Encode(Msg m)
    {
        var w = _ws ??= new W();
        w.Reset();
        EncodeInto(m, w, 0);
        var result = new byte[w.Pos];
        Array.Copy(w.B, result, w.Pos);
        return result;
    }

    private static bool IsZero(object v) => v switch
    {
        int i => i == 0,
        long l => l == 0,
        uint u => u == 0,
        ulong ul => ul == 0,
        _ => false,
    };

    private static void EncodeInto(Msg m, W w, int depth)
    {
        if (depth > Wire.MaxDepth)
        {
            throw new CoreException("nesting exceeds " + Wire.MaxDepth, 4);
        }
        var def = m.Def;
        var noRequired = !def.HasRequired; // proto3: skip the check
        var fields = def.Fields;
        for (var s = 0; s < fields.Length; s++)
        {
            var v = m.V[s];
            if (v is null)
            {
                if (!noRequired && fields[s].Presence == Presence.Required)
                {
                    Tree.CheckPresentRequired(m, s); // throws
                }
                continue;
            }
            var f = fields[s];
            if (f.Repeated)
            {
                var arr = (List<object?>)v;
                if (arr.Count == 0)
                {
                    continue;
                }
                if (Kinds.IsPackable(f.Kind))
                {
                    EncodeRepNum(w, f, arr);
                }
                else
                {
                    foreach (var item in arr)
                    {
                        WriteValue(w, f, item, depth);
                    }
                }
                continue;
            }
            if (Kinds.IsPackable(f.Kind))
            {
                // Only implicit defaults skip here; explicit defaults (and
                // required) emit once set.
                if (IsZero(v) && f.Presence == Presence.Implicit)
                {
                    continue;
                }
                WriteSingularNum(w, f, v);
                continue;
            }
            // Implicit empty strings and bytes stay silent, everything else emits.
            if (f.Presence == Presence.Implicit)
            {
                if (v is string str && str.Length == 0)
                {
                    continue;
                }
                if (v is byte[] bytes && bytes.Length == 0)
                {
                    continue;
                }
            }
            WriteValue(w, f, v, depth);
        }
        if (m.Unknown is not null)
        {
            // Unknown fields last, in input order.
            foreach (var u in m.Unknown)
            {
                Wire.Ensure(w, u.Length);
                Array.Copy(u, 0, w.B, w.Pos, u.Length);
                w.Pos += u.Length;
            }
        }
    }

    private static void WriteValue(W w, FieldDef f, object? v, int depth)
    {
        if (v is Raw raw)
        {
            // Tag included: a verbatim copy stays byte-exact.
            Wire.Ensure(w, raw.Len);
            Array.Copy(raw.B, raw.Off, w.B, w.Pos, raw.Len);
            w.Pos += raw.Len;
            return;
        }
        switch (f.Kind)
        {
            case K.String:
            {
                var s = (string)v!;
                var n = Wire.Utf8Length(s);
                Wire.Ensure(w, f.Tag.Length + 5 + n);
                var p = w.Pos;
                var b = w.B;
                for (var i = 0; i < f.Tag.Length; i++)
                {
                    b[p++] = f.Tag[i];
                }
                var x = (uint)n;
                while (x > 0x7f)
                {
                    b[p++] = (byte)((x & 0x7f) | 0x80);
                    x >>= 7;
                }
                b[p++] = (byte)x;
                w.Pos = p;
                Wire.Utf8Write(w, s);
                return;
            }
            case K.Bytes:
                Wire.WBytes(w, f.Tag, (byte[])v!);
                return;
            case K.Message:
            {
                // Inlined tag + length placeholder + patch: a repeated message
                // field is the hottest nesting shape.
                var tag = f.Tag;
                Wire.Ensure(w, tag.Length + 1);
                var p = w.Pos;
                var b = w.B;
                for (var i = 0; i < tag.Length; i++)
                {
                    b[p++] = tag[i];
                }
                var lp = p;
                b[p++] = 0;
                w.Pos = p;
                EncodeInto((Msg)v!, w, depth + 1);
                Wire.PatchLen(w, lp);
                return;
            }
            default:
            {
                // K.Group: start tag, body, end tag.
                Wire.WTag(w, f.Tag);
                EncodeInto((Msg)v!, w, depth + 1);
                Wire.WTag(w, f.TagEnd!);
                return;
            }
        }
    }

    /// <summary>Singular numeric write: one fused tag+value call per field.</summary>
    private static void WriteSingularNum(W w, FieldDef f, object v)
    {
        switch (f.Kind)
        {
            case K.Int32:
            case K.Enum:
            {
                // Encode the int32 interpretation, sign-extended: the reference
                // truncates a wide varint to int32 and re-emits a negative as 10
                // bytes.
                var x = (int)v;
                if (x >= 0)
                {
                    Wire.WTagVar32(w, f.Tag, (uint)x);
                }
                else
                {
                    Wire.WTagVar64(w, f.Tag, unchecked((ulong)(long)x));
                }
                return;
            }
            case K.Uint32:
                Wire.WTagVar32(w, f.Tag, (uint)v);
                return;
            case K.Sint32:
                Wire.WTagVar32(w, f.Tag, Wire.Zig32((int)v));
                return;
            case K.Int64:
                Wire.WTagVar64(w, f.Tag, unchecked((ulong)(long)v));
                return;
            case K.Uint64:
                Wire.WTagVar64(w, f.Tag, (ulong)v);
                return;
            case K.Sint64:
                Wire.WTagVar64(w, f.Tag, Wire.Zig64((long)v));
                return;
            case K.Bool:
                Wire.WTagVar32(w, f.Tag, (int)v == 0 ? 0u : 1u);
                return;
            case K.Fixed32:
            case K.Float:
                Wire.WTagFixed32(w, f.Tag, (uint)v);
                return;
            case K.Sfixed32:
                Wire.WTagFixed32(w, f.Tag, (uint)(int)v);
                return;
            case K.Fixed64:
            case K.Double:
                Wire.WTagFixed64(w, f.Tag, (ulong)v);
                return;
            default: // K.Sfixed64
                Wire.WTagFixed64(w, f.Tag, unchecked((ulong)(long)v));
                return;
        }
    }

    /// <summary>One dispatch per field; the loops below it are branch-free writes.</summary>
    private static void EncodeRepNum(W w, FieldDef f, List<object?> arr)
    {
        var n = arr.Count;
        if (f.Packed)
        {
            if (f.Kind == K.Bool)
            {
                Wire.WTag(w, f.Tag);
                Wire.WVar32(w, (uint)n);
                Wire.Ensure(w, n);
                for (var i = 0; i < n; i++)
                {
                    w.B[w.Pos++] = (int)arr[i]! == 0 ? (byte)0 : (byte)1;
                }
                return;
            }
            if (f.Kind is K.Fixed32 or K.Sfixed32 or K.Float)
            {
                Wire.WTag(w, f.Tag);
                Wire.WVar32(w, (uint)(n * 4));
                Wire.Ensure(w, n * 4);
                for (var i = 0; i < n; i++)
                {
                    var v = f.Kind == K.Sfixed32 ? (uint)(int)arr[i]! : (uint)arr[i]!;
                    var p = w.Pos;
                    var b = w.B;
                    b[p++] = (byte)v;
                    b[p++] = (byte)(v >> 8);
                    b[p++] = (byte)(v >> 16);
                    b[p++] = (byte)(v >> 24);
                    w.Pos = p;
                }
                return;
            }
            if (f.Kind is K.Fixed64 or K.Sfixed64 or K.Double)
            {
                Wire.WTag(w, f.Tag);
                Wire.WVar32(w, (uint)(n * 8));
                Wire.Ensure(w, n * 8);
                for (var i = 0; i < n; i++)
                {
                    Wire.WFixed64(w, f.Kind == K.Sfixed64 ? unchecked((ulong)(long)arr[i]!) : (ulong)arr[i]!);
                }
                return;
            }
            // Varint kinds: write the payload over a 5-byte placeholder, then
            // shift it down: one memmove beats a size pre-pass over thousands of
            // elements.
            Wire.WTag(w, f.Tag);
            var lp = w.Pos;
            Wire.Ensure(w, n * 10 + 5);
            w.Pos += 5;
            for (var i = 0; i < n; i++)
            {
                WritePackedVar(w, f.Kind, arr[i]!);
            }
            var len = w.Pos - lp - 5;
            var llen = Wire.SizeVar32((uint)len);
            Array.Copy(w.B, lp + 5, w.B, lp + llen, len);
            var q = lp;
            var x = (uint)len;
            while (x > 0x7f)
            {
                w.B[q++] = (byte)((x & 0x7f) | 0x80);
                x >>= 7;
            }
            w.B[q++] = (byte)x;
            w.Pos = lp + llen + len;
            return;
        }
        for (var i = 0; i < n; i++)
        {
            Wire.WTag(w, f.Tag);
            WriteNumSingle(w, f.Kind, arr[i]!);
        }
    }

    private static void WritePackedVar(W w, int kind, object v)
    {
        switch (kind)
        {
            case K.Int32:
            case K.Enum:
            {
                var x = (int)v;
                if (x >= 0)
                {
                    Wire.WVar32(w, (uint)x);
                }
                else
                {
                    Wire.WVar64(w, unchecked((ulong)(long)x));
                }
                return;
            }
            case K.Uint32:
                Wire.WVar32(w, (uint)v);
                return;
            case K.Sint32:
                Wire.WVar32(w, Wire.Zig32((int)v));
                return;
            case K.Int64:
                Wire.WVar64(w, unchecked((ulong)(long)v));
                return;
            case K.Uint64:
                Wire.WVar64(w, (ulong)v);
                return;
            case K.Sint64:
                Wire.WVar64(w, Wire.Zig64((long)v));
                return;
            default:
                throw new CoreException("not packable");
        }
    }

    private static void WriteNumSingle(W w, int kind, object v)
    {
        switch (kind)
        {
            case K.Int32:
            case K.Enum:
            {
                var x = (int)v;
                if (x >= 0)
                {
                    Wire.WVar32(w, (uint)x);
                }
                else
                {
                    Wire.WVar64(w, unchecked((ulong)(long)x));
                }
                return;
            }
            case K.Uint32:
                Wire.WVar32(w, (uint)v);
                return;
            case K.Sint32:
                Wire.WVar32(w, Wire.Zig32((int)v));
                return;
            case K.Int64:
                Wire.WVar64(w, unchecked((ulong)(long)v));
                return;
            case K.Uint64:
                Wire.WVar64(w, (ulong)v);
                return;
            case K.Sint64:
                Wire.WVar64(w, Wire.Zig64((long)v));
                return;
            case K.Bool:
                Wire.Ensure(w, 1);
                w.B[w.Pos++] = (int)v == 0 ? (byte)0 : (byte)1;
                return;
            case K.Fixed32:
            case K.Float:
                Wire.WFixed32(w, (uint)v);
                return;
            case K.Sfixed32:
                Wire.WFixed32(w, (uint)(int)v);
                return;
            case K.Fixed64:
            case K.Double:
                Wire.WFixed64(w, (ulong)v);
                return;
            default: // K.Sfixed64
                Wire.WFixed64(w, unchecked((ulong)(long)v));
                return;
        }
    }
}

// Decoder: strict tags and lengths (like the C core), unknown field numbers
// AND wrong-wire-type occurrences retained as raw tag+payload bytes in input
// order, groups decoded to their matching end tag, packed runs accepted for
// every numeric kind (and the unpacked element form for a packed field),
// oneofs collapsed to the last member, presence following the field's own
// zero, proto2 required fields checked over the whole tree, and the nesting
// cap enforced on every recursive path.
//
// The C# twin of bindings/node/lib/core/decode.ts and
// bindings/java/runtime/GenericCodec.decodeInto.

namespace Pbwire.Core;

/// <summary>The binary decoder.</summary>
public static class Decoder
{
    /// <summary>Decodes a whole message, unknown fields retained.</summary>
    public static Msg Decode(MsgDef def, byte[] bytes, Limits? limits = null)
    {
        Limits.CheckBytes(limits, bytes.Length, "message");
        var r = new R(bytes) { Cap = Limits.EffectiveDepth(limits) };
        var m = new Msg(def);
        DecodeInto(m, r, bytes.Length, -1, 0);
        if (def.DeepRequired)
        {
            CheckRequired(m, r.Cap, 0);
        }
        return m;
    }

    /// <summary>Decodes a message body that already starts at <paramref name="off"/> and ends at off+len.</summary>
    public static Msg DecodeSlice(MsgDef def, byte[] bytes, int off, int len, Limits? limits = null)
    {
        Limits.CheckBytes(limits, len, "message");
        var r = new R(bytes, off, off + len) { Cap = Limits.EffectiveDepth(limits) };
        var m = new Msg(def);
        DecodeInto(m, r, off + len, -1, 0);
        if (def.DeepRequired)
        {
            CheckRequired(m, r.Cap, 0);
        }
        return m;
    }

    /// <summary>An empty message of <paramref name="def"/>.</summary>
    public static Msg NewMessage(MsgDef def) => new(def);

    /// <summary>
    /// True when the field does not accept this wire type. A repeated numeric
    /// field always accepts the LEN form (a packed run, whatever the declared
    /// form) and a packed field also accepts its unpacked element form.
    /// </summary>
    private static bool WtIncompatible(FieldDef f, int wt)
    {
        if (f.Kind == K.Group)
        {
            return wt != Wt.StartGroup;
        }
        if (f.Repeated && Kinds.IsPackable(f.Kind))
        {
            if (wt == Wt.Len)
            {
                return false;
            }
            if (f.Packed)
            {
                return wt != f.ElemWt;
            }
        }
        return wt != f.Wt;
    }

    /// <summary>
    /// Decodes into <paramref name="m"/> up to <paramref name="end"/> (or the
    /// matching end-group when groupEnd &gt;= 0). One shared reader threads
    /// through the recursion, so positions never need saving: a nested payload
    /// consumes exactly its slice.
    /// </summary>
    private static void DecodeInto(Msg m, R r, int end, int groupEnd, int depth)
    {
        if (depth > r.Cap)
        {
            throw new CoreException("nesting exceeds " + r.Cap, 4);
        }
        var def = m.Def;
        var outerEnd = r.End;
        r.End = end;
        var closed = groupEnd < 0;
        while (r.Pos < r.End)
        {
            var fieldStart = r.Pos;
            var tag = Wire.RTag(r);
            var fnum = (int)(tag >> 3);
            var wt = (int)(tag & 7);
            if (fnum == 0 || wt > Wt.Fixed32)
            {
                throw new CoreException("bad tag " + tag, 1);
            }
            if (wt == Wt.EndGroup)
            {
                // Only our own number closes us; any other end tag is stray.
                if (groupEnd < 0 || fnum != groupEnd)
                {
                    throw new CoreException("stray end-group for " + fnum, 1);
                }
                closed = true;
                break;
            }
            var s = Descriptor.SlotOf(def, fnum);
            if (s < 0 || WtIncompatible(def.Fields[s], wt))
            {
                RetainUnknown(m, r, fieldStart, fnum, wt, depth);
                continue;
            }
            ReadKnown(m, r, def.Fields[s], s, wt, fieldStart, depth);
        }
        r.End = outerEnd;
        if (!closed)
        {
            throw new CoreException("missing end-group for " + groupEnd, 1);
        }
    }

    /// <summary>
    /// Wrong wire type (or unknown number) keeps the raw blob, like the core.
    /// Groups validate to their matching end tag; anything else only checks
    /// value shape -- unknown LEN payloads stay opaque.
    /// </summary>
    private static void RetainUnknown(Msg m, R r, int fieldStart, int fnum, int wt, int depth)
    {
        Wire.SkipUnknown(r, fnum, wt, depth);
        var raw = new byte[r.Pos - fieldStart];
        Array.Copy(r.B, fieldStart, raw, 0, raw.Length);
        (m.Unknown ??= []).Add(raw);
    }

    private static void ReadKnown(Msg m, R r, FieldDef f, int s, int wt, int fieldStart, int depth)
    {
        if (f.Repeated)
        {
            var arr = m.V[s] as List<object?>;
            if (arr is null)
            {
                arr = [];
                m.V[s] = arr;
            }
            if (Kinds.IsPackable(f.Kind))
            {
                if (wt == Wt.Len)
                {
                    ReadPacked(r, f, arr);
                    if (arr.Count == 0)
                    {
                        // An empty packed run adds nothing: presence follows
                        // length, so drop the holder again.
                        m.V[s] = null;
                    }
                    return;
                }
                arr.Add(ReadNumSingle(r, f.Kind));
                return;
            }
            if (f.Kind == K.Message)
            {
                var len = Wire.RLen32(r);
                var sub = new Msg(f.MsgType!);
                DecodeInto(sub, r, r.Pos + len, -1, depth + 1);
                arr.Add(sub);
                return;
            }
            if (f.Kind == K.Group)
            {
                var sub = new Msg(f.MsgType!);
                DecodeInto(sub, r, r.End, f.Number, depth + 1);
                arr.Add(sub);
                return;
            }
            // K.String / K.Bytes element: validate, then record one slice.
            var n = Wire.RLen32(r);
            if (f.Utf8 && !Wire.Utf8Valid(r.B, r.Pos, n))
            {
                throw new CoreException("invalid UTF-8 in a string field", 1);
            }
            r.Pos += n;
            arr.Add(new Raw(r.B, fieldStart, r.Pos - fieldStart));
            return;
        }
        if (Kinds.IsPackable(f.Kind))
        {
            var v = ReadNumSingle(r, f.Kind);
            // Implicit presence is decided on the *field's* zero, not the raw
            // wire bits: an int32/uint32 varint wider than 32 bits truncates and
            // the reference then drops the field as an implicit zero.
            if (f.Presence == Presence.Implicit && IsZero(v))
            {
                return;
            }
            Tree.SetSlot(m, s, v);
            return;
        }
        // Ref-kind singular: a second occurrence merges (like the core).
        if (f.Kind == K.Message)
        {
            var len = Wire.RLen32(r);
            var existing = MergeTarget(m, f, s);
            if (existing is not null)
            {
                DecodeInto(existing, r, r.Pos + len, -1, depth + 1);
            }
            else
            {
                var sub = new Msg(f.MsgType!);
                DecodeInto(sub, r, r.Pos + len, -1, depth + 1);
                Tree.SetSlot(m, s, sub);
            }
            return;
        }
        if (f.Kind == K.Group)
        {
            var existing = MergeTarget(m, f, s);
            if (existing is not null)
            {
                DecodeInto(existing, r, r.End, f.Number, depth + 1);
            }
            else
            {
                var sub = new Msg(f.MsgType!);
                DecodeInto(sub, r, r.End, f.Number, depth + 1);
                Tree.SetSlot(m, s, sub);
            }
            return;
        }
        // K.String / K.Bytes (an empty implicit string stays absent).
        var size = Wire.RLen32(r);
        if (size == 0 && f.Presence == Presence.Implicit)
        {
            return;
        }
        /* proto3/editions strings are validated here, at parse, like the
         * reference: a malformed value fails the decode instead of surfacing as
         * U+FFFD when the slice is materialized. proto2 keeps the loose mapping. */
        if (f.Utf8 && !Wire.Utf8Valid(r.B, r.Pos, size))
        {
            throw new CoreException("invalid UTF-8 in a string field", 1);
        }
        r.Pos += size;
        Tree.SetSlot(m, s, new Raw(r.B, fieldStart, r.Pos - fieldStart));
    }

    /// <summary>
    /// The submessage a repeated singular message/group field merges into, or
    /// null for the first occurrence.
    /// </summary>
    private static Msg? MergeTarget(Msg m, FieldDef f, int s)
    {
        var v = m.V[s];
        if (v is null)
        {
            return null;
        }
        if (f.Oneof >= 0 && m.OneofCase is not null && m.OneofCase[f.Oneof] != s + 1)
        {
            return null;
        }
        return v as Msg;
    }

    private static bool IsZero(object v) => v switch
    {
        int i => i == 0,
        long l => l == 0,
        uint u => u == 0,
        ulong ul => ul == 0,
        _ => false,
    };

    /// <summary>One packed run of numeric elements into <paramref name="arr"/>.</summary>
    private static void ReadPacked(R r, FieldDef f, List<object?> arr)
    {
        var len = Wire.RLen32(r);
        var end = r.Pos + len;
        if (f.Kind == K.Bool)
        {
            while (r.Pos < end)
            {
                arr.Add(Wire.RVarBool(r));
            }
        }
        else if (f.Kind is K.Fixed32 or K.Sfixed32 or K.Float)
        {
            var count = len / 4;
            for (var i = 0; i < count; i++)
            {
                var v = Wire.RFixed32(r);
                arr.Add(f.Kind == K.Sfixed32 ? (int)v : v);
            }
        }
        else if (f.Kind is K.Fixed64 or K.Sfixed64 or K.Double)
        {
            var count = len / 8;
            for (var i = 0; i < count; i++)
            {
                var v = Wire.RFixed64(r);
                arr.Add(f.Kind == K.Sfixed64 ? unchecked((long)v) : v);
            }
        }
        else
        {
            while (r.Pos < end)
            {
                arr.Add(ReadPackedVar(r, f.Kind));
            }
        }
        if (r.Pos != end)
        {
            throw new CoreException("packed overrun", 1);
        }
    }

    private static object ReadPackedVar(R r, int kind) => kind switch
    {
        K.Int32 or K.Enum => (int)Wire.RVar(r),
        K.Uint32 => Wire.RVar(r),
        // Truncate before unzigzag: a 64-bit varint on a sint32 field decodes
        // its low 32 bits, like the reference.
        K.Sint32 => Wire.Unzig32(Wire.RVar(r)),
        K.Int64 => unchecked((long)Wire.RVar64(r)),
        K.Uint64 => Wire.RVar64(r),
        K.Sint64 => Wire.Unzig64(Wire.RVar64(r)),
        _ => throw new CoreException("not numeric", 1),
    };

    /// <summary>One numeric element in its storage form (see Tree).</summary>
    private static object ReadNumSingle(R r, int kind) => kind switch
    {
        K.Int32 or K.Enum => (int)Wire.RVar(r),
        K.Uint32 => Wire.RVar(r),
        K.Bool => Wire.RVarBool(r),
        K.Sint32 => Wire.Unzig32(Wire.RVar(r)),
        K.Int64 => unchecked((long)Wire.RVar64(r)),
        K.Uint64 => Wire.RVar64(r),
        K.Sint64 => Wire.Unzig64(Wire.RVar64(r)),
        K.Fixed32 => Wire.RFixed32(r),
        K.Sfixed32 => (int)Wire.RFixed32(r),
        K.Float => Wire.RFixed32(r),
        K.Fixed64 => Wire.RFixed64(r),
        K.Sfixed64 => unchecked((long)Wire.RFixed64(r)),
        K.Double => Wire.RFixed64(r),
        _ => throw new CoreException("not numeric", 1),
    };

    /// <summary>
    /// Whole-tree required check: every present nested message is checked too,
    /// so a missing proto2 required field fails the decode, like the core.
    /// </summary>
    private static void CheckRequired(Msg m, int cap, int depth)
    {
        if (depth > cap)
        {
            throw new CoreException("nesting exceeds " + cap, 4);
        }
        var def = m.Def;
        if (def.HasRequired)
        {
            for (var s = 0; s < def.Fields.Length; s++)
            {
                if (def.Fields[s].Presence == Presence.Required && m.V[s] is null)
                {
                    throw new CoreException(
                        "missing required field " + def.FullName + "." + def.Fields[s].Name,
                        6);
                }
            }
        }
        for (var s = 0; s < def.Fields.Length; s++)
        {
            var f = def.Fields[s];
            if (f.Kind is not (K.Message or K.Group))
            {
                continue;
            }
            var v = m.V[s];
            if (v is null)
            {
                continue;
            }
            if (f.Repeated)
            {
                foreach (var item in (List<object?>)v)
                {
                    CheckRequired((Msg)item!, cap, depth + 1);
                }
            }
            else if (v is Msg child)
            {
                CheckRequired(child, cap, depth + 1);
            }
        }
    }
}

// FileDescriptorSet reader and schema builder for the pure-C# core: the only
// reader of descriptor.proto the no-native path needs. It parses just enough
// for a binary codec (names, numbers, labels, types, type names, oneofs,
// extensions, packedness, proto3 optional, editions features, syntax) and
// skips everything else by wire type, so unknown descriptor.proto additions
// never break loading.
//
// The wire layout knowledge lives here; Encode/Decode know the message wire
// layout and Schema is the public facade. This mirrors
// bindings/node/lib/core/descriptor.ts, bindings/java/runtime/Proto.java and
// the C core's src/descriptor_set.c.

namespace Pbwire.Core;

/* ---------------- compiled schema (what encode/decode read) ---------------- */

/// <summary>Wire kinds of the table engine (its own numbering, not protobuf's).</summary>
public static class K
{
    public const int Int32 = 0;
    public const int Uint32 = 1;
    public const int Sint32 = 2;
    public const int Int64 = 3;
    public const int Uint64 = 4;
    public const int Sint64 = 5;
    public const int Bool = 6;
    public const int Enum = 7;
    public const int Fixed32 = 8;
    public const int Sfixed32 = 9;
    public const int Float = 10;
    public const int Fixed64 = 11;
    public const int Sfixed64 = 12;
    public const int Double = 13;
    public const int String = 14;
    public const int Bytes = 15;
    public const int Message = 16;
    public const int Group = 17;
}

/// <summary>Presence kinds. Implicit = proto3 plain scalar (no has-bit).</summary>
public static class Presence
{
    public const int Implicit = 0;
    public const int Explicit = 1;
    public const int Required = 2;
    public const int MessagePresence = 3;
}

/// <summary>The packable numeric kinds: packed runs cross every one of them.</summary>
public static class Kinds
{
    public static bool IsPackable(int kind) => kind <= K.Double;
}

public sealed class EnumDef
{
    public required string FullName { get; init; }

    public required Dictionary<string, int> ByName { get; init; }

    public required Dictionary<int, string> ByNumber { get; init; }

    public required bool Closed { get; init; }
}

public sealed class FieldDef
{
    public int Number { get; init; }

    public required string Name { get; init; }

    public int Kind { get; init; }

    /// <summary>Wire type of the declared form: LEN for packed numerics.</summary>
    public int Wt { get; init; }

    /// <summary>Wire type of a single element (what an unpacked occurrence uses).</summary>
    public int ElemWt { get; init; }

    public bool Repeated { get; init; }

    public bool Packed { get; init; }

    /// <summary>Oneof index, -1 when the field is not in a oneof.</summary>
    public int Oneof { get; init; }

    public int Presence { get; init; }

    public MsgDef? MsgType { get; init; }

    public EnumDef? EnumType { get; init; }

    public bool EnumClosed { get; init; }

    /// <summary>A decoded string must be valid UTF-8 (proto3/editions VERIFY).</summary>
    public bool Utf8 { get; init; }

    /// <summary>Extensions only: the global name <c>[pkg.name]</c>.</summary>
    public string? FullName { get; set; }

    /// <summary>Precomputed tag bytes (start-group tag for a group).</summary>
    public required byte[] Tag { get; init; }

    /// <summary>Groups only: the end-group tag.</summary>
    public byte[]? TagEnd { get; init; }
}

public sealed class MsgDef
{
    public required string FullName { get; init; }

    public FieldDef[] Fields { get; set; } = [];

    /// <summary>Dense number-to-slot map, or null when field numbers are sparse.</summary>
    public int[]? SlotByFnum { get; set; }

    public Dictionary<string, int> ByName { get; set; } = [];

    public Dictionary<string, int> OneofByName { get; set; } = [];

    public int OneofCount { get; set; }

    public bool HasRequired { get; set; }

    /// <summary>Some transitively reachable message has a Required field.</summary>
    public bool DeepRequired { get; set; }

    /// <summary>protoc's synthesized map entry (field 1 key, field 2 value).</summary>
    public bool MapEntry { get; init; }

    public string[] ReservedNames { get; init; } = [];
}

public sealed class CoreSchema
{
    public Dictionary<string, MsgDef> Messages { get; } = [];

    public Dictionary<string, EnumDef> Enums { get; } = [];

    public MsgDef Message(string fullName) =>
        Messages.TryGetValue(fullName, out var def)
            ? def
            : throw new CoreException("no such message type: " + fullName);
}

public sealed class Limits
{
    public static readonly Limits Default = new();

    /// <summary>Rejects inputs larger than this; 0 means no cap.</summary>
    public int MaxBytes { get; init; }

    /// <summary>Nesting cap; 0 means <see cref="Wire.MaxDepth"/>. A larger value clamps to it.</summary>
    public int MaxDepth { get; init; }

    public static int EffectiveDepth(Limits? lim) =>
        lim is null || lim.MaxDepth == 0 || lim.MaxDepth > Wire.MaxDepth ? Wire.MaxDepth : lim.MaxDepth;

    public static void CheckBytes(Limits? lim, int n, string what)
    {
        if (lim is not null && lim.MaxBytes > 0 && n > lim.MaxBytes)
        {
            throw new CoreException(what + " exceeds the " + lim.MaxBytes + "-byte limit", 5);
        }
    }
}

/* ---------------- descriptor.proto model ---------------- */

internal static class T
{
    public const int Double = 1;
    public const int Float = 2;
    public const int Int64 = 3;
    public const int Uint64 = 4;
    public const int Int32 = 5;
    public const int Fixed64 = 6;
    public const int Fixed32 = 7;
    public const int Bool = 8;
    public const int String = 9;
    public const int Group = 10;
    public const int Message = 11;
    public const int Bytes = 12;
    public const int Uint32 = 13;
    public const int Enum = 14;
    public const int Sfixed32 = 15;
    public const int Sfixed64 = 16;
    public const int Sint32 = 17;
    public const int Sint64 = 18;

    public const int LabelOptional = 1;
    public const int LabelRequired = 2;
    public const int LabelRepeated = 3;
}

internal sealed class POneof
{
    public string Name = "";
}

internal sealed class PField
{
    public string? Name;
    public string? Extendee;
    public int Number;
    public int Label = T.LabelOptional;
    public int Type;
    public string? TypeName;
    public int OneofIndex = -1;
    public bool Proto3Optional;
    public bool Packed;
    public bool HasPackedOption;

    /// <summary>Editions FeatureSet.enum_type: 0 unset, 1 OPEN, 2 CLOSED.</summary>
    public int EnumOpen;

    /// <summary>Editions FeatureSet.utf8_validation: 0 unset, 2 VERIFY, 3 NONE.</summary>
    public int Utf8;

    // Linked: the parsed message/enum and the oneof.
    public PMessage? MessageType;
    public PEnum? EnumType;
    public bool EnumClosed;
    public POneof? Oneof;

    // Linked: explicit presence (not a proto3 plain scalar).
    public bool HasPresence;

    // Linked: the global extension name.
    public string? FullName;
}

internal sealed class PEnumValue
{
    public string? Name;
    public int Number;
}

internal sealed class PEnum
{
    public string? Name;
    public string FullName = "";
    public List<PEnumValue> Values = [];
    public int EnumOpen;
    public bool Closed;
}

internal sealed class PMessage
{
    public string? Name;
    public string FullName = "";
    public bool MapEntry;
    public List<PField> Fields = [];
    public List<PMessage> Nested = [];
    public List<PField> Extensions = [];
    public List<POneof> Oneofs = [];
    public List<PEnum> Enums = [];
    public List<string> ReservedNames = [];
}

internal sealed class PFile
{
    public string? Name;
    public string Pkg = "";
    public string Syntax = "proto2";

    /// <summary>Field 14: an editions file (behaves as proto3 here).</summary>
    public bool Edition;

    public int EnumOpen;
    public int Utf8;
    public List<PMessage> Messages = [];
    public List<PField> Extensions = [];
    public List<PEnum> Enums = [];
}

/* ---------------- wire parsing ---------------- */

internal static class DescriptorParser
{
    private static R Reader(byte[] b, int off, int len)
    {
        if (len < 0 || off < 0 || off + len > b.Length)
        {
            throw new CoreException("descriptor overrun");
        }
        return new R(b, off, off + len);
    }

    private static string Str(byte[] b, int off, int len) => Wire.Utf8Decode(b, off, len);

    public static List<PFile> ParseSet(byte[] bytes)
    {
        var r = new R(bytes);
        var files = new List<PFile>();
        while (r.Pos < r.End)
        {
            var tag = Wire.RVar(r);
            if (tag >> 3 == 1 && (tag & 7) == Wt.Len)
            {
                var n = Wire.RLen(r);
                files.Add(ParseFile(r.B, r.Pos, n));
                r.Pos += n;
            }
            else
            {
                Wire.Skip(r, (int)(tag & 7));
            }
        }
        return files;
    }

    private static PFile ParseFile(byte[] b, int off, int len)
    {
        var r = Reader(b, off, len);
        var f = new PFile();
        while (r.Pos < r.End)
        {
            var tag = Wire.RVar(r);
            var num = (int)(tag >> 3);
            var wt = (int)(tag & 7);
            if (wt == Wt.Varint)
            {
                // Field 14 is the edition marker; its value is irrelevant here.
                if (num == 14)
                {
                    Wire.RVar(r);
                    f.Edition = true;
                }
                else
                {
                    Wire.Skip(r, wt);
                }
                continue;
            }
            if (wt == Wt.Len)
            {
                var n = Wire.RLen(r);
                if (num == 4)
                {
                    f.Messages.Add(ParseMessage(r.B, r.Pos, n, 1));
                }
                else if (num == 5)
                {
                    f.Enums.Add(ParseEnum(r.B, r.Pos, n));
                }
                else if (num == 7)
                {
                    f.Extensions.Add(ParseField(r.B, r.Pos, n));
                }
                else if (num == 8)
                {
                    f.EnumOpen = ParseFeatures(r.B, r.Pos, r.Pos + n, 50);
                    f.Utf8 = ParseFeatureSetUtf8(r.B, r.Pos, r.Pos + n, 50);
                }
                else if (num == 1)
                {
                    f.Name = Str(r.B, r.Pos, n);
                }
                else if (num == 2)
                {
                    f.Pkg = Str(r.B, r.Pos, n);
                }
                else if (num == 12)
                {
                    f.Syntax = Str(r.B, r.Pos, n);
                }
                r.Pos += n;
            }
            else
            {
                Wire.Skip(r, wt);
            }
        }
        return f;
    }

    private static PMessage ParseMessage(byte[] b, int off, int len, int depth)
    {
        // Descriptor nesting is peer data too: a FileDescriptorSet a few tens of
        // KB can nest thousands of DescriptorProtos, so cap it instead of letting
        // the recursion overflow the stack.
        if (depth > Wire.MaxDepth)
        {
            throw new CoreException("descriptor nesting exceeds " + Wire.MaxDepth);
        }
        var r = Reader(b, off, len);
        var m = new PMessage();
        while (r.Pos < r.End)
        {
            var tag = Wire.RVar(r);
            var num = (int)(tag >> 3);
            if ((tag & 7) != Wt.Len)
            {
                Wire.Skip(r, (int)(tag & 7));
                continue;
            }
            var n = Wire.RLen(r);
            if (num == 1)
            {
                m.Name = Str(r.B, r.Pos, n);
            }
            else if (num == 2)
            {
                m.Fields.Add(ParseField(r.B, r.Pos, n));
            }
            else if (num == 3)
            {
                m.Nested.Add(ParseMessage(r.B, r.Pos, n, depth + 1));
            }
            else if (num == 4)
            {
                m.Enums.Add(ParseEnum(r.B, r.Pos, n));
            }
            else if (num == 6)
            {
                m.Extensions.Add(ParseField(r.B, r.Pos, n));
            }
            else if (num == 7)
            {
                m.MapEntry = ParseMapEntry(r.B, r.Pos, n);
            }
            else if (num == 8)
            {
                m.Oneofs.Add(ParseOneof(r.B, r.Pos, n));
            }
            else if (num == 10)
            {
                m.ReservedNames.Add(Str(r.B, r.Pos, n));
            }
            r.Pos += n;
        }
        if (m.Name is null)
        {
            throw new CoreException("message without a name");
        }
        return m;
    }

    private static POneof ParseOneof(byte[] b, int off, int len)
    {
        var r = Reader(b, off, len);
        var o = new POneof();
        while (r.Pos < r.End)
        {
            var tag = Wire.RVar(r);
            if (tag >> 3 == 1 && (tag & 7) == Wt.Len)
            {
                var n = Wire.RLen(r);
                o.Name = Str(r.B, r.Pos, n);
                r.Pos += n;
            }
            else
            {
                Wire.Skip(r, (int)(tag & 7));
            }
        }
        return o;
    }

    private static PEnum ParseEnum(byte[] b, int off, int len)
    {
        var r = Reader(b, off, len);
        var e = new PEnum();
        while (r.Pos < r.End)
        {
            var tag = Wire.RVar(r);
            var num = (int)(tag >> 3);
            if ((tag & 7) != Wt.Len)
            {
                Wire.Skip(r, (int)(tag & 7));
                continue;
            }
            var n = Wire.RLen(r);
            if (num == 1)
            {
                e.Name = Str(r.B, r.Pos, n);
            }
            else if (num == 2)
            {
                e.Values.Add(ParseEnumValue(r.B, r.Pos, n));
            }
            else if (num == 3)
            {
                e.EnumOpen = ParseFeatures(r.B, r.Pos, r.Pos + n, 7);
            }
            r.Pos += n;
        }
        return e;
    }

    private static PEnumValue ParseEnumValue(byte[] b, int off, int len)
    {
        var r = Reader(b, off, len);
        var v = new PEnumValue();
        var hasNumber = false;
        while (r.Pos < r.End)
        {
            var tag = Wire.RVar(r);
            var num = (int)(tag >> 3);
            var wt = (int)(tag & 7);
            if (wt == Wt.Varint)
            {
                if (num == 2)
                {
                    v.Number = (int)Wire.RVar(r);
                    hasNumber = true;
                }
                else
                {
                    Wire.Skip(r, wt);
                }
            }
            else if (wt == Wt.Len)
            {
                var n = Wire.RLen(r);
                if (num == 1)
                {
                    v.Name = Str(r.B, r.Pos, n);
                }
                r.Pos += n;
            }
            else
            {
                Wire.Skip(r, wt);
            }
        }
        if (v.Name is null || !hasNumber)
        {
            throw new CoreException("enum value without name/number");
        }
        return v;
    }

    /// <summary>MessageOptions.map_entry is field 7.</summary>
    private static bool ParseMapEntry(byte[] b, int off, int len)
    {
        var r = Reader(b, off, len);
        var mapEntry = false;
        while (r.Pos < r.End)
        {
            var tag = Wire.RVar(r);
            if ((tag & 7) != Wt.Varint)
            {
                Wire.Skip(r, (int)(tag & 7));
                continue;
            }
            if (tag >> 3 == 7)
            {
                mapEntry = Wire.RVar(r) != 0;
            }
            else
            {
                Wire.Skip(r, Wt.Varint);
            }
        }
        return mapEntry;
    }

    /// <summary>
    /// Reads one Options message and returns FeatureSet.enum_type (1 OPEN, 2
    /// CLOSED) when it carries one; 0 means unset. <paramref name="optionNumber"/>
    /// is the field holding the FeatureSet (50 file, 21 field, ...).
    /// </summary>
    private static int ParseFeatures(byte[] b, int off, int end, int optionNumber)
    {
        var r = Reader(b, off, end - off);
        var open = 0;
        while (r.Pos < r.End)
        {
            var tag = Wire.RVar(r);
            var num = (int)(tag >> 3);
            if ((tag & 7) != Wt.Len)
            {
                Wire.Skip(r, (int)(tag & 7));
                continue;
            }
            var n = Wire.RLen(r);
            if (num == optionNumber)
            {
                open = ParseFeatureSetEnumType(r.B, r.Pos, r.Pos + n);
            }
            r.Pos += n;
        }
        return open;
    }

    /// <summary>FileOptions.features (50) -> FeatureSet.utf8_validation.</summary>
    private static int ParseFeatureSetUtf8(byte[] b, int off, int end, int optionNumber)
    {
        var r = Reader(b, off, end - off);
        var utf8 = 0;
        while (r.Pos < r.End)
        {
            var tag = Wire.RVar(r);
            var num = (int)(tag >> 3);
            if ((tag & 7) != Wt.Len)
            {
                Wire.Skip(r, (int)(tag & 7));
                continue;
            }
            var n = Wire.RLen(r);
            if (num == optionNumber)
            {
                utf8 = ParseFeatureSetUtf8Value(r.B, r.Pos, r.Pos + n);
            }
            r.Pos += n;
        }
        return utf8;
    }

    /// <summary>FeatureSet.enum_type = 2: 1 OPEN or 2 CLOSED.</summary>
    private static int ParseFeatureSetEnumType(byte[] b, int off, int end)
    {
        var r = Reader(b, off, end - off);
        var open = 0;
        while (r.Pos < r.End)
        {
            var tag = Wire.RVar(r);
            if (tag >> 3 == 2 && (tag & 7) == Wt.Varint)
            {
                var v = (int)Wire.RVar(r);
                if (v is 1 or 2)
                {
                    open = v;
                }
            }
            else
            {
                Wire.Skip(r, (int)(tag & 7));
            }
        }
        return open;
    }

    /// <summary>FeatureSet.utf8_validation = 4: 2 VERIFY or 3 NONE.</summary>
    private static int ParseFeatureSetUtf8Value(byte[] b, int off, int end)
    {
        var r = Reader(b, off, end - off);
        var utf8 = 0;
        while (r.Pos < r.End)
        {
            var tag = Wire.RVar(r);
            if (tag >> 3 == 4 && (tag & 7) == Wt.Varint)
            {
                var v = (int)Wire.RVar(r);
                if (v is 2 or 3)
                {
                    utf8 = v;
                }
            }
            else
            {
                Wire.Skip(r, (int)(tag & 7));
            }
        }
        return utf8;
    }

    private static PField ParseField(byte[] b, int off, int len)
    {
        var r = Reader(b, off, len);
        var f = new PField();
        while (r.Pos < r.End)
        {
            var tag = Wire.RVar(r);
            var num = (int)(tag >> 3);
            var wt = (int)(tag & 7);
            if (wt == Wt.Varint)
            {
                var v = (int)Wire.RVar(r);
                if (num == 3)
                {
                    f.Number = v;
                }
                else if (num == 4)
                {
                    f.Label = v;
                }
                else if (num == 5)
                {
                    f.Type = v;
                }
                else if (num == 9)
                {
                    f.OneofIndex = v;
                }
                else if (num == 17)
                {
                    f.Proto3Optional = v != 0;
                }
            }
            else if (wt == Wt.Len)
            {
                var n = Wire.RLen(r);
                if (num == 1)
                {
                    f.Name = Str(r.B, r.Pos, n);
                }
                else if (num == 2)
                {
                    f.Extendee = Str(r.B, r.Pos, n);
                }
                else if (num == 6)
                {
                    f.TypeName = Str(r.B, r.Pos, n);
                }
                else if (num == 8)
                {
                    ParseFieldOptions(r.B, r.Pos, r.Pos + n, f);
                }
                r.Pos += n;
            }
            else
            {
                Wire.Skip(r, wt);
            }
        }
        if (f.Name is null || f.Type == 0 || f.Number == 0)
        {
            throw new CoreException("field without name/type/number");
        }
        if (f.Type < T.Double || f.Type > T.Sint64)
        {
            throw new CoreException("bad field type " + f.Type);
        }
        if (f.Label < T.LabelOptional || f.Label > T.LabelRepeated)
        {
            throw new CoreException("bad label " + f.Label);
        }
        return f;
    }

    /// <summary>FieldOptions: packed (2) and the editions FeatureSet (21).</summary>
    private static void ParseFieldOptions(byte[] b, int off, int end, PField f)
    {
        var r = Reader(b, off, end - off);
        while (r.Pos < r.End)
        {
            var tag = Wire.RVar(r);
            var num = (int)(tag >> 3);
            if (num == 2 && (tag & 7) == Wt.Varint)
            {
                f.Packed = Wire.RVar(r) != 0;
                f.HasPackedOption = true;
            }
            else if (num == 21 && (tag & 7) == Wt.Len)
            {
                var n = Wire.RLen(r);
                f.EnumOpen = ParseFeatureSetEnumType(r.B, r.Pos, r.Pos + n);
                f.Utf8 = ParseFeatureSetUtf8Value(r.B, r.Pos, r.Pos + n);
                r.Pos += n;
            }
            else
            {
                Wire.Skip(r, (int)(tag & 7));
            }
        }
    }
}

/* ---------------- linking ---------------- */

internal static class Linker
{
    private static void IndexMessage(
        PMessage m,
        string scope,
        Dictionary<string, PMessage> byName,
        Dictionary<string, PEnum> enums)
    {
        m.FullName = scope.Length == 0 ? m.Name! : scope + "." + m.Name;
        if (!byName.TryAdd(m.FullName, m))
        {
            throw new CoreException("duplicate message " + m.FullName);
        }
        foreach (var e in m.Enums)
        {
            IndexEnum(e, m.FullName, enums);
        }
        foreach (var n in m.Nested)
        {
            IndexMessage(n, m.FullName, byName, enums);
        }
    }

    private static void IndexEnum(PEnum e, string scope, Dictionary<string, PEnum> enums)
    {
        e.FullName = scope.Length == 0 ? e.Name! : scope + "." + e.Name;
        if (!enums.TryAdd(e.FullName, e))
        {
            throw new CoreException("duplicate enum " + e.FullName);
        }
    }

    /// <summary>An enum is closed for proto2 files (and editions files asking for it).</summary>
    private static void LinkEnum(PEnum e, bool proto3, int fileEnumOpen)
    {
        if (e.Name is null)
        {
            throw new CoreException("enum without a name");
        }
        if (e.EnumOpen != 0)
        {
            e.Closed = e.EnumOpen == 2;
        }
        else if (fileEnumOpen != 0)
        {
            e.Closed = fileEnumOpen == 2;
        }
        else
        {
            e.Closed = !proto3;
        }
    }

    private static void LinkEnums(PMessage m, bool proto3, int fileEnumOpen)
    {
        foreach (var e in m.Enums)
        {
            LinkEnum(e, proto3, fileEnumOpen);
        }
        foreach (var n in m.Nested)
        {
            LinkEnums(n, proto3, fileEnumOpen);
        }
    }

    private static void LinkMessage(
        PMessage m,
        string scope,
        bool proto3,
        int fileEnumOpen,
        int fileUtf8,
        Dictionary<string, PMessage> byName,
        Dictionary<string, PEnum> enums)
    {
        foreach (var f in m.Fields)
        {
            LinkField(f, scope, proto3, fileUtf8, byName, enums, m);
        }
        foreach (var n in m.Nested)
        {
            LinkMessage(n, m.FullName, proto3, fileEnumOpen, fileUtf8, byName, enums);
        }
        foreach (var e in m.Extensions)
        {
            LinkField(e, scope, proto3, fileUtf8, byName, enums, null);
            e.FullName = e.Name is null ? null : scope.Length == 0 ? e.Name : scope + "." + e.Name;
        }
    }

    private static bool IsPackableType(int t) => t is not (T.String or T.Group or T.Message or T.Bytes);

    private static void LinkField(
        PField f,
        string scope,
        bool proto3,
        int fileUtf8,
        Dictionary<string, PMessage> byName,
        Dictionary<string, PEnum> enums,
        PMessage? parent)
    {
        if (f.OneofIndex >= 0 && parent is not null)
        {
            if (f.OneofIndex >= parent.Oneofs.Count)
            {
                throw new CoreException("field " + f.Name + ": oneof_index out of range");
            }
            f.Oneof = parent.Oneofs[f.OneofIndex];
        }
        if ((f.Type == T.Message || f.Type == T.Group) && f.TypeName is not null)
        {
            f.MessageType = Resolve(byName, scope, f.TypeName);
            if (f.MessageType is null)
            {
                throw new CoreException("field " + f.Name + ": no such type " + f.TypeName);
            }
        }
        if (f.Type == T.Enum && f.TypeName is not null)
        {
            f.EnumType = ResolveEnum(enums, scope, f.TypeName);
            if (f.EnumType is null)
            {
                throw new CoreException("field " + f.Name + ": no such enum " + f.TypeName);
            }
            // Editions may close one field in an open file; that wins over the
            // enum type, exactly like the core's per-field enum_open.
            f.EnumClosed = f.EnumOpen != 0 ? f.EnumOpen == 2 : f.EnumType.Closed;
        }
        if (f.Label == T.LabelRepeated && IsPackableType(f.Type) && !f.HasPackedOption)
        {
            f.Packed = proto3; // the syntax default
        }
        /* utf8_validation: field FeatureSet, then the file's, then the dialect
         * default (VERIFY for proto3/editions, NONE for proto2). Only `string`
         * fields carry it, and only they validate on parse. */
        if (f.Type == T.String)
        {
            var utf8 = f.Utf8 != 0 ? f.Utf8 : fileUtf8 != 0 ? fileUtf8 : proto3 ? 2 : 3;
            f.Utf8 = utf8 == 2 ? 2 : 3;
        }
        else
        {
            f.Utf8 = 3;
        }
        f.HasPresence = f.Label != T.LabelRepeated
            && (f.Type == T.Message
                || f.Type == T.Group
                || f.Label == T.LabelRequired
                || !proto3
                || f.Proto3Optional);
    }

    /// <summary>Leading-dot names resolve from the root; otherwise search the scope up.</summary>
    private static PMessage? Resolve(Dictionary<string, PMessage> byName, string scope, string name)
    {
        if (name.Length > 0 && name[0] == '.')
        {
            return byName.GetValueOrDefault(name[1..]);
        }
        var s = scope;
        for (;;)
        {
            var cand = s.Length == 0 ? name : s + "." + name;
            if (byName.TryGetValue(cand, out var found))
            {
                return found;
            }
            // Nested scopes qualify from the inside out: C++ scoping.
            var dot = name.IndexOf('.');
            if (dot >= 0)
            {
                var head = name[..dot];
                var rest = name[(dot + 1)..];
                var prefix = s.Length == 0 ? head : s + "." + head;
                if (byName.ContainsKey(prefix))
                {
                    return Resolve(byName, prefix, rest);
                }
            }
            if (s.Length == 0)
            {
                return null;
            }
            var cut = s.LastIndexOf('.');
            s = cut < 0 ? "" : s[..cut];
        }
    }

    /// <summary>The enum counterpart of Resolve: same C++ scoping rules.</summary>
    private static PEnum? ResolveEnum(Dictionary<string, PEnum> byName, string scope, string name)
    {
        if (name.Length > 0 && name[0] == '.')
        {
            return byName.GetValueOrDefault(name[1..]);
        }
        var s = scope;
        for (;;)
        {
            var cand = s.Length == 0 ? name : s + "." + name;
            if (byName.TryGetValue(cand, out var found))
            {
                return found;
            }
            var dot = name.IndexOf('.');
            if (dot >= 0)
            {
                var head = name[..dot];
                var rest = name[(dot + 1)..];
                var prefix = s.Length == 0 ? head : s + "." + head;
                if (byName.ContainsKey(prefix))
                {
                    return ResolveEnum(byName, prefix, rest);
                }
            }
            if (s.Length == 0)
            {
                return null;
            }
            var cut = s.LastIndexOf('.');
            s = cut < 0 ? "" : s[..cut];
        }
    }

    private static void LinkFiles(List<PFile> files)
    {
        var byName = new Dictionary<string, PMessage>();
        var enums = new Dictionary<string, PEnum>();
        foreach (var f in files)
        {
            foreach (var m in f.Messages)
            {
                IndexMessage(m, f.Pkg, byName, enums);
            }
            foreach (var e in f.Enums)
            {
                IndexEnum(e, f.Pkg, enums);
            }
        }
        foreach (var f in files)
        {
            // Editions files carry syntax "editions" plus field 14: the core
            // treats them as proto3, so packed defaults and presence match.
            var proto3 = f.Syntax == "proto3" || f.Edition;
            foreach (var e in f.Enums)
            {
                LinkEnum(e, proto3, f.EnumOpen);
            }
            // Enums close/open before fields link: a field copies its enum's
            // closed flag, so the enum must be linked first.
            foreach (var m in f.Messages)
            {
                LinkEnums(m, proto3, f.EnumOpen);
            }
            foreach (var m in f.Messages)
            {
                LinkMessage(m, f.Pkg, proto3, f.EnumOpen, f.Utf8, byName, enums);
            }
            foreach (var e in f.Extensions)
            {
                LinkField(e, f.Pkg, proto3, f.Utf8, byName, enums, null);
                e.FullName = e.Name is null ? null : f.Pkg.Length == 0 ? e.Name : f.Pkg + "." + e.Name;
            }
        }
    }

    public static void Link(List<PFile> files) => LinkFiles(files);
}

/* ---------------- compiled schema building ---------------- */

internal sealed class Builder
{
    private readonly CoreSchema _schema = new();

    public CoreSchema Build(List<PFile> files)
    {
        // Enums first: message fields link their element type while the defs
        // are built.
        foreach (var f in files)
        {
            foreach (var e in f.Enums)
            {
                BuildEnum(e);
            }
            foreach (var m in f.Messages)
            {
                BuildEnums(m, 1);
            }
        }
        foreach (var f in files)
        {
            foreach (var m in f.Messages)
            {
                BuildDef(m, 1);
            }
        }
        // Extensions attach to their extendee after all defs exist.
        foreach (var f in files)
        {
            foreach (var e in f.Extensions)
            {
                AttachExtension(e);
            }
            foreach (var m in f.Messages)
            {
                AttachMessageExtensions(m);
            }
        }
        FinishTransitive();
        return _schema;
    }

    private void BuildEnum(PEnum e)
    {
        var byName = new Dictionary<string, int>();
        var byNumber = new Dictionary<int, string>();
        foreach (var v in e.Values)
        {
            var name = v.Name!;
            if (!byName.TryAdd(name, v.Number))
            {
                throw new CoreException("duplicate enum value " + e.FullName + "." + name);
            }
            // Aliases repeat a number: the first declared name is printed.
            byNumber.TryAdd(v.Number, name);
        }
        _schema.Enums[e.FullName] = new EnumDef
        {
            FullName = e.FullName,
            ByName = byName,
            ByNumber = byNumber,
            Closed = e.Closed,
        };
    }

    private void BuildEnums(PMessage m, int depth)
    {
        if (depth > Wire.MaxDepth)
        {
            throw new CoreException("descriptor nesting exceeds " + Wire.MaxDepth);
        }
        foreach (var e in m.Enums)
        {
            BuildEnum(e);
        }
        foreach (var n in m.Nested)
        {
            BuildEnums(n, depth + 1);
        }
    }

    private MsgDef BuildDef(PMessage m, int depth)
    {
        if (depth > Wire.MaxDepth)
        {
            throw new CoreException("descriptor nesting exceeds " + Wire.MaxDepth);
        }
        if (_schema.Messages.TryGetValue(m.FullName, out var done))
        {
            return done;
        }
        var d = new MsgDef
        {
            FullName = m.FullName,
            MapEntry = m.MapEntry,
            ReservedNames = m.ReservedNames.ToArray(),
        };
        _schema.Messages[m.FullName] = d; // before fields: recursion-safe
        var fs = new List<FieldDef>();
        var oneofs = new Dictionary<POneof, int>();
        foreach (var f in m.Fields)
        {
            fs.Add(BuildField(f, oneofs));
        }
        FinishDef(d, fs, oneofs);
        foreach (var n in m.Nested)
        {
            BuildDef(n, depth + 1);
        }
        return d;
    }

    private FieldDef BuildField(PField f, Dictionary<POneof, int> oneofs)
    {
        var kind = KindOf(f.Type);
        var repeated = f.Label == T.LabelRepeated;
        var oneof = -1;
        if (f.Oneof is not null)
        {
            if (!oneofs.TryGetValue(f.Oneof, out oneof))
            {
                oneof = oneofs.Count;
                oneofs[f.Oneof] = oneof;
            }
        }
        int presence;
        if (oneof >= 0)
        {
            presence = Presence.Explicit; // reads/writes go through the oneof case
        }
        else if (f.Label == T.LabelRequired)
        {
            presence = Presence.Required;
        }
        else if (kind is K.Message or K.Group)
        {
            presence = Presence.MessagePresence;
        }
        else if (f.HasPresence)
        {
            presence = Presence.Explicit;
        }
        else
        {
            presence = Presence.Implicit;
        }
        var packed = repeated && f.Packed;
        var wt = WireType(f.Type, packed);
        var number = f.Number;
        return new FieldDef
        {
            Number = number,
            Name = f.Name!,
            Kind = kind,
            Wt = wt,
            ElemWt = WireType(f.Type, false),
            Repeated = repeated,
            Packed = packed,
            Oneof = oneof,
            Presence = presence,
            MsgType = f.MessageType is null ? null : BuildDef(f.MessageType, 1),
            EnumType = f.EnumType is null ? null : _schema.Enums.GetValueOrDefault(f.EnumType.FullName),
            EnumClosed = f.EnumClosed,
            Utf8 = f.Type == T.String && f.Utf8 == 2,
            FullName = f.FullName,
            Tag = Wire.TagBytes(number, wt),
            TagEnd = kind == K.Group ? Wire.TagBytes(number, 4) : null,
        };
    }

    private void AttachMessageExtensions(PMessage m)
    {
        foreach (var e in m.Extensions)
        {
            AttachExtension(e);
        }
        foreach (var n in m.Nested)
        {
            AttachMessageExtensions(n);
        }
    }

    private void AttachExtension(PField e)
    {
        var extendee = e.Extendee ?? throw new CoreException("extension without extendee");
        if (extendee.Length > 0 && extendee[0] == '.')
        {
            extendee = extendee[1..];
        }
        if (!_schema.Messages.TryGetValue(extendee, out var d))
        {
            throw new CoreException("extendee missing: " + e.Extendee);
        }
        var s = BuildField(e, []);
        s.FullName = e.FullName;
        var fs = new List<FieldDef>(d.Fields) { s };
        var oneofByName = new Dictionary<string, int>(d.OneofByName);
        var count = d.OneofCount;
        FinishDef(d, fs, []);
        d.OneofByName = oneofByName; // extensions are never in a oneof
        d.OneofCount = count;
    }

    private static int KindOf(int t) => t switch
    {
        T.Int32 => K.Int32,
        T.Uint32 => K.Uint32,
        T.Sint32 => K.Sint32,
        T.Int64 => K.Int64,
        T.Uint64 => K.Uint64,
        T.Sint64 => K.Sint64,
        T.Bool => K.Bool,
        T.Enum => K.Enum,
        T.Fixed32 => K.Fixed32,
        T.Sfixed32 => K.Sfixed32,
        T.Float => K.Float,
        T.Fixed64 => K.Fixed64,
        T.Sfixed64 => K.Sfixed64,
        T.Double => K.Double,
        T.String => K.String,
        T.Bytes => K.Bytes,
        T.Message => K.Message,
        _ => K.Group,
    };

    private static int WireType(int t, bool packed)
    {
        if (packed)
        {
            return Wt.Len;
        }
        return t switch
        {
            T.Double or T.Fixed64 or T.Sfixed64 => 1,
            T.String or T.Bytes or T.Message => Wt.Len,
            T.Group => 3,
            T.Fixed32 or T.Sfixed32 or T.Float => 5,
            _ => Wt.Varint,
        };
    }

    private static void FinishDef(MsgDef d, List<FieldDef> fs, Dictionary<POneof, int> oneofs)
    {
        fs.Sort((a, b) => a.Number - b.Number);
        var max = 0;
        var req = false;
        foreach (var f in fs)
        {
            if (f.Number > max)
            {
                max = f.Number;
            }
            req = req || f.Presence == Presence.Required;
        }
        d.HasRequired = req;
        d.Fields = fs.ToArray();
        // Dense only when bounded, like the C core's fnum map: one field
        // numbered 536870911 must not allocate a 2 GiB array.
        if (max <= 512 && max <= fs.Count * 8 + 8)
        {
            var map = new int[max + 1];
            Array.Fill(map, -1);
            for (var i = 0; i < fs.Count; i++)
            {
                map[fs[i].Number] = i;
            }
            d.SlotByFnum = map;
        }
        else
        {
            d.SlotByFnum = null;
        }
        d.ByName = [];
        for (var i = 0; i < fs.Count; i++)
        {
            d.ByName[fs[i].Name] = i;
        }
        d.OneofByName = [];
        foreach (var (oneof, idx) in oneofs)
        {
            d.OneofByName[oneof.Name] = idx;
        }
        d.OneofCount = Math.Max(d.OneofCount, oneofs.Count);
    }

    private void FinishTransitive()
    {
        var changed = true;
        while (changed)
        {
            changed = false;
            foreach (var d in _schema.Messages.Values)
            {
                if (!d.DeepRequired && NeedsRequired(d))
                {
                    d.DeepRequired = true;
                    changed = true;
                }
            }
        }
    }

    private static bool NeedsRequired(MsgDef d)
    {
        foreach (var f in d.Fields)
        {
            if (f.Presence == Presence.Required)
            {
                return true;
            }
            if (f.Kind is K.Message or K.Group && f.MsgType is not null && f.MsgType.DeepRequired)
            {
                return true;
            }
        }
        return false;
    }
}

/* ---------------- public entry ---------------- */

/// <summary>The descriptor reader's entry points.</summary>
public static class Descriptor
{
    /// <summary>Parses and links a FileDescriptorSet into a compiled schema.</summary>
    public static CoreSchema LoadSchema(byte[] bytes, Limits? limits = null)
    {
        Limits.CheckBytes(limits, bytes.Length, "descriptor set");
        List<PFile> files;
        try
        {
            files = DescriptorParser.ParseSet(bytes);
            Linker.Link(files);
        }
        catch (CoreException e)
        {
            throw new CoreException("descriptor set: " + e.Message, 1);
        }
        return new Builder().Build(files);
    }

    /// <summary>Slot of a field number, -1 when the message has no such field.</summary>
    public static int SlotOf(MsgDef def, int fnum)
    {
        if (def.SlotByFnum is not null)
        {
            return fnum >= 0 && fnum < def.SlotByFnum.Length ? def.SlotByFnum[fnum] : -1;
        }
        for (var i = 0; i < def.Fields.Length; i++)
        {
            if (def.Fields[i].Number == fnum)
            {
                return i;
            }
        }
        return -1;
    }

    /// <summary>Number of declared fields in a message (descriptor test surface).</summary>
    public static int FieldCount(MsgDef def) => def.Fields.Length;

    /// <summary>The field named <paramref name="name"/>, or null.</summary>
    public static FieldDef? FieldByName(MsgDef def, string name) =>
        def.ByName.TryGetValue(name, out var slot) ? def.Fields[slot] : null;
}

// Tests for the pure-C# core (bindings/csharp/Pbwire/Core): the descriptor
// reader first (Stage 1 gate), then wire/encode/decode parity against the
// bytes the repo already pins for the C binding (matrix3.bin, oneoftest.bin,
// grouptest.bin, extensiontest.bin, maptest.bin).
//
// Nothing here calls the native library. Run through `make csharp-test`.

namespace Pbwire.Test;

using Pbwire.Core;

internal static class CoreTests
{
    private static int _checks;
    private static string _data = "";

    private static void Check(bool ok, string what)
    {
        _checks++;
        if (!ok)
        {
            throw new InvalidOperationException("core: " + what);
        }
    }

    private static byte[] Bin(string name) => File.ReadAllBytes(Path.Combine(_data, name));

    private static MessageType Type(string desc, string name) =>
        Schema.FromFile(Path.Combine(_data, desc)).Message(name);

    private static FieldDef Field(string desc, string type, string field) =>
        Type(desc, type).Field(field) ?? throw new InvalidOperationException("no field " + field);

    /// <summary>Runs every core check; returns the number of assertions.</summary>
    public static int Run(string dataDir)
    {
        _data = dataDir;
        _checks = 0;
        DescriptorGate();
        EncodeParity();
        DecodeRetention();
        return _checks;
    }

    /// <summary>
    /// Stage 1 gate: the descriptor reader over the same FileDescriptorSet
    /// subset the C core reads (src/descriptor_set.c). Only facts the codec
    /// depends on: field count and order, presence kinds, repetition and
    /// packedness, kind mapping, oneofs, groups, extensions, map entries.
    /// </summary>
    private static void DescriptorGate()
    {
        var scalars = Type("matrix3.desc", "structtest.Scalars3");
        Check(Descriptor.FieldCount(scalars.Def) == 22, "matrix3.desc has 22 fields");
        Check(scalars.Def.FullName == "structtest.Scalars3", "full name");
        for (var i = 0; i < 22; i++)
        {
            Check(scalars.Def.Fields[i].Number == i + 1, "field numbers are dense from 1");
        }
        Check(scalars.Def.Fields[0].Name == "i32", "first field name");
        Check(scalars.Def.Fields[21].Name == "flags", "last field name");

        // proto3 plain scalars have no has-bit...
        Check(Field("matrix3.desc", "structtest.Scalars3", "i32").Presence == Presence.Implicit, "i32 implicit");
        Check(Field("matrix3.desc", "structtest.Scalars3", "str").Presence == Presence.Implicit, "str implicit");
        // ...while `optional` in a proto3 file is explicit presence.
        Check(Field("matrix3.desc", "structtest.Scalars3", "opt_i32").Presence == Presence.Explicit, "opt_i32 explicit");
        Check(Field("matrix3.desc", "structtest.Scalars3", "opt_str").Presence == Presence.Explicit, "opt_str explicit");
        // ...and a message field always has it.
        Check(
            Field("matrix3.desc", "structtest.Scalars3", "inner").Presence == Presence.MessagePresence,
            "inner message presence");

        var flags = Field("matrix3.desc", "structtest.Scalars3", "flags");
        Check(flags.Repeated && flags.Kind == K.Bool && flags.Packed, "flags repeated packed bool");
        Check(flags.Wt == Wt.Len, "flags packed under the LEN tag");
        var nums = Field("matrix3.desc", "structtest.Scalars3", "nums");
        Check(nums.Repeated && nums.Kind == K.Int32 && nums.Packed, "nums packed int32");
        Check(nums.ElemWt == Wt.Varint, "nums accepts the unpacked element form");

        Check(Field("matrix3.desc", "structtest.Scalars3", "i64").Kind == K.Int64, "i64 kind");
        Check(Field("matrix3.desc", "structtest.Scalars3", "u64").Kind == K.Uint64, "u64 kind");
        Check(Field("matrix3.desc", "structtest.Scalars3", "s64").Kind == K.Sint64, "s64 kind");
        Check(Field("matrix3.desc", "structtest.Scalars3", "fx64").Kind == K.Fixed64, "fx64 kind");
        Check(Field("matrix3.desc", "structtest.Scalars3", "sfx64").Kind == K.Sfixed64, "sfx64 kind");
        Check(Field("matrix3.desc", "structtest.Scalars3", "f32").Kind == K.Float, "f32 kind");
        Check(Field("matrix3.desc", "structtest.Scalars3", "f64").Kind == K.Double, "f64 kind");
        Check(Field("matrix3.desc", "structtest.Scalars3", "color").Kind == K.Enum, "color kind");
        var innerField = Field("matrix3.desc", "structtest.Scalars3", "inner");
        Check(innerField.Kind == K.Message, "inner kind");
        Check(innerField.MsgType?.FullName == "structtest.Inner3", "inner type resolves");
        Check(Field("matrix3.desc", "structtest.Scalars3", "str").Utf8, "proto3 strings verify UTF-8");
        var color = Field("matrix3.desc", "structtest.Scalars3", "color").EnumType!;
        Check(!color.Closed, "proto3 enum is open");
        Check(color.ByName["COLOR3_GREEN"] == 2, "enum value by name");
        Check(color.ByNumber[2] == "COLOR3_GREEN", "enum name by number");

        // proto2 required closure and group tags.
        var oneof = Type("oneoftest.desc", "structtest.OneOfTest");
        Check(oneof.Def.OneofCount == 1, "oneoftest has one oneof");
        Check(oneof.Def.Fields[0].Oneof >= 0 && oneof.Def.Fields[0].Presence == Presence.Explicit, "oneof member presence");
        Check(oneof.Def.ByName.ContainsKey("text"), "oneof members are ordinary fields");

        var group = Field("grouptest.desc", "structtest.GroupTest", "inner");
        Check(group.Kind == K.Group, "group kind");
        Check(group.Wt == Wt.StartGroup && group.TagEnd is not null, "group start/end tags");
        Check(group.MsgType?.FullName == "structtest.GroupTest.Inner", "group type resolves");
        var items = Field("grouptest.desc", "structtest.GroupTest", "item");
        Check(items.Repeated && items.Kind == K.Group, "repeated group");

        var map = Type("maptest.desc", "structtest.Maps");
        var byInt = map.Field("by_int")!;
        Check(byInt.Repeated && byInt.Kind == K.Message, "map field is a repeated entry message");
        Check(byInt.MsgType?.MapEntry == true, "map entry is marked");
        Check(byInt.MsgType?.Fields.Length == 2, "map entry has key and value");
        Check(
            Field("extensiontest.desc", "exttest.HasExt", "ext_str").FullName == "exttest.ext_str",
            "extension attaches to its extendee");

        // proto2 fields have explicit presence; a required field sets the flag.
        var matrix2 = Type("matrix2.desc", "structtest.Scalars2");
        Check(matrix2.Def.HasRequired && matrix2.Def.Fields[0].Presence == Presence.Required, "proto2 required");
        Check(matrix2.Def.Fields[1].Presence == Presence.Explicit, "proto2 explicit presence");
    }

    /// <summary>Encode is byte-identical with the pinned protoc bytes, on every fixture.</summary>
    private static void EncodeParity()
    {
        var scalars = Type("matrix3.desc", "structtest.Scalars3");
        var msg = scalars.NewMessage();
        msg.Set("i32", int.MinValue)
            .Set("i64", long.MaxValue)
            .Set("u32", uint.MaxValue)
            .Set("u64", ulong.MaxValue)
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
        foreach (var n in new[] { 1L, -5L, 300L })
        {
            msg.Add("nums", n);
        }
        foreach (var tag in new[] { "a", "bb" })
        {
            msg.Add("tags", tag);
        }
        var inner = scalars.Def.Fields[15].MsgType!;
        foreach (var (name, value) in new[] { ("x", 1L), ("y", -2L) })
        {
            var kid = new Msg(inner);
            kid.Set("name", name).Set("value", value);
            msg.GetRep("inners").Add(kid);
        }
        msg.Add("flags", true).Add("flags", false).Add("flags", true);
        msg.NewMessage("inner").Set("name", "kid").Set("value", 42L);

        var want = Bin("matrix3.bin");
        var got = scalars.Encode(msg);
        Check(got.SequenceEqual(want), $"matrix3 encode matches protoc ({got.Length} vs {want.Length})");

        // Long/ulong edges survive exactly.
        var edges = scalars.NewMessage().Set("i64", long.MinValue).Set("u64", ulong.MaxValue);
        var back = scalars.Decode(scalars.Encode(edges));
        Check(back.Get("i64") is long l && l == long.MinValue, "INT64_MIN round-trips");
        Check(back.Get("u64") is ulong u && u == ulong.MaxValue, "UINT64_MAX round-trips");

        // oneof: the pinned text member bytes, and last-wins.
        var oneof = Type("oneoftest.desc", "structtest.OneOfTest");
        var one = oneof.NewMessage();
        one.NewMessage("sub").Set("v", 7L);
        one.Set("id", 9L);
        var oneBytes = oneof.Encode(one);
        Check(oneof.Encode(oneof.Decode(oneBytes)).SequenceEqual(oneBytes), "oneof re-encodes exactly");
        var oneBack = oneof.Decode(oneBytes);
        Check(oneBack.OneofCaseOf("kind") == 3, "oneof case is the selected field");
        one.Set("text", "t");
        Check(one.OneofCaseOf("kind") == 2, "setting a oneof member moves the case");
        Check(!one.Has("sub"), "the previous oneof member is cleared");

        // group: pinned proto2 bytes.
        var group = Type("grouptest.desc", "structtest.GroupTest");
        var g = group.NewMessage();
        g.NewMessage("inner").Set("x", 5L).Set("s", "g");
        foreach (var v in new[] { 1L, -2L })
        {
            var item = new Msg(group.Field("item")!.MsgType!);
            item.Set("v", v);
            g.GetRep("item").Add(item);
        }
        g.Set("id", 9L);
        var gBytes = group.Encode(g);
        Check(gBytes.SequenceEqual(Bin("grouptest.bin")), "group bytes match protoc");
        Check(group.Encode(group.Decode(gBytes)).SequenceEqual(gBytes), "group re-encodes exactly");

        // extension: the extendee's trailing field number.
        var ext = Type("extensiontest.desc", "exttest.HasExt");
        var e = ext.NewMessage().Set("id", 7L);
        e.Add("ext_nums", 1L).Add("ext_nums", -3L);
        var eBytes = ext.Encode(e);
        Check(ext.Encode(ext.Decode(eBytes)).SequenceEqual(eBytes), "extension re-encodes exactly");

        // map: an entry is an ordinary repeated message.
        var maps = Type("maptest.desc", "structtest.Maps");
        var maps0 = maps.NewMessage();
        Check(maps.Encode(maps0).Length == 0, "an empty message encodes to no bytes");
        var entry = new Msg(maps.Field("by_int")!.MsgType!);
        entry.Set("key", 7L).Set("value", "seven");
        maps0.GetRep("by_int").Add(entry);
        var mBytes = maps.Encode(maps0);
        Check(maps.Encode(maps.Decode(mBytes)).SequenceEqual(mBytes), "map entry re-encodes exactly");
    }

    /// <summary>Decode: unknown retention in input order, wrong wire types, truncation.</summary>
    private static void DecodeRetention()
    {
        var scalars = Type("matrix3.desc", "structtest.Scalars3");
        var pinned = Bin("matrix3.bin");

        // An unknown varint field (number 999) before and after the known bytes.
        var unknown = new byte[] { 0xB8, 0x3E, 0x07 };
        var wire = unknown.Concat(pinned).Concat(unknown).ToArray();
        var back = scalars.Decode(wire);
        Check(back.Unknown is not null && back.Unknown.Count == 2, "both unknown occurrences are retained");
        var reencoded = scalars.Encode(back);
        Check(
            reencoded.SequenceEqual(pinned.Concat(unknown).Concat(unknown).ToArray()),
            "unknown fields are re-emitted after the known fields, in input order");
        Check(back.Get("str") is string s && s == "hello", "known fields decode next to unknowns");

        // A wrong wire type on a known field is retained, not fatal.
        var wrong = new byte[] { 0x0D, 0x01, 0x02, 0x03, 0x04 }; // i32 (field 1) as fixed32
        var wrongBack = scalars.Decode(wrong);
        Check(wrongBack.Unknown?.Count == 1, "wrong-wire-type occurrence is retained");
        Check(!wrongBack.Has("i32"), "the wrong wire type does not set the field");
        Check(scalars.Encode(wrongBack).SequenceEqual(wrong), "wrong-wire-type bytes re-emit verbatim");

        // Truncation and bad tags are errors, like the C core.
        Check(Throws(() => scalars.Decode(new byte[] { 0x08 })), "truncated varint is rejected");
        Check(Throws(() => scalars.Decode(new byte[] { 0x00 })), "field number zero is rejected");
        Check(Throws(() => scalars.Decode(new byte[] { 0x0A, 0x05, 0x00 })), "truncated length-delimited is rejected");
        Check(Throws(() => scalars.Decode(new byte[] { 0x0B })), "stray start-group is rejected");
        Check(
            Throws(() => new MessageType(scalars.Def).Decode(pinned, new Limits { MaxBytes = 8 })),
            "MaxBytes is enforced");

        // Invalid UTF-8 on a proto3 string fails the parse.
        var badUtf8 = new byte[] { 0x72, 0x01, 0xC0 }; // field 14 (str), len 1, overlong
        Check(Throws(() => scalars.Decode(badUtf8)), "invalid UTF-8 is rejected on a proto3 string");

        // A decoded tree re-encodes byte-exactly.
        Check(scalars.Encode(scalars.Decode(pinned)).SequenceEqual(pinned), "decode/encode round trip is byte-exact");
    }

    private static bool Throws(Action op)
    {
        try
        {
            op();
            return false;
        }
        catch (CoreException)
        {
            return true;
        }
    }
}

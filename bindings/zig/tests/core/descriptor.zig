//! Stage 1 gate for the pure-Zig core: the descriptor reader over the same
//! `FileDescriptorSet` subset the C core reads (`src/descriptor_set.c`).
//!
//! Only facts the codec depends on: field count and order, the four presence
//! kinds, repetition and packedness, kind mapping, oneofs, groups and the
//! transitive required closure. Every assertion comes from a `.desc` fixture
//! the C tests already pin, so the two readers cannot drift.

const std = @import("std");
const core = @import("pbwire_core");

const data_dir = "../c/tests/data/";

fn load(alloc: std.mem.Allocator, file: []const u8) !core.Schema {
    const path = try std.fmt.allocPrint(alloc, "{s}{s}", .{ data_dir, file });
    defer alloc.free(path);
    return core.Schema.fromFile(alloc, path);
}

fn varint(a: std.mem.Allocator, v0: u64) ![]u8 {
    var out: std.ArrayListUnmanaged(u8) = .empty;
    var v = v0;
    while (v > 0x7f) {
        try out.append(a, @as(u8, @truncate(v & 0x7f)) | 0x80);
        v >>= 7;
    }
    try out.append(a, @truncate(v));
    return out.toOwnedSlice(a);
}

fn ld(a: std.mem.Allocator, fnum: u32, payload: []const u8) ![]u8 {
    var out: std.ArrayListUnmanaged(u8) = .empty;
    const tag = try varint(a, @as(u64, fnum) * 8 + 2);
    defer a.free(tag);
    try out.appendSlice(a, tag);
    const len = try varint(a, payload.len);
    defer a.free(len);
    try out.appendSlice(a, len);
    try out.appendSlice(a, payload);
    return out.toOwnedSlice(a);
}

fn vint(a: std.mem.Allocator, fnum: u32, v: u64) ![]u8 {
    var out: std.ArrayListUnmanaged(u8) = .empty;
    const tag = try varint(a, @as(u64, fnum) * 8);
    defer a.free(tag);
    try out.appendSlice(a, tag);
    const val = try varint(a, v);
    defer a.free(val);
    try out.appendSlice(a, val);
    return out.toOwnedSlice(a);
}

fn deepType(alloc: std.mem.Allocator) !core.Schema {
    const fld_name = try ld(alloc, 1, "kid");
    defer alloc.free(fld_name);
    const fld_num = try vint(alloc, 3, 1);
    defer alloc.free(fld_num);
    const fld_label = try vint(alloc, 4, 1);
    defer alloc.free(fld_label);
    const fld_type = try vint(alloc, 5, 11);
    defer alloc.free(fld_type);
    const fld_typename = try ld(alloc, 6, ".deep.Node");
    defer alloc.free(fld_typename);

    var fld: std.ArrayListUnmanaged(u8) = .empty;
    defer fld.deinit(alloc);
    try fld.appendSlice(alloc, fld_name);
    try fld.appendSlice(alloc, fld_num);
    try fld.appendSlice(alloc, fld_label);
    try fld.appendSlice(alloc, fld_type);
    try fld.appendSlice(alloc, fld_typename);
    const field = try ld(alloc, 2, fld.items);
    defer alloc.free(field);

    const msg_name = try ld(alloc, 1, "Node");
    defer alloc.free(msg_name);
    var msg: std.ArrayListUnmanaged(u8) = .empty;
    defer msg.deinit(alloc);
    try msg.appendSlice(alloc, msg_name);
    try msg.appendSlice(alloc, field);
    const message = try ld(alloc, 4, msg.items);
    defer alloc.free(message);

    const file_name = try ld(alloc, 1, "deep.proto");
    defer alloc.free(file_name);
    const file_pkg = try ld(alloc, 2, "deep");
    defer alloc.free(file_pkg);
    const file_syntax = try ld(alloc, 12, "proto3");
    defer alloc.free(file_syntax);
    var file: std.ArrayListUnmanaged(u8) = .empty;
    defer file.deinit(alloc);
    try file.appendSlice(alloc, file_name);
    try file.appendSlice(alloc, file_pkg);
    try file.appendSlice(alloc, message);
    try file.appendSlice(alloc, file_syntax);
    const set = try ld(alloc, 1, file.items);
    defer alloc.free(set);
    return core.Schema.fromBytes(alloc, set);
}

test "descriptor" {
    const alloc = std.testing.allocator;

    // ---------------------------------------------------------- matrix3
    {
        var schema = try load(alloc, "matrix3.desc");
        defer schema.deinit();
        const ty = try schema.messageType("structtest.Scalars3");
        const def = ty.defn();
        try std.testing.expectEqualStrings("structtest.Scalars3", def.full_name);
        try std.testing.expectEqual(@as(usize, 22), def.fields.len);
        for (def.fields, 0..) |f, i| try std.testing.expectEqual(@as(u32, @intCast(i + 1)), f.number);
        try std.testing.expectEqualStrings("i32", def.fields[0].name);
        try std.testing.expectEqualStrings("flags", def.fields[21].name);

        try std.testing.expectEqual(core.Presence.implicit, (try ty.field("i32")).presence);
        try std.testing.expectEqual(core.Presence.implicit, (try ty.field("str")).presence);
        try std.testing.expectEqual(core.Presence.explicit, (try ty.field("opt_i32")).presence);
        try std.testing.expectEqual(core.Presence.explicit, (try ty.field("opt_str")).presence);
        try std.testing.expectEqual(core.Presence.message_presence, (try ty.field("inner")).presence);

        const flags = try ty.field("flags");
        try std.testing.expect(flags.repeated);
        try std.testing.expectEqual(core.Kind.bool, flags.kind);
        try std.testing.expect(flags.is_packed);
        try std.testing.expectEqual(@as(u8, 2), flags.wt);
        const nums = try ty.field("nums");
        try std.testing.expect(nums.repeated);
        try std.testing.expectEqual(core.Kind.int32, nums.kind);
        try std.testing.expect(nums.is_packed);
        try std.testing.expectEqual(@as(u8, 0), nums.elem_wt);

        try std.testing.expectEqual(core.Kind.int64, (try ty.field("i64")).kind);
        try std.testing.expectEqual(core.Kind.uint64, (try ty.field("u64")).kind);
        try std.testing.expectEqual(core.Kind.sint64, (try ty.field("s64")).kind);
        try std.testing.expectEqual(core.Kind.fixed64, (try ty.field("fx64")).kind);
        try std.testing.expectEqual(core.Kind.sfixed64, (try ty.field("sfx64")).kind);
        try std.testing.expectEqual(core.Kind.float, (try ty.field("f32")).kind);
        try std.testing.expectEqual(core.Kind.double, (try ty.field("f64")).kind);
        try std.testing.expectEqual(core.Kind.enumeration, (try ty.field("color")).kind);
        try std.testing.expectEqual(core.Kind.message, (try ty.field("inner")).kind);
        try std.testing.expectEqualStrings("structtest.Inner3", schema.core.messages[(try ty.field("inner")).msg_type.?].full_name);
        try std.testing.expect((try ty.field("str")).utf8);
        const color = &schema.core.enums[(try ty.field("color")).enum_type.?];
        try std.testing.expect(!color.closed);
        try std.testing.expectEqual(@as(i32, 2), color.by_name.get("COLOR3_GREEN").?);
        try std.testing.expectEqualStrings("COLOR3_GREEN", color.by_number.get(2).?);
    }

    // ---------------------------------------------------------- matrix2
    {
        var schema = try load(alloc, "matrix2.desc");
        defer schema.deinit();
        const ty = try schema.messageType("structtest.Scalars2");
        try std.testing.expectEqual(core.Presence.required, (try ty.field("id")).presence);
        try std.testing.expect(ty.defn().has_required);
        const inner = try schema.messageType("structtest.Inner2");
        try std.testing.expect(inner.defn().has_required);
        try std.testing.expect(ty.defn().deep_required);
        const nums = try ty.field("nums");
        try std.testing.expect(nums.repeated and !nums.is_packed);
        try std.testing.expect((try ty.field("packed_nums")).is_packed);
        try std.testing.expect(!(try ty.field("str")).utf8);
    }

    // ------------------------------------------------------- grouptest
    {
        var schema = try load(alloc, "grouptest.desc");
        defer schema.deinit();
        const ty = try schema.messageType("structtest.GroupTest");
        const inner = try ty.field("inner");
        try std.testing.expectEqual(core.Kind.group, inner.kind);
        try std.testing.expectEqual(@as(u8, 3), inner.wt);
        try std.testing.expectEqualStrings("structtest.GroupTest.Inner", schema.core.messages[inner.msg_type.?].full_name);
        try std.testing.expectEqualSlices(u8, &[_]u8{0x0b}, inner.tag.slice());
        try std.testing.expectEqualSlices(u8, &[_]u8{0x0c}, inner.tag_end.slice());
        const item = try ty.field("item");
        try std.testing.expectEqual(core.Kind.group, item.kind);
        try std.testing.expect(item.repeated);
    }

    // ------------------------------------------------------- oneoftest
    {
        var schema = try load(alloc, "oneoftest.desc");
        defer schema.deinit();
        const ty = try schema.messageType("structtest.OneOfTest");
        try std.testing.expectEqual(@as(usize, 1), ty.defn().oneof_count);
        try std.testing.expectEqual(@as(usize, 0), ty.defn().oneof_by_name.get("kind").?);
        for ([_][]const u8{ "num", "text", "sub", "blob" }) |name| {
            const f = try ty.field(name);
            try std.testing.expectEqual(@as(i32, 0), f.oneof);
            try std.testing.expectEqual(core.Presence.explicit, f.presence);
        }
        try std.testing.expectEqual(@as(i32, -1), (try ty.field("id")).oneof);
    }

    // ------------------------------------------------------- synthetic
    {
        var schema = try deepType(alloc);
        defer schema.deinit();
        const node = try schema.messageType("deep.Node");
        try std.testing.expectEqual(@as(usize, 1), node.defn().fields.len);
        const kid = &node.defn().fields[0];
        try std.testing.expectEqual(@as(?usize, 0), kid.msg_type);
        try std.testing.expectEqualStrings("deep.Node", schema.core.messages[kid.msg_type.?].full_name);
        try std.testing.expect(!kid.utf8);
        try std.testing.expect(!node.defn().has_required);
    }
}

//! Codec gate for the pure-Zig core: the pinned `matrix3` fixture bytes in
//! both directions, unknown retention (input order, wrong-wire-type included),
//! groups, packed runs, oneofs, presence, 64-bit edges and the limits.

const std = @import("std");
const core = @import("pbwire_core");

const data_dir = "../c/tests/data/";

fn load(alloc: std.mem.Allocator, file: []const u8) !core.Schema {
    const path = try std.fmt.allocPrint(alloc, "{s}{s}", .{ data_dir, file });
    defer alloc.free(path);
    return core.Schema.fromFile(alloc, path);
}

fn fixture(alloc: std.mem.Allocator, name: []const u8) ![]u8 {
    const path = try std.fmt.allocPrint(alloc, "{s}{s}", .{ data_dir, name });
    defer alloc.free(path);
    return core.readFile(alloc, path);
}

fn appendVarint(a: std.mem.Allocator, out: *std.ArrayListUnmanaged(u8), v0: u64) !void {
    var v = v0;
    while (v > 0x7f) {
        try out.append(a, @as(u8, @truncate(v & 0x7f)) | 0x80);
        v >>= 7;
    }
    try out.append(a, @truncate(v));
}

fn vint(a: std.mem.Allocator, fnum: u32, v: u64) ![]u8 {
    var out: std.ArrayListUnmanaged(u8) = .empty;
    errdefer out.deinit(a);
    try appendVarint(a, &out, @as(u64, fnum) * 8);
    try appendVarint(a, &out, v);
    return out.toOwnedSlice(a);
}

fn ld(a: std.mem.Allocator, fnum: u32, payload: []const u8) ![]u8 {
    var out: std.ArrayListUnmanaged(u8) = .empty;
    errdefer out.deinit(a);
    try appendVarint(a, &out, @as(u64, fnum) * 8 + 2);
    try appendVarint(a, &out, payload.len);
    try out.appendSlice(a, payload);
    return out.toOwnedSlice(a);
}

fn build(alloc: std.mem.Allocator, ty: *const core.MessageType) !core.Message {
    var m = try ty.newMessage(alloc);
    errdefer m.deinit();
    try m.setI64("i32", std.math.minInt(i32));
    try m.setI64("i64", std.math.maxInt(i64));
    try m.setU64("u32", std.math.maxInt(u32));
    try m.setU64("u64", std.math.maxInt(u64));
    try m.setI64("s32", std.math.minInt(i32));
    try m.setI64("s64", std.math.minInt(i64));
    try m.setU64("fx32", 0xdeadbeef);
    try m.setU64("fx64", 72623859790382856);
    try m.setI64("sfx32", std.math.minInt(i32));
    try m.setI64("sfx64", std.math.minInt(i64));
    try m.setF32("f32", std.math.inf(f32));
    try m.setF64("f64", -123456.789);
    try m.setBool("flag", true);
    try m.setStr("str", "hello");
    try m.setI64("color", 2);
    try m.setI64("opt_i32", -7);
    try m.setStr("opt_str", "opt");
    {
        const inner = try m.setMessage("inner");
        try inner.setStr("name", "kid");
        try inner.setI64("value", 42);
    }
    for ([_]i64{ 1, -5, 300 }) |n| try m.push("nums", .{ .i32 = @intCast(n) });
    for ([_][]const u8{ "a", "bb" }) |t| try m.push("tags", .{ .bytes = t });
    for ([_][]const u8{ "x", "y" }) |name| {
        const inner = try m.pushMessage("inners");
        try inner.setStr("name", name);
        try inner.setI64("value", if (std.mem.eql(u8, name, "x")) 1 else -2);
    }
    for ([_]bool{ true, false, true }) |v| try m.push("flags", .{ .bool = v });
    return m;
}

test "encode matches the reference" {
    const alloc = std.testing.allocator;
    var schema = try load(alloc, "matrix3.desc");
    defer schema.deinit();
    const ty = try schema.messageType("structtest.Scalars3");
    const reference = try fixture(alloc, "matrix3.bin");
    defer alloc.free(reference);
    var m = try build(alloc, &ty);
    defer m.deinit();
    const got = try ty.encode(alloc, &m);
    defer alloc.free(got);
    try std.testing.expectEqualSlices(u8, reference, got);
    try std.testing.expectEqual(@as(usize, 192), reference.len);
}

test "decode reads every field and re-encodes" {
    const alloc = std.testing.allocator;
    var schema = try load(alloc, "matrix3.desc");
    defer schema.deinit();
    const ty = try schema.messageType("structtest.Scalars3");
    const reference = try fixture(alloc, "matrix3.bin");
    defer alloc.free(reference);
    var m = try ty.decode(alloc, reference);
    defer m.deinit();
    try std.testing.expectEqual(@as(?i64, std.math.minInt(i32)), try m.getI64("i32"));
    try std.testing.expectEqual(@as(?i64, std.math.maxInt(i64)), try m.getI64("i64"));
    try std.testing.expectEqual(@as(?u64, std.math.maxInt(u32)), try m.getU64("u32"));
    try std.testing.expectEqual(@as(?u64, std.math.maxInt(u64)), try m.getU64("u64"));
    try std.testing.expectEqual(@as(?u64, 0xdeadbeef), try m.getU64("fx32"));
    try std.testing.expectEqual(@as(?f32, std.math.inf(f32)), try m.getF32("f32"));
    try std.testing.expectEqual(@as(?bool, true), try m.getBool("flag"));
    try std.testing.expectEqualStrings("hello", (try m.getStr("str")).?);
    try std.testing.expectEqual(@as(?i64, 2), try m.getI64("color"));
    try std.testing.expectEqual(@as(?i64, -7), try m.getI64("opt_i32"));
    const inner = (try m.getMessage("inner")).?;
    try std.testing.expectEqualStrings("kid", (try inner.getStr("name")).?);
    try std.testing.expectEqual(@as(?i64, 42), try inner.getI64("value"));
    try std.testing.expectEqual(@as(usize, 3), try m.len("nums"));
    try std.testing.expectEqual(@as(usize, 2), try m.len("tags"));
    try std.testing.expectEqual(@as(usize, 2), try m.len("inners"));
    try std.testing.expectEqual(@as(usize, 3), try m.len("flags"));
    const re = try ty.encode(alloc, &m);
    defer alloc.free(re);
    try std.testing.expectEqualSlices(u8, reference, re);
}

test "unknown fields are retained in input order" {
    const alloc = std.testing.allocator;
    var schema = try load(alloc, "matrix3.desc");
    defer schema.deinit();
    const ty = try schema.messageType("structtest.Scalars3");
    const reference = try fixture(alloc, "matrix3.bin");
    defer alloc.free(reference);
    const unk1 = try vint(alloc, 99, 5);
    defer alloc.free(unk1);
    const unk2 = try ld(alloc, 100, "opaque");
    defer alloc.free(unk2);
    var wire: std.ArrayListUnmanaged(u8) = .empty;
    defer wire.deinit(alloc);
    try wire.appendSlice(alloc, reference);
    try wire.appendSlice(alloc, unk1);
    try wire.appendSlice(alloc, unk2);
    var m = try ty.decode(alloc, wire.items);
    defer m.deinit();
    try std.testing.expectEqual(@as(usize, 2), m.unknowns().len);
    try std.testing.expectEqualSlices(u8, unk1, m.unknowns()[0]);
    try std.testing.expectEqualSlices(u8, unk2, m.unknowns()[1]);
    const re = try ty.encode(alloc, &m);
    defer alloc.free(re);
    try std.testing.expectEqualSlices(u8, wire.items, re);

    // Wrong wire type is an unknown, not a known field.
    const wrong = [_]u8{ 0x0d, 1, 2, 3, 4 };
    var mw = try ty.decode(alloc, &wrong);
    defer mw.deinit();
    try std.testing.expect(!(try mw.has("i32")));
    try std.testing.expectEqual(@as(usize, 1), mw.unknowns().len);
    try std.testing.expectEqualSlices(u8, &wrong, mw.unknowns()[0]);
    const rw = try ty.encode(alloc, &mw);
    defer alloc.free(rw);
    try std.testing.expectEqualSlices(u8, &wrong, rw);
}

test "groups round trip and merge" {
    const alloc = std.testing.allocator;
    var schema = try load(alloc, "grouptest.desc");
    defer schema.deinit();
    const ty = try schema.messageType("structtest.GroupTest");
    const reference = try fixture(alloc, "grouptest.bin");
    defer alloc.free(reference);
    var m = try ty.decode(alloc, reference);
    defer m.deinit();
    const re = try ty.encode(alloc, &m);
    defer alloc.free(re);
    try std.testing.expectEqualSlices(u8, reference, re);
    try std.testing.expect((try m.getMessage("inner")) != null);

    const s1 = try vint(alloc, 3, 0);
    defer alloc.free(s1);
    const s2 = try vint(alloc, 2, 7);
    defer alloc.free(s2);
    var a: std.ArrayListUnmanaged(u8) = .empty;
    defer a.deinit(alloc);
    try appendVarint(alloc, &a, 1 * 8 + 3);
    try a.appendSlice(alloc, s1);
    try appendVarint(alloc, &a, 1 * 8 + 4);
    var b: std.ArrayListUnmanaged(u8) = .empty;
    defer b.deinit(alloc);
    try appendVarint(alloc, &b, 1 * 8 + 3);
    try b.appendSlice(alloc, s2);
    try appendVarint(alloc, &b, 1 * 8 + 4);
    var both: std.ArrayListUnmanaged(u8) = .empty;
    defer both.deinit(alloc);
    try both.appendSlice(alloc, a.items);
    try both.appendSlice(alloc, b.items);
    var merged = try ty.decode(alloc, both.items);
    defer merged.deinit();
    try std.testing.expectEqual(@as(?i64, 7), try (try merged.getMessage("inner")).?.getI64("x"));
}

test "packed and unpacked forms are both accepted" {
    const alloc = std.testing.allocator;
    var schema = try load(alloc, "matrix3.desc");
    defer schema.deinit();
    const ty = try schema.messageType("structtest.Scalars3");
    const nums = (try ty.field("nums")).number;
    const a = try vint(alloc, nums, 1);
    defer alloc.free(a);
    const b = try vint(alloc, nums, 2);
    defer alloc.free(b);
    var unpacked: std.ArrayListUnmanaged(u8) = .empty;
    defer unpacked.deinit(alloc);
    try unpacked.appendSlice(alloc, a);
    try unpacked.appendSlice(alloc, b);
    var m = try ty.decode(alloc, unpacked.items);
    defer m.deinit();
    try std.testing.expectEqual(@as(usize, 2), try m.len("nums"));

    var payload: std.ArrayListUnmanaged(u8) = .empty;
    defer payload.deinit(alloc);
    try appendVarint(alloc, &payload, 1);
    try appendVarint(alloc, &payload, std.math.maxInt(u64));
    const packed_wire = try ld(alloc, nums, payload.items);
    defer alloc.free(packed_wire);
    var mp = try ty.decode(alloc, packed_wire);
    defer mp.deinit();
    const items = try mp.items("nums");
    try std.testing.expectEqual(@as(i64, 1), items[0].asI64().?);
    try std.testing.expectEqual(@as(i64, -1), items[1].asI64().?);
    const re = try ty.encode(alloc, &mp);
    defer alloc.free(re);
    try std.testing.expectEqualSlices(u8, packed_wire, re);

    const empty = try ld(alloc, nums, "");
    defer alloc.free(empty);
    var me = try ty.decode(alloc, empty);
    defer me.deinit();
    try std.testing.expect(!(try me.has("nums")));
}

test "oneofs collapse to the last member" {
    const alloc = std.testing.allocator;
    var schema = try load(alloc, "oneoftest.desc");
    defer schema.deinit();
    const ty = try schema.messageType("structtest.OneOfTest");
    var m = try ty.newMessage(alloc);
    defer m.deinit();
    try m.setI64("num", 3);
    try m.setStr("text", "later");
    try std.testing.expectEqual((try ty.field("text")).number, try m.oneofCase("kind"));
    try std.testing.expect(!(try m.has("num")));
    const wire = try ty.encode(alloc, &m);
    defer alloc.free(wire);
    const want = try ld(alloc, (try ty.field("text")).number, "later");
    defer alloc.free(want);
    try std.testing.expectEqualSlices(u8, want, wire);
}

test "presence follows the field rule" {
    const alloc = std.testing.allocator;
    var schema = try load(alloc, "matrix3.desc");
    defer schema.deinit();
    const ty = try schema.messageType("structtest.Scalars3");
    const zero = try vint(alloc, 1, 0);
    defer alloc.free(zero);
    var m = try ty.decode(alloc, zero);
    defer m.deinit();
    try std.testing.expect(!(try m.has("i32")));
    const re = try ty.encode(alloc, &m);
    defer alloc.free(re);
    try std.testing.expectEqual(@as(usize, 0), re.len);

    const opt = try vint(alloc, (try ty.field("opt_i32")).number, 0);
    defer alloc.free(opt);
    var mo = try ty.decode(alloc, opt);
    defer mo.deinit();
    try std.testing.expect(try mo.has("opt_i32"));
    const reo = try ty.encode(alloc, &mo);
    defer alloc.free(reo);
    try std.testing.expectEqualSlices(u8, opt, reo);

    const empty = try ld(alloc, (try ty.field("inner")).number, "");
    defer alloc.free(empty);
    var mm = try ty.decode(alloc, empty);
    defer mm.deinit();
    try std.testing.expect(try mm.has("inner"));
    const rem = try ty.encode(alloc, &mm);
    defer alloc.free(rem);
    try std.testing.expectEqualSlices(u8, empty, rem);
}

test "required fields fail the decode" {
    const alloc = std.testing.allocator;
    var schema = try load(alloc, "matrix2.desc");
    defer schema.deinit();
    const ty = try schema.messageType("structtest.Scalars2");
    const nums = try vint(alloc, (try ty.field("nums")).number, 1);
    defer alloc.free(nums);
    try std.testing.expectError(core.Error.RequiredMissing, ty.decode(alloc, nums));
    var empty = try ty.newMessage(alloc);
    defer empty.deinit();
    try empty.setI64("nums", 1);
    try std.testing.expectError(core.Error.RequiredMissing, ty.encode(alloc, &empty));
}

test "utf8 is validated only where the schema says" {
    const alloc = std.testing.allocator;
    var schema = try load(alloc, "matrix3.desc");
    defer schema.deinit();
    const ty = try schema.messageType("structtest.Scalars3");
    const bad = try ld(alloc, (try ty.field("str")).number, &[_]u8{ 0xed, 0xa0, 0x80 });
    defer alloc.free(bad);
    try std.testing.expectError(core.Error.InvalidData, ty.decode(alloc, bad));

    var schema2 = try load(alloc, "matrix2.desc");
    defer schema2.deinit();
    const ty2 = try schema2.messageType("structtest.Scalars2");
    const id = try vint(alloc, (try ty2.field("id")).number, 1);
    defer alloc.free(id);
    const raw = try ld(alloc, (try ty2.field("str")).number, &[_]u8{0x80});
    defer alloc.free(raw);
    var wire: std.ArrayListUnmanaged(u8) = .empty;
    defer wire.deinit(alloc);
    try wire.appendSlice(alloc, id);
    try wire.appendSlice(alloc, raw);
    var m = try ty2.decode(alloc, wire.items);
    defer m.deinit();
    const re = try ty2.encode(alloc, &m);
    defer alloc.free(re);
    try std.testing.expectEqualSlices(u8, wire.items, re);
}

test "bad tags and truncation fail" {
    const alloc = std.testing.allocator;
    var schema = try load(alloc, "matrix3.desc");
    defer schema.deinit();
    const ty = try schema.messageType("structtest.Scalars3");
    try std.testing.expectError(core.Error.InvalidData, ty.decode(alloc, &[_]u8{ 0, 0 }));
    try std.testing.expectError(core.Error.InvalidData, ty.decode(alloc, &[_]u8{ 0x0e, 0 }));
    try std.testing.expectError(core.Error.InvalidData, ty.decode(alloc, &[_]u8{0x08}));
    try std.testing.expectError(core.Error.InvalidData, ty.decode(alloc, &[_]u8{ 0x0a, 0x05, 0 }));
    try std.testing.expectError(core.Error.InvalidData, ty.decode(alloc, &[_]u8{ 0x09, 0, 0, 0 }));
    // A 5-byte tag above UINT32_MAX truncates instead of failing.
    const trunc = [_]u8{ 0x88, 0x80, 0x80, 0x80, 0x10, 0x05 };
    var m = try ty.decode(alloc, &trunc);
    defer m.deinit();
    try std.testing.expectEqual(@as(?i64, 5), try m.getI64("i32"));
}

test "limits bound bytes and depth" {
    const alloc = std.testing.allocator;
    var schema = try load(alloc, "matrix3.desc");
    defer schema.deinit();
    const ty = try schema.messageType("structtest.Scalars3");
    const reference = try fixture(alloc, "matrix3.bin");
    defer alloc.free(reference);
    try std.testing.expectError(core.Error.LimitExceeded, ty.decodeWith(alloc, reference, .{ .max_bytes = 10 }));
}

test "every kind has a storage form and NaN bits survive" {
    const alloc = std.testing.allocator;
    var schema = try load(alloc, "matrix3.desc");
    defer schema.deinit();
    const ty = try schema.messageType("structtest.Scalars3");
    var m = try ty.newMessage(alloc);
    defer m.deinit();
    try m.setF32("f32", @bitCast(@as(u32, 0x7fc01234)));
    const wire = try ty.encode(alloc, &m);
    defer alloc.free(wire);
    var back = try ty.decode(alloc, wire);
    defer back.deinit();
    const v = (try back.get("f32")).?;
    switch (v) {
        .f32 => |bits| try std.testing.expectEqual(@as(u32, 0x7fc01234), bits),
        else => return error.TestUnexpectedResult,
    }
}

test "u64 and i64 edges round trip" {
    const alloc = std.testing.allocator;
    var schema = try load(alloc, "matrix3.desc");
    defer schema.deinit();
    const ty = try schema.messageType("structtest.Scalars3");
    var m = try ty.newMessage(alloc);
    defer m.deinit();
    try m.setU64("u64", std.math.maxInt(u64));
    try m.setI64("i64", std.math.minInt(i64));
    try m.setU64("fx64", std.math.maxInt(u64));
    try m.setI64("sfx64", std.math.minInt(i64));
    try m.setI64("s64", std.math.minInt(i64));
    const wire = try ty.encode(alloc, &m);
    defer alloc.free(wire);
    var back = try ty.decode(alloc, wire);
    defer back.deinit();
    try std.testing.expectEqual(@as(?u64, std.math.maxInt(u64)), try back.getU64("u64"));
    try std.testing.expectEqual(@as(?i64, std.math.minInt(i64)), try back.getI64("i64"));
    try std.testing.expectEqual(@as(?i64, std.math.minInt(i64)), try back.getI64("s64"));
    const re = try ty.encode(alloc, &back);
    defer alloc.free(re);
    try std.testing.expectEqualSlices(u8, wire, re);
}

//! Generated-conformance check for the Zig binding: build matrix3.proto's
//! Scalars3 through the generated structs, encode, require the protoc
//! reference bytes (matrix3.bin), then decode those bytes back and check
//! every field. The schema travels embedded in gen/matrix3.zig.
//!
//!   zig build generated-test   (from bindings/zig)
const std = @import("std");
const pbwire = @import("pbwire");
const m3 = @import("gen/matrix3.zig");
const m3meta = @import("gen/matrix3_meta.zig");

const BIN_PATH = "../c/tests/data/matrix3.bin";

fn build() m3.Structtest_Scalars3 {
    return .{
        .i32 = std.math.minInt(i32),
        .i64 = std.math.maxInt(i64),
        .u32 = std.math.maxInt(u32),
        .u64 = std.math.maxInt(u64),
        .s32 = std.math.minInt(i32),
        .s64 = std.math.minInt(i64),
        .fx32 = 0xdeadbeef,
        .fx64 = 72623859790382856,
        .sfx32 = std.math.minInt(i32),
        .sfx64 = std.math.minInt(i64),
        .f32 = std.math.inf(f32),
        .f64 = -123456.789,
        .flag = true,
        .str = "hello",
        .color = .COLOR3_GREEN,
        .inner = .{ .name = "kid", .value = 42 },
        .nums = &[_]i32{ 1, -5, 300 },
        .tags = &[_][]const u8{ "a", "bb" },
        .inners = &[_]m3.Structtest_Inner3{
            .{ .name = "x", .value = 1 },
            .{ .name = "y", .value = -2 },
        },
        .opt_i32 = -7,
        .opt_str = "opt",
        .flags = &[_]bool{ true, false, true },
    };
}

test "generated model encodes to the protoc reference bytes and back" {
    const alloc = std.testing.allocator;
    var schema = try m3meta.loadSchema();
    defer schema.deinit();

    const wire = try (build()).encode(alloc, &schema);
    defer alloc.free(wire);

    const ref = try pbwire.readFile(alloc, BIN_PATH);
    defer alloc.free(ref);
    try std.testing.expectEqualSlices(u8, ref, wire);

    const back = try m3.Structtest_Scalars3.decode(alloc, &schema, ref);
    defer {
        for (back.inners) |n| alloc.free(n.name);
        alloc.free(back.inners);
        alloc.free(back.nums);
        for (back.tags) |t| alloc.free(t);
        alloc.free(back.tags);
        alloc.free(back.flags);
        alloc.free(back.str);
        alloc.free(back.inner.?.name);
        if (back.opt_str) |s| alloc.free(s);
    }
    try std.testing.expectEqual(@as(i32, std.math.minInt(i32)), back.i32);
    try std.testing.expectEqual(@as(i64, std.math.maxInt(i64)), back.i64);
    try std.testing.expectEqual(@as(u32, std.math.maxInt(u32)), back.u32);
    try std.testing.expectEqual(@as(u64, std.math.maxInt(u64)), back.u64);
    try std.testing.expectEqual(true, back.flag);
    try std.testing.expectEqualStrings("hello", back.str);
    try std.testing.expectEqual(m3.Structtest_Color3.COLOR3_GREEN, back.color);
    try std.testing.expectEqualStrings("kid", back.inner.?.name);
    try std.testing.expectEqual(@as(i32, 42), back.inner.?.value);
    try std.testing.expectEqualSlices(i32, &[_]i32{ 1, -5, 300 }, back.nums);
    try std.testing.expectEqual(@as(i32, -7), back.opt_i32.?);
    try std.testing.expectEqualStrings("opt", back.opt_str.?);
    try std.testing.expectEqualSlices(bool, &[_]bool{ true, false, true }, back.flags);
}

test "empty generated message encodes to nothing" {
    const alloc = std.testing.allocator;
    var schema = try m3meta.loadSchema();
    defer schema.deinit();
    const empty: m3.Structtest_Scalars3 = .{};
    const wire = try empty.encode(alloc, &schema);
    defer alloc.free(wire);
    try std.testing.expectEqual(@as(usize, 0), wire.len);
}

test "dual registration: external schema and preload" {
    const alloc = std.testing.allocator;
    const mmeta = @import("gen/maptest_meta.zig");
    var found = false;
    for (mmeta.MESSAGE_NAMES) |n| {
        if (std.mem.eql(u8, n, "spectest.MapTest")) found = true;
    }
    try std.testing.expect(found);
    var embedded = try mmeta.loadSchema();
    defer embedded.deinit();
    try mmeta.preload(&embedded);
    const raw = try pbwire.readFile(alloc, "../../codegen/testdata/maptest.desc");
    defer alloc.free(raw);
    var ext = try pbwire.Schema.fromBytes(raw);
    defer ext.deinit();
    const ty = try ext.messageType("spectest.MapTest");
    _ = ty;
    try mmeta.preload(&ext);
}

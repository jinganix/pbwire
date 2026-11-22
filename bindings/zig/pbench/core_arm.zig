//! The `core` arm of the Zig pbench: the pure-Zig wire core
//! (`bindings/zig/core/`) as the codec, with no C library anywhere in the
//! path.
//!
//! Same user-visible task as the other two arms: the payload is a native object
//! (here the core's own `Message` tree, built through the same per-field `set`
//! calls the `tree` arm makes), a prebuilt tree in and wire bytes out, wire
//! bytes in and a fully readable tree out. Build time is outside the timer,
//! exactly like `pbwire-zig-tree`'s prebuilt handle, so the rows differ only in
//! who walks the wire.

const std = @import("std");
const core = @import("pbwire_core");

pub const CoreArm = struct {
    schema: core.Schema,
    ty: core.MessageType,

    pub fn init(alloc: std.mem.Allocator, descriptor_set: []const u8) !CoreArm {
        var schema = try core.Schema.fromFile(alloc, descriptor_set);
        errdefer schema.deinit();
        const ty = try schema.messageType("structtest.Scalars3");
        return .{ .schema = schema, .ty = ty };
    }

    pub fn deinit(self: *CoreArm) void {
        self.schema.deinit();
    }

    /// The `matrix3` fixture through the core tree: every scalar kind at its
    /// edge, the nested message, the two proto3 optionals and the repeated
    /// fields scaled by `n`.
    pub fn build(self: *const CoreArm, alloc: std.mem.Allocator, n: usize) !core.Message {
        var m = try self.ty.newMessage(alloc);
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
        var i: usize = 0;
        while (i < n) : (i += 1) {
            try m.push("nums", .{ .i32 = @as(i32, @intCast(i)) - 3 });
            try m.push("tags", .{ .bytes = "tag" });
            try m.push("flags", .{ .bool = i % 2 == 0 });
            const kid = try m.pushMessage("inners");
            try kid.setStr("name", "grandkid");
            try kid.setI64("value", @as(i64, @intCast(i)) + 1);
        }
        return m;
    }

    /// One encode into a reused buffer: the hot-loop shape every arm uses.
    pub fn encodeTo(self: *const CoreArm, m: *const core.Message, buf: *std.ArrayListUnmanaged(u8), alloc: std.mem.Allocator) !void {
        return self.ty.encodeTo(m, buf, alloc);
    }

    pub fn encode(self: *const CoreArm, alloc: std.mem.Allocator, m: *const core.Message) ![]u8 {
        return self.ty.encode(alloc, m);
    }

    /// One-shot decode, like the other arms' decode rows.
    pub fn decode(self: *const CoreArm, alloc: std.mem.Allocator, wire: []const u8) !core.Message {
        return self.ty.decode(alloc, wire);
    }

    /// The gate: byte equality with the official runtime's reference image, and
    /// an oracle read of the decoded tree. Runs before any timing.
    pub fn gate(self: *const CoreArm, alloc: std.mem.Allocator, reference: []const u8, n: usize) !void {
        var message = try self.build(alloc, n);
        defer message.deinit();
        const bytes = try self.encode(alloc, &message);
        defer alloc.free(bytes);
        if (!std.mem.eql(u8, bytes, reference)) return error.CoreBytesDiffer;
        var back = try self.decode(alloc, reference);
        defer back.deinit();
        const re = try self.encode(alloc, &back);
        defer alloc.free(re);
        if (!std.mem.eql(u8, re, reference)) return error.CoreReencodeDiffers;
        if (!std.mem.eql(u8, (try back.getStr("str")) orelse "", "hello")) return error.CoreStr;
        if ((try back.getU64("u64")) != std.math.maxInt(u64)) return error.CoreU64;
        if ((try back.getI64("opt_i32")) != -7) return error.CoreOptI32;
        if (!(try back.has("opt_str"))) return error.CoreOptStr;
        if (!std.mem.eql(u8, (try (try back.getMessage("inner")).?.getStr("name")) orelse "", "kid")) return error.CoreInner;
        if ((try back.len("nums")) != n) return error.CoreNums;
        if (n > 0) {
            const items = try back.items("nums");
            if (items[n - 1].asI64() != @as(i64, @intCast(n)) - 4) return error.CoreLastNum;
        }
        if ((try back.len("inners")) != n) return error.CoreInners;
        if (n > 0) {
            const items = try back.items("inners");
            const last = items[n - 1].asMessage() orelse return error.CoreLastInner;
            if ((try last.getI64("value")) != @as(i64, @intCast(n))) return error.CoreLastValue;
            if (!std.mem.eql(u8, (try last.getStr("name")) orelse "", "grandkid")) return error.CoreLastName;
        }
        if ((try back.len("flags")) != n) return error.CoreFlags;
    }
};

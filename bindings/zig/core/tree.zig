//! The dynamic message tree: one slot per declared field, plus the oneof cases
//! and the retained unknown fields.
//!
//! Scalars live in their wire-neutral form (`i32`/`u32`/`i64`/`u64` for every
//! integer width; raw bit patterns for `float`/`double` so a NaN payload
//! survives a round trip; `bool`; bytes for both `string` and `bytes`) and
//! messages live nested. Every allocation belongs to one arena per root
//! message, so a failed decode frees the whole partial tree at once and a
//! decoded tree owns no slice of its input.
//!
//! This is the Zig twin of `bindings/rust/src/core/tree.rs`.

const std = @import("std");
const descriptor = @import("descriptor.zig");
const wire = @import("wire.zig");

const Allocator = std.mem.Allocator;
const CoreSchema = descriptor.CoreSchema;
const Field = descriptor.Field;
const Kind = descriptor.Kind;
const Error = wire.Error;

pub const Value = union(enum) {
    bool: bool,
    i32: i32,
    u32: u32,
    i64: i64,
    u64: u64,
    /// float32 bits, kept raw so a NaN payload survives.
    f32: u32,
    /// float64 bits, kept raw so a NaN payload survives.
    f64: u64,
    /// A `string` or `bytes` payload, exactly as parsed (or as set).
    bytes: []const u8,
    msg: *Message,
    list: std.ArrayListUnmanaged(Value),

    pub fn asI64(self: Value) ?i64 {
        return switch (self) {
            .i32 => |v| v,
            .u32 => |v| @intCast(v),
            .i64 => |v| v,
            .u64 => |v| @bitCast(v),
            .bool => |v| @intFromBool(v),
            .f32 => |bits| @as(i32, @bitCast(bits)),
            .f64 => |bits| @bitCast(bits),
            else => null,
        };
    }

    pub fn asU64(self: Value) ?u64 {
        return switch (self) {
            .i32 => |v| @bitCast(@as(i64, v)),
            .u32 => |v| v,
            .i64 => |v| @bitCast(v),
            .u64 => |v| v,
            .bool => |v| @intFromBool(v),
            .f32 => |bits| bits,
            .f64 => |bits| bits,
            else => null,
        };
    }

    pub fn asBool(self: Value) ?bool {
        return switch (self) {
            .bool => |v| v,
            .i32 => |v| v != 0,
            .u32 => |v| v != 0,
            .i64 => |v| v != 0,
            .u64 => |v| v != 0,
            else => null,
        };
    }

    pub fn asBytes(self: Value) ?[]const u8 {
        return switch (self) {
            .bytes => |b| b,
            else => null,
        };
    }

    pub fn asMessage(self: Value) ?*Message {
        return switch (self) {
            .msg => |m| m,
            else => null,
        };
    }

    pub fn asList(self: Value) ?[]const Value {
        return switch (self) {
            .list => |items| items.items,
            else => null,
        };
    }
};

/// One message value: per-slot values, the oneof cases and the unknowns.
pub const Message = struct {
    core: *const CoreSchema,
    def: usize,
    values: []?Value,
    /// Slot+1 per oneof, 0 = none set; empty when the message has none.
    oneof_case: []u32,
    /// Retained unknown fields in input order (tag and payload verbatim).
    unknown: std.ArrayListUnmanaged([]const u8) = .empty,
    a: Allocator,
    root_arena: ?*std.heap.ArenaAllocator = null,
    owner: Allocator = undefined,

    /// A message whose storage is `a` (the root's arena, for a nested message).
    pub fn newIn(core: *const CoreSchema, def: usize, a: Allocator) !Message {
        const d = &core.messages[def];
        const values = try a.alloc(?Value, d.fields.len);
        @memset(values, null);
        var cases: []u32 = &[_]u32{};
        if (d.oneof_count > 0) {
            cases = try a.alloc(u32, d.oneof_count);
            @memset(cases, 0);
        }
        return .{ .core = core, .def = def, .values = values, .oneof_case = cases, .a = a };
    }

    /// A root message (the result of `newMessage`/`decode`). Its arena owns the
    /// whole tree; nested messages borrow it and their `deinit` is a no-op.
    pub fn newRoot(owner: Allocator, core: *const CoreSchema, def: usize) !Message {
        const ar = try owner.create(std.heap.ArenaAllocator);
        ar.* = std.heap.ArenaAllocator.init(owner);
        errdefer {
            ar.deinit();
            owner.destroy(ar);
        }
        var m = try newIn(core, def, ar.allocator());
        m.root_arena = ar;
        m.owner = owner;
        return m;
    }

    pub fn deinit(self: *Message) void {
        if (self.root_arena) |ar| {
            const owner = self.owner;
            ar.deinit();
            owner.destroy(ar);
            self.root_arena = null;
        }
    }

    pub fn fullName(self: *const Message) []const u8 {
        return self.core.messages[self.def].full_name;
    }

    pub fn defn(self: *const Message) *const descriptor.MsgDef {
        return &self.core.messages[self.def];
    }

    pub fn fieldSlot(self: *const Message, name: []const u8) !usize {
        return self.defn().by_name.get(name) orelse Error.NotFound;
    }

    pub fn field(self: *const Message, name: []const u8) !*const Field {
        return &self.defn().fields[try self.fieldSlot(name)];
    }

    pub fn has(self: *const Message, name: []const u8) !bool {
        return self.values[try self.fieldSlot(name)] != null;
    }

    pub fn get(self: *const Message, name: []const u8) !?Value {
        return self.values[try self.fieldSlot(name)];
    }

    pub fn oneofCase(self: *const Message, oneof_name: []const u8) !u32 {
        const idx = self.defn().oneof_by_name.get(oneof_name) orelse Error.NotFound;
        if (idx >= self.oneof_case.len) return 0;
        const c = self.oneof_case[idx];
        return if (c == 0) 0 else self.defn().fields[c - 1].number;
    }

    pub fn setSlot(self: *Message, slot: usize, value: Value) void {
        const oneof = self.defn().fields[slot].oneof;
        if (oneof >= 0 and self.oneof_case.len > 0) {
            const prev = self.oneof_case[@intCast(oneof)];
            if (prev != 0 and prev != slot + 1) self.values[prev - 1] = null;
            self.oneof_case[@intCast(oneof)] = @intCast(slot + 1);
        }
        self.values[slot] = value;
    }

    pub fn set(self: *Message, name: []const u8, value: Value) !void {
        const slot = try self.fieldSlot(name);
        const f = &self.defn().fields[slot];
        self.setSlot(slot, try coerce(self.a, f, value));
    }

    pub fn push(self: *Message, name: []const u8, value: Value) !void {
        const slot = try self.fieldSlot(name);
        const f = &self.defn().fields[slot];
        const v = try coerce(self.a, f, value);
        if (self.values[slot]) |*existing| {
            if (existing.* == .list) {
                try existing.list.append(self.a, v);
                return;
            }
        }
        var list: std.ArrayListUnmanaged(Value) = .empty;
        try list.append(self.a, v);
        self.setSlot(slot, .{ .list = list });
    }

    pub fn setI64(self: *Message, name: []const u8, v: i64) !void {
        try self.set(name, .{ .i64 = v });
    }

    pub fn setU64(self: *Message, name: []const u8, v: u64) !void {
        try self.set(name, .{ .u64 = v });
    }

    pub fn setBool(self: *Message, name: []const u8, v: bool) !void {
        try self.set(name, .{ .bool = v });
    }

    pub fn setF64(self: *Message, name: []const u8, v: f64) !void {
        try self.set(name, .{ .f64 = @bitCast(v) });
    }

    pub fn setF32(self: *Message, name: []const u8, v: f32) !void {
        try self.set(name, .{ .f32 = @bitCast(v) });
    }

    pub fn setStr(self: *Message, name: []const u8, v: []const u8) !void {
        try self.set(name, .{ .bytes = v });
    }

    pub fn setBytes(self: *Message, name: []const u8, v: []const u8) !void {
        try self.set(name, .{ .bytes = v });
    }

    /// Replaces (or creates) a sub-message field and returns it for filling.
    pub fn setMessage(self: *Message, name: []const u8) !*Message {
        const slot = try self.fieldSlot(name);
        const msg_type = self.defn().fields[slot].msg_type orelse return Error.WrongType;
        const sub = try self.a.create(Message);
        sub.* = try Message.newIn(self.core, msg_type, self.a);
        self.setSlot(slot, .{ .msg = sub });
        return sub;
    }

    /// Creates a new element of a repeated message field and returns it.
    pub fn pushMessage(self: *Message, name: []const u8) !*Message {
        const slot = try self.fieldSlot(name);
        const msg_type = self.defn().fields[slot].msg_type orelse return Error.WrongType;
        const sub = try self.a.create(Message);
        sub.* = try Message.newIn(self.core, msg_type, self.a);
        if (self.values[slot]) |*existing| {
            if (existing.* == .list) {
                try existing.list.append(self.a, .{ .msg = sub });
                return sub;
            }
        }
        var list: std.ArrayListUnmanaged(Value) = .empty;
        try list.append(self.a, .{ .msg = sub });
        self.setSlot(slot, .{ .list = list });
        return sub;
    }

    pub fn getI64(self: *const Message, name: []const u8) !?i64 {
        const v = (try self.get(name)) orelse return null;
        return v.asI64();
    }

    pub fn getU64(self: *const Message, name: []const u8) !?u64 {
        const v = (try self.get(name)) orelse return null;
        return v.asU64();
    }

    pub fn getBool(self: *const Message, name: []const u8) !?bool {
        const v = (try self.get(name)) orelse return null;
        return v.asBool();
    }

    pub fn getF64(self: *const Message, name: []const u8) !?f64 {
        const v = (try self.get(name)) orelse return null;
        return switch (v) {
            .f64 => |bits| @bitCast(bits),
            .f32 => |bits| @as(f64, @floatCast(@as(f32, @bitCast(bits)))),
            else => null,
        };
    }

    pub fn getF32(self: *const Message, name: []const u8) !?f32 {
        const v = (try self.get(name)) orelse return null;
        return switch (v) {
            .f32 => |bits| @bitCast(bits),
            .f64 => |bits| @floatCast(@as(f64, @bitCast(bits))),
            else => null,
        };
    }

    pub fn getBytes(self: *const Message, name: []const u8) !?[]const u8 {
        const v = (try self.get(name)) orelse return null;
        return v.asBytes();
    }

    pub fn getStr(self: *const Message, name: []const u8) !?[]const u8 {
        return self.getBytes(name);
    }

    pub fn getMessage(self: *const Message, name: []const u8) !?*Message {
        const v = (try self.get(name)) orelse return null;
        return v.asMessage();
    }

    pub fn len(self: *const Message, name: []const u8) !usize {
        const v = (try self.get(name)) orelse return 0;
        return if (v.asList()) |list| list.len else 0;
    }

    pub fn is_empty(self: *const Message, name: []const u8) !bool {
        return (try self.len(name)) == 0;
    }

    pub fn items(self: *const Message, name: []const u8) ![]const Value {
        const v = (try self.get(name)) orelse return &[_]Value{};
        return v.asList() orelse &[_]Value{};
    }

    pub fn unknowns(self: *const Message) []const []const u8 {
        return self.unknown.items;
    }
};

/// One public value in the storage form of its field (see `Kind`).
fn coerce(a: Allocator, f: *const Field, v: Value) !Value {
    return switch (f.kind) {
        .bool => .{ .bool = switch (v) {
            .bool => |b| b,
            else => (v.asI64() orelse 0) != 0,
        } },
        .int32, .sint32, .sfixed32, .enumeration => .{ .i32 = switch (v) {
            .i32 => |x| x,
            .u32 => |x| @bitCast(x),
            .i64 => |x| @truncate(x),
            .u64 => |x| @bitCast(@as(u32, @truncate(x))),
            .bool => |x| @intFromBool(x),
            .f32 => |x| @bitCast(x),
            .f64 => |x| @bitCast(@as(u32, @truncate(x))),
            else => 0,
        } },
        .uint32, .fixed32 => .{ .u32 = switch (v) {
            .i32 => |x| @bitCast(x),
            .u32 => |x| x,
            .i64 => |x| @truncate(@as(u64, @bitCast(x))),
            .u64 => |x| @truncate(x),
            .bool => |x| @intFromBool(x),
            .f32 => |x| x,
            .f64 => |x| @truncate(x),
            else => 0,
        } },
        .int64, .sint64, .sfixed64 => .{ .i64 = switch (v) {
            .i32 => |x| x,
            .u32 => |x| x,
            .i64 => |x| x,
            .u64 => |x| @bitCast(x),
            .bool => |x| @intFromBool(x),
            .f32 => |x| @as(i32, @bitCast(x)),
            .f64 => |x| @bitCast(x),
            else => 0,
        } },
        .uint64, .fixed64 => .{ .u64 = switch (v) {
            .i32 => |x| @bitCast(@as(i64, x)),
            .u32 => |x| x,
            .i64 => |x| @bitCast(x),
            .u64 => |x| x,
            .bool => |x| @intFromBool(x),
            .f32 => |x| x,
            .f64 => |x| x,
            else => 0,
        } },
        .float => .{ .f32 = switch (v) {
            .f32 => |x| x,
            .f64 => |x| @bitCast(@as(f32, @floatCast(@as(f64, @bitCast(x))))),
            else => @bitCast(@as(f32, @floatFromInt(v.asI64() orelse 0))),
        } },
        .double => .{ .f64 = switch (v) {
            .f64 => |x| x,
            .f32 => |x| @bitCast(@as(f64, @floatCast(@as(f32, @bitCast(x))))),
            else => @bitCast(@as(f64, @floatFromInt(v.asI64() orelse 0))),
        } },
        .string, .bytes => switch (v) {
            .bytes => |b| .{ .bytes = try a.dupe(u8, b) },
            else => .{ .bytes = try a.dupe(u8, &.{}) },
        },
        .message, .group => v,
    };
}

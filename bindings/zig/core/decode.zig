//! Decoder: strict tags and lengths, unknown field numbers AND wrong-wire-type
//! occurrences retained as raw tag+payload bytes in input order, groups decoded
//! to their matching end tag, packed runs accepted for every numeric kind (and
//! the unpacked element form for a packed field), oneofs collapsed to the last
//! member, presence following the field's own zero, proto2 required fields
//! checked over the whole tree, and the nesting cap enforced on every recursive
//! path. The Zig twin of `bindings/rust/src/core/decode.rs`.

const std = @import("std");
const descriptor = @import("descriptor.zig");
const tree = @import("tree.zig");
const wire = @import("wire.zig");

const Allocator = std.mem.Allocator;
const CoreSchema = descriptor.CoreSchema;
const Field = descriptor.Field;
const Kind = descriptor.Kind;
const Limits = descriptor.Limits;
const Presence = descriptor.Presence;
const Message = tree.Message;
const Value = tree.Value;
const Reader = wire.Reader;
const Error = wire.Error;

pub fn decode(alloc: Allocator, core: *const CoreSchema, def: usize, data: []const u8, limits: Limits) !Message {
    try limits.checkBytes(data.len, "message");
    var m = try Message.newRoot(alloc, core, def);
    errdefer m.deinit();
    var r = Reader.init(data);
    r.cap = limits.depth();
    try decodeInto(&m, &r, data.len, -1, 0);
    if (m.defn().deep_required) try checkRequired(&m, r.cap, 0);
    return m;
}

pub fn decodeSlice(alloc: Allocator, core: *const CoreSchema, def: usize, data: []const u8, off: usize, length: usize, limits: Limits) !Message {
    try limits.checkBytes(length, "message");
    const end = std.math.add(usize, off, length) catch return Error.BadSlice;
    var m = try Message.newRoot(alloc, core, def);
    errdefer m.deinit();
    var r = try Reader.sliceRange(data, off, end);
    r.cap = limits.depth();
    try decodeInto(&m, &r, end, -1, 0);
    if (m.defn().deep_required) try checkRequired(&m, r.cap, 0);
    return m;
}

fn wtIncompatible(f: *const Field, wt: u8) bool {
    if (f.kind == .group) return wt != wire.WT_START_GROUP;
    if (f.repeated and f.kind.isPackable()) {
        if (wt == wire.WT_LEN) return false;
        if (f.is_packed) return wt != f.elem_wt;
    }
    return wt != f.wt;
}

fn listMut(m: *Message, slot: usize) !*std.ArrayListUnmanaged(Value) {
    if (m.values[slot] == null) m.values[slot] = .{ .list = .empty };
    if (m.values[slot]) |*v| {
        switch (v.*) {
            .list => |*items| return items,
            else => unreachable,
        }
    }
    unreachable;
}

fn newSub(m: *Message, def: usize) !*Message {
    const sub = try m.a.create(Message);
    sub.* = try Message.newIn(m.core, def, m.a);
    return sub;
}

fn canMerge(m: *Message, f: *const Field, slot: usize) bool {
    if (m.values[slot]) |v| {
        if (v != .msg) return false;
    } else return false;
    if (f.oneof < 0 or m.oneof_case.len == 0) return true;
    return m.oneof_case[@intCast(f.oneof)] == slot + 1;
}

pub fn decodeInto(m: *Message, r: *Reader, end: usize, group_end: i64, depth: u32) !void {
    if (depth > r.cap) return Error.LimitExceeded;
    const outer_end = r.end;
    r.end = end;
    defer r.end = outer_end;
    var closed = group_end < 0;
    while (r.pos < r.end) {
        const field_start = r.pos;
        const tag = try r.rTag();
        const fnum = tag >> 3;
        const wt: u8 = @truncate(tag & 7);
        if (fnum == 0 or wt > wire.WT_FIXED32) return Error.InvalidData;
        if (wt == wire.WT_END_GROUP) {
            if (group_end < 0 or @as(i64, fnum) != group_end) return Error.InvalidData;
            closed = true;
            break;
        }
        const slot = m.core.slotOf(m.def, fnum);
        if (slot < 0 or wtIncompatible(&m.core.messages[m.def].fields[@intCast(slot)], wt)) {
            try r.skipUnknown(fnum, wt, depth);
            const raw = try m.a.dupe(u8, r.b[field_start..r.pos]);
            try m.unknown.append(m.a, raw);
            continue;
        }
        try readKnown(m, r, @intCast(slot), wt, depth);
    }
    if (!closed) return Error.InvalidData;
}

fn readKnown(m: *Message, r: *Reader, slot: usize, wt: u8, depth: u32) !void {
    const f = &m.core.messages[m.def].fields[slot];
    const kind = f.kind;
    if (f.repeated) {
        if (kind.isPackable()) {
            const items = try listMut(m, slot);
            if (wt == wire.WT_LEN) {
                try readPacked(r, kind, items, m.a);
                if (items.items.len == 0) m.values[slot] = null;
                return;
            }
            try items.append(m.a, try readNumSingle(r, kind));
            return;
        }
        if (kind == .message) {
            const length = try r.rLen32();
            const sub_end = r.pos + length;
            const sub = try newSub(m, f.msg_type orelse return Error.InvalidData);
            try decodeInto(sub, r, sub_end, -1, depth + 1);
            try (try listMut(m, slot)).append(m.a, .{ .msg = sub });
            return;
        }
        if (kind == .group) {
            const sub = try newSub(m, f.msg_type orelse return Error.InvalidData);
            try decodeInto(sub, r, r.end, @intCast(f.number), depth + 1);
            try (try listMut(m, slot)).append(m.a, .{ .msg = sub });
            return;
        }
        const length = try r.rLen32();
        if (f.utf8 and !wire.utf8Valid(r.b[r.pos .. r.pos + length])) return Error.InvalidData;
        const payload = try m.a.dupe(u8, r.b[r.pos .. r.pos + length]);
        r.pos += length;
        try (try listMut(m, slot)).append(m.a, .{ .bytes = payload });
        return;
    }
    if (kind.isPackable()) {
        const value = try readNumSingle(r, kind);
        if (f.presence == .implicit and isZero(value)) return;
        m.setSlot(slot, value);
        return;
    }
    if (kind == .message) {
        const length = try r.rLen32();
        const sub_end = r.pos + length;
        if (canMerge(m, f, slot)) {
            const sub = m.values[slot].?.msg;
            try decodeInto(sub, r, sub_end, -1, depth + 1);
            return;
        }
        const sub = try newSub(m, f.msg_type orelse return Error.InvalidData);
        try decodeInto(sub, r, sub_end, -1, depth + 1);
        m.setSlot(slot, .{ .msg = sub });
        return;
    }
    if (kind == .group) {
        if (canMerge(m, f, slot)) {
            const sub = m.values[slot].?.msg;
            try decodeInto(sub, r, r.end, @intCast(f.number), depth + 1);
            return;
        }
        const sub = try newSub(m, f.msg_type orelse return Error.InvalidData);
        try decodeInto(sub, r, r.end, @intCast(f.number), depth + 1);
        m.setSlot(slot, .{ .msg = sub });
        return;
    }
    const length = try r.rLen32();
    if (length == 0 and f.presence == .implicit) return;
    if (f.utf8 and !wire.utf8Valid(r.b[r.pos .. r.pos + length])) return Error.InvalidData;
    const payload = try m.a.dupe(u8, r.b[r.pos .. r.pos + length]);
    r.pos += length;
    m.setSlot(slot, .{ .bytes = payload });
}

fn isZero(v: Value) bool {
    return switch (v) {
        .i32 => |x| x == 0,
        .u32 => |x| x == 0,
        .i64 => |x| x == 0,
        .u64 => |x| x == 0,
        .bool => |x| !x,
        .f32 => |x| x == 0,
        .f64 => |x| x == 0,
        else => false,
    };
}

fn readPacked(r: *Reader, kind: Kind, items: *std.ArrayListUnmanaged(Value), a: Allocator) !void {
    const length = try r.rLen32();
    const end = r.pos + length;
    switch (kind) {
        .bool => while (r.pos < end) try items.append(a, .{ .bool = try r.rVarBool() }),
        .fixed32, .sfixed32, .float => {
            const count = length / 4;
            try items.ensureUnusedCapacity(a, count);
            var i: usize = 0;
            while (i < count) : (i += 1) {
                const v = try r.rFixed32();
                try items.append(a, switch (kind) {
                    .sfixed32 => .{ .i32 = @bitCast(v) },
                    .float => .{ .f32 = v },
                    else => .{ .u32 = v },
                });
            }
        },
        .fixed64, .sfixed64, .double => {
            const count = length / 8;
            try items.ensureUnusedCapacity(a, count);
            var i: usize = 0;
            while (i < count) : (i += 1) {
                const v = try r.rFixed64();
                try items.append(a, switch (kind) {
                    .sfixed64 => .{ .i64 = @bitCast(v) },
                    .double => .{ .f64 = v },
                    else => .{ .u64 = v },
                });
            }
        },
        else => while (r.pos < end) try items.append(a, try readPackedVar(r, kind)),
    }
    if (r.pos != end) return Error.InvalidData;
}

fn readPackedVar(r: *Reader, kind: Kind) !Value {
    return switch (kind) {
        .int32, .enumeration => .{ .i32 = @bitCast(try r.rVar32()) },
        .uint32 => .{ .u32 = try r.rVar32() },
        .sint32 => .{ .i32 = wire.unzig32(try r.rVar32()) },
        .int64 => .{ .i64 = @bitCast(try r.rVar64()) },
        .uint64 => .{ .u64 = try r.rVar64() },
        .sint64 => .{ .i64 = wire.unzig64(try r.rVar64()) },
        else => Error.InvalidData,
    };
}

fn readNumSingle(r: *Reader, kind: Kind) !Value {
    return switch (kind) {
        .int32, .enumeration => .{ .i32 = @bitCast(try r.rVar32()) },
        .uint32 => .{ .u32 = try r.rVar32() },
        .bool => .{ .bool = try r.rVarBool() },
        .sint32 => .{ .i32 = wire.unzig32(try r.rVar32()) },
        .int64 => .{ .i64 = @bitCast(try r.rVar64()) },
        .uint64 => .{ .u64 = try r.rVar64() },
        .sint64 => .{ .i64 = wire.unzig64(try r.rVar64()) },
        .fixed32 => .{ .u32 = try r.rFixed32() },
        .sfixed32 => .{ .i32 = @bitCast(try r.rFixed32()) },
        .float => .{ .f32 = try r.rFixed32() },
        .fixed64 => .{ .u64 = try r.rFixed64() },
        .sfixed64 => .{ .i64 = @bitCast(try r.rFixed64()) },
        .double => .{ .f64 = try r.rFixed64() },
        else => Error.InvalidData,
    };
}

fn checkRequired(m: *const Message, cap: u32, depth: u32) !void {
    if (depth > cap) return Error.LimitExceeded;
    const def = m.defn();
    if (def.has_required) {
        for (def.fields, 0..) |*f, slot| {
            if (f.presence == .required and m.values[slot] == null) return Error.RequiredMissing;
        }
    }
    for (def.fields, 0..) |*f, slot| {
        if (f.kind != .message and f.kind != .group) continue;
        const v = m.values[slot] orelse continue;
        switch (v) {
            .msg => |sub| try checkRequired(sub, cap, depth + 1),
            .list => |items| for (items.items) |item| {
                if (item == .msg) try checkRequired(item.msg, cap, depth + 1);
            },
            else => {},
        }
    }
}

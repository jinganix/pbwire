//! Encoder: one pass into a buffer, fields in schema (field-number) order,
//! unknown fields appended verbatim in input order. Nested payloads use a
//! one-byte length placeholder patched afterwards and packed runs use a
//! five-byte placeholder shifted down once, so a decode/encode round trip is
//! byte-exact. The Zig twin of `bindings/rust/src/core/encode.rs`.

const std = @import("std");
const descriptor = @import("descriptor.zig");
const tree = @import("tree.zig");
const wire = @import("wire.zig");

const Allocator = std.mem.Allocator;
const Field = descriptor.Field;
const Kind = descriptor.Kind;
const Presence = descriptor.Presence;
const Message = tree.Message;
const Value = tree.Value;
const Error = wire.Error;

const Buf = std.ArrayListUnmanaged(u8);

pub fn encode(alloc: Allocator, m: *const Message) ![]u8 {
    var w: Buf = .empty;
    errdefer w.deinit(alloc);
    try encodeInto(m, &w, alloc, 0);
    return w.toOwnedSlice(alloc);
}

pub fn encodeTo(m: *const Message, w: *Buf, alloc: Allocator) !void {
    w.clearRetainingCapacity();
    try encodeInto(m, w, alloc, 0);
}

pub fn encodeInto(m: *const Message, w: *Buf, alloc: Allocator, depth: u32) !void {
    if (depth > wire.MAX_DEPTH) return Error.LimitExceeded;
    const def = m.defn();
    const check_required = def.has_required;
    for (def.fields, 0..) |*f, slot| {
        const v = m.values[slot] orelse {
            if (check_required and f.presence == .required) return Error.RequiredMissing;
            continue;
        };
        if (f.repeated) {
            const items = if (v == .list) v.list.items else &[_]Value{};
            if (items.len == 0) continue;
            if (f.kind.isPackable()) {
                try encodeRepNum(w, alloc, f, items);
            } else {
                for (items) |item| try writeValue(w, alloc, f, item, depth);
            }
            continue;
        }
        if (f.kind.isPackable()) {
            if (f.presence == .implicit and isZero(v)) continue;
            try writeTag(w, alloc, &f.tag);
            try writeNumSingle(w, alloc, f.kind, v);
            continue;
        }
        if (f.presence == .implicit) {
            if (v == .bytes and v.bytes.len == 0) continue;
        }
        try writeValue(w, alloc, f, v, depth);
    }
    for (m.unknown.items) |u| try w.appendSlice(alloc, u);
}

fn isZero(v: Value) bool {
    return switch (v) {
        .bool => |x| !x,
        .i32 => |x| x == 0,
        .u32 => |x| x == 0,
        .i64 => |x| x == 0,
        .u64 => |x| x == 0,
        .f32 => |x| x == 0,
        .f64 => |x| x == 0,
        else => false,
    };
}

fn writeTag(w: *Buf, alloc: Allocator, tag: *const wire.Tag) !void {
    try w.appendSlice(alloc, tag.slice());
}

fn writeVar32(w: *Buf, alloc: Allocator, v: u32) !void {
    var x: u64 = v;
    while (x > 0x7f) {
        try w.append(alloc, @as(u8, @truncate(x & 0x7f)) | 0x80);
        x >>= 7;
    }
    try w.append(alloc, @truncate(x));
}

fn writeVar64(w: *Buf, alloc: Allocator, v: u64) !void {
    var x = v;
    while (x > 0x7f) {
        try w.append(alloc, @as(u8, @truncate(x & 0x7f)) | 0x80);
        x >>= 7;
    }
    try w.append(alloc, @truncate(x));
}

fn writeFixed32(w: *Buf, alloc: Allocator, v: u32) !void {
    var bytes: [4]u8 = undefined;
    std.mem.writeInt(u32, &bytes, v, .little);
    try w.appendSlice(alloc, &bytes);
}

fn writeFixed64(w: *Buf, alloc: Allocator, v: u64) !void {
    var bytes: [8]u8 = undefined;
    std.mem.writeInt(u64, &bytes, v, .little);
    try w.appendSlice(alloc, &bytes);
}

fn writeLen(w: *Buf, alloc: Allocator, n: usize) !void {
    try writeVar64(w, alloc, @intCast(n));
}

fn writeBytes(w: *Buf, alloc: Allocator, tag: *const wire.Tag, data: []const u8) !void {
    try writeTag(w, alloc, tag);
    try writeLen(w, alloc, data.len);
    try w.appendSlice(alloc, data);
}

fn patchLen(w: *Buf, alloc: Allocator, lp: usize) !void {
    const length = w.items.len - lp - 1;
    if (length < 128) {
        w.items[lp] = @intCast(length);
        return;
    }
    const n = wire.sizeVar32(@intCast(length));
    const extra = n - 1;
    const old_len = w.items.len;
    try w.resize(alloc, old_len + extra);
    @memset(w.items[old_len..], 0);
    std.mem.copyBackwards(u8, w.items[lp + n .. lp + n + length], w.items[lp + 1 .. old_len]);
    var p = lp;
    var x: u64 = @intCast(length);
    while (x > 0x7f) {
        w.items[p] = @as(u8, @truncate(x & 0x7f)) | 0x80;
        p += 1;
        x >>= 7;
    }
    w.items[p] = @truncate(x);
}

fn writeValue(w: *Buf, alloc: Allocator, f: *const Field, v: Value, depth: u32) !void {
    switch (f.kind) {
        .string, .bytes => {
            const data = if (v == .bytes) v.bytes else &[_]u8{};
            try writeBytes(w, alloc, &f.tag, data);
        },
        .message => {
            const sub = if (v == .msg) v.msg else return;
            try writeTag(w, alloc, &f.tag);
            const lp = w.items.len;
            try w.append(alloc, 0);
            try encodeInto(sub, w, alloc, depth + 1);
            try patchLen(w, alloc, lp);
        },
        .group => {
            const sub = if (v == .msg) v.msg else return;
            try writeTag(w, alloc, &f.tag);
            try encodeInto(sub, w, alloc, depth + 1);
            try writeTag(w, alloc, &f.tag_end);
        },
        else => {},
    }
}

fn writeNumSingle(w: *Buf, alloc: Allocator, kind: Kind, v: Value) !void {
    const i = v.asI64() orelse 0;
    const u = v.asU64() orelse 0;
    switch (kind) {
        .int32, .enumeration => {
            if (i >= 0) {
                try writeVar32(w, alloc, @truncate(@as(u64, @bitCast(i))));
            } else {
                try writeVar64(w, alloc, @bitCast(i));
            }
        },
        .uint32 => try writeVar32(w, alloc, @truncate(u)),
        .sint32 => try writeVar32(w, alloc, wire.zig32(@truncate(i))),
        .int64, .uint64 => try writeVar64(w, alloc, u),
        .sint64 => try writeVar64(w, alloc, wire.zig64(i)),
        .bool => try writeVar32(w, alloc, if (u != 0) 1 else 0),
        .fixed32, .sfixed32, .float => try writeFixed32(w, alloc, @truncate(u)),
        else => try writeFixed64(w, alloc, u),
    }
}

fn encodeRepNum(w: *Buf, alloc: Allocator, f: *const Field, items: []const Value) !void {
    const n = items.len;
    if (f.is_packed) {
        switch (f.kind) {
            .bool => {
                try writeTag(w, alloc, &f.tag);
                try writeLen(w, alloc, n);
                for (items) |v| try w.append(alloc, if ((v.asBool() orelse false)) 1 else 0);
            },
            .fixed32, .sfixed32, .float => {
                try writeTag(w, alloc, &f.tag);
                try writeLen(w, alloc, n * 4);
                for (items) |v| try writeFixed32(w, alloc, @truncate(v.asU64() orelse 0));
            },
            .fixed64, .sfixed64, .double => {
                try writeTag(w, alloc, &f.tag);
                try writeLen(w, alloc, n * 8);
                for (items) |v| try writeFixed64(w, alloc, v.asU64() orelse 0);
            },
            else => {
                try writeTag(w, alloc, &f.tag);
                const lp = w.items.len;
                const old = w.items.len;
                try w.resize(alloc, old + 5);
                @memset(w.items[old..], 0);
                for (items) |v| try writeNumSingle(w, alloc, f.kind, v);
                const length = w.items.len - lp - 5;
                const llen = wire.sizeVar32(@intCast(length));
                if (llen != 5) {
                    const written = w.items.len;
                    std.mem.copyForwards(u8, w.items[lp + llen .. lp + llen + length], w.items[lp + 5 .. written]);
                    w.shrinkRetainingCapacity(lp + llen + length);
                }
                var p = lp;
                var x: u64 = @intCast(length);
                while (x > 0x7f) {
                    w.items[p] = @as(u8, @truncate(x & 0x7f)) | 0x80;
                    p += 1;
                    x >>= 7;
                }
                w.items[p] = @truncate(x);
            },
        }
        return;
    }
    for (items) |v| {
        try writeTag(w, alloc, &f.tag);
        try writeNumSingle(w, alloc, f.kind, v);
    }
}

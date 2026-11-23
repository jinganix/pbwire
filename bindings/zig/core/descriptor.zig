//! `FileDescriptorSet` reader and schema builder for the pure-Zig core.
//!
//! Parses just enough of `descriptor.proto` for a binary codec (names, numbers,
//! labels, types, type names, oneofs, extensions, packedness, proto3 optional,
//! editions features, syntax) and skips everything else by wire type, so a
//! newer descriptor.proto never breaks loading.
//!
//! This is the Zig twin of `bindings/rust/src/core/descriptor.rs`; the wire
//! knowledge lives in `wire.zig`, and every allocation belongs to the schema's
//! arena, so a failed load frees the whole partial schema at once.

const std = @import("std");
const wire = @import("wire.zig");

const Allocator = std.mem.Allocator;
const Reader = wire.Reader;
const Tag = wire.Tag;
const Error = wire.Error;

pub const MAX_DEPTH = wire.MAX_DEPTH;

pub const Kind = enum(u8) {
    int32 = 0,
    uint32 = 1,
    sint32 = 2,
    int64 = 3,
    uint64 = 4,
    sint64 = 5,
    bool = 6,
    enumeration = 7,
    fixed32 = 8,
    sfixed32 = 9,
    float = 10,
    fixed64 = 11,
    sfixed64 = 12,
    double = 13,
    string = 14,
    bytes = 15,
    message = 16,
    group = 17,

    pub fn isPackable(self: Kind) bool {
        return @backingInt(self) <= @backingInt(Kind.double);
    }

    pub fn isRef(self: Kind) bool {
        return switch (self) {
            .string, .bytes, .message, .group => true,
            else => false,
        };
    }
};

pub const Presence = enum(u8) {
    implicit = 0,
    explicit = 1,
    required = 2,
    message_presence = 3,
};

pub const Limits = struct {
    max_bytes: usize = 0,
    max_depth: u32 = 0,

    pub fn depth(self: Limits) u32 {
        if (self.max_depth == 0 or self.max_depth > MAX_DEPTH) return MAX_DEPTH;
        return self.max_depth;
    }

    pub fn checkBytes(self: Limits, n: usize, what: []const u8) !void {
        _ = what;
        if (self.max_bytes > 0 and n > self.max_bytes) return Error.LimitExceeded;
    }
};

pub const EnumDef = struct {
    full_name: []const u8,
    by_name: std.StringHashMapUnmanaged(i32) = .empty,
    by_number: std.AutoHashMapUnmanaged(i32, []const u8) = .empty,
    closed: bool = false,
};

pub const Field = struct {
    number: u32,
    name: []const u8,
    kind: Kind,
    /// Wire type of the declared form: LEN for packed numerics.
    wt: u8,
    /// Wire type of a single element (what an unpacked occurrence uses).
    elem_wt: u8,
    repeated: bool,
    is_packed: bool,
    /// Oneof index, -1 when the field is not in a oneof.
    oneof: i32,
    presence: Presence,
    msg_type: ?usize,
    enum_type: ?usize,
    enum_closed: bool,
    /// A decoded string must be valid UTF-8 (proto3/editions VERIFY).
    utf8: bool,
    /// Extensions only: the global name `[pkg.name]`.
    full_name: ?[]const u8,
    tag: Tag,
    tag_end: Tag,
};

pub const MsgDef = struct {
    full_name: []const u8 = "",
    fields: []Field = &[_]Field{},
    slot_by_fnum: ?[]i32 = null,
    by_name: std.StringHashMapUnmanaged(usize) = .empty,
    oneof_by_name: std.StringHashMapUnmanaged(usize) = .empty,
    oneof_count: usize = 0,
    has_required: bool = false,
    deep_required: bool = false,
    map_entry: bool = false,
    reserved_names: []const []const u8 = &[_][]const u8{},
};

pub const CoreSchema = struct {
    owner: Allocator,
    arena: std.heap.ArenaAllocator,
    a: Allocator,
    messages: []MsgDef,
    enums: []EnumDef,
    message_by_name: std.StringHashMapUnmanaged(usize) = .empty,
    enum_by_name: std.StringHashMapUnmanaged(usize) = .empty,

    pub fn destroy(self: *CoreSchema) void {
        const owner = self.owner;
        self.arena.deinit();
        owner.destroy(self);
    }

    pub fn messageIndex(self: *const CoreSchema, full_name: []const u8) !usize {
        return self.message_by_name.get(full_name) orelse return Error.NotFound;
    }

    pub fn message(self: *const CoreSchema, full_name: []const u8) !*const MsgDef {
        return &self.messages[try self.messageIndex(full_name)];
    }

    /// Slot of a field number, -1 when the message has no such field.
    pub fn slotOf(self: *const CoreSchema, def: usize, fnum: u32) i32 {
        const d = &self.messages[def];
        if (d.slot_by_fnum) |slots| {
            if (fnum < slots.len) return slots[fnum];
            return -1;
        }
        for (d.fields, 0..) |f, i| {
            if (f.number == fnum) return @intCast(i);
        }
        return -1;
    }
};

// descriptor.proto type/label numbers
const T_DOUBLE: i32 = 1;
const T_FLOAT: i32 = 2;
const T_INT64: i32 = 3;
const T_UINT64: i32 = 4;
const T_INT32: i32 = 5;
const T_FIXED64: i32 = 6;
const T_FIXED32: i32 = 7;
const T_BOOL: i32 = 8;
const T_STRING: i32 = 9;
const T_GROUP: i32 = 10;
const T_MESSAGE: i32 = 11;
const T_BYTES: i32 = 12;
const T_UINT32: i32 = 13;
const T_ENUM: i32 = 14;
const T_SFIXED32: i32 = 15;
const T_SFIXED64: i32 = 16;
const T_SINT32: i32 = 17;
const T_SINT64: i32 = 18;

const L_OPTIONAL: i32 = 1;
const L_REQUIRED: i32 = 2;
const L_REPEATED: i32 = 3;

const POneof = struct {
    name: []const u8 = "",
};

const PField = struct {
    name: ?[]const u8 = null,
    extendee: ?[]const u8 = null,
    number: i32 = 0,
    label: i32 = L_OPTIONAL,
    type_: i32 = 0,
    type_name: ?[]const u8 = null,
    oneof_index: i32 = -1,
    proto3_optional: bool = false,
    is_packed: bool = false,
    has_packed_option: bool = false,
    enum_open: i32 = 0,
    utf8: i32 = 0,
    msg_type: ?usize = null,
    enum_type: ?usize = null,
    enum_closed: bool = false,
    oneof: ?usize = null,
    has_presence: bool = false,
    full_name: ?[]const u8 = null,
};

const PEnumValue = struct {
    name: ?[]const u8 = null,
    number: i32 = 0,
};

const PEnum = struct {
    name: ?[]const u8 = null,
    full_name: []const u8 = "",
    idx: usize = 0,
    values: std.ArrayListUnmanaged(PEnumValue) = .empty,
    enum_open: i32 = 0,
    closed: bool = false,
};

const PMessage = struct {
    name: ?[]const u8 = null,
    full_name: []const u8 = "",
    idx: usize = 0,
    map_entry: bool = false,
    fields: std.ArrayListUnmanaged(PField) = .empty,
    nested: std.ArrayListUnmanaged(PMessage) = .empty,
    extensions: std.ArrayListUnmanaged(PField) = .empty,
    oneofs: std.ArrayListUnmanaged(POneof) = .empty,
    enums: std.ArrayListUnmanaged(PEnum) = .empty,
    reserved_names: std.ArrayListUnmanaged([]const u8) = .empty,
};

const PFile = struct {
    name: ?[]const u8 = null,
    pkg: []const u8 = "",
    syntax: []const u8 = "",
    edition: bool = false,
    enum_open: i32 = 0,
    utf8: i32 = 0,
    messages: std.ArrayListUnmanaged(PMessage) = .empty,
    extensions: std.ArrayListUnmanaged(PField) = .empty,
    enums: std.ArrayListUnmanaged(PEnum) = .empty,
};

fn dup(a: Allocator, b: []const u8) ![]const u8 {
    return a.dupe(u8, b);
}

fn strOf(a: Allocator, b: []const u8, off: usize, n: usize) ![]const u8 {
    return a.dupe(u8, b[off .. off + n]);
}

fn asInt32(v: u64) i32 {
    return @bitCast(@as(u32, @truncate(v)));
}

fn join(a: Allocator, scope: []const u8, name: []const u8) ![]const u8 {
    if (scope.len == 0) return a.dupe(u8, name);
    return std.fmt.allocPrint(a, "{s}.{s}", .{ scope, name });
}

fn parseSet(a: Allocator, bytes: []const u8) ![]PFile {
    var r = Reader.init(bytes);
    var files: std.ArrayListUnmanaged(PFile) = .empty;
    while (r.pos < r.end) {
        const tag = try r.rVar32();
        if (tag >> 3 == 1 and @as(u8, @truncate(tag & 7)) == wire.WT_LEN) {
            const n = try r.rLen();
            try files.append(a, try parseFile(a, bytes, r.pos, n));
            r.pos += n;
        } else {
            try r.skip(@truncate(tag & 7));
        }
    }
    return files.toOwnedSlice(a);
}

fn parseFile(a: Allocator, b: []const u8, off: usize, len: usize) !PFile {
    var r = try Reader.sliceRange(b, off, off + len);
    var f = PFile{};
    while (r.pos < r.end) {
        const tag = try r.rVar32();
        const num = tag >> 3;
        const wt: u8 = @truncate(tag & 7);
        if (wt == wire.WT_VARINT) {
            if (num == 14) {
                _ = try r.rVar32();
                f.edition = true;
            } else {
                try r.skip(wt);
            }
            continue;
        }
        if (wt == wire.WT_LEN) {
            const n = try r.rLen();
            switch (num) {
                4 => try f.messages.append(a, try parseMessage(a, b, r.pos, n, 1)),
                5 => try f.enums.append(a, try parseEnum(a, b, r.pos, n)),
                7 => try f.extensions.append(a, try parseField(a, b, r.pos, n)),
                8 => {
                    f.enum_open = try parseFeatures(a, b, r.pos, r.pos + n, 50, Feature.enum_type);
                    f.utf8 = try parseFeatures(a, b, r.pos, r.pos + n, 50, Feature.utf8_validation);
                },
                1 => f.name = try strOf(a, b, r.pos, n),
                2 => f.pkg = try strOf(a, b, r.pos, n),
                12 => f.syntax = try strOf(a, b, r.pos, n),
                else => {},
            }
            r.pos += n;
        } else {
            try r.skip(wt);
        }
    }
    return f;
}

fn parseMessage(a: Allocator, b: []const u8, off: usize, len: usize, depth: u32) !PMessage {
    if (depth > MAX_DEPTH) return Error.InvalidData;
    var r = try Reader.sliceRange(b, off, off + len);
    var m = PMessage{};
    while (r.pos < r.end) {
        const tag = try r.rVar32();
        const num = tag >> 3;
        if (@as(u8, @truncate(tag & 7)) != wire.WT_LEN) {
            try r.skip(@truncate(tag & 7));
            continue;
        }
        const n = try r.rLen();
        switch (num) {
            1 => m.name = try strOf(a, b, r.pos, n),
            2 => try m.fields.append(a, try parseField(a, b, r.pos, n)),
            3 => try m.nested.append(a, try parseMessage(a, b, r.pos, n, depth + 1)),
            4 => try m.enums.append(a, try parseEnum(a, b, r.pos, n)),
            6 => try m.extensions.append(a, try parseField(a, b, r.pos, n)),
            7 => m.map_entry = try parseMapEntry(b, r.pos, n),
            8 => try m.oneofs.append(a, try parseOneof(a, b, r.pos, n)),
            10 => try m.reserved_names.append(a, try strOf(a, b, r.pos, n)),
            else => {},
        }
        r.pos += n;
    }
    if (m.name == null) return Error.InvalidData;
    return m;
}

fn parseOneof(a: Allocator, b: []const u8, off: usize, len: usize) !POneof {
    var r = try Reader.sliceRange(b, off, off + len);
    var o = POneof{};
    while (r.pos < r.end) {
        const tag = try r.rVar32();
        if (tag >> 3 == 1 and @as(u8, @truncate(tag & 7)) == wire.WT_LEN) {
            const n = try r.rLen();
            o.name = try strOf(a, b, r.pos, n);
            r.pos += n;
        } else {
            try r.skip(@truncate(tag & 7));
        }
    }
    return o;
}

fn parseEnum(a: Allocator, b: []const u8, off: usize, len: usize) !PEnum {
    var r = try Reader.sliceRange(b, off, off + len);
    var e = PEnum{};
    while (r.pos < r.end) {
        const tag = try r.rVar32();
        const num = tag >> 3;
        if (@as(u8, @truncate(tag & 7)) != wire.WT_LEN) {
            try r.skip(@truncate(tag & 7));
            continue;
        }
        const n = try r.rLen();
        switch (num) {
            1 => e.name = try strOf(a, b, r.pos, n),
            2 => try e.values.append(a, try parseEnumValue(a, b, r.pos, n)),
            3 => e.enum_open = try parseFeatures(a, b, r.pos, r.pos + n, 7, Feature.enum_type),
            else => {},
        }
        r.pos += n;
    }
    return e;
}

fn parseEnumValue(a: Allocator, b: []const u8, off: usize, len: usize) !PEnumValue {
    var r = try Reader.sliceRange(b, off, off + len);
    var v = PEnumValue{};
    var has_number = false;
    while (r.pos < r.end) {
        const tag = try r.rVar32();
        const num = tag >> 3;
        const wt: u8 = @truncate(tag & 7);
        if (wt == wire.WT_VARINT) {
            if (num == 2) {
                v.number = asInt32(try r.rVar32());
                has_number = true;
            } else {
                try r.skip(wt);
            }
        } else if (wt == wire.WT_LEN) {
            const n = try r.rLen();
            if (num == 1) v.name = try strOf(a, b, r.pos, n);
            r.pos += n;
        } else {
            try r.skip(wt);
        }
    }
    if (v.name == null or !has_number) return Error.InvalidData;
    return v;
}

fn parseMapEntry(b: []const u8, off: usize, len: usize) !bool {
    var r = try Reader.sliceRange(b, off, off + len);
    var map_entry = false;
    while (r.pos < r.end) {
        const tag = try r.rVar32();
        if (@as(u8, @truncate(tag & 7)) != wire.WT_VARINT) {
            try r.skip(@truncate(tag & 7));
            continue;
        }
        if (tag >> 3 == 7) {
            map_entry = (try r.rVar32()) != 0;
        } else {
            try r.skip(wire.WT_VARINT);
        }
    }
    return map_entry;
}

const Feature = enum { enum_type, utf8_validation };

fn parseFeatures(a: Allocator, b: []const u8, off: usize, end: usize, option_number: u32, feature: Feature) !i32 {
    _ = a;
    var r = try Reader.sliceRange(b, off, end);
    var value: i32 = 0;
    while (r.pos < r.end) {
        const tag = try r.rVar32();
        const num = tag >> 3;
        if (@as(u8, @truncate(tag & 7)) != wire.WT_LEN) {
            try r.skip(@truncate(tag & 7));
            continue;
        }
        const n = try r.rLen();
        if (num == option_number) value = try parseFeatureSet(b, r.pos, r.pos + n, feature);
        r.pos += n;
    }
    return value;
}

fn parseFeatureSet(b: []const u8, off: usize, end: usize, feature: Feature) !i32 {
    var r = try Reader.sliceRange(b, off, end);
    var value: i32 = 0;
    while (r.pos < r.end) {
        const tag = try r.rVar32();
        const num = tag >> 3;
        const wanted: u32 = if (feature == .enum_type) 2 else 4;
        if (num == wanted and @as(u8, @truncate(tag & 7)) == wire.WT_VARINT) {
            const v = asInt32(try r.rVar32());
            if (feature == .enum_type) {
                if (v == 1 or v == 2) value = v;
            } else {
                if (v == 2 or v == 3) value = v;
            }
        } else {
            try r.skip(@truncate(tag & 7));
        }
    }
    return value;
}

fn parseField(a: Allocator, b: []const u8, off: usize, len: usize) !PField {
    var r = try Reader.sliceRange(b, off, off + len);
    var f = PField{ .label = L_OPTIONAL, .oneof_index = -1 };
    while (r.pos < r.end) {
        const tag = try r.rVar32();
        const num = tag >> 3;
        const wt: u8 = @truncate(tag & 7);
        if (wt == wire.WT_VARINT) {
            const v = try r.rVar32();
            switch (num) {
                3 => f.number = asInt32(v),
                4 => f.label = asInt32(v),
                5 => f.type_ = asInt32(v),
                9 => f.oneof_index = asInt32(v),
                17 => f.proto3_optional = v != 0,
                else => {},
            }
        } else if (wt == wire.WT_LEN) {
            const n = try r.rLen();
            switch (num) {
                1 => f.name = try strOf(a, b, r.pos, n),
                2 => f.extendee = try strOf(a, b, r.pos, n),
                6 => f.type_name = try strOf(a, b, r.pos, n),
                8 => try parseFieldOptions(b, r.pos, r.pos + n, &f),
                else => {},
            }
            r.pos += n;
        } else {
            try r.skip(wt);
        }
    }
    if (f.name == null or f.type_ == 0 or f.number == 0) return Error.InvalidData;
    if (f.type_ < T_DOUBLE or f.type_ > T_SINT64) return Error.InvalidData;
    if (f.label < L_OPTIONAL or f.label > L_REPEATED) return Error.InvalidData;
    return f;
}

fn parseFieldOptions(b: []const u8, off: usize, end: usize, f: *PField) !void {
    var r = try Reader.sliceRange(b, off, end);
    while (r.pos < r.end) {
        const tag = try r.rVar32();
        const num = tag >> 3;
        if (num == 2 and @as(u8, @truncate(tag & 7)) == wire.WT_VARINT) {
            f.is_packed = (try r.rVar32()) != 0;
            f.has_packed_option = true;
        } else if (num == 21 and @as(u8, @truncate(tag & 7)) == wire.WT_LEN) {
            const n = try r.rLen();
            f.enum_open = try parseFeatureSet(b, r.pos, r.pos + n, .enum_type);
            f.utf8 = try parseFeatureSet(b, r.pos, r.pos + n, .utf8_validation);
            r.pos += n;
        } else {
            try r.skip(@truncate(tag & 7));
        }
    }
}

const Linker = struct {
    a: Allocator,
    message_by_name: std.StringHashMapUnmanaged(usize) = .empty,
    enum_by_name: std.StringHashMapUnmanaged(usize) = .empty,
    enum_closed: std.ArrayListUnmanaged(bool) = .empty,
    next_message: usize = 0,
    next_enum: usize = 0,

    fn indexMessage(self: *Linker, m: *PMessage, scope: []const u8) !void {
        const name = m.name orelse "";
        m.full_name = try join(self.a, scope, name);
        if (self.message_by_name.contains(m.full_name)) return Error.InvalidData;
        m.idx = self.next_message;
        self.next_message += 1;
        try self.message_by_name.put(self.a, m.full_name, m.idx);
        for (m.enums.items) |*e| try self.indexEnum(e, m.full_name);
        for (m.nested.items) |*n| try self.indexMessage(n, m.full_name);
    }

    fn indexEnum(self: *Linker, e: *PEnum, scope: []const u8) !void {
        const name = e.name orelse "";
        e.full_name = try join(self.a, scope, name);
        if (self.enum_by_name.contains(e.full_name)) return Error.InvalidData;
        e.idx = self.next_enum;
        self.next_enum += 1;
        try self.enum_closed.append(self.a, false);
        try self.enum_by_name.put(self.a, e.full_name, e.idx);
    }

    fn linkEnum(self: *Linker, e: *PEnum, proto3: bool, file_enum_open: i32) !void {
        if (e.name == null) return Error.InvalidData;
        e.closed = if (e.enum_open != 0)
            e.enum_open == 2
        else if (file_enum_open != 0)
            file_enum_open == 2
        else
            !proto3;
        self.enum_closed.items[e.idx] = e.closed;
    }

    fn linkEnums(self: *Linker, m: *PMessage, proto3: bool, file_enum_open: i32) !void {
        if (m.full_name.len == 0) return Error.InvalidData;
        for (m.enums.items) |*e| try self.linkEnum(e, proto3, file_enum_open);
        for (m.nested.items) |*n| try self.linkEnums(n, proto3, file_enum_open);
    }

    fn linkMessage(self: *Linker, m: *PMessage, scope: []const u8, proto3: bool, file_utf8: i32) !void {
        const full = m.full_name;
        const oneofs: []const POneof = m.oneofs.items;
        for (m.fields.items) |*f| try self.linkField(f, scope, proto3, file_utf8, oneofs);
        for (m.nested.items) |*n| try self.linkMessage(n, full, proto3, file_utf8);
        for (m.extensions.items) |*e| {
            try self.linkField(e, scope, proto3, file_utf8, &[_]POneof{});
            if (e.name) |n| e.full_name = try join(self.a, scope, n);
        }
    }

    fn linkField(self: *Linker, f: *PField, scope: []const u8, proto3: bool, file_utf8: i32, parent_oneofs: []const POneof) !void {
        if (f.oneof_index >= 0 and parent_oneofs.len > 0) {
            if (@as(usize, @intCast(f.oneof_index)) >= parent_oneofs.len) return Error.InvalidData;
            f.oneof = @intCast(f.oneof_index);
        }
        if ((f.type_ == T_MESSAGE or f.type_ == T_GROUP) and f.type_name != null) {
            const name = f.type_name.?;
            f.msg_type = (try self.resolveMessage(scope, name)) orelse return Error.InvalidData;
        }
        if (f.type_ == T_ENUM and f.type_name != null) {
            const name = f.type_name.?;
            const idx = (try self.resolveEnum(scope, name)) orelse return Error.InvalidData;
            f.enum_type = idx;
            f.enum_closed = if (f.enum_open != 0) f.enum_open == 2 else self.enum_closed.items[idx];
        }
        if (f.label == L_REPEATED and isPackableType(f.type_) and !f.has_packed_option) {
            f.is_packed = proto3;
        }
        if (f.type_ == T_STRING) {
            const utf8: i32 = if (f.utf8 != 0)
                f.utf8
            else if (file_utf8 != 0)
                file_utf8
            else if (proto3)
                2
            else
                3;
            f.utf8 = if (utf8 == 2) 2 else 3;
        } else {
            f.utf8 = 3;
        }
        f.has_presence = f.label != L_REPEATED and
            (f.type_ == T_MESSAGE or f.type_ == T_GROUP or
                f.label == L_REQUIRED or !proto3 or f.proto3_optional);
    }

    fn resolveMessage(self: *Linker, scope: []const u8, name: []const u8) !?usize {
        if (name.len > 0 and name[0] == '.') return self.message_by_name.get(name[1..]);
        var s: []const u8 = scope;
        while (true) {
            const key = if (s.len == 0) try self.a.dupe(u8, name) else try join(self.a, s, name);
            if (self.message_by_name.get(key)) |i| return i;
            if (std.mem.indexOfScalar(u8, name, '.')) |dot| {
                const head = name[0..dot];
                const rest = name[dot + 1 ..];
                const prefix = if (s.len == 0) try self.a.dupe(u8, head) else try join(self.a, s, head);
                if (self.message_by_name.contains(prefix)) return self.resolveMessage(prefix, rest);
            }
            if (s.len == 0) return null;
            if (std.mem.lastIndexOfScalar(u8, s, '.')) |cut| s = s[0..cut] else s = "";
        }
    }

    fn resolveEnum(self: *Linker, scope: []const u8, name: []const u8) !?usize {
        if (name.len > 0 and name[0] == '.') return self.enum_by_name.get(name[1..]);
        var s: []const u8 = scope;
        while (true) {
            const key = if (s.len == 0) try self.a.dupe(u8, name) else try join(self.a, s, name);
            if (self.enum_by_name.get(key)) |i| return i;
            if (std.mem.indexOfScalar(u8, name, '.')) |dot| {
                const head = name[0..dot];
                const rest = name[dot + 1 ..];
                const prefix = if (s.len == 0) try self.a.dupe(u8, head) else try join(self.a, s, head);
                if (self.enum_by_name.contains(prefix)) return self.resolveEnum(prefix, rest);
            }
            if (s.len == 0) return null;
            if (std.mem.lastIndexOfScalar(u8, s, '.')) |cut| s = s[0..cut] else s = "";
        }
    }

    fn linkFiles(self: *Linker, files: []PFile) !void {
        for (files) |*f| {
            for (f.messages.items) |*m| try self.indexMessage(m, f.pkg);
            for (f.enums.items) |*e| try self.indexEnum(e, f.pkg);
        }
        for (files) |*f| {
            const proto3 = std.mem.eql(u8, f.syntax, "proto3") or f.edition;
            for (f.enums.items) |*e| try self.linkEnum(e, proto3, f.enum_open);
            for (f.messages.items) |*m| try self.linkEnums(m, proto3, f.enum_open);
            for (f.messages.items) |*m| try self.linkMessage(m, f.pkg, proto3, f.utf8);
            for (f.extensions.items) |*e| {
                try self.linkField(e, f.pkg, proto3, f.utf8, &[_]POneof{});
                if (e.name) |n| e.full_name = try join(self.a, f.pkg, n);
            }
        }
    }
};

fn isPackableType(t: i32) bool {
    return !(t == T_STRING or t == T_GROUP or t == T_MESSAGE or t == T_BYTES);
}

fn kindOf(t: i32) Kind {
    return switch (t) {
        T_INT32 => .int32,
        T_UINT32 => .uint32,
        T_SINT32 => .sint32,
        T_INT64 => .int64,
        T_UINT64 => .uint64,
        T_SINT64 => .sint64,
        T_BOOL => .bool,
        T_ENUM => .enumeration,
        T_FIXED32 => .fixed32,
        T_SFIXED32 => .sfixed32,
        T_FLOAT => .float,
        T_FIXED64 => .fixed64,
        T_SFIXED64 => .sfixed64,
        T_DOUBLE => .double,
        T_STRING => .string,
        T_BYTES => .bytes,
        T_MESSAGE => .message,
        else => .group,
    };
}

fn wireType(t: i32, is_packed: bool) u8 {
    if (is_packed) return wire.WT_LEN;
    return switch (t) {
        T_DOUBLE, T_FIXED64, T_SFIXED64 => 1,
        T_STRING, T_BYTES, T_MESSAGE => wire.WT_LEN,
        T_GROUP => wire.WT_START_GROUP,
        T_FIXED32, T_SFIXED32, T_FLOAT => 5,
        else => wire.WT_VARINT,
    };
}

fn buildEnum(schema: *CoreSchema, e: *const PEnum) !void {
    var def = EnumDef{ .full_name = e.full_name, .closed = e.closed };
    for (e.values.items) |v| {
        const name = v.name orelse return Error.InvalidData;
        if (def.by_name.contains(name)) return Error.InvalidData;
        try def.by_name.put(schema.a, name, v.number);
        if (!def.by_number.contains(v.number)) try def.by_number.put(schema.a, v.number, name);
    }
    schema.enums[e.idx] = def;
}

fn buildEnums(schema: *CoreSchema, m: *const PMessage, depth: u32) !void {
    if (depth > MAX_DEPTH) return Error.InvalidData;
    for (m.enums.items) |*e| try buildEnum(schema, e);
    for (m.nested.items) |*n| try buildEnums(schema, n, depth + 1);
}

fn buildField(a: Allocator, f: *const PField, oneofs: *std.AutoHashMapUnmanaged(usize, usize)) !Field {
    const kind = kindOf(f.type_);
    const repeated = f.label == L_REPEATED;
    var oneof: i32 = -1;
    if (f.oneof) |declared| {
        const next = oneofs.count();
        const gop = try oneofs.getOrPut(a, declared);
        if (!gop.found_existing) gop.value_ptr.* = next;
        oneof = @intCast(gop.value_ptr.*);
    }
    const presence: Presence = if (oneof >= 0)
        .explicit
    else if (f.label == L_REQUIRED)
        .required
    else if (kind == .message or kind == .group)
        .message_presence
    else if (f.has_presence)
        .explicit
    else
        .implicit;
    const is_packed = repeated and f.is_packed;
    const number: u32 = @intCast(f.number);
    return .{
        .number = number,
        .name = f.name orelse "",
        .kind = kind,
        .wt = wireType(f.type_, is_packed),
        .elem_wt = wireType(f.type_, false),
        .repeated = repeated,
        .is_packed = is_packed,
        .oneof = oneof,
        .presence = presence,
        .msg_type = f.msg_type,
        .enum_type = f.enum_type,
        .enum_closed = f.enum_closed,
        .utf8 = f.type_ == T_STRING and f.utf8 == 2,
        .full_name = f.full_name,
        .tag = Tag.new(number, wireType(f.type_, is_packed)),
        .tag_end = if (kind == .group) Tag.new(number, wire.WT_END_GROUP) else Tag{},
    };
}

fn lessThan(_: void, a: Field, b: Field) bool {
    return a.number < b.number;
}

fn finishDef(
    a: Allocator,
    d: *MsgDef,
    fields: *std.ArrayListUnmanaged(Field),
    oneofs: *std.AutoHashMapUnmanaged(usize, usize),
    oneof_names: []const POneof,
) !void {
    std.mem.sort(Field, fields.items, {}, lessThan);
    var max_number: u32 = 0;
    var has_required = false;
    for (fields.items) |f| {
        if (f.number > max_number) max_number = f.number;
        if (f.presence == .required) has_required = true;
    }
    d.has_required = has_required;
    if (max_number <= 512 and @as(usize, max_number) <= fields.items.len * 8 + 8) {
        const slots = try a.alloc(i32, @as(usize, max_number) + 1);
        @memset(slots, -1);
        for (fields.items, 0..) |f, i| slots[@as(usize, f.number)] = @intCast(i);
        d.slot_by_fnum = slots;
    } else {
        d.slot_by_fnum = null;
    }
    d.by_name = .empty;
    for (fields.items, 0..) |f, i| try d.by_name.put(a, f.name, i);
    d.oneof_by_name = .empty;
    var it = oneofs.iterator();
    while (it.next()) |entry| {
        if (entry.key_ptr.* < oneof_names.len) {
            try d.oneof_by_name.put(a, oneof_names[entry.key_ptr.*].name, entry.value_ptr.*);
        }
    }
    d.oneof_count = @max(d.oneof_count, oneofs.count());
    d.fields = try fields.toOwnedSlice(a);
}

fn buildDef(schema: *CoreSchema, m: *const PMessage, depth: u32) !void {
    if (depth > MAX_DEPTH) return Error.InvalidData;
    var oneofs: std.AutoHashMapUnmanaged(usize, usize) = .empty;
    var fields: std.ArrayListUnmanaged(Field) = .empty;
    for (m.fields.items) |*f| try fields.append(schema.a, try buildField(schema.a, f, &oneofs));
    const d = &schema.messages[m.idx];
    d.full_name = m.full_name;
    d.reserved_names = m.reserved_names.items;
    d.map_entry = m.map_entry;
    try finishDef(schema.a, d, &fields, &oneofs, m.oneofs.items);
    for (m.nested.items) |*n| try buildDef(schema, n, depth + 1);
}

fn attachExtension(schema: *CoreSchema, e: *const PField) !void {
    var extendee = e.extendee orelse return Error.InvalidData;
    if (extendee.len > 0 and extendee[0] == '.') extendee = extendee[1..];
    const idx = schema.message_by_name.get(extendee) orelse return Error.InvalidData;
    var empty: std.AutoHashMapUnmanaged(usize, usize) = .empty;
    var s = try buildField(schema.a, e, &empty);
    s.full_name = e.full_name;
    var fields: std.ArrayListUnmanaged(Field) = .empty;
    try fields.appendSlice(schema.a, schema.messages[idx].fields);
    try fields.append(schema.a, s);
    const oneof_by_name = schema.messages[idx].oneof_by_name;
    const count = schema.messages[idx].oneof_count;
    try finishDef(schema.a, &schema.messages[idx], &fields, &empty, &[_]POneof{});
    schema.messages[idx].oneof_by_name = oneof_by_name;
    schema.messages[idx].oneof_count = count;
}

fn attachMessageExtensions(schema: *CoreSchema, m: *const PMessage) !void {
    for (m.extensions.items) |*e| try attachExtension(schema, e);
    for (m.nested.items) |*n| try attachMessageExtensions(schema, n);
}

fn finishTransitive(schema: *CoreSchema) void {
    var changed = true;
    while (changed) {
        changed = false;
        for (schema.messages) |*d| {
            if (d.deep_required) continue;
            var needs = false;
            for (d.fields) |f| {
                if (f.presence == .required) {
                    needs = true;
                    break;
                }
                if ((f.kind == .message or f.kind == .group) and f.msg_type != null) {
                    if (schema.messages[f.msg_type.?].deep_required) {
                        needs = true;
                        break;
                    }
                }
            }
            if (needs) {
                d.deep_required = true;
                changed = true;
            }
        }
    }
}

/// Parses and links a `FileDescriptorSet` into a compiled schema. The returned
/// schema owns its arena; destroy it with `destroy`.
pub fn loadSchema(owner: Allocator, bytes: []const u8, limits: Limits) !*CoreSchema {
    try limits.checkBytes(bytes.len, "descriptor set");
    const self = try owner.create(CoreSchema);
    self.* = .{
        .owner = owner,
        .arena = std.heap.ArenaAllocator.init(owner),
        .a = undefined,
        .messages = &[_]MsgDef{},
        .enums = &[_]EnumDef{},
    };
    self.a = self.arena.allocator();
    errdefer self.destroy();

    const a = self.a;
    const files = parseSet(a, bytes) catch return Error.InvalidData;
    var linker = Linker{ .a = a };
    linker.linkFiles(files) catch return Error.InvalidData;

    self.messages = try a.alloc(MsgDef, linker.next_message);
    for (self.messages) |*d| d.* = .{};
    self.enums = try a.alloc(EnumDef, linker.next_enum);
    for (self.enums) |*e| e.* = .{ .full_name = "" };

    var mit = linker.message_by_name.iterator();
    while (mit.next()) |entry| try self.message_by_name.put(a, entry.key_ptr.*, entry.value_ptr.*);
    var eit = linker.enum_by_name.iterator();
    while (eit.next()) |entry| try self.enum_by_name.put(a, entry.key_ptr.*, entry.value_ptr.*);

    for (files) |*f| {
        for (f.enums.items) |*e| try buildEnum(self, e);
        for (f.messages.items) |*m| try buildEnums(self, m, 1);
    }
    for (files) |*f| {
        for (f.messages.items) |*m| try buildDef(self, m, 1);
    }
    for (files) |*f| {
        for (f.extensions.items) |*e| try attachExtension(self, e);
        for (f.messages.items) |*m| try attachMessageExtensions(self, m);
    }
    finishTransitive(self);
    return self;
}

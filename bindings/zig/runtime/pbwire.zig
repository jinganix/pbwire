//! Zig binding over the pbwire C ABI: a dynamic protobuf message tree with no
//! generated code. A `FileDescriptorSet` (`protoc --descriptor_set_out`) is
//! loaded at run time and drives field lookup, wire types and presence; the
//! wire format itself lives in the C core, which this module links statically
//! (see `build.zig`: the core sources compile into `libpbwire.a`, no `dlopen`,
//! no second wire implementation, no bindgen).
//!
//! ```zig
//! const pbwire = @import("pbwire");
//!
//! var schema = try pbwire.Schema.fromBytes(desc_bytes);
//! defer schema.deinit();
//! const ty = try schema.messageType("structtest.Scalars3");
//! const f_str = try ty.field("str");
//!
//! var msg = try ty.newMessage();
//! defer msg.deinit();
//! try msg.setStr(f_str, "alice");
//! const wire = try msg.encode(allocator);
//! defer allocator.free(wire);
//!
//! var decoded = try ty.decode(wire);
//! defer decoded.deinit();
//! // unknown fields are preserved: re-encoding is byte-exact.
//! ```
//!
//! A `Field` is resolved once and reused: `set*`/`get*` take the handle, so a
//! hot loop makes one native call per operation instead of a name lookup.
//!
//! Ownership: a `Schema` frees itself on `deinit`, and so does a root
//! `Message` (from `newMessage`/`decode`/parse). A nested message is a view of
//! its parent's tree (`setMessage`/`getMessage`/`asMessage`): it borrows the
//! parent, so it must not outlive it, and `deinit` on it is a no-op.
//!
//! Not thread safe, like the C core: one message belongs to one thread. A
//! schema is immutable once loaded, so it can be shared between threads.
const std = @import("std");

    pub const c = @import("c");

pub const Error = error{
    InvalidArgument,
    OutOfMemory,
    NotFound,
    WrongType,
    LimitExceeded,
    RequiredMissing,
    BufferTooSmall,
    BadUtf8,
    NameTooLong,
    UnknownStatus,
};

fn check(status: c.pbw_status) Error!void {
    if (status == c.PBW_OK) return;
    return switch (@as(c_int, @bitCast(status))) {
        c.PBW_ERR_INVALID => Error.InvalidArgument,
        c.PBW_ERR_NOMEM => Error.OutOfMemory,
        c.PBW_ERR_NOT_FOUND => Error.NotFound,
        c.PBW_ERR_TYPE => Error.WrongType,
        c.PBW_ERR_LIMIT => Error.LimitExceeded,
        c.PBW_ERR_REQUIRED => Error.RequiredMissing,
        else => Error.UnknownStatus,
    };
}

/// Human-readable form of a `pbw_status` value (for logs and test failures).
pub fn strerror(status: c.pbw_status) [:0]const u8 {
    return zstr(c.pbw_strerror(status));
}

fn zstr(p: [*c]const u8) [:0]const u8 {
    if (p == null) return "";
    return std.mem.span(@as([*:0]const u8, @ptrCast(p)));
}

var empty_sentinel: u8 = 0;
var empty_mut: u8 = 0;

/// Non-null data pointer for a (possibly empty) slice. The core never reads
/// `len` bytes from it when `len == 0`, so a stable one-byte stand-in is safe.
fn dataPtr(bytes: []const u8) [*]const u8 {
    if (bytes.len == 0) return @ptrCast(&empty_sentinel);
    return bytes.ptr;
}

/// Mutable twin of `dataPtr`, for the encode-into-caller-memory path.
fn dataPtrMut(bytes: []u8) [*]u8 {
    if (bytes.len == 0) return @ptrCast(&empty_mut);
    return bytes.ptr;
}

/// Copies `name` into `buf` as NUL-terminated text for the C lookup calls.
/// Rejects interior NUL bytes (they would silently truncate on the way to C).
fn toNul(name: []const u8, buf: *[512]u8) Error!void {
    if (name.len >= buf.len) return Error.NameTooLong;
    for (name, 0..) |b, i| {
        if (b == 0) return Error.InvalidArgument;
        buf[i] = @bitCast(b);
    }
    buf[name.len] = 0;
}

fn nulPtr(buf: *[512]u8) [*c]const u8 {
    return @ptrCast(buf);
}

/// Reads a whole file (descriptor sets, fixtures). Zig 0.15+ routes all
/// blocking I/O through an explicit `Io`; one short-lived threaded loop per
/// call is plenty for load-once inputs like these.
pub fn readFile(allocator: std.mem.Allocator, path: []const u8) ![]u8 {
    var threaded: std.Io.Threaded = .init(allocator, .{});
    defer threaded.deinit();
    return std.Io.Dir.cwd().readFileAlloc(threaded.io(), path, allocator, .limited(1 << 30));
}
pub const Schema = struct {
    ptr: *c.pbw_schema,

    /// Loads descriptor-set bytes; use `--include_imports` so every type resolves.
    pub fn fromBytes(bytes: []const u8) Error!Schema {
        var out: ?*c.pbw_schema = null;
        const st = c.pbw_descriptor_set_load(@ptrCast(dataPtr(bytes)), bytes.len, &out);
        try check(st);
        if (out == null) return Error.UnknownStatus;
        return .{ .ptr = @ptrCast(out) };
    }

    /// Loads a descriptor set written by `protoc --descriptor_set_out`.
    pub fn fromFile(allocator: std.mem.Allocator, path: []const u8) !Schema {
        const bytes = try readFile(allocator, path);
        defer allocator.free(bytes);
        return fromBytes(bytes);
    }

    /// Empty schema for hand-built descriptors (the conformance testee's
    /// request/response envelope; not for `.proto` schemas, which always
    /// come from a `FileDescriptorSet`).
    pub fn init() Error!Schema {
        const p = c.pbw_schema_new();
        if (p == null) return Error.OutOfMemory;
        return .{ .ptr = @ptrCast(p) };
    }

    pub fn deinit(self: *Schema) void {
        c.pbw_schema_free(self.ptr);
    }

    /// Looks up a message by fully qualified name, e.g. `"structtest.Inner3"`.
    pub fn messageType(self: *const Schema, name: []const u8) Error!MessageType {
        var nbuf: [512]u8 = undefined;
        try toNul(name, &nbuf);
        const desc = c.pbw_schema_find(self.ptr, nulPtr(&nbuf));
        if (desc == null) return Error.NotFound;
        return .{ .ptr = @ptrCast(desc) };
    }

    /// Appends a message descriptor (hand-built schemas only).
    pub fn addMessage(self: *Schema, name: []const u8) Error!MessageType {
        var nbuf: [512]u8 = undefined;
        try toNul(name, &nbuf);
        const desc = c.pbw_schema_add_message(self.ptr, nulPtr(&nbuf));
        if (desc == null) return Error.OutOfMemory;
        return .{ .ptr = @ptrCast(desc) };
    }

    /// Resolves type names and freezes the field layout; required before use.
    pub fn link(self: *Schema) Error!void {
        try check(c.pbw_schema_link(self.ptr));
    }

    pub fn lastError(self: *const Schema) [:0]const u8 {
        return zstr(c.pbw_schema_error(self.ptr));
    }
};

/// A message type from a `Schema`. A plain handle: no lifetime of its own,
/// but the `Schema` must outlive every message built from it.
pub const MessageType = struct {
    ptr: *c.pbw_descriptor,

    pub fn fullName(self: *const MessageType) [:0]const u8 {
        return zstr(c.pbw_descriptor_name(self.ptr));
    }

    /// Resolves a field once; reuse the handle in hot loops.
    pub fn field(self: *const MessageType, name: []const u8) Error!Field {
        var nbuf: [512]u8 = undefined;
        try toNul(name, &nbuf);
        const f = c.pbw_descriptor_find_field_name(self.ptr, nulPtr(&nbuf));
        if (f == null) return Error.NotFound;
        return .{ .ptr = @ptrCast(f) };
    }

    pub fn fieldByNumber(self: *const MessageType, number: u32) Error!Field {
        const f = c.pbw_descriptor_find_field(self.ptr, number);
        if (f == null) return Error.NotFound;
        return .{ .ptr = @ptrCast(f) };
    }

    /// A new empty message, freed with `deinit`.
    pub fn newMessage(self: *const MessageType) Error!Message {
        const p = c.pbw_message_new(self.ptr);
        if (p == null) return Error.OutOfMemory;
        return .{ .ptr = @ptrCast(p), .owned = true };
    }

    /// Decodes wire bytes; unknown fields are preserved and re-encoded.
    pub fn decode(self: *const MessageType, wire: []const u8) Error!Message {
        var out: ?*c.pbw_message = null;
        const st = c.pbw_decode(self.ptr, @ptrCast(dataPtr(wire)), wire.len, &out);
        try check(st);
        if (out == null) return Error.UnknownStatus;
        return .{ .ptr = @ptrCast(out), .owned = true };
    }

    /// Parses JSON mapping text into a new message tree.
    pub fn parseJson(self: *const MessageType, text: []const u8, opts: JsonOptions) Error!Message {
        var copts = c.pbw_json_parse_options{
            .ignore_unknown_fields = if (opts.ignore_unknown_fields) 1 else 0,
        };
        var out: ?*c.pbw_message = null;
        const st = c.pbw_json_parse_opts(self.ptr, @ptrCast(dataPtr(text)), text.len, &copts, &out);
        try check(st);
        if (out == null) return Error.UnknownStatus;
        return .{ .ptr = @ptrCast(out), .owned = true };
    }

    /// Parses text format into a new message tree.
    pub fn parseText(self: *const MessageType, text: []const u8) Error!Message {
        var out: ?*c.pbw_message = null;
        const st = c.pbw_text_parse(self.ptr, @ptrCast(dataPtr(text)), text.len, &out);
        try check(st);
        if (out == null) return Error.UnknownStatus;
        return .{ .ptr = @ptrCast(out), .owned = true };
    }

    /// Appends a field to a hand-built message descriptor.
    pub fn addField(self: *MessageType, name: []const u8, number: u32, ftype: c.pbw_type, label: c.pbw_label) Error!Field {
        var nbuf: [512]u8 = undefined;
        try toNul(name, &nbuf);
        const f = c.pbw_message_add_field(self.ptr, nulPtr(&nbuf), number, ftype, label);
        if (f == null) return Error.OutOfMemory;
        return .{ .ptr = @ptrCast(f) };
    }
};

pub const JsonOptions = struct {
    ignore_unknown_fields: bool = false,
};

/// A resolved field handle: the number, wire type and storage the schema says.
pub const Field = struct {
    ptr: *const c.pbw_field,

    pub fn number(self: Field) u32 {
        return c.pbw_field_number(self.ptr);
    }

    pub fn fieldName(self: Field) [:0]const u8 {
        return zstr(c.pbw_field_name(self.ptr));
    }

    /// `pbw_type`: `c.PBW_TYPE_INT32`, `c.PBW_TYPE_MESSAGE`, ...
    pub fn fieldType(self: Field) c.pbw_type {
        return c.pbw_field_type(self.ptr);
    }

    /// `pbw_label`: `c.PBW_LABEL_REPEATED` (3) for repeated fields.
    pub fn label(self: Field) c.pbw_label {
        return c.pbw_field_label(self.ptr);
    }
};

/// Exact bytes `encode` would write, without writing them: for callers that
/// must size a buffer (a length-prefixed frame, `encodeInto`) up front.
pub fn encodedSizeOf(msg: *const Message) Error!usize {
    var len: usize = 0;
    try check(c.pbw_message_encoded_size(msg.ptr, &len));
    return len;
}

/// Slack `encodeInto` needs past `encodedSizeOf`: the writer over-reserves tag
/// and varint room, so `size + encode_slack` is the smallest cap guaranteed
/// to fit, while `size` alone may be rejected.
pub const encode_slack: usize = c.PBW_ENCODE_SLACK;

/// One dynamic message. A root owns its tree; a nested message borrows it.
pub const Message = struct {
    ptr: *c.pbw_message,
    owned: bool = true,

    pub fn deinit(self: *Message) void {
        if (self.owned) c.pbw_message_free(self.ptr);
    }

    /// Any integer or enum field; the value is stored as raw 64-bit bits.
    pub fn setI64(self: *Message, f: Field, v: i64) Error!void {
        try self.store(f, c.pbw_value_new_int(v));
    }

    pub fn setBool(self: *Message, f: Field, v: bool) Error!void {
        try self.store(f, c.pbw_value_new_bool(@intFromBool(v)));
    }

    pub fn setF32(self: *Message, f: Field, v: f32) Error!void {
        try self.store(f, c.pbw_value_new_float(v));
    }

    pub fn setF64(self: *Message, f: Field, v: f64) Error!void {
        try self.store(f, c.pbw_value_new_double(v));
    }

    /// Rejects strings containing a NUL byte (it would truncate on the way
    /// to C); use `setBytes` for binary payloads, including on a `string`
    /// field.
    pub fn setStr(self: *Message, f: Field, v: []const u8) Error!void {
        if (std.mem.indexOfScalar(u8, v, 0) != null) return Error.InvalidArgument;
        try self.store(f, c.pbw_value_new_string(@ptrCast(dataPtr(v))));
    }

    pub fn setBytes(self: *Message, f: Field, v: []const u8) Error!void {
        try self.store(f, c.pbw_value_new_bytes(@ptrCast(dataPtr(v)), v.len));
    }

    /// Nested message for a MESSAGE field, created in place (or returned as
    /// it is). Borrows `self`: must not outlive it.
    pub fn setMessage(self: *Message, f: Field) Error!Message {
        const p = c.pbw_message_set_message(self.ptr, f.number());
        if (p == null) return Error.WrongType;
        return .{ .ptr = @ptrCast(p), .owned = false };
    }

    /// Stores an owned `Value`, appending for repeated fields and replacing
    /// otherwise. Takes ownership on success; frees the value on failure, so
    /// the caller never leaks a rejected value.
    pub fn set(self: *Message, number: u32, v: Value) Error!void {
        const st = c.pbw_message_set(self.ptr, number, v.ptr);
        if (st != c.PBW_OK) c.pbw_value_free(v.ptr);
        try check(st);
    }

    /// Appends to a repeated field or replaces a singular one.
    fn store(self: *Message, f: Field, raw: ?*c.pbw_value) Error!void {
        const v = raw orelse return Error.OutOfMemory;
        const st = c.pbw_message_set_field(self.ptr, f.ptr, v);
        if (st != c.PBW_OK) {
            // Rejected values stay owned by the caller.
            c.pbw_value_free(v);
            return check(st);
        }
    }

    pub fn clear(self: *Message, f: Field) void {
        c.pbw_message_clear(self.ptr, f.number());
    }

    pub fn value(self: *const Message, f: Field) ?Value {
        const p = c.pbw_message_get_field(self.ptr, f.ptr);
        if (p == null) return null;
        return .{ .ptr = @ptrCast(p), .owned = false };
    }

    pub fn getByNumber(self: *const Message, number: u32) ?Value {
        const p = c.pbw_message_get(self.ptr, number);
        if (p == null) return null;
        return .{ .ptr = @ptrCast(p), .owned = false };
    }

    pub fn has(self: *const Message, f: Field) bool {
        return self.value(f) != null;
    }

    pub fn getI64(self: *const Message, f: Field) i64 {
        const v = self.value(f) orelse return 0;
        return c.pbw_value_get_int(v.ptr);
    }

    pub fn getBool(self: *const Message, f: Field) bool {
        const v = self.value(f) orelse return false;
        return c.pbw_value_get_bool(v.ptr) != 0;
    }

    pub fn getF32(self: *const Message, f: Field) f32 {
        const v = self.value(f) orelse return 0.0;
        return c.pbw_value_get_float(v.ptr);
    }

    pub fn getF64(self: *const Message, f: Field) f64 {
        const v = self.value(f) orelse return 0.0;
        return c.pbw_value_get_double(v.ptr);
    }

    pub fn getStr(self: *const Message, f: Field) ?[]const u8 {
        const v = self.value(f) orelse return null;
        return v.asStr();
    }

    pub fn getBytes(self: *const Message, f: Field) ?[]const u8 {
        const v = self.value(f) orelse return null;
        return v.asBytes();
    }

    pub fn getMessage(self: *const Message, f: Field) ?Message {
        const v = self.value(f) orelse return null;
        return v.asMessage();
    }

    /// Wire length this tree was decoded from, 0 for a tree built by hand: a
    /// buffer-size hint, not a size calculator (see `encodedSizeOf`).
    pub fn wireSize(self: *const Message) usize {
        return c.pbw_message_wire_size(self.ptr);
    }

    /// True when every proto2 `required` field is present.
    pub fn isInitialized(self: *const Message) bool {
        return c.pbw_message_is_initialized(self.ptr) != 0;
    }

    /// Encodes into a fresh slice owned by `allocator`.
    pub fn encode(self: *const Message, allocator: std.mem.Allocator) ![]u8 {
        var out: [*c]u8 = null;
        var len: usize = 0;
        try check(c.pbw_encode(self.ptr, @ptrCast(&out), &len));
        if (len == 0 or out == null) {
            if (out != null) c.pbw_free(@ptrCast(out));
            return try allocator.dupe(u8, &[_]u8{});
        }
        defer c.pbw_free(@ptrCast(out));
        return try allocator.dupe(u8, @as([*]const u8, @ptrCast(out))[0..len]);
    }

    /// Encodes into caller memory. `buf` must hold
    /// `encodedSizeOf(msg) + encode_slack` bytes; returns the bytes written.
    pub fn encodeInto(self: *const Message, buf: []u8) Error![]u8 {
        var len: usize = 0;
        const st = c.pbw_encode_to(self.ptr, dataPtrMut(buf), buf.len, &len);
        if (st == c.PBW_ERR_LIMIT) return Error.BufferTooSmall;
        try check(st);
        return buf[0..len];
    }

    /// Encodes with map entries sorted by key, so equal messages encode to
    /// equal bytes regardless of insertion order.
    pub fn encodeDeterministic(self: *const Message, allocator: std.mem.Allocator) ![]u8 {
        var out: [*c]u8 = null;
        var len: usize = 0;
        try check(c.pbw_encode_deterministic(self.ptr, @ptrCast(&out), &len));
        if (len == 0 or out == null) {
            if (out != null) c.pbw_free(@ptrCast(out));
            return try allocator.dupe(u8, &[_]u8{});
        }
        defer c.pbw_free(@ptrCast(out));
        return try allocator.dupe(u8, @as([*]const u8, @ptrCast(out))[0..len]);
    }

    /// Prints this message in protobuf JSON mapping. Unknown fields are not
    /// printed, matching the reference printer.
    pub fn printJson(self: *const Message, allocator: std.mem.Allocator) ![]u8 {
        var out: [*c]u8 = null;
        var len: usize = 0;
        try check(c.pbw_json_print(self.ptr, @ptrCast(&out), &len));
        if (len == 0 or out == null) {
            if (out != null) c.pbw_free(@ptrCast(out));
            return try allocator.dupe(u8, &[_]u8{});
        }
        defer c.pbw_free(@ptrCast(out));
        return try allocator.dupe(u8, @as([*]const u8, @ptrCast(out))[0..len]);
    }

    /// Prints this message in protobuf text format.
    pub fn printText(self: *const Message, allocator: std.mem.Allocator, print_unknown_fields: bool) ![]u8 {
        var out: [*c]u8 = null;
        var len: usize = 0;
        try check(c.pbw_text_print_opts(self.ptr, @intFromBool(print_unknown_fields), @ptrCast(&out), &len));
        if (len == 0 or out == null) {
            if (out != null) c.pbw_free(@ptrCast(out));
            return try allocator.dupe(u8, &[_]u8{});
        }
        defer c.pbw_free(@ptrCast(out));
        return try allocator.dupe(u8, @as([*]const u8, @ptrCast(out))[0..len]);
    }
};

/// A field value: what `Message.value` hands back, typed by `kind`.
pub const Value = struct {
    ptr: *c.pbw_value,
    owned: bool = false,

    pub fn deinit(self: *Value) void {
        if (self.owned) c.pbw_value_free(self.ptr);
    }

    pub fn newInt(value: i64) Error!Value {
        const p = c.pbw_value_new_int(value);
        if (p == null) return Error.OutOfMemory;
        return .{ .ptr = @ptrCast(p), .owned = true };
    }

    pub fn newBool(value: bool) Error!Value {
        const p = c.pbw_value_new_bool(@intFromBool(value));
        if (p == null) return Error.OutOfMemory;
        return .{ .ptr = @ptrCast(p), .owned = true };
    }

    pub fn newFloat(value: f32) Error!Value {
        const p = c.pbw_value_new_float(value);
        if (p == null) return Error.OutOfMemory;
        return .{ .ptr = @ptrCast(p), .owned = true };
    }

    pub fn newDouble(value: f64) Error!Value {
        const p = c.pbw_value_new_double(value);
        if (p == null) return Error.OutOfMemory;
        return .{ .ptr = @ptrCast(p), .owned = true };
    }

    pub fn newString(text: []const u8) Error!Value {
        if (std.mem.indexOfScalar(u8, text, 0) != null) return Error.InvalidArgument;
        const p = c.pbw_value_new_string(@ptrCast(dataPtr(text)));
        if (p == null) return Error.OutOfMemory;
        return .{ .ptr = @ptrCast(p), .owned = true };
    }

    pub fn newBytes(bytes: []const u8) Error!Value {
        const p = c.pbw_value_new_bytes(@ptrCast(dataPtr(bytes)), bytes.len);
        if (p == null) return Error.OutOfMemory;
        return .{ .ptr = @ptrCast(p), .owned = true };
    }

    /// `pbw_value_kind`: 0 numeric bits, 1 bytes/string, 2 message, 3 list.
    pub fn kind(self: *const Value) c.pbw_type {
        return c.pbw_value_get_kind(self.ptr);
    }

    pub fn asI64(self: *const Value) i64 {
        return c.pbw_value_get_int(self.ptr);
    }

    pub fn asBool(self: *const Value) bool {
        return c.pbw_value_get_bool(self.ptr) != 0;
    }

    pub fn asF32(self: *const Value) f32 {
        return c.pbw_value_get_float(self.ptr);
    }

    pub fn asF64(self: *const Value) f64 {
        return c.pbw_value_get_double(self.ptr);
    }

    pub fn asBytes(self: *const Value) ?[]const u8 {
        var n: usize = 0;
        const p = c.pbw_value_get_bytes(self.ptr, &n);
        if (p == null) return null;
        if (n == 0) return &[_]u8{};
        return @as([*]const u8, @ptrCast(p))[0..n];
    }

    /// String payload, or null when absent, non-UTF-8, or not a string field.
    pub fn asStr(self: *const Value) ?[]const u8 {
        const b = self.asBytes() orelse return null;
        if (!std.unicode.utf8ValidateSlice(b)) return null;
        return b;
    }

    pub fn asMessage(self: *const Value) ?Message {
        const p = c.pbw_value_get_message(self.ptr);
        if (p == null) return null;
        return .{ .ptr = @ptrCast(p), .owned = false };
    }

    /// Items in a repeated field (1 for a singular value).
    pub fn len(self: *const Value) usize {
        return c.pbw_value_count(self.ptr);
    }

    pub fn isEmpty(self: *const Value) bool {
        return self.len() == 0;
    }

    pub fn at(self: *const Value, index: usize) ?Value {
        const p = c.pbw_value_at(self.ptr, index);
        if (p == null) return null;
        return .{ .ptr = @ptrCast(p), .owned = false };
    }
};

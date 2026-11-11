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
        return fromBytesLimits(bytes, null);
    }

    /// Same with explicit bounds: rejects inputs larger than
    /// `limits.max_bytes` and caps nesting at `limits.max_depth`.
    pub fn fromBytesLimits(bytes: []const u8, limits: ?Limits) Error!Schema {
        var out: ?*c.pbw_schema = null;
        var storage: c.pbw_limits = undefined;
        const st = c.pbw_descriptor_set_load_limits(@ptrCast(dataPtr(bytes)), bytes.len, limitsPtr(limits, &storage), &out);
        if (st != c.PBW_OK) {
            // The core hands back the partially built schema on failure so the
            // error text can be read; the caller must free it as usual.
            if (out) |p| c.pbw_schema_free(p);
            try check(st);
            return Error.UnknownStatus; // unreachable: a failed status always errors
        }
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

    /// Resolve `service`/`method` to its input/output descriptors for
    /// routing. `Error.NotFound` when anything is unknown.
    pub fn resolve(self: *const Schema, svc: []const u8, method: []const u8) Error!struct { input: MessageType, output: MessageType } {
        var sbuf: [512]u8 = undefined;
        var mbuf: [512]u8 = undefined;
        try toNul(svc, &sbuf);
        try toNul(method, &mbuf);
        var in_desc: ?*const c.pbw_descriptor = null;
        var out_desc: ?*const c.pbw_descriptor = null;
        const st = c.pbw_service_resolve(self.ptr, nulPtr(&sbuf), nulPtr(&mbuf), &in_desc, &out_desc);
        try check(st);
        const ind = in_desc orelse return Error.NotFound;
        const outd = out_desc orelse return Error.NotFound;
        return .{
            .input = .{ .ptr = @constCast(ind) },
            .output = .{ .ptr = @constCast(outd) },
        };
    }

    pub fn lastError(self: *const Schema) [:0]const u8 {
        return zstr(c.pbw_schema_error(self.ptr));
    }

    /// Services loaded from the descriptor set (routing data only; the core
    /// does no RPC). Hand-built schemas carry none.
    pub fn serviceCount(self: *const Schema) usize {
        return c.pbw_schema_service_count(self.ptr);
    }

    pub fn serviceAt(self: *const Schema, index: usize) ?Service {
        const s = c.pbw_schema_service_at(self.ptr, index);
        if (s == null) return null;
        return .{ .ptr = @ptrCast(s) };
    }

    pub fn service(self: *const Schema, name: []const u8) Error!Service {
        var nbuf: [512]u8 = undefined;
        try toNul(name, &nbuf);
        const s = c.pbw_schema_service_find(self.ptr, nulPtr(&nbuf));
        if (s == null) return Error.NotFound;
        return .{ .ptr = @ptrCast(s) };
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
        return decodeLimits(self, wire, null);
    }

    /// Same with explicit bounds (`max_bytes` rejects oversized inputs,
    /// `max_depth` caps nesting).
    pub fn decodeLimits(self: *const MessageType, wire: []const u8, limits: ?Limits) Error!Message {
        var out: ?*c.pbw_message = null;
        var storage: c.pbw_limits = undefined;
        const st = c.pbw_decode_limits(self.ptr, @ptrCast(dataPtr(wire)), wire.len, limitsPtr(limits, &storage), &out);
        try check(st);
        if (out == null) return Error.UnknownStatus;
        return .{ .ptr = @ptrCast(out), .owned = true };
    }

    /// Parses JSON mapping text into a new message tree.
    pub fn parseJson(self: *const MessageType, text: []const u8, opts: JsonOptions) Error!Message {
        return parseJsonLimits(self, text, opts, null);
    }

    /// Same with explicit bounds (either pointer may be null for defaults).
    pub fn parseJsonLimits(self: *const MessageType, text: []const u8, opts: ?JsonOptions, limits: ?Limits) Error!Message {
        const o: JsonOptions = opts orelse .{};
        var copts = c.pbw_json_parse_options{
            .ignore_unknown_fields = if (o.ignore_unknown_fields) 1 else 0,
        };
        var out: ?*c.pbw_message = null;
        var storage: c.pbw_limits = undefined;
        const st = c.pbw_json_parse_limits(self.ptr, @ptrCast(dataPtr(text)), text.len, limitsPtr(limits, &storage), &copts, &out);
        try check(st);
        if (out == null) return Error.UnknownStatus;
        return .{ .ptr = @ptrCast(out), .owned = true };
    }

    /// Parses text format into a new message tree.
    pub fn parseText(self: *const MessageType, text: []const u8) Error!Message {
        return parseTextLimits(self, text, null);
    }

    /// Same with explicit bounds.
    pub fn parseTextLimits(self: *const MessageType, text: []const u8, limits: ?Limits) Error!Message {
        var out: ?*c.pbw_message = null;
        var storage: c.pbw_limits = undefined;
        const st = c.pbw_text_parse_limits(self.ptr, @ptrCast(dataPtr(text)), text.len, limitsPtr(limits, &storage), &out);
        try check(st);
        if (out == null) return Error.UnknownStatus;
        return .{ .ptr = @ptrCast(out), .owned = true };
    }

    /// Single-level scan of `wire` without building a message tree: one
    /// validating tag pass, one event per field, no allocation by the core.
    /// Events borrow `wire`; free the list (not the payloads) with `free`.
    pub fn visitCollect(self: *const MessageType, allocator: std.mem.Allocator, wire: []const u8, limits: ?Limits) Error![]VisitEvent {
        var storage: c.pbw_limits = undefined;
        var list: std.ArrayList(VisitEvent) = .empty;
        errdefer list.deinit(allocator);
        var state = VisitState{ .list = &list, .alloc = allocator };
        const st = c.pbw_visit_limits(self.ptr, dataPtr(wire), wire.len, limitsPtr(limits, &storage), visitCb, &state, null);
        try check(st);
        return list.toOwnedSlice(allocator);
    }

    /// Decode framed with `encodeDelimited`: the message plus the prefix +
    /// frame bytes consumed, so concatenated frames decode in a loop.
    pub fn decodeDelimited(self: *const MessageType, wire: []const u8, limits: ?Limits) Error!Delimited {
        var out: ?*c.pbw_message = null;
        var consumed: usize = 0;
        var storage: c.pbw_limits = undefined;
        const st = c.pbw_decode_delimited_limits(self.ptr, dataPtr(wire), wire.len, limitsPtr(limits, &storage), &out, &consumed);
        try check(st);
        if (out == null) return Error.UnknownStatus;
        return .{ .message = .{ .ptr = @ptrCast(out), .owned = true }, .consumed = consumed };
    }

    /// Pack already-encoded `payload` (bytes of the message named by
    /// `type_url`, e.g. `"type.googleapis.com/pkg.Msg"`) into a new `Any`
    /// tree of this type.
    pub fn anyPack(self: *const MessageType, type_url: []const u8, payload: []const u8) Error!Message {
        var nbuf: [512]u8 = undefined;
        try toNul(type_url, &nbuf);
        var out: ?*c.pbw_message = null;
        try check(c.pbw_any_pack(self.ptr, nulPtr(&nbuf), dataPtr(payload), payload.len, &out));
        if (out == null) return Error.UnknownStatus;
        return .{ .ptr = @ptrCast(out), .owned = true };
    }

    /// A new `Timestamp`/`Duration` tree of this type holding `ms`. Nanos
    /// keep the millisecond remainder, so the value round-trips.
    pub fn timestampFromMillis(self: *const MessageType, ms: i64) Error!Message {
        var out: ?*c.pbw_message = null;
        try check(c.pbw_timestamp_from_millis(self.ptr, ms, &out));
        if (out == null) return Error.UnknownStatus;
        return .{ .ptr = @ptrCast(out), .owned = true };
    }

    pub fn durationFromMillis(self: *const MessageType, ms: i64) Error!Message {
        var out: ?*c.pbw_message = null;
        try check(c.pbw_duration_from_millis(self.ptr, ms, &out));
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
/// A resolved field handle. The pointer lives in the schema, so the owning
/// `Schema` (normally reached through the `MessageType`) must outlive it.
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

    /// Length-delimited framing (varint length + message), the standard
    /// framing for a stream of messages.
    pub fn encodeDelimited(self: *const Message, allocator: std.mem.Allocator) ![]u8 {
        var out: [*c]u8 = null;
        var len: usize = 0;
        try check(c.pbw_encode_delimited(self.ptr, @ptrCast(&out), &len));
        if (len == 0 or out == null) {
            if (out != null) c.pbw_free(@ptrCast(out));
            return try allocator.dupe(u8, &[_]u8{});
        }
        defer c.pbw_free(@ptrCast(out));
        return try allocator.dupe(u8, @as([*]const u8, @ptrCast(out))[0..len]);
    }

    /// Framing into caller memory: no allocation, `buf` is the hard limit.
    /// Returns the whole frame; `Error.BufferTooSmall` when it does not fit.
    pub fn encodeDelimitedInto(self: *const Message, buf: []u8) Error![]u8 {
        var len: usize = 0;
        const st = c.pbw_encode_delimited_to(self.ptr, dataPtrMut(buf), buf.len, &len);
        if (st == c.PBW_ERR_LIMIT) return Error.BufferTooSmall;
        try check(st);
        return buf[0..len];
    }

    /// Encode and hand the bytes to `sink` in 8 KiB chunks, without
    /// materializing the whole message. The sink gets `(ctx, data, len)` and
    /// returns `PBW_OK` to continue; anything else aborts with that status.
    pub fn encodeSink(self: *const Message, ctx: ?*anyopaque, sink: *const fn ([*c]const u8, usize, ?*anyopaque) callconv(.c) c.pbw_status) Error!void {
        try check(c.pbw_encode_sink(self.ptr, @ptrCast(sink), ctx));
    }

    /// `type_url` (field 1) of an `Any` tree, or null when absent. Borrowed.
    pub fn anyTypeUrl(self: *const Message) ?[]const u8 {
        const p = c.pbw_any_type_url(self.ptr);
        if (p == null) return null;
        return std.mem.span(@as([*:0]const u8, @ptrCast(p)));
    }

    /// Value bytes (field 2) of an `Any` tree, or null when absent. Borrowed.
    pub fn anyPayload(self: *const Message) ?[]const u8 {
        var len: usize = 0;
        const p = c.pbw_any_payload(self.ptr, &len);
        if (p == null) return null;
        if (len == 0) return &[_]u8{};
        return @as([*]const u8, @ptrCast(p))[0..len];
    }

    /// This `Timestamp`/`Duration` tree as milliseconds.
    pub fn timestampMillis(self: *const Message) Error!i64 {
        var out: i64 = 0;
        try check(c.pbw_timestamp_to_millis(self.ptr, &out));
        return out;
    }

    pub fn durationMillis(self: *const Message) Error!i64 {
        var out: i64 = 0;
        try check(c.pbw_duration_to_millis(self.ptr, &out));
        return out;
    }

    /// This `FieldMask`'s paths (field 1, repeated string) without
    /// materializing the list.
    pub fn fieldmaskPathCount(self: *const Message) usize {
        return c.pbw_fieldmask_path_count(self.ptr);
    }

    pub fn fieldmaskPathAt(self: *const Message, index: usize) ?[]const u8 {
        const p = c.pbw_fieldmask_path_at(self.ptr, index);
        if (p == null) return null;
        return std.mem.span(@as([*:0]const u8, @ptrCast(p)));
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

/// Bounds for untrusted input. Zero fields mean the core defaults: no byte
/// cap and `PBW_MAX_DEPTH` nesting. A `max_depth` above the cap clamps to it.
pub const Limits = struct {
    max_bytes: usize = 0,
    max_depth: u32 = 0,

    fn toC(self: *const Limits, storage: *c.pbw_limits) *const c.pbw_limits {
        storage.* = .{ .max_bytes = self.max_bytes, .max_depth = self.max_depth };
        return storage;
    }
};

fn limitsPtr(limits: ?Limits, storage: *c.pbw_limits) ?*const c.pbw_limits {
    if (limits) |*l| return l.toC(storage);
    return null;
}

/// One scanned field: raw wire content, no allocation, no value tree.
/// `wire_type` is the numeric wire type (0 varint, 1 64-bit, 2 LEN,
/// 3/4 start/end group, 5 32-bit). Scalars arrive as `u64` (varints unfolded:
/// no zigzag or sign extension); LEN payloads and whole group fields arrive
/// as `data` (a nested message, a packed run, a string, or raw group bytes).
pub const VisitEvent = struct {
    number: u32,
    wire_type: u32,
    /// Null when the schema does not declare the number.
    field: ?Field,
    u64: u64,
    data: []const u8,
};

const VisitState = struct {
    list: *std.ArrayList(VisitEvent),
    alloc: std.mem.Allocator,
};

fn visitCb(ud: ?*anyopaque, field: ?*const c.pbw_field, number: u32, wire_type: u32, value: ?*const c.pbw_visit_value) callconv(.c) c.pbw_status {
    const v = value orelse return c.PBW_ERR_INVALID;
    const st: *VisitState = @ptrCast(@alignCast(ud));
    var ev = VisitEvent{
        .number = number,
        .wire_type = wire_type,
        .field = if (field) |f| Field{ .ptr = f } else null,
        .u64 = v.u64,
        .data = &.{} ,
    };
    if (v.len > 0 and v.data != null) {
        ev.data = @as([*]const u8, @ptrCast(v.data))[0..v.len];
    }
    st.list.append(st.alloc, ev) catch return c.PBW_ERR_NOMEM;
    return c.PBW_OK;
}

/// Decode framed with `encodeDelimited`: the message plus the prefix + frame
/// bytes consumed, so concatenated frames decode in a loop.
pub const Delimited = struct {
    message: Message,
    consumed: usize,
};

/// A declared RPC: fully qualified input/output message names plus the two
/// streaming flags from `MethodDescriptorProto` (both 0 is unary).
pub const Method = struct {
    ptr: *const c.pbw_method,

    pub fn methodName(self: Method) [:0]const u8 {
        return zstr(c.pbw_method_name(self.ptr));
    }

    pub fn inputType(self: Method) [:0]const u8 {
        return zstr(c.pbw_method_input_type(self.ptr));
    }

    pub fn outputType(self: Method) [:0]const u8 {
        return zstr(c.pbw_method_output_type(self.ptr));
    }

    pub fn clientStreaming(self: Method) bool {
        return c.pbw_method_client_streaming(self.ptr) != 0;
    }

    pub fn serverStreaming(self: Method) bool {
        return c.pbw_method_server_streaming(self.ptr) != 0;
    }
};

/// A loaded service: routing data only, the core does no RPC. A transport
/// (gRPC, Twirp, plain HTTP) decodes with the input type and encodes with
/// the output type.
pub const Service = struct {
    ptr: *const c.pbw_service,

    pub fn serviceName(self: Service) [:0]const u8 {
        return zstr(c.pbw_service_name(self.ptr));
    }

    pub fn methodCount(self: Service) usize {
        return c.pbw_service_method_count(self.ptr);
    }

    pub fn methodAt(self: Service, index: usize) ?Method {
        const m = c.pbw_service_method_at(self.ptr, index);
        if (m == null) return null;
        return .{ .ptr = @ptrCast(m) };
    }

    pub fn method(self: Service, name: []const u8) Error!Method {
        var nbuf: [512]u8 = undefined;
        try toNul(name, &nbuf);
        const m = c.pbw_service_method_find(self.ptr, nulPtr(&nbuf));
        if (m == null) return Error.NotFound;
        return .{ .ptr = @ptrCast(m) };
    }
};

/// A reusable arena plus encode buffer: steady encode/decode in a loop stops
/// calling malloc after the first few messages. One context per thread; never
/// share one concurrently. Decoded trees borrow the context: they die with
/// `reset` and are read-only (`deinit` on them is a no-op, writes fail).
pub const Context = struct {
    ptr: *c.pbw_ctx,

    pub fn init() Error!Context {
        const p = c.pbw_ctx_new();
        if (p == null) return Error.OutOfMemory;
        return .{ .ptr = @ptrCast(p) };
    }

    pub fn deinit(self: *Context) void {
        c.pbw_ctx_free(self.ptr);
    }

    /// Release every allocation since the last reset, keeping the buffers.
    pub fn reset(self: *Context) void {
        c.pbw_ctx_reset(self.ptr);
    }

    /// Max bytes the context keeps resident across resets (default 1 MiB).
    pub fn setRetainCap(self: *Context, cap: usize) void {
        c.pbw_ctx_set_retain_cap(self.ptr, cap);
    }

    /// Bytes currently held in the context (for sizing/debugging).
    pub fn used(self: *const Context) usize {
        return c.pbw_ctx_used(self.ptr);
    }

    /// Decode into the arena. The tree borrows the context: it dies with the
    /// next `reset` and must not outlive the context.
    pub fn decode(self: *Context, ty: *const MessageType, wire: []const u8) Error!Message {
        var out: ?*c.pbw_message = null;
        const st = c.pbw_decode_ctx(self.ptr, ty.ptr, dataPtr(wire), wire.len, &out);
        try check(st);
        if (out == null) return Error.UnknownStatus;
        return .{ .ptr = @ptrCast(out), .owned = false };
    }

    /// Encode into the context's buffer. The slice borrows the context:
    /// valid until the next encode on it, `reset`, or `deinit`.
    pub fn encodeBuf(self: *Context, msg: *const Message) Error![]const u8 {
        var out: [*c]const u8 = null;
        var len: usize = 0;
        try check(c.pbw_encode_ctx(self.ptr, msg.ptr, &out, &len));
        if (len == 0) return &[_]u8{};
        return @as([*]const u8, @ptrCast(out))[0..len];
    }
};

/// Bounded freelist of contexts: `acquire` instead of `Context.init`,
/// `release` instead of `reset`+`free`. A released context is already reset.
/// Unsynchronized: one pool per thread, never shared concurrently.
pub const Pool = struct {
    ptr: *c.pbw_pool,

    pub fn init(max_idle: usize, retain_cap: usize) Error!Pool {
        const p = c.pbw_pool_new(max_idle, retain_cap);
        if (p == null) return Error.OutOfMemory;
        return .{ .ptr = @ptrCast(p) };
    }

    pub fn deinit(self: *Pool) void {
        c.pbw_pool_free(self.ptr);
    }

    /// Exclusive use until `release`. The context is already reset.
    pub fn acquire(self: *Pool) Error!Context {
        const p = c.pbw_pool_acquire(self.ptr);
        if (p == null) return Error.OutOfMemory;
        return .{ .ptr = @ptrCast(p) };
    }

    pub fn release(self: *Pool, ctx: *Context) void {
        c.pbw_pool_release(self.ptr, ctx.ptr);
        ctx.ptr = @ptrFromInt(0x1);
    }

    /// Idle contexts held now (for sizing/debugging).
    pub fn idle(self: *const Pool) usize {
        return c.pbw_pool_idle(self.ptr);
    }
};

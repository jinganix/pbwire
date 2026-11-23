//! The pure-Zig wire core: a `FileDescriptorSet` reader, a value tree, wire
//! primitives and a binary encoder/decoder, with no C library, no `@cImport`,
//! no translate-c and no generated code.
//!
//! It is **not** a replacement for the binding's default codec, which links the
//! C core statically. It is the `core` arm of the pbench three-arm contract and
//! the mode a build with no C sources uses:
//!
//! ```zig
//! var schema = try Schema.fromFile(alloc, "matrix3.desc");
//! defer schema.deinit();
//! const ty = try schema.messageType("structtest.Scalars3");
//! var msg = try ty.newMessage(alloc);
//! defer msg.deinit();
//! try msg.setStr("str", "hello");
//! const wire = try ty.encode(alloc, &msg);
//! defer alloc.free(wire);
//! var back = try ty.decode(alloc, wire);
//! defer back.deinit();
//! ```
//!
//! The decode side keeps unknown fields (in input order, tag and payload
//! verbatim), validates groups to their matching end tag, accepts packed runs
//! for every numeric kind, collapses oneofs to the last member, follows each
//! field's own presence rule and enforces the nesting cap.
//!
//! This mirrors `bindings/rust/src/core/`, `bindings/swift/Sources/PbwireCore/`,
//! `bindings/python/pure/`, `bindings/node/lib/core/`, `bindings/dart/lib/core/`
//! and `bindings/julia/runtime/src/core/`.

const std = @import("std");
const descriptor = @import("descriptor.zig");
const tree = @import("tree.zig");
const wire = @import("wire.zig");
const enc = @import("encode.zig");
const dec = @import("decode.zig");

pub const Kind = descriptor.Kind;
pub const Presence = descriptor.Presence;
pub const Limits = descriptor.Limits;
pub const Field = descriptor.Field;
pub const MsgDef = descriptor.MsgDef;
pub const EnumDef = descriptor.EnumDef;
pub const CoreSchema = descriptor.CoreSchema;
pub const Value = tree.Value;
pub const Message = tree.Message;
pub const Error = wire.Error;
pub const MAX_DEPTH = wire.MAX_DEPTH;
pub const WT_VARINT = wire.WT_VARINT;
pub const WT_FIXED64 = wire.WT_FIXED64;
pub const WT_LEN = wire.WT_LEN;
pub const WT_START_GROUP = wire.WT_START_GROUP;
pub const WT_END_GROUP = wire.WT_END_GROUP;
pub const WT_FIXED32 = wire.WT_FIXED32;

/// Reads a whole file through the explicit Zig `Io` API.
pub fn readFile(allocator: std.mem.Allocator, path: []const u8) ![]u8 {
    var threaded: std.Io.Threaded = .init(allocator, .{});
    defer threaded.deinit();
    return std.Io.Dir.cwd().readFileAlloc(threaded.io(), path, allocator, .limited(1 << 30));
}

/// A loaded `FileDescriptorSet`: the runtime schema every type comes from.
pub const Schema = struct {
    core: *CoreSchema,

    pub fn fromBytes(alloc: std.mem.Allocator, bytes: []const u8) !Schema {
        return fromBytesLimits(alloc, bytes, .{});
    }

    pub fn fromBytesLimits(alloc: std.mem.Allocator, bytes: []const u8, limits: Limits) !Schema {
        const core = descriptor.loadSchema(alloc, bytes, limits) catch return Error.InvalidData;
        return .{ .core = core };
    }

    pub fn fromFile(alloc: std.mem.Allocator, path: []const u8) !Schema {
        const bytes = try readFile(alloc, path);
        defer alloc.free(bytes);
        return fromBytes(alloc, bytes);
    }

    pub fn deinit(self: *Schema) void {
        self.core.destroy();
    }

    pub fn messageType(self: *const Schema, name: []const u8) !MessageType {
        return .{ .core = self.core, .def = try self.core.messageIndex(name) };
    }

    pub fn messageIndex(self: *const Schema, name: []const u8) !usize {
        return self.core.messageIndex(name);
    }

    pub fn hasMessage(self: *const Schema, name: []const u8) bool {
        return self.core.message_by_name.contains(name);
    }
};

/// One message type: `newMessage` plus binary decode/encode.
pub const MessageType = struct {
    core: *const CoreSchema,
    def: usize,

    pub fn name(self: *const MessageType) []const u8 {
        return self.core.messages[self.def].full_name;
    }

    pub fn defn(self: *const MessageType) *const MsgDef {
        return &self.core.messages[self.def];
    }

    pub fn fields(self: *const MessageType) []const Field {
        return self.defn().fields;
    }

    pub fn field(self: *const MessageType, field_name: []const u8) !*const Field {
        const slot = self.defn().by_name.get(field_name) orelse return Error.NotFound;
        return &self.defn().fields[slot];
    }

    pub fn newMessage(self: *const MessageType, alloc: std.mem.Allocator) !Message {
        return Message.newRoot(alloc, self.core, self.def);
    }

    pub fn decode(self: *const MessageType, alloc: std.mem.Allocator, data: []const u8) !Message {
        return dec.decode(alloc, self.core, self.def, data, .{});
    }

    pub fn decodeWith(self: *const MessageType, alloc: std.mem.Allocator, data: []const u8, limits: Limits) !Message {
        return dec.decode(alloc, self.core, self.def, data, limits);
    }

    pub fn decodeSlice(self: *const MessageType, alloc: std.mem.Allocator, data: []const u8, off: usize, length: usize, limits: Limits) !Message {
        return dec.decodeSlice(alloc, self.core, self.def, data, off, length, limits);
    }

    pub fn encode(self: *const MessageType, alloc: std.mem.Allocator, m: *const Message) ![]u8 {
        _ = self;
        return enc.encode(alloc, m);
    }

    pub fn encodeTo(self: *const MessageType, m: *const Message, buf: *std.ArrayListUnmanaged(u8), alloc: std.mem.Allocator) !void {
        _ = self;
        return enc.encodeTo(m, buf, alloc);
    }
};

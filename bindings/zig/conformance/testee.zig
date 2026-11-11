//! Conformance testee for the official Protobuf conformance suite (binary wire
//! format plus JSON and text mappings; JSPB answers `skipped`, like the C
//! testee's JSPB branch).
//!
//! It speaks the runner's wire protocol (4-byte little-endian length prefix,
//! then a ConformanceRequest / ConformanceResponse) and, because the binding
//! is schema-driven, it needs no per-message code: the request names the
//! message type, the descriptor set answers with the schema.
//!
//! The two envelope messages are hand-built descriptors (three fields wide in
//! spirit, mirroring `conformance/testee.c`); the message under test always
//! comes from the descriptor set given on the command line.
//!
//!   zig build testee   # installs zig-out/bin/testee-zig
//!   testee-zig <descriptor-set>   # from the repo root, like testee.c
const std = @import("std");
const pbwire = @import("pbwire");

const WIRE_PROTOBUF: i64 = 1;
const WIRE_JSON: i64 = 2;
const WIRE_TEXT: i64 = 4;
const CATEGORY_JSON_IGNORE_UNKNOWN: i64 = 3;

const FieldDef = struct {
    name: []const u8,
    number: u32,
    ftype: pbwire.c.pbw_type,
    label: pbwire.c.pbw_label,
};

fn buildEnvelope(message_name: []const u8, fields: []const FieldDef) !pbwire.Schema {
    var s = try pbwire.Schema.init();
    errdefer s.deinit();
    var m = try s.addMessage(message_name);
    for (fields) |f| _ = try m.addField(f.name, f.number, f.ftype, f.label);
    s.link() catch |err| {
        std.debug.print("testee: envelope schema: {s}\n", .{s.lastError()});
        return err;
    };
    return s;
}

fn requestSchema() !pbwire.Schema {
    return buildEnvelope("conformance.ConformanceRequest", &[_]FieldDef{
        .{ .name = "protobuf_payload", .number = 1, .ftype = pbwire.c.PBW_TYPE_BYTES, .label = pbwire.c.PBW_LABEL_OPTIONAL },
        .{ .name = "json_payload", .number = 2, .ftype = pbwire.c.PBW_TYPE_STRING, .label = pbwire.c.PBW_LABEL_OPTIONAL },
        .{ .name = "requested_output_format", .number = 3, .ftype = pbwire.c.PBW_TYPE_INT32, .label = pbwire.c.PBW_LABEL_OPTIONAL },
        .{ .name = "message_type", .number = 4, .ftype = pbwire.c.PBW_TYPE_STRING, .label = pbwire.c.PBW_LABEL_OPTIONAL },
        .{ .name = "test_category", .number = 5, .ftype = pbwire.c.PBW_TYPE_INT32, .label = pbwire.c.PBW_LABEL_OPTIONAL },
        .{ .name = "jspb_payload", .number = 7, .ftype = pbwire.c.PBW_TYPE_STRING, .label = pbwire.c.PBW_LABEL_OPTIONAL },
        .{ .name = "text_payload", .number = 8, .ftype = pbwire.c.PBW_TYPE_STRING, .label = pbwire.c.PBW_LABEL_OPTIONAL },
        .{ .name = "print_unknown_fields", .number = 9, .ftype = pbwire.c.PBW_TYPE_BOOL, .label = pbwire.c.PBW_LABEL_OPTIONAL },
    });
}

fn responseSchema() !pbwire.Schema {
    return buildEnvelope("conformance.ConformanceResponse", &[_]FieldDef{
        .{ .name = "parse_error", .number = 1, .ftype = pbwire.c.PBW_TYPE_STRING, .label = pbwire.c.PBW_LABEL_OPTIONAL },
        .{ .name = "runtime_error", .number = 2, .ftype = pbwire.c.PBW_TYPE_STRING, .label = pbwire.c.PBW_LABEL_OPTIONAL },
        .{ .name = "protobuf_payload", .number = 3, .ftype = pbwire.c.PBW_TYPE_BYTES, .label = pbwire.c.PBW_LABEL_OPTIONAL },
        .{ .name = "json_payload", .number = 4, .ftype = pbwire.c.PBW_TYPE_STRING, .label = pbwire.c.PBW_LABEL_OPTIONAL },
        .{ .name = "skipped", .number = 5, .ftype = pbwire.c.PBW_TYPE_STRING, .label = pbwire.c.PBW_LABEL_OPTIONAL },
        .{ .name = "serialize_error", .number = 6, .ftype = pbwire.c.PBW_TYPE_STRING, .label = pbwire.c.PBW_LABEL_OPTIONAL },
        .{ .name = "jspb_payload", .number = 7, .ftype = pbwire.c.PBW_TYPE_STRING, .label = pbwire.c.PBW_LABEL_OPTIONAL },
        .{ .name = "text_payload", .number = 8, .ftype = pbwire.c.PBW_TYPE_STRING, .label = pbwire.c.PBW_LABEL_OPTIONAL },
        .{ .name = "timeout_error", .number = 9, .ftype = pbwire.c.PBW_TYPE_STRING, .label = pbwire.c.PBW_LABEL_OPTIONAL },
    });
}

fn setStr(out: *pbwire.Message, n: u32, text: []const u8) !void {
    // Conformance error strings are ASCII; fall back to the error name when
    // the text cannot be stored (e.g. an interior NUL, which cannot happen).
    const v = pbwire.Value.newString(text) catch return error.OutOfMemory;
    try out.set(n, v);
}

fn setBytes(out: *pbwire.Message, n: u32, bytes: []const u8) !void {
    const v = try pbwire.Value.newBytes(bytes);
    try out.set(n, v);
}

fn intField(msg: *const pbwire.Message, n: u32) i64 {
    const v = msg.getByNumber(n) orelse return 0;
    return v.asI64();
}

fn handleOne(
    alloc: std.mem.Allocator,
    req_ty: *const pbwire.MessageType,
    resp_ty: *const pbwire.MessageType,
    under_test: *const pbwire.Schema,
    req_bytes: []const u8,
) ![]u8 {
    var out = try resp_ty.newMessage();
    defer out.deinit();

    var req = req_ty.decode(req_bytes) catch {
        try setStr(&out, 2, "bad request");
        return out.encode(alloc);
    };
    defer req.deinit();

    const format = intField(&req, 3);
    const category = intField(&req, 5);
    const print_unknown = if (req.getByNumber(9)) |v| v.asBool() else false;
    const type_name = if (req.getByNumber(4)) |v| v.asStr() else null;
    const payload = if (req.getByNumber(1)) |v| v.asBytes() else null;
    const text = if (req.getByNumber(8)) |v| v.asStr() else null;
    const json = if (req.getByNumber(2)) |v| v.asStr() else null;

    // JSPB has no pbwire layer: reported as skipped.
    if (format != WIRE_PROTOBUF and format != WIRE_JSON and format != WIRE_TEXT) {
        try setStr(&out, 5, "no JSPB layer");
        return out.encode(alloc);
    }
    if (type_name == null) {
        try setStr(&out, 2, "no message_type");
        return out.encode(alloc);
    }
    const desc = under_test.messageType(type_name.?) catch {
        const why = try std.fmt.allocPrint(alloc, "unknown message type {s}", .{type_name.?});
        defer alloc.free(why);
        try setStr(&out, 2, why);
        return out.encode(alloc);
    };

    // Input by presence: json_payload, text_payload or protobuf_payload.
    var decoded: ?pbwire.Message = null;
    defer if (decoded) |*d| d.deinit();
    if (json) |text_in| {
        decoded = desc.parseJson(text_in, .{ .ignore_unknown_fields = category == CATEGORY_JSON_IGNORE_UNKNOWN }) catch |err| {
            try setStr(&out, 1, @errorName(err));
            return out.encode(alloc);
        };
    } else if (text) |text_in| {
        decoded = desc.parseText(text_in) catch |err| {
            try setStr(&out, 1, @errorName(err));
            return out.encode(alloc);
        };
    } else if (payload) |wire| {
        decoded = desc.decode(wire) catch |err| {
            try setStr(&out, 1, @errorName(err));
            return out.encode(alloc);
        };
    } else {
        try setStr(&out, 5, "unsupported payload");
        return out.encode(alloc);
    }

    if (format == WIRE_JSON) {
        const jout = decoded.?.printJson(alloc) catch |err| {
            try setStr(&out, 6, @errorName(err));
            return out.encode(alloc);
        };
        defer alloc.free(jout);
        try setBytes(&out, 4, jout);
    } else if (format == WIRE_TEXT) {
        const tout = decoded.?.printText(alloc, print_unknown) catch |err| {
            try setStr(&out, 6, @errorName(err));
            return out.encode(alloc);
        };
        defer alloc.free(tout);
        try setBytes(&out, 8, tout);
    } else {
        const bytes = decoded.?.encode(alloc) catch |err| {
            try setStr(&out, 6, @errorName(err));
            return out.encode(alloc);
        };
        defer alloc.free(bytes);
        try setBytes(&out, 3, bytes);
    }
    return out.encode(alloc);
}

pub fn main(init: std.process.Init.Minimal) !void {
    var gpa = std.heap.DebugAllocator(.{}){};
    defer _ = gpa.deinit();
    const alloc = gpa.allocator();

    var arg_it = std.process.Args.Iterator.init(init.args);
    _ = arg_it.next();
    const desc_path = arg_it.next() orelse {
        std.debug.print("usage: testee-zig <descriptor-set>\n", .{});
        std.process.exit(2);
    };

    var under_test = pbwire.Schema.fromFile(alloc, desc_path) catch |err| {
        std.debug.print("load {s}: {s}\n", .{ desc_path, @errorName(err) });
        std.process.exit(2);
    };
    defer under_test.deinit();
    var req_schema = try requestSchema();
    defer req_schema.deinit();
    var resp_schema = try responseSchema();
    defer resp_schema.deinit();
    const req_ty = try req_schema.messageType("conformance.ConformanceRequest");
    const resp_ty = try resp_schema.messageType("conformance.ConformanceResponse");

    var threaded: std.Io.Threaded = .init(alloc, .{});
    defer threaded.deinit();
    const io = threaded.io();

    const stdin = std.Io.File.stdin();
    const stdout = std.Io.File.stdout();
    var in_buf: [65536]u8 = undefined;
    var out_buf: [65536]u8 = undefined;
    var in_file = stdin.reader(io, &in_buf);
    var out_file = stdout.writer(io, &out_buf);
    var in = &in_file.interface;
    var out = &out_file.interface;
    defer out.flush() catch {};
    var len_buf: [4]u8 = undefined;
    while (true) {
        in.readSliceAll(len_buf[0..]) catch |err| {
            if (err == error.EndOfStream) break;
            return err;
        };
        const len = std.mem.readInt(u32, &len_buf, .little);
        const req_bytes = try alloc.alloc(u8, len);
        // Explicit frees, not defer: one request at a time is live and the
        // suite sends thousands of them.
        try in.readSliceAll(req_bytes);

        const resp = try handleOne(alloc, &req_ty, &resp_ty, &under_test, req_bytes);
        alloc.free(req_bytes);
        var out_len: [4]u8 = undefined;
        std.mem.writeInt(u32, &out_len, @intCast(resp.len), .little);
        try out.writeAll(&out_len);
        try out.writeAll(resp);
        alloc.free(resp);
        // The runner reads one response before sending the next request: a
        // buffered write that is never flushed deadlocks the exchange.
        try out.flush();
    }
}

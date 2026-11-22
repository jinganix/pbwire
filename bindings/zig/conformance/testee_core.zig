//! Google conformance testee driven by the pure-Zig core: the same wire
//! protocol as the other testees (4-byte little-endian length prefix, then a
//! `ConformanceRequest` / `ConformanceResponse`) and the same schema-driven
//! shape -- the request names the message type and the descriptor set answers
//! with the schema, so there is no per-message code and no C library.
//!
//! Binary in -> binary out is what this core implements. JSON and text requests
//! answer `skipped` (the runner does not count a skip as a failure), so the pass
//! set is the binary suite, not the JSON/text mappings.
//!
//!   zig build -Dcore=true testee   # installs zig-out/bin/testee-zig
//!   testee-zig <descriptor-set>
//!
//! Driven by `make conformance-zig-core` from the repo root.
const std = @import("std");
const core = @import("pbwire_core");

const WIRE_PROTOBUF: i64 = 1;

fn intField(msg: *const core.Message, name: []const u8) i64 {
    return (msg.getI64(name) catch null) orelse 0;
}

fn setStr(out: *core.Message, name: []const u8, text: []const u8) void {
    out.setStr(name, text) catch {};
}

fn handleOne(
    alloc: std.mem.Allocator,
    schema: *const core.Schema,
    req_ty: *const core.MessageType,
    resp_ty: *const core.MessageType,
    req_bytes: []const u8,
) ![]u8 {
    var out = try resp_ty.newMessage(alloc);
    defer out.deinit();

    var req = req_ty.decode(alloc, req_bytes) catch {
        setStr(&out, "runtime_error", "cannot parse ConformanceRequest");
        return resp_ty.encode(alloc, &out);
    };
    defer req.deinit();

    const format = intField(&req, "requested_output_format");
    if (format != WIRE_PROTOBUF) {
        setStr(&out, "skipped", "only the binary wire format is supported");
        return resp_ty.encode(alloc, &out);
    }
    if ((req.has("json_payload") catch false) or (req.has("text_payload") catch false)) {
        setStr(&out, "skipped", "JSON and text input are not supported by the pure-Zig core");
        return resp_ty.encode(alloc, &out);
    }
    if (!(req.has("protobuf_payload") catch false)) {
        setStr(&out, "skipped", "no binary payload");
        return resp_ty.encode(alloc, &out);
    }
    const name = (req.getStr("message_type") catch null) orelse "";
    const ty = schema.messageType(name) catch {
        const why = try std.fmt.allocPrint(alloc, "unknown message type {s}", .{name});
        defer alloc.free(why);
        setStr(&out, "runtime_error", why);
        return resp_ty.encode(alloc, &out);
    };
    const payload = (req.getBytes("protobuf_payload") catch null) orelse &[_]u8{};

    var decoded = ty.decode(alloc, payload) catch |err| {
        setStr(&out, "parse_error", @errorName(err));
        return resp_ty.encode(alloc, &out);
    };
    defer decoded.deinit();

    const bytes = ty.encode(alloc, &decoded) catch |err| {
        setStr(&out, "serialize_error", @errorName(err));
        return resp_ty.encode(alloc, &out);
    };
    defer alloc.free(bytes);
    try out.setBytes("protobuf_payload", bytes);
    return resp_ty.encode(alloc, &out);
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

    var schema = core.Schema.fromFile(alloc, desc_path) catch |err| {
        std.debug.print("load {s}: {s}\n", .{ desc_path, @errorName(err) });
        std.process.exit(2);
    };
    defer schema.deinit();
    const req_ty = schema.messageType("conformance.ConformanceRequest") catch |err| {
        std.debug.print("schema: {s}\n", .{@errorName(err)});
        std.process.exit(2);
    };
    const resp_ty = schema.messageType("conformance.ConformanceResponse") catch |err| {
        std.debug.print("schema: {s}\n", .{@errorName(err)});
        std.process.exit(2);
    };

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
        try in.readSliceAll(req_bytes);

        const resp = try handleOne(alloc, &schema, &req_ty, &resp_ty, req_bytes);
        alloc.free(req_bytes);
        var out_len: [4]u8 = undefined;
        std.mem.writeInt(u32, &out_len, @intCast(resp.len), .little);
        try out.writeAll(&out_len);
        try out.writeAll(resp);
        alloc.free(resp);
        try out.flush();
    }
}

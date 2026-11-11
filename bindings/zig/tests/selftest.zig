//! Self-check against the pinned `protoc` bytes: builds `structtest.Scalars3`
//! from field values, encodes, and compares with `matrix3.bin` (the same
//! oracle the C and Rust bindings use). Then decodes it back and checks the
//! values survived, including repeated-field handling. Run with:
//!
//!   zig build test    (from bindings/zig)
const std = @import("std");
const pbwire = @import("pbwire");

// Fixtures live outside the package, so @embedFile cannot see them (Zig
// 0.15+ restricts embeds to the package path): read them at run time.
// `zig build test` runs with cwd = bindings/zig.
const DESC_PATH = "../c/tests/data/matrix3.desc";
const BIN_PATH = "../c/tests/data/matrix3.bin";
const MSG = "structtest.Scalars3";
const INNER = "structtest.Inner3";

fn loadSchema() !pbwire.Schema {
    return pbwire.Schema.fromFile(std.testing.allocator, DESC_PATH);
}

fn loadOracle() ![]u8 {
    return pbwire.readFile(std.testing.allocator, BIN_PATH);
}

fn fill(ty: *const pbwire.MessageType, inner_ty: *const pbwire.MessageType) !pbwire.Message {
    var m = try ty.newMessage();
    errdefer m.deinit();
    try m.setI64(try ty.field("i32"), std.math.minInt(i32));
    try m.setI64(try ty.field("i64"), std.math.maxInt(i64));
    try m.setI64(try ty.field("u32"), std.math.maxInt(u32));
    try m.setI64(try ty.field("u64"), -1); // u64::MAX bits
    try m.setI64(try ty.field("s32"), std.math.minInt(i32));
    try m.setI64(try ty.field("s64"), std.math.minInt(i64));
    try m.setI64(try ty.field("fx32"), 0xdeadbeef);
    try m.setI64(try ty.field("fx64"), 72623859790382856);
    try m.setI64(try ty.field("sfx32"), std.math.minInt(i32));
    try m.setI64(try ty.field("sfx64"), std.math.minInt(i64));
    try m.setF32(try ty.field("f32"), std.math.inf(f32));
    try m.setF64(try ty.field("f64"), -123456.789);
    try m.setBool(try ty.field("flag"), true);
    try m.setStr(try ty.field("str"), "hello");
    try m.setI64(try ty.field("color"), 2);
    try m.setI64(try ty.field("opt_i32"), -7);
    try m.setStr(try ty.field("opt_str"), "opt");
    for ([_]i64{ 1, -5, 300 }) |v| try m.setI64(try ty.field("nums"), v);
    for ([_][]const u8{ "a", "bb" }) |tag| try m.setStr(try ty.field("tags"), tag);
    for ([_][2][]const u8{ .{ "x", "" }, .{ "y", "" } }, [_]i64{ 1, -2 }) |pair, v| {
        var inner = try m.setMessage(try ty.field("inners"));
        try inner.setStr(try inner_ty.field("name"), pair[0]);
        try inner.setI64(try inner_ty.field("value"), v);
    }
    for ([_]bool{ true, false, true }) |flag| try m.setBool(try ty.field("flags"), flag);
    var inner = try m.setMessage(try ty.field("inner"));
    try inner.setStr(try inner_ty.field("name"), "kid");
    try inner.setI64(try inner_ty.field("value"), 42);
    return m;
}

test "encode matches protoc oracle" {
    const alloc = std.testing.allocator;
    var schema = try loadSchema();
    defer schema.deinit();
    const ty = try schema.messageType(MSG);
    const inner_ty = try schema.messageType(INNER);
    var msg = try fill(&ty, &inner_ty);
    defer msg.deinit();

    const wire = try msg.encode(alloc);
    defer alloc.free(wire);
    const oracle = try loadOracle();
    defer alloc.free(oracle);
    try std.testing.expectEqualSlices(u8, oracle, wire);

    // The caller-buffer path must produce the same bytes.
    const size = try pbwire.encodedSizeOf(&msg);
    const buf = try alloc.alloc(u8, size + pbwire.encode_slack);
    defer alloc.free(buf);
    const written = try msg.encodeInto(buf);
    try std.testing.expectEqualSlices(u8, oracle, written);

    // Repeated scalars append; a singular field is replaced.
    try msg.setStr(try ty.field("str"), "hello");
    const again = try msg.encode(alloc);
    defer alloc.free(again);
    try std.testing.expectEqualSlices(u8, oracle, again);
}

test "decode roundtrip" {
    const alloc = std.testing.allocator;
    const oracle = try loadOracle();
    defer alloc.free(oracle);
    var schema = try loadSchema();
    defer schema.deinit();
    const ty = try schema.messageType(MSG);
    const inner_ty = try schema.messageType(INNER);
    var decoded = try ty.decode(oracle);
    defer decoded.deinit();

    try std.testing.expectEqual(@as(i64, std.math.minInt(i32)), decoded.getI64(try ty.field("i32")));
    try std.testing.expectEqual(std.math.maxInt(i64), decoded.getI64(try ty.field("i64")));
    try std.testing.expectEqual(@as(i64, std.math.maxInt(u32)), decoded.getI64(try ty.field("u32")));
    try std.testing.expectEqual(@as(i64, -1), decoded.getI64(try ty.field("u64")));
    try std.testing.expectEqual(@as(i64, 0xdeadbeef), decoded.getI64(try ty.field("fx32")));
    try std.testing.expectEqual(@as(i64, 72623859790382856), decoded.getI64(try ty.field("fx64")));
    try std.testing.expectEqual(std.math.inf(f32), decoded.getF32(try ty.field("f32")));
    try std.testing.expectEqual(@as(f64, -123456.789), decoded.getF64(try ty.field("f64")));
    try std.testing.expect(decoded.getBool(try ty.field("flag")));
    try std.testing.expectEqualStrings("hello", decoded.getStr(try ty.field("str")).?);
    try std.testing.expectEqual(@as(i64, 2), decoded.getI64(try ty.field("color")));
    try std.testing.expectEqual(@as(i64, -7), decoded.getI64(try ty.field("opt_i32")));
    try std.testing.expectEqualStrings("opt", decoded.getStr(try ty.field("opt_str")).?);
    try std.testing.expect(decoded.has(try ty.field("opt_i32")));

    const nums = decoded.value(try ty.field("nums")).?;
    try std.testing.expectEqual(@as(usize, 3), nums.len());
    try std.testing.expectEqual(@as(i64, 1), nums.at(0).?.asI64());
    try std.testing.expectEqual(@as(i64, -5), nums.at(1).?.asI64());
    try std.testing.expectEqual(@as(i64, 300), nums.at(2).?.asI64());

    const tags = decoded.value(try ty.field("tags")).?;
    try std.testing.expectEqual(@as(usize, 2), tags.len());
    try std.testing.expectEqualStrings("a", tags.at(0).?.asStr().?);
    try std.testing.expectEqualStrings("bb", tags.at(1).?.asStr().?);

    const inners = decoded.value(try ty.field("inners")).?;
    try std.testing.expectEqual(@as(usize, 2), inners.len());
    const first = inners.at(0).?.asMessage().?;
    try std.testing.expectEqualStrings("x", first.getStr(try inner_ty.field("name")).?);
    try std.testing.expectEqual(@as(i64, 1), first.getI64(try inner_ty.field("value")));
    const second = inners.at(1).?.asMessage().?;
    try std.testing.expectEqualStrings("y", second.getStr(try inner_ty.field("name")).?);
    try std.testing.expectEqual(@as(i64, -2), second.getI64(try inner_ty.field("value")));

    const inner = decoded.getMessage(try ty.field("inner")).?;
    try std.testing.expectEqualStrings("kid", inner.getStr(try inner_ty.field("name")).?);
    try std.testing.expectEqual(@as(i64, 42), inner.getI64(try inner_ty.field("value")));

    // Decode -> encode is byte-exact (unknown fields preserved).
    const back = try decoded.encode(alloc);
    defer alloc.free(back);
    try std.testing.expectEqualSlices(u8, oracle, back);
}

test "empty message roundtrip" {
    const alloc = std.testing.allocator;
    var schema = try loadSchema();
    defer schema.deinit();
    const ty = try schema.messageType(MSG);

    var empty = try ty.newMessage();
    defer empty.deinit();
    const wire = try empty.encode(alloc);
    defer alloc.free(wire);
    try std.testing.expectEqual(@as(usize, 0), wire.len);

    var decoded = try ty.decode(&[_]u8{});
    defer decoded.deinit();
    try std.testing.expect(!decoded.has(try ty.field("str")));
    try std.testing.expect(decoded.isInitialized());
    const back = try decoded.encode(alloc);
    defer alloc.free(back);
    try std.testing.expectEqual(@as(usize, 0), back.len);
}

test "wrong typed value is rejected" {
    var schema = try loadSchema();
    defer schema.deinit();
    const ty = try schema.messageType(MSG);
    var msg = try ty.newMessage();
    defer msg.deinit();
    // `i32` is an integer field: storing a string must fail, not corrupt.
    try std.testing.expectError(pbwire.Error.WrongType, msg.setStr(try ty.field("i32"), "not a number"));
}

test "setMessage on a scalar field is rejected" {
    var schema = try loadSchema();
    defer schema.deinit();
    const ty = try schema.messageType(MSG);
    var msg = try ty.newMessage();
    defer msg.deinit();
    try std.testing.expectError(pbwire.Error.WrongType, msg.setMessage(try ty.field("str")));
}

test "clear drops the field" {
    var schema = try loadSchema();
    defer schema.deinit();
    const ty = try schema.messageType(MSG);
    const inner_ty = try schema.messageType(INNER);
    var msg = try fill(&ty, &inner_ty);
    defer msg.deinit();
    try std.testing.expect(msg.has(try ty.field("str")));
    msg.clear(try ty.field("str"));
    try std.testing.expect(!msg.has(try ty.field("str")));
}

test "text and json roundtrip" {
    const alloc = std.testing.allocator;
    var schema = try loadSchema();
    defer schema.deinit();
    const ty = try schema.messageType(MSG);
    const inner_ty = try schema.messageType(INNER);
    var msg = try fill(&ty, &inner_ty);
    defer msg.deinit();

    const text = try msg.printText(alloc, false);
    defer alloc.free(text);
    var from_text = try ty.parseText(text);
    defer from_text.deinit();
    const text_back = try from_text.printText(alloc, false);
    defer alloc.free(text_back);
    try std.testing.expectEqualStrings(text, text_back);

    const json = try msg.printJson(alloc);
    defer alloc.free(json);
    var from_json = try ty.parseJson(json, .{});
    defer from_json.deinit();
    const json_back = try from_json.printJson(alloc);
    defer alloc.free(json_back);
    try std.testing.expectEqualStrings(json, json_back);
}

test "visit scans fields without a tree" {
    const alloc = std.testing.allocator;
    var schema = try loadSchema();
    defer schema.deinit();
    const ty = try schema.messageType(MSG);
    const inner_ty = try schema.messageType(INNER);
    var msg = try fill(&ty, &inner_ty);
    defer msg.deinit();
    const wire = try msg.encode(alloc);
    defer alloc.free(wire);

    const events = try ty.visitCollect(alloc, wire, null);
    defer alloc.free(events);
    // str (field 14, "hello") arrives as one LEN event.
    var found_str = false;
    for (events) |ev| {
        if (ev.number == 14) {
            try std.testing.expectEqual(@as(u32, 2), ev.wire_type);
            try std.testing.expectEqualStrings("hello", ev.data);
            try std.testing.expect(ev.field != null);
            found_str = true;
        }
        if (ev.number == 13) { // flag, varint true
            try std.testing.expectEqual(@as(u32, 0), ev.wire_type);
            try std.testing.expectEqual(@as(u64, 1), ev.u64);
        }
    }
    try std.testing.expect(found_str);
    // A byte cap below the payload fails instead of scanning.
    try std.testing.expectError(pbwire.Error.LimitExceeded, ty.visitCollect(alloc, wire, .{ .max_bytes = 4 }));
}

test "delimited framing roundtrips" {
    const alloc = std.testing.allocator;
    var schema = try loadSchema();
    defer schema.deinit();
    const ty = try schema.messageType(MSG);
    const inner_ty = try schema.messageType(INNER);
    var msg = try fill(&ty, &inner_ty);
    defer msg.deinit();

    const frame = try msg.encodeDelimited(alloc);
    defer alloc.free(frame);
    const buf = try alloc.alloc(u8, frame.len + pbwire.encode_slack);
    defer alloc.free(buf);
    try std.testing.expectEqualSlices(u8, frame, try msg.encodeDelimitedInto(buf));

    // Two concatenated frames decode in a loop via `consumed`.
    var cat: std.ArrayList(u8) = .empty;
    defer cat.deinit(alloc);
    try cat.appendSlice(alloc, frame);
    try cat.appendSlice(alloc, frame);
    var first = try ty.decodeDelimited(cat.items, null);
    defer first.message.deinit();
    try std.testing.expectEqual(frame.len, first.consumed);
    var second = try ty.decodeDelimited(cat.items[first.consumed..], null);
    defer second.message.deinit();
    const back = try second.message.encode(alloc);
    defer alloc.free(back);
    const fwd = try first.message.encode(alloc);
    defer alloc.free(fwd);
    try std.testing.expectEqualSlices(u8, fwd, back);
}

test "limits reject oversized input" {
    const alloc = std.testing.allocator;
    var schema = try loadSchema();
    defer schema.deinit();
    const ty = try schema.messageType(MSG);
    const inner_ty = try schema.messageType(INNER);
    var msg = try fill(&ty, &inner_ty);
    defer msg.deinit();
    const wire = try msg.encode(alloc);
    defer alloc.free(wire);
    try std.testing.expectError(pbwire.Error.LimitExceeded, ty.decodeLimits(wire, .{ .max_bytes = 4 }));
    const ok = try ty.decodeLimits(wire, .{});
    defer {
        var m = ok;
        m.deinit();
    }
}

test "sink streams without materializing" {
    const alloc = std.testing.allocator;
    var schema = try loadSchema();
    defer schema.deinit();
    const ty = try schema.messageType(MSG);
    const inner_ty = try schema.messageType(INNER);
    var msg = try fill(&ty, &inner_ty);
    defer msg.deinit();

    const State = struct {
        var buf: std.ArrayList(u8) = .empty;
        var a: std.mem.Allocator = undefined;
        var chunks: usize = 0;
        fn sink(data: [*c]const u8, len: usize, ctx: ?*anyopaque) callconv(.c) pbwire.c.pbw_status {
            _ = ctx;
            chunks += 1;
            buf.appendSlice(a, @as([*]const u8, @ptrCast(data))[0..len]) catch return pbwire.c.PBW_ERR_NOMEM;
            return pbwire.c.PBW_OK;
        }
    };
    State.a = alloc;
    defer State.buf.deinit(alloc);
    try msg.encodeSink(null, State.sink);
    try std.testing.expect(State.chunks >= 1);
    const wire = try msg.encode(alloc);
    defer alloc.free(wire);
    try std.testing.expectEqualSlices(u8, wire, State.buf.items);
}

test "wkt mirrors roundtrip through hand-built descriptors" {
    var schema = try pbwire.Schema.init();
    defer schema.deinit();
    var any_ty = try schema.addMessage("google.protobuf.Any");
    _ = try any_ty.addField("type_url", 1, pbwire.c.PBW_TYPE_STRING, pbwire.c.PBW_LABEL_OPTIONAL);
    _ = try any_ty.addField("value", 2, pbwire.c.PBW_TYPE_BYTES, pbwire.c.PBW_LABEL_OPTIONAL);
    var ts_ty = try schema.addMessage("google.protobuf.Timestamp");
    _ = try ts_ty.addField("seconds", 1, pbwire.c.PBW_TYPE_INT64, pbwire.c.PBW_LABEL_OPTIONAL);
    _ = try ts_ty.addField("nanos", 2, pbwire.c.PBW_TYPE_INT32, pbwire.c.PBW_LABEL_OPTIONAL);
    var fm_ty = try schema.addMessage("google.protobuf.FieldMask");
    _ = try fm_ty.addField("paths", 1, pbwire.c.PBW_TYPE_STRING, pbwire.c.PBW_LABEL_REPEATED);
    try schema.link();

    var pack_msg = try any_ty.anyPack("type.googleapis.com/x.Y", &[_]u8{ 0x08, 0x01 });
    defer pack_msg.deinit();
    try std.testing.expectEqualStrings("type.googleapis.com/x.Y", pack_msg.anyTypeUrl().?);
    try std.testing.expectEqualSlices(u8, &[_]u8{ 0x08, 0x01 }, pack_msg.anyPayload().?);

    var ts = try ts_ty.timestampFromMillis(1501);
    defer ts.deinit();
    try std.testing.expectEqual(@as(i64, 1501), try ts.timestampMillis());
    try std.testing.expectEqual(@as(i64, -2500), try (try ts_ty.timestampFromMillis(-2500)).timestampMillis());

    var fm = try fm_ty.newMessage();
    defer fm.deinit();
    try fm.setStr(try fm_ty.field("paths"), "a.b");
    try fm.setStr(try fm_ty.field("paths"), "c");
    try std.testing.expectEqual(@as(usize, 2), fm.fieldmaskPathCount());
    try std.testing.expectEqualStrings("c", fm.fieldmaskPathAt(1).?);
    try std.testing.expect(fm.fieldmaskPathAt(2) == null);
}

test "services are absent without a service fixture" {
    var schema = try loadSchema();
    defer schema.deinit();
    try std.testing.expectEqual(@as(usize, 0), schema.serviceCount());
    try std.testing.expectError(pbwire.Error.NotFound, schema.service("pbwire.svc.Jobs"));
    try std.testing.expectError(pbwire.Error.InvalidArgument, schema.resolve("pbwire.svc.Jobs", "Get"));
}

test "pool and context recycle arenas" {
    const alloc = std.testing.allocator;
    var schema = try loadSchema();
    defer schema.deinit();
    const ty = try schema.messageType(MSG);
    const inner_ty = try schema.messageType(INNER);
    var msg = try fill(&ty, &inner_ty);
    defer msg.deinit();
    const wire = try msg.encode(alloc);
    defer alloc.free(wire);

    var pool = try pbwire.Pool.init(2, 0);
    defer pool.deinit();
    var ctx = try pool.acquire();
    try std.testing.expectEqual(@as(usize, 0), pool.idle());
    var decoded = try ctx.decode(&ty, wire);
    try std.testing.expect(decoded.has(try ty.field("str")));
    const bytes = try ctx.encodeBuf(&decoded);
    try std.testing.expectEqualSlices(u8, wire, bytes);
    try std.testing.expect(ctx.used() > 0);
    ctx.reset();
    pool.release(&ctx);
    try std.testing.expectEqual(@as(usize, 1), pool.idle());
}

//! Benchmarks the Zig binding on the same `matrix3.proto` and the four standard
//! tiers from BENCHMARKS.md (small / medium / large / xlarge scale the repeated
//! fields x0 / x1 / x100 / x3000, landing on 136 / 174 / 2469 / 74913 bytes).
//!
//! Every tier is gated before timing: the encoded bytes must land on the
//! pinned size, and decoding the reference and re-encoding must return the
//! same bytes. Encode reuses one caller buffer (`encodeInto`), decode is
//! one-shot per op, each side's idiomatic hot loop. Prints one block per case
//! in the repo-wide harness format (throughput over wire bytes).
//!
//!   zig build bench [-- <matrix3.desc>]
//!   ./benchmarks/bench.sh
const std = @import("std");
const pbwire = @import("pbwire");

const CASES = [_][]const u8{ "small", "medium", "large", "xlarge" };
const REPEATS = [_]usize{ 0, 1, 100, 3000 };
const WANT = [_]usize{ 136, 174, 2469, 74913 };
const FLOORS = [_]usize{ 50_000, 50_000, 5_000, 500 };
const PASSES = 3;
const MIN_PASS_SECONDS = 0.25;

var failures: usize = 0;

fn fail(case: []const u8, comptime fmt: []const u8, args: anytype) void {
    std.debug.print("FAIL {s}: ", .{case});
    std.debug.print(fmt, args);
    std.debug.print("\n", .{});
    failures += 1;
}

fn fill(ty: *const pbwire.MessageType, inner_ty: *const pbwire.MessageType, n: usize) !pbwire.Message {
    var m = try ty.newMessage();
    errdefer m.deinit();
    try m.setI64(try ty.field("i32"), std.math.minInt(i32));
    try m.setI64(try ty.field("i64"), std.math.maxInt(i64));
    try m.setI64(try ty.field("u32"), std.math.maxInt(u32));
    try m.setI64(try ty.field("u64"), -1);
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
    try m.setI64(try ty.field("color"), 2); // COLOR3_GREEN
    try m.setI64(try ty.field("opt_i32"), -7);
    try m.setStr(try ty.field("opt_str"), "opt");
    var inner = try m.setMessage(try ty.field("inner"));
    try inner.setStr(try inner_ty.field("name"), "kid");
    try inner.setI64(try inner_ty.field("value"), 42);
    var i: usize = 0;
    while (i < n) : (i += 1) {
        try m.setI64(try ty.field("nums"), @as(i64, @intCast(i)) - 3);
        try m.setStr(try ty.field("tags"), "tag");
        try m.setBool(try ty.field("flags"), i % 2 == 0);
        var kid = try m.setMessage(try ty.field("inners"));
        try kid.setStr(try inner_ty.field("name"), "grandkid");
        try kid.setI64(try inner_ty.field("value"), @as(i64, @intCast(i)) + 1);
    }
    return m;
}

fn nanos(io: std.Io) i96 {
    return std.Io.Clock.now(.awake, io).nanoseconds;
}

fn calibrate(io: std.Io, op: anytype, floor: usize) usize {
    var count = @max(floor, 1);
    while (true) {
        const start = nanos(io);
        var i: usize = 0;
        while (i < count) : (i += 1) op();
        const elapsed: f64 = @floatFromInt(nanos(io) - start);
        if (elapsed / 1e9 >= MIN_PASS_SECONDS or count >= 100_000_000) return count;
        count *= 2;
    }
}

fn run(io: std.Io, op: anytype, iterations: usize) f64 {
    const start = nanos(io);
    var i: usize = 0;
    while (i < iterations) : (i += 1) op();
    const elapsed: f64 = @floatFromInt(nanos(io) - start);
    return elapsed / @as(f64, @floatFromInt(iterations));
}

fn runCase(io: std.Io, alloc: std.mem.Allocator, ty: *const pbwire.MessageType, inner_ty: *const pbwire.MessageType, tier: usize) !void {
    const label = CASES[tier];
    const n = REPEATS[tier];

    var message = try fill(ty, inner_ty, n);
    defer message.deinit();
    const reference = try message.encode(alloc);
    defer alloc.free(reference);
    if (reference.len != WANT[tier]) {
        fail(label, "pbwire-zig wrote {d} bytes, expected {d}", .{ reference.len, WANT[tier] });
        return;
    }
    std.debug.print("OK   {s} encoding parity, x{d} ({d} bytes)\n", .{ label, n, reference.len });

    // Decode must round-trip byte-exactly and expose the same values.
    var back = try ty.decode(reference);
    defer back.deinit();
    const f_str = try ty.field("str");
    const f_inner = try ty.field("inner");
    const f_nums = try ty.field("nums");
    const f_name = try inner_ty.field("name");
    const back_ok = blk: {
        const enc = back.encode(alloc) catch break :blk false;
        defer alloc.free(enc);
        if (!std.mem.eql(u8, enc, reference)) break :blk false;
        if (!std.mem.eql(u8, back.getStr(f_str) orelse "", "hello")) break :blk false;
        const got_inner = back.getMessage(f_inner) orelse break :blk false;
        if (!std.mem.eql(u8, got_inner.getStr(f_name) orelse "", "kid")) break :blk false;
        const nums = back.value(f_nums) orelse break :blk n == 0;
        if (nums.len() != n) break :blk false;
        if (n > 0 and nums.at(n - 1).?.asI64() != @as(i64, @intCast(n)) - 4) break :blk false;
        break :blk true;
    };
    if (!back_ok) {
        fail(label, "pbwire-zig decode differs", .{});
        return;
    }

    const size = try pbwire.encodedSizeOf(&message);
    const buf = try alloc.alloc(u8, size + pbwire.encode_slack);
    defer alloc.free(buf);
    const iterations = FLOORS[tier];

    const S = struct {
        var m: *const pbwire.Message = undefined;
        var t: *const pbwire.MessageType = undefined;
        var r: []const u8 = undefined;
        var b: []u8 = undefined;
        fn enc() void {
            const out = m.encodeInto(b) catch unreachable;
            std.mem.doNotOptimizeAway(out);
        }
        fn dec() void {
            var d = t.decode(r) catch unreachable;
            defer d.deinit();
            std.mem.doNotOptimizeAway(&d);
        }
    };
    S.m = &message;
    S.t = ty;
    S.r = reference;
    S.b = buf;

    const enc_count = calibrate(io, S.enc, iterations);
    var enc_best: f64 = std.math.floatMax(f64);
    for (0..PASSES) |_| enc_best = @min(enc_best, run(io, S.enc, enc_count));
    const dec_count = calibrate(io, S.dec, iterations);
    var dec_best: f64 = std.math.floatMax(f64);
    for (0..PASSES) |_| dec_best = @min(dec_best, run(io, S.dec, dec_count));

    const bytes_f: f64 = @floatFromInt(reference.len);
    std.debug.print("{s}: {d} bytes, {d} iterations (repeat field x{d})\n", .{ label, reference.len, enc_count, n });
    std.debug.print("  impl      op           ns/op       MB/s\n", .{});
    std.debug.print("  pbwire-zig encode      {d:>8.1}    {d:>8.1}\n", .{ enc_best, bytes_f / enc_best * 1000.0 });
    std.debug.print("  pbwire-zig decode      {d:>8.1}    {d:>8.1}\n", .{ dec_best, bytes_f / dec_best * 1000.0 });
}

pub fn main(init: std.process.Init.Minimal) !void {
    var gpa = std.heap.DebugAllocator(.{}){};
    defer _ = gpa.deinit();
    const alloc = gpa.allocator();

    var arg_it = std.process.Args.Iterator.init(init.args);
    _ = arg_it.next();
    const desc_path = arg_it.next() orelse "../c/tests/data/matrix3.desc";

    var schema = pbwire.Schema.fromFile(alloc, desc_path) catch |err| {
        std.debug.print("load {s}: {s}\n", .{ desc_path, @errorName(err) });
        std.process.exit(2);
    };
    defer schema.deinit();
    const ty = try schema.messageType("structtest.Scalars3");
    const inner_ty = try schema.messageType("structtest.Inner3");

    var threaded: std.Io.Threaded = .init(alloc, .{});
    defer threaded.deinit();
    const io = threaded.io();

    for (0..CASES.len) |tier| try runCase(io, alloc, &ty, &inner_ty, tier);
    if (failures != 0) {
        std.debug.print("{d} check(s) failed\n", .{failures});
        std.process.exit(1);
    }
}

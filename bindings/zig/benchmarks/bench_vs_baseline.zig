//! Four standard tiers, pbwire-zig vs Arwalk/zig-protobuf generated code on the
//! same `matrix3.proto` (small / medium / large / xlarge scale the repeated
//! fields x0 / x1 / x100 / x3000, landing on 136 / 174 / 2469 / 74913 bytes).
//!
//! Every tier is gated before timing: both sides must encode byte-identical
//! output at the pinned size, and decoding the reference and re-encoding must
//! return the same bytes. Encode reuses one caller buffer each side (pbwire's
//! `encodeInto`, a fixed `std.Io.Writer` plus a retained scratch arena for
//! the baseline); decode is one-shot per op, each side's idiomatic hot loop.
//! Prints one block per case in the repo-wide harness format, with `base`
//! rows and a `-- vs baseline` line like the other bindings.
//!
//!   zig build -Doptimize=ReleaseFast -Dbaseline=true bench-baseline [-- <matrix3.desc>]
const std = @import("std");
const pbwire = @import("pbwire");
const matrix3 = @import("matrix3");
const Base = matrix3.Scalars3;

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

fn fillPbwire(ty: *const pbwire.MessageType, inner_ty: *const pbwire.MessageType, n: usize) !pbwire.Message {
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

fn fillBase(alloc: std.mem.Allocator, n: usize) !Base {
    var m = Base{};
    m.i32 = std.math.minInt(i32);
    m.i64 = std.math.maxInt(i64);
    m.u32 = std.math.maxInt(u32);
    m.u64 = std.math.maxInt(u64);
    m.s32 = std.math.minInt(i32);
    m.s64 = std.math.minInt(i64);
    m.fx32 = 0xdeadbeef;
    m.fx64 = 72623859790382856;
    m.sfx32 = std.math.minInt(i32);
    m.sfx64 = std.math.minInt(i64);
    m.f32 = std.math.inf(f32);
    m.f64 = -123456.789;
    m.flag = true;
    m.str = "hello";
    m.color = @fromBackingInt(2); // COLOR3_GREEN
    m.opt_i32 = -7;
    m.opt_str = "opt";
    m.inner = .{ .name = "kid", .value = 42 };
    var i: usize = 0;
    while (i < n) : (i += 1) {
        try m.nums.append(alloc, @as(i32, @intCast(i)) - 3);
        try m.tags.append(alloc, "tag");
        try m.flags.append(alloc, i % 2 == 0);
        try m.inners.append(alloc, .{ .name = "grandkid", .value = @as(i32, @intCast(i)) + 1 });
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

    // pbwire side: fill once, encode, gate the pinned size.
    var pmsg = try fillPbwire(ty, inner_ty, n);
    defer pmsg.deinit();
    const reference = try pmsg.encode(alloc);
    defer alloc.free(reference);
    if (reference.len != WANT[tier]) {
        fail(label, "pbwire-zig wrote {d} bytes, expected {d}", .{ reference.len, WANT[tier] });
        return;
    }

    // Baseline side: same values through the generated struct. String and
    // message literals are only read by encode, so an arena owns just the
    // repeated appends and is dropped with the tier.
    var barena = std.heap.ArenaAllocator.init(alloc);
    defer barena.deinit();
    const bmsg = try fillBase(barena.allocator(), n);
    const bsize = try pbwire.encodedSizeOf(&pmsg);
    const buf = try alloc.alloc(u8, bsize + pbwire.encode_slack);
    defer alloc.free(buf);
    var bw: std.Io.Writer = .fixed(buf);
    var bscratch = std.heap.ArenaAllocator.init(alloc);
    defer bscratch.deinit();
    try bmsg.encode(&bw, bscratch.allocator());
    if (bw.end != WANT[tier] or !std.mem.eql(u8, buf[0..bw.end], reference)) {
        fail(label, "baseline wrote {d} bytes, expected {d} identical bytes", .{ bw.end, WANT[tier] });
        return;
    }

    // Decode gates: pbwire round-trips byte-exactly, the baseline re-encodes
    // the reference to the same bytes.
    var back = try ty.decode(reference);
    defer back.deinit();
    const re = try back.encode(alloc);
    defer alloc.free(re);
    if (!std.mem.eql(u8, re, reference)) {
        fail(label, "pbwire-zig decode differs", .{});
        return;
    }
    var br: std.Io.Reader = .fixed(reference);
    var bback = try Base.decode(&br, alloc);
    defer bback.deinit(alloc);
    _ = bscratch.reset(.retain_capacity);
    var bw2: std.Io.Writer = .fixed(buf);
    try bback.encode(&bw2, bscratch.allocator());
    if (bw2.end != reference.len or !std.mem.eql(u8, buf[0..bw2.end], reference)) {
        fail(label, "baseline decode differs", .{});
        return;
    }

    const iterations = FLOORS[tier];

    const S = struct {
        var pm: *const pbwire.Message = undefined;
        var t: *const pbwire.MessageType = undefined;
        var r: []const u8 = undefined;
        var b: []u8 = undefined;
        var bm: *const Base = undefined;
        var sa: *std.heap.ArenaAllocator = undefined;
        var ga: std.mem.Allocator = undefined;
        fn encPb() void {
            const out = pm.encodeInto(b) catch unreachable;
            std.mem.doNotOptimizeAway(out);
        }
        fn decPb() void {
            var d = t.decode(r) catch unreachable;
            defer d.deinit();
            std.mem.doNotOptimizeAway(&d);
        }
        fn encBase() void {
            _ = sa.reset(.retain_capacity);
            var w: std.Io.Writer = .fixed(b);
            bm.encode(&w, sa.allocator()) catch unreachable;
            std.mem.doNotOptimizeAway(w.end);
        }
        fn decBase() void {
            var rd: std.Io.Reader = .fixed(r);
            var d = Base.decode(&rd, ga) catch unreachable;
            defer d.deinit(ga);
            std.mem.doNotOptimizeAway(&d);
        }
    };
    S.pm = &pmsg;
    S.t = ty;
    S.r = reference;
    S.b = buf;
    S.bm = &bmsg;
    S.sa = &bscratch;
    S.ga = alloc;

    const penc = calibrate(io, S.encPb, iterations);
    var penc_best: f64 = std.math.floatMax(f64);
    for (0..PASSES) |_| penc_best = @min(penc_best, run(io, S.encPb, penc));
    const pdec = calibrate(io, S.decPb, iterations);
    var pdec_best: f64 = std.math.floatMax(f64);
    for (0..PASSES) |_| pdec_best = @min(pdec_best, run(io, S.decPb, pdec));
    const benc = calibrate(io, S.encBase, iterations);
    var benc_best: f64 = std.math.floatMax(f64);
    for (0..PASSES) |_| benc_best = @min(benc_best, run(io, S.encBase, benc));
    const bdec = calibrate(io, S.decBase, iterations);
    var bdec_best: f64 = std.math.floatMax(f64);
    for (0..PASSES) |_| bdec_best = @min(bdec_best, run(io, S.decBase, bdec));

    const bytes_f: f64 = @floatFromInt(reference.len);
    std.debug.print("{s}: {d} bytes, {d} iterations (repeat field x{d})\n", .{ label, reference.len, penc, n });
    std.debug.print("  impl      op           ns/op       MB/s\n", .{});
    std.debug.print("  pbwire-zig encode      {d:>8.1}    {d:>8.1}\n", .{ penc_best, bytes_f / penc_best * 1000.0 });
    std.debug.print("  pbwire-zig decode      {d:>8.1}    {d:>8.1}\n", .{ pdec_best, bytes_f / pdec_best * 1000.0 });
    std.debug.print("  base      encode      {d:>8.1}    {d:>8.1}\n", .{ benc_best, bytes_f / benc_best * 1000.0 });
    std.debug.print("  base      decode      {d:>8.1}    {d:>8.1}\n", .{ bdec_best, bytes_f / bdec_best * 1000.0 });
    std.debug.print("  -- vs baseline: encode {d:.2}x, decode {d:.2}x\n", .{ benc_best / penc_best, bdec_best / pdec_best });
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

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
const m3 = @import("pbwire_gen");
const pbcore = @import("pbwire_core");
const core_arm_mod = @import("pbench_core");

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

const ShimFixture = struct {
    image: m3.Scalars3Mirror,
    inner: *m3.Inner3Mirror,
    nums: []i32,
    tags: []?[*:0]const u8,
    inners: []m3.Inner3Mirror,
    flags: []u8,

    fn deinit(self: *ShimFixture, alloc: std.mem.Allocator) void {
        alloc.destroy(self.inner);
        alloc.free(self.nums);
        alloc.free(self.tags);
        alloc.free(self.inners);
        alloc.free(self.flags);
    }
};

/// The same payload as `fill`, in the generated extern-struct mirror. Every
/// pointer borrows an allocation the fixture owns, so the image stays valid
/// across all timed calls.
fn fillShim(alloc: std.mem.Allocator, n: usize) !ShimFixture {
    const inner = try alloc.create(m3.Inner3Mirror);
    errdefer alloc.destroy(inner);
    inner.* = .{ .name = "kid", .value = 42 };
    const nums = try alloc.alloc(i32, n);
    errdefer alloc.free(nums);
    const tags = try alloc.alloc(?[*:0]const u8, n);
    errdefer alloc.free(tags);
    const inners = try alloc.alloc(m3.Inner3Mirror, n);
    errdefer alloc.free(inners);
    const flags = try alloc.alloc(u8, n);
    errdefer alloc.free(flags);
    for (0..n) |i| {
        nums[i] = @as(i32, @intCast(i)) - 3;
        tags[i] = "tag";
        inners[i] = .{ .name = "grandkid", .value = @as(i32, @intCast(i)) + 1 };
        flags[i] = if (i % 2 == 0) 1 else 0;
    }
    return .{
        .image = .{
            .i32 = std.math.minInt(i32),
            .i64 = std.math.maxInt(i64),
            .u32 = std.math.maxInt(u32),
            .u64 = std.math.maxInt(u64),
            .s32 = std.math.minInt(i32),
            .s64 = std.math.minInt(i64),
            .fx32 = 0xdeadbeef,
            .fx64 = 72623859790382856,
            .sfx32 = std.math.minInt(i32),
            .sfx64 = std.math.minInt(i64),
            .f32 = std.math.inf(f32),
            .f64 = -123456.789,
            .flag = 1,
            .str = "hello",
            .color = 2,
            .inner = inner,
            .nums = if (n == 0) null else nums.ptr,
            .nums_count = n,
            .tags = if (n == 0) null else tags.ptr,
            .tags_count = n,
            .inners = if (n == 0) null else inners.ptr,
            .inners_count = n,
            .opt_i32 = -7,
            .has_opt_i32 = 1,
            .opt_str = "opt",
            .flags = if (n == 0) null else flags.ptr,
            .flags_count = n,
        },
        .inner = inner,
        .nums = nums,
        .tags = tags,
        .inners = inners,
        .flags = flags,
    };
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

fn runCase(io: std.Io, alloc: std.mem.Allocator, ty: *const pbwire.MessageType, inner_ty: *const pbwire.MessageType, shim_plan: *m3.Scalars3Mirror.plan, core_arm: *core_arm_mod.CoreArm, tier: usize) !void {
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

    // Shim arm: the generated mirror is the native object; one call per op,
    // the C struct codec walks it in place. Gate its bytes before timing.
    var shim = try fillShim(alloc, n);
    defer shim.deinit(alloc);
    const shim_wire = try shim_plan.encodeInPlace(&shim.image);
    if (shim_wire.len != reference.len or !std.mem.eql(u8, shim_wire, reference)) {
        fail(label, "pbwire-zig-shim wrote {d} bytes, expected {d} identical bytes", .{ shim_wire.len, reference.len });
        return;
    }
    var shim_back = std.mem.zeroes(m3.Scalars3Mirror);
    try shim_plan.decodeInPlace(reference, &shim_back);
    const shim_again = try shim_plan.encodeInPlace(&shim_back);
    if (!std.mem.eql(u8, shim_again, reference)) {
        fail(label, "pbwire-zig-shim decode differs", .{});
        return;
    }
    if (shim_back.nums_count != n or shim_back.inners_count != n or shim_back.tags_count != n) {
        fail(label, "pbwire-zig-shim repeated counts differ", .{});
        return;
    }
    if (shim_back.inner == null or !std.mem.eql(u8, std.mem.span(shim_back.inner.?.name orelse ""), "kid")) {
        fail(label, "pbwire-zig-shim fields differ", .{});
        return;
    }

    // Core arm: the pure-Zig codec. Gate its bytes and read-back before timing.
    core_arm.gate(alloc, reference, n) catch |err| {
        fail(label, "pbwire-zig-core gate: {s}", .{@errorName(err)});
        return;
    };
    std.debug.print("OK   {s} core parity, x{d} ({d} bytes)\n", .{ label, n, reference.len });
    var core_message = try core_arm.build(alloc, n);
    defer core_message.deinit();
    var core_buf: std.ArrayListUnmanaged(u8) = .empty;
    defer core_buf.deinit(alloc);

    const size = try pbwire.encodedSizeOf(&message);
    const buf = try alloc.alloc(u8, size + pbwire.encode_slack);
    defer alloc.free(buf);
    const iterations = FLOORS[tier];

    const S = struct {
        var m: *const pbwire.Message = undefined;
        var t: *const pbwire.MessageType = undefined;
        var r: []const u8 = undefined;
        var b: []u8 = undefined;
        var plan: *m3.Scalars3Mirror.plan = undefined;
        var image: *const m3.Scalars3Mirror = undefined;
        var sback: *m3.Scalars3Mirror = undefined;
        var core: *core_arm_mod.CoreArm = undefined;
        var cm: *const pbcore.Message = undefined;
        var cbuf: *std.ArrayListUnmanaged(u8) = undefined;
        var allocator: std.mem.Allocator = undefined;
        fn enc() void {
            const out = m.encodeInto(b) catch unreachable;
            std.mem.doNotOptimizeAway(out);
        }
        fn dec() void {
            var d = t.decode(r) catch unreachable;
            defer d.deinit();
            std.mem.doNotOptimizeAway(&d);
        }
        fn encShim() void {
            const out = plan.encodeInPlace(image) catch unreachable;
            std.mem.doNotOptimizeAway(out);
        }
        fn decShim() void {
            plan.decodeInPlace(r, sback) catch unreachable;
            std.mem.doNotOptimizeAway(sback);
        }
        fn encCore() void {
            core.encodeTo(cm, cbuf, allocator) catch unreachable;
            std.mem.doNotOptimizeAway(cbuf.items.len);
        }
        fn decCore() void {
            var d = core.decode(allocator, r) catch unreachable;
            defer d.deinit();
            std.mem.doNotOptimizeAway(&d);
        }
    };
    S.m = &message;
    S.t = ty;
    S.r = reference;
    S.b = buf;
    S.plan = shim_plan;
    S.image = &shim.image;
    S.sback = &shim_back;
    S.core = core_arm;
    S.allocator = alloc;

    const enc_count = calibrate(io, S.enc, iterations);
    var enc_best: f64 = std.math.floatMax(f64);
    for (0..PASSES) |_| enc_best = @min(enc_best, run(io, S.enc, enc_count));
    const dec_count = calibrate(io, S.dec, iterations);
    var dec_best: f64 = std.math.floatMax(f64);
    for (0..PASSES) |_| dec_best = @min(dec_best, run(io, S.dec, dec_count));
    S.cm = &core_message;
    S.cbuf = &core_buf;
    const cenc_count = calibrate(io, S.encCore, iterations);
    var cenc_best: f64 = std.math.floatMax(f64);
    for (0..PASSES) |_| cenc_best = @min(cenc_best, run(io, S.encCore, cenc_count));
    const cdec_count = calibrate(io, S.decCore, iterations);
    var cdec_best: f64 = std.math.floatMax(f64);
    for (0..PASSES) |_| cdec_best = @min(cdec_best, run(io, S.decCore, cdec_count));
    const senc_count = calibrate(io, S.encShim, iterations);
    var senc_best: f64 = std.math.floatMax(f64);
    for (0..PASSES) |_| senc_best = @min(senc_best, run(io, S.encShim, senc_count));
    const sdec_count = calibrate(io, S.decShim, iterations);
    var sdec_best: f64 = std.math.floatMax(f64);
    for (0..PASSES) |_| sdec_best = @min(sdec_best, run(io, S.decShim, sdec_count));

    const bytes_f: f64 = @floatFromInt(reference.len);
    std.debug.print("{s}: {d} bytes, {d} iterations (repeat field x{d})\n", .{ label, reference.len, enc_count, n });
    std.debug.print("  impl      op           ns/op       MB/s\n", .{});
    std.debug.print("  pbwire-zig-tree encode      {d:>8.1}    {d:>8.1}\n", .{ enc_best, bytes_f / enc_best * 1000.0 });
    std.debug.print("  pbwire-zig-tree decode      {d:>8.1}    {d:>8.1}\n", .{ dec_best, bytes_f / dec_best * 1000.0 });
    std.debug.print("  pbwire-zig-shim encode      {d:>8.1}    {d:>8.1}\n", .{ senc_best, bytes_f / senc_best * 1000.0 });
    std.debug.print("  pbwire-zig-shim decode      {d:>8.1}    {d:>8.1}\n", .{ sdec_best, bytes_f / sdec_best * 1000.0 });
    std.debug.print("  pbwire-zig-core encode      {d:>8.1}    {d:>8.1}\n", .{ cenc_best, bytes_f / cenc_best * 1000.0 });
    std.debug.print("  pbwire-zig-core decode      {d:>8.1}    {d:>8.1}\n", .{ cdec_best, bytes_f / cdec_best * 1000.0 });
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
    var shim_plan = try m3.Scalars3Mirror.plan.init(alloc, &schema);
    defer shim_plan.deinit();
    var core_arm = try core_arm_mod.CoreArm.init(alloc, desc_path);
    defer core_arm.deinit();

    var threaded: std.Io.Threaded = .init(alloc, .{});
    defer threaded.deinit();
    const io = threaded.io();

    for (0..CASES.len) |tier| try runCase(io, alloc, &ty, &inner_ty, &shim_plan, &core_arm, tier);
    if (failures != 0) {
        std.debug.print("{d} check(s) failed\n", .{failures});
        std.process.exit(1);
    }
}

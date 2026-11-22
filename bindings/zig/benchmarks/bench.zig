//! Benchmarks the Zig binding on the same `matrix3.proto` and the four standard
//! tiers from BENCHMARKS.md (small / medium / large / xlarge scale the repeated
//! fields x0 / x1 / x100 / x3000, landing on 136 / 174 / 2469 / 74913 bytes).
//!
//! Every tier is gated before timing: the encoded bytes must land on the
//! pinned size, and decoding the reference must expose every fixture field
//! and re-encode to the same bytes. The `tree` rows time the full native
//! task: encode builds a fresh `Message` per iteration (one `set` per field,
//! then `encodeInto`, then free); decode builds a fresh `Message` and reads
//! every gate-checked field back out before freeing. Encode reuses one caller
//! buffer (`encodeInto`), the same reuse the harness always had. The `proxy`
//! rows are the old prebuilt shape (a held handle's `encodeInto`,
//! decode-and-drop with no materialization), relabelled not re-measured as a
//! new task: same-run timing only, never the headline. `shim`/`core` keep
//! the steady-state shape: the app-owned mirror image (plus the plan's
//! decode arena) and the prebuilt core tree live across iterations. Prints
//! one block per case in the repo-wide harness format (throughput over wire
//! bytes).
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

fn fill(ty: *const pbwire.MessageType, f: *const Fields, n: usize) !pbwire.Message {
    var m = try ty.newMessage();
    errdefer m.deinit();
    try m.setI64(f.f_i32, std.math.minInt(i32));
    try m.setI64(f.f_i64, std.math.maxInt(i64));
    try m.setI64(f.f_u32, std.math.maxInt(u32));
    try m.setI64(f.f_u64, -1);
    try m.setI64(f.f_s32, std.math.minInt(i32));
    try m.setI64(f.f_s64, std.math.minInt(i64));
    try m.setI64(f.f_fx32, 0xdeadbeef);
    try m.setI64(f.f_fx64, 72623859790382856);
    try m.setI64(f.f_sfx32, std.math.minInt(i32));
    try m.setI64(f.f_sfx64, std.math.minInt(i64));
    try m.setF32(f.f_f32, std.math.inf(f32));
    try m.setF64(f.f_f64, -123456.789);
    try m.setBool(f.f_flag, true);
    try m.setStr(f.f_str, "hello");
    try m.setI64(f.f_color, 2); // COLOR3_GREEN
    try m.setI64(f.f_opt_i32, -7);
    try m.setStr(f.f_opt_str, "opt");
    var inner = try m.setMessage(f.f_inner);
    try inner.setStr(f.f_name, "kid");
    try inner.setI64(f.f_value, 42);
    var i: usize = 0;
    while (i < n) : (i += 1) {
        try m.setI64(f.f_nums, @as(i64, @intCast(i)) - 3);
        try m.setStr(f.f_tags, "tag");
        try m.setBool(f.f_flags, i % 2 == 0);
        var kid = try m.setMessage(f.f_inners);
        try kid.setStr(f.f_name, "grandkid");
        try kid.setI64(f.f_value, @as(i64, @intCast(i)) + 1);
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

/// Cached per-field handles for the matrix3 fixture, resolved once per tier
/// so the timed build/read loops pay only the per-field calls.
const Fields = struct {
    f_i32: pbwire.Field,
    f_i64: pbwire.Field,
    f_u32: pbwire.Field,
    f_u64: pbwire.Field,
    f_s32: pbwire.Field,
    f_s64: pbwire.Field,
    f_fx32: pbwire.Field,
    f_fx64: pbwire.Field,
    f_sfx32: pbwire.Field,
    f_sfx64: pbwire.Field,
    f_f32: pbwire.Field,
    f_f64: pbwire.Field,
    f_flag: pbwire.Field,
    f_str: pbwire.Field,
    f_color: pbwire.Field,
    f_opt_i32: pbwire.Field,
    f_opt_str: pbwire.Field,
    f_inner: pbwire.Field,
    f_nums: pbwire.Field,
    f_tags: pbwire.Field,
    f_flags: pbwire.Field,
    f_inners: pbwire.Field,
    f_name: pbwire.Field,
    f_value: pbwire.Field,

    fn resolve(ty: *const pbwire.MessageType, inner_ty: *const pbwire.MessageType) !Fields {
        return .{
            .f_i32 = try ty.field("i32"),
            .f_i64 = try ty.field("i64"),
            .f_u32 = try ty.field("u32"),
            .f_u64 = try ty.field("u64"),
            .f_s32 = try ty.field("s32"),
            .f_s64 = try ty.field("s64"),
            .f_fx32 = try ty.field("fx32"),
            .f_fx64 = try ty.field("fx64"),
            .f_sfx32 = try ty.field("sfx32"),
            .f_sfx64 = try ty.field("sfx64"),
            .f_f32 = try ty.field("f32"),
            .f_f64 = try ty.field("f64"),
            .f_flag = try ty.field("flag"),
            .f_str = try ty.field("str"),
            .f_color = try ty.field("color"),
            .f_opt_i32 = try ty.field("opt_i32"),
            .f_opt_str = try ty.field("opt_str"),
            .f_inner = try ty.field("inner"),
            .f_nums = try ty.field("nums"),
            .f_tags = try ty.field("tags"),
            .f_flags = try ty.field("flags"),
            .f_inners = try ty.field("inners"),
            .f_name = try inner_ty.field("name"),
            .f_value = try inner_ty.field("value"),
        };
    }
};

var tree_sink: u64 = 0;

fn fold(v: u64) void {
    tree_sink = tree_sink *% 31 +% v;
}

/// Deep materialization of every gate-checked field: the decode side of the
/// full native task. Folds each value into a live sink so no read is
/// optimized away.
fn consumeTree(m: *const pbwire.Message, f: *const Fields) void {
    fold(@as(u64, @bitCast(m.getI64(f.f_i32))));
    fold(@as(u64, @bitCast(m.getI64(f.f_i64))));
    fold(@as(u64, @bitCast(m.getI64(f.f_u32))));
    fold(@as(u64, @bitCast(m.getI64(f.f_u64))));
    fold(@as(u64, @bitCast(m.getI64(f.f_s32))));
    fold(@as(u64, @bitCast(m.getI64(f.f_s64))));
    fold(@as(u64, @bitCast(m.getI64(f.f_fx32))));
    fold(@as(u64, @bitCast(m.getI64(f.f_fx64))));
    fold(@as(u64, @bitCast(m.getI64(f.f_sfx32))));
    fold(@as(u64, @bitCast(m.getI64(f.f_sfx64))));
    fold(@as(u64, @as(u32, @bitCast(m.getF32(f.f_f32)))));
    fold(@as(u64, @bitCast(m.getF64(f.f_f64))));
    fold(@as(u64, @intFromBool(m.getBool(f.f_flag))));
    if (m.getStr(f.f_str)) |s| {
        fold(s.len);
        for (s) |c| fold(@as(u64, c));
    }
    fold(@as(u64, @bitCast(m.getI64(f.f_color))));
    fold(@as(u64, @bitCast(m.getI64(f.f_opt_i32))));
    fold(@as(u64, @intFromBool(m.has(f.f_opt_str))));
    if (m.getStr(f.f_opt_str)) |s| fold(s.len);
    if (m.getMessage(f.f_inner)) |inner| {
        if (inner.getStr(f.f_name)) |s| {
            fold(s.len);
            for (s) |c| fold(@as(u64, c));
        }
        fold(@as(u64, @bitCast(inner.getI64(f.f_value))));
    }
    if (m.value(f.f_nums)) |v| {
        fold(v.len());
        if (v.len() > 0) fold(@as(u64, @bitCast(v.at(v.len() - 1).?.asI64())));
    }
    if (m.value(f.f_tags)) |v| {
        fold(v.len());
        if (v.len() > 0) {
            const item = v.at(v.len() - 1).?;
            if (item.asStr()) |s| fold(s.len);
        }
    }
    if (m.value(f.f_flags)) |v| {
        fold(v.len());
        if (v.len() > 0) fold(@as(u64, @intFromBool(v.at(v.len() - 1).?.asBool())));
    }
    if (m.value(f.f_inners)) |v| {
        fold(v.len());
        if (v.len() > 0) {
            const last = v.at(v.len() - 1).?.asMessage() orelse return;
            if (last.getStr(f.f_name)) |s| fold(s.len);
            fold(@as(u64, @bitCast(last.getI64(f.f_value))));
        }
    }
}

/// Gate: every fixture field must read back at its pinned value. Same field
/// list the timer materializes, so gate and timer agree by construction.
fn checkTree(label: []const u8, m: *const pbwire.Message, f: *const Fields, n: usize) bool {
    if (m.getI64(f.f_i32) != std.math.minInt(i32)) { fail(label, "tree i32 differs", .{}); return false; }
    if (m.getI64(f.f_i64) != std.math.maxInt(i64)) { fail(label, "tree i64 differs", .{}); return false; }
    if (m.getI64(f.f_u32) != std.math.maxInt(u32)) { fail(label, "tree u32 differs", .{}); return false; }
    if (m.getI64(f.f_u64) != -1) { fail(label, "tree u64 differs", .{}); return false; }
    if (m.getI64(f.f_s32) != std.math.minInt(i32)) { fail(label, "tree s32 differs", .{}); return false; }
    if (m.getI64(f.f_s64) != std.math.minInt(i64)) { fail(label, "tree s64 differs", .{}); return false; }
    if (m.getI64(f.f_fx32) != 0xdeadbeef) { fail(label, "tree fx32 differs", .{}); return false; }
    if (m.getI64(f.f_fx64) != 72623859790382856) { fail(label, "tree fx64 differs", .{}); return false; }
    if (m.getI64(f.f_sfx32) != std.math.minInt(i32)) { fail(label, "tree sfx32 differs", .{}); return false; }
    if (m.getI64(f.f_sfx64) != std.math.minInt(i64)) { fail(label, "tree sfx64 differs", .{}); return false; }
    if (m.getF32(f.f_f32) != std.math.inf(f32)) { fail(label, "tree f32 differs", .{}); return false; }
    if (m.getF64(f.f_f64) != -123456.789) { fail(label, "tree f64 differs", .{}); return false; }
    if (!m.getBool(f.f_flag)) { fail(label, "tree flag differs", .{}); return false; }
    if (!std.mem.eql(u8, m.getStr(f.f_str) orelse "", "hello")) { fail(label, "tree str differs", .{}); return false; }
    if (m.getI64(f.f_color) != 2) { fail(label, "tree color differs", .{}); return false; }
    if (m.getI64(f.f_opt_i32) != -7) { fail(label, "tree opt_i32 differs", .{}); return false; }
    if (!m.has(f.f_opt_str)) { fail(label, "tree opt_str missing", .{}); return false; }
    if (!std.mem.eql(u8, m.getStr(f.f_opt_str) orelse "", "opt")) { fail(label, "tree opt_str differs", .{}); return false; }
    const inner = m.getMessage(f.f_inner) orelse { fail(label, "tree inner missing", .{}); return false; };
    if (!std.mem.eql(u8, inner.getStr(f.f_name) orelse "", "kid")) { fail(label, "tree inner.name differs", .{}); return false; }
    if (inner.getI64(f.f_value) != 42) { fail(label, "tree inner.value differs", .{}); return false; }
    if (m.value(f.f_nums)) |nums| {
        if (nums.len() != n) { fail(label, "tree nums count differs", .{}); return false; }
        if (n > 0 and nums.at(n - 1).?.asI64() != @as(i64, @intCast(n)) - 4) { fail(label, "tree last nums differs", .{}); return false; }
    } else if (n != 0) { fail(label, "tree nums missing", .{}); return false; }
    if (m.value(f.f_tags)) |tags| {
        if (tags.len() != n) { fail(label, "tree tags count differs", .{}); return false; }
        if (n > 0) {
        const item = tags.at(n - 1).?;
        if (!std.mem.eql(u8, item.asStr() orelse "", "tag")) { fail(label, "tree last tag differs", .{}); return false; }
    }
    } else if (n != 0) { fail(label, "tree tags missing", .{}); return false; }
    if (m.value(f.f_flags)) |flags| {
        if (flags.len() != n) { fail(label, "tree flags count differs", .{}); return false; }
        if (n > 0 and flags.at(n - 1).?.asBool() != ((n - 1) % 2 == 0)) { fail(label, "tree last flag differs", .{}); return false; }
    } else if (n != 0) { fail(label, "tree flags missing", .{}); return false; }
    if (m.value(f.f_inners)) |inners| {
        if (inners.len() != n) { fail(label, "tree inners count differs", .{}); return false; }
        if (n > 0) {
            const last = inners.at(n - 1).?.asMessage() orelse { fail(label, "tree last inner missing", .{}); return false; };
            if (!std.mem.eql(u8, last.getStr(f.f_name) orelse "", "grandkid")) { fail(label, "tree last inner name differs", .{}); return false; }
            if (last.getI64(f.f_value) != @as(i64, @intCast(n))) { fail(label, "tree last inner value differs", .{}); return false; }
        }
    } else if (n != 0) { fail(label, "tree inners missing", .{}); return false; }
    return true;
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

    const f = Fields.resolve(ty, inner_ty) catch {
        fail(label, "field resolve failed", .{});
        return;
    };

    var message = try fill(ty, &f, n);
    defer message.deinit();
    const reference = try message.encode(alloc);
    defer alloc.free(reference);
    if (reference.len != WANT[tier]) {
        fail(label, "pbwire-zig wrote {d} bytes, expected {d}", .{ reference.len, WANT[tier] });
        return;
    }
    std.debug.print("OK   {s} encoding parity, x{d} ({d} bytes)\n", .{ label, n, reference.len });

    // Decode must round-trip byte-exactly and expose every fixture value.
    var back = try ty.decode(reference);
    defer back.deinit();
    const back_ok = blk: {
        const enc = back.encode(alloc) catch break :blk false;
        defer alloc.free(enc);
        if (!std.mem.eql(u8, enc, reference)) break :blk false;
        if (!checkTree(label, &back, &f, n)) break :blk false;
        break :blk true;
    };
    if (!back_ok) {
        fail(label, "pbwire-zig decode differs", .{});
        return;
    }
    // A second fresh-build message must encode to the same bytes, so the
    // timed per-iteration build is gated too.
    var fresh = try fill(ty, &f, n);
    defer fresh.deinit();
    const fresh_wire = try fresh.encode(alloc);
    defer alloc.free(fresh_wire);
    if (!std.mem.eql(u8, fresh_wire, reference)) {
        fail(label, "pbwire-zig fresh-build bytes differ", .{});
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
        var ff: *const Fields = undefined;
        var nn: usize = 0;
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
        fn encTree() void {
            var fresh_m = fill(t, ff, nn) catch unreachable;
            defer fresh_m.deinit();
            const out = fresh_m.encodeInto(b) catch unreachable;
            fold(out.len);
        }
        fn decTree() void {
            var d = t.decode(r) catch unreachable;
            defer d.deinit();
            consumeTree(&d, ff);
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
    S.ff = &f;
    S.nn = n;
    S.r = reference;
    S.b = buf;
    S.plan = shim_plan;
    S.image = &shim.image;
    S.sback = &shim_back;
    S.core = core_arm;
    S.allocator = alloc;

    // Floor calibration stays on the fast prebuilt op: calibrating on the
    // slower full-task op would inflate every row's iteration count and the
    // run time. Every row shares the count, so the printout stays comparable.
    const enc_count = calibrate(io, S.enc, iterations);
    var enc_best: f64 = std.math.floatMax(f64);
    for (0..PASSES) |_| enc_best = @min(enc_best, run(io, S.enc, enc_count));
    const dec_count = calibrate(io, S.dec, iterations);
    var dec_best: f64 = std.math.floatMax(f64);
    for (0..PASSES) |_| dec_best = @min(dec_best, run(io, S.dec, dec_count));
    var tenc_best: f64 = std.math.floatMax(f64);
    for (0..PASSES) |_| tenc_best = @min(tenc_best, run(io, S.encTree, enc_count));
    var tdec_best: f64 = std.math.floatMax(f64);
    for (0..PASSES) |_| tdec_best = @min(tdec_best, run(io, S.decTree, dec_count));
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
    std.debug.print("  pbwire-zig-tree encode      {d:>8.1}    {d:>8.1}\n", .{ tenc_best, bytes_f / tenc_best * 1000.0 });
    std.debug.print("  pbwire-zig-tree decode      {d:>8.1}    {d:>8.1}\n", .{ tdec_best, bytes_f / tdec_best * 1000.0 });
    std.debug.print("  pbwire-zig-proxy encode      {d:>8.1}    {d:>8.1}\n", .{ enc_best, bytes_f / enc_best * 1000.0 });
    std.debug.print("  pbwire-zig-proxy decode      {d:>8.1}    {d:>8.1}\n", .{ dec_best, bytes_f / dec_best * 1000.0 });
    std.debug.print("  pbwire-zig-shim encode      {d:>8.1}    {d:>8.1}\n", .{ senc_best, bytes_f / senc_best * 1000.0 });
    std.debug.print("  pbwire-zig-shim decode      {d:>8.1}    {d:>8.1}\n", .{ sdec_best, bytes_f / sdec_best * 1000.0 });
    std.debug.print("  pbwire-zig-core encode      {d:>8.1}    {d:>8.1}\n", .{ cenc_best, bytes_f / cenc_best * 1000.0 });
    std.debug.print("  pbwire-zig-core decode      {d:>8.1}    {d:>8.1}\n", .{ cdec_best, bytes_f / cdec_best * 1000.0 });
}

pub fn main(init: std.process.Init.Minimal) !void {
    // A release allocator: the C core allocates with malloc and the pure core
    // through this one, so neither side pays a debug allocator's bookkeeping.
    const alloc = std.heap.smp_allocator;

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

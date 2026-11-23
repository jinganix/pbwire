//! Concurrency check over the matrix3 oracle: 8 threads x 200 iters, three
//! phases per worker mirroring the C/C++/Rust shape:
//!
//!   (a) per-thread message encode == want bytes,
//!   (b) SHARED read-only message concurrent encode == want bytes,
//!   (c) fresh-target decode of want with field checks (NOT re-encode).
//!
//! The schema and the want bytes are the only cross-thread state, both
//! read-only after startup. Every message a worker encodes/decodes into is
//! thread-owned, except the phase-(b) tree, which is encoded through a shared
//! `*const Message` (encode only reads the tree; the C core keeps concurrent
//! encodes of one message independent). Speed goes to stdout only (never
//! asserted). Run with:
//!
//!   zig build test    (from bindings/zig)
const std = @import("std");
const pbwire = @import("pbwire");

// Fixtures live outside the package, same as selftest.zig: `zig build test`
// runs with cwd = bindings/zig.
const DESC_PATH = "../c/tests/data/matrix3.desc";
const BIN_PATH = "../c/tests/data/matrix3.bin";
const MSG = "structtest.Scalars3";
const INNER = "structtest.Inner3";

const THREADS = 8;
const ITERS = 200;

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

const Worker = struct {
    id: usize,
    schema: *const pbwire.Schema,
    want: []const u8,
    shared: *const pbwire.Message,
    failures: usize = 0,
    checks: usize = 0,
};

fn fail(w: *Worker, comptime phase: []const u8, comptime reason: []const u8) void {
    std.debug.print("worker {d}: phase {s} {s}\n", .{ w.id, phase, reason });
    w.failures += 1;
}

// (a) per-thread message encode == want bytes. `alloc` is the worker's
// own GPA allocator: `std.testing.allocator` is not safe for concurrent
// alloc/free across threads, so it must never cross a thread boundary.
fn phaseEncodeOwn(w: *Worker, alloc: std.mem.Allocator, ty: *const pbwire.MessageType, inner_ty: *const pbwire.MessageType) void {
    var m = fill(ty, inner_ty) catch {
        fail(w, "(a)", "fill failed");
        return;
    };
    defer m.deinit();
    const wire = m.encode(alloc) catch {
        fail(w, "(a)", "encode failed");
        return;
    };
    defer alloc.free(wire);
    w.checks += 1;
    std.testing.expectEqualSlices(u8, w.want, wire) catch {
        fail(w, "(a)", "encode bytes differ from oracle");
    };
}

// (b) SHARED read-only message concurrent encode == want bytes. Encode only
// reads the tree, so sharing one `*const Message` across workers is
// read-only sharing; the encoded output buffer is per-call as in (a).
// `alloc` is the worker's own GPA allocator (see phaseEncodeOwn).
fn phaseEncodeShared(w: *Worker, alloc: std.mem.Allocator) void {
    const wire = w.shared.encode(alloc) catch {
        fail(w, "(b)", "encode failed");
        return;
    };
    defer alloc.free(wire);
    w.checks += 1;
    std.testing.expectEqualSlices(u8, w.want, wire) catch {
        fail(w, "(b)", "encode bytes differ from oracle");
    };
}

// (c) fresh-target decode of want with field checks (NOT re-encode). Only the
// schema is shared; each decode builds a thread-owned tree.
fn phaseDecode(w: *Worker, ty: *const pbwire.MessageType, inner_ty: *const pbwire.MessageType) void {
    var got = ty.decode(w.want) catch {
        fail(w, "(c)", "decode failed");
        return;
    };
    defer got.deinit();
    w.checks += 1;
    const ok = got.getI64(ty.field("i32") catch return fail(w, "(c)", "field lookup failed")) == std.math.minInt(i32) and
        got.getBool(ty.field("flag") catch return fail(w, "(c)", "field lookup failed")) and
        std.mem.eql(u8, got.getStr(ty.field("str") catch return fail(w, "(c)", "field lookup failed")) orelse "", "hello") and
        got.getI64(ty.field("color") catch return fail(w, "(c)", "field lookup failed")) == 2 and
        got.getI64(ty.field("opt_i32") catch return fail(w, "(c)", "field lookup failed")) == -7 and
        std.mem.eql(u8, got.getStr(ty.field("opt_str") catch return fail(w, "(c)", "field lookup failed")) orelse "", "opt") and
        checkNested(ty, inner_ty, &got);
    if (!ok) fail(w, "(c)", "decode fields differ from oracle");
}

fn checkNested(ty: *const pbwire.MessageType, inner_ty: *const pbwire.MessageType, got: *const pbwire.Message) bool {
    const inner = got.getMessage(ty.field("inner") catch return false) orelse return false;
    if (!std.mem.eql(u8, inner.getStr(inner_ty.field("name") catch return false) orelse "", "kid")) return false;
    if (inner.getI64(inner_ty.field("value") catch return false) != 42) return false;
    const numsv = got.value(ty.field("nums") catch return false) orelse return false;
    if (numsv.len() != 3) return false;
    if (numsv.at(0).?.asI64() != 1 or numsv.at(1).?.asI64() != -5 or numsv.at(2).?.asI64() != 300) return false;
    const tagsv = got.value(ty.field("tags") catch return false) orelse return false;
    if (tagsv.len() != 2) return false;
    const innersv = got.value(ty.field("inners") catch return false) orelse return false;
    if (innersv.len() != 2) return false;
    const flagsv = got.value(ty.field("flags") catch return false) orelse return false;
    if (flagsv.len() != 3) return false;
    return true;
}

fn runWorker(w: *Worker) void {
    // Per-worker allocator: `std.testing.allocator` is not thread-safe for
    // concurrent alloc/free, and every `encode` dupes through the passed
    // allocator — so each worker dupes through its own debug allocator
    // (`GeneralPurposeAllocator` under its 0.17 name). Leak-checked
    // before exit and folded into the failure count.
    var gpa: std.heap.DebugAllocator(.{}) = .{};
    const alloc = gpa.allocator();
    // Per-thread descriptor lookup off the shared (immutable) schema.
    const ty = w.schema.messageType(MSG) catch {
        fail(w, "setup", "messageType lookup failed");
        _ = gpa.deinit();
        return;
    };
    const inner_ty = w.schema.messageType(INNER) catch {
        fail(w, "setup", "inner messageType lookup failed");
        _ = gpa.deinit();
        return;
    };
    for (0..ITERS) |_| phaseEncodeOwn(w, alloc, &ty, &inner_ty);
    for (0..ITERS) |_| phaseEncodeShared(w, alloc);
    for (0..ITERS) |_| phaseDecode(w, &ty, &inner_ty);
    if (gpa.deinit() != .ok) fail(w, "teardown", "leak detected");
}

test "concurrent encode" {
    const alloc = std.testing.allocator;
    var schema = try loadSchema();
    defer schema.deinit();
    const ty = try schema.messageType(MSG);
    const inner_ty = try schema.messageType(INNER);

    // Serial encode-once baseline: must equal the protoc oracle before any
    // thread starts.
    var baseline = try fill(&ty, &inner_ty);
    defer baseline.deinit();
    const baseline_wire = try baseline.encode(alloc);
    defer alloc.free(baseline_wire);
    const want = try loadOracle();
    defer alloc.free(want);
    try std.testing.expectEqualSlices(u8, want, baseline_wire);

    // The phase-(b) tree: built once on this thread, only ever encoded (never
    // mutated or freed) while workers run.
    var shared = try fill(&ty, &inner_ty);
    defer shared.deinit();

    // Zig 0.17 has no `std.time.nanoTimestamp`; the repo idiom (see
    // benchmarks/bench.zig) reads the awake clock off an explicit `Io`.
    var threaded: std.Io.Threaded = .init(alloc, .{});
    defer threaded.deinit();
    const io = threaded.io();
    const start = std.Io.Clock.now(.awake, io).nanoseconds;
    var workers: [THREADS]Worker = undefined;
    for (&workers, 0..) |*w, i| {
        w.* = .{ .id = i, .schema = &schema, .want = want, .shared = &shared };
    }
    var threads: [THREADS]std.Thread = undefined;
    var started: usize = 0;
    for (&threads, &workers) |*t, *w| {
        t.* = std.Thread.spawn(.{}, runWorker, .{w}) catch |err| {
            std.debug.print("worker {d}: spawn failed: {any}\n", .{ w.id, err });
            w.failures += 1;
            for (threads[0..started]) |*st| st.join();
            return err;
        };
        started += 1;
    }
    for (&threads) |*t| t.join();
    const elapsed_ns = std.Io.Clock.now(.awake, io).nanoseconds - start;

    var failures: usize = 0;
    var checks: usize = 0;
    for (workers) |w| {
        failures += w.failures;
        checks += w.checks;
    }
    // Timing is informational only, never asserted.
    std.debug.print("concurrent: {d} threads x {d} iters x 3 phases, {d} verifications in {d} ns ({d} ns/op)\n", .{
        THREADS, ITERS, checks, elapsed_ns, @divTrunc(elapsed_ns, @as(i128, checks)),
    });
    try std.testing.expectEqual(@as(usize, 0), failures);
}

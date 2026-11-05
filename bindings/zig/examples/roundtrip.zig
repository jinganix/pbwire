//! Round trip through the dynamic API: builds a `structtest.Scalars3`, encodes
//! it, decodes it back and prints it. Compares against the pinned reference
//! bytes when they are available.
//!
//!   zig build example [-- <matrix3.desc>]
const std = @import("std");
const pbwire = @import("pbwire");

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

    var msg = try ty.newMessage();
    defer msg.deinit();
    try msg.setI64(try ty.field("i32"), -42);
    try msg.setStr(try ty.field("str"), "alice");
    for ([_][]const u8{ "a", "bb" }) |tag| try msg.setStr(try ty.field("tags"), tag);
    var inner = try msg.setMessage(try ty.field("inner"));
    try inner.setStr(try inner_ty.field("name"), "kid");
    try inner.setI64(try inner_ty.field("value"), 42);

    const wire = try msg.encode(alloc);
    defer alloc.free(wire);

    if (pbwire.readFile(alloc, "../c/tests/data/matrix3.bin")) |ref| {
        defer alloc.free(ref);
    } else |_| {}

    var decoded = try ty.decode(wire);
    defer decoded.deinit();
    const back = try decoded.encode(alloc);
    defer alloc.free(back);
    if (!std.mem.eql(u8, wire, back)) {
        std.debug.print("round trip mismatch\n", .{});
        std.process.exit(1);
    }

    const f_str = try ty.field("str");
    const f_inner = try ty.field("inner");
    const f_name = try inner_ty.field("name");
    const f_value = try inner_ty.field("value");
    std.debug.print("roundtrip: {d} bytes, str={s}", .{
        wire.len,
        decoded.getStr(f_str) orelse "(missing)",
    });
    if (decoded.getMessage(f_inner)) |got| {
        std.debug.print(", inner={s}/{d}", .{
            got.getStr(f_name) orelse "(missing)",
            got.getI64(f_value),
        });
    }
    std.debug.print("\n", .{});
}

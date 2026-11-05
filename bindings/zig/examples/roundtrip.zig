//! The C example (examples/addressbook.c), in Zig, through the generated
//! structs: assign fields, encode, decode, compare. The encoded bytes
//! must equal the pinned reference examples/addressbook.bin.
//!
//!   zig build example
const std = @import("std");
const pbwire = @import("pbwire");
const ab = @import("gen/addressbook.zig");

const BIN_PATH = "../../examples/addressbook.bin";

pub fn main(init: std.process.Init.Minimal) !void {
    var gpa = std.heap.DebugAllocator(.{}){};
    defer _ = gpa.deinit();
    const alloc = gpa.allocator();

    _ = init;

    var schema = try ab.loadSchema();
    defer schema.deinit();

    var alice = ab.Tutorial_Person{ .name = "Alice", .id = 1 };
    alice.phones = &[_]ab.Tutorial_Person_PhoneNumber{
        .{ .number = "555-0100", .type_ = .MOBILE },
    };
    var bob = ab.Tutorial_Person{ .name = "Bob", .id = 2, .email = "bob@example.com" };
    bob.phones = &[_]ab.Tutorial_Person_PhoneNumber{
        .{ .number = "555-0200", .type_ = .HOME },
    };
    const book = ab.Tutorial_AddressBook{ .people = &[_]ab.Tutorial_Person{ alice, bob } };

    const wire = try book.encode(alloc, &schema);
    defer alloc.free(wire);

    if (pbwire.readFile(alloc, BIN_PATH)) |ref| {
        defer alloc.free(ref);
        if (!std.mem.eql(u8, wire, ref)) {
            std.debug.print("encode does not match {s}\n", .{BIN_PATH});
            std.process.exit(1);
        }
    } else |_| {}

    const back = try ab.Tutorial_AddressBook.decode(alloc, &schema, wire);
    defer {
        // decoded slices are owned by the caller: free inner slices first.
        for (back.people) |p| {
            for (p.phones) |ph| alloc.free(ph.number);
            alloc.free(p.phones);
            alloc.free(p.name);
            if (p.email) |e| alloc.free(e);
        }
        alloc.free(back.people);
    }
    std.debug.print("addressbook: {d} bytes, {d} people\n", .{ wire.len, back.people.len });
    for (back.people) |p| {
        std.debug.print("  {s} ({d}) {s}\n", .{ p.name, p.id, p.email orelse "" });
        for (p.phones) |ph| {
            std.debug.print("    {s}\n", .{ph.number});
        }
    }
}

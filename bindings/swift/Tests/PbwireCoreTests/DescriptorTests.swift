// Stage 1 gate for the pure-Swift core: the descriptor reader over the same
// `FileDescriptorSet` subset the C core reads (`src/descriptor_set.c`).
//
// Only facts the codec depends on: field count and order, the four presence
// kinds, repetition and packedness, kind mapping, oneofs, groups and the
// transitive required closure. Every assertion comes from a `.desc` fixture the
// C tests already pin, so the two readers cannot drift.
import XCTest

@testable import PbwireCore

final class DescriptorTests: XCTestCase {
    private static let dataDir = URL(fileURLWithPath: #filePath)
        .deletingLastPathComponent()  // PbwireCoreTests
        .deletingLastPathComponent()  // Tests
        .deletingLastPathComponent()  // swift
        .appendingPathComponent("../c/tests/data")

    private func load(_ file: String) throws -> Schema {
        let path = Self.dataDir.appendingPathComponent(file).path
        return try Schema(contentsOfFile: path)
    }

    private func type(_ file: String, _ name: String) throws -> MessageType {
        try load(file).message(name)
    }

    private func field(_ ty: MessageType, _ name: String) throws -> Field {
        try ty.field(name)
    }

    /// The descriptor test surface: 22 fields of `structtest.Scalars3`, the four
    /// presence kinds, proto2 requiredness, groups and oneofs.
    func testDescriptor() throws {
        // ---------------------------------------------------------- matrix3
        let ty = try type("matrix3.desc", "structtest.Scalars3")
        let def = ty.def
        XCTAssertEqual(def.fullName, "structtest.Scalars3")
        XCTAssertEqual(def.fields.count, 22, "matrix3.desc: 22 fields")
        for (i, f) in def.fields.enumerated() {
            XCTAssertEqual(f.number, UInt32(i + 1), "field \(i + 1) is numbered \(f.number)")
        }
        XCTAssertEqual(def.fields[0].name, "i32", "first field is i32")
        XCTAssertEqual(def.fields[21].name, "flags", "last field is flags")

        // proto3 plain scalars have no has-bit...
        XCTAssertEqual(try field(ty, "i32").presence, .implicit)
        XCTAssertEqual(try field(ty, "str").presence, .implicit)
        // ...while `optional` in a proto3 file is explicit presence.
        XCTAssertEqual(try field(ty, "opt_i32").presence, .explicit)
        XCTAssertEqual(try field(ty, "opt_str").presence, .explicit)
        // ...and a message field always has it.
        XCTAssertEqual(try field(ty, "inner").presence, .messagePresence)

        let flags = try field(ty, "flags")
        XCTAssertTrue(flags.repeated, "flags repeated")
        XCTAssertEqual(flags.kind, .bool, "flags is bool")
        XCTAssertTrue(flags.packed, "proto3 repeated numerics default to packed")
        XCTAssertEqual(flags.wt, 2, "a packed field encodes under the LEN tag")

        let nums = try field(ty, "nums")
        XCTAssertTrue(nums.repeated, "nums repeated")
        XCTAssertEqual(nums.kind, .int32, "nums is int32")
        XCTAssertTrue(nums.packed, "nums packed")
        XCTAssertEqual(nums.elemWt, 0, "the unpacked element form is still accepted")

        XCTAssertEqual(try field(ty, "i64").kind, .int64)
        XCTAssertEqual(try field(ty, "u64").kind, .uint64)
        XCTAssertEqual(try field(ty, "s64").kind, .sint64)
        XCTAssertEqual(try field(ty, "fx64").kind, .fixed64)
        XCTAssertEqual(try field(ty, "sfx64").kind, .sfixed64)
        XCTAssertEqual(try field(ty, "f32").kind, .float)
        XCTAssertEqual(try field(ty, "f64").kind, .double)
        XCTAssertEqual(try field(ty, "color").kind, .enumeration)
        XCTAssertEqual(try field(ty, "inner").kind, .message)
        XCTAssertEqual(
            ty.schema.messages[try field(ty, "inner").msgType!].fullName, "structtest.Inner3",
            "inner type")
        // proto3 strings verify UTF-8 on parse.
        XCTAssertTrue(try field(ty, "str").utf8, "proto3 str verifies utf8")
        // The enum is open (proto3), and its values are indexed by name.
        let color = ty.schema.enums[try field(ty, "color").enumType!]
        XCTAssertFalse(color.closed, "proto3 enum is open")
        XCTAssertEqual(color.byName["COLOR3_GREEN"], 2)
        XCTAssertEqual(color.byNumber[2], "COLOR3_GREEN")

        // ---------------------------------------------------------- matrix2
        let ty2 = try type("matrix2.desc", "structtest.Scalars2")
        XCTAssertEqual(try field(ty2, "id").presence, .required, "proto2 required")
        XCTAssertTrue(ty2.def.hasRequired, "Scalars2 has a required field")
        // Inner2.name is required, so Scalars2 is required transitively too.
        XCTAssertTrue(
            try type("matrix2.desc", "structtest.Inner2").def.hasRequired,
            "Inner2 has a required field")
        XCTAssertTrue(ty2.def.deepRequired, "Scalars2 is transitively required")

        // proto2 packs only with [packed = true].
        let p2nums = try field(ty2, "nums")
        XCTAssertTrue(p2nums.repeated && !p2nums.packed, "proto2 nums is unpacked")
        XCTAssertTrue(try field(ty2, "packed_nums").packed, "proto2 packed_nums is packed")
        // proto2 strings do not verify UTF-8.
        XCTAssertFalse(try field(ty2, "str").utf8, "proto2 str does not verify utf8")

        // ------------------------------------------------------- grouptest
        let g = try type("grouptest.desc", "structtest.GroupTest")
        let ginner = try field(g, "inner")
        XCTAssertEqual(ginner.kind, .group, "inner is a group")
        XCTAssertEqual(ginner.wt, 3, "start-group tag")
        XCTAssertEqual(
            g.schema.messages[ginner.msgType!].fullName, "structtest.GroupTest.Inner", "group type")
        XCTAssertEqual(ginner.tag.bytes, [0x0b], "start tag 0x0b")
        XCTAssertEqual(ginner.tagEnd.bytes, [0x0c], "end tag 0x0c")
        let gitem = try field(g, "item")
        XCTAssertEqual(gitem.kind, .group, "item is a group")
        XCTAssertTrue(gitem.repeated, "item is repeated")

        // ------------------------------------------------------- oneoftest
        let o = try type("oneoftest.desc", "structtest.OneOfTest")
        XCTAssertEqual(o.def.oneofCount, 1, "one oneof")
        XCTAssertEqual(o.def.oneofByName["kind"], 0, "oneof kind is slot 0")
        for name in ["num", "text", "sub", "blob"] {
            let f = try field(o, name)
            XCTAssertEqual(f.oneof, 0, "\(name) is a member of the oneof")
            XCTAssertEqual(f.presence, .explicit, "\(name) has explicit presence")
        }
        XCTAssertEqual(try field(o, "id").oneof, -1, "id is not in the oneof")

        // ------------------------------------------------------- synthetic
        let node = try deepType()
        XCTAssertEqual(node.def.fields.count, 1, "synthetic: one field")
        let kid = node.def.fields[0]
        XCTAssertEqual(kid.msgType, 0, "the field points back at Node (index 0)")
        XCTAssertEqual(node.schema.messages[kid.msgType!].fullName, "deep.Node")
        XCTAssertFalse(kid.utf8, "a message field carries no utf8 flag")
        XCTAssertFalse(node.def.hasRequired, "proto3 message has no required field")
    }

    // ------------------------------------------------------------ fixtures

    private func varint(_ v: UInt64) -> [UInt8] {
        var x = v
        var out: [UInt8] = []
        while x > 0x7f {
            out.append(UInt8(truncatingIfNeeded: x) & 0x7f | 0x80)
            x >>= 7
        }
        out.append(UInt8(x))
        return out
    }

    private func ld(_ fnum: UInt32, _ payload: [UInt8]) -> [UInt8] {
        var out = varint(UInt64(fnum) * 8 + 2)
        out.append(contentsOf: varint(UInt64(payload.count)))
        out.append(contentsOf: payload)
        return out
    }

    private func vint(_ fnum: UInt32, _ v: UInt64) -> [UInt8] {
        var out = varint(UInt64(fnum) * 8)
        out.append(contentsOf: varint(v))
        return out
    }

    /// A hand-built `FileDescriptorSet` for a self-nesting proto3 message:
    /// `syntax = "proto3"; package deep; message Node { Node kid = 1; }`.
    private func deepType() throws -> MessageType {
        var fld = ld(1, Array("kid".utf8))
        fld.append(contentsOf: vint(3, 1))  // number
        fld.append(contentsOf: vint(4, 1))  // label optional
        fld.append(contentsOf: vint(5, 11))  // type message
        fld.append(contentsOf: ld(6, Array(".deep.Node".utf8)))
        var msg = ld(1, Array("Node".utf8))
        msg.append(contentsOf: ld(2, fld))
        var file = ld(1, Array("deep.proto".utf8))
        file.append(contentsOf: ld(2, Array("deep".utf8)))
        file.append(contentsOf: ld(4, msg))
        file.append(contentsOf: ld(12, Array("proto3".utf8)))
        let set = ld(1, file)
        let schema = try Schema(bytes: set, limits: Limits())
        return try schema.message("deep.Node")
    }
}

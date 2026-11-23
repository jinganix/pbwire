// Codec gate for the pure-Swift core: the pinned `matrix3` fixture bytes in
// both directions, unknown retention (in input order, wrong-wire-type
// included), groups, packed runs, oneofs, presence, the 64-bit edges and the
// limits. Every byte string here is either the C binding's pinned reference or
// a hand-written wire image with the shape the assertion is about.
import XCTest

@testable import PbwireCore

final class CodecTests: XCTestCase {
    private static let dataDir = URL(fileURLWithPath: #filePath)
        .deletingLastPathComponent()
        .deletingLastPathComponent()
        .deletingLastPathComponent()
        .appendingPathComponent("../c/tests/data")

    private func fixture(_ name: String) throws -> [UInt8] {
        let path = Self.dataDir.appendingPathComponent(name).path
        guard let data = FileManager.default.contents(atPath: path) else {
            throw CoreError("\(path): cannot read")
        }
        return [UInt8](data)
    }

    private func load(_ file: String) throws -> Schema {
        try Schema(contentsOfFile: Self.dataDir.appendingPathComponent(file).path)
    }

    private func type(_ file: String, _ name: String) throws -> MessageType {
        try load(file).message(name)
    }

    private func status(_ body: () throws -> Void) -> Int32 {
        do {
            try body()
            return 0
        } catch let e as CoreError {
            return e.status
        } catch {
            return -99
        }
    }

    // ------------------------------------------------------------ wire forms

    private func varint(_ v: UInt64) -> [UInt8] {
        var x = v
        var out: [UInt8] = []
        while x > 0x7f {
            out.append(UInt8(truncatingIfNeeded: x) & 0x7f | 0x80)
            x >>= 7
        }
        out.append(UInt8(truncatingIfNeeded: x))
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

    private func startGroup(_ fnum: UInt32) -> [UInt8] { varint(UInt64(fnum) * 8 + 3) }
    private func endGroup(_ fnum: UInt32) -> [UInt8] { varint(UInt64(fnum) * 8 + 4) }

    /// The `structtest.Scalars3` fixture, built through the public tree API.
    private func build(_ ty: MessageType) throws -> Message {
        let m = ty.newMessage()
        try m.setI64("i32", Int64(Int32.min))
        try m.setI64("i64", Int64.max)
        try m.setU64("u32", UInt64(UInt32.max))
        try m.setU64("u64", UInt64.max)
        try m.setI64("s32", Int64(Int32.min))
        try m.setI64("s64", Int64.min)
        try m.setU64("fx32", 0xdead_beef)
        try m.setU64("fx64", 72_623_859_790_382_856)
        try m.setI64("sfx32", Int64(Int32.min))
        try m.setI64("sfx64", Int64.min)
        try m.setF32("f32", .infinity)
        try m.setF64("f64", -123456.789)
        try m.setBool("flag", true)
        try m.setStr("str", "hello")
        try m.setI64("color", 2)
        try m.setI64("opt_i32", -7)
        try m.setStr("opt_str", "opt")
        do {
            let inner = try m.setMessage("inner")
            try inner.setStr("name", "kid")
            try inner.setI64("value", 42)
        }
        for n in [Int64(1), -5, 300] { try m.push("nums", .i64(n)) }
        for t in ["a", "bb"] { try m.push("tags", .bytes(Array(t.utf8))) }
        for (name, value) in [("x", Int64(1)), ("y", -2)] {
            let inner = try m.pushMessage("inners")
            try inner.setStr("name", name)
            try inner.setI64("value", value)
        }
        for v in [true, false, true] { try m.push("flags", .bool(v)) }
        return m
    }

    // ---------------------------------------------------------------- tests

    func testEncodeMatchesTheReference() throws {
        let ty = try type("matrix3.desc", "structtest.Scalars3")
        let reference = try fixture("matrix3.bin")
        let got = try ty.encode(try build(ty))
        XCTAssertEqual(got, reference, "core encode != protoc reference bytes")
        XCTAssertEqual(reference.count, 192)
    }

    func testDecodeReadsEveryFieldAndReEncodes() throws {
        let ty = try type("matrix3.desc", "structtest.Scalars3")
        let reference = try fixture("matrix3.bin")
        let m = try ty.decode(reference)
        XCTAssertEqual(try m.getI64("i32"), Int64(Int32.min))
        XCTAssertEqual(try m.getI64("i64"), Int64.max)
        XCTAssertEqual(try m.getU64("u32"), UInt64(UInt32.max))
        XCTAssertEqual(try m.getU64("u64"), UInt64.max)
        XCTAssertEqual(try m.getI64("s32"), Int64(Int32.min))
        XCTAssertEqual(try m.getI64("s64"), Int64.min)
        XCTAssertEqual(try m.getU64("fx32"), 0xdead_beef)
        XCTAssertEqual(try m.getU64("fx64"), 72_623_859_790_382_856)
        XCTAssertEqual(try m.getI64("sfx32"), Int64(Int32.min))
        XCTAssertEqual(try m.getI64("sfx64"), Int64.min)
        XCTAssertEqual(try m.getF32("f32"), Float.infinity)
        XCTAssertEqual(try m.getF64("f64"), -123456.789)
        XCTAssertEqual(try m.getBool("flag"), true)
        XCTAssertEqual(try m.getStr("str"), "hello")
        XCTAssertEqual(try m.getI64("color"), 2)
        XCTAssertEqual(try m.getI64("opt_i32"), -7)
        XCTAssertEqual(try m.getStr("opt_str"), "opt")
        let inner = try XCTUnwrap(try m.getMessage("inner"))
        XCTAssertEqual(try inner.getStr("name"), "kid")
        XCTAssertEqual(try inner.getI64("value"), 42)
        XCTAssertEqual(try m.count("nums"), 3)
        let nums = try m.items("nums")
        XCTAssertEqual(nums.map(\.asI64), [1, -5, 300])
        XCTAssertEqual(try m.count("tags"), 2)
        XCTAssertEqual(try m.items("tags")[1].asBytes, Array("bb".utf8))
        XCTAssertEqual(try m.count("inners"), 2)
        XCTAssertEqual(try m.count("flags"), 3)
        XCTAssertEqual(try m.items("flags")[1].asBool, false)
        XCTAssertEqual(try ty.encode(m), reference)
        XCTAssertEqual(try ty.encode(try ty.decode(reference)), reference)
    }

    func testU64AndI64Edges() throws {
        let ty = try type("matrix3.desc", "structtest.Scalars3")
        let m = ty.newMessage()
        try m.setU64("u64", UInt64.max)
        try m.setI64("i64", Int64.min)
        try m.setU64("fx64", UInt64.max)
        try m.setI64("sfx64", Int64.min)
        try m.setI64("s64", Int64.min)
        let wire = try ty.encode(m)
        let back = try ty.decode(wire)
        XCTAssertEqual(try back.getU64("u64"), UInt64.max)
        XCTAssertEqual(try back.getI64("i64"), Int64.min)
        XCTAssertEqual(try back.getU64("fx64"), UInt64.max)
        XCTAssertEqual(try back.getI64("sfx64"), Int64.min)
        XCTAssertEqual(try back.getI64("s64"), Int64.min)
        XCTAssertEqual(try ty.encode(back), wire)
        // sint64 zigzag: Int64.min is the widest encoding (10 bytes unsigned).
        let s64 = try ty.field("s64").number
        let w = vint(s64, UInt64.max)
        XCTAssertEqual(try ty.decode(w).getI64("s64"), Int64.min)
    }

    func testUnknownFieldsAreRetainedInInputOrder() throws {
        let ty = try type("matrix3.desc", "structtest.Scalars3")
        var wire = try fixture("matrix3.bin")
        // A known field, then two unknowns: varint and length-delimited.
        var tail = vint(99, 5)
        tail.append(contentsOf: ld(100, Array("opaque".utf8)))
        wire.append(contentsOf: tail)
        let m = try ty.decode(wire)
        XCTAssertEqual(m.unknown.count, 2, "two retained unknowns")
        XCTAssertEqual(m.unknown[0], vint(99, 5))
        XCTAssertEqual(m.unknown[1], ld(100, Array("opaque".utf8)))
        XCTAssertEqual(try ty.encode(m), wire, "unknowns re-encode verbatim and last")
        // An unknown group is validated to its end tag, then kept.
        var wire2 = try fixture("matrix3.bin")
        var group = startGroup(101)
        group.append(contentsOf: vint(1, 1))
        group.append(contentsOf: endGroup(101))
        wire2.append(contentsOf: group)
        let m2 = try ty.decode(wire2)
        XCTAssertEqual(m2.unknown, [group])
        XCTAssertEqual(try ty.encode(m2), wire2)
        // A mismatched end tag inside an unknown group fails the whole decode...
        var bad = startGroup(101)
        bad.append(contentsOf: endGroup(102))
        XCTAssertEqual(status { _ = try ty.decode(bad) }, -1, "mismatched end-group")
        // ...and an end tag with no open group is stray (status 1).
        XCTAssertEqual(status { _ = try ty.decode(endGroup(102)) }, 1, "stray end-group")
    }

    func testWrongWireTypeIsAnUnknown() throws {
        let ty = try type("matrix3.desc", "structtest.Scalars3")
        // Field 1 (`i32`) declared FIXED32: a wrong wire type, kept as unknown.
        let wire: [UInt8] = [0x0d, 1, 2, 3, 4]
        let m = try ty.decode(wire)
        XCTAssertNil(try m.get("i32"), "not read as i32")
        XCTAssertEqual(m.unknown, [wire])
        XCTAssertEqual(try ty.encode(m), wire)
    }

    func testGroupsRoundTrip() throws {
        let ty = try type("grouptest.desc", "structtest.GroupTest")
        let reference = try fixture("grouptest.bin")
        let m = try ty.decode(reference)
        XCTAssertEqual(try ty.encode(m), reference)
        let inner = try XCTUnwrap(try m.getMessage("inner"))
        XCTAssertNotNil(try inner.getStr("s"))
        // Two occurrences of a singular group merge into one, like the core.
        var a = startGroup(1)
        a.append(contentsOf: ld(3, Array("one".utf8)))
        a.append(contentsOf: endGroup(1))
        var b = startGroup(1)
        b.append(contentsOf: vint(2, 7))
        b.append(contentsOf: endGroup(1))
        var wire = a
        wire.append(contentsOf: b)
        let merged = try ty.decode(wire)
        let mergedInner = try XCTUnwrap(try merged.getMessage("inner"))
        XCTAssertEqual(try mergedInner.getStr("s"), "one")
        XCTAssertEqual(try mergedInner.getI64("x"), 7)
        // A group body cut off before its end tag fails.
        var cut = startGroup(1)
        cut.append(contentsOf: ld(3, Array("one".utf8)))
        XCTAssertEqual(status { _ = try ty.decode(cut) }, 1)
    }

    func testPackedAndUnpackedFormsAreBothAccepted() throws {
        let ty = try type("matrix3.desc", "structtest.Scalars3")
        // `nums` is packed in the schema; the unpacked element form still parses.
        let nums = try ty.field("nums").number
        var wire = vint(nums, 1)
        wire.append(contentsOf: vint(nums, 2))
        XCTAssertEqual(try ty.decode(wire).count("nums"), 2)
        // And the packed form under the same field number.
        var payload = varint(1)
        payload.append(contentsOf: varint(UInt64.max))  // -1 as int32 truncates
        let packed = ld(nums, payload)
        let m = try ty.decode(packed)
        XCTAssertEqual(try m.items("nums").map { Int32(truncatingIfNeeded: $0.asI64 ?? 0) }, [1, -1])
        XCTAssertEqual(try ty.encode(m), packed)
        // An empty packed run adds nothing.
        XCTAssertFalse(try ty.decode(ld(nums, [])).has("nums"))
        // A packed run that overruns its own length fails.
        var bad = varint(UInt64(nums) * 8 + 2)
        bad.append(contentsOf: varint(1))
        bad.append(0x80)  // one byte of a varint that never terminates
        XCTAssertEqual(status { _ = try ty.decode(bad) }, -1, "packed overrun")
        // A proto2 unpacked field accepts a packed run too.
        let ty2 = try type("matrix2.desc", "structtest.Scalars2")
        let p2nums = try ty2.field("nums")
        XCTAssertTrue(p2nums.repeated && !p2nums.packed)
        var payload2 = varint(4)
        var wire2 = vint(try ty2.field("id").number, 1)
        wire2.append(contentsOf: ld(p2nums.number, payload2))
        XCTAssertEqual(try ty2.decode(wire2).items("nums")[0].asI64, 4)
        payload2.removeAll()
    }

    func testOneofsCollapseToTheLastMember() throws {
        let ty = try type("oneoftest.desc", "structtest.OneOfTest")
        let reference = try fixture("oneoftest.bin")
        let m = try ty.decode(reference)
        XCTAssertEqual(try ty.encode(m), reference)
        XCTAssertNotEqual(try m.oneofCase("kind"), 0, "one member is set")
        // Setting two members leaves only the last one.
        let fresh = ty.newMessage()
        try fresh.setI64("num", 3)
        try fresh.setStr("text", "later")
        XCTAssertEqual(try fresh.oneofCase("kind"), try ty.field("text").number)
        XCTAssertFalse(try fresh.has("num"), "the earlier member is cleared")
        XCTAssertEqual(try ty.encode(fresh), ld(try ty.field("text").number, Array("later".utf8)))
        // A decode of both members keeps the last.
        var both = vint(try ty.field("num").number, 5)
        both.append(contentsOf: ld(try ty.field("text").number, Array("win".utf8)))
        let decoded = try ty.decode(both)
        XCTAssertEqual(try decoded.getStr("text"), "win")
        XCTAssertEqual(try decoded.oneofCase("kind"), try ty.field("text").number)
    }

    func testPresenceFollowsTheFieldRule() throws {
        let ty = try type("matrix3.desc", "structtest.Scalars3")
        // An implicit zero on the wire is dropped; an explicit one is kept.
        let m = try ty.decode(vint(1, 0))
        XCTAssertFalse(try m.has("i32"), "implicit zero stays absent")
        XCTAssertEqual(try ty.encode(m), [])
        let explicit = vint(try ty.field("opt_i32").number, 0)
        let m2 = try ty.decode(explicit)
        XCTAssertTrue(try m2.has("opt_i32"), "explicit zero is present")
        XCTAssertEqual(try ty.encode(m2), explicit)
        // An implicit empty string on the wire is dropped; an explicit one stays.
        let emptyImplicit = ld(try ty.field("str").number, [])
        XCTAssertFalse(try ty.decode(emptyImplicit).has("str"))
        let emptyExplicit = ld(try ty.field("opt_str").number, [])
        XCTAssertTrue(try ty.decode(emptyExplicit).has("opt_str"))
        XCTAssertEqual(try ty.encode(try ty.decode(emptyExplicit)), emptyExplicit)
        // A message field is present even when its body is empty.
        let emptyMessage = ld(try ty.field("inner").number, [])
        XCTAssertTrue(try ty.decode(emptyMessage).has("inner"))
        XCTAssertEqual(try ty.encode(try ty.decode(emptyMessage)), emptyMessage)
    }

    func testRequiredFieldsFailTheDecode() throws {
        let ty = try type("matrix2.desc", "structtest.Scalars2")
        let reference = try fixture("matrix2.bin")
        let m = try ty.decode(reference)
        XCTAssertEqual(try ty.encode(m), reference)
        // A minimal body without the required `id`.
        var wire: [UInt8] = []
        if try !m.isEmpty("nums") {
            wire.append(contentsOf: vint(try ty.field("nums").number, 1))
        }
        XCTAssertEqual(status { _ = try ty.decode(wire) }, 6, "missing required field")
        // Encoding the same tree fails the same way.
        let empty = ty.newMessage()
        try empty.setI64("nums", 1)
        XCTAssertEqual(status { _ = try ty.encode(empty) }, 6)
    }

    func testUtf8IsValidatedOnlyWhereTheSchemaSays() throws {
        let ty = try type("matrix3.desc", "structtest.Scalars3")
        let surrogate: [UInt8] = [0xed, 0xa0, 0x80]  // invalid UTF-8
        XCTAssertEqual(status { _ = try ty.decode(ld(14, surrogate)) }, 1,
                       "proto3 string verifies")
        // proto2 keeps the loose mapping: the bytes stay, a re-encode is exact.
        let ty2 = try type("matrix2.desc", "structtest.Scalars2")
        var wire = vint(try ty2.field("id").number, 1)
        wire.append(contentsOf: ld(try ty2.field("str").number, [0x80]))
        let m = try ty2.decode(wire)
        XCTAssertEqual(try m.getBytes("str"), [0x80])
        XCTAssertEqual(try ty2.encode(m), wire)
    }

    func testBadTagsAndTruncationFail() throws {
        let ty = try type("matrix3.desc", "structtest.Scalars3")
        XCTAssertEqual(status { _ = try ty.decode([0x00, 0x00]) }, 1, "field number 0")
        XCTAssertEqual(status { _ = try ty.decode([0x0e, 0x00]) }, 1, "wire type 6")
        XCTAssertEqual(status { _ = try ty.decode([0x0f, 0x00]) }, 1, "wire type 7")
        XCTAssertEqual(status { _ = try ty.decode([0x08]) }, -1, "truncated varint")
        XCTAssertEqual(status { _ = try ty.decode([0x0a, 0x05, 0x00]) }, -1,
                       "truncated length")
        XCTAssertEqual(status { _ = try ty.decode([0x09, 0x00, 0x00, 0x00]) }, -1,
                       "truncated fixed32")
        XCTAssertEqual(
            status { _ = try ty.decode([0x0a, 0x80, 0x80, 0x80, 0x80, 0x80, 0x01]) }, -1,
            "length past the buffer")
        // A 5-byte tag above UINT32_MAX truncates instead of failing: the
        // official suite expects the two BadTag cases to parse. The high bits
        // make tag 0x1000000008, which truncates to tag 8 (field 1, varint).
        var wire: [UInt8] = [0x88, 0x80, 0x80, 0x80, 0x10]
        wire.append(contentsOf: varint(5))
        XCTAssertEqual(try ty.decode(wire).getI64("i32"), 5)
    }

    func testLimitsBoundBytesAndDepth() throws {
        let ty = try type("matrix3.desc", "structtest.Scalars3")
        let reference = try fixture("matrix3.bin")
        XCTAssertEqual(status { _ = try ty.decode(reference, limits: Limits(maxBytes: 10)) }, 5)
        XCTAssertEqual(
            status { _ = try Schema(bytes: reference, limits: Limits(maxBytes: 10)) }, 5)
        // A self-nesting message deeper than the cap fails with status 4.
        let node = try deepNode()
        var wire: [UInt8] = []
        for _ in 0...Int(maxDepth) { wire = ld(1, wire) }
        XCTAssertEqual(status { _ = try node.decode(wire) }, 4, "nesting exceeds the cap")
        let shallow = Limits(maxDepth: 4)
        let nested5 = ld(1, ld(1, ld(1, ld(1, ld(1, [])))))
        XCTAssertEqual(status { _ = try node.decode(nested5, limits: shallow) }, 4)
        // A depth below the cap still decodes.
        XCTAssertNoThrow(try node.decode(ld(1, []), limits: shallow))
    }

    func testEveryKindHasAStorageForm() throws {
        let ty = try type("matrix3.desc", "structtest.Scalars3")
        let expected: [(String, Kind)] = [
            ("i32", .int32), ("i64", .int64), ("u32", .uint32), ("u64", .uint64),
            ("s32", .sint32), ("s64", .sint64), ("fx32", .fixed32), ("fx64", .fixed64),
            ("sfx32", .sfixed32), ("sfx64", .sfixed64), ("f32", .float), ("f64", .double),
            ("flag", .bool), ("str", .string), ("color", .enumeration), ("inner", .message),
        ]
        for (name, kind) in expected {
            XCTAssertEqual(try ty.field(name).kind, kind, name)
        }
        // A float NaN payload survives a round trip (bits, not a converted value).
        let m = ty.newMessage()
        try m.setF32("f32", Float(bitPattern: 0x7fc0_1234))
        let back = try ty.decode(try ty.encode(m))
        guard case .f32(let bits)? = try back.get("f32") else {
            return XCTFail("f32 slot is not a float")
        }
        XCTAssertEqual(bits, 0x7fc0_1234)
    }

    /// `syntax = "proto3"; package deep; message Node { Node kid = 1; }`
    private func deepNode() throws -> MessageType {
        var fld = ld(1, Array("kid".utf8))
        fld.append(contentsOf: vint(3, 1))
        fld.append(contentsOf: vint(4, 1))
        fld.append(contentsOf: vint(5, 11))
        fld.append(contentsOf: ld(6, Array(".deep.Node".utf8)))
        var msg = ld(1, Array("Node".utf8))
        msg.append(contentsOf: ld(2, fld))
        var file = ld(1, Array("deep.proto".utf8))
        file.append(contentsOf: ld(2, Array("deep".utf8)))
        file.append(contentsOf: ld(4, msg))
        file.append(contentsOf: ld(12, Array("proto3".utf8)))
        return try Schema(bytes: ld(1, file)).message("deep.Node")
    }
}

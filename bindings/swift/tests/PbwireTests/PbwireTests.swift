// Assert-based self-check for the Swift binding: no dependencies beyond
// XCTest and the C core. Run it with `swift test` (or `./build.sh`) from
// bindings/swift.
//
// Covers: descriptor set load (file + bytes), unknown names, a full scalar
// and nested round trip, byte-exact re-encode, presence of absent/zero
// fields, repeated add/count/at, schema/API errors, decode rejecting
// garbage, and message lifetime.
import Foundation
import Pbwire
import XCTest

final class PbwireTests: XCTestCase {
    // #filePath is the compiled source path, so the fixture resolves without
    // depending on the working directory.
    static var fixture: String {
        URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
            .appendingPathComponent("tests/data/probe.desc").path
    }

    private func loadSchema() throws -> Schema {
        try Schema(contentsOfFile: Self.fixture)
    }

    func testSchemaLoad() throws {
        let schema = try loadSchema()
        XCTAssertNotNil(schema.message("pbwswift.Probe"))
        XCTAssertNotNil(schema.message("pbwswift.Inner"))
        // Unknown names and enums are nil, not errors.
        XCTAssertNil(schema.message("pbwswift.Nope"))
        XCTAssertNil(schema.message("pbwswift.Color"))
        XCTAssertNil(schema.message(""))

        // Same schema from raw bytes.
        let bytes = try Data(contentsOf: URL(fileURLWithPath: Self.fixture))
        let fromBytes = try Schema(bytes: bytes)
        XCTAssertNotNil(fromBytes.message("pbwswift.Probe"))

        // A missing file and garbage bytes both throw.
        XCTAssertThrowsError(try Schema(contentsOfFile: "/nonexistent/probe.desc"))
        XCTAssertThrowsError(try Schema(bytes: Data([0xFF, 0x00, 0x01])))
    }

    func testScalarRoundTrip() throws {
        let schema = try loadSchema()
        let message = schema.message("pbwswift.Probe")!

        try message.set(.int(-12), "i32")
        try message.set(.int(-34), "s32")
        try message.set(.int(Int64.min + 7), "i64")
        try message.set(.uint(300), "u32")
        try message.set(.uint(UInt64.max - 8), "u64")
        try message.set(.bool(true), "flag")
        try message.set(.float(1.5), "f32")
        try message.set(.double(-123456.789), "f64")
        try message.set(.string("alice"), "text")
        try message.set(.bytes(Data([0x00, 0xFF, 0x10])), "blob")
        try message.set(.int(2), "color") // COLOR_BLUE
        try message.makeNested("inner").set(.string("child"), "label")
        try message.nested("inner")!.set(.int(42), "n")

        let wire = try message.encode()
        let decoded = try message.decode(wire)

        XCTAssertEqual(decoded.get("i32"), .int(-12))
        XCTAssertEqual(decoded.get("s32"), .int(-34))
        XCTAssertEqual(decoded.get("i64"), .int(Int64.min + 7))
        XCTAssertEqual(decoded.get("u32"), .uint(300))
        XCTAssertEqual(decoded.get("u64"), .uint(UInt64.max - 8))
        XCTAssertEqual(decoded.get("flag"), .bool(true))
        XCTAssertEqual(decoded.get("f32"), .float(1.5))
        XCTAssertEqual(decoded.get("f64"), .double(-123456.789))
        XCTAssertEqual(decoded.get("text"), .string("alice"))
        XCTAssertEqual(decoded.get("blob"), .bytes(Data([0x00, 0xFF, 0x10])))
        XCTAssertEqual(decoded.get("color"), .int(2))
        XCTAssertEqual(decoded.nested("inner")?.get("label"), .string("child"))
        XCTAssertEqual(decoded.nested("inner")?.get("n"), .int(42))

        // Byte-exact re-encode: unknown fields and ordering survive.
        XCTAssertEqual(try decoded.encode(), wire)
    }

    func testPinnedBytes() throws {
        // One pinned encoding, so wire drift fails here instead of in a
        // benchmark: i32 = 150 packs as 08 96 01, text = "hi" as 4A 02 68 69.
        let schema = try loadSchema()
        let message = schema.message("pbwswift.Probe")!
        try message.set(.int(150), "i32")
        try message.set(.string("hi"), "text")
        XCTAssertEqual(
            try message.encode(),
            Data([0x08, 0x96, 0x01, 0x4A, 0x02, 0x68, 0x69]))
        // An empty message encodes to zero bytes, and empty bytes payloads
        // survive the round trip.
        let empty = schema.message("pbwswift.Probe")!
        XCTAssertEqual(try empty.encode(), Data())
        try empty.set(.bytes(Data()), "blob")
        XCTAssertEqual(try empty.decode(try empty.encode()).get("blob"), .bytes(Data()))
    }

    func testPresence() throws {
        let schema = try loadSchema()
        let message = schema.message("pbwswift.Probe")!

        // Absent fields read nil and report no presence.
        XCTAssertFalse(message.has("i32"))
        XCTAssertNil(message.get("i32"))
        XCTAssertNil(message.get("nope"))
        XCTAssertNil(message.nested("inner"))

        // An explicit zero is present; clearing removes it again.
        try message.set(.int(0), "i32")
        XCTAssertTrue(message.has("i32"))
        XCTAssertEqual(message.get("i32"), .int(0))
        try message.set(nil, "i32")
        XCTAssertFalse(message.has("i32"))
        XCTAssertNil(message.get("i32"))

        // Presence survives the wire.
        try message.set(.string(""), "text")
        let decoded = try message.decode(try message.encode())
        XCTAssertTrue(decoded.has("text"))
        XCTAssertEqual(decoded.get("text"), .string(""))
        XCTAssertFalse(decoded.has("flag"))
    }

    func testRepeated() throws {
        // probe.proto has no repeated field, so matrix3 carries this row:
        // tags is repeated string, nums is repeated (packed) int32.
        let matrix = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
            .appendingPathComponent("tests/data/matrix3.desc").path
        let schema = try Schema(contentsOfFile: matrix)
        let message = schema.message("structtest.Scalars3")!

        XCTAssertEqual(message.count("tags"), 0)
        XCTAssertNil(message.at("tags", 0))
        // Singular fields are not lists.
        XCTAssertEqual(message.count("str"), 0)
        XCTAssertNil(message.at("str", 0))

        try message.add(.string("a"), "tags")
        try message.add(.string("bb"), "tags")
        try message.add(.int(1), "nums")
        try message.add(.int(-5), "nums")
        XCTAssertEqual(message.count("tags"), 2)
        XCTAssertEqual(message.at("tags", 1), .string("bb"))
        XCTAssertEqual(message.at("nums", 1), .int(-5))
        XCTAssertNil(message.at("tags", 2))

        let decoded = try message.decode(try message.encode())
        XCTAssertEqual(decoded.count("tags"), 2)
        XCTAssertEqual(decoded.at("tags", 0), .string("a"))
        XCTAssertEqual(try decoded.encode(), try message.encode())
    }

    func testNestedLifetime() throws {
        let schema = try loadSchema()
        let message = schema.message("pbwswift.Probe")!

        // A view edits the parent's tree in place and retains the parent,
        // so it cannot outlive the storage it points into.
        let view = try message.makeNested("inner")
        try view.set(.string("kept"), "label")
        XCTAssertEqual(message.nested("inner")?.get("label"), .string("kept"))
        // makeNested on the same singular field returns the same storage.
        try message.makeNested("inner").set(.int(7), "n")
        XCTAssertEqual(view.get("n"), .int(7))

        // Editing through a decoded tree works the same way.
        let decoded = try message.decode(try message.encode())
        try decoded.makeNested("inner").set(.int(9), "n")
        XCTAssertEqual(decoded.nested("inner")?.get("label"), .string("kept"))
        XCTAssertEqual(decoded.nested("inner")?.get("n"), .int(9))
    }

    func testErrors() throws {
        let schema = try loadSchema()
        let message = schema.message("pbwswift.Probe")!

        // Unknown field.
        XCTAssertThrowsError(try message.set(.int(1), "nope")) { error in
            XCTAssertEqual((error as? PbwireError)?.status, 3) // PBW_ERR_NOT_FOUND
        }
        // Wrong value class.
        XCTAssertThrowsError(try message.set(.string("x"), "i32"))
        XCTAssertThrowsError(try message.set(.bytes(Data([1])), "text"))
        XCTAssertThrowsError(try message.set(.bool(true), "text"))
        // Out-of-range 32-bit values fail instead of truncating.
        XCTAssertThrowsError(try message.set(.int(Int64(Int32.max) + 1), "i32"))
        XCTAssertThrowsError(try message.set(.uint(UInt64(UInt32.max) + 1), "u32"))
        XCTAssertThrowsError(try message.set(.int(-1), "u32"))
        // Singular set on a message field: use makeNested.
        XCTAssertThrowsError(try message.set(.int(1), "inner"))
        // Garbage bytes do not decode (empty bytes decode to an empty
        // message, which is valid protobuf).
        XCTAssertThrowsError(try message.decode(Data([0xFF, 0xFF, 0xFF, 0xFF, 0xFF])))
        XCTAssertNoThrow(try message.decode(Data()))
    }

    func testLifetimeLoop() throws {
        // 200 encode/decode cycles: no leak is visible to the test, but a
        // double-free or use-after-free crashes it.
        let schema = try loadSchema()
        for i in 0..<200 {
            let message = schema.message("pbwswift.Probe")!
            try message.set(.int(Int64(i)), "i32")
            try message.makeNested("inner").set(.int(Int64(i)), "n")
            let decoded = try message.decode(try message.encode())
            XCTAssertEqual(decoded.get("i32"), .int(Int64(i)))
        }
    }
}

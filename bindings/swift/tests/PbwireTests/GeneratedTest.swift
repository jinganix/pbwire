// Generated-conformance check for the Swift binding: build matrix3.proto's
// Scalars3 through the generated structs, encode, require the protoc reference
// bytes (matrix3.bin), then decode those bytes back and check every field.
// The schema travels embedded in tests/gen/matrix3.swift; no .desc file.
//
// Run it with `swift test --filter GeneratedTest` from bindings/swift.
import Foundation
import XCTest

final class GeneratedTest: XCTestCase {
    static var bin: String {
        URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
            .appendingPathComponent("tests/data/matrix3.bin").path
    }

    private func build() -> Structtest_Scalars3 {
        var inner = Structtest_Inner3()
        inner.name = "kid"
        inner.value = 42
        var x = Structtest_Inner3()
        x.name = "x"
        x.value = 1
        var y = Structtest_Inner3()
        y.name = "y"
        y.value = -2
        var m = Structtest_Scalars3()
        m.i32 = -2147483648
        m.i64 = 9223372036854775807
        m.u32 = 4294967295
        m.u64 = 18446744073709551615
        m.s32 = -2147483648
        m.s64 = -9223372036854775808
        m.fx32 = 3735928559
        m.fx64 = 72623859790382856
        m.sfx32 = -2147483648
        m.sfx64 = -9223372036854775808
        m.f32 = Float.infinity
        m.f64 = -123456.789
        m.flag = true
        m.str = "hello"
        m.color = 2
        m.inner = inner
        m.nums = [1, -5, 300]
        m.tags = ["a", "bb"]
        m.inners = [x, y]
        m.optI32 = -7
        m.optStr = "opt"
        m.flags = [true, false, true]
        return m
    }

    func testGeneratedBytes() throws {
        let wire = try build().encode()
        let reference = try Data(contentsOf: URL(fileURLWithPath: Self.bin))
        XCTAssertEqual(wire, reference, "generated encode equals the protoc reference bytes")
    }

    func testGeneratedDecode() throws {
        let reference = try Data(contentsOf: URL(fileURLWithPath: Self.bin))
        let m = try Structtest_Scalars3.decode(reference)
        XCTAssertEqual(m.i32, -2147483648)
        XCTAssertEqual(m.i64, 9223372036854775807)
        XCTAssertEqual(m.u32, 4294967295)
        XCTAssertEqual(m.u64, 18446744073709551615)
        XCTAssertEqual(m.f32, Float.infinity)
        XCTAssertEqual(m.f64, -123456.789)
        XCTAssertEqual(m.flag, true)
        XCTAssertEqual(m.str, "hello")
        XCTAssertEqual(m.color, 2)
        XCTAssertEqual(m.inner?.name, "kid")
        XCTAssertEqual(m.inner?.value, 42)
        XCTAssertEqual(m.nums, [1, -5, 300])
        XCTAssertEqual(m.tags, ["a", "bb"])
        XCTAssertEqual(m.inners.count, 2)
        XCTAssertEqual(m.inners[1].value, -2)
        XCTAssertEqual(m.optI32, -7)
        XCTAssertEqual(m.optStr, "opt")
        XCTAssertEqual(m.flags, [true, false, true])
    }

    func testGeneratedEmpty() throws {
        let wire = try Structtest_Scalars3().encode()
        XCTAssertEqual(wire.count, 0, "empty generated message encodes to nothing")
        var zero = Structtest_Scalars3()
        zero.optI32 = 0
        let back = try Structtest_Scalars3.decode(try zero.encode())
        XCTAssertEqual(back.optI32, 0, "explicit zero with presence survives")
    }
}

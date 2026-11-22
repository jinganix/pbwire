// pbench gate for the Swift `shim` arm (Stage 3): the generated `@frozen`
// mirror must produce the reference bytes, decode them back into an object
// that exposes every payload value, and re-encode to the same bytes. The
// reference is the pinned protoc output (`tests/data/matrix3.bin`), which is
// also what apple/swift-protobuf's generated code writes for the same values
// (benchmarks/baseline/BenchBoth asserts that equality for the `tree` row).
//
// Run it with `swift test --filter InPlaceTest`, or `make swift-test`.
import Foundation
import Pbwire
import XCTest

final class InPlaceTest: XCTestCase {
    static var bin: String {
        URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent().deletingLastPathComponent()
            .deletingLastPathComponent()
            .appendingPathComponent("tests/data/matrix3.bin").path
    }

    /// The payload the C matrix3 fixture uses, as a mirror. Every buffer the
    /// mirror borrows is owned by `storage`, which the caller keeps alive.
    private func mirror(_ storage: ReprStorage) -> Structtest_Scalars3Repr {
        var repr = Structtest_Scalars3Repr()
        repr.i32 = -2147483648
        repr.i64 = 9223372036854775807
        repr.u32 = 4294967295
        repr.u64 = 18446744073709551615
        repr.s32 = -2147483648
        repr.s64 = -9223372036854775808
        repr.fx32 = 3735928559
        repr.fx64 = 72623859790382856
        repr.sfx32 = -2147483648
        repr.sfx64 = -9223372036854775808
        repr.f32 = Float.infinity
        repr.f64 = -123456.789
        repr.flag = true
        repr.str = storage.string("hello")
        repr.color = 2
        let inner = storage.mirror(of: Structtest_Inner3Repr.self)
        inner.pointee.name = storage.string("kid")
        inner.pointee.value = 42
        repr.inner = inner
        repr.nums = storage.array([1, -5, 300].map { Int32($0) })
        repr.countNums = 3
        repr.tags = storage.array(["a", "bb"].map { storage.string($0) })
        repr.countTags = 2
        let inners = storage.array(
            [(value: 1, name: "x"), (value: -2, name: "y")].map { item in
                var child = Structtest_Inner3Repr()
                child.name = storage.string(item.name)
                child.value = Int32(item.value)
                return child
            })
        repr.inners = inners
        repr.countInners = 2
        repr.optI32 = -7
        repr.hasOptI32 = 1
        repr.optStr = storage.string("opt")
        repr.flags = storage.array([true, false, true])
        repr.countFlags = 3
        return repr
    }

    /// The layout rows must describe one slot per field, with every offset
    /// inside the mirror and no two rows sharing one (a wrong keypath would
    /// silently read a neighbour).
    func testLayoutRows() {
        let plans = Structtest_Scalars3Repr.fields
        XCTAssertEqual(plans.count, 22, "one row per field")
        XCTAssertEqual(plans.map { Int($0.number) }, Array(1...22), "descriptor order")
        let size = MemoryLayout<Structtest_Scalars3Repr>.size
        var offsets: [Int] = []
        for plan in plans {
            XCTAssertGreaterThanOrEqual(plan.offset, 0)
            XCTAssertLessThan(plan.offset, size, "field \(plan.number) inside the mirror")
            offsets.append(plan.offset)
            for extra in [plan.has, plan.count, plan.cs] where extra != 0 {
                XCTAssertLessThan(extra, size, "aux slot of field \(plan.number)")
                offsets.append(extra)
            }
        }
        XCTAssertEqual(Set(offsets).count, offsets.count, "no two slots share an offset")
        // A has flag at offset 0 would read as "no has flag" in the codec.
        let optI32 = plans.first { $0.number == 20 }
        XCTAssertNotNil(optI32)
        XCTAssertGreaterThan(optI32?.has ?? 0, 0, "opt_i32 carries a has flag")
        XCTAssertEqual(Structtest_Inner3Repr.fields.count, 2)
    }

    func testMirrorEncodesReferenceBytes() throws {
        let storage = ReprStorage()
        let repr = mirror(storage)
        let reference = try Data(contentsOf: URL(fileURLWithPath: Self.bin))
        let schema = try Schema(contentsOfFile: Self.schemaPath)
        let wire = try schema.encodeInPlace(repr)
        XCTAssertEqual(wire, reference, "mirror encode equals the protoc reference bytes")
        // The mirror and the generated idiomatic struct come from the same
        // emitter and the same payload; GeneratedTest pins the handle path
        // against this same reference, so both arms answer to one oracle.
    }

    func testMirrorDecodesReferenceBytes() throws {
        let reference = try Data(contentsOf: URL(fileURLWithPath: Self.bin))
        let schema = try Schema(contentsOfFile: Self.schemaPath)
        let inPlace = try schema.inPlace(Structtest_Scalars3Repr.self)
        var decoded = Structtest_Scalars3Repr()
        try inPlace.decodeInPlace(reference, into: &decoded)

        XCTAssertEqual(decoded.i32, -2147483648)
        XCTAssertEqual(decoded.i64, 9223372036854775807)
        XCTAssertEqual(decoded.u32, 4294967295)
        XCTAssertEqual(decoded.u64, 18446744073709551615)
        XCTAssertEqual(decoded.s32, -2147483648)
        XCTAssertEqual(decoded.s64, -9223372036854775808)
        XCTAssertEqual(decoded.fx32, 3735928559)
        XCTAssertEqual(decoded.fx64, 72623859790382856)
        XCTAssertEqual(decoded.sfx32, -2147483648)
        XCTAssertEqual(decoded.sfx64, -9223372036854775808)
        XCTAssertEqual(decoded.f32, Float.infinity)
        XCTAssertEqual(decoded.f64, -123456.789)
        XCTAssertTrue(decoded.flag)
        XCTAssertEqual(decoded.str.map { String(cString: $0) } ?? "", "hello")
        XCTAssertEqual(decoded.color, 2)
        XCTAssertEqual(decoded.inner?.pointee.value, 42)
        XCTAssertEqual(
            decoded.inner.flatMap { $0.pointee.name.map { String(cString: $0) } }, "kid")
        XCTAssertEqual(nums(decoded), [1, -5, 300])
        XCTAssertEqual(tags(decoded), ["a", "bb"])
        XCTAssertEqual(inners(decoded).map { $0.value }, [1, -2])
        XCTAssertEqual(
            inners(decoded).map { $0.name.map { String(cString: $0) } ?? "" }, ["x", "y"])
        XCTAssertEqual(decoded.optI32, -7)
        XCTAssertEqual(decoded.hasOptI32, 1, "proto3 optional keeps its presence flag")
        XCTAssertEqual(decoded.optStr.map { String(cString: $0) }, "opt")
        XCTAssertEqual(flags(decoded), [true, false, true])

        // Re-encode the decoded image in place: same bytes back.
        let out = try schema.encodeInPlace(decoded)
        XCTAssertEqual(out, reference, "decoded mirror re-encodes to the reference bytes")
    }

    /// One mirror type per (schema, message): the second `inPlace` call
    /// returns the registered codec instead of compiling a second one, which
    /// is what makes the steady-state loop allocate nothing.
    func testTypeIsCached() throws {
        let schema = try Schema(contentsOfFile: Self.schemaPath)
        let first = try schema.inPlace(Structtest_Scalars3Repr.self)
        let second = try schema.inPlace(Structtest_Scalars3Repr.self)
        XCTAssertTrue(first === second)
    }

    /// A message with no bytes decodes into an absent mirror, not a crash.
    /// The C layout codec writes every scalar whose row has no presence flag
    /// (the shared proto3-implicit rule of the Rust/Julia/Dart mirrors), so an
    /// empty message re-encodes to explicit zero fields: valid, canonical for
    /// this mechanism, and decoding it again yields the same mirror. The
    /// published arm bytes come from the payload in `mirror(_:)`, which sets
    /// every scalar and therefore matches the reference exactly.
    func testEmptyWire() throws {
        let schema = try Schema(contentsOfFile: Self.schemaPath)
        var decoded = Structtest_Scalars3Repr()
        decoded.i32 = 7
        try schema.decodeInPlace(Data(), into: &decoded)
        XCTAssertEqual(decoded.i32, 0)
        XCTAssertNil(decoded.str)
        XCTAssertNil(decoded.optStr)

        let out = try schema.encodeInPlace(decoded)
        var again = Structtest_Scalars3Repr()
        try schema.decodeInPlace(out, into: &again)
        XCTAssertEqual(again.i32, 0)
        XCTAssertEqual(again.u64, 0)
        XCTAssertNil(again.str)
        XCTAssertEqual(try schema.encodeInPlace(again), out, "the zero mirror round-trips")
    }

    // Reading a decoded mirror: the slots borrow the type's arena, so these
    // view that memory instead of copying a tree (the pbench arms wrap the
    // same reads for the payload check).
    private func nums(_ repr: Structtest_Scalars3Repr) -> [Int32] {
        guard let nums = repr.nums else { return [] }
        return Array(UnsafeBufferPointer(start: nums, count: repr.countNums))
    }

    private func tags(_ repr: Structtest_Scalars3Repr) -> [String] {
        guard let tags = repr.tags else { return [] }
        return (0..<repr.countTags).map { String(cString: tags[$0]) }
    }

    private func inners(_ repr: Structtest_Scalars3Repr) -> [Structtest_Inner3Repr] {
        guard let inners = repr.inners else { return [] }
        return Array(UnsafeBufferPointer(start: inners, count: repr.countInners))
    }

    private func flags(_ repr: Structtest_Scalars3Repr) -> [Bool] {
        guard let flags = repr.flags else { return [] }
        return Array(UnsafeBufferPointer(start: flags, count: repr.countFlags))
    }

    static let schemaPath =
        URL(fileURLWithPath: #filePath)
        .deletingLastPathComponent().deletingLastPathComponent()
        .deletingLastPathComponent()
        .appendingPathComponent("tests/data/matrix3.desc").path
}

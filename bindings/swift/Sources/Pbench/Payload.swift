// The pbench payload: one native object per arm, the same matrix3 values as
// the C/Dart/Julia/Rust rows, on the four standard tiers from BENCHMARKS.md.
// Every arm does the same user-visible task (see bindings/README.md):
// a native object in and wire bytes out; wire bytes in and a fully readable
// native object out.
//
// Two object shapes live here:
//  - the shipped `Message` handle (`tree`), built with one `set` per field;
//  - the generated `@frozen Structtest_Scalars3Repr` (`shim`), a flat mirror
//    whose pointers borrow a `ReprStorage`, so the mirror itself is a value.
import Foundation
import Pbwire

/// One standard tier: the repeat count, the wire size it must produce and the
/// per-pass floor the harness calibrates to.
public struct Tier: Sendable {
    public let name: String
    public let repeats: Int
    public let bytes: Int
    public let floor: Int

    public init(name: String, repeats: Int, bytes: Int, floor: Int) {
        self.name = name
        self.repeats = repeats
        self.bytes = bytes
        self.floor = floor
    }
}

public let pbenchTiers: [Tier] = [
    Tier(name: "small", repeats: 0, bytes: 136, floor: 50_000),
    Tier(name: "medium", repeats: 1, bytes: 174, floor: 50_000),
    Tier(name: "large", repeats: 100, bytes: 2469, floor: 5_000),
    Tier(name: "xlarge", repeats: 3000, bytes: 74_913, floor: 500),
]

/// The scalar payload every tier shares (`fill` in the C bench and every
/// binding's bench agree on these values, so the wire sizes match).
public func fillTree(_ message: Message, repeats n: Int) throws {
    try message.set(.int(Int64(Int32.min)), "i32")
    try message.set(.int(Int64.max), "i64")
    try message.set(.uint(UInt64(UInt32.max)), "u32")
    try message.set(.uint(UInt64.max), "u64")
    try message.set(.int(Int64(Int32.min)), "s32")
    try message.set(.int(Int64.min), "s64")
    try message.set(.uint(0xDEAD_BEEF), "fx32")
    try message.set(.uint(72_623_859_790_382_856), "fx64")
    try message.set(.int(Int64(Int32.min)), "sfx32")
    try message.set(.int(Int64.min), "sfx64")
    try message.set(.float(.infinity), "f32")
    try message.set(.double(-123456.789), "f64")
    try message.set(.bool(true), "flag")
    try message.set(.string("hello"), "str")
    try message.set(.int(2), "color") // COLOR3_GREEN
    try message.makeNested("inner").set(.string("kid"), "name")
    try message.nested("inner")!.set(.int(42), "value")
    try message.set(.int(-7), "opt_i32")
    try message.set(.string("opt"), "opt_str")
    for i in 0..<n {
        try message.add(.int(Int64(i - 3)), "nums")
        try message.add(.string("tag"), "tags")
        let grandkid = try message.makeNested("inners")
        try grandkid.set(.string("grandkid"), "name")
        try grandkid.set(.int(Int64(i + 1)), "value")
        try message.add(.bool(i % 2 == 0), "flags")
    }
}

/// Fills the flat mirror with the same values `fillTree` sets, borrowing
/// `storage` (`Pbwire.ReprStorage`). Every non-blittable value goes through
/// the storage, so the mirror holds no Swift references (no ARC in the walk).
public func fillMirror(
    _ repr: inout Structtest_Scalars3Repr, repeats n: Int, storage: ReprStorage
) {
    repr.i32 = Int32.min
    repr.i64 = Int64.max
    repr.u32 = UInt32.max
    repr.u64 = UInt64.max
    repr.s32 = Int32.min
    repr.s64 = Int64.min
    repr.fx32 = 0xDEAD_BEEF
    repr.fx64 = 72_623_859_790_382_856
    repr.sfx32 = Int32.min
    repr.sfx64 = Int64.min
    repr.f32 = .infinity
    repr.f64 = -123456.789
    repr.flag = true
    repr.str = storage.string("hello")
    repr.color = 2
    let inner = storage.mirror(of: Structtest_Inner3Repr.self)
    inner.pointee.name = storage.string("kid")
    inner.pointee.value = 42
    repr.inner = inner
    repr.nums = storage.array((0..<n).map { Int32($0 - 3) })
    repr.countNums = n
    let tags = (0..<n).map { _ in storage.string("tag") }
    repr.tags = storage.array(tags)
    repr.countTags = n
    let inners = storage.array(
        (0..<n).map { i -> Structtest_Inner3Repr in
            var child = Structtest_Inner3Repr()
            child.name = storage.string("grandkid")
            child.value = Int32(i + 1)
            return child
        })
    repr.inners = inners
    repr.countInners = n
    repr.optI32 = -7
    repr.hasOptI32 = 1
    repr.optStr = storage.string("opt")
    repr.flags = storage.array((0..<n).map { $0 % 2 == 0 })
    repr.countFlags = n
}

// Reading a decoded mirror: the slots are pointers into the arena the decode
// borrowed from, so these helpers view that memory instead of copying a tree.

extension Structtest_Scalars3Repr {
    public var text: String { str.map { String(cString: $0) } ?? "" }
    public var optText: String? { optStr.map { String(cString: $0) } }
    public var numsValues: [Int32] {
        nums.map { Array(UnsafeBufferPointer(start: $0, count: countNums)) } ?? []
    }
    public var tagsValues: [String] {
        guard let tags else { return [] }
        return (0..<countTags).map { String(cString: tags[$0]) }
    }
    public var innersValues: [Structtest_Inner3Repr] {
        inners.map { Array(UnsafeBufferPointer(start: $0, count: countInners)) } ?? []
    }
    public var flagsValues: [Bool] {
        flags.map { Array(UnsafeBufferPointer(start: $0, count: countFlags)) } ?? []
    }
    public var innerValue: Structtest_Inner3Repr? { inner?.pointee }
}

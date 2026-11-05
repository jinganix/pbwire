// Side-by-side benchmark: pbwire-swift vs apple/swift-protobuf generated
// code on the same matrix3.proto and the four standard tiers from
// BENCHMARKS.md. Both sides must produce identical bytes before anything is
// timed, and both allocate per call, so the comparison is like for like.
//
//   swift run -c release BenchBoth   (needs network once for swift-protobuf)
import Foundation
import Model
import Pbwire
import SwiftProtobuf

private let cases = ["small", "medium", "large", "xlarge"]
private let repeats = [0, 1, 100, 3000]
private let want = [136, 174, 2469, 74_913]
private let floors = [50_000, 50_000, 5_000, 500]
private let minPassSeconds = 0.25
private let passes = 3

private var failures = 0

private func fillPbwire(_ message: Pbwire.Message, n: Int) throws {
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
    try message.set(.int(2), "color")
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

private func fillOfficial(_ n: Int) -> Structtest_Scalars3 {
    var m = Structtest_Scalars3()
    m.i32 = -2_147_483_648
    m.i64 = 9_223_372_036_854_775_807
    m.u32 = 4_294_967_295
    m.u64 = 18_446_744_073_709_551_615
    m.s32 = -2_147_483_648
    m.s64 = -9_223_372_036_854_775_808
    m.fx32 = 0xDEAD_BEEF
    m.fx64 = 72_623_859_790_382_856
    m.sfx32 = -2_147_483_648
    m.sfx64 = -9_223_372_036_854_775_808
    m.f32 = .infinity
    m.f64 = -123456.789
    m.flag = true
    m.str = "hello"
    m.color = .green
    m.inner = Structtest_Inner3.with { $0.name = "kid"; $0.value = 42 }
    m.optI32 = -7
    m.optStr = "opt"
    m.nums = (0..<n).map { Int32($0 - 3) }
    m.tags = (0..<n).map { _ in "tag" }
    m.inners = (0..<n).map { i in Structtest_Inner3.with { $0.name = "grandkid"; $0.value = Int32(i + 1) } }
    m.flags = (0..<n).map { $0 % 2 == 0 }
    return m
}

private func now() -> UInt64 { DispatchTime.now().uptimeNanoseconds }

private func measure(_ count: Int, _ op: () throws -> Void) rethrows -> Double {
    var best = Double.greatestFiniteMagnitude
    for _ in 0..<passes {
        let start = now()
        for _ in 0..<count { try op() }
        best = min(best, Double(now() - start) / Double(count))
    }
    return best
}

private func calibrate(_ count: Int, _ op: () throws -> Void) rethrows -> Int {
    var count = count
    while true {
        let start = now()
        for _ in 0..<count { try op() }
        if Double(now() - start) / 1e9 >= minPassSeconds { return count }
        count *= 2
    }
}

private func row(_ impl: String, _ op: String, _ bytes: Int, _ ns: Double) {
    let mbs = Double(bytes) / ns * 1000
    print(
        "  \(impl.padding(toLength: 12, withPad: " ", startingAt: 0)) "
            + "\(op.padding(toLength: 7, withPad: " ", startingAt: 0)) "
            + "\(String(format: "%12.1f", ns)) \(String(format: "%10.1f", mbs))")
}

// #filePath is the compiled source path: file, BenchBoth, Sources,
// baseline, benchmarks, then the package root holding tests/data.
let descPath = URL(fileURLWithPath: #filePath)
    .deletingLastPathComponent().deletingLastPathComponent()
    .deletingLastPathComponent().deletingLastPathComponent()
    .deletingLastPathComponent()
    .appendingPathComponent("tests/data/matrix3.desc").path

let schema = try Schema(contentsOfFile: descPath)
let scalars = schema.message("structtest.Scalars3")!

for tier in 0..<cases.count {
    let n = repeats[tier]
    let official = fillOfficial(n)
    let reference = try official.serializedData()
    guard reference.count == want[tier] else {
        fputs(
            "bench: \(cases[tier]): official wrote \(reference.count) bytes, expected \(want[tier])\n",
            stderr)
        failures += 1
        continue
    }
    let built = schema.message("structtest.Scalars3")!
    try fillPbwire(built, n: n)
    let got = try built.encode()
    guard got == reference else {
        fputs(
            "bench: \(cases[tier]): pbwire wrote \(got.count) bytes, official \(reference.count)\n",
            stderr)
        failures += 1
        continue
    }
    let back = try scalars.decode(reference)
    guard try back.encode() == reference,
        back.get("str") == .string("hello"),
        back.nested("inner")?.get("name") == .string("kid"),
        back.count("nums") == n,
        n == 0 || back.at("nums", n - 1) == .int(Int64(n - 4))
    else {
        fputs("bench: \(cases[tier]): pbwire decode differs\n", stderr)
        failures += 1
        continue
    }

    let count = try calibrate(floors[tier]) { _ = try built.encode() }
    let encodeNs = try measure(count) { _ = try built.encode() }
    let decodeNs = try measure(count) { _ = try scalars.decode(reference) }
    let copy = official
    let officialEncodeNs = try measure(count) { _ = try copy.serializedData() }
    let officialDecodeNs = try measure(count) { _ = try Structtest_Scalars3(serializedBytes: reference) }

    print("\(cases[tier]): \(reference.count) bytes, \(count) iterations (repeat field x\(n))")
    print("  \("impl".padding(toLength: 12, withPad: " ", startingAt: 0)) \("op".padding(toLength: 7, withPad: " ", startingAt: 0)) \("ns/op".padding(toLength: 12, withPad: " ", startingAt: 0)) \("MB/s".padding(toLength: 10, withPad: " ", startingAt: 0))")
    row("pbwire-swift", "encode", reference.count, encodeNs)
    row("swift-protobuf", "encode", reference.count, officialEncodeNs)
    row("pbwire-swift", "decode", reference.count, decodeNs)
    row("swift-protobuf", "decode", reference.count, officialDecodeNs)
    print()
}

if failures != 0 {
    fputs("bench: \(failures) check(s) failed\n", stderr)
    exit(1)
}

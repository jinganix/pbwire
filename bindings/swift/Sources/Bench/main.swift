// Swift binding benchmark on the four standard tiers from BENCHMARKS.md.
//
// The same payload as the C/Dart/Go/Java rows: every scalar kind, the nested
// message, the two proto3 optionals, and the repeated fields scaled x0 / x1 /
// x100 / x3000, which lands the tiers on 136 / 174 / 2469 / 74913 bytes. The
// harness gates the wire size before timing anything and prints one block per
// case in the standard format (throughput over wire bytes, higher is better).
// The apple/swift-protobuf baseline lives in benchmarks/bench.sh: it needs
// the generated matrix3 bindings, so the comparison runs there, not here.
//
//   swift run -c release Bench [matrix3.desc]
import Foundation
import Pbwire

private let cases = ["small", "medium", "large", "xlarge"]
private let repeats = [0, 1, 100, 3000]
private let want = [136, 174, 2469, 74_913]
private let floors = [50_000, 50_000, 5_000, 500]
private let minPassSeconds = 0.25
private let passes = 3

private var failures = 0

private func fill(_ message: Message, inner _: Message, n: Int) throws {
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

private func now() -> UInt64 { DispatchTime.now().uptimeNanoseconds }

// Best of `passes` runs, each at least `count` iterations.
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

let sourceDir = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
let descPath =
    CommandLine.arguments.count > 1
    ? CommandLine.arguments[1]
    : sourceDir.deletingLastPathComponent().deletingLastPathComponent()
    .appendingPathComponent("tests/data/matrix3.desc").path

let schema = try Schema(contentsOfFile: descPath)
let scalars = schema.message("structtest.Scalars3")!

for tier in 0..<cases.count {
    let n = repeats[tier]
    // Build once, then time encode of the tree and decode of its bytes.
    let built = schema.message("structtest.Scalars3")!
    try fill(built, inner: built, n: n)
    let reference = try built.encode()
    guard reference.count == want[tier] else {
        fputs(
            "bench: \(cases[tier]): wrote \(reference.count) bytes, expected \(want[tier])\n",
            stderr)
        failures += 1
        continue
    }
    // Decode must round-trip byte-exactly and expose the same values.
    let back = try scalars.decode(reference)
    guard try back.encode() == reference,
        back.get("str") == .string("hello"),
        back.nested("inner")?.get("name") == .string("kid"),
        back.count("nums") == n,
        n == 0 || back.at("nums", n - 1) == .int(Int64(n - 4))
    else {
        fputs("bench: \(cases[tier]): decode differs\n", stderr)
        failures += 1
        continue
    }

    let count = try calibrate(floors[tier]) { _ = try built.encode() }
    let encodeNs = try measure(count) { _ = try built.encode() }
    let decodeNs = try measure(count) { _ = try scalars.decode(reference) }

    print("\(cases[tier]): \(reference.count) bytes, \(count) iterations (repeat field x\(n))")
    print("  \("impl".padding(toLength: 12, withPad: " ", startingAt: 0)) \("op".padding(toLength: 7, withPad: " ", startingAt: 0)) \("ns/op".padding(toLength: 12, withPad: " ", startingAt: 0)) \("MB/s".padding(toLength: 10, withPad: " ", startingAt: 0))")
    row("pbwire-swift", "encode", reference.count, encodeNs)
    row("pbwire-swift", "decode", reference.count, decodeNs)
    print()
}

if failures != 0 {
    fputs("bench: \(failures) check(s) failed\n", stderr)
    exit(1)
}

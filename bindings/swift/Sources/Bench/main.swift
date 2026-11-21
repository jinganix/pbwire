// Swift binding benchmark on the four standard tiers from BENCHMARKS.md.
//
// Three rows per tier, the same payload as the C/Dart/Go/Java/Julia/Rust rows
// (every scalar kind, the nested message, the two proto3 optionals, and the
// repeated fields scaled x0 / x1 / x100 / x3000, which lands the tiers on
// 136 / 174 / 2469 / 74913 bytes):
//
//   pbwire-swift-tree  the shipped handle, prebuilt: `encode()` / `decode()`
//   pbwire-swift-shim  the generated `@frozen Structtest_Scalars3Repr`, read
//                      and written in place: one `encodeInPlace` call and one
//                      `decodeInPlace` call into the C layout codec
//   pbwire-swift-core  the pure-Swift `PbwireCore` product: a prebuilt
//                      `Message` tree, `encode`/`decode` with no C library
//
// The harness gates the wire size and every payload field before timing
// anything, and prints the standard block format (throughput over wire bytes,
// higher is better). The apple/swift-protobuf baseline lives in
// benchmarks/bench.sh: it needs the official generated bindings, so the
// comparison runs there, not here.
//
//   swift run -c release Bench [matrix3.desc]
//   swift run -c release Bench compare [matrix3.desc]   # provisional table
import Foundation
import Pbwire
import Pbench

private var failures = 0

let sourceDir = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
let arguments = CommandLine.arguments.dropFirst()
let compare = arguments.first == "compare"
let descArg = arguments.last == "compare" ? nil : arguments.last
let descPath =
    descArg
    ?? sourceDir.deletingLastPathComponent().deletingLastPathComponent()
    .appendingPathComponent("tests/data/matrix3.desc").path

let schema = try Schema(contentsOfFile: descPath)

if compare {
    exit(try runCompare(schema: schema, descPath: descPath))
}

for tier in pbenchTiers {
    let n = tier.repeats
    let tree = try TreeArm(schema: schema, repeats: n)
    let shim = try ShimArm(schema: schema, repeats: n)
    let core = try CoreArm(descPath: descPath, repeats: n)

    let reference = try tree.encode()
    guard reference.count == tier.bytes else {
        fputs(
            "bench: \(tier.name): wrote \(reference.count) bytes, expected \(tier.bytes)\n",
            stderr)
        failures += 1
        continue
    }
    // Both arms must write the same bytes, and both decode paths must expose
    // the payload before anything is timed.
    guard try shim.encode() == reference else {
        fputs("bench: \(tier.name): mirror bytes differ from the handle's\n", stderr)
        failures += 1
        continue
    }
    let treeBack = try tree.decode(reference)
    if let bad = tree.readBack(treeBack, repeats: n) {
        fputs("bench: \(tier.name): tree decode differs at \(bad)\n", stderr)
        failures += 1
        continue
    }
    let shimBack = try shim.decode(reference)
    if let bad = shim.readBack(shimBack, repeats: n) {
        fputs("bench: \(tier.name): mirror decode differs at \(bad)\n", stderr)
        failures += 1
        continue
    }
    // The core arm is a second wire implementation: its bytes must equal the
    // C core's reference image, and its decoded tree must expose the payload.
    let coreWire = try core.encode()
    guard coreWire == [UInt8](reference) else {
        fputs("bench: \(tier.name): core bytes differ from the handle's\n", stderr)
        failures += 1
        continue
    }
    if let bad = try core.readBack(try core.decode(coreWire), repeats: n) {
        fputs("bench: \(tier.name): core decode differs at \(bad)\n", stderr)
        failures += 1
        continue
    }

    let count = try Harness.calibrate(tier.floor) { _ = try tree.encode() }
    let treeEncodeNs = try Harness.best(count) { _ = try tree.encode() }
    let shimEncodeNs = try Harness.best(count) { _ = try shim.encode() }
    let treeDecodeNs = try Harness.best(count) { _ = try tree.decode(reference) }
    let shimDecodeNs = try Harness.best(count) { _ = try shim.decode(reference) }
    let coreEncodeNs = try Harness.best(count) { _ = try core.encode() }
    let coreDecodeNs = try Harness.best(count) { _ = try core.decode(coreWire) }

    Harness.header(tier, reference.count, count)
    Harness.row("pbwire-swift-tree", "encode", reference.count, treeEncodeNs)
    Harness.row("pbwire-swift-shim", "encode", reference.count, shimEncodeNs)
    Harness.row("pbwire-swift-tree", "decode", reference.count, treeDecodeNs)
    Harness.row("pbwire-swift-shim", "decode", reference.count, shimDecodeNs)
    Harness.row("pbwire-swift-core", "encode", reference.count, coreEncodeNs)
    Harness.row("pbwire-swift-core", "decode", reference.count, coreDecodeNs)
    print()
}

if failures != 0 {
    fputs("bench: \(failures) check(s) failed\n", stderr)
    exit(1)
}

// Swift binding benchmark on the four standard tiers from BENCHMARKS.md.
//
// Three rows per tier, the same payload as the C/Dart/Go/Java/Julia/Rust rows
// (every scalar kind, the nested message, the two proto3 optionals, and the
// repeated fields scaled x0 / x1 / x100 / x3000, which lands the tiers on
// 136 / 174 / 2469 / 74913 bytes):
//
//   pbwire-swift-tree   the shipped handle full task: a fresh native
//                       `Message` per iteration (one `set` per field, `encode`,
//                       deinit frees inside the timer); a fresh `decode` per
//                       iteration with every gate-checked field folded into a
//                       live sink, deinit frees inside the timer
//   pbwire-swift-proxy  the old prebuilt shape, relabelled not re-measured
//                       as a new task: a held handle's `encode`, bare decode
//                       with no materialization (same-run timing only, never
//                       the headline)
//   pbwire-swift-shim   the generated `@frozen Structtest_Scalars3Repr`, read
//                       and written in place: one `encodeInPlace` call and one
//                       `decodeInPlace` call into the C layout codec. The
//                       app-owned image and arena live across iterations
//                       (steady state: an application reuses its buffers).
//   pbwire-swift-core   the pure-Swift `PbwireCore` product: a prebuilt
//                       `Message` tree, `encode`/`decode` with no C library.
//                       The app-owned tree lives across iterations
//                       (steady state, same carve-out as the shim).
//
// The harness gates the wire size and every payload field before timing
// anything, and prints the standard block format (throughput over wire bytes,
// higher is better). The apple/swift-protobuf baseline lives in
// benchmarks/bench.sh: it needs the official generated bindings, so the
// comparison runs there, not here.
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

/// Folds every gate-checked tree field into `sink`, so the timed decode
/// materializes the same field list the gate compares
/// (`TreeArm.readBack`, same list by construction).
func consumeTree(_ back: Message, repeats n: Int, sink: inout UInt64) {
    func fold(_ v: UInt64) { sink &+= v &+ 0x9E3779B97F4A7C15 }
    func fold(_ v: Int64) { fold(UInt64(bitPattern: v)) }
    func fold(_ s: String) {
        fold(UInt64(s.utf8.count))
        for b in s.utf8 { fold(UInt64(b)) }
    }
    if case .string(let s)? = back.get("str") { fold(s) }
    if case .string(let s)? = back.nested("inner")?.get("name") { fold(s) }
    if case .int(let v)? = back.nested("inner")?.get("value") { fold(v) }
    if case .uint(let v)? = back.get("u64") { fold(v) }
    if case .int(let v)? = back.get("color") { fold(v) }
    if case .int(let v)? = back.get("opt_i32") { fold(v) }
    if case .string(let s)? = back.get("opt_str") { fold(s) }
    fold(Int64(back.count("nums")))
    if n > 0, case .int(let v)? = back.at("nums", n - 1) { fold(v) }
    fold(Int64(back.count("tags")))
    if n > 0, case .string(let s)? = back.at("tags", n - 1) { fold(s) }
    fold(Int64(back.count("inners")))
    if n > 0, case .message(let m)? = back.at("inners", n - 1) {
        if case .int(let v)? = m.get("value") { fold(v) }
        if case .string(let s)? = m.get("name") { fold(s) }
    }
    fold(Int64(back.count("flags")))
}

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

    // The full-task gate: the timer's fresh build must write the same bytes,
    // expose the same fields, and re-encode byte-exactly, on every tier.
    guard let freshProbe = schema.message("structtest.Scalars3") else {
        fputs("bench: \(tier.name): schema has no structtest.Scalars3\n", stderr)
        failures += 1
        continue
    }
    try fillTree(freshProbe, repeats: n)
    guard try freshProbe.encode() == reference else {
        fputs("bench: \(tier.name): fresh-build bytes differ from the handle's\n", stderr)
        failures += 1
        continue
    }
    let freshBack = try tree.decode(reference)
    if let bad = tree.readBack(freshBack, repeats: n) {
        fputs("bench: \(tier.name): fresh decode differs at \(bad)\n", stderr)
        failures += 1
        continue
    }
    guard try freshBack.encode() == reference else {
        fputs("bench: \(tier.name): decoded re-encode differs\n", stderr)
        failures += 1
        continue
    }

    // Floor calibration stays on the fast prebuilt op: calibrating on the
    // slower full-task op would inflate every row's iteration count and the
    // run time.
    let count = try Harness.calibrate(tier.floor) { _ = try tree.encode() }

    // The tree full task (Go parity): a fresh native `Message` per
    // iteration (one `set` per field, `encode`, deinit frees inside the
    // timer); a fresh decode per iteration with every gate-checked field
    // folded into a live sink, deinit frees inside the timer.
    var treeSink: UInt64 = 0
    let treeEncodeNs = try Harness.best(count) {
        guard let fresh = schema.message("structtest.Scalars3") else {
            throw PbenchError("schema has no structtest.Scalars3")
        }
        try fillTree(fresh, repeats: n)
        treeSink &+= UInt64(try fresh.encode().count)
    }
    let treeDecodeNs = try Harness.best(count) {
        let back = try tree.decode(reference)
        consumeTree(back, repeats: n, sink: &treeSink)
    }
    if treeSink == UInt64.max {
        fputs("bench: unreachable sink\n", stderr)
    }
    // The proxy task: the old prebuilt shape (a held handle's `encode`,
    // bare decode with no materialization), relabelled not re-measured as
    // a new task, never the headline. Same-run timing only: it is timed
    // here because the row must print beside the other rows.
    let proxyEncodeNs = try Harness.best(count) { _ = try tree.encode() }
    let proxyDecodeNs = try Harness.best(count) { _ = try tree.decode(reference) }
    let shimEncodeNs = try Harness.best(count) { _ = try shim.encode() }
    let shimDecodeNs = try Harness.best(count) { _ = try shim.decode(reference) }
    let coreEncodeNs = try Harness.best(count) { _ = try core.encode() }
    let coreDecodeNs = try Harness.best(count) { _ = try core.decode(coreWire) }

    Harness.header(tier, reference.count, count)
    Harness.row("pbwire-swift-tree", "encode", reference.count, treeEncodeNs)
    Harness.row("pbwire-swift-tree", "decode", reference.count, treeDecodeNs)
    Harness.row("pbwire-swift-proxy", "encode", reference.count, proxyEncodeNs)
    Harness.row("pbwire-swift-proxy", "decode", reference.count, proxyDecodeNs)
    Harness.row("pbwire-swift-shim", "encode", reference.count, shimEncodeNs)
    Harness.row("pbwire-swift-shim", "decode", reference.count, shimDecodeNs)
    Harness.row("pbwire-swift-core", "encode", reference.count, coreEncodeNs)
    Harness.row("pbwire-swift-core", "decode", reference.count, coreDecodeNs)
    print()
}

if failures != 0 {
    fputs("bench: \(failures) check(s) failed\n", stderr)
    exit(1)
}

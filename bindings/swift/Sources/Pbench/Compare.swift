// The provisional table (Stage 3): interleaved rounds, median of five, both
// arms back to back on the same tier. `swift run -c release Bench compare`.
//
// Bytes and every payload field are checked before anything is timed, so a
// row that prints is a row whose two arms agree with each other and with the
// pinned wire size.
import Foundation
import Pbwire

/// Runs the provisional compare over the four tiers; returns 1 on any check
/// failure (bytes, sizes or a field), 0 otherwise.
public func runCompare(schema: Schema) throws -> Int32 {
    var failures = 0
    let rounds = 5

    for tier in pbenchTiers {
        let n = tier.repeats
        let tree = try TreeArm(schema: schema, repeats: n)
        let shim = try ShimArm(schema: schema, repeats: n)

        let treeBytes = try tree.encode()
        let shimBytes = try shim.encode()
        guard treeBytes.count == tier.bytes else {
            fputs("compare: \(tier.name): tree wrote \(treeBytes.count) bytes, want \(tier.bytes)\n", stderr)
            failures += 1
            continue
        }
        guard shimBytes == treeBytes else {
            fputs("compare: \(tier.name): mirror bytes differ from the handle's\n", stderr)
            failures += 1
            continue
        }
        let treeBack = try tree.decode(treeBytes)
        if let bad = tree.readBack(treeBack, repeats: n) {
            fputs("compare: \(tier.name): tree decode differs at \(bad)\n", stderr)
            failures += 1
            continue
        }
        let shimBack = try shim.decode(shimBytes)
        if let bad = shim.readBack(shimBack, repeats: n) {
            fputs("compare: \(tier.name): mirror decode differs at \(bad)\n", stderr)
            failures += 1
            continue
        }
        guard try shim.encode() == shimBytes else {
            fputs("compare: \(tier.name): mirror re-encode differs\n", stderr)
            failures += 1
            continue
        }

        let count = try Harness.calibrate(tier.floor) { _ = try tree.encode() }
        let ops: [(String, () throws -> Void)] = [
            ("tree", { _ = try tree.encode() }),
            ("shim", { _ = try shim.encode() }),
            ("tree", { _ = try tree.decode(treeBytes) }),
            ("shim", { _ = try shim.decode(shimBytes) }),
        ]
        var samples = [[Double]](repeating: [], count: ops.count)
        for _ in 0..<rounds {
            for (i, op) in ops.enumerated() {
                let begin = Harness.now()
                for _ in 0..<count { try op.1() }
                samples[i].append(Double(Harness.now() - begin) / Double(count))
            }
        }
        Harness.header(tier, tier.bytes, count)
        for (i, op) in ops.enumerated() {
            let median = samples[i].sorted()[rounds / 2]
            Harness.row("pbwire-swift-\(op.0)", i < 2 ? "encode" : "decode", tier.bytes, median)
        }
        print()
    }
    return failures == 0 ? 0 : 1
}

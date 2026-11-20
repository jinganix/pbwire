// The two Swift pbench arms, on the same payload and tiers.
//
//  - `tree` (a-tree): the shipped `Message` handle. The formal row times a
//    *prebuilt* handle's `encode()` and `decode()`, exactly as the harness did
//    before this plan; building the handle costs one downcall per field and is
//    the per-field walk issue 41 targets.
//  - `shim` (b-shim): the generated `@frozen Structtest_Scalars3Repr`. One
//    `encodeInPlace` / `decodeInPlace` call walks that memory in place in C
//    (`bindings/swift/Sources/CPbwire/shim.c` + the C binding's layout codec),
//    so no field crosses the boundary.
import Foundation
import Pbwire

/// A pbench-side failure (the arms are a benchmark, not part of the binding).
public struct PbenchError: Error, CustomStringConvertible {
    public let message: String
    public init(_ message: String) { self.message = message }
    public var description: String { message }
}

// MARK: - tree arm

public final class TreeArm {
    public let message: Message

    public init(schema: Schema, repeats: Int) throws {
        guard let message = schema.message("structtest.Scalars3") else {
            throw PbenchError("schema has no structtest.Scalars3")
        }
        try fillTree(message, repeats: repeats)
        self.message = message
    }

    public func encode() throws -> Data { try message.encode() }

    public func decode(_ wire: Data) throws -> Message { try message.decode(wire) }

    /// Reads every field the payload sets, so the decoded tree is as readable
    /// as the mirror. Returns nil when a value differs.
    public func readBack(_ back: Message, repeats n: Int) -> String? {
        if back.get("str") != .string("hello") { return "str" }
        if back.nested("inner")?.get("name") != .string("kid") { return "inner.name" }
        if back.nested("inner")?.get("value") != .int(42) { return "inner.value" }
        if back.get("u64") != .uint(UInt64.max) { return "u64" }
        if back.get("color") != .int(2) { return "color" }
        if back.get("opt_i32") != .int(-7) { return "opt_i32" }
        if back.get("opt_str") != .string("opt") { return "opt_str" }
        if back.count("nums") != n { return "nums count" }
        if n > 0, back.at("nums", n - 1) != .int(Int64(n - 4)) { return "nums last" }
        if back.count("tags") != n { return "tags count" }
        if n > 0, back.at("tags", n - 1) != .string("tag") { return "tags last" }
        if back.count("inners") != n { return "inners count" }
        if n > 0 {
            guard let last = back.at("inners", n - 1)?.messageProbe,
                last == (Int64(n), "grandkid")
            else { return "inners last" }
        }
        if back.count("flags") != n { return "flags count" }
        return nil
    }
}

extension PbwireValue {
    // (name, value) of a decoded sub-message, for the inners comparison.
    fileprivate var messageProbe: (Int64, String)? {
        guard case .message(let inner) = self,
            case .int(let value)? = inner.get("value"),
            case .string(let name)? = inner.get("name")
        else { return nil }
        return (value, name)
    }
}

// MARK: - shim arm

public final class ShimArm {
    /// The registered mirror type: compiled C layout codec, reusable encode
    /// buffer and decode arena, one per schema.
    public let mirror: InPlace<Structtest_Scalars3Repr>
    private let storage = ReprStorage()
    /// The native object this arm times: a value whose pointers borrow
    /// `storage` (no references, so the walk pays no ARC).
    public var image = Structtest_Scalars3Repr()

    public init(schema: Schema, repeats: Int) throws {
        mirror = try schema.inPlace(Structtest_Scalars3Repr.self)
        fillMirror(&image, repeats: repeats, storage: storage)
    }

    public func encode() throws -> Data { try mirror.encodeInPlace(image) }

    /// One call: wire bytes in, a mirror out. The result borrows the mirror
    /// type's arena, so it stays readable until the next decode of this type.
    public func decode(_ wire: Data) throws -> Structtest_Scalars3Repr {
        var out = Structtest_Scalars3Repr()
        try mirror.decodeInPlace(wire, into: &out)
        return out
    }

    /// Reads every field the payload sets; nil when a value differs.
    public func readBack(_ back: Structtest_Scalars3Repr, repeats n: Int) -> String? {
        if back.text != "hello" { return "str" }
        if back.innerValue?.value != 42 { return "inner.value" }
        if back.innerValue?.name.map({ String(cString: $0) }) != "kid" { return "inner.name" }
        if back.u64 != UInt64.max { return "u64" }
        if back.color != 2 { return "color" }
        if back.optI32 != -7 || back.hasOptI32 != 1 { return "opt_i32" }
        if back.optText != "opt" { return "opt_str" }
        if back.numsValues.count != n { return "nums count" }
        if n > 0, back.numsValues[n - 1] != Int32(n - 4) { return "nums last" }
        if back.tagsValues.count != n { return "tags count" }
        if n > 0, back.tagsValues[n - 1] != "tag" { return "tags last" }
        if back.innersValues.count != n { return "inners count" }
        if n > 0 {
            let last = back.innersValues[n - 1]
            if last.value != Int32(n) || String(cString: last.name!) != "grandkid" {
                return "inners last"
            }
        }
        if back.flagsValues.count != n { return "flags count" }
        return nil
    }
}

// MARK: - timing harness

public enum Harness {
    public static func now() -> UInt64 { DispatchTime.now().uptimeNanoseconds }

    /// Doubles `start` until one pass lasts at least `minSeconds`.
    public static func calibrate(
        _ start: Int, minSeconds: Double = 0.25, _ op: () throws -> Void
    ) rethrows -> Int {
        var count = start
        while true {
            let begin = now()
            for _ in 0..<count { try op() }
            if Double(now() - begin) / 1e9 >= minSeconds { return count }
            count *= 2
        }
    }

    /// Best of `passes` runs of `count` iterations, in ns/op.
    public static func best(
        _ count: Int, passes: Int = 3, _ op: () throws -> Void
    ) rethrows -> Double {
        var best = Double.greatestFiniteMagnitude
        for _ in 0..<passes {
            let begin = now()
            for _ in 0..<count { try op() }
            best = min(best, Double(now() - begin) / Double(count))
        }
        return best
    }

    /// Median of `rounds` runs of `count` iterations, in ns/op.
    public static func median(
        _ count: Int, rounds: Int = 5, _ op: () throws -> Void
    ) rethrows -> Double {
        var samples: [Double] = []
        for _ in 0..<rounds {
            let begin = now()
            for _ in 0..<count { try op() }
            samples.append(Double(now() - begin) / Double(count))
        }
        samples.sort()
        return samples[samples.count / 2]
    }

    /// Left-justified, never truncated: an impl name longer than the column
    /// (`pbwire-swift-shim`) pushes the next one right, like the C and Rust
    /// harnesses' printf padding.
    private static func pad(_ text: String, _ width: Int) -> String {
        text.count >= width
            ? text : text + String(repeating: " ", count: width - text.count)
    }

    public static func row(_ impl: String, _ op: String, _ bytes: Int, _ ns: Double) {
        let mbs = Double(bytes) / ns * 1000
        print(
            "  \(pad(impl, 12)) \(pad(op, 7)) \(String(format: "%12.1f", ns)) "
                + "\(String(format: "%10.1f", mbs))")
    }

    public static func header(_ tier: Tier, _ bytes: Int, _ count: Int) {
        print("\(tier.name): \(bytes) bytes, \(count) iterations (repeat field x\(tier.repeats))")
        print(
            "  \(pad("impl", 12)) \(pad("op", 7)) \(pad("ns/op", 12)) \(pad("MB/s", 10))")
    }
}

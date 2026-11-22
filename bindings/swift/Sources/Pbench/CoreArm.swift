// The `core` arm of the Swift pbench: the pure-Swift `PbwireCore` product as
// the codec, with no C library anywhere in the path.
//
// Same user-visible task as the other two arms: the payload is a native object
// (here the core's own `Message` tree, built through the same per-field `set`
// calls the `tree` arm makes), a prebuilt tree in and wire bytes out, wire
// bytes in and a fully readable tree out. Build time is outside the timer,
// exactly like `pbwire-swift-tree`'s prebuilt handle, so the rows differ only
// in who walks the wire.
import Foundation
import PbwireCore

public final class CoreArm {
    public let ty: MessageType
    /// The prebuilt tree this arm times.
    public let message: Message

    public init(descPath: String, repeats n: Int) throws {
        let schema = try Schema(contentsOfFile: descPath)
        ty = try schema.message("structtest.Scalars3")
        message = try Self.build(ty, n)
    }

    /// The matrix3 payload through the core tree: every scalar kind at its edge,
    /// the nested message, the two proto3 optionals and the repeated fields
    /// scaled by `n`. Same values as `fillTree`.
    private static func build(_ ty: MessageType, _ n: Int) throws -> Message {
        let m = ty.newMessage()
        try m.setI64("i32", Int64(Int32.min))
        try m.setI64("i64", Int64.max)
        try m.setU64("u32", UInt64(UInt32.max))
        try m.setU64("u64", UInt64.max)
        try m.setI64("s32", Int64(Int32.min))
        try m.setI64("s64", Int64.min)
        try m.setU64("fx32", 0xDEAD_BEEF)
        try m.setU64("fx64", 72_623_859_790_382_856)
        try m.setI64("sfx32", Int64(Int32.min))
        try m.setI64("sfx64", Int64.min)
        try m.setF32("f32", .infinity)
        try m.setF64("f64", -123456.789)
        try m.setBool("flag", true)
        try m.setStr("str", "hello")
        try m.setI64("color", 2)
        do {
            let inner = try m.setMessage("inner")
            try inner.setStr("name", "kid")
            try inner.setI64("value", 42)
        }
        try m.setI64("opt_i32", -7)
        try m.setStr("opt_str", "opt")
        for i in 0..<n {
            try m.push("nums", .i32(Int32(i - 3)))
            try m.push("tags", .bytes(Array("tag".utf8)))
            let grandkid = try m.pushMessage("inners")
            try grandkid.setStr("name", "grandkid")
            try grandkid.setI64("value", Int64(i + 1))
            try m.push("flags", .bool(i % 2 == 0))
        }
        return m
    }

    public func encode() throws -> [UInt8] { try ty.encode(message) }

    /// One encode into a reused buffer: the hot-loop shape every arm uses.
    public func encode(into buffer: inout [UInt8]) throws -> Int {
        try ty.encode(message, into: &buffer)
        return buffer.count
    }

    /// One-shot decode, like the other arms' decode rows.
    public func decode(_ wire: [UInt8]) throws -> Message { try ty.decode(wire) }

    /// Reads every field the payload sets; nil when a value differs.
    public func readBack(_ back: Message, repeats n: Int) throws -> String? {
        if try back.getStr("str") != "hello" { return "str" }
        if try back.getMessage("inner")?.getStr("name") != "kid" { return "inner.name" }
        if try back.getMessage("inner")?.getI64("value") != 42 { return "inner.value" }
        if try back.getU64("u64") != UInt64.max { return "u64" }
        if try back.getI64("color") != 2 { return "color" }
        if try back.getI64("opt_i32") != -7 { return "opt_i32" }
        if try back.getStr("opt_str") != "opt" { return "opt_str" }
        if try back.count("nums") != n { return "nums count" }
        if n > 0, try back.items("nums")[n - 1].asI64 != Int64(n - 4) { return "nums last" }
        if try back.count("tags") != n { return "tags count" }
        if n > 0, try back.items("tags")[n - 1].asBytes != Array("tag".utf8) {
            return "tags last"
        }
        if try back.count("inners") != n { return "inners count" }
        if n > 0 {
            let last = try back.items("inners")[n - 1].asMessage
            if try last?.getI64("value") != Int64(n) { return "inners last.value" }
            if try last?.getStr("name") != "grandkid" { return "inners last.name" }
        }
        if try back.count("flags") != n { return "flags count" }
        return nil
    }
}

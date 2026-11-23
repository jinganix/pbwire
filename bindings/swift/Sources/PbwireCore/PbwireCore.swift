// The pure-Swift wire core: a `FileDescriptorSet` reader, a value tree, wire
// primitives and a binary encoder/decoder, with no C module, no `libpbwire.a`
// and no `unsafeFlags`.
//
// It is **not** a replacement for the `Pbwire` product, which links the C core
// statically (see the package manifest). It is the `core` arm of the pbench
// three-arm contract and the product a SwiftPM consumer can depend on when a
// static C archive is not an option:
//
// ```swift
// let schema = try Schema(contentsOfFile: "tests/data/matrix3.desc")
// let ty = try schema.message("structtest.Scalars3")
// let msg = ty.newMessage()
// try msg.setStr("str", "hello")
// let wire = try ty.encode(msg)
// let back = try ty.decode(wire)
// XCTAssertEqual(try back.getStr("str"), "hello")
// ```
//
// The decode side keeps unknown fields (in input order, tag and payload
// verbatim), validates groups to their matching end tag, accepts packed runs for
// every numeric kind, collapses oneofs to the last member, follows each field's
// own presence rule and enforces the nesting cap.
//
// This mirrors `bindings/rust/src/core/`, `bindings/python/pure/`,
// `bindings/node/lib/core/` and `bindings/dart/lib/core/`: one host-language
// port of the C core per binding, held to the same official binary conformance
// suite (`make conformance-swift`).

import Foundation

/// A loaded `FileDescriptorSet`: the runtime schema every type comes from.
public final class Schema: @unchecked Sendable {
    public let messages: [MsgDef]
    public let enums: [EnumDef]
    let messageByName: [String: Int]
    let enumByName: [String: Int]
    /// The file the set came from, when it came from a file.
    public let path: String?

    init(messages: [MsgDef], enums: [EnumDef], messageByName: [String: Int],
         enumByName: [String: Int], path: String? = nil)
    {
        self.messages = messages
        self.enums = enums
        self.messageByName = messageByName
        self.enumByName = enumByName
        self.path = path
    }

    /// Loads a descriptor set written by `protoc --descriptor_set_out`.
    public convenience init(contentsOfFile path: String) throws {
        let data = FileManager.default.contents(atPath: path)
        guard let data else {
            throw CoreError("\(path): cannot read the descriptor set")
        }
        let loaded = try loadSchema([UInt8](data), Limits())
        self.init(
            messages: loaded.messages, enums: loaded.enums,
            messageByName: loaded.messageByName, enumByName: loaded.enumByName, path: path)
    }

    /// Loads descriptor-set bytes; use `--include_imports` so every type
    /// resolves.
    public init(bytes: [UInt8], limits: Limits = Limits()) throws {
        let loaded = try loadSchema(bytes, limits)
        self.messages = loaded.messages
        self.enums = loaded.enums
        self.messageByName = loaded.messageByName
        self.enumByName = loaded.enumByName
        self.path = nil
    }

    /// Looks up a message by its fully qualified name.
    public func message(_ fullName: String) throws -> MessageType {
        guard let def = messageByName[fullName] else {
            throw CoreError("no such message type: \(fullName)")
        }
        return MessageType(schema: self, defIndex: def)
    }

    /// Index of a message by its fully qualified name.
    public func messageIndex(_ fullName: String) throws -> Int {
        guard let def = messageByName[fullName] else {
            throw CoreError("no such message type: \(fullName)")
        }
        return def
    }

    /// True when the set declares that message.
    public func hasMessage(_ fullName: String) -> Bool { messageByName[fullName] != nil }
}

/// One message type: `newMessage()` plus binary decode/encode.
public struct MessageType: Sendable {
    public let schema: Schema
    /// Index of this type in `schema.messages`.
    public let defIndex: Int

    init(schema: Schema, defIndex: Int) {
        self.schema = schema
        self.defIndex = defIndex
    }

    /// Fully qualified name, e.g. `structtest.Scalars3`.
    public var name: String { def.fullName }

    /// The compiled field table.
    public var def: MsgDef { schema.messages[defIndex] }

    public var fields: [Field] { def.fields }

    public func field(_ name: String) throws -> Field {
        guard let slot = def.byName[name] else {
            throw CoreError("\(def.fullName) has no field \(name)")
        }
        return def.fields[slot]
    }

    /// An empty, mutable message of this type.
    public func newMessage() -> Message { Message(schema: schema, def: defIndex) }

    /// Decodes wire bytes; unknown fields are kept and re-encoded. The bytes are
    /// the authority on presence, so a field absent on the wire stays absent.
    public func decode(_ data: [UInt8], limits: Limits = Limits()) throws -> Message {
        try PbwireCore.decode(schema, defIndex, data, limits)
    }

    /// Decodes one body slice (offset + length) of a larger buffer.
    public func decodeSlice(_ data: [UInt8], offset: Int, length: Int,
                            limits: Limits = Limits()) throws -> Message
    {
        try PbwireCore.decodeSlice(schema, defIndex, data, offset, length, limits)
    }

    /// Encodes a tree; a tree that came from `decode()` re-emits its bytes.
    public func encode(_ m: Message) throws -> [UInt8] {
        try PbwireCore.encode(m)
    }

    /// Encodes into a reused buffer: a hot loop allocates nothing.
    public func encode(_ m: Message, into buffer: inout [UInt8]) throws {
        try PbwireCore.encode(m, into: &buffer)
    }
}

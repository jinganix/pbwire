/// pbwire Swift binding: an FFI host over the C ABI, like the Objective-C
/// and Dart bindings. A `FileDescriptorSet` (`protoc --descriptor_set_out`)
/// becomes a schema at runtime; messages are `Message` objects with field
/// access by name. No code generation, no protoc plugin, no wire code.
///
///     let schema = try Schema(contentsOfFile: "probe.desc")
///     let message = schema.message("pbwswift.Probe")!
///     try message.set(.int(1234), "i32")
///     try message.set(.string("alice"), "text")
///     try message.makeNested("inner").set(.string("child"), "label")
///     let wire = try message.encode()
///     let decoded = try message.decode(wire)
///     assert(decoded.get("text") == .string("alice"))
///
/// Ownership mirrors the Objective-C wrapper: a root `Message` owns its C
/// tree and frees it on release; a nested message is a view that retains its
/// parent, so it cannot outlive the storage it points into. A message keeps
/// its schema alive the same way. One tree belongs to one thread, like the
/// C core.

import CPbwire
import Foundation

// MARK: - Errors

/// Every failure in the binding. `status` is the `pbw_status` of the failing
/// C call, so callers never need the C ABI to find out what happened.
public struct PbwireError: Error, CustomStringConvertible, Equatable {
    public let status: Int32
    public let message: String

    public var description: String { "PbwireError(\(status)): \(message)" }
}

// The C enums import with an unsigned raw value; the binding reports Int32.
extension pbw_status {
    var code: Int32 { Int32(rawValue) }
}

// Zero fields mean the C defaults (no byte cap, PBW_MAX_DEPTH nesting). A
// negative value would wrap into the unsigned C field and silently disable the
// cap, so it is rejected; a depth above UInt32.max clamps instead of trapping.
private func makeLimits(maxBytes: Int, maxDepth: Int) throws -> pbw_limits {
    guard maxBytes >= 0, maxDepth >= 0 else {
        throw PbwireError(status: PBW_ERR_INVALID.code,
                          message: "limits must not be negative")
    }
    let depth = maxDepth > Int(UInt32.max) ? UInt32.max : UInt32(maxDepth)
    return pbw_limits(max_bytes: size_t(maxBytes), max_depth: depth)
}

private func throwStatus(_ status: pbw_status, _ detail: String? = nil) throws -> Never {
    var message = String(cString: pbw_strerror(status))
    if let detail, !detail.isEmpty { message += ": \(detail)" }
    throw PbwireError(status: status.code, message: message)
}

// MARK: - Values

/// One field value. The field's schema type decides the wire type, so an
/// integer is written as whatever the `.proto` declared.
public enum PbwireValue: Equatable {
    case int(Int64) // int32/int64/sint*/sfixed*/enum
    case uint(UInt64) // uint32/uint64/fixed*
    case bool(Bool)
    case float(Float)
    case double(Double)
    case string(String)
    case bytes(Data)
    case message(Message)
}

// MARK: - Schema

/// A loaded `FileDescriptorSet`: the schema every `Message` comes from.
public final class Schema {
    fileprivate var handle: OpaquePointer?

    /// Loads a schema from a serialized `FileDescriptorSet` file.
    public convenience init(contentsOfFile path: String) throws {
        let data: Data
        do {
            data = try Data(contentsOf: URL(fileURLWithPath: path))
        } catch {
            throw PbwireError(status: -1, message: "cannot read \(path): \(error)")
        }
        try self.init(bytes: data)
    }

    /// Loads a schema from descriptor-set bytes, e.g. embedded in the bundle.
    /// Use `--include_imports` so every referenced type resolves.
    public init(bytes: Data) throws {
        var schema: OpaquePointer?
        let status = bytes.withUnsafeBytes { raw in
            pbw_descriptor_set_load(raw.baseAddress, bytes.count, &schema)
        }
        guard status == PBW_OK else {
            // *out holds the partially built schema so the reason survives.
            var reason = String(cString: pbw_strerror(status))
            if let schema, let why = pbw_schema_error(schema) {
                let text = String(cString: why)
                if !text.isEmpty { reason += ": \(text)" }
                pbw_schema_free(schema)
            }
            throw PbwireError(status: status.code, message: reason)
        }
        guard let schema else {
            throw PbwireError(status: -1, message: "descriptor set: no schema returned")
        }
        handle = schema
    }

    deinit {
        if let handle { pbw_schema_free(handle) }
    }

    /// An empty message of that fully qualified type (`"pkg.Outer.Inner"`),
    /// or nil when the name is unknown or names an enum.
    public func message(_ fullName: String) -> Message? {
        guard let handle else { return nil }
        return fullName.withCString { name in
            guard let desc = pbw_schema_find(handle, name),
                pbw_descriptor_is_enum(desc) == 0,
                let tree = pbw_message_new(desc)
            else { return nil }
            return Message(schema: self, descriptor: desc, tree: tree, owned: true, parent: nil)
        }
    }
}

// MARK: - Messages

/// One dynamic message. A root owns its tree; a nested message borrows it.
public final class Message: Equatable {
    fileprivate let schema: Schema // owns the descriptor below
    fileprivate let descriptor: OpaquePointer
    fileprivate var tree: OpaquePointer?
    fileprivate let ownsTree: Bool
    fileprivate let parent: Message? // a view must not outlive its storage

    fileprivate init(
        schema: Schema, descriptor: OpaquePointer, tree: OpaquePointer,
        owned: Bool, parent: Message?
    ) {
        self.schema = schema
        self.descriptor = descriptor
        self.tree = tree
        self.ownsTree = owned
        self.parent = parent
    }

    deinit {
        // Frees the whole decoded tree, nested messages included; a nested
        // view (owned == false) frees nothing, its parent does.
        if ownsTree, let tree { pbw_message_free(tree) }
    }

    public static func == (lhs: Message, rhs: Message) -> Bool { lhs === rhs }

    /// The message's full type name (`"pkg.Outer.Inner"`).
    public var typeName: String { String(cString: pbw_descriptor_name(descriptor)) }

    private func field(_ name: String) -> OpaquePointer? {
        name.withCString { pbw_descriptor_find_field_name(descriptor, $0) }
    }

    /// True when the field has a value (set here or arrived on the wire).
    public func has(_ name: String) -> Bool {
        guard let tree, let f = field(name) else { return false }
        return pbw_message_get(tree, pbw_field_number(f)) != nil
    }

    /// The present value of a singular field, or nil when it is absent,
    /// unknown or repeated. A string field whose bytes are not valid UTF-8
    /// also reads as nil (the payload is kept, so re-encoding stays exact).
    public func get(_ name: String) -> PbwireValue? {
        guard let tree, let f = field(name),
            pbw_field_label(f) != PBW_LABEL_REPEATED,
            let value = pbw_message_get(tree, pbw_field_number(f))
        else { return nil }
        return read(value, as: pbw_field_type(f), sub: pbw_field_message_type(f))
    }

    /// Items in a repeated field, or 0 when the field is absent or singular.
    public func count(_ name: String) -> Int {
        guard let tree, let f = field(name),
            pbw_field_label(f) == PBW_LABEL_REPEATED,
            let value = pbw_message_get(tree, pbw_field_number(f))
        else { return 0 }
        return pbw_value_count(value)
    }

    /// One item of a repeated field, or nil when the field is not repeated
    /// or the index is out of range.
    public func at(_ name: String, _ index: Int) -> PbwireValue? {
        guard let tree, let f = field(name),
            pbw_field_label(f) == PBW_LABEL_REPEATED,
            let value = pbw_message_get(tree, pbw_field_number(f))
        else { return nil }
        guard let item = pbw_value_at(value, index) else { return nil }
        return read(item, as: pbw_field_type(f), sub: pbw_field_message_type(f))
    }

    /// Stores a singular field, replacing the previous one; nil clears it.
    /// Fails with not-found for an unknown field, type mismatch for the wrong
    /// `PbwireValue` case or an out-of-range 32-bit value, and invalid for a
    /// repeated field (use `add`) or a message field (use `makeNested`).
    @discardableResult
    public func set(_ value: PbwireValue?, _ name: String) throws -> Message {
        try store(value, name, allowRepeated: false)
    }

    /// Appends one item to a repeated field (or sets a singular one).
    @discardableResult
    public func add(_ value: PbwireValue, _ name: String) throws -> Message {
        try store(value, name, allowRepeated: true)
    }

    /// The nested message of a singular message field, or nil when absent.
    public func nested(_ name: String) -> Message? {
        guard case .message(let inner) = get(name) else { return nil }
        return inner
    }

    /// Gets or creates the nested message of a message field, filling it in
    /// place: a singular field replaces its value, a repeated one appends.
    @discardableResult
    public func makeNested(_ name: String) throws -> Message {
        guard let tree, let f = field(name) else {
            throw PbwireError(status: PBW_ERR_NOT_FOUND.code, message: "no such field: \(name)")
        }
        let type = pbw_field_type(f)
        guard type == PBW_TYPE_MESSAGE || type == PBW_TYPE_GROUP,
            let sub = pbw_field_message_type(f),
            let nested = pbw_message_set_message(tree, pbw_field_number(f))
        else {
            // This wrapper never builds in a context arena, so the only
            // failure left is an allocation failure.
            try throwStatus(PBW_ERR_NOMEM, name)
        }
        return Message(schema: schema, descriptor: sub, tree: nested, owned: false, parent: self)
    }

    /// Wire bytes of this message.
    public func encode() throws -> Data {
        guard let tree else {
            throw PbwireError(status: PBW_ERR_INVALID.code, message: "message is closed")
        }
        var bytes: UnsafeMutablePointer<UInt8>?
        var len = 0
        let status = pbw_encode(tree, &bytes, &len)
        guard status == PBW_OK else { try throwStatus(status) }
        guard len > 0, let bytes else {
            if let bytes { pbw_free(bytes) }
            return Data()
        }
        // Hand the malloc'd buffer over instead of copying it.
        return Data(bytesNoCopy: bytes, count: len, deallocator: .free)
    }

    /// Decodes bytes as a message of the same type. Zero bounds mean the
    /// defaults (no byte cap, `PBW_MAX_DEPTH` nesting); a depth above the
    /// cap clamps to it.
    public func decode(_ data: Data, maxBytes: Int = 0, maxDepth: Int = 0) throws -> Message {
        var decoded: OpaquePointer?
        var limits = try makeLimits(maxBytes: maxBytes, maxDepth: maxDepth)
        let status = data.withUnsafeBytes { raw in
            pbw_decode_limits(descriptor, raw.baseAddress, data.count, &limits, &decoded)
        }
        guard status == PBW_OK, let decoded else { try throwStatus(status) }
        return Message(
            schema: schema, descriptor: descriptor, tree: decoded, owned: true, parent: nil)
    }

    /// Wire bytes with map entries sorted by key, so equal messages encode
    /// to equal bytes regardless of insertion order.
    public func encodeDeterministic() throws -> Data {
        guard let tree else {
            throw PbwireError(status: PBW_ERR_INVALID.code, message: "message is closed")
        }
        var bytes: UnsafeMutablePointer<UInt8>?
        var len = 0
        let status = pbw_encode_deterministic(tree, &bytes, &len)
        guard status == PBW_OK else { try throwStatus(status) }
        guard len > 0, let bytes else {
            if let bytes { pbw_free(bytes) }
            return Data()
        }
        return Data(bytesNoCopy: bytes, count: len, deallocator: .free)
    }

    /// One length-delimited frame (varint length + message), the standard
    /// framing for a stream of messages.
    public func encodeDelimited() throws -> Data {
        guard let tree else {
            throw PbwireError(status: PBW_ERR_INVALID.code, message: "message is closed")
        }
        var bytes: UnsafeMutablePointer<UInt8>?
        var len = 0
        let status = pbw_encode_delimited(tree, &bytes, &len)
        guard status == PBW_OK else { try throwStatus(status) }
        guard len > 0, let bytes else {
            if let bytes { pbw_free(bytes) }
            return Data()
        }
        return Data(bytesNoCopy: bytes, count: len, deallocator: .free)
    }

    /// Decodes one length-delimited frame as a message of the same type.
    /// Returns the message plus the prefix + frame bytes consumed, so
    /// concatenated frames decode in a loop by advancing past them. A
    /// truncated frame throws; a declared length past `maxBytes` throws
    /// `PBW_ERR_LIMIT` before the body is decoded.
    public func decodeDelimited(_ data: Data, maxBytes: Int = 0, maxDepth: Int = 0) throws
        -> (message: Message, consumed: Int)
    {
        var decoded: OpaquePointer?
        var consumed = 0
        var limits = try makeLimits(maxBytes: maxBytes, maxDepth: maxDepth)
        let status = data.withUnsafeBytes { raw in
            pbw_decode_delimited_limits(
                descriptor, raw.baseAddress, data.count, &limits, &decoded, &consumed)
        }
        guard status == PBW_OK, let decoded else { try throwStatus(status) }
        return (
            Message(
                schema: schema, descriptor: descriptor, tree: decoded, owned: true,
                parent: nil), consumed)
    }

    /// Protobuf JSON mapping text of this message. Matches the reference
    /// printer: 64-bit integers print quoted, bytes print as standard
    /// base64, enums print by name, well-known types use their JSON form.
    public func toJSON() throws -> String {
        guard let tree else {
            throw PbwireError(status: PBW_ERR_INVALID.code, message: "message is closed")
        }
        var out: UnsafeMutablePointer<CChar>?
        var len = 0
        let status = pbw_json_print(tree, &out, &len)
        guard status == PBW_OK, let out else { try throwStatus(status) }
        defer { pbw_free(out) }
        guard let text = String(bytes: Data(bytes: out, count: len), encoding: .utf8) else {
            throw PbwireError(status: PBW_ERR_INVALID.code, message: "invalid UTF-8 in JSON output")
        }
        return text
    }

    /// Parses JSON mapping text into a message of the same type. Unknown
    /// field names throw unless `ignoreUnknown` is set; `maxBytes` /
    /// `maxDepth` bound the input, zeros meaning the defaults.
    public func fromJSON(
        _ text: String, ignoreUnknown: Bool = false, maxBytes: Int = 0, maxDepth: Int = 0
    ) throws -> Message {
        var parsed: OpaquePointer?
        var limits = try makeLimits(maxBytes: maxBytes, maxDepth: maxDepth)
        var options = pbw_json_parse_options(ignore_unknown_fields: ignoreUnknown ? 1 : 0)
        let bytes = Array(text.utf8)
        let status = bytes.withUnsafeBytes { raw in
            pbw_json_parse_limits(
                descriptor, raw.baseAddress?.assumingMemoryBound(to: CChar.self), bytes.count,
                &limits, &options, &parsed)
        }
        guard status == PBW_OK, let parsed else { try throwStatus(status) }
        return Message(
            schema: schema, descriptor: descriptor, tree: parsed, owned: true, parent: nil)
    }

    /// Protobuf text format of this message. Unknown fields print as
    /// numeric lines unless `printUnknown` is false.
    public func toText(printUnknown: Bool = true) throws -> String {
        guard let tree else {
            throw PbwireError(status: PBW_ERR_INVALID.code, message: "message is closed")
        }
        var out: UnsafeMutablePointer<CChar>?
        var len = 0
        let status = pbw_text_print_opts(tree, printUnknown ? 1 : 0, &out, &len)
        guard status == PBW_OK, let out else { try throwStatus(status) }
        defer { pbw_free(out) }
        guard let text = String(bytes: Data(bytes: out, count: len), encoding: .utf8) else {
            throw PbwireError(status: PBW_ERR_INVALID.code, message: "invalid UTF-8 in text output")
        }
        return text
    }

    /// Parses text format into a message of the same type. `maxBytes` /
    /// `maxDepth` bound the input, zeros meaning the defaults.
    public func fromText(_ text: String, maxBytes: Int = 0, maxDepth: Int = 0) throws -> Message {
        var parsed: OpaquePointer?
        var limits = try makeLimits(maxBytes: maxBytes, maxDepth: maxDepth)
        let bytes = Array(text.utf8)
        let status = bytes.withUnsafeBytes { raw in
            pbw_text_parse_limits(
                descriptor, raw.baseAddress?.assumingMemoryBound(to: CChar.self), bytes.count,
                &limits, &parsed)
        }
        guard status == PBW_OK, let parsed else { try throwStatus(status) }
        return Message(
            schema: schema, descriptor: descriptor, tree: parsed, owned: true, parent: nil)
    }

    // MARK: - internals

    private func read(_ value: OpaquePointer, as type: pbw_type, sub: OpaquePointer?) -> PbwireValue? {
        switch type {
        case PBW_TYPE_STRING:
            var len = 0
            guard let bytes = pbw_value_get_bytes(value, &len) else { return nil }
            let data = Data(bytes: bytes, count: len)
            guard let text = String(data: data, encoding: .utf8) else { return nil }
            return .string(text)
        case PBW_TYPE_BYTES:
            var len = 0
            guard let bytes = pbw_value_get_bytes(value, &len) else { return nil }
            return .bytes(Data(bytes: bytes, count: len))
        case PBW_TYPE_MESSAGE, PBW_TYPE_GROUP:
            guard let sub, let nested = pbw_value_get_message(value) else { return nil }
            return .message(
                Message(schema: schema, descriptor: sub, tree: nested, owned: false, parent: self))
        case PBW_TYPE_BOOL:
            return .bool(pbw_value_get_bool(value) != 0)
        case PBW_TYPE_FLOAT:
            return .float(pbw_value_get_float(value))
        case PBW_TYPE_DOUBLE:
            return .double(pbw_value_get_double(value))
        case PBW_TYPE_UINT32, PBW_TYPE_FIXED32:
            return .uint(UInt64(pbw_value_get_uint32(value)))
        case PBW_TYPE_UINT64, PBW_TYPE_FIXED64:
            return .uint(pbw_value_get_uint(value))
        case PBW_TYPE_INT64, PBW_TYPE_SINT64, PBW_TYPE_SFIXED64:
            return .int(pbw_value_get_int(value))
        default: // int32, sint32, sfixed32, enum
            return .int(Int64(pbw_value_get_int32(value)))
        }
    }

    private func store(_ value: PbwireValue?, _ name: String, allowRepeated: Bool) throws -> Message {
        guard let tree, let f = field(name) else {
            throw PbwireError(status: PBW_ERR_NOT_FOUND.code, message: "no such field: \(name)")
        }
        let number = pbw_field_number(f)
        guard let value else {
            pbw_message_clear(tree, number)
            return self
        }
        if pbw_field_label(f) == PBW_LABEL_REPEATED, !allowRepeated {
            throw PbwireError(
                status: PBW_ERR_INVALID.code,
                message: "repeated field (use add): \(name)")
        }
        guard let stored = try build(value, for: f, name: name) else {
            throw PbwireError(status: PBW_ERR_NOMEM.code, message: "out of memory: \(name)")
        }
        let status = pbw_message_set(tree, number, stored)
        guard status == PBW_OK else {
            // Ownership only moves on success (pbw_message_set contract).
            pbw_value_free(stored)
            try throwStatus(status, name)
        }
        return self
    }

    private func build(_ value: PbwireValue, for f: OpaquePointer, name: String) throws -> OpaquePointer? {
        let type = pbw_field_type(f)
        switch (type, value) {
        case (PBW_TYPE_STRING, .string(let text)):
            return text.withCString { pbw_value_new_string($0) }
        case (PBW_TYPE_BYTES, .bytes(let data)):
            // Empty Data has no base address; the empty literal keeps the
            // pointer valid for the zero-length copy.
            if data.isEmpty { return pbw_value_new_bytes("", 0) }
            return data.withUnsafeBytes { raw in
                pbw_value_new_bytes(raw.baseAddress, data.count)
            }
        case (PBW_TYPE_BOOL, .bool(let b)):
            return pbw_value_new_bool(b ? 1 : 0)
        case (PBW_TYPE_FLOAT, .float(let v)):
            return pbw_value_new_float(v)
        case (PBW_TYPE_FLOAT, .double(let v)):
            return pbw_value_new_float(Float(v))
        case (PBW_TYPE_FLOAT, .int(let v)):
            return pbw_value_new_float(Float(v))
        case (PBW_TYPE_FLOAT, .uint(let v)):
            return pbw_value_new_float(Float(v))
        case (PBW_TYPE_DOUBLE, .double(let v)):
            return pbw_value_new_double(v)
        case (PBW_TYPE_DOUBLE, .float(let v)):
            return pbw_value_new_double(Double(v))
        case (PBW_TYPE_DOUBLE, .int(let v)):
            return pbw_value_new_double(Double(v))
        case (PBW_TYPE_DOUBLE, .uint(let v)):
            return pbw_value_new_double(Double(v))
        case (_, .int(let v)) where isInteger(type):
            guard isSigned(type) else { return try unsignedBits(v, type, name) }
            if is32(type), v < Int64(Int32.min) || v > Int64(Int32.max) {
                throw PbwireError(
                    status: PBW_ERR_TYPE.code, message: "32-bit field out of range: \(name)")
            }
            // Signed types are stored sign extended, like the C ABI documents.
            return pbw_value_new_uint(UInt64(bitPattern: v))
        case (_, .uint(let v)) where isInteger(type):
            guard !isSigned(type) else {
                guard v <= UInt64(Int64.max) else {
                    throw PbwireError(
                        status: PBW_ERR_TYPE.code, message: "out of range for \(name)")
                }
                return pbw_value_new_uint(v)
            }
            if is32(type), v > UInt64(UInt32.max) {
                throw PbwireError(
                    status: PBW_ERR_TYPE.code, message: "32-bit field out of range: \(name)")
            }
            return pbw_value_new_uint(v)
        default:
            throw PbwireError(status: PBW_ERR_TYPE.code, message: "wrong value for \(name)")
        }
    }

    private func unsignedBits(_ v: Int64, _ type: pbw_type, _ name: String) throws -> OpaquePointer? {
        // An .int into an unsigned field: accept in-range values, like the
        // Objective-C wrapper's longLongValue/unsignedLongLongValue pair.
        if is32(type) {
            guard v >= 0, v <= Int64(UInt32.max) else {
                throw PbwireError(
                    status: PBW_ERR_TYPE.code, message: "32-bit field out of range: \(name)")
            }
        } else if v < 0 {
            throw PbwireError(status: PBW_ERR_TYPE.code, message: "negative for \(name)")
        }
        return pbw_value_new_uint(UInt64(bitPattern: v))
    }

    private func isInteger(_ type: pbw_type) -> Bool {
        switch type {
        case PBW_TYPE_INT32, PBW_TYPE_INT64, PBW_TYPE_UINT32, PBW_TYPE_UINT64,
            PBW_TYPE_SINT32, PBW_TYPE_SINT64, PBW_TYPE_FIXED32, PBW_TYPE_FIXED64,
            PBW_TYPE_SFIXED32, PBW_TYPE_SFIXED64, PBW_TYPE_ENUM:
            return true
        default:
            return false
        }
    }

    private func isSigned(_ type: pbw_type) -> Bool {
        switch type {
        case PBW_TYPE_INT32, PBW_TYPE_INT64, PBW_TYPE_SINT32, PBW_TYPE_SINT64,
            PBW_TYPE_SFIXED32, PBW_TYPE_SFIXED64, PBW_TYPE_ENUM:
            return true
        default:
            return false
        }
    }

    private func is32(_ type: pbw_type) -> Bool {
        switch type {
        case PBW_TYPE_INT32, PBW_TYPE_UINT32, PBW_TYPE_SINT32, PBW_TYPE_FIXED32,
            PBW_TYPE_SFIXED32, PBW_TYPE_ENUM, PBW_TYPE_FLOAT:
            return true
        default:
            return false
        }
    }
}

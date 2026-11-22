// The dynamic message tree: one slot per declared field, plus the oneof cases
// and the retained unknown fields.
//
// Scalars live in their wire-neutral form (`Int32`/`UInt32`/`Int64`/`UInt64` for
// every integer width, so nothing is truncated to what the host could hold; raw
// bit patterns for `float`/`double` so a NaN payload survives a round trip;
// `Bool`; a byte array for both `string` and `bytes`) and messages live nested.
// A decoded string stays the exact bytes that came off the wire, so a proto2
// string with invalid UTF-8 re-encodes byte for byte.
//
// This is the Swift twin of `bindings/rust/src/core/tree.rs`. Unlike the
// interpreter cores it owns no slice of the input: strings and unknown fields
// are copied out at parse (Swift has no borrowed-slice value that can outlive
// its buffer), which keeps `Message` free of a lifetime.
//
// `Value` is a Swift enum with payloads, so it is not `Sendable`: a message
// tree is a single-threaded working object (the schema is the shared part).

/// One field value in its storage form.
public enum Value {
    case bool(Bool)
    case i32(Int32)
    case u32(UInt32)
    case i64(Int64)
    case u64(UInt64)
    /// float32 bits, kept raw so a NaN payload survives.
    case f32(UInt32)
    /// float64 bits, kept raw so a NaN payload survives.
    case f64(UInt64)
    /// A `string` or `bytes` payload, exactly as parsed (or as set).
    case bytes([UInt8])
    case message(Message)
    case list([Value])

    public var asI64: Int64? {
        switch self {
        case .i32(let v): return Int64(v)
        case .u32(let v): return Int64(v)
        case .i64(let v): return v
        case .u64(let v): return Int64(bitPattern: v)
        case .bool(let v): return v ? 1 : 0
        case .f32(let bits): return Int64(Int32(bitPattern: bits))
        case .f64(let bits): return Int64(bitPattern: bits)
        default: return nil
        }
    }

    public var asU64: UInt64? {
        switch self {
        case .i32(let v): return UInt64(bitPattern: Int64(v))
        case .u32(let v): return UInt64(v)
        case .i64(let v): return UInt64(bitPattern: v)
        case .u64(let v): return v
        case .bool(let v): return v ? 1 : 0
        case .f32(let bits): return UInt64(bits)
        case .f64(let bits): return bits
        default: return nil
        }
    }

    public var asBool: Bool? {
        switch self {
        case .bool(let v): return v
        case .i32(let v): return v != 0
        case .u32(let v): return v != 0
        case .i64(let v): return v != 0
        case .u64(let v): return v != 0
        default: return nil
        }
    }

    public var asBytes: [UInt8]? {
        if case .bytes(let b) = self { return b }
        return nil
    }

    public var asMessage: Message? {
        if case .message(let m) = self { return m }
        return nil
    }

    public var asList: [Value]? {
        if case .list(let items) = self { return items }
        return nil
    }
}

/// One message value: per-slot values, the oneof cases and the unknowns.
public final class Message {
    public let schema: Schema
    /// Index of this message's type in `schema.messages`.
    public let def: Int
    var values: [Value?]
    /// Slot+1 per oneof, 0 = none set; empty when the message has none.
    var oneofCase: [UInt32]
    /// Retained unknown fields in input order (tag and payload verbatim).
    var unknownFields: [[UInt8]]

    public init(schema: Schema, def: Int) {
        let d = schema.messages[def]
        self.schema = schema
        self.def = def
        self.values = [Value?](repeating: nil, count: d.fields.count)
        self.oneofCase = d.oneofCount > 0 ? [UInt32](repeating: 0, count: d.oneofCount) : []
        self.unknownFields = []
    }

    public var fullName: String { schema.messages[def].fullName }

    public var msgDef: MsgDef { schema.messages[def] }

    /// Slot index of a field name.
    public func fieldSlot(_ name: String) throws -> Int {
        guard let slot = schema.messages[def].byName[name] else {
            throw CoreError("\(fullName) has no field \(name)")
        }
        return slot
    }

    public func field(_ name: String) throws -> Field {
        schema.messages[def].fields[try fieldSlot(name)]
    }

    public func has(_ name: String) throws -> Bool {
        values[try fieldSlot(name)] != nil
    }

    /// The raw slot value, which is a `.list` for a repeated field.
    public func get(_ name: String) throws -> Value? {
        values[try fieldSlot(name)]
    }

    /// Which member of a oneof is set: its field number, or 0 for none.
    public func oneofCase(_ oneofName: String) throws -> UInt32 {
        guard let idx = schema.messages[def].oneofByName[oneofName] else {
            throw CoreError("\(fullName) has no oneof \(oneofName)")
        }
        let c = idx < oneofCase.count ? oneofCase[idx] : 0
        return c == 0 ? 0 : schema.messages[def].fields[Int(c) - 1].number
    }

    func setSlot(_ slot: Int, _ value: Value) {
        let oneof = schema.messages[def].fields[slot].oneof
        if oneof >= 0 && !oneofCase.isEmpty {
            let prev = oneofCase[Int(oneof)]
            if prev != 0 && prev != UInt32(slot) + 1 {
                values[Int(prev) - 1] = nil
            }
            oneofCase[Int(oneof)] = UInt32(slot) + 1
        }
        values[slot] = value
    }

    /// Stores a value, converting it to the field's storage form.
    public func set(_ name: String, _ value: Value) throws {
        let slot = try fieldSlot(name)
        setSlot(slot, coerce(schema.messages[def].fields[slot], value))
    }

    /// Appends one element of a repeated field.
    public func push(_ name: String, _ value: Value) throws {
        let slot = try fieldSlot(name)
        let f = schema.messages[def].fields[slot]
        let value = coerce(f, value)
        if case .list(var items)? = values[slot] {
            items.append(value)
            values[slot] = .list(items)
            return
        }
        setSlot(slot, .list([value]))
    }

    public func setI64(_ name: String, _ v: Int64) throws { try set(name, .i64(v)) }
    public func setU64(_ name: String, _ v: UInt64) throws { try set(name, .u64(v)) }
    public func setBool(_ name: String, _ v: Bool) throws { try set(name, .bool(v)) }
    public func setF64(_ name: String, _ v: Double) throws { try set(name, .f64(v.bitPattern)) }
    public func setF32(_ name: String, _ v: Float) throws { try set(name, .f32(v.bitPattern)) }
    public func setStr(_ name: String, _ v: String) throws { try set(name, .bytes(Array(v.utf8))) }
    public func setBytes(_ name: String, _ v: [UInt8]) throws { try set(name, .bytes(v)) }

    /// Replaces (or creates) a sub-message field and returns it for filling.
    @discardableResult
    public func setMessage(_ name: String) throws -> Message {
        let slot = try fieldSlot(name)
        guard let msgType = schema.messages[def].fields[slot].msgType else {
            throw CoreError("\(name) is not a message field")
        }
        let sub = Message(schema: schema, def: msgType)
        setSlot(slot, .message(sub))
        return sub
    }

    /// Creates a new element of a repeated message field and returns it.
    @discardableResult
    public func pushMessage(_ name: String) throws -> Message {
        let slot = try fieldSlot(name)
        guard let msgType = schema.messages[def].fields[slot].msgType else {
            throw CoreError("\(name) is not a message field")
        }
        let sub = Message(schema: schema, def: msgType)
        push(slot, .message(sub))
        return sub
    }

    func push(_ slot: Int, _ value: Value) {
        if case .list(var items)? = values[slot] {
            items.append(value)
            values[slot] = .list(items)
            return
        }
        setSlot(slot, .list([value]))
    }

    public func getI64(_ name: String) throws -> Int64? { try get(name)?.asI64 }
    public func getU64(_ name: String) throws -> UInt64? { try get(name)?.asU64 }
    public func getBool(_ name: String) throws -> Bool? { try get(name)?.asBool }

    public func getF64(_ name: String) throws -> Double? {
        switch try get(name) {
        case .f64(let bits)?: return Double(bitPattern: bits)
        case .f32(let bits)?: return Double(Float(bitPattern: bits))
        default: return nil
        }
    }

    public func getF32(_ name: String) throws -> Float? {
        switch try get(name) {
        case .f32(let bits)?: return Float(bitPattern: bits)
        case .f64(let bits)?: return Float(Double(bitPattern: bits))
        default: return nil
        }
    }

    public func getBytes(_ name: String) throws -> [UInt8]? { try get(name)?.asBytes }

    /// A string field's bytes as UTF-8 (lossy: proto2 strings may be invalid).
    public func getStr(_ name: String) throws -> String? {
        guard let b = try getBytes(name) else { return nil }
        return String(decoding: b, as: UTF8.self)
    }

    public func getMessage(_ name: String) throws -> Message? { try get(name)?.asMessage }

    /// Number of elements in a repeated field.
    public func count(_ name: String) throws -> Int {
        if case .list(let items)? = try get(name) { return items.count }
        return 0
    }

    public func isEmpty(_ name: String) throws -> Bool { try count(name) == 0 }

    /// The elements of a repeated field.
    public func items(_ name: String) throws -> [Value] {
        if case .list(let items)? = try get(name) { return items }
        return []
    }

    /// The retained unknown field images, in input order.
    public var unknown: [[UInt8]] { unknownFields }
}

/// One public value in the storage form of its field (see ``Kind``).
private func coerce(_ f: Field, _ v: Value) -> Value {
    switch f.kind {
    case .bool:
        return .bool(v.asI64.map { $0 != 0 } ?? false)
    case .int32, .sint32, .sfixed32, .enumeration:
        return .i32(Int32(truncatingIfNeeded: v.asU64 ?? 0))
    case .uint32, .fixed32:
        return .u32(UInt32(truncatingIfNeeded: v.asU64 ?? 0))
    case .int64, .sint64, .sfixed64:
        return .i64(Int64(bitPattern: v.asU64 ?? 0))
    case .uint64, .fixed64:
        return .u64(v.asU64 ?? 0)
    case .float:
        switch v {
        case .f32(let bits): return .f32(bits)
        case .f64(let bits): return .f32(Float(Double(bitPattern: bits)).bitPattern)
        default: return .f32(Float(v.asI64 ?? 0).bitPattern)
        }
    case .double:
        switch v {
        case .f64(let bits): return .f64(bits)
        case .f32(let bits): return .f64(Double(Float(bitPattern: bits)).bitPattern)
        default: return .f64(Double(v.asI64 ?? 0).bitPattern)
        }
    case .string, .bytes:
        return .bytes(v.asBytes ?? [])
    case .message, .group:
        return v
    }
}

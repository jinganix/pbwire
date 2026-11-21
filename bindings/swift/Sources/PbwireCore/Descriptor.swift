// `FileDescriptorSet` reader and schema builder for the pure-Swift core: the
// only reader of `descriptor.proto` the pure path needs. It parses just enough
// for a binary codec (names, numbers, labels, types, type names, oneofs,
// extensions, packedness, proto3 optional, editions features, syntax) and skips
// everything else by wire type, so unknown `descriptor.proto` additions never
// break loading.
//
// The wire layout knowledge lives in `Wire.swift`; `Encode.swift`/`Decode.swift`
// know the message wire layout and `PbwireCore.swift` is the public facade.
// This mirrors `bindings/rust/src/core/descriptor.rs` (itself the twin of
// `bindings/python/pure/descriptor.py` and `bindings/java/runtime/Proto.java`).

// ---------------------------------------------------------------- wire kinds

/// Wire kinds of the table engine (its own numbering, not protobuf's): every
/// packable kind sorts before `double`.
public enum Kind: UInt8, Sendable {
    case int32 = 0
    case uint32 = 1
    case sint32 = 2
    case int64 = 3
    case uint64 = 4
    case sint64 = 5
    case bool = 6
    case enumeration = 7
    case fixed32 = 8
    case sfixed32 = 9
    case float = 10
    case fixed64 = 11
    case sfixed64 = 12
    case double = 13
    case string = 14
    case bytes = 15
    case message = 16
    case group = 17

    /// The packable numeric kinds: packed runs cross every one of them.
    public var isPackable: Bool { rawValue <= Kind.double.rawValue }

    public var isRef: Bool {
        self == .string || self == .bytes || self == .message || self == .group
    }
}

/// Presence kinds. `implicit` is a proto3 plain scalar (no has-bit).
public enum Presence: Sendable {
    case implicit
    case explicit
    case required
    case messagePresence
}

// ----------------------------------------------------------- compiled schema

public struct EnumDef: Sendable {
    public var fullName = ""
    public var byName: [String: Int32] = [:]
    public var byNumber: [Int32: String] = [:]
    /// proto2 (and editions CLOSED) enums are closed: an unknown number is an
    /// unknown field instead of a kept value.
    public var closed = false

    public init() {}
}

public struct Field: Sendable {
    public var number: UInt32 = 0
    public var name = ""
    public var kind: Kind = .int32
    /// Wire type of the declared form: LEN for packed numerics.
    public var wt: UInt8 = 0
    /// Wire type of a single element (what an unpacked occurrence uses).
    public var elemWt: UInt8 = 0
    public var repeated = false
    public var packed = false
    /// Oneof index, -1 when the field is not in a oneof.
    public var oneof: Int32 = -1
    public var presence: Presence = .implicit
    public var msgType: Int? = nil
    public var enumType: Int? = nil
    public var enumClosed = false
    /// A decoded string must be valid UTF-8 (proto3/editions VERIFY).
    public var utf8 = false
    /// Extensions only: the global name `[pkg.name]`.
    public var fullName: String? = nil
    /// Precomputed tag bytes (the start-group tag for a group).
    public var tag = Tag(fieldNumber: 0, wireType: 0)
    /// Groups only: the end-group tag.
    public var tagEnd = Tag.empty

    public init() {}
}

/// A message's compiled table. A class on purpose: the codec reads
/// `schema.messages[i]` on every message and field, and a struct copy here
/// retains two dictionaries and three arrays each time.
public final class MsgDef: @unchecked Sendable {
    public var fullName = ""
    public var fields: [Field] = []
    /// Dense number-to-slot map, `nil` when field numbers are sparse.
    public var slotByFnum: [Int32]? = nil
    public var byName: [String: Int] = [:]
    public var oneofByName: [String: Int] = [:]
    public var oneofCount = 0
    public var hasRequired = false
    /// Some transitively reachable message has a REQUIRED field.
    public var deepRequired = false
    /// protoc's synthesized map entry (field 1 key, field 2 value).
    public var mapEntry = false
    public var reservedNames: [String] = []

    public init() {}

    /// Slot of a field number, -1 when the message has no such field.
    public func slot(of fnum: UInt32) -> Int32 {
        if let slots = slotByFnum {
            let i = Int(fnum)
            return i < slots.count ? slots[i] : -1
        }
        for (i, f) in fields.enumerated() where f.number == fnum { return Int32(i) }
        return -1
    }
}

// ------------------------------------------------- descriptor.proto model

private let tDouble: Int32 = 1
private let tFloat: Int32 = 2
private let tInt64: Int32 = 3
private let tUint64: Int32 = 4
private let tInt32: Int32 = 5
private let tFixed64: Int32 = 6
private let tFixed32: Int32 = 7
private let tBool: Int32 = 8
private let tString: Int32 = 9
private let tGroup: Int32 = 10
private let tMessage: Int32 = 11
private let tBytes: Int32 = 12
private let tUint32: Int32 = 13
private let tEnum: Int32 = 14
private let tSfixed32: Int32 = 15
private let tSfixed64: Int32 = 16
private let tSint32: Int32 = 17
private let tSint64: Int32 = 18

private let lOptional: Int32 = 1
private let lRequired: Int32 = 2
private let lRepeated: Int32 = 3

private struct POneof {
    var name = ""
}

private struct PField {
    var name: String? = nil
    var extendee: String? = nil
    var number: Int32 = 0
    var label: Int32 = lOptional
    var type: Int32 = 0
    var typeName: String? = nil
    var oneofIndex: Int32 = -1
    var proto3Optional = false
    var packed = false
    var hasPackedOption = false
    /// Editions `FeatureSet.enum_type`: 0 unset, 1 OPEN, 2 CLOSED.
    var enumOpen: Int32 = 0
    /// Editions `FeatureSet.utf8_validation`: 0 unset, 2 VERIFY, 3 NONE.
    var utf8: Int32 = 0
    /// Linked: the parsed message/enum index.
    var msgType: Int? = nil
    var enumType: Int? = nil
    var enumClosed = false
    /// Linked: the declared oneof slot.
    var oneof: Int? = nil
    var hasPresence = false
    var fullName: String? = nil
}

private struct PEnumValue {
    var name: String? = nil
    var number: Int32 = 0
}

private struct PEnum {
    var name: String? = nil
    var fullName = ""
    var idx = -1
    var values: [PEnumValue] = []
    var enumOpen: Int32 = 0
    var closed = false
}

private struct PMessage {
    var name: String? = nil
    var fullName = ""
    var idx = -1
    var mapEntry = false
    var fields: [PField] = []
    /// Indices into the flat message array.
    var nested: [Int] = []
    var extensions: [PField] = []
    var oneofs: [POneof] = []
    /// Indices into the flat enum array.
    var enums: [Int] = []
    var reservedNames: [String] = []
}

private struct PFile {
    var name: String? = nil
    var pkg = ""
    var syntax = ""
    /// Field 14: an editions file (behaves as proto3 here).
    var edition = false
    var enumOpen: Int32 = 0
    var utf8: Int32 = 0
    var messages: [Int] = []
    var extensions: [PField] = []
    var enums: [Int] = []
}

// -------------------------------------------------------------- wire parsing

private func strOf(_ b: [UInt8], _ off: Int, _ n: Int) -> String {
    String(decoding: b[off..<off + n], as: UTF8.self)
}

private func asInt32(_ v: UInt32) -> Int32 {
    Int32(bitPattern: v)
}

private func parseSet(_ bytes: [UInt8]) throws -> ([PFile], [PMessage], [PEnum]) {
    var files: [PFile] = []
    var messages: [PMessage] = []
    var enums: [PEnum] = []
    var r = Reader(bytes)
    while r.pos < r.end {
        let tag = try r.readVar32()
        if tag >> 3 == 1 && UInt8(tag & 7) == wtLen {
            let n = try r.readLen()
            files.append(try parseFile(r.b, r.pos, n, &messages, &enums))
            r.pos += n
        } else {
            try r.skip(UInt8(tag & 7))
        }
    }
    return (files, messages, enums)
}

private func parseFile(
    _ b: [UInt8], _ off: Int, _ len: Int, _ messages: inout [PMessage], _ enums: inout [PEnum]
) throws -> PFile {
    var r = try Reader(b, from: off, to: off + len)
    var f = PFile()
    while r.pos < r.end {
        let tag = try r.readVar32()
        let num = tag >> 3
        let wt = UInt8(tag & 7)
        if wt == wtVarint {
            // Field 14 is the edition marker; its value is irrelevant here.
            if num == 14 {
                _ = try r.readVar32()
                f.edition = true
            } else {
                try r.skip(wt)
            }
            continue
        }
        if wt == wtLen {
            let n = try r.readLen()
            switch num {
            case 4: f.messages.append(try parseMessage(b, r.pos, n, 1, &messages, &enums))
            case 5: f.enums.append(try parseEnum(b, r.pos, n, &enums))
            case 7: f.extensions.append(try parseField(b, r.pos, n))
            case 8:
                f.enumOpen = try parseFeatures(b, r.pos, r.pos + n, 50)
                f.utf8 = try parseFeaturesUtf8(b, r.pos, r.pos + n, 50)
            case 1: f.name = strOf(b, r.pos, n)
            case 2: f.pkg = strOf(b, r.pos, n)
            case 12: f.syntax = strOf(b, r.pos, n)
            default: break
            }
            r.pos += n
        } else {
            try r.skip(wt)
        }
    }
    return f
}

private func parseMessage(
    _ b: [UInt8], _ off: Int, _ len: Int, _ depth: UInt32, _ messages: inout [PMessage],
    _ enums: inout [PEnum]
) throws -> Int {
    // Descriptor nesting is peer data too: a FileDescriptorSet a few tens of KB
    // can nest thousands of DescriptorProtos, so cap it instead of letting the
    // recursion overflow the stack.
    if depth > maxDepth {
        throw CoreError("descriptor nesting exceeds \(maxDepth)")
    }
    var r = try Reader(b, from: off, to: off + len)
    var m = PMessage()
    let pos = messages.count
    messages.append(m)  // reserve the index before parsing children
    while r.pos < r.end {
        let tag = try r.readVar32()
        let num = tag >> 3
        if UInt8(tag & 7) != wtLen {
            try r.skip(UInt8(tag & 7))
            continue
        }
        let n = try r.readLen()
        switch num {
        case 1: m.name = strOf(b, r.pos, n)
        case 2: m.fields.append(try parseField(b, r.pos, n))
        case 3: m.nested.append(try parseMessage(b, r.pos, n, depth + 1, &messages, &enums))
        case 4: m.enums.append(try parseEnum(b, r.pos, n, &enums))
        case 6: m.extensions.append(try parseField(b, r.pos, n))
        case 7: m.mapEntry = try parseMapEntry(b, r.pos, n)
        case 8: m.oneofs.append(try parseOneof(b, r.pos, n))
        case 10: m.reservedNames.append(strOf(b, r.pos, n))
        default: break
        }
        r.pos += n
    }
    if m.name == nil {
        throw CoreError("message without a name")
    }
    messages[pos] = m
    return pos
}

private func parseOneof(_ b: [UInt8], _ off: Int, _ len: Int) throws -> POneof {
    var r = try Reader(b, from: off, to: off + len)
    var o = POneof()
    while r.pos < r.end {
        let tag = try r.readVar32()
        if tag >> 3 == 1 && UInt8(tag & 7) == wtLen {
            let n = try r.readLen()
            o.name = strOf(b, r.pos, n)
            r.pos += n
        } else {
            try r.skip(UInt8(tag & 7))
        }
    }
    return o
}

private func parseEnum(
    _ b: [UInt8], _ off: Int, _ len: Int, _ enums: inout [PEnum]
) throws -> Int {
    var r = try Reader(b, from: off, to: off + len)
    var e = PEnum()
    let pos = enums.count
    enums.append(e)
    while r.pos < r.end {
        let tag = try r.readVar32()
        let num = tag >> 3
        if UInt8(tag & 7) != wtLen {
            try r.skip(UInt8(tag & 7))
            continue
        }
        let n = try r.readLen()
        switch num {
        case 1: e.name = strOf(b, r.pos, n)
        case 2: e.values.append(try parseEnumValue(b, r.pos, n))
        case 3: e.enumOpen = try parseFeatures(b, r.pos, r.pos + n, 7)
        default: break
        }
        r.pos += n
    }
    enums[pos] = e
    return pos
}

private func parseEnumValue(_ b: [UInt8], _ off: Int, _ len: Int) throws -> PEnumValue {
    var r = try Reader(b, from: off, to: off + len)
    var v = PEnumValue()
    var hasNumber = false
    while r.pos < r.end {
        let tag = try r.readVar32()
        let num = tag >> 3
        let wt = UInt8(tag & 7)
        if wt == wtVarint {
            if num == 2 {
                v.number = asInt32(try r.readVar32())
                hasNumber = true
            } else {
                try r.skip(wt)
            }
        } else if wt == wtLen {
            let n = try r.readLen()
            if num == 1 { v.name = strOf(b, r.pos, n) }
            r.pos += n
        } else {
            try r.skip(wt)
        }
    }
    if v.name == nil || !hasNumber {
        throw CoreError("enum value without name/number")
    }
    return v
}

/// `MessageOptions.map_entry` is field 7.
private func parseMapEntry(_ b: [UInt8], _ off: Int, _ len: Int) throws -> Bool {
    var r = try Reader(b, from: off, to: off + len)
    var mapEntry = false
    while r.pos < r.end {
        let tag = try r.readVar32()
        if UInt8(tag & 7) != wtVarint {
            try r.skip(UInt8(tag & 7))
            continue
        }
        if tag >> 3 == 7 {
            mapEntry = try r.readVar32() != 0
        } else {
            try r.skip(wtVarint)
        }
    }
    return mapEntry
}

/// Reads one Options message and returns `FeatureSet.enum_type` (1 OPEN,
/// 2 CLOSED) when it carries one; 0 means unset.
private func parseFeatures(_ b: [UInt8], _ off: Int, _ end: Int, _ optionNumber: UInt32) throws
    -> Int32
{
    var r = try Reader(b, from: off, to: end)
    var open: Int32 = 0
    while r.pos < r.end {
        let tag = try r.readVar32()
        let num = tag >> 3
        if UInt8(tag & 7) != wtLen {
            try r.skip(UInt8(tag & 7))
            continue
        }
        let n = try r.readLen()
        if num == optionNumber {
            open = try parseFeatureSetEnumType(b, r.pos, r.pos + n)
        }
        r.pos += n
    }
    return open
}

/// `FileOptions.features` (50) -> `FeatureSet.utf8_validation`.
private func parseFeaturesUtf8(
    _ b: [UInt8], _ off: Int, _ end: Int, _ optionNumber: UInt32
) throws -> Int32 {
    var r = try Reader(b, from: off, to: end)
    var utf8: Int32 = 0
    while r.pos < r.end {
        let tag = try r.readVar32()
        let num = tag >> 3
        if UInt8(tag & 7) != wtLen {
            try r.skip(UInt8(tag & 7))
            continue
        }
        let n = try r.readLen()
        if num == optionNumber {
            utf8 = try parseFeatureSetUtf8(b, r.pos, r.pos + n)
        }
        r.pos += n
    }
    return utf8
}

/// `FeatureSet.enum_type` = 2: 1 OPEN or 2 CLOSED.
private func parseFeatureSetEnumType(_ b: [UInt8], _ off: Int, _ end: Int) throws -> Int32 {
    var r = try Reader(b, from: off, to: end)
    var open: Int32 = 0
    while r.pos < r.end {
        let tag = try r.readVar32()
        if tag >> 3 == 2 && UInt8(tag & 7) == wtVarint {
            let v = asInt32(try r.readVar32())
            if v == 1 || v == 2 { open = v }
        } else {
            try r.skip(UInt8(tag & 7))
        }
    }
    return open
}

/// `FeatureSet.utf8_validation` = 4: 2 VERIFY or 3 NONE.
private func parseFeatureSetUtf8(_ b: [UInt8], _ off: Int, _ end: Int) throws -> Int32 {
    var r = try Reader(b, from: off, to: end)
    var utf8: Int32 = 0
    while r.pos < r.end {
        let tag = try r.readVar32()
        if tag >> 3 == 4 && UInt8(tag & 7) == wtVarint {
            let v = asInt32(try r.readVar32())
            if v == 2 || v == 3 { utf8 = v }
        } else {
            try r.skip(UInt8(tag & 7))
        }
    }
    return utf8
}

private func parseField(_ b: [UInt8], _ off: Int, _ len: Int) throws -> PField {
    var r = try Reader(b, from: off, to: off + len)
    var f = PField()
    while r.pos < r.end {
        let tag = try r.readVar32()
        let num = tag >> 3
        let wt = UInt8(tag & 7)
        if wt == wtVarint {
            let v = try r.readVar32()
            switch num {
            case 3: f.number = asInt32(v)
            case 4: f.label = asInt32(v)
            case 5: f.type = asInt32(v)
            case 9: f.oneofIndex = asInt32(v)
            case 17: f.proto3Optional = v != 0
            default: break
            }
        } else if wt == wtLen {
            let n = try r.readLen()
            switch num {
            case 1: f.name = strOf(b, r.pos, n)
            case 2: f.extendee = strOf(b, r.pos, n)
            case 6: f.typeName = strOf(b, r.pos, n)
            case 8: try parseFieldOptions(b, r.pos, r.pos + n, &f)
            default: break
            }
            r.pos += n
        } else {
            try r.skip(wt)
        }
    }
    if f.name == nil || f.type == 0 || f.number == 0 {
        throw CoreError("field without name/type/number")
    }
    if f.type < tDouble || f.type > tSint64 {
        throw CoreError("bad field type \(f.type)")
    }
    if f.label < lOptional || f.label > lRepeated {
        throw CoreError("bad label \(f.label)")
    }
    return f
}

/// `FieldOptions`: packed (2) and the editions FeatureSet (21).
private func parseFieldOptions(_ b: [UInt8], _ off: Int, _ end: Int, _ f: inout PField) throws {
    var r = try Reader(b, from: off, to: end)
    while r.pos < r.end {
        let tag = try r.readVar32()
        let num = tag >> 3
        if num == 2 && UInt8(tag & 7) == wtVarint {
            f.packed = try r.readVar32() != 0
            f.hasPackedOption = true
        } else if num == 21 && UInt8(tag & 7) == wtLen {
            let n = try r.readLen()
            f.enumOpen = try parseFeatureSetEnumType(b, r.pos, r.pos + n)
            f.utf8 = try parseFeatureSetUtf8(b, r.pos, r.pos + n)
            r.pos += n
        } else {
            try r.skip(UInt8(tag & 7))
        }
    }
}

// ---------------------------------------------------------------- linking

private final class Linker {
    /// full name -> message index
    var messageByName: [String: Int] = [:]
    /// full name -> enum index
    var enumByName: [String: Int] = [:]
    /// per enum index: closed?
    var enumClosed: [Bool] = []
    var nextMessage = 0
    var nextEnum = 0
}

private func indexMessage(
    _ messages: inout [PMessage], _ pos: Int, _ scope: String, _ l: Linker,
    _ enums: inout [PEnum]
) throws {
    let name = messages[pos].name ?? ""
    let full = scope.isEmpty ? name : "\(scope).\(name)"
    messages[pos].fullName = full
    if l.messageByName[full] != nil {
        throw CoreError("duplicate message \(full)")
    }
    let idx = l.nextMessage
    l.nextMessage += 1
    messages[pos].idx = idx
    l.messageByName[full] = idx
    let own = messages[pos].enums
    for e in own { try indexEnum(&enums, e, full, l) }
    let nested = messages[pos].nested
    for n in nested { try indexMessage(&messages, n, full, l, &enums) }
}

private func indexEnum(_ enums: inout [PEnum], _ pos: Int, _ scope: String, _ l: Linker) throws {
    let name = enums[pos].name ?? ""
    let full = scope.isEmpty ? name : "\(scope).\(name)"
    enums[pos].fullName = full
    if l.enumByName[full] != nil {
        throw CoreError("duplicate enum \(full)")
    }
    enums[pos].idx = l.nextEnum
    l.nextEnum += 1
    l.enumClosed.append(false)
    l.enumByName[full] = enums[pos].idx
}

/// An enum is closed for proto2 files (and editions files asking for it).
private func linkEnum(_ enums: inout [PEnum], _ pos: Int, _ proto3: Bool, _ fileEnumOpen: Int32,
                      _ l: Linker) throws
{
    if enums[pos].name == nil {
        throw CoreError("enum without a name")
    }
    let closed: Bool
    if enums[pos].enumOpen != 0 {
        closed = enums[pos].enumOpen == 2
    } else if fileEnumOpen != 0 {
        closed = fileEnumOpen == 2
    } else {
        closed = !proto3
    }
    enums[pos].closed = closed
    l.enumClosed[enums[pos].idx] = closed
}

private func linkEnums(_ messages: inout [PMessage], _ pos: Int, _ proto3: Bool,
                       _ fileEnumOpen: Int32, _ l: Linker, _ enums: inout [PEnum]) throws
{
    if messages[pos].fullName.isEmpty {
        throw CoreError("message without a name")
    }
    let own = messages[pos].enums
    for e in own { try linkEnum(&enums, e, proto3, fileEnumOpen, l) }
    let nested = messages[pos].nested
    for n in nested { try linkEnums(&messages, n, proto3, fileEnumOpen, l, &enums) }
}

private func linkMessage(
    _ messages: inout [PMessage], _ pos: Int, _ scope: String, _ proto3: Bool, _ fileUtf8: Int32,
    _ l: Linker
) throws {
    let full = messages[pos].fullName
    let oneofNames = messages[pos].oneofs.map(\.name)
    let fieldCount = messages[pos].fields.count
    for i in 0..<fieldCount {
        try linkField(&messages[pos].fields[i], scope, proto3, fileUtf8, oneofNames, l)
    }
    let nested = messages[pos].nested
    for n in nested { try linkMessage(&messages, n, full, proto3, fileUtf8, l) }
    let extCount = messages[pos].extensions.count
    for i in 0..<extCount {
        try linkField(&messages[pos].extensions[i], scope, proto3, fileUtf8, nil, l)
        let name = messages[pos].extensions[i].name ?? ""
        messages[pos].extensions[i].fullName = scope.isEmpty ? name : "\(scope).\(name)"
    }
}

private func linkField(
    _ f: inout PField, _ scope: String, _ proto3: Bool, _ fileUtf8: Int32,
    _ parentOneofs: [String]?, _ l: Linker
) throws {
    // Extensions are never in a oneof (the core ignores the index there).
    if f.oneofIndex >= 0, let oneofs = parentOneofs {
        if Int(f.oneofIndex) >= oneofs.count {
            throw CoreError("field \(f.name ?? ""): oneof_index out of range")
        }
        f.oneof = Int(f.oneofIndex)
    }
    if (f.type == tMessage || f.type == tGroup), let typeName = f.typeName {
        guard let idx = resolve(typeName, scope, l.messageByName) else {
            throw CoreError("field \(f.name ?? ""): no such type \(typeName)")
        }
        f.msgType = idx
    }
    if f.type == tEnum, let typeName = f.typeName {
        guard let idx = resolve(typeName, scope, l.enumByName) else {
            throw CoreError("field \(f.name ?? ""): no such enum \(typeName)")
        }
        f.enumType = idx
        // Editions may close one field in an open file; that wins over the enum
        // type, like the core's per-field enum_open.
        if f.enumOpen != 0 {
            f.enumClosed = f.enumOpen == 2
        } else {
            f.enumClosed = l.enumClosed[idx]
        }
    }
    if f.label == lRepeated && isPackableType(f.type) && !f.hasPackedOption {
        f.packed = proto3  // the syntax default
    }
    // utf8_validation: field FeatureSet, then the file's, then the dialect
    // default (VERIFY for proto3/editions, NONE for proto2). Only `string`
    // fields carry it, and only they validate on parse.
    if f.type == tString {
        let utf8: Int32
        if f.utf8 != 0 {
            utf8 = f.utf8
        } else if fileUtf8 != 0 {
            utf8 = fileUtf8
        } else {
            utf8 = proto3 ? 2 : 3
        }
        f.utf8 = utf8 == 2 ? 2 : 3
    } else {
        f.utf8 = 3
    }
    f.hasPresence =
        f.label != lRepeated
        && (f.type == tMessage || f.type == tGroup || f.label == lRequired || !proto3
            || f.proto3Optional)
}

private func isPackableType(_ t: Int32) -> Bool {
    !(t == tString || t == tGroup || t == tMessage || t == tBytes)
}

/// Leading-dot names resolve from the root; otherwise search the scope up.
private func resolve(_ name: String, _ scope: String, _ byName: [String: Int]) -> Int? {
    if name.hasPrefix(".") {
        return byName[String(name.dropFirst())]
    }
    var s = scope
    while true {
        let key = s.isEmpty ? name : "\(s).\(name)"
        if let i = byName[key] { return i }
        // Nested scopes qualify from the inside out: C++ scoping.
        if let dot = name.firstIndex(of: ".") {
            let head = String(name[..<dot])
            let rest = String(name[name.index(after: dot)...])
            let prefix = s.isEmpty ? head : "\(s).\(head)"
            if byName[prefix] != nil {
                return resolve(rest, prefix, byName)
            }
        }
        if s.isEmpty { return nil }
        if let cut = s.lastIndex(of: ".") {
            s = String(s[..<cut])
        } else {
            s = ""
        }
    }
}

// --------------------------------------------- compiled schema building

private func kindOf(_ t: Int32) -> Kind {
    switch t {
    case tInt32: return .int32
    case tUint32: return .uint32
    case tSint32: return .sint32
    case tInt64: return .int64
    case tUint64: return .uint64
    case tSint64: return .sint64
    case tBool: return .bool
    case tEnum: return .enumeration
    case tFixed32: return .fixed32
    case tSfixed32: return .sfixed32
    case tFloat: return .float
    case tFixed64: return .fixed64
    case tSfixed64: return .sfixed64
    case tDouble: return .double
    case tString: return .string
    case tBytes: return .bytes
    case tMessage: return .message
    default: return .group
    }
}

private func wireType(_ t: Int32, _ packed: Bool) -> UInt8 {
    if packed { return wtLen }
    switch t {
    case tDouble, tFixed64, tSfixed64: return wtFixed64
    case tString, tBytes, tMessage: return wtLen
    case tGroup: return wtStartGroup
    case tFixed32, tSfixed32, tFloat: return wtFixed32
    default: return wtVarint
    }
}

private func buildEnum(_ enumDefs: inout [EnumDef], _ e: PEnum) throws {
    var byName: [String: Int32] = [:]
    var byNumber: [Int32: String] = [:]
    for v in e.values {
        let name = v.name ?? ""
        if byName[name] != nil {
            throw CoreError("duplicate enum value \(e.fullName).\(name)")
        }
        byName[name] = v.number
        // Aliases repeat a number: the first declared name is printed.
        if byNumber[v.number] == nil { byNumber[v.number] = name }
    }
    var d = EnumDef()
    d.fullName = e.fullName
    d.byName = byName
    d.byNumber = byNumber
    d.closed = e.closed
    enumDefs[e.idx] = d
}

private func buildField(_ f: PField, _ oneofs: inout [Int: Int]) throws -> Field {
    let kind = kindOf(f.type)
    let repeated = f.label == lRepeated
    var oneof: Int32 = -1
    if let declared = f.oneof {
        if let slot = oneofs[declared] {
            oneof = Int32(slot)
        } else {
            let slot = oneofs.count
            oneofs[declared] = slot
            oneof = Int32(slot)
        }
    }
    let presence: Presence
    if oneof >= 0 {
        presence = .explicit  // reads/writes go through the oneof case
    } else if f.label == lRequired {
        presence = .required
    } else if kind == .message || kind == .group {
        presence = .messagePresence
    } else if f.hasPresence {
        presence = .explicit
    } else {
        presence = .implicit
    }
    let packed = repeated && f.packed
    let number = UInt32(f.number)
    var out = Field()
    out.number = number
    out.name = f.name ?? ""
    out.kind = kind
    out.wt = wireType(f.type, packed)
    out.elemWt = wireType(f.type, false)
    out.repeated = repeated
    out.packed = packed
    out.oneof = oneof
    out.presence = presence
    out.msgType = f.msgType
    out.enumType = f.enumType
    out.enumClosed = f.enumClosed
    out.utf8 = f.type == tString && f.utf8 == 2
    out.fullName = f.fullName
    out.tag = Tag(fieldNumber: number, wireType: out.wt)
    out.tagEnd = kind == .group ? Tag(fieldNumber: number, wireType: wtEndGroup) : Tag.empty
    return out
}

private func finishDef(_ d: MsgDef, _ unsorted: [Field], _ oneofs: [Int: Int],
                       _ oneofNames: [String])
{
    // Stable sort by field number: the slots decode into are field-number
    // ordered, like the C core's table. Swift's sort is not stable, so the
    // original index breaks ties explicitly.
    let fields = unsorted.enumerated().sorted {
        $0.element.number == $1.element.number
            ? $0.offset < $1.offset : $0.element.number < $1.element.number
    }.map(\.element)
    var maxNumber: UInt32 = 0
    var hasRequired = false
    for f in fields {
        if f.number > maxNumber { maxNumber = f.number }
        if f.presence == .required { hasRequired = true }
    }
    d.hasRequired = hasRequired
    // Dense only when bounded, like the C core's fnum map: one field numbered
    // 536870911 must not allocate a 2 GiB table.
    if maxNumber <= 512, Int(maxNumber) <= fields.count * 8 + 8 {
        var slots = [Int32](repeating: -1, count: Int(maxNumber) + 1)
        for (i, f) in fields.enumerated() { slots[Int(f.number)] = Int32(i) }
        d.slotByFnum = slots
    } else {
        d.slotByFnum = nil
    }
    var byName: [String: Int] = [:]
    for (i, f) in fields.enumerated() { byName[f.name] = i }
    d.byName = byName
    var oneofByName: [String: Int] = [:]
    for (declared, slot) in oneofs where declared < oneofNames.count {
        oneofByName[oneofNames[declared]] = slot
    }
    d.oneofByName = oneofByName
    d.oneofCount = max(d.oneofCount, oneofs.count)
    d.fields = fields
}

private func buildDef(_ defs: inout [MsgDef], _ messages: [PMessage], _ pos: Int, _ depth: UInt32)
    throws
{
    if depth > maxDepth {
        throw CoreError("descriptor nesting exceeds \(maxDepth)")
    }
    var oneofs: [Int: Int] = [:]
    var fields: [Field] = []
    fields.reserveCapacity(messages[pos].fields.count)
    for f in messages[pos].fields {
        fields.append(try buildField(f, &oneofs))
    }
    let names = messages[pos].oneofs.map(\.name)
    let idx = messages[pos].idx
    let d = MsgDef()
    d.fullName = messages[pos].fullName
    d.reservedNames = messages[pos].reservedNames
    d.mapEntry = messages[pos].mapEntry
    finishDef(d, fields, oneofs, names)
    defs[idx] = d
    let nested = messages[pos].nested
    for n in nested { try buildDef(&defs, messages, n, depth + 1) }
}

private func attachExtension(_ defs: [MsgDef], _ e: PField, _ byName: [String: Int]) throws {
    guard var extendee = e.extendee else {
        throw CoreError("extension without extendee")
    }
    if extendee.hasPrefix(".") { extendee = String(extendee.dropFirst()) }
    guard let idx = byName[extendee] else {
        throw CoreError("extendee missing: \(e.extendee ?? "")")
    }
    var empty: [Int: Int] = [:]
    var s = try buildField(e, &empty)
    s.fullName = e.fullName
    let d = defs[idx]
    let oneofByName = d.oneofByName
    let count = d.oneofCount
    var fields = d.fields
    fields.append(s)
    let noOneofs: [Int: Int] = [:]
    finishDef(d, fields, noOneofs, [])
    d.oneofByName = oneofByName  // extensions are never in a oneof
    d.oneofCount = count
}

private func attachMessageExtensions(_ defs: [MsgDef], _ messages: [PMessage], _ pos: Int,
                                     _ byName: [String: Int]) throws
{
    for e in messages[pos].extensions { try attachExtension(defs, e, byName) }
    let nested = messages[pos].nested
    for n in nested { try attachMessageExtensions(defs, messages, n, byName) }
}

private func finishTransitive(_ defs: [MsgDef]) {
    while true {
        var changed = false
        for idx in defs.indices where !defs[idx].deepRequired {
            let needs = defs[idx].fields.contains { f in
                f.presence == .required
                    || ((f.kind == .message || f.kind == .group)
                        && (f.msgType.map { defs[$0].deepRequired } ?? false))
            }
            if needs {
                defs[idx].deepRequired = true
                changed = true
            }
        }
        if !changed { return }
    }
}

/// Parses and links a `FileDescriptorSet` into a compiled schema.
public func loadSchema(_ bytes: [UInt8], _ limits: Limits) throws -> Schema {
    try limits.checkBytes(bytes.count, "descriptor set")
    let parsed = try parseSet(bytes)
    var files = parsed.0
    var messages = parsed.1
    var enums = parsed.2
    let l = Linker()
    do {
        for i in files.indices {
            let pkg = files[i].pkg
            for pos in files[i].messages {
                try indexMessage(&messages, pos, pkg, l, &enums)
            }
            for pos in files[i].enums { try indexEnum(&enums, pos, pkg, l) }
        }
        for i in files.indices {
            // Editions files carry syntax "editions" plus field 14: the core
            // treats them as proto3, so packed defaults and presence match.
            let proto3 = files[i].syntax == "proto3" || files[i].edition
            let fileEnumOpen = files[i].enumOpen
            for pos in files[i].enums {
                try linkEnum(&enums, pos, proto3, fileEnumOpen, l)
            }
            // Enums close/open before fields link: a field copies its enum's
            // closed flag, so the enum must be linked first.
            let fileMessages = files[i].messages
            for pos in fileMessages {
                try linkEnums(&messages, pos, proto3, fileEnumOpen, l, &enums)
            }
        }
        for i in files.indices {
            let proto3 = files[i].syntax == "proto3" || files[i].edition
            let pkg = files[i].pkg
            let fileUtf8 = files[i].utf8
            let fileMessages = files[i].messages
            for pos in fileMessages {
                try linkMessage(&messages, pos, pkg, proto3, fileUtf8, l)
            }
            let extCount = files[i].extensions.count
            for j in 0..<extCount {
                try linkField(&files[i].extensions[j], pkg, proto3, fileUtf8, nil, l)
                let name = files[i].extensions[j].name ?? ""
                files[i].extensions[j].fullName = pkg.isEmpty ? name : "\(pkg).\(name)"
            }
        }
    } catch let e as CoreError {
        throw CoreError("descriptor set: \(e.message)", status: 1)
    }

    var enumDefs = [EnumDef](repeating: EnumDef(), count: l.nextEnum)
    for pos in enums.indices { try buildEnum(&enumDefs, enums[pos]) }
    var defs = (0..<l.nextMessage).map { _ in MsgDef() }
    for i in files.indices {
        let fileMessages = files[i].messages
        for pos in fileMessages { try buildDef(&defs, messages, pos, 1) }
    }
    // Extensions attach to their extendee after all defs exist.
    for i in files.indices {
        for e in files[i].extensions {
            try attachExtension(defs, e, l.messageByName)
        }
        let fileMessages = files[i].messages
        for pos in fileMessages {
            try attachMessageExtensions(defs, messages, pos, l.messageByName)
        }
    }
    finishTransitive(defs)
    return Schema(messages: defs, enums: enumDefs, messageByName: l.messageByName,
                  enumByName: l.enumByName)
}

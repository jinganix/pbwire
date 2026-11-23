// Decoder: strict tags and lengths (like the C core), unknown field numbers
// AND wrong-wire-type occurrences retained as raw tag+payload bytes in input
// order, groups decoded to their matching end tag, packed runs accepted for
// every numeric kind (and the unpacked element form for a packed field),
// oneofs collapsed to the last member, presence following the field's own
// zero, proto2 required fields checked over the whole tree, and the nesting cap
// enforced on every recursive path.
//
// The Swift twin of `bindings/rust/src/core/decode.rs`. The reader is a value
// type passed `inout` (see `Reader`), and the hot loop mutates the `Message`
// class directly instead of taking its arrays `inout`: a class property passed
// `inout` costs a runtime exclusivity check, a direct store does not.
//
// The Swift twin of `bindings/rust/src/core/decode.rs`.

/// Decodes a whole message, unknown fields retained.
public func decode(_ schema: Schema, _ def: Int, _ data: [UInt8], _ limits: Limits = Limits())
    throws -> Message
{
    try limits.checkBytes(data.count, "message")
    var r = Reader(data)
    r.cap = limits.effectiveDepth
    let m = Message(schema: schema, def: def)
    try decodeInto(m, &r, data.count, -1, 0)
    if m.msgDef.deepRequired {
        try checkRequired(m, r.cap, 0)
    }
    return m
}

/// Decodes a message body that already starts at `off` and ends at
/// `off + length`.
public func decodeSlice(_ schema: Schema, _ def: Int, _ data: [UInt8], _ off: Int, _ length: Int,
                        _ limits: Limits = Limits()) throws -> Message
{
    try limits.checkBytes(length, "message")
    let end = off + length
    var r = try Reader(data, from: off, to: end)
    r.cap = limits.effectiveDepth
    let m = Message(schema: schema, def: def)
    try decodeInto(m, &r, end, -1, 0)
    if m.msgDef.deepRequired {
        try checkRequired(m, r.cap, 0)
    }
    return m
}

/// True when the field does not accept this wire type. A repeated numeric field
/// always accepts the LEN form (a packed run, whatever the declared form) and a
/// packed field also accepts its unpacked element form.
private func wtIncompatible(_ f: Field, _ wt: UInt8) -> Bool {
    if f.kind == .group { return wt != wtStartGroup }
    if f.repeated && f.kind.isPackable {
        if wt == wtLen { return false }
        if f.packed { return wt != f.elemWt }
    }
    return wt != f.wt
}

/// Stores a value in a slot, collapsing the oneof it belongs to.
private func store(_ m: Message, _ defIdx: Int, _ slot: Int, _ value: Value) {
    let oneof = m.schema.messages[defIdx].fields[slot].oneof
    if oneof >= 0 && !m.oneofCase.isEmpty {
        let prev = m.oneofCase[Int(oneof)]
        if prev != 0 && prev != UInt32(slot) + 1 {
            m.values[Int(prev) - 1] = nil
        }
        m.oneofCase[Int(oneof)] = UInt32(slot) + 1
    }
    m.values[slot] = value
}

/// Appends one element to a repeated slot. The slot is cleared first so the
/// array buffer is uniquely referenced and `append` stays amortized O(1)
/// instead of copying the list on every element (Swift arrays are COW).
private func appendItem(_ m: Message, _ slot: Int, _ value: Value) {
    if case .list(var items)? = m.values[slot] {
        m.values[slot] = nil
        items.append(value)
        m.values[slot] = .list(items)
    } else {
        m.values[slot] = .list([value])
    }
}

func decodeInto(_ m: Message, _ r: inout Reader, _ end: Int, _ groupEnd: Int64, _ depth: UInt32)
    throws
{
    if depth > r.cap {
        throw CoreError("nesting exceeds \(r.cap)", status: 4)
    }
    let outerEnd = r.end
    r.end = end
    var closed = groupEnd < 0
    let schema = m.schema
    let defIdx = m.def
    let def = schema.messages[defIdx]
    while r.pos < r.end {
        let fieldStart = r.pos
        let tag = try r.readTag()
        let fnum = tag >> 3
        let wt = UInt8(tag & 7)
        if fnum == 0 || wt > wtFixed32 {
            throw CoreError("bad tag \(tag)", status: 1)
        }
        if wt == wtEndGroup {
            // Only our own number closes us; any other end tag is stray.
            if groupEnd < 0 || Int64(fnum) != groupEnd {
                throw CoreError("stray end-group for \(fnum)", status: 1)
            }
            closed = true
            break
        }
        let slot = def.slot(of: fnum)
        if slot < 0 || wtIncompatible(def.fields[Int(slot)], wt) {
            // Wrong wire type (or unknown number) keeps the raw blob.
            try r.skipUnknown(fnum, wt, depth)
            m.unknownFields.append(Array(r.b[fieldStart..<r.pos]))
            continue
        }
        try readKnown(schema, defIdx, m, &r, Int(slot), wt, depth)
    }
    r.end = outerEnd
    if !closed {
        throw CoreError("missing end-group for \(groupEnd)", status: 1)
    }
}

private func readKnown(_ schema: Schema, _ defIdx: Int, _ m: Message, _ r: inout Reader,
                       _ slot: Int, _ wt: UInt8, _ depth: UInt32) throws
{
    let f = schema.messages[defIdx].fields[slot]
    let kind = f.kind
    if f.repeated {
        if kind.isPackable {
            var items: [Value] = []
            if case .list(let held)? = m.values[slot] {
                m.values[slot] = nil  // make the buffer unique before appending
                items = held
            }
            if wt == wtLen {
                try readPacked(&r, kind, &items)
                // An empty packed run adds nothing: presence follows length, so
                // the holder stays dropped.
                if !items.isEmpty { m.values[slot] = .list(items) }
                return
            }
            items.append(try readNumSingle(&r, kind))
            m.values[slot] = .list(items)
            return
        }
        if kind == .message {
            let length = try r.readLen32()
            let subEnd = r.pos + length
            let sub = Message(schema: schema, def: f.msgType!)
            try decodeInto(sub, &r, subEnd, -1, depth + 1)
            appendItem(m, slot, .message(sub))
            return
        }
        if kind == .group {
            let sub = Message(schema: schema, def: f.msgType!)
            let groupEnd = Int64(f.number)
            try decodeInto(sub, &r, r.end, groupEnd, depth + 1)
            appendItem(m, slot, .message(sub))
            return
        }
        // String / bytes element: validate, then record one payload.
        let length = try r.readLen32()
        if f.utf8 && !utf8Valid(r.b, from: r.pos, count: length) {
            throw CoreError("invalid UTF-8 in a string field", status: 1)
        }
        let payload = Array(r.b[r.pos..<r.pos + length])
        r.pos += length
        appendItem(m, slot, .bytes(payload))
        return
    }
    if kind.isPackable {
        let value = try readNumSingle(&r, kind)
        // Implicit presence is decided on the *field's* zero, not the raw wire
        // bits: an int32/uint32 varint wider than 32 bits truncates and the
        // reference then drops the field as an implicit zero.
        if f.presence == .implicit && isZero(value) { return }
        store(m, defIdx, slot, value)
        return
    }
    // Ref-kind singular: a second occurrence merges (like the core).
    if kind == .message {
        let length = try r.readLen32()
        let subEnd = r.pos + length
        let canMerge: Bool
        if case .message? = m.values[slot] {
            canMerge = f.oneof < 0 || m.oneofCase.isEmpty
                || m.oneofCase[Int(f.oneof)] == UInt32(slot) + 1
        } else {
            canMerge = false
        }
        if canMerge, case .message(let sub)? = m.values[slot] {
            try decodeInto(sub, &r, subEnd, -1, depth + 1)
            return
        }
        let sub = Message(schema: schema, def: f.msgType!)
        try decodeInto(sub, &r, subEnd, -1, depth + 1)
        store(m, defIdx, slot, .message(sub))
        return
    }
    if kind == .group {
        let groupEnd = Int64(f.number)
        let canMerge: Bool
        if case .message? = m.values[slot] {
            canMerge = f.oneof < 0 || m.oneofCase.isEmpty
                || m.oneofCase[Int(f.oneof)] == UInt32(slot) + 1
        } else {
            canMerge = false
        }
        if canMerge, case .message(let sub)? = m.values[slot] {
            try decodeInto(sub, &r, r.end, groupEnd, depth + 1)
            return
        }
        let sub = Message(schema: schema, def: f.msgType!)
        try decodeInto(sub, &r, r.end, groupEnd, depth + 1)
        store(m, defIdx, slot, .message(sub))
        return
    }
    // String / bytes (an empty implicit string stays absent).
    let length = try r.readLen32()
    if length == 0 && f.presence == .implicit { return }
    // proto3/editions strings are validated here, at parse, like the reference:
    // a malformed value fails the decode instead of surfacing as U+FFFD. proto2
    // keeps the loose mapping.
    if f.utf8 && !utf8Valid(r.b, from: r.pos, count: length) {
        throw CoreError("invalid UTF-8 in a string field", status: 1)
    }
    let payload = Array(r.b[r.pos..<r.pos + length])
    r.pos += length
    store(m, defIdx, slot, .bytes(payload))
}

private func isZero(_ v: Value) -> Bool {
    switch v {
    case .i32(let x): return x == 0
    case .u32(let x): return x == 0
    case .i64(let x): return x == 0
    case .u64(let x): return x == 0
    case .bool(let b): return !b
    case .f32(let x): return x == 0
    case .f64(let x): return x == 0
    default: return false
    }
}

/// One packed run of numeric elements into `items`.
private func readPacked(_ r: inout Reader, _ kind: Kind, _ items: inout [Value]) throws {
    let length = try r.readLen32()
    let end = r.pos + length
    switch kind {
    case .bool:
        while r.pos < end { items.append(.bool(try r.readVarBool())) }
    case .fixed32, .sfixed32, .float:
        items.reserveCapacity(items.count + length / 4)
        for _ in 0..<(length / 4) {
            let v = try r.readFixed32()
            items.append(kind == .sfixed32 ? .i32(Int32(bitPattern: v)) : .u32(v))
        }
    case .fixed64, .sfixed64, .double:
        items.reserveCapacity(items.count + length / 8)
        for _ in 0..<(length / 8) {
            let v = try r.readFixed64()
            items.append(kind == .sfixed64 ? .i64(Int64(bitPattern: v)) : .u64(v))
        }
    default:
        while r.pos < end { items.append(try readPackedVar(&r, kind)) }
    }
    if r.pos != end {
        throw CoreError("packed overrun", status: 1)
    }
}

private func readPackedVar(_ r: inout Reader, _ kind: Kind) throws -> Value {
    switch kind {
    case .int32, .enumeration: return .i32(Int32(bitPattern: try r.readVar32()))
    case .uint32: return .u32(try r.readVar32())
    // Truncate before unzigzag: a 64-bit varint on a sint32 field decodes its
    // low 32 bits, like the reference.
    case .sint32: return .i32(unzig32(try r.readVar32()))
    case .int64: return .i64(Int64(bitPattern: try r.readVar64()))
    case .uint64: return .u64(try r.readVar64())
    case .sint64: return .i64(unzig64(try r.readVar64()))
    default: throw CoreError("not numeric", status: 1)
    }
}

/// One numeric element in its storage form.
private func readNumSingle(_ r: inout Reader, _ kind: Kind) throws -> Value {
    switch kind {
    case .int32, .enumeration: return .i32(Int32(bitPattern: try r.readVar32()))
    case .uint32: return .u32(try r.readVar32())
    case .bool: return .bool(try r.readVarBool())
    case .sint32: return .i32(unzig32(try r.readVar32()))
    case .int64: return .i64(Int64(bitPattern: try r.readVar64()))
    case .uint64: return .u64(try r.readVar64())
    case .sint64: return .i64(unzig64(try r.readVar64()))
    case .fixed32: return .u32(try r.readFixed32())
    case .sfixed32: return .i32(Int32(bitPattern: try r.readFixed32()))
    case .float: return .f32(try r.readFixed32())
    case .fixed64: return .u64(try r.readFixed64())
    case .sfixed64: return .i64(Int64(bitPattern: try r.readFixed64()))
    case .double: return .f64(try r.readFixed64())
    default: throw CoreError("not numeric", status: 1)
    }
}

/// Whole-tree required check: every present nested message is checked too, so a
/// missing proto2 required field fails the decode, like the core.
private func checkRequired(_ m: Message, _ cap: UInt32, _ depth: UInt32) throws {
    if depth > cap {
        throw CoreError("nesting exceeds \(cap)", status: 4)
    }
    let def = m.schema.messages[m.def]
    if def.hasRequired {
        for (slot, f) in def.fields.enumerated() where f.presence == .required {
            if m.values[slot] == nil {
                throw CoreError("missing required field \(def.fullName).\(f.name)", status: 6)
            }
        }
    }
    for (slot, f) in def.fields.enumerated() {
        guard f.kind == .message || f.kind == .group else { continue }
        guard let value = m.values[slot] else { continue }
        switch value {
        case .message(let sub):
            try checkRequired(sub, cap, depth + 1)
        case .list(let items):
            for item in items {
                if case .message(let sub) = item { try checkRequired(sub, cap, depth + 1) }
            }
        default:
            break
        }
    }
}

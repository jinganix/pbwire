// Encoder: one pass into a buffer, fields in schema (field-number) order,
// unknown fields appended verbatim in input order. Nested payloads use a
// one-byte length placeholder patched afterwards and packed runs use a
// five-byte placeholder shifted down once, so a decode/encode round trip is
// byte-exact.
//
// The Swift twin of `bindings/rust/src/core/encode.rs`.

/// Encodes a tree into a fresh buffer.
public func encode(_ m: Message) throws -> [UInt8] {
    var w: [UInt8] = []
    w.reserveCapacity(256)
    try encodeInto(m, &w, 0)
    return w
}

/// Encodes a tree into a reusable buffer (cleared first): a hot loop pays no
/// allocation per op.
public func encode(_ m: Message, into w: inout [UInt8]) throws {
    w.removeAll(keepingCapacity: true)
    try encodeInto(m, &w, 0)
}

/// Encodes a tree body into the buffer, appending.
public func encodeInto(_ m: Message, _ w: inout [UInt8], _ depth: UInt32) throws {
    if depth > maxDepth {
        throw CoreError("nesting exceeds \(maxDepth)", status: 4)
    }
    let def = m.schema.messages[m.def]
    let noRequired = !def.hasRequired  // proto3: skip the check
    for (slot, f) in def.fields.enumerated() {
        guard let v = m.values[slot] else {
            if !noRequired && f.presence == .required {
                throw CoreError("missing required field \(def.fullName).\(f.name)", status: 6)
            }
            continue
        }
        if f.repeated {
            let items: [Value]
            if case .list(let l) = v { items = l } else { items = [] }
            if items.isEmpty { continue }
            if f.kind.isPackable {
                try encodeRepeatedNum(&w, f, items)
            } else {
                for item in items { try writeValue(&w, f, item, depth) }
            }
            continue
        }
        if f.kind.isPackable {
            // Only implicit defaults skip here; explicit defaults (and
            // required) emit once set.
            if isZero(v) && f.presence == .implicit { continue }
            writeSingularNum(&w, f, v)
            continue
        }
        // Implicit empty strings and bytes stay silent, everything else emits.
        if f.presence == .implicit, case .bytes(let b) = v, b.isEmpty {
            continue
        }
        try writeValue(&w, f, v, depth)
    }
    // Unknown fields last, in input order.
    for u in m.unknownFields {
        w.append(contentsOf: u)
    }
}

private func isZero(_ v: Value) -> Bool {
    switch v {
    case .bool(let b): return !b
    case .i32(let x): return x == 0
    case .u32(let x): return x == 0
    case .i64(let x): return x == 0
    case .u64(let x): return x == 0
    case .f32(let x): return x == 0
    case .f64(let x): return x == 0
    default: return false
    }
}

private func writeValue(_ w: inout [UInt8], _ f: Field, _ v: Value, _ depth: UInt32) throws {
    switch f.kind {
    case .string, .bytes:
        writeBytes(&w, f.tag, v.asBytes ?? [])
    case .message:
        // Inlined tag + length placeholder + patch: a repeated message field is
        // the hottest nesting shape.
        guard let sub = v.asMessage else { return }
        writeTag(&w, f.tag)
        let lp = w.count
        w.append(0)
        try encodeInto(sub, &w, depth + 1)
        patchLen(&w, lp)
    case .group:
        // Start tag, body, end tag.
        guard let sub = v.asMessage else { return }
        writeTag(&w, f.tag)
        try encodeInto(sub, &w, depth + 1)
        writeTag(&w, f.tagEnd)
    default:
        return
    }
}

/// Singular numeric write: one fused tag+value call per field.
private func writeSingularNum(_ w: inout [UInt8], _ f: Field, _ v: Value) {
    let i = v.asI64 ?? 0
    let u = v.asU64 ?? 0
    switch f.kind {
    // Encode the int32 interpretation, sign-extended: the reference truncates
    // a wide varint to int32 and re-emits a negative as 10 bytes.
    case .int32, .enumeration:
        if i >= 0 {
            writeTagVar32(&w, f.tag, UInt32(truncatingIfNeeded: i))
        } else {
            writeTagVar64(&w, f.tag, u)
        }
    case .uint32: writeTagVar32(&w, f.tag, UInt32(truncatingIfNeeded: u))
    case .sint32: writeTagVar32(&w, f.tag, zig32(Int32(truncatingIfNeeded: i)))
    case .int64, .uint64: writeTagVar64(&w, f.tag, u)
    case .sint64: writeTagVar64(&w, f.tag, zig64(i))
    case .bool: writeTagVar32(&w, f.tag, u != 0 ? 1 : 0)
    case .fixed32, .sfixed32, .float: writeTagFixed32(&w, f.tag, UInt32(truncatingIfNeeded: u))
    // fixed64, sfixed64, double
    default: writeTagFixed64(&w, f.tag, u)
    }
}

/// One dispatch per field; the loops below it are branch-free writes.
private func encodeRepeatedNum(_ out: inout [UInt8], _ f: Field, _ items: [Value]) throws {
    let n = items.count
    if f.packed {
        switch f.kind {
        case .bool:
            writeTag(&out, f.tag)
            writeLen(&out, n)
            out.reserveCapacity(out.count + n)
            for v in items { out.append(v.asBool == true ? 1 : 0) }
        case .fixed32, .sfixed32, .float:
            writeTag(&out, f.tag)
            writeLen(&out, n * 4)
            out.reserveCapacity(out.count + n * 4)
            for v in items { writeFixed32(&out, UInt32(truncatingIfNeeded: v.asU64 ?? 0)) }
        case .fixed64, .sfixed64, .double:
            writeTag(&out, f.tag)
            writeLen(&out, n * 8)
            out.reserveCapacity(out.count + n * 8)
            for v in items { writeFixed64(&out, v.asU64 ?? 0) }
        default:
            // Varint kinds: write the payload over a 5-byte placeholder, then
            // shift it down: one move beats a size pre-pass over thousands of
            // elements.
            writeTag(&out, f.tag)
            let lp = out.count
            out.reserveCapacity(out.count + n * 10 + 5)
            out.append(contentsOf: [0, 0, 0, 0, 0])
            for v in items { writeNumSingle(&out, f.kind, v) }
            let length = out.count - lp - 5
            let llen = sizeVar32(UInt64(length))
            if llen != 5 {
                var i = 0
                while i < length {
                    out[lp + llen + i] = out[lp + 5 + i]
                    i += 1
                }
                out.removeLast(5 - llen)
            }
            var p = lp
            var x = UInt64(length)
            while x > 0x7f {
                out[p] = UInt8(truncatingIfNeeded: x) & 0x7f | 0x80
                p += 1
                x >>= 7
            }
            out[p] = UInt8(truncatingIfNeeded: x)
        }
        return
    }
    for v in items {
        writeTag(&out, f.tag)
        writeNumSingle(&out, f.kind, v)
    }
}

private func writeNumSingle(_ w: inout [UInt8], _ kind: Kind, _ v: Value) {
    let i = v.asI64 ?? 0
    let u = v.asU64 ?? 0
    switch kind {
    case .int32, .enumeration:
        if i >= 0 {
            writeVar32(&w, UInt32(truncatingIfNeeded: i))
        } else {
            writeVar64(&w, u)
        }
    case .uint32: writeVar32(&w, UInt32(truncatingIfNeeded: u))
    case .sint32: writeVar32(&w, zig32(Int32(truncatingIfNeeded: i)))
    case .int64, .uint64: writeVar64(&w, u)
    case .sint64: writeVar64(&w, zig64(i))
    case .bool: w.append(u != 0 ? 1 : 0)
    case .fixed32, .sfixed32, .float: writeFixed32(&w, UInt32(truncatingIfNeeded: u))
    // fixed64, sfixed64, double
    default: writeFixed64(&w, u)
    }
}

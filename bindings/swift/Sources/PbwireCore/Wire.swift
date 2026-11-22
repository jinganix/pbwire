// Protobuf wire primitives for the pure-Swift core.
//
// Varint/tag/little-endian read and write, zigzag, skipping (with group
// frames) and the structural UTF-8 check. One home for everything the
// descriptor reader, the encoder and the decoder do to bytes, so they cannot
// diverge.
//
// The rules are the C core's (`src/wire.c`, `src/decode.c`): strict tags and
// lengths, unknown fields skippable by shape, groups validated to their
// matching end tag, one nesting cap for groups and messages alike. This is the
// twin of `bindings/rust/src/core/wire.rs` and `bindings/python/pure/wire.py`.

public let wtVarint: UInt8 = 0
public let wtFixed64: UInt8 = 1
public let wtLen: UInt8 = 2
public let wtStartGroup: UInt8 = 3
public let wtEndGroup: UInt8 = 4
public let wtFixed32: UInt8 = 5

/// Nesting cap, the same bound as the C core's `PBW_MAX_DEPTH`.
public let maxDepth: UInt32 = 256

/// A pbwire failure: the C core's `pbw_status` where one exists, else -1.
public struct CoreError: Error, CustomStringConvertible, Sendable {
    public let message: String
    /// The C core's `pbw_status` for the same failure (1 malformed, 4 depth,
    /// 5 size, 6 required); -1 when there is no C counterpart.
    public let status: Int32

    public init(_ message: String, status: Int32 = -1) {
        self.message = message
        self.status = status
    }

    public var description: String { "pbwire: \(message)" }
}

/// Bounds for untrusted input; zero fields mean the defaults.
public struct Limits: Sendable {
    /// Rejects inputs larger than this; 0 means no cap.
    public let maxBytes: Int
    /// Nesting cap; 0 means `maxDepth`.
    public let maxDepth: UInt32

    public init(maxBytes: Int = 0, maxDepth: UInt32 = 0) {
        self.maxBytes = maxBytes
        self.maxDepth = maxDepth
    }

    var effectiveDepth: UInt32 {
        maxDepth == 0 || maxDepth > PbwireCore.maxDepth ? PbwireCore.maxDepth : maxDepth
    }

    func checkBytes(_ n: Int, _ what: String) throws {
        if maxBytes > 0 && n > maxBytes {
            throw CoreError("\(what) exceeds the \(maxBytes)-byte limit", status: 5)
        }
    }
}

/// A precomputed tag: field number and wire type as their varint bytes, inline.
/// A hot loop copies at most five bytes and touches no reference, so a tag write
/// costs no ARC traffic (an `[UInt8]` field here was the largest single cost in
/// the first profile of the core arm).
public struct Tag: Sendable, Equatable {
    @usableFromInline let b0: UInt8
    @usableFromInline let b1: UInt8
    @usableFromInline let b2: UInt8
    @usableFromInline let b3: UInt8
    @usableFromInline let b4: UInt8
    /// Number of valid bytes, 0 for `Tag.empty`.
    public let count: Int

    public init(fieldNumber: UInt32, wireType: UInt8) {
        var tmp: [UInt8] = [0, 0, 0, 0, 0]
        var tag = UInt64(fieldNumber) * 8 + UInt64(wireType)
        var n = 0
        while tag > 0x7f {
            tmp[n] = UInt8(truncatingIfNeeded: tag) & 0x7f | 0x80
            n += 1
            tag >>= 7
        }
        tmp[n] = UInt8(truncatingIfNeeded: tag)
        n += 1
        b0 = tmp[0]
        b1 = tmp[1]
        b2 = tmp[2]
        b3 = tmp[3]
        b4 = tmp[4]
        count = n
    }

    private init(_ bytes: [UInt8], _ count: Int) {
        b0 = count > 0 ? bytes[0] : 0
        b1 = count > 1 ? bytes[1] : 0
        b2 = count > 2 ? bytes[2] : 0
        b3 = count > 3 ? bytes[3] : 0
        b4 = count > 4 ? bytes[4] : 0
        self.count = count
    }

    /// The absent tag (every non-group field's `tagEnd`).
    public static let empty = Tag([0, 0, 0, 0, 0], 0)

    /// The tag's bytes as an array (tests and descriptor reading only; the write
    /// path uses the inline bytes).
    public var bytes: [UInt8] {
        var out = [b0, b1, b2, b3, b4]
        out.removeLast(5 - count)
        return out
    }
}

// ------------------------------------------------------------------ writing

@inline(__always)
public func writeTag(_ w: inout [UInt8], _ tag: Tag) {
    switch tag.count {
    case 1:
        w.append(tag.b0)
    case 2:
        w.append(tag.b0)
        w.append(tag.b1)
    case 3:
        w.append(tag.b0)
        w.append(tag.b1)
        w.append(tag.b2)
    case 4:
        w.append(tag.b0)
        w.append(tag.b1)
        w.append(tag.b2)
        w.append(tag.b3)
    case 5:
        w.append(tag.b0)
        w.append(tag.b1)
        w.append(tag.b2)
        w.append(tag.b3)
        w.append(tag.b4)
    default:
        return
    }
}

@inline(__always)
public func writeVar32(_ w: inout [UInt8], _ value: UInt32) {
    var x = UInt64(value)
    while x > 0x7f {
        w.append(UInt8(truncatingIfNeeded: x) & 0x7f | 0x80)
        x >>= 7
    }
    w.append(UInt8(truncatingIfNeeded: x))
}

@inline(__always)
public func writeVar64(_ w: inout [UInt8], _ value: UInt64) {
    var x = value
    while x > 0x7f {
        w.append(UInt8(truncatingIfNeeded: x) & 0x7f | 0x80)
        x >>= 7
    }
    w.append(UInt8(truncatingIfNeeded: x))
}

@inline(__always)
public func writeFixed32(_ w: inout [UInt8], _ value: UInt32) {
    w.append(UInt8(truncatingIfNeeded: value))
    w.append(UInt8(truncatingIfNeeded: value >> 8))
    w.append(UInt8(truncatingIfNeeded: value >> 16))
    w.append(UInt8(truncatingIfNeeded: value >> 24))
}

@inline(__always)
public func writeFixed64(_ w: inout [UInt8], _ value: UInt64) {
    writeFixed32(&w, UInt32(truncatingIfNeeded: value))
    writeFixed32(&w, UInt32(truncatingIfNeeded: value >> 32))
}

@inline(__always)
public func writeLen(_ w: inout [UInt8], _ n: Int) {
    writeVar64(&w, UInt64(n))
}

/// Fused tag + varint32: one reserve per field.
@inline(__always)
public func writeTagVar32(_ w: inout [UInt8], _ tag: Tag, _ value: UInt32) {
    writeTag(&w, tag)
    writeVar32(&w, value)
}

@inline(__always)
public func writeTagVar64(_ w: inout [UInt8], _ tag: Tag, _ value: UInt64) {
    writeTag(&w, tag)
    writeVar64(&w, value)
}

@inline(__always)
public func writeTagFixed32(_ w: inout [UInt8], _ tag: Tag, _ value: UInt32) {
    writeTag(&w, tag)
    writeFixed32(&w, value)
}

@inline(__always)
public func writeTagFixed64(_ w: inout [UInt8], _ tag: Tag, _ value: UInt64) {
    writeTag(&w, tag)
    writeFixed64(&w, value)
}

/// Length-delimited payload under `tag` (string/bytes/message body).
@inline(__always)
public func writeBytes(_ w: inout [UInt8], _ tag: Tag, _ value: [UInt8]) {
    writeTag(&w, tag)
    writeLen(&w, value.count)
    w.append(contentsOf: value)
}

/// Patches a one-byte length placeholder written at `lp` with the body that now
/// follows it (shifting the body up when the length does not fit one byte).
public func patchLen(_ w: inout [UInt8], _ lp: Int) {
    let length = w.count - lp - 1
    if length < 128 {
        w[lp] = UInt8(truncatingIfNeeded: length)
        return
    }
    let n = sizeVar32(UInt64(length))
    // Insert the extra (n - 1) length bytes at lp + 1, moving the body up.
    w.append(contentsOf: repeatElement(0, count: n - 1))
    let end = w.count - (n - 1)
    var i = end - 1
    while i >= lp + 1 {
        w[i + n - 1] = w[i]
        i -= 1
    }
    var p = lp
    var x = UInt64(length)
    while x > 0x7f {
        w[p] = UInt8(truncatingIfNeeded: x) & 0x7f | 0x80
        p += 1
        x >>= 7
    }
    w[p] = UInt8(truncatingIfNeeded: x)
}

public func sizeVar32(_ value: UInt64) -> Int {
    var n = 1
    var x = value
    while x > 0x7f {
        x >>= 7
        n += 1
    }
    return n
}

@inline(__always)
public func zig32(_ v: Int32) -> UInt32 {
    (UInt32(bitPattern: v) << 1) ^ (v < 0 ? 0xffff_ffff : 0)
}

@inline(__always)
public func zig64(_ v: Int64) -> UInt64 {
    (UInt64(bitPattern: v) << 1) ^ (v < 0 ? 0xffff_ffff_ffff_ffff : 0)
}

@inline(__always)
public func unzig32(_ v: UInt32) -> Int32 {
    Int32(bitPattern: (v >> 1) ^ (0 &- (v & 1)))
}

@inline(__always)
public func unzig64(_ v: UInt64) -> Int64 {
    Int64(bitPattern: (v >> 1) ^ (0 &- (v & 1)))
}

// ------------------------------------------------------------------ reading

/// Cursor over an input buffer. One reader threads through a whole decode, so a
/// nested payload consumes exactly its slice and positions never need saving.
///
/// A value type on purpose: `inout` is statically enforced and free, while a
/// class's stored-property mutations cost one `swift_beginAccess` per byte read
/// (the first profile of the core arm was dominated by them). The payload array
/// is copied by reference (COW), so nesting copies a buffer header, not bytes.
public struct Reader {
    public let b: [UInt8]
    public var pos: Int
    public var end: Int
    /// Nesting cap for this decode; set from `Limits` at entry.
    public var cap: UInt32

    public init(_ b: [UInt8]) {
        self.b = b
        self.pos = 0
        self.end = b.count
        self.cap = maxDepth
    }

    public init(_ b: [UInt8], from off: Int, to end: Int) throws {
        if off > end || end > b.count {
            throw CoreError("descriptor overrun")
        }
        self.b = b
        self.pos = off
        self.end = end
        self.cap = maxDepth
    }

    /// varint, low 32 bits (unsigned). Validates the encoding's length only:
    /// the high bits are discarded on purpose, like the C core's `uint32_t`.
    public mutating func readVar32() throws -> UInt32 {
        var p = pos
        let e = end
        var lo: UInt64 = 0
        var j = 0
        while true {
            if p >= e { throw CoreError("truncated varint") }
            let c = b[p]
            p += 1
            if j < 5 { lo |= UInt64(c & 0x7f) << (j * 7) }
            if c & 0x80 == 0 {
                pos = p
                return UInt32(truncatingIfNeeded: lo)
            }
            j += 1
            if j >= 10 { throw CoreError("varint overflow") }
        }
    }

    /// varint, full 64 bits (unsigned).
    public mutating func readVar64() throws -> UInt64 {
        var p = pos
        let e = end
        var v: UInt64 = 0
        var shift = 0
        while true {
            if p >= e { throw CoreError("truncated varint") }
            let c = b[p]
            p += 1
            v |= UInt64(c & 0x7f) << shift
            if c & 0x80 == 0 {
                pos = p
                return v
            }
            shift += 7
            if shift >= 64 { throw CoreError("varint overflow") }
        }
    }

    /// varint reduced to its bool value: any set payload bit means true.
    public mutating func readVarBool() throws -> Bool {
        var p = pos
        let e = end
        var nz: UInt8 = 0
        var j = 0
        while true {
            if p >= e { throw CoreError("truncated varint") }
            let c = b[p]
            p += 1
            nz |= c & 0x7f
            if c & 0x80 == 0 {
                pos = p
                return nz != 0
            }
            j += 1
            if j >= 10 { throw CoreError("varint overflow") }
        }
    }

    /// Strict length read, like the core's: at most five bytes, the fifth only
    /// carries four bits; an out-of-range length is rejected, not truncated.
    /// Leaves `pos` after the length.
    public mutating func readLen32() throws -> Int {
        var p = pos
        let e = end
        if p >= e { throw CoreError("truncated length") }
        var c = b[p]
        p += 1
        var v: UInt64 = UInt64(c & 0x7f)
        var shift = 7
        while c & 0x80 != 0 {
            if shift >= 35 || p >= e { throw CoreError("bad length") }
            c = b[p]
            p += 1
            if shift == 28 && (c & 0x7f) > 0x0f { throw CoreError("length overflow") }
            v |= UInt64(c & 0x7f) << shift
            shift += 7
        }
        if p + Int(v) > e { throw CoreError("truncated length-delimited") }
        pos = p
        return Int(v)
    }

    /// Loose length read for descriptor parsing: any varint up to 2^31-1,
    /// bounds checked.
    public mutating func readLen() throws -> Int {
        let v = try readVar64()
        if v > 0x7fff_ffff { throw CoreError("length overflow") }
        if pos + Int(v) > end { throw CoreError("truncated length-delimited") }
        return Int(v)
    }

    /// Strict tag read: a varint32 of at most five bytes. A 5-byte tag above
    /// UINT32_MAX truncates instead of failing, exactly what the reference
    /// parser does (the official suite expects two such cases to parse).
    public mutating func readTag() throws -> UInt32 {
        var p = pos
        let e = end
        if p >= e { throw CoreError("truncated tag") }
        var c = b[p]
        p += 1
        var v: UInt64 = UInt64(c & 0x7f)
        var shift = 7
        while c & 0x80 != 0 {
            if shift >= 35 || p >= e { throw CoreError("bad tag") }
            c = b[p]
            p += 1
            v |= UInt64(c & 0x7f) << shift
            shift += 7
        }
        pos = p
        return UInt32(truncatingIfNeeded: v)
    }

    public mutating func readFixed32() throws -> UInt32 {
        if pos + 4 > end { throw CoreError("truncated fixed32") }
        let p = pos
        pos = p + 4
        return UInt32(b[p]) | UInt32(b[p + 1]) << 8 | UInt32(b[p + 2]) << 16
            | UInt32(b[p + 3]) << 24
    }

    public mutating func readFixed64() throws -> UInt64 {
        if pos + 8 > end { throw CoreError("truncated fixed64") }
        let p = pos
        pos = p + 8
        var v: UInt64 = 0
        for i in 0..<8 { v |= UInt64(b[p + i]) << (i * 8) }
        return v
    }

    /// Skips one field value of a non-group wire type.
    public mutating func skip(_ wt: UInt8) throws {
        switch wt {
        case wtVarint:
            _ = try readVarBool()
        case wtFixed64:
            if pos + 8 > end { throw CoreError("truncated fixed64") }
            pos += 8
        case wtLen:
            let n = try readLen32()
            pos += n
        case wtFixed32:
            if pos + 4 > end { throw CoreError("truncated fixed32") }
            pos += 4
        default:
            throw CoreError("bad wire type \(wt)")
        }
    }

    /// Validates a group body up to its matching end tag, like the core's group
    /// skipper: every tag is strict, field 0 and wire types above 5 are
    /// rejected, and an end tag only closes the group it names.
    public mutating func skipGroup(_ number: UInt32, _ depth: UInt32) throws {
        if depth > cap {
            throw CoreError("nesting exceeds \(cap)", status: 4)
        }
        while true {
            let tag = try readTag()
            let fnum = tag >> 3
            let wt = UInt8(tag & 7)
            if fnum == 0 || wt > wtFixed32 {
                throw CoreError("bad tag \(tag)")
            }
            if wt == wtEndGroup {
                if fnum != number {
                    throw CoreError("mismatched end-group for \(fnum), want \(number)")
                }
                return
            }
            if wt == wtStartGroup {
                try skipGroup(fnum, depth + 1)
            } else {
                try skip(wt)
            }
        }
    }

    /// Skips one unknown field: groups validate to their matching end tag,
    /// anything else only checks value shape. End-group tags never reach here;
    /// the decode loop owns them.
    public mutating func skipUnknown(_ fnum: UInt32, _ wt: UInt8, _ depth: UInt32) throws {
        if wt == wtStartGroup {
            try skipGroup(fnum, depth + 1)
        } else {
            try skip(wt)
        }
    }
}

/// Structural UTF-8 check: rejects overlong forms, surrogates and values past
/// U+10FFFF, like the core's `pbw_utf8_valid` and the reference parsers.
public func utf8Valid(_ b: [UInt8], from start: Int, count: Int) -> Bool {
    var i = start
    let end = start + count
    while i < end {
        let c = b[i]
        if c < 0x80 {
            i += 1
        } else if c & 0xe0 == 0xc0 {
            if c < 0xc2 || i + 1 >= end || b[i + 1] & 0xc0 != 0x80 { return false }
            i += 2
        } else if c & 0xf0 == 0xe0 {
            if i + 2 >= end || b[i + 1] & 0xc0 != 0x80 || b[i + 2] & 0xc0 != 0x80 {
                return false
            }
            let v = (UInt32(c & 15) << 12) | (UInt32(b[i + 1] & 63) << 6)
                | UInt32(b[i + 2] & 63)
            if v < 0x800 || (0xd800...0xdfff).contains(v) { return false }
            i += 3
        } else if c & 0xf8 == 0xf0 {
            if c > 0xf4 || i + 3 >= end || b[i + 1] & 0xc0 != 0x80
                || b[i + 2] & 0xc0 != 0x80 || b[i + 3] & 0xc0 != 0x80
            {
                return false
            }
            let v = (UInt32(c & 7) << 18) | (UInt32(b[i + 1] & 63) << 12)
                | (UInt32(b[i + 2] & 63) << 6) | UInt32(b[i + 3] & 63)
            if v < 0x10000 || v > 0x10ffff { return false }
            i += 4
        } else {
            return false
        }
    }
    return true
}

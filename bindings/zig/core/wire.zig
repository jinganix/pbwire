//! Protobuf wire primitives for the pure-Zig core.
//!
//! Varint/tag/little-endian read and write, zigzag, skipping (with group
//! frames) and the structural UTF-8 check. One home for everything the
//! descriptor reader, the encoder and the decoder do to bytes, so they cannot
//! diverge. This is the Zig twin of `bindings/rust/src/core/wire.rs`.
//!
//! ReleaseFast wraps arithmetic silently, so every offset and length addition
//! here is either guarded by a subtraction or checked explicitly.

const std = @import("std");

pub const WT_VARINT: u8 = 0;
pub const WT_FIXED64: u8 = 1;
pub const WT_LEN: u8 = 2;
pub const WT_START_GROUP: u8 = 3;
pub const WT_END_GROUP: u8 = 4;
pub const WT_FIXED32: u8 = 5;

/// Nesting cap, the same bound as the C core's `PBW_MAX_DEPTH`.
pub const MAX_DEPTH: u32 = 100;

pub const Error = error{
    InvalidData,
    OutOfMemory,
    LimitExceeded,
    RequiredMissing,
    NotFound,
    WrongType,
    BadSlice,
};

/// A precomputed tag: field number and wire type as their varint bytes.
pub const Tag = struct {
    bytes: [5]u8 = [_]u8{ 0, 0, 0, 0, 0 },
    len: u8 = 0,

    pub fn new(fnum: u32, wt: u8) Tag {
        var tag: u64 = @as(u64, fnum) * 8 + @as(u64, wt);
        var out = Tag{};
        var n: usize = 0;
        while (tag > 0x7f) {
            out.bytes[n] = @as(u8, @truncate(tag & 0x7f)) | 0x80;
            n += 1;
            tag >>= 7;
        }
        out.bytes[n] = @truncate(tag);
        out.len = @intCast(n + 1);
        return out;
    }

    pub fn slice(self: *const Tag) []const u8 {
        return self.bytes[0..self.len];
    }
};

pub fn tagBytes(fnum: u32, wt: u8) Tag {
    return Tag.new(fnum, wt);
}

pub fn sizeVar32(v: u64) usize {
    var n: usize = 1;
    var x = v;
    while (x > 0x7f) {
        x >>= 7;
        n += 1;
    }
    return n;
}

pub fn zig32(v: i32) u32 {
    return @bitCast((v << 1) ^ (v >> 31));
}

pub fn zig64(v: i64) u64 {
    return @bitCast((v << 1) ^ (v >> 63));
}

pub fn unzig32(v: u32) i32 {
    return @as(i32, @bitCast(v >> 1)) ^ -@as(i32, @intCast(v & 1));
}

pub fn unzig64(v: u64) i64 {
    return @as(i64, @bitCast(v >> 1)) ^ -@as(i64, @intCast(v & 1));
}

/// Cursor over an input slice. A nested payload consumes exactly its slice,
/// so positions never need saving.
pub const Reader = struct {
    b: []const u8,
    pos: usize,
    end: usize,
    cap: u32,

    pub fn init(b: []const u8) Reader {
        return .{ .b = b, .pos = 0, .end = b.len, .cap = MAX_DEPTH };
    }

    pub fn sliceRange(b: []const u8, off: usize, end: usize) !Reader {
        if (off > end or end > b.len) return Error.BadSlice;
        return .{ .b = b, .pos = off, .end = end, .cap = MAX_DEPTH };
    }

    /// varint, low 32 bits (unsigned); high bits discarded like a C uint32.
    pub fn rVar32(self: *Reader) !u32 {
        var p = self.pos;
        const end = self.end;
        var lo: u64 = 0;
        var j: u32 = 0;
        while (true) {
            if (p >= end) return Error.InvalidData;
            const c = self.b[p];
            p += 1;
            if (j < 5) lo |= @as(u64, c & 0x7f) << @intCast(j * 7);
            if (c & 0x80 == 0) {
                self.pos = p;
                return @truncate(lo);
            }
            j += 1;
            if (j >= 10) return Error.InvalidData;
        }
    }

    /// varint, full 64 bits (unsigned).
    pub fn rVar64(self: *Reader) !u64 {
        var p = self.pos;
        const end = self.end;
        var v: u64 = 0;
        var shift: u6 = 0;
        while (true) {
            if (p >= end) return Error.InvalidData;
            const c = self.b[p];
            p += 1;
            v |= @as(u64, c & 0x7f) << shift;
            if (c & 0x80 == 0) {
                self.pos = p;
                return v;
            }
            if (shift >= 63) return Error.InvalidData;
            shift += 7;
        }
    }

    pub fn rVarBool(self: *Reader) !bool {
        var p = self.pos;
        const end = self.end;
        var nz: u8 = 0;
        var j: u32 = 0;
        while (true) {
            if (p >= end) return Error.InvalidData;
            const c = self.b[p];
            p += 1;
            nz |= c & 0x7f;
            if (c & 0x80 == 0) {
                self.pos = p;
                return nz != 0;
            }
            j += 1;
            if (j >= 10) return Error.InvalidData;
        }
    }

    /// Strict length read, like the core's: at most five bytes, the fifth only
    /// carries four bits; an out-of-range length is rejected, not truncated.
    pub fn rLen32(self: *Reader) !usize {
        var p = self.pos;
        const end = self.end;
        if (p >= end) return Error.InvalidData;
        var c = self.b[p];
        p += 1;
        var v: u64 = c & 0x7f;
        var shift: u32 = 7;
        while (c & 0x80 != 0) {
            if (shift >= 35 or p >= end) return Error.InvalidData;
            c = self.b[p];
            p += 1;
            if (shift == 28 and (c & 0x7f) > 0x0f) return Error.InvalidData;
            v |= @as(u64, c & 0x7f) << @intCast(shift);
            shift += 7;
        }
        const n: usize = @intCast(v);
        if (n > end - p) return Error.InvalidData;
        self.pos = p;
        return n;
    }

    /// Loose length read for descriptor parsing: any varint up to 2^31-1.
    pub fn rLen(self: *Reader) !usize {
        const v = try self.rVar64();
        if (v > 0x7fff_ffff) return Error.InvalidData;
        const n: usize = @intCast(v);
        if (n > self.end - self.pos) return Error.InvalidData;
        return n;
    }

    /// Strict tag read: a varint32 of at most five bytes. A 5-byte tag above
    /// UINT32_MAX truncates instead of failing, exactly like the reference.
    pub fn rTag(self: *Reader) !u32 {
        var p = self.pos;
        const end = self.end;
        if (p >= end) return Error.InvalidData;
        var c = self.b[p];
        p += 1;
        var v: u64 = c & 0x7f;
        var shift: u32 = 7;
        while (c & 0x80 != 0) {
            if (shift >= 35 or p >= end) return Error.InvalidData;
            c = self.b[p];
            p += 1;
            v |= @as(u64, c & 0x7f) << @intCast(shift);
            shift += 7;
        }
        self.pos = p;
        return @truncate(v);
    }

    pub fn rFixed32(self: *Reader) !u32 {
        if (self.end - self.pos < 4) return Error.InvalidData;
        const p = self.pos;
        self.pos = p + 4;
        return std.mem.readInt(u32, self.b[p..][0..4], .little);
    }

    pub fn rFixed64(self: *Reader) !u64 {
        if (self.end - self.pos < 8) return Error.InvalidData;
        const p = self.pos;
        self.pos = p + 8;
        return std.mem.readInt(u64, self.b[p..][0..8], .little);
    }

    /// Skips one field value of a non-group wire type.
    pub fn skip(self: *Reader, wt: u8) !void {
        switch (wt) {
            WT_VARINT => {
                _ = try self.rVarBool();
            },
            WT_FIXED64 => {
                if (self.end - self.pos < 8) return Error.InvalidData;
                self.pos += 8;
            },
            WT_LEN => {
                const n = try self.rLen32();
                self.pos += n;
            },
            WT_FIXED32 => {
                if (self.end - self.pos < 4) return Error.InvalidData;
                self.pos += 4;
            },
            else => return Error.InvalidData,
        }
    }

    /// Validates a group body up to its matching end tag.
    pub fn skipGroup(self: *Reader, number: u32, depth: u32) !void {
        if (depth > self.cap) return Error.LimitExceeded;
        while (true) {
            const tag = try self.rTag();
            const fnum = tag >> 3;
            const wt: u8 = @truncate(tag & 7);
            if (fnum == 0 or wt > WT_FIXED32) return Error.InvalidData;
            if (wt == WT_END_GROUP) {
                if (fnum != number) return Error.InvalidData;
                return;
            }
            if (wt == WT_START_GROUP) {
                try self.skipGroup(fnum, depth + 1);
            } else {
                try self.skip(wt);
            }
        }
    }

    pub fn skipUnknown(self: *Reader, fnum: u32, wt: u8, depth: u32) !void {
        if (wt == WT_START_GROUP) return self.skipGroup(fnum, depth + 1);
        return self.skip(wt);
    }
};

/// Structural UTF-8 check: rejects overlong forms, surrogates and values past
/// U+10FFFF, like the core's `pbw_utf8_valid`.
pub fn utf8Valid(b: []const u8) bool {
    var i: usize = 0;
    const end = b.len;
    while (i < end) {
        const c = b[i];
        if (c < 0x80) {
            i += 1;
        } else if (c & 0xe0 == 0xc0) {
            if (c < 0xc2 or i + 1 >= end or b[i + 1] & 0xc0 != 0x80) return false;
            i += 2;
        } else if (c & 0xf0 == 0xe0) {
            if (i + 2 >= end or b[i + 1] & 0xc0 != 0x80 or b[i + 2] & 0xc0 != 0x80) return false;
            const v = (@as(u32, c & 15) << 12) |
                (@as(u32, b[i + 1] & 63) << 6) |
                @as(u32, b[i + 2] & 63);
            if (v < 0x800 or (v >= 0xd800 and v <= 0xdfff)) return false;
            i += 3;
        } else if (c & 0xf8 == 0xf0) {
            if (c > 0xf4 or i + 3 >= end or b[i + 1] & 0xc0 != 0x80 or
                b[i + 2] & 0xc0 != 0x80 or b[i + 3] & 0xc0 != 0x80) return false;
            const v = (@as(u32, c & 7) << 18) |
                (@as(u32, b[i + 1] & 63) << 12) |
                (@as(u32, b[i + 2] & 63) << 6) |
                @as(u32, b[i + 3] & 63);
            if (v < 0x10000 or v > 0x10ffff) return false;
            i += 4;
        } else {
            return false;
        }
    }
    return true;
}

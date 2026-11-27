//! Protobuf wire primitives for the pure-Rust core.
//!
//! Varint/tag/little-endian read and write, zigzag, skipping (with group
//! frames) and the structural UTF-8 check. One home for everything the
//! descriptor reader, the encoder and the decoder do to bytes, so they cannot
//! diverge.
//!
//! The rules are the C core's (`src/wire.c`, `src/decode.c`): strict tags and
//! lengths, unknown fields skippable by shape, groups validated to their
//! matching end tag, one nesting cap for groups and messages alike. This is the
//! twin of `bindings/python/pure/wire.py` and `bindings/node/lib/core/wire.ts`.

use std::fmt;

pub const WT_VARINT: u8 = 0;
pub const WT_FIXED64: u8 = 1;
pub const WT_LEN: u8 = 2;
pub const WT_START_GROUP: u8 = 3;
pub const WT_END_GROUP: u8 = 4;
pub const WT_FIXED32: u8 = 5;

/// Nesting cap, the same bound as the C core's `PBW_MAX_DEPTH`.
pub const MAX_DEPTH: u32 = 100;

/// A pbwire failure: the C core's `pbw_status` where one exists, else -1.
#[derive(Debug, Clone)]
pub struct CoreError {
    message: String,
    status: i32,
}

impl CoreError {
    pub fn new(message: impl Into<String>) -> CoreError {
        CoreError { message: message.into(), status: -1 }
    }

    pub fn with_status(message: impl Into<String>, status: i32) -> CoreError {
        CoreError { message: message.into(), status }
    }

    pub fn message(&self) -> &str {
        &self.message
    }

    pub fn status(&self) -> i32 {
        self.status
    }
}

impl fmt::Display for CoreError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "pbwire: {}", self.message)
    }
}

impl std::error::Error for CoreError {}

pub type Result<T> = std::result::Result<T, CoreError>;

pub(crate) fn fail<T>(message: impl Into<String>) -> Result<T> {
    Err(CoreError::new(message))
}

pub(crate) fn fail_status<T>(message: impl Into<String>, status: i32) -> Result<T> {
    Err(CoreError::with_status(message, status))
}

/// A precomputed tag: field number and wire type as their varint bytes, so a
/// hot loop copies at most five bytes instead of re-encoding the number.
#[derive(Debug, Clone, Copy, Default)]
pub struct Tag {
    bytes: [u8; 5],
    len: u8,
}

impl Tag {
    pub fn new(fnum: u32, wt: u8) -> Tag {
        let mut tag = fnum as u64 * 8 + wt as u64;
        let mut bytes = [0u8; 5];
        let mut n = 0;
        while tag > 0x7f {
            bytes[n] = (tag & 0x7f) as u8 | 0x80;
            n += 1;
            tag >>= 7;
        }
        bytes[n] = tag as u8;
        Tag { bytes, len: (n + 1) as u8 }
    }

    pub fn as_slice(&self) -> &[u8] {
        &self.bytes[..self.len as usize]
    }
}

/// Bytes of the `(field-number, wire-type)` tag, the descriptor-free form.
pub fn tag_bytes(fnum: u32, wt: u8) -> Tag {
    Tag::new(fnum, wt)
}

// ------------------------------------------------------------------ writing

pub fn w_tag(w: &mut Vec<u8>, tag: &Tag) {
    w.extend_from_slice(tag.as_slice());
}

pub fn w_var32(w: &mut Vec<u8>, v: u32) {
    let mut x = v as u64;
    while x > 0x7f {
        w.push((x & 0x7f) as u8 | 0x80);
        x >>= 7;
    }
    w.push(x as u8);
}

pub fn w_var64(w: &mut Vec<u8>, v: u64) {
    let mut x = v;
    while x > 0x7f {
        w.push((x & 0x7f) as u8 | 0x80);
        x >>= 7;
    }
    w.push(x as u8);
}

pub fn w_fixed32(w: &mut Vec<u8>, v: u32) {
    w.extend_from_slice(&v.to_le_bytes());
}

pub fn w_fixed64(w: &mut Vec<u8>, v: u64) {
    w.extend_from_slice(&v.to_le_bytes());
}

pub fn w_len(w: &mut Vec<u8>, n: usize) {
    let mut x = n as u64;
    while x > 0x7f {
        w.push((x & 0x7f) as u8 | 0x80);
        x >>= 7;
    }
    w.push(x as u8);
}

/// Fused tag + varint32: one reserve per field.
pub fn w_tag_var32(w: &mut Vec<u8>, tag: &Tag, v: u32) {
    w.reserve(tag.as_slice().len() + 5);
    w_tag(w, tag);
    w_var32(w, v);
}

pub fn w_tag_var64(w: &mut Vec<u8>, tag: &Tag, v: u64) {
    w.reserve(tag.as_slice().len() + 10);
    w_tag(w, tag);
    w_var64(w, v);
}

pub fn w_tag_fixed32(w: &mut Vec<u8>, tag: &Tag, v: u32) {
    w.reserve(tag.as_slice().len() + 4);
    w_tag(w, tag);
    w_fixed32(w, v);
}

pub fn w_tag_fixed64(w: &mut Vec<u8>, tag: &Tag, v: u64) {
    w.reserve(tag.as_slice().len() + 8);
    w_tag(w, tag);
    w_fixed64(w, v);
}

/// Length-delimited payload under `tag` (string/bytes/message).
pub fn w_bytes(w: &mut Vec<u8>, tag: &Tag, v: &[u8]) {
    w.reserve(tag.as_slice().len() + 5 + v.len());
    w_tag(w, tag);
    w_len(w, v.len());
    w.extend_from_slice(v);
}

/// Patches a one-byte length placeholder written at `lp` with the body that now
/// follows it (shifting the body up when the length does not fit one byte).
pub fn patch_len(w: &mut Vec<u8>, lp: usize) {
    let length = w.len() - lp - 1;
    if length < 128 {
        w[lp] = length as u8;
        return;
    }
    let n = size_var32(length as u64);
    // Insert the extra (n - 1) length bytes at lp + 1, moving the body up.
    w.resize(w.len() + n - 1, 0);
    let end = w.len() - (n - 1);
    w.copy_within(lp + 1..end, lp + n);
    let mut p = lp;
    let mut x = length as u64;
    while x > 0x7f {
        w[p] = (x & 0x7f) as u8 | 0x80;
        p += 1;
        x >>= 7;
    }
    w[p] = x as u8;
}

pub fn size_var32(v: u64) -> usize {
    let mut n = 1;
    let mut x = v;
    while x > 0x7f {
        x >>= 7;
        n += 1;
    }
    n
}

pub fn zig32(v: i32) -> u32 {
    ((v << 1) ^ (v >> 31)) as u32
}

pub fn zig64(v: i64) -> u64 {
    ((v << 1) ^ (v >> 63)) as u64
}

pub fn unzig32(v: u32) -> i32 {
    ((v >> 1) as i32) ^ -((v & 1) as i32)
}

pub fn unzig64(v: u64) -> i64 {
    ((v >> 1) as i64) ^ -((v & 1) as i64)
}

// ------------------------------------------------------------------ reading

/// Cursor over an input slice. One reader threads through a whole decode, so a
/// nested payload consumes exactly its slice and positions never need saving.
pub struct Reader<'a> {
    pub b: &'a [u8],
    pub pos: usize,
    pub end: usize,
    /// Nesting cap for this decode; set from `Limits` at entry.
    pub cap: u32,
}

impl<'a> Reader<'a> {
    pub fn new(b: &'a [u8]) -> Reader<'a> {
        Reader { b, pos: 0, end: b.len(), cap: MAX_DEPTH }
    }

    pub fn slice(b: &'a [u8], off: usize, end: usize) -> Result<Reader<'a>> {
        if off > end || end > b.len() {
            return fail("descriptor overrun");
        }
        Ok(Reader { b, pos: off, end, cap: MAX_DEPTH })
    }

    /// varint, low 32 bits (unsigned). Validates the encoding's length only:
    /// the high bits are discarded on purpose, like the C core's `uint32_t`.
    pub fn r_var32(&mut self) -> Result<u32> {
        let b = self.b;
        let end = self.end;
        let mut p = self.pos;
        let mut lo: u64 = 0;
        let mut j = 0;
        loop {
            if p >= end {
                return fail("truncated varint");
            }
            let c = b[p];
            p += 1;
            if j < 5 {
                lo |= ((c & 0x7f) as u64) << (j * 7);
            }
            if c & 0x80 == 0 {
                self.pos = p;
                return Ok(lo as u32);
            }
            j += 1;
            if j >= 10 {
                return fail("varint overflow");
            }
        }
    }

    /// varint, full 64 bits (unsigned).
    pub fn r_var64(&mut self) -> Result<u64> {
        let b = self.b;
        let end = self.end;
        let mut p = self.pos;
        let mut v: u64 = 0;
        let mut shift = 0;
        loop {
            if p >= end {
                return fail("truncated varint");
            }
            let c = b[p];
            p += 1;
            v |= ((c & 0x7f) as u64) << shift;
            if c & 0x80 == 0 {
                self.pos = p;
                return Ok(v);
            }
            shift += 7;
            if shift >= 64 {
                return fail("varint overflow");
            }
        }
    }

    /// varint reduced to its bool value: any set payload bit means true.
    pub fn r_var_bool(&mut self) -> Result<bool> {
        let b = self.b;
        let end = self.end;
        let mut p = self.pos;
        let mut nz: u8 = 0;
        let mut j = 0;
        loop {
            if p >= end {
                return fail("truncated varint");
            }
            let c = b[p];
            p += 1;
            nz |= c & 0x7f;
            if c & 0x80 == 0 {
                self.pos = p;
                return Ok(nz != 0);
            }
            j += 1;
            if j >= 10 {
                return fail("varint overflow");
            }
        }
    }

    /// Strict length read, like the core's: at most five bytes, the fifth only
    /// carries four bits; an out-of-range length is rejected, not truncated.
    /// Leaves `pos` after the length.
    pub fn r_len32(&mut self) -> Result<usize> {
        let b = self.b;
        let end = self.end;
        let mut p = self.pos;
        if p >= end {
            return fail("truncated length");
        }
        let mut c = b[p];
        p += 1;
        let mut v: u64 = (c & 0x7f) as u64;
        let mut shift = 7;
        while c & 0x80 != 0 {
            if shift >= 35 || p >= end {
                return fail("bad length");
            }
            c = b[p];
            p += 1;
            if shift == 28 && (c & 0x7f) > 0x0f {
                return fail("length overflow");
            }
            v |= ((c & 0x7f) as u64) << shift;
            shift += 7;
        }
        if p + v as usize > end {
            return fail("truncated length-delimited");
        }
        self.pos = p;
        Ok(v as usize)
    }

    /// Loose length read for descriptor parsing: any varint up to 2^31-1,
    /// bounds checked.
    pub fn r_len(&mut self) -> Result<usize> {
        let v = self.r_var64()?;
        if v > 0x7fff_ffff {
            return fail("length overflow");
        }
        if self.pos + v as usize > self.end {
            return fail("truncated length-delimited");
        }
        Ok(v as usize)
    }

    /// Strict tag read: a varint32 of at most five bytes. A 5-byte tag above
    /// UINT32_MAX truncates instead of failing, exactly what the reference
    /// parser does (the official suite expects two such cases to parse).
    pub fn r_tag(&mut self) -> Result<u32> {
        let b = self.b;
        let end = self.end;
        let mut p = self.pos;
        if p >= end {
            return fail("truncated tag");
        }
        let mut c = b[p];
        p += 1;
        let mut v: u64 = (c & 0x7f) as u64;
        let mut shift = 7;
        while c & 0x80 != 0 {
            if shift >= 35 || p >= end {
                return fail("bad tag");
            }
            c = b[p];
            p += 1;
            v |= ((c & 0x7f) as u64) << shift;
            shift += 7;
        }
        self.pos = p;
        Ok(v as u32)
    }

    pub fn r_fixed32(&mut self) -> Result<u32> {
        if self.pos + 4 > self.end {
            return fail("truncated fixed32");
        }
        let p = self.pos;
        self.pos = p + 4;
        Ok(u32::from_le_bytes([self.b[p], self.b[p + 1], self.b[p + 2], self.b[p + 3]]))
    }

    pub fn r_fixed64(&mut self) -> Result<u64> {
        if self.pos + 8 > self.end {
            return fail("truncated fixed64");
        }
        let p = self.pos;
        self.pos = p + 8;
        let mut bytes = [0u8; 8];
        bytes.copy_from_slice(&self.b[p..p + 8]);
        Ok(u64::from_le_bytes(bytes))
    }

    /// Skips one field value of a non-group wire type.
    pub fn skip(&mut self, wt: u8) -> Result<()> {
        match wt {
            WT_VARINT => {
                self.r_var_bool()?;
                Ok(())
            }
            WT_FIXED64 => {
                if self.pos + 8 > self.end {
                    return fail("truncated fixed64");
                }
                self.pos += 8;
                Ok(())
            }
            WT_LEN => {
                let n = self.r_len32()?;
                self.pos += n;
                Ok(())
            }
            WT_FIXED32 => {
                if self.pos + 4 > self.end {
                    return fail("truncated fixed32");
                }
                self.pos += 4;
                Ok(())
            }
            _ => fail(format!("bad wire type {wt}")),
        }
    }

    /// Validates a group body up to its matching end tag, like the core's group
    /// skipper: every tag is strict, field 0 and wire types above 5 are
    /// rejected, and an end tag only closes the group it names.
    pub fn skip_group(&mut self, number: u32, depth: u32) -> Result<()> {
        if depth > self.cap {
            return fail_status(format!("nesting exceeds {}", self.cap), 4);
        }
        loop {
            let tag = self.r_tag()?;
            let fnum = tag >> 3;
            let wt = (tag & 7) as u8;
            if fnum == 0 || wt > WT_FIXED32 {
                return fail(format!("bad tag {tag}"));
            }
            if wt == WT_END_GROUP {
                if fnum != number {
                    return fail(format!("mismatched end-group for {fnum}, want {number}"));
                }
                return Ok(());
            }
            if wt == WT_START_GROUP {
                self.skip_group(fnum, depth + 1)?;
            } else {
                self.skip(wt)?;
            }
        }
    }

    /// Skips one unknown field: groups validate to their matching end tag,
    /// anything else only checks value shape. End-group tags never reach here;
    /// the decode loop owns them.
    pub fn skip_unknown(&mut self, fnum: u32, wt: u8, depth: u32) -> Result<()> {
        if wt == WT_START_GROUP {
            return self.skip_group(fnum, depth + 1);
        }
        self.skip(wt)
    }
}

/// Structural UTF-8 check: rejects overlong forms, surrogates and values past
/// U+10FFFF, like the core's `pbw_utf8_valid` and the reference parsers.
pub fn utf8_valid(b: &[u8]) -> bool {
    let mut i = 0;
    let end = b.len();
    while i < end {
        let c = b[i];
        if c < 0x80 {
            i += 1;
        } else if c & 0xe0 == 0xc0 {
            if c < 0xc2 || i + 1 >= end || b[i + 1] & 0xc0 != 0x80 {
                return false;
            }
            i += 2;
        } else if c & 0xf0 == 0xe0 {
            if i + 2 >= end || b[i + 1] & 0xc0 != 0x80 || b[i + 2] & 0xc0 != 0x80 {
                return false;
            }
            let v = (((c & 15) as u32) << 12)
                | (((b[i + 1] & 63) as u32) << 6)
                | ((b[i + 2] & 63) as u32);
            if v < 0x800 || (0xd800..=0xdfff).contains(&v) {
                return false;
            }
            i += 3;
        } else if c & 0xf8 == 0xf0 {
            if c > 0xf4
                || i + 3 >= end
                || b[i + 1] & 0xc0 != 0x80
                || b[i + 2] & 0xc0 != 0x80
                || b[i + 3] & 0xc0 != 0x80
            {
                return false;
            }
            let v = (((c & 7) as u32) << 18)
                | (((b[i + 1] & 63) as u32) << 12)
                | (((b[i + 2] & 63) as u32) << 6)
                | ((b[i + 3] & 63) as u32);
            if !(0x10000..=0x10ffff).contains(&v) {
                return false;
            }
            i += 4;
        } else {
            return false;
        }
    }
    true
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn zigzag_round_trip() {
        for v in [0i32, 1, -1, 2, -2, i32::MAX, i32::MIN, 12345, -12345] {
            assert_eq!(unzig32(zig32(v)), v);
        }
        for v in [0i64, 1, -1, i64::MAX, i64::MIN, 1 << 40, -(1 << 40)] {
            assert_eq!(unzig64(zig64(v)), v);
        }
        assert_eq!(zig32(-1), 1);
        assert_eq!(zig32(i32::MIN), 0xffff_ffff);
        assert_eq!(zig64(i64::MIN), u64::MAX);
    }

    #[test]
    fn varint_edges() {
        let mut w = Vec::new();
        w_var64(&mut w, u64::MAX);
        assert_eq!(w.len(), 10);
        let mut r = Reader::new(&w);
        assert_eq!(r.r_var64().unwrap(), u64::MAX);
        let mut w = Vec::new();
        w_var64(&mut w, i64::MIN as u64);
        assert_eq!(w.len(), 10);
        let mut r = Reader::new(&w);
        assert_eq!(r.r_var64().unwrap(), i64::MIN as u64);
    }

    #[test]
    fn tag_truncates_above_u32() {
        // 5-byte tag whose value is past UINT32_MAX: truncates, like the
        // reference reader.
        let bytes = [0xff, 0xff, 0xff, 0xff, 0x7f];
        let mut r = Reader::new(&bytes);
        assert_eq!(r.r_tag().unwrap(), u32::MAX as u32);
        let bytes = [0xff, 0xff, 0xff, 0xff, 0xff, 0x01];
        let mut r = Reader::new(&bytes);
        assert!(r.r_tag().is_err());
    }

    #[test]
    fn utf8_rejects_surrogates_and_overlongs() {
        assert!(utf8_valid(b"hello"));
        assert!(utf8_valid("héllo → 😀".as_bytes()));
        assert!(!utf8_valid(&[0xed, 0xa0, 0x80])); // surrogate
        assert!(!utf8_valid(&[0xc0, 0x80])); // overlong
        assert!(!utf8_valid(&[0xf5, 0x80, 0x80, 0x80])); // > U+10FFFF
        assert!(!utf8_valid(&[0x80]));
    }

    #[test]
    fn patch_len_shifts() {
        let mut w: Vec<u8> = vec![0x08];
        let lp = w.len();
        w.push(0);
        for _ in 0..200 {
            w.push(0x41);
        }
        patch_len(&mut w, lp);
        // 1 tag byte + 2 length bytes + 200 body bytes.
        assert_eq!(w.len(), 1 + 2 + 200);
        assert_eq!(&w[1..3], &[0xc8, 0x01]);
        assert_eq!(w[3], 0x41);
    }
}

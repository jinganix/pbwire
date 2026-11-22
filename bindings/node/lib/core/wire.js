"use strict";
/*
 * Protobuf wire primitives for the pure-TypeScript core: varint/tag/little-
 * endian read and write, zigzag, skipping (with group frames) and the
 * structural UTF-8 check. One home for everything the descriptor reader,
 * the encoder and the decoder do to bytes, so they cannot diverge.
 *
 * The rules are the C core's (src/wire.c, src/decode.c): strict tags and
 * lengths, unknown fields skippable by shape, groups validated to their
 * matching end tag, one nesting cap for groups and messages alike.
 */
Object.defineProperty(exports, "__esModule", { value: true });
exports.R = exports.W = exports.CoreError = exports.MAX_DEPTH = exports.WT_FIXED32 = exports.WT_END_GROUP = exports.WT_START_GROUP = exports.WT_LEN = exports.WT_FIXED64 = exports.WT_VARINT = void 0;
exports.ensure = ensure;
exports.tagBytes = tagBytes;
exports.wTag = wTag;
exports.wVar32 = wVar32;
exports.wVar64 = wVar64;
exports.wFixed32 = wFixed32;
exports.wFixed64 = wFixed64;
exports.wTagVar32 = wTagVar32;
exports.wTagVar64 = wTagVar64;
exports.wTagFixed32 = wTagFixed32;
exports.wTagFixed64 = wTagFixed64;
exports.wBytes = wBytes;
exports.patchLen = patchLen;
exports.sizeVar32 = sizeVar32;
exports.sizeVar64 = sizeVar64;
exports.sizeInt32 = sizeInt32;
exports.zig32 = zig32;
exports.zig64 = zig64;
exports.unzig32 = unzig32;
exports.unzig64 = unzig64;
exports.rVar = rVar;
exports.rVar64 = rVar64;
exports.rVarBool = rVarBool;
exports.rLen32 = rLen32;
exports.rLen = rLen;
exports.rTag = rTag;
exports.rFixed32 = rFixed32;
exports.rFixed64 = rFixed64;
exports.skip = skip;
exports.skipGroup = skipGroup;
exports.skipUnknown = skipUnknown;
exports.utf8Valid = utf8Valid;
exports.utf8Length = utf8Length;
exports.utf8Write = utf8Write;
exports.utf8Decode = utf8Decode;
exports.floatBits = floatBits;
exports.bitsFloat = bitsFloat;
exports.doubleBits = doubleBits;
exports.bitsDouble = bitsDouble;
exports.WT_VARINT = 0;
exports.WT_FIXED64 = 1;
exports.WT_LEN = 2;
exports.WT_START_GROUP = 3;
exports.WT_END_GROUP = 4;
exports.WT_FIXED32 = 5;
/** Nesting cap, the same bound as the C core's PBW_MAX_DEPTH. */
exports.MAX_DEPTH = 256;
/** A pbwire failure: the C core's pbw_status where one exists, else -1. */
class CoreError extends Error {
    constructor(message, status = -1) {
        super(message);
        this.name = 'CoreError';
        this.status = status;
    }
}
exports.CoreError = CoreError;
/** Growable output buffer, reset per encode (the C core's pbw_ctx twin). */
class W {
    constructor() {
        this.b = new Uint8Array(256);
        this.pos = 0;
    }
    reset() {
        this.pos = 0;
    }
}
exports.W = W;
/** Cursor over an input slice. */
class R {
    constructor(b, pos = 0, end = b.length) {
        /** Nesting cap for this decode; set from Limits at entry. */
        this.cap = exports.MAX_DEPTH;
        this.b = b;
        this.pos = pos;
        this.end = end;
    }
}
exports.R = R;
function ensure(w, extra) {
    if (w.pos + extra > w.b.length) {
        grow(w, extra);
    }
}
function grow(w, extra) {
    let n = w.b.length * 2;
    const need = w.pos + extra;
    while (n < need) {
        n *= 2;
    }
    const next = new Uint8Array(n);
    next.set(w.b.subarray(0, w.pos));
    w.b = next;
}
/** The (field-number, wire-type) tag as bytes. */
function tagBytes(fnum, wt) {
    let tag = fnum * 8 + wt;
    const out = new Uint8Array(5);
    let n = 0;
    while (tag > 0x7f) {
        out[n++] = (tag & 0x7f) | 0x80;
        tag = Math.floor(tag / 128);
    }
    out[n++] = tag;
    return out.subarray(0, n);
}
function wTag(w, tag) {
    ensure(w, tag.length);
    w.b.set(tag, w.pos);
    w.pos += tag.length;
}
/** varint32 write of an unsigned 32-bit value. */
function wVar32(w, v) {
    ensure(w, 5);
    const b = w.b;
    let p = w.pos;
    let x = v >>> 0;
    while (x > 0x7f) {
        b[p++] = (x & 0x7f) | 0x80;
        x >>>= 7;
    }
    b[p++] = x;
    w.pos = p;
}
/** varint64 write of an unsigned 64-bit value. */
function wVar64(w, v) {
    ensure(w, 10);
    const b = w.b;
    let p = w.pos;
    let x = BigInt.asUintN(64, v);
    while (x > 0x7fn) {
        b[p++] = Number(x & 0x7fn) | 0x80;
        x >>= 7n;
    }
    b[p++] = Number(x);
    w.pos = p;
}
function wFixed32(w, v) {
    ensure(w, 4);
    const b = w.b;
    let p = w.pos;
    b[p++] = v & 0xff;
    b[p++] = (v >>> 8) & 0xff;
    b[p++] = (v >>> 16) & 0xff;
    b[p++] = (v >>> 24) & 0xff;
    w.pos = p;
}
function wFixed64(w, v) {
    ensure(w, 8);
    const b = w.b;
    let p = w.pos;
    let x = BigInt.asUintN(64, v);
    for (let i = 0; i < 8; i++) {
        b[p++] = Number(x & 0xffn);
        x >>= 8n;
    }
    w.pos = p;
}
/** Fused tag+varint32: one capacity check per field. */
function wTagVar32(w, tag, v) {
    ensure(w, tag.length + 5);
    const b = w.b;
    let p = w.pos;
    for (let i = 0; i < tag.length; i++) {
        b[p++] = tag[i];
    }
    let x = v >>> 0;
    while (x > 0x7f) {
        b[p++] = (x & 0x7f) | 0x80;
        x >>>= 7;
    }
    b[p++] = x;
    w.pos = p;
}
function wTagVar64(w, tag, v) {
    ensure(w, tag.length + 10);
    const b = w.b;
    let p = w.pos;
    for (let i = 0; i < tag.length; i++) {
        b[p++] = tag[i];
    }
    let x = BigInt.asUintN(64, v);
    while (x > 0x7fn) {
        b[p++] = Number(x & 0x7fn) | 0x80;
        x >>= 7n;
    }
    b[p++] = Number(x);
    w.pos = p;
}
function wTagFixed32(w, tag, v) {
    ensure(w, tag.length + 4);
    const b = w.b;
    let p = w.pos;
    for (let i = 0; i < tag.length; i++) {
        b[p++] = tag[i];
    }
    b[p++] = v & 0xff;
    b[p++] = (v >>> 8) & 0xff;
    b[p++] = (v >>> 16) & 0xff;
    b[p++] = (v >>> 24) & 0xff;
    w.pos = p;
}
function wTagFixed64(w, tag, v) {
    ensure(w, tag.length + 8);
    const b = w.b;
    let p = w.pos;
    for (let i = 0; i < tag.length; i++) {
        b[p++] = tag[i];
    }
    let x = BigInt.asUintN(64, v);
    for (let i = 0; i < 8; i++) {
        b[p++] = Number(x & 0xffn);
        x >>= 8n;
    }
    w.pos = p;
}
/** Length-delimited payload under `tag` (string/bytes/message). */
function wBytes(w, tag, v) {
    ensure(w, (tag === null ? 0 : tag.length) + 5 + v.length);
    const b = w.b;
    let p = w.pos;
    if (tag !== null) {
        for (let i = 0; i < tag.length; i++) {
            b[p++] = tag[i];
        }
    }
    let n = v.length;
    while (n > 0x7f) {
        b[p++] = (n & 0x7f) | 0x80;
        n >>>= 7;
    }
    b[p++] = n;
    b.set(v, p);
    w.pos = p + v.length;
}
/**
 * Fills a 1-byte length placeholder written at `lp`: the single-byte fast
 * path, or a memmove when the length needs more bytes.
 */
function patchLen(w, lp) {
    const len = w.pos - lp - 1;
    if (len < 128) {
        w.b[lp] = len;
        return;
    }
    const lb = new Uint8Array(5);
    let n = 0;
    let v = len;
    while (v > 0x7f) {
        lb[n++] = (v & 0x7f) | 0x80;
        v >>>= 7;
    }
    lb[n++] = v;
    ensure(w, n - 1);
    w.b.copyWithin(lp + n, lp + 1, w.pos);
    w.b.set(lb.subarray(0, n), lp);
    w.pos += n - 1;
}
function sizeVar32(v) {
    let n = 1;
    while (v > 0x7f) {
        v >>>= 7;
        n++;
    }
    return n;
}
/** varint64 of an unsigned value. */
function sizeVar64(v) {
    let n = 1;
    let x = BigInt.asUintN(64, v);
    while (x > 0x7fn) {
        x >>= 7n;
        n++;
    }
    return n;
}
/** int32: negatives sign-extend to 10 bytes, same as libprotobuf. */
function sizeInt32(v) {
    return v >= 0 ? sizeVar32(v) : 10;
}
function zig32(v) {
    return ((v << 1) ^ (v >> 31)) >>> 0;
}
function zig64(v) {
    return BigInt.asUintN(64, (v << 1n) ^ (v >> 63n));
}
function unzig32(v) {
    return (v >>> 1) ^ -(v & 1);
}
function unzig64(v) {
    return BigInt.asIntN(64, (v >> 1n) ^ -(v & 1n));
}
/* ---------------- reading ---------------- */
/** varint, low 32 bits (unsigned). Validates the encoding's length only. */
function rVar(r) {
    const b = r.b;
    const end = r.end;
    let p = r.pos;
    let lo = 0;
    let j = 0;
    for (;;) {
        if (p >= end) {
            throw new CoreError('truncated varint');
        }
        const c = b[p++];
        if (j < 5) {
            // The 32-bit shift discards the high bits on purpose: only the
            // low 32 are ever visible for the 32-bit kinds.
            lo |= (c & 0x7f) << (j * 7);
        }
        if ((c & 0x80) === 0) {
            r.pos = p;
            return lo >>> 0;
        }
        j++;
        if (j >= 10) {
            throw new CoreError('varint overflow');
        }
    }
}
/** varint, full 64 bits (unsigned). */
function rVar64(r) {
    const b = r.b;
    const end = r.end;
    let p = r.pos;
    let v = 0n;
    let shift = 0n;
    for (;;) {
        if (p >= end) {
            throw new CoreError('truncated varint');
        }
        const c = b[p++];
        v |= BigInt(c & 0x7f) << shift;
        if ((c & 0x80) === 0) {
            r.pos = p;
            return BigInt.asUintN(64, v);
        }
        shift += 7n;
        if (shift >= 64n) {
            throw new CoreError('varint overflow');
        }
    }
}
/** varint, reduced to its bool value: any set payload bit means true. */
function rVarBool(r) {
    const b = r.b;
    const end = r.end;
    let p = r.pos;
    let nz = 0;
    let j = 0;
    for (;;) {
        if (p >= end) {
            throw new CoreError('truncated varint');
        }
        const c = b[p++];
        nz |= c & 0x7f;
        if ((c & 0x80) === 0) {
            r.pos = p;
            return nz !== 0 ? 1 : 0;
        }
        j++;
        if (j >= 10) {
            throw new CoreError('varint overflow');
        }
    }
}
/**
 * Strict length read, like the core's length reader: at most five bytes and
 * the fifth only carries four bits; an out-of-range length is rejected, not
 * truncated. Leaves r.pos after the length.
 */
function rLen32(r) {
    const b = r.b;
    const end = r.end;
    let p = r.pos;
    if (p >= end) {
        throw new CoreError('truncated length');
    }
    let c = b[p++];
    let v = c & 0x7f;
    let shift = 7;
    while ((c & 0x80) !== 0) {
        if (shift >= 35 || p >= end) {
            throw new CoreError('bad length');
        }
        c = b[p++];
        if (shift === 28 && (c & 0x7f) > 0x0f) {
            throw new CoreError('length overflow');
        }
        v |= (c & 0x7f) << shift;
        shift += 7;
    }
    if (v < 0 || p + v > end) {
        throw new CoreError('truncated length-delimited');
    }
    r.pos = p;
    return v;
}
/**
 * Loose length read for descriptor parsing: any varint up to 2^31-1, bounds
 * checked.
 */
function rLen(r) {
    const v = rVar64(r);
    if (v > 0x7fffffffn) {
        throw new CoreError('length overflow');
    }
    const n = Number(v);
    if (r.pos + n > r.end) {
        throw new CoreError('truncated length-delimited');
    }
    return n;
}
/**
 * Strict tag read, like the core's tag reader: a varint32 of at most five
 * bytes. A 5-byte tag above UINT32_MAX truncates instead of failing, exactly
 * what the reference parser does (the official suite expects two such cases
 * to parse).
 */
function rTag(r) {
    const b = r.b;
    const end = r.end;
    let p = r.pos;
    if (p >= end) {
        throw new CoreError('truncated tag');
    }
    let c = b[p++];
    let v = c & 0x7f;
    let shift = 7;
    while ((c & 0x80) !== 0) {
        if (shift >= 35 || p >= end) {
            throw new CoreError('bad tag');
        }
        c = b[p++];
        v |= (c & 0x7f) << shift; // 32-bit: truncates like the reference
        shift += 7;
    }
    r.pos = p;
    return v >>> 0;
}
function rFixed32(r) {
    if (r.pos + 4 > r.end) {
        throw new CoreError('truncated fixed32');
    }
    const b = r.b;
    const p = r.pos;
    r.pos = p + 4;
    return (b[p] | (b[p + 1] << 8) | (b[p + 2] << 16) | (b[p + 3] << 24)) >>> 0;
}
/** Raw 64 bits, unsigned. */
function rFixed64(r) {
    if (r.pos + 8 > r.end) {
        throw new CoreError('truncated fixed64');
    }
    const b = r.b;
    const p = r.pos;
    r.pos = p + 8;
    const lo = (b[p] | (b[p + 1] << 8) | (b[p + 2] << 16) | (b[p + 3] << 24)) >>> 0;
    const hi = (b[p + 4] | (b[p + 5] << 8) | (b[p + 6] << 16) | (b[p + 7] << 24)) >>> 0;
    return BigInt(lo) | (BigInt(hi) << 32n);
}
/** Skips one field value of a non-group wire type. */
function skip(r, wt) {
    switch (wt) {
        case exports.WT_VARINT:
            rVarBool(r);
            return;
        case exports.WT_FIXED64:
            if (r.pos + 8 > r.end) {
                throw new CoreError('truncated fixed64');
            }
            r.pos += 8;
            return;
        case exports.WT_LEN: {
            const n = rLen32(r);
            r.pos += n;
            return;
        }
        case exports.WT_FIXED32:
            if (r.pos + 4 > r.end) {
                throw new CoreError('truncated fixed32');
            }
            r.pos += 4;
            return;
        default:
            throw new CoreError('bad wire type ' + wt);
    }
}
/**
 * Validates a group body up to its matching end tag, like the core's group
 * skipper: every tag is strict, field 0 and wire types above 5 are rejected,
 * and an end tag only closes the group it names.
 */
function skipGroup(r, number, depth) {
    if (depth > r.cap) {
        throw new CoreError('nesting exceeds ' + r.cap, 4);
    }
    for (;;) {
        const tag = rTag(r);
        const fnum = tag >>> 3;
        const wt = tag & 7;
        if (fnum === 0 || wt > exports.WT_FIXED32) {
            throw new CoreError('bad tag ' + tag);
        }
        if (wt === exports.WT_END_GROUP) {
            if (fnum !== number) {
                throw new CoreError('mismatched end-group for ' + fnum + ', want ' + number);
            }
            return;
        }
        if (wt === exports.WT_START_GROUP) {
            skipGroup(r, fnum, depth + 1);
        }
        else {
            skip(r, wt);
        }
    }
}
/**
 * Skips one unknown field: groups validate to their matching end tag,
 * anything else only checks value shape. End-group tags never reach here;
 * the decode loop owns them.
 */
function skipUnknown(r, fnum, wt, depth) {
    if (wt === exports.WT_START_GROUP) {
        skipGroup(r, fnum, depth + 1);
        return;
    }
    skip(r, wt);
}
/**
 * Structural UTF-8 check: rejects overlong forms, surrogates and values past
 * U+10FFFF, like the core's pbw_utf8_valid and the reference parsers.
 */
function utf8Valid(b, off, len) {
    let i = off;
    const end = off + len;
    while (i < end) {
        const c = b[i];
        if (c < 0x80) {
            i++;
        }
        else if ((c & 0xe0) === 0xc0) {
            if (c < 0xc2 || i + 1 >= end || (b[i + 1] & 0xc0) !== 0x80) {
                return false;
            }
            i += 2;
        }
        else if ((c & 0xf0) === 0xe0) {
            if (i + 2 >= end || (b[i + 1] & 0xc0) !== 0x80 || (b[i + 2] & 0xc0) !== 0x80) {
                return false;
            }
            const v = ((c & 15) << 12) | ((b[i + 1] & 63) << 6) | (b[i + 2] & 63);
            if (v < 0x800 || (v >= 0xd800 && v <= 0xdfff)) {
                return false;
            }
            i += 3;
        }
        else if ((c & 0xf8) === 0xf0) {
            if (c > 0xf4 ||
                i + 3 >= end ||
                (b[i + 1] & 0xc0) !== 0x80 ||
                (b[i + 2] & 0xc0) !== 0x80 ||
                (b[i + 3] & 0xc0) !== 0x80) {
                return false;
            }
            const v = ((c & 7) << 18) | ((b[i + 1] & 63) << 12) | ((b[i + 2] & 63) << 6) | (b[i + 3] & 63);
            if (v < 0x10000 || v > 0x10ffff) {
                return false;
            }
            i += 4;
        }
        else {
            return false;
        }
    }
    return true;
}
/** UTF-8 length of a JS string; lone surrogates count as U+FFFD. */
function utf8Length(s) {
    let n = 0;
    for (let i = 0; i < s.length; i++) {
        const c = s.charCodeAt(i);
        if (c < 0x80) {
            n += 1;
        }
        else if (c < 0x800) {
            n += 2;
        }
        else if (c >= 0xd800 && c <= 0xdbff && i + 1 < s.length) {
            const d = s.charCodeAt(i + 1);
            if (d >= 0xdc00 && d <= 0xdfff) {
                n += 4;
                i++;
            }
            else {
                n += 3; // lone high surrogate: U+FFFD
            }
        }
        else {
            n += 3;
        }
    }
    return n;
}
/** Writes `s` as UTF-8 at w.pos (capacity must already be ensured). */
function utf8Write(w, s) {
    const b = w.b;
    let p = w.pos;
    for (let i = 0; i < s.length; i++) {
        let c = s.charCodeAt(i);
        if (c < 0x80) {
            b[p++] = c;
        }
        else if (c < 0x800) {
            b[p++] = 0xc0 | (c >> 6);
            b[p++] = 0x80 | (c & 0x3f);
        }
        else if (c >= 0xd800 && c <= 0xdfff) {
            // A surrogate pair is one code point; a lone surrogate is not a
            // character and encodes as U+FFFD, like TextEncoder.
            const d = i + 1 < s.length ? s.charCodeAt(i + 1) : 0;
            if (c <= 0xdbff && d >= 0xdc00 && d <= 0xdfff) {
                c = 0x10000 + ((c - 0xd800) << 10) + (d - 0xdc00);
                i++;
                b[p++] = 0xf0 | (c >> 18);
                b[p++] = 0x80 | ((c >> 12) & 0x3f);
                b[p++] = 0x80 | ((c >> 6) & 0x3f);
                b[p++] = 0x80 | (c & 0x3f);
            }
            else {
                b[p++] = 0xef;
                b[p++] = 0xbf;
                b[p++] = 0xbd;
            }
        }
        else {
            b[p++] = 0xe0 | (c >> 12);
            b[p++] = 0x80 | ((c >> 6) & 0x3f);
            b[p++] = 0x80 | (c & 0x3f);
        }
    }
    w.pos = p;
}
/** UTF-8 decodes a slice, replacing each malformed byte with U+FFFD. */
function utf8Decode(b, off, len) {
    const end = off + len;
    let s = '';
    let i = off;
    while (i < end) {
        const c = b[i];
        if (c < 0x80) {
            // ASCII run: the common case, one slice per run. Chunked so a
            // 100 KB string never becomes 100k arguments to apply().
            const start = i;
            while (i < end && b[i] < 0x80) {
                i++;
            }
            for (let p = start; p < i; p += 4096) {
                const chunk = b.subarray(p, Math.min(p + 4096, i));
                s += String.fromCharCode.apply(null, chunk);
            }
        }
        else if ((c & 0xe0) === 0xc0 && i + 1 < end && (b[i + 1] & 0xc0) === 0x80) {
            s += String.fromCharCode(((c & 0x1f) << 6) | (b[i + 1] & 0x3f));
            i += 2;
        }
        else if ((c & 0xf0) === 0xe0 &&
            i + 2 < end &&
            (b[i + 1] & 0xc0) === 0x80 &&
            (b[i + 2] & 0xc0) === 0x80) {
            const v = ((c & 0x0f) << 12) | ((b[i + 1] & 0x3f) << 6) | (b[i + 2] & 0x3f);
            if (v < 0x800 || (v >= 0xd800 && v <= 0xdfff)) {
                s += '\ufffd'; // overlong or surrogate: one malformed unit
            }
            else {
                s += String.fromCharCode(v);
            }
            i += 3;
        }
        else if ((c & 0xf8) === 0xf0 &&
            i + 3 < end &&
            (b[i + 1] & 0xc0) === 0x80 &&
            (b[i + 2] & 0xc0) === 0x80 &&
            (b[i + 3] & 0xc0) === 0x80) {
            let v = ((c & 7) << 18) | ((b[i + 1] & 0x3f) << 12) | ((b[i + 2] & 0x3f) << 6) | (b[i + 3] & 0x3f);
            if (v < 0x10000 || v > 0x10ffff) {
                s += '\ufffd';
            }
            else {
                v -= 0x10000;
                s += String.fromCharCode(0xd800 + (v >> 10), 0xdc00 + (v & 0x3ff));
            }
            i += 4;
        }
        else {
            s += '\ufffd';
            i++;
        }
    }
    return s;
}
/* ---------------- float/double bit patterns ---------------- */
const f32Buf = new ArrayBuffer(4);
const f32Bits = new Uint32Array(f32Buf);
const f32Value = new Float32Array(f32Buf);
/** The float32 bit pattern of a JS number (IEEE round-to-nearest). */
function floatBits(v) {
    f32Value[0] = v;
    return f32Bits[0];
}
function bitsFloat(bits) {
    f32Bits[0] = bits;
    return f32Value[0];
}
const f64Buf = new ArrayBuffer(8);
const f64Bits = new Uint32Array(f64Buf);
const f64Value = new Float64Array(f64Buf);
/** The float64 bit pattern of a JS number, as raw 64 bits. */
function doubleBits(v) {
    f64Value[0] = v;
    return BigInt(f64Bits[0]) | (BigInt(f64Bits[1]) << 32n);
}
function bitsDouble(bits) {
    f64Bits[0] = Number(BigInt.asUintN(32, bits));
    f64Bits[1] = Number(BigInt.asUintN(32, bits >> 32n));
    return f64Value[0];
}

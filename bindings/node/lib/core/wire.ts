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

export const WT_VARINT = 0;
export const WT_FIXED64 = 1;
export const WT_LEN = 2;
export const WT_START_GROUP = 3;
export const WT_END_GROUP = 4;
export const WT_FIXED32 = 5;

/** Nesting cap, the same bound as the C core's PBW_MAX_DEPTH. */
export const MAX_DEPTH = 256;

/** A pbwire failure: the C core's pbw_status where one exists, else -1. */
export class CoreError extends Error {
    status: number;

    constructor(message: string, status = -1) {
        super(message);
        this.name = 'CoreError';
        this.status = status;
    }
}

/** Growable output buffer, reset per encode (the C core's pbw_ctx twin). */
export class W {
    b: Uint8Array = new Uint8Array(256);
    pos = 0;

    reset(): void {
        this.pos = 0;
    }
}

/** Cursor over an input slice. */
export class R {
    b: Uint8Array;
    pos: number;
    end: number;
    /** Nesting cap for this decode; set from Limits at entry. */
    cap: number = MAX_DEPTH;

    constructor(b: Uint8Array, pos = 0, end = b.length) {
        this.b = b;
        this.pos = pos;
        this.end = end;
    }
}

export function ensure(w: W, extra: number): void {
    if (w.pos + extra > w.b.length) {
        grow(w, extra);
    }
}

function grow(w: W, extra: number): void {
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
export function tagBytes(fnum: number, wt: number): Uint8Array {
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

export function wTag(w: W, tag: Uint8Array): void {
    ensure(w, tag.length);
    w.b.set(tag, w.pos);
    w.pos += tag.length;
}

/** varint32 write of an unsigned 32-bit value. */
export function wVar32(w: W, v: number): void {
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
export function wVar64(w: W, v: bigint): void {
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

export function wFixed32(w: W, v: number): void {
    ensure(w, 4);
    const b = w.b;
    let p = w.pos;
    b[p++] = v & 0xff;
    b[p++] = (v >>> 8) & 0xff;
    b[p++] = (v >>> 16) & 0xff;
    b[p++] = (v >>> 24) & 0xff;
    w.pos = p;
}

export function wFixed64(w: W, v: bigint): void {
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
export function wTagVar32(w: W, tag: Uint8Array, v: number): void {
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

export function wTagVar64(w: W, tag: Uint8Array, v: bigint): void {
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

export function wTagFixed32(w: W, tag: Uint8Array, v: number): void {
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

export function wTagFixed64(w: W, tag: Uint8Array, v: bigint): void {
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
export function wBytes(w: W, tag: Uint8Array | null, v: Uint8Array): void {
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
export function patchLen(w: W, lp: number): void {
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

export function sizeVar32(v: number): number {
    let n = 1;
    while (v > 0x7f) {
        v >>>= 7;
        n++;
    }
    return n;
}

/** varint64 of an unsigned value. */
export function sizeVar64(v: bigint): number {
    let n = 1;
    let x = BigInt.asUintN(64, v);
    while (x > 0x7fn) {
        x >>= 7n;
        n++;
    }
    return n;
}

/** int32: negatives sign-extend to 10 bytes, same as libprotobuf. */
export function sizeInt32(v: number): number {
    return v >= 0 ? sizeVar32(v) : 10;
}

export function zig32(v: number): number {
    return ((v << 1) ^ (v >> 31)) >>> 0;
}

export function zig64(v: bigint): bigint {
    return BigInt.asUintN(64, (v << 1n) ^ (v >> 63n));
}

export function unzig32(v: number): number {
    return (v >>> 1) ^ -(v & 1);
}

export function unzig64(v: bigint): bigint {
    return BigInt.asIntN(64, (v >> 1n) ^ -(v & 1n));
}

/* ---------------- reading ---------------- */

/** varint, low 32 bits (unsigned). Validates the encoding's length only. */
export function rVar(r: R): number {
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
export function rVar64(r: R): bigint {
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
export function rVarBool(r: R): number {
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
export function rLen32(r: R): number {
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
export function rLen(r: R): number {
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
export function rTag(r: R): number {
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

export function rFixed32(r: R): number {
    if (r.pos + 4 > r.end) {
        throw new CoreError('truncated fixed32');
    }
    const b = r.b;
    const p = r.pos;
    r.pos = p + 4;
    return (b[p] | (b[p + 1] << 8) | (b[p + 2] << 16) | (b[p + 3] << 24)) >>> 0;
}

/** Raw 64 bits, unsigned. */
export function rFixed64(r: R): bigint {
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
export function skip(r: R, wt: number): void {
    switch (wt) {
        case WT_VARINT:
            rVarBool(r);
            return;
        case WT_FIXED64:
            if (r.pos + 8 > r.end) {
                throw new CoreError('truncated fixed64');
            }
            r.pos += 8;
            return;
        case WT_LEN: {
            const n = rLen32(r);
            r.pos += n;
            return;
        }
        case WT_FIXED32:
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
export function skipGroup(r: R, number: number, depth: number): void {
    if (depth > r.cap) {
        throw new CoreError('nesting exceeds ' + r.cap, 4);
    }
    for (;;) {
        const tag = rTag(r);
        const fnum = tag >>> 3;
        const wt = tag & 7;
        if (fnum === 0 || wt > WT_FIXED32) {
            throw new CoreError('bad tag ' + tag);
        }
        if (wt === WT_END_GROUP) {
            if (fnum !== number) {
                throw new CoreError('mismatched end-group for ' + fnum + ', want ' + number);
            }
            return;
        }
        if (wt === WT_START_GROUP) {
            skipGroup(r, fnum, depth + 1);
        } else {
            skip(r, wt);
        }
    }
}

/**
 * Skips one unknown field: groups validate to their matching end tag,
 * anything else only checks value shape. End-group tags never reach here;
 * the decode loop owns them.
 */
export function skipUnknown(r: R, fnum: number, wt: number, depth: number): void {
    if (wt === WT_START_GROUP) {
        skipGroup(r, fnum, depth + 1);
        return;
    }
    skip(r, wt);
}

/**
 * Structural UTF-8 check: rejects overlong forms, surrogates and values past
 * U+10FFFF, like the core's pbw_utf8_valid and the reference parsers.
 */
export function utf8Valid(b: Uint8Array, off: number, len: number): boolean {
    let i = off;
    const end = off + len;
    while (i < end) {
        const c = b[i];
        if (c < 0x80) {
            i++;
        } else if ((c & 0xe0) === 0xc0) {
            if (c < 0xc2 || i + 1 >= end || (b[i + 1] & 0xc0) !== 0x80) {
                return false;
            }
            i += 2;
        } else if ((c & 0xf0) === 0xe0) {
            if (i + 2 >= end || (b[i + 1] & 0xc0) !== 0x80 || (b[i + 2] & 0xc0) !== 0x80) {
                return false;
            }
            const v = ((c & 15) << 12) | ((b[i + 1] & 63) << 6) | (b[i + 2] & 63);
            if (v < 0x800 || (v >= 0xd800 && v <= 0xdfff)) {
                return false;
            }
            i += 3;
        } else if ((c & 0xf8) === 0xf0) {
            if (
                c > 0xf4 ||
                i + 3 >= end ||
                (b[i + 1] & 0xc0) !== 0x80 ||
                (b[i + 2] & 0xc0) !== 0x80 ||
                (b[i + 3] & 0xc0) !== 0x80
            ) {
                return false;
            }
            const v =
                ((c & 7) << 18) | ((b[i + 1] & 63) << 12) | ((b[i + 2] & 63) << 6) | (b[i + 3] & 63);
            if (v < 0x10000 || v > 0x10ffff) {
                return false;
            }
            i += 4;
        } else {
            return false;
        }
    }
    return true;
}

/** UTF-8 length of a JS string; lone surrogates count as U+FFFD. */
export function utf8Length(s: string): number {
    let n = 0;
    for (let i = 0; i < s.length; i++) {
        const c = s.charCodeAt(i);
        if (c < 0x80) {
            n += 1;
        } else if (c < 0x800) {
            n += 2;
        } else if (c >= 0xd800 && c <= 0xdbff && i + 1 < s.length) {
            const d = s.charCodeAt(i + 1);
            if (d >= 0xdc00 && d <= 0xdfff) {
                n += 4;
                i++;
            } else {
                n += 3; // lone high surrogate: U+FFFD
            }
        } else {
            n += 3;
        }
    }
    return n;
}

/** Writes `s` as UTF-8 at w.pos (capacity must already be ensured). */
export function utf8Write(w: W, s: string): void {
    const b = w.b;
    let p = w.pos;
    for (let i = 0; i < s.length; i++) {
        let c = s.charCodeAt(i);
        if (c < 0x80) {
            b[p++] = c;
        } else if (c < 0x800) {
            b[p++] = 0xc0 | (c >> 6);
            b[p++] = 0x80 | (c & 0x3f);
        } else if (c >= 0xd800 && c <= 0xdfff) {
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
            } else {
                b[p++] = 0xef;
                b[p++] = 0xbf;
                b[p++] = 0xbd;
            }
        } else {
            b[p++] = 0xe0 | (c >> 12);
            b[p++] = 0x80 | ((c >> 6) & 0x3f);
            b[p++] = 0x80 | (c & 0x3f);
        }
    }
    w.pos = p;
}

/** UTF-8 decodes a slice, replacing each malformed byte with U+FFFD. */
export function utf8Decode(b: Uint8Array, off: number, len: number): string {
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
                s += String.fromCharCode.apply(null, chunk as unknown as number[]);
            }
        } else if ((c & 0xe0) === 0xc0 && i + 1 < end && (b[i + 1] & 0xc0) === 0x80) {
            s += String.fromCharCode(((c & 0x1f) << 6) | (b[i + 1] & 0x3f));
            i += 2;
        } else if (
            (c & 0xf0) === 0xe0 &&
            i + 2 < end &&
            (b[i + 1] & 0xc0) === 0x80 &&
            (b[i + 2] & 0xc0) === 0x80
        ) {
            const v = ((c & 0x0f) << 12) | ((b[i + 1] & 0x3f) << 6) | (b[i + 2] & 0x3f);
            if (v < 0x800 || (v >= 0xd800 && v <= 0xdfff)) {
                s += '\ufffd'; // overlong or surrogate: one malformed unit
            } else {
                s += String.fromCharCode(v);
            }
            i += 3;
        } else if (
            (c & 0xf8) === 0xf0 &&
            i + 3 < end &&
            (b[i + 1] & 0xc0) === 0x80 &&
            (b[i + 2] & 0xc0) === 0x80 &&
            (b[i + 3] & 0xc0) === 0x80
        ) {
            let v = ((c & 7) << 18) | ((b[i + 1] & 0x3f) << 12) | ((b[i + 2] & 0x3f) << 6) | (b[i + 3] & 0x3f);
            if (v < 0x10000 || v > 0x10ffff) {
                s += '\ufffd';
            } else {
                v -= 0x10000;
                s += String.fromCharCode(0xd800 + (v >> 10), 0xdc00 + (v & 0x3ff));
            }
            i += 4;
        } else {
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
export function floatBits(v: number): number {
    f32Value[0] = v;
    return f32Bits[0];
}

export function bitsFloat(bits: number): number {
    f32Bits[0] = bits;
    return f32Value[0];
}

const f64Buf = new ArrayBuffer(8);
const f64Bits = new Uint32Array(f64Buf);
const f64Value = new Float64Array(f64Buf);

/** The float64 bit pattern of a JS number, as raw 64 bits. */
export function doubleBits(v: number): bigint {
    f64Value[0] = v;
    return BigInt(f64Bits[0]) | (BigInt(f64Bits[1]) << 32n);
}

export function bitsDouble(bits: bigint): number {
    f64Bits[0] = Number(BigInt.asUintN(32, bits));
    f64Bits[1] = Number(BigInt.asUintN(32, bits >> 32n));
    return f64Value[0];
}

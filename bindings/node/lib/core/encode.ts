/*
 * Encoder: one pass into a reusable buffer, fields in schema (field-number)
 * order, unknown fields appended verbatim in input order. Nested payloads use
 * a one-byte length placeholder patched afterwards, packed runs use a
 * five-byte placeholder shifted down once, and untouched Raw slices copy
 * verbatim (tag included), so a decode/encode round trip is byte-exact.
 *
 * The TS twin of bindings/java/runtime/GenericCodec.encodeInto.
 */

import type { Field } from './descriptor';
import { IMPLICIT, K, REQUIRED, isPackable } from './descriptor';
import { Msg, Raw, checkPresentRequired } from './tree';
import {
    CoreError,
    MAX_DEPTH,
    W,
    ensure,
    patchLen,
    sizeVar32,
    tagBytes,
    utf8Length,
    utf8Write,
    wBytes,
    wFixed32,
    wFixed64,
    wTag,
    wTagFixed32,
    wTagFixed64,
    wTagVar32,
    wTagVar64,
    wVar32,
    wVar64,
    zig32,
    zig64,
} from './wire';

/* One workspace buffer: an encode pays no allocation but the result copy. */
const WORKSPACE = new W();

export function encode(m: Msg): Uint8Array {
    const w = WORKSPACE;
    w.pos = 0;
    encodeInto(m, w, 0);
    return w.b.slice(0, w.pos);
}

function isZero(v: unknown): boolean {
    return v === 0 || v === 0n;
}

function encodeInto(m: Msg, w: W, depth: number): void {
    if (depth > MAX_DEPTH) {
        throw new CoreError('nesting exceeds ' + MAX_DEPTH, 4);
    }
    const def = m.def;
    const noRequired = !def.hasRequired; // proto3: skip the check
    const fields = def.fields;
    for (let s = 0; s < fields.length; s++) {
        const v = m.v[s];
        if (v === undefined) {
            if (!noRequired && fields[s].presence === REQUIRED) {
                checkPresentRequired(m, s); // throws
            }
            continue;
        }
        const f = fields[s];
        if (f.repeated) {
            const arr = v as unknown[];
            if (arr.length === 0) {
                continue;
            }
            if (isPackable(f.kind)) {
                encodeRepNum(w, f, arr);
            } else {
                for (let i = 0; i < arr.length; i++) {
                    writeValue(w, f, arr[i], depth);
                }
            }
            continue;
        }
        if (isPackable(f.kind)) {
            // Only implicit defaults skip here; explicit defaults (and
            // required) emit once set.
            if (isZero(v) && f.presence === IMPLICIT) {
                continue;
            }
            writeSingularNum(w, f, v);
            continue;
        }
        // Implicit empty strings and bytes stay silent, everything else emits.
        if (f.presence === IMPLICIT) {
            if (typeof v === 'string' && v.length === 0) {
                continue;
            }
            if (v instanceof Uint8Array && v.length === 0) {
                continue;
            }
        }
        writeValue(w, f, v, depth);
    }
    if (m.unknown !== null) {
        // Unknown fields last, in input order.
        for (const u of m.unknown) {
            ensure(w, u.length);
            w.b.set(u, w.pos);
            w.pos += u.length;
        }
    }
}

function writeValue(w: W, f: Field, v: unknown, depth: number): void {
    if (v instanceof Raw) {
        // Tag included: a verbatim copy stays byte-exact.
        ensure(w, v.len);
        w.b.set(v.b.subarray(v.off, v.off + v.len), w.pos);
        w.pos += v.len;
        return;
    }
    switch (f.kind) {
        case K.STRING: {
            const s = v as string;
            const n = utf8Length(s);
            ensure(w, f.tag.length + 5 + n);
            let p = w.pos;
            const b = w.b;
            for (let i = 0; i < f.tag.length; i++) {
                b[p++] = f.tag[i];
            }
            let x = n;
            while (x > 0x7f) {
                b[p++] = (x & 0x7f) | 0x80;
                x >>>= 7;
            }
            b[p++] = x;
            w.pos = p;
            utf8Write(w, s);
            return;
        }
        case K.BYTES:
            wBytes(w, f.tag, v as Uint8Array);
            return;
        case K.MESSAGE: {
            // Inlined tag + length placeholder + patch: a repeated message
            // field is the hottest nesting shape.
            const tag = f.tag;
            ensure(w, tag.length + 1);
            let p = w.pos;
            const b = w.b;
            for (let i = 0; i < tag.length; i++) {
                b[p++] = tag[i];
            }
            const lp = p;
            b[p++] = 0;
            w.pos = p;
            encodeInto(v as Msg, w, depth + 1);
            patchLen(w, lp);
            return;
        }
        default: {
            // K.GROUP: start tag, body, end tag.
            wTag(w, f.tag);
            encodeInto(v as Msg, w, depth + 1);
            wTag(w, f.tagEnd as Uint8Array);
            return;
        }
    }
}

/** Singular numeric write: one fused tag+value call per field. */
function writeSingularNum(w: W, f: Field, v: unknown): void {
    switch (f.kind) {
        case K.INT32:
        case K.ENUM: {
            // Encode the int32 interpretation, sign-extended: the reference
            // truncates a wide varint to int32 and re-emits a negative as 10
            // bytes.
            const x = v as number;
            if (x >= 0) {
                wTagVar32(w, f.tag, x);
            } else {
                wTagVar64(w, f.tag, BigInt(x));
            }
            return;
        }
        case K.UINT32:
            wTagVar32(w, f.tag, v as number);
            return;
        case K.SINT32:
            wTagVar32(w, f.tag, zig32(v as number));
            return;
        case K.INT64:
        case K.UINT64:
            wTagVar64(w, f.tag, v as bigint);
            return;
        case K.SINT64:
            wTagVar64(w, f.tag, zig64(v as bigint));
            return;
        case K.BOOL:
            wTagVar32(w, f.tag, (v as number) === 0 ? 0 : 1);
            return;
        case K.FIXED32:
        case K.SFIXED32:
        case K.FLOAT:
            wTagFixed32(w, f.tag, v as number);
            return;
        default: // K.FIXED64, K.SFIXED64, K.DOUBLE
            wTagFixed64(w, f.tag, v as bigint);
            return;
    }
}

/** One dispatch per field; the loops below it are branch-free writes. */
function encodeRepNum(w: W, f: Field, arr: unknown[]): void {
    const n = arr.length;
    if (f.packed) {
        if (f.kind === K.BOOL) {
            wTag(w, f.tag);
            wVar32(w, n);
            ensure(w, n);
            for (let i = 0; i < n; i++) {
                w.b[w.pos++] = (arr[i] as number) === 0 ? 0 : 1;
            }
            return;
        }
        if (f.kind === K.FIXED32 || f.kind === K.SFIXED32 || f.kind === K.FLOAT) {
            wTag(w, f.tag);
            wVar32(w, n * 4);
            ensure(w, n * 4);
            for (let i = 0; i < n; i++) {
                const v = arr[i] as number;
                let p = w.pos;
                const b = w.b;
                b[p++] = v & 0xff;
                b[p++] = (v >>> 8) & 0xff;
                b[p++] = (v >>> 16) & 0xff;
                b[p++] = (v >>> 24) & 0xff;
                w.pos = p;
            }
            return;
        }
        if (f.kind === K.FIXED64 || f.kind === K.SFIXED64 || f.kind === K.DOUBLE) {
            wTag(w, f.tag);
            wVar32(w, n * 8);
            ensure(w, n * 8);
            for (let i = 0; i < n; i++) {
                wFixed64(w, arr[i] as bigint);
            }
            return;
        }
        // Varint kinds: write the payload over a 5-byte placeholder, then
        // shift it down: one memmove beats a size pre-pass over thousands of
        // elements.
        wTag(w, f.tag);
        const lp = w.pos;
        ensure(w, n * 10 + 5);
        w.pos += 5;
        for (let i = 0; i < n; i++) {
            writePackedVar(w, f.kind, arr[i]);
        }
        const len = w.pos - lp - 5;
        const llen = sizeVar32(len);
        w.b.copyWithin(lp + llen, lp + 5, w.pos);
        let p = lp;
        let x = len;
        while (x > 0x7f) {
            w.b[p++] = (x & 0x7f) | 0x80;
            x >>>= 7;
        }
        w.b[p++] = x;
        w.pos = lp + llen + len;
        return;
    }
    for (let i = 0; i < n; i++) {
        wTag(w, f.tag);
        writeNumSingle(w, f.kind, arr[i]);
    }
}

function writePackedVar(w: W, kind: number, v: unknown): void {
    switch (kind) {
        case K.INT32:
        case K.ENUM: {
            const x = v as number;
            if (x >= 0) {
                wVar32(w, x);
            } else {
                wVar64(w, BigInt(x));
            }
            return;
        }
        case K.UINT32:
            wVar32(w, v as number);
            return;
        case K.SINT32:
            wVar32(w, zig32(v as number));
            return;
        case K.INT64:
        case K.UINT64:
            wVar64(w, v as bigint);
            return;
        case K.SINT64:
            wVar64(w, zig64(v as bigint));
            return;
        default:
            throw new CoreError('not packable');
    }
}

function writeNumSingle(w: W, kind: number, v: unknown): void {
    switch (kind) {
        case K.INT32:
        case K.ENUM: {
            const x = v as number;
            if (x >= 0) {
                wVar32(w, x);
            } else {
                wVar64(w, BigInt(x));
            }
            return;
        }
        case K.UINT32:
            wVar32(w, v as number);
            return;
        case K.SINT32:
            wVar32(w, zig32(v as number));
            return;
        case K.INT64:
        case K.UINT64:
            wVar64(w, v as bigint);
            return;
        case K.SINT64:
            wVar64(w, zig64(v as bigint));
            return;
        case K.BOOL:
            ensure(w, 1);
            w.b[w.pos++] = (v as number) === 0 ? 0 : 1;
            return;
        case K.FIXED32:
        case K.SFIXED32:
        case K.FLOAT:
            wFixed32(w, v as number);
            return;
        default: // K.FIXED64, K.SFIXED64, K.DOUBLE
            wFixed64(w, v as bigint);
            return;
    }
}

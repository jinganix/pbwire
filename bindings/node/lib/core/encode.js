"use strict";
/*
 * Encoder: one pass into a reusable buffer, fields in schema (field-number)
 * order, unknown fields appended verbatim in input order. Nested payloads use
 * a one-byte length placeholder patched afterwards, packed runs use a
 * five-byte placeholder shifted down once, and untouched Raw slices copy
 * verbatim (tag included), so a decode/encode round trip is byte-exact.
 *
 * The TS twin of bindings/java/runtime/GenericCodec.encodeInto.
 */
Object.defineProperty(exports, "__esModule", { value: true });
exports.encode = encode;
const descriptor_1 = require("./descriptor");
const tree_1 = require("./tree");
const wire_1 = require("./wire");
/* One workspace buffer: an encode pays no allocation but the result copy. */
const WORKSPACE = new wire_1.W();
function encode(m) {
    const w = WORKSPACE;
    w.pos = 0;
    encodeInto(m, w, 0);
    return w.b.slice(0, w.pos);
}
function isZero(v) {
    return v === 0 || v === 0n;
}
function encodeInto(m, w, depth) {
    if (depth > wire_1.MAX_DEPTH) {
        throw new wire_1.CoreError('nesting exceeds ' + wire_1.MAX_DEPTH, 4);
    }
    const def = m.def;
    const noRequired = !def.hasRequired; // proto3: skip the check
    const fields = def.fields;
    for (let s = 0; s < fields.length; s++) {
        const v = m.v[s];
        if (v === undefined) {
            if (!noRequired && fields[s].presence === descriptor_1.REQUIRED) {
                (0, tree_1.checkPresentRequired)(m, s); // throws
            }
            continue;
        }
        const f = fields[s];
        if (f.repeated) {
            const arr = v;
            if (arr.length === 0) {
                continue;
            }
            if ((0, descriptor_1.isPackable)(f.kind)) {
                encodeRepNum(w, f, arr);
            }
            else {
                for (let i = 0; i < arr.length; i++) {
                    writeValue(w, f, arr[i], depth);
                }
            }
            continue;
        }
        if ((0, descriptor_1.isPackable)(f.kind)) {
            // Only implicit defaults skip here; explicit defaults (and
            // required) emit once set.
            if (isZero(v) && f.presence === descriptor_1.IMPLICIT) {
                continue;
            }
            writeSingularNum(w, f, v);
            continue;
        }
        // Implicit empty strings and bytes stay silent, everything else emits.
        if (f.presence === descriptor_1.IMPLICIT) {
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
            (0, wire_1.ensure)(w, u.length);
            w.b.set(u, w.pos);
            w.pos += u.length;
        }
    }
}
function writeValue(w, f, v, depth) {
    if (v instanceof tree_1.Raw) {
        // Tag included: a verbatim copy stays byte-exact.
        (0, wire_1.ensure)(w, v.len);
        w.b.set(v.b.subarray(v.off, v.off + v.len), w.pos);
        w.pos += v.len;
        return;
    }
    switch (f.kind) {
        case descriptor_1.K.STRING: {
            const s = v;
            const n = (0, wire_1.utf8Length)(s);
            (0, wire_1.ensure)(w, f.tag.length + 5 + n);
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
            (0, wire_1.utf8Write)(w, s);
            return;
        }
        case descriptor_1.K.BYTES:
            (0, wire_1.wBytes)(w, f.tag, v);
            return;
        case descriptor_1.K.MESSAGE: {
            // Inlined tag + length placeholder + patch: a repeated message
            // field is the hottest nesting shape.
            const tag = f.tag;
            (0, wire_1.ensure)(w, tag.length + 1);
            let p = w.pos;
            const b = w.b;
            for (let i = 0; i < tag.length; i++) {
                b[p++] = tag[i];
            }
            const lp = p;
            b[p++] = 0;
            w.pos = p;
            encodeInto(v, w, depth + 1);
            (0, wire_1.patchLen)(w, lp);
            return;
        }
        default: {
            // K.GROUP: start tag, body, end tag.
            (0, wire_1.wTag)(w, f.tag);
            encodeInto(v, w, depth + 1);
            (0, wire_1.wTag)(w, f.tagEnd);
            return;
        }
    }
}
/** Singular numeric write: one fused tag+value call per field. */
function writeSingularNum(w, f, v) {
    switch (f.kind) {
        case descriptor_1.K.INT32:
        case descriptor_1.K.ENUM: {
            // Encode the int32 interpretation, sign-extended: the reference
            // truncates a wide varint to int32 and re-emits a negative as 10
            // bytes.
            const x = v;
            if (x >= 0) {
                (0, wire_1.wTagVar32)(w, f.tag, x);
            }
            else {
                (0, wire_1.wTagVar64)(w, f.tag, BigInt(x));
            }
            return;
        }
        case descriptor_1.K.UINT32:
            (0, wire_1.wTagVar32)(w, f.tag, v);
            return;
        case descriptor_1.K.SINT32:
            (0, wire_1.wTagVar32)(w, f.tag, (0, wire_1.zig32)(v));
            return;
        case descriptor_1.K.INT64:
        case descriptor_1.K.UINT64:
            (0, wire_1.wTagVar64)(w, f.tag, v);
            return;
        case descriptor_1.K.SINT64:
            (0, wire_1.wTagVar64)(w, f.tag, (0, wire_1.zig64)(v));
            return;
        case descriptor_1.K.BOOL:
            (0, wire_1.wTagVar32)(w, f.tag, v === 0 ? 0 : 1);
            return;
        case descriptor_1.K.FIXED32:
        case descriptor_1.K.SFIXED32:
        case descriptor_1.K.FLOAT:
            (0, wire_1.wTagFixed32)(w, f.tag, v);
            return;
        default: // K.FIXED64, K.SFIXED64, K.DOUBLE
            (0, wire_1.wTagFixed64)(w, f.tag, v);
            return;
    }
}
/** One dispatch per field; the loops below it are branch-free writes. */
function encodeRepNum(w, f, arr) {
    const n = arr.length;
    if (f.packed) {
        if (f.kind === descriptor_1.K.BOOL) {
            (0, wire_1.wTag)(w, f.tag);
            (0, wire_1.wVar32)(w, n);
            (0, wire_1.ensure)(w, n);
            for (let i = 0; i < n; i++) {
                w.b[w.pos++] = arr[i] === 0 ? 0 : 1;
            }
            return;
        }
        if (f.kind === descriptor_1.K.FIXED32 || f.kind === descriptor_1.K.SFIXED32 || f.kind === descriptor_1.K.FLOAT) {
            (0, wire_1.wTag)(w, f.tag);
            (0, wire_1.wVar32)(w, n * 4);
            (0, wire_1.ensure)(w, n * 4);
            for (let i = 0; i < n; i++) {
                const v = arr[i];
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
        if (f.kind === descriptor_1.K.FIXED64 || f.kind === descriptor_1.K.SFIXED64 || f.kind === descriptor_1.K.DOUBLE) {
            (0, wire_1.wTag)(w, f.tag);
            (0, wire_1.wVar32)(w, n * 8);
            (0, wire_1.ensure)(w, n * 8);
            for (let i = 0; i < n; i++) {
                (0, wire_1.wFixed64)(w, arr[i]);
            }
            return;
        }
        // Varint kinds: write the payload over a 5-byte placeholder, then
        // shift it down: one memmove beats a size pre-pass over thousands of
        // elements.
        (0, wire_1.wTag)(w, f.tag);
        const lp = w.pos;
        (0, wire_1.ensure)(w, n * 10 + 5);
        w.pos += 5;
        for (let i = 0; i < n; i++) {
            writePackedVar(w, f.kind, arr[i]);
        }
        const len = w.pos - lp - 5;
        const llen = (0, wire_1.sizeVar32)(len);
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
        (0, wire_1.wTag)(w, f.tag);
        writeNumSingle(w, f.kind, arr[i]);
    }
}
function writePackedVar(w, kind, v) {
    switch (kind) {
        case descriptor_1.K.INT32:
        case descriptor_1.K.ENUM: {
            const x = v;
            if (x >= 0) {
                (0, wire_1.wVar32)(w, x);
            }
            else {
                (0, wire_1.wVar64)(w, BigInt(x));
            }
            return;
        }
        case descriptor_1.K.UINT32:
            (0, wire_1.wVar32)(w, v);
            return;
        case descriptor_1.K.SINT32:
            (0, wire_1.wVar32)(w, (0, wire_1.zig32)(v));
            return;
        case descriptor_1.K.INT64:
        case descriptor_1.K.UINT64:
            (0, wire_1.wVar64)(w, v);
            return;
        case descriptor_1.K.SINT64:
            (0, wire_1.wVar64)(w, (0, wire_1.zig64)(v));
            return;
        default:
            throw new wire_1.CoreError('not packable');
    }
}
function writeNumSingle(w, kind, v) {
    switch (kind) {
        case descriptor_1.K.INT32:
        case descriptor_1.K.ENUM: {
            const x = v;
            if (x >= 0) {
                (0, wire_1.wVar32)(w, x);
            }
            else {
                (0, wire_1.wVar64)(w, BigInt(x));
            }
            return;
        }
        case descriptor_1.K.UINT32:
            (0, wire_1.wVar32)(w, v);
            return;
        case descriptor_1.K.SINT32:
            (0, wire_1.wVar32)(w, (0, wire_1.zig32)(v));
            return;
        case descriptor_1.K.INT64:
        case descriptor_1.K.UINT64:
            (0, wire_1.wVar64)(w, v);
            return;
        case descriptor_1.K.SINT64:
            (0, wire_1.wVar64)(w, (0, wire_1.zig64)(v));
            return;
        case descriptor_1.K.BOOL:
            (0, wire_1.ensure)(w, 1);
            w.b[w.pos++] = v === 0 ? 0 : 1;
            return;
        case descriptor_1.K.FIXED32:
        case descriptor_1.K.SFIXED32:
        case descriptor_1.K.FLOAT:
            (0, wire_1.wFixed32)(w, v);
            return;
        default: // K.FIXED64, K.SFIXED64, K.DOUBLE
            (0, wire_1.wFixed64)(w, v);
            return;
    }
}

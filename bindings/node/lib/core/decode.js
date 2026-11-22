"use strict";
/*
 * Decoder: strict tags and lengths (like the C core), unknown field numbers
 * AND wrong-wire-type occurrences retained as raw tag+payload bytes in input
 * order, groups decoded to their matching end tag, packed runs accepted for
 * every numeric kind (and the unpacked element form for a packed field),
 * oneofs collapsed to the last member, presence following the field's own
 * zero, proto2 required fields checked over the whole tree, and the nesting
 * cap enforced on every recursive path.
 *
 * The TS twin of bindings/java/runtime/GenericCodec.decodeInto.
 */
Object.defineProperty(exports, "__esModule", { value: true });
exports.decode = decode;
exports.decodeSlice = decodeSlice;
exports.newMessage = newMessage;
const descriptor_1 = require("./descriptor");
const tree_1 = require("./tree");
const wire_1 = require("./wire");
/** Decodes a whole message, unknown fields retained. */
function decode(def, bytes, limits) {
    (0, descriptor_1.checkBytes)(limits, bytes.length, 'message');
    const r = new wire_1.R(bytes);
    r.cap = (0, descriptor_1.effectiveDepth)(limits);
    const m = new tree_1.Msg(def);
    decodeInto(m, r, bytes.length, -1, 0);
    if (def.deepRequired) {
        checkRequired(m, r.cap, 0);
    }
    return m;
}
/** Decodes a message body that already starts at `off` and ends at `off+len`. */
function decodeSlice(def, bytes, off, len, limits) {
    (0, descriptor_1.checkBytes)(limits, len, 'message');
    const r = new wire_1.R(bytes, off, off + len);
    r.cap = (0, descriptor_1.effectiveDepth)(limits);
    const m = new tree_1.Msg(def);
    decodeInto(m, r, off + len, -1, 0);
    if (def.deepRequired) {
        checkRequired(m, r.cap, 0);
    }
    return m;
}
/** An empty message of `def` (the twin of newMessage()). */
function newMessage(def) {
    return new tree_1.Msg(def);
}
/**
 * True when the field does not accept this wire type. A repeated numeric
 * field always accepts the LEN form (a packed run, whatever the declared
 * form) and a packed field also accepts its unpacked element form.
 */
function wtIncompatible(f, wt) {
    if (f.kind === descriptor_1.K.GROUP) {
        return wt !== wire_1.WT_START_GROUP;
    }
    if (f.repeated && (0, descriptor_1.isPackable)(f.kind)) {
        if (wt === wire_1.WT_LEN) {
            return false;
        }
        if (f.packed) {
            return wt !== f.elemWt;
        }
    }
    return wt !== f.wt;
}
/**
 * Decodes into m up to `end` (or the matching end-group when groupEnd >= 0).
 * One shared reader threads through the recursion, so positions never need
 * saving: a nested payload consumes exactly its slice.
 */
function decodeInto(m, r, end, groupEnd, depth) {
    if (depth > r.cap) {
        throw new wire_1.CoreError('nesting exceeds ' + r.cap, 4);
    }
    const def = m.def;
    const outerEnd = r.end;
    r.end = end;
    let closed = groupEnd < 0;
    while (r.pos < r.end) {
        const fieldStart = r.pos;
        const tag = (0, wire_1.rTag)(r);
        const fnum = tag >>> 3;
        const wt = tag & 7;
        if (fnum === 0 || wt > wire_1.WT_FIXED32) {
            throw new wire_1.CoreError('bad tag ' + tag, 1);
        }
        if (wt === wire_1.WT_END_GROUP) {
            // Only our own number closes us; any other end tag is stray.
            if (groupEnd < 0 || fnum !== groupEnd) {
                throw new wire_1.CoreError('stray end-group for ' + fnum, 1);
            }
            closed = true;
            break;
        }
        const s = (0, descriptor_1.slotOf)(def, fnum);
        if (s < 0 || wtIncompatible(def.fields[s], wt)) {
            retainUnknown(m, r, fieldStart, fnum, wt, depth);
            continue;
        }
        readKnown(m, r, def.fields[s], s, wt, fieldStart, depth);
    }
    r.end = outerEnd;
    if (!closed) {
        throw new wire_1.CoreError('missing end-group for ' + groupEnd, 1);
    }
}
/**
 * Wrong wire type (or unknown number) keeps the raw blob, like the core.
 * Groups validate to their matching end tag; anything else only checks value
 * shape -- unknown LEN payloads stay opaque.
 */
function retainUnknown(m, r, fieldStart, fnum, wt, depth) {
    (0, wire_1.skipUnknown)(r, fnum, wt, depth);
    if (m.unknown === null) {
        m.unknown = [];
    }
    m.unknown.push(r.b.slice(fieldStart, r.pos));
}
function readKnown(m, r, f, s, wt, fieldStart, depth) {
    if (f.repeated) {
        let arr = m.v[s];
        if (arr === undefined) {
            arr = [];
            m.v[s] = arr;
        }
        if ((0, descriptor_1.isPackable)(f.kind)) {
            if (wt === wire_1.WT_LEN) {
                readPacked(r, f, arr);
                if (arr.length === 0) {
                    // An empty packed run adds nothing: presence follows
                    // length, so drop the holder again.
                    m.v[s] = undefined;
                }
                return;
            }
            arr.push(readNumSingle(r, f.kind));
            return;
        }
        if (f.kind === descriptor_1.K.MESSAGE) {
            const len = (0, wire_1.rLen32)(r);
            const sub = new tree_1.Msg(f.msgType);
            decodeInto(sub, r, r.pos + len, -1, depth + 1);
            arr.push(sub);
            return;
        }
        if (f.kind === descriptor_1.K.GROUP) {
            const sub = new tree_1.Msg(f.msgType);
            decodeInto(sub, r, r.end, f.number, depth + 1);
            arr.push(sub);
            return;
        }
        // K.STRING / K.BYTES element: validate, then record one slice.
        const len = (0, wire_1.rLen32)(r);
        if (f.utf8 && !(0, wire_1.utf8Valid)(r.b, r.pos, len)) {
            throw new wire_1.CoreError('invalid UTF-8 in a string field', 1);
        }
        r.pos += len;
        arr.push(new tree_1.Raw(r.b, fieldStart, r.pos - fieldStart));
        return;
    }
    if ((0, descriptor_1.isPackable)(f.kind)) {
        const v = readNumSingle(r, f.kind);
        // Implicit presence is decided on the *field's* zero, not the raw
        // wire bits: an int32/uint32 varint wider than 32 bits truncates and
        // the reference then drops the field as an implicit zero.
        if (f.presence === descriptor_1.IMPLICIT && isZero(v)) {
            return;
        }
        (0, tree_1.setSlot)(m, s, v);
        return;
    }
    // Ref-kind singular: a second occurrence merges (like the core).
    if (f.kind === descriptor_1.K.MESSAGE) {
        const len = (0, wire_1.rLen32)(r);
        const existing = mergeTarget(m, f, s);
        if (existing !== null) {
            decodeInto(existing, r, r.pos + len, -1, depth + 1);
        }
        else {
            const sub = new tree_1.Msg(f.msgType);
            decodeInto(sub, r, r.pos + len, -1, depth + 1);
            (0, tree_1.setSlot)(m, s, sub);
        }
        return;
    }
    if (f.kind === descriptor_1.K.GROUP) {
        const existing = mergeTarget(m, f, s);
        if (existing !== null) {
            decodeInto(existing, r, r.end, f.number, depth + 1);
        }
        else {
            const sub = new tree_1.Msg(f.msgType);
            decodeInto(sub, r, r.end, f.number, depth + 1);
            (0, tree_1.setSlot)(m, s, sub);
        }
        return;
    }
    // K.STRING / K.BYTES (an empty implicit string stays absent).
    const len = (0, wire_1.rLen32)(r);
    if (len === 0 && f.presence === descriptor_1.IMPLICIT) {
        return;
    }
    /* proto3/editions strings are validated here, at parse, like the
     * reference: a malformed value fails the decode instead of surfacing as
     * U+FFFD when the slice is materialized. proto2 keeps the loose mapping. */
    if (f.utf8 && !(0, wire_1.utf8Valid)(r.b, r.pos, len)) {
        throw new wire_1.CoreError('invalid UTF-8 in a string field', 1);
    }
    r.pos += len;
    (0, tree_1.setSlot)(m, s, new tree_1.Raw(r.b, fieldStart, r.pos - fieldStart));
}
/**
 * The submessage a repeated singular message/group field merges into, or
 * null for the first occurrence.
 */
function mergeTarget(m, f, s) {
    const v = m.v[s];
    if (v === undefined) {
        return null;
    }
    if (f.oneof >= 0 && m.oneofCase !== null && m.oneofCase[f.oneof] !== s + 1) {
        return null;
    }
    return v instanceof tree_1.Msg ? v : null;
}
function isZero(v) {
    return v === 0 || v === 0n;
}
/** One packed run of numeric elements into `arr`. */
function readPacked(r, f, arr) {
    const len = (0, wire_1.rLen32)(r);
    const end = r.pos + len;
    if (f.kind === descriptor_1.K.BOOL) {
        while (r.pos < end) {
            arr.push((0, wire_1.rVarBool)(r));
        }
    }
    else if (f.kind === descriptor_1.K.FIXED32 || f.kind === descriptor_1.K.SFIXED32 || f.kind === descriptor_1.K.FLOAT) {
        const count = Math.floor(len / 4);
        for (let i = 0; i < count; i++) {
            const v = (0, wire_1.rFixed32)(r);
            arr.push(f.kind === descriptor_1.K.SFIXED32 ? (v | 0) : v);
        }
    }
    else if (f.kind === descriptor_1.K.FIXED64 || f.kind === descriptor_1.K.SFIXED64 || f.kind === descriptor_1.K.DOUBLE) {
        const count = Math.floor(len / 8);
        for (let i = 0; i < count; i++) {
            const v = (0, wire_1.rFixed64)(r);
            arr.push(f.kind === descriptor_1.K.SFIXED64 ? BigInt.asIntN(64, v) : v);
        }
    }
    else {
        while (r.pos < end) {
            arr.push(readPackedVar(r, f.kind));
        }
    }
    if (r.pos !== end) {
        throw new wire_1.CoreError('packed overrun', 1);
    }
}
function readPackedVar(r, kind) {
    switch (kind) {
        case descriptor_1.K.INT32:
        case descriptor_1.K.ENUM:
            return (0, wire_1.rVar)(r) | 0;
        case descriptor_1.K.UINT32:
            return (0, wire_1.rVar)(r);
        case descriptor_1.K.SINT32:
            // Truncate before unzigzag: a 64-bit varint on a sint32 field
            // decodes its low 32 bits, like the reference.
            return (0, wire_1.unzig32)((0, wire_1.rVar)(r));
        case descriptor_1.K.INT64:
            return BigInt.asIntN(64, (0, wire_1.rVar64)(r));
        case descriptor_1.K.UINT64:
            return (0, wire_1.rVar64)(r);
        case descriptor_1.K.SINT64:
            return (0, wire_1.unzig64)((0, wire_1.rVar64)(r));
        default:
            throw new wire_1.CoreError('not numeric', 1);
    }
}
/** One numeric element in its storage form (see tree.ts). */
function readNumSingle(r, kind) {
    switch (kind) {
        case descriptor_1.K.INT32:
        case descriptor_1.K.ENUM:
            return (0, wire_1.rVar)(r) | 0;
        case descriptor_1.K.UINT32:
            return (0, wire_1.rVar)(r);
        case descriptor_1.K.BOOL:
            return (0, wire_1.rVarBool)(r);
        case descriptor_1.K.SINT32:
            return (0, wire_1.unzig32)((0, wire_1.rVar)(r));
        case descriptor_1.K.INT64:
            return BigInt.asIntN(64, (0, wire_1.rVar64)(r));
        case descriptor_1.K.UINT64:
            return (0, wire_1.rVar64)(r);
        case descriptor_1.K.SINT64:
            return (0, wire_1.unzig64)((0, wire_1.rVar64)(r));
        case descriptor_1.K.FIXED32:
            return (0, wire_1.rFixed32)(r);
        case descriptor_1.K.SFIXED32:
            return (0, wire_1.rFixed32)(r) | 0;
        case descriptor_1.K.FLOAT:
            return (0, wire_1.rFixed32)(r);
        case descriptor_1.K.FIXED64:
            return (0, wire_1.rFixed64)(r);
        case descriptor_1.K.SFIXED64:
            return BigInt.asIntN(64, (0, wire_1.rFixed64)(r));
        case descriptor_1.K.DOUBLE:
            return (0, wire_1.rFixed64)(r);
        default:
            throw new wire_1.CoreError('not numeric', 1);
    }
}
/**
 * Whole-tree required check: every present nested message is checked too, so
 * a missing proto2 required field fails the decode, like the core.
 */
function checkRequired(m, cap, depth) {
    if (depth > cap) {
        throw new wire_1.CoreError('nesting exceeds ' + cap, 4);
    }
    const def = m.def;
    if (def.hasRequired) {
        for (let s = 0; s < def.fields.length; s++) {
            if (def.fields[s].presence === 2 /* REQUIRED */ && m.v[s] === undefined) {
                throw new wire_1.CoreError('missing required field ' + def.fullName + '.' + def.fields[s].name, 6);
            }
        }
    }
    for (let s = 0; s < def.fields.length; s++) {
        const f = def.fields[s];
        if (f.kind !== descriptor_1.K.MESSAGE && f.kind !== descriptor_1.K.GROUP) {
            continue;
        }
        const v = m.v[s];
        if (v === undefined) {
            continue;
        }
        if (f.repeated) {
            const arr = v;
            for (let i = 0; i < arr.length; i++) {
                checkRequired(arr[i], cap, depth + 1);
            }
        }
        else if (v instanceof tree_1.Msg) {
            checkRequired(v, cap, depth + 1);
        }
    }
}

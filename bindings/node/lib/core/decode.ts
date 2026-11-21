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

import type { Field, Limits, MsgDef } from './descriptor';
import {
    IMPLICIT,
    K,
    checkBytes,
    effectiveDepth,
    isPackable,
    slotOf,
} from './descriptor';
import { Msg, Raw, setSlot } from './tree';
import {
    CoreError,
    R,
    WT_END_GROUP,
    WT_FIXED32,
    WT_LEN,
    WT_START_GROUP,
    rFixed32,
    rFixed64,
    rLen32,
    rTag,
    rVar,
    rVar64,
    rVarBool,
    skipUnknown,
    unzig32,
    unzig64,
    utf8Valid,
} from './wire';

/** Decodes a whole message, unknown fields retained. */
export function decode(def: MsgDef, bytes: Uint8Array, limits?: Limits): Msg {
    checkBytes(limits, bytes.length, 'message');
    const r = new R(bytes);
    r.cap = effectiveDepth(limits);
    const m = new Msg(def);
    decodeInto(m, r, bytes.length, -1, 0);
    if (def.deepRequired) {
        checkRequired(m, r.cap, 0);
    }
    return m;
}

/** Decodes a message body that already starts at `off` and ends at `off+len`. */
export function decodeSlice(
    def: MsgDef,
    bytes: Uint8Array,
    off: number,
    len: number,
    limits?: Limits,
): Msg {
    checkBytes(limits, len, 'message');
    const r = new R(bytes, off, off + len);
    r.cap = effectiveDepth(limits);
    const m = new Msg(def);
    decodeInto(m, r, off + len, -1, 0);
    if (def.deepRequired) {
        checkRequired(m, r.cap, 0);
    }
    return m;
}

/** An empty message of `def` (the twin of newMessage()). */
export function newMessage(def: MsgDef): Msg {
    return new Msg(def);
}

/**
 * True when the field does not accept this wire type. A repeated numeric
 * field always accepts the LEN form (a packed run, whatever the declared
 * form) and a packed field also accepts its unpacked element form.
 */
function wtIncompatible(f: Field, wt: number): boolean {
    if (f.kind === K.GROUP) {
        return wt !== WT_START_GROUP;
    }
    if (f.repeated && isPackable(f.kind)) {
        if (wt === WT_LEN) {
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
function decodeInto(
    m: Msg,
    r: R,
    end: number,
    groupEnd: number,
    depth: number,
): void {
    if (depth > r.cap) {
        throw new CoreError('nesting exceeds ' + r.cap, 4);
    }
    const def = m.def;
    const outerEnd = r.end;
    r.end = end;
    let closed = groupEnd < 0;
    while (r.pos < r.end) {
        const fieldStart = r.pos;
        const tag = rTag(r);
        const fnum = tag >>> 3;
        const wt = tag & 7;
        if (fnum === 0 || wt > WT_FIXED32) {
            throw new CoreError('bad tag ' + tag, 1);
        }
        if (wt === WT_END_GROUP) {
            // Only our own number closes us; any other end tag is stray.
            if (groupEnd < 0 || fnum !== groupEnd) {
                throw new CoreError('stray end-group for ' + fnum, 1);
            }
            closed = true;
            break;
        }
        const s = slotOf(def, fnum);
        if (s < 0 || wtIncompatible(def.fields[s], wt)) {
            retainUnknown(m, r, fieldStart, fnum, wt, depth);
            continue;
        }
        readKnown(m, r, def.fields[s], s, wt, fieldStart, depth);
    }
    r.end = outerEnd;
    if (!closed) {
        throw new CoreError('missing end-group for ' + groupEnd, 1);
    }
}

/**
 * Wrong wire type (or unknown number) keeps the raw blob, like the core.
 * Groups validate to their matching end tag; anything else only checks value
 * shape -- unknown LEN payloads stay opaque.
 */
function retainUnknown(
    m: Msg,
    r: R,
    fieldStart: number,
    fnum: number,
    wt: number,
    depth: number,
): void {
    skipUnknown(r, fnum, wt, depth);
    if (m.unknown === null) {
        m.unknown = [];
    }
    m.unknown.push(r.b.slice(fieldStart, r.pos));
}

function readKnown(
    m: Msg,
    r: R,
    f: Field,
    s: number,
    wt: number,
    fieldStart: number,
    depth: number,
): void {
    if (f.repeated) {
        let arr = m.v[s] as unknown[] | undefined;
        if (arr === undefined) {
            arr = [];
            m.v[s] = arr;
        }
        if (isPackable(f.kind)) {
            if (wt === WT_LEN) {
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
        if (f.kind === K.MESSAGE) {
            const len = rLen32(r);
            const sub = new Msg(f.msgType as MsgDef);
            decodeInto(sub, r, r.pos + len, -1, depth + 1);
            arr.push(sub);
            return;
        }
        if (f.kind === K.GROUP) {
            const sub = new Msg(f.msgType as MsgDef);
            decodeInto(sub, r, r.end, f.number, depth + 1);
            arr.push(sub);
            return;
        }
        // K.STRING / K.BYTES element: validate, then record one slice.
        const len = rLen32(r);
        if (f.utf8 && !utf8Valid(r.b, r.pos, len)) {
            throw new CoreError('invalid UTF-8 in a string field', 1);
        }
        r.pos += len;
        arr.push(new Raw(r.b, fieldStart, r.pos - fieldStart));
        return;
    }
    if (isPackable(f.kind)) {
        const v = readNumSingle(r, f.kind);
        // Implicit presence is decided on the *field's* zero, not the raw
        // wire bits: an int32/uint32 varint wider than 32 bits truncates and
        // the reference then drops the field as an implicit zero.
        if (f.presence === IMPLICIT && isZero(v)) {
            return;
        }
        setSlot(m, s, v);
        return;
    }
    // Ref-kind singular: a second occurrence merges (like the core).
    if (f.kind === K.MESSAGE) {
        const len = rLen32(r);
        const existing = mergeTarget(m, f, s);
        if (existing !== null) {
            decodeInto(existing, r, r.pos + len, -1, depth + 1);
        } else {
            const sub = new Msg(f.msgType as MsgDef);
            decodeInto(sub, r, r.pos + len, -1, depth + 1);
            setSlot(m, s, sub);
        }
        return;
    }
    if (f.kind === K.GROUP) {
        const existing = mergeTarget(m, f, s);
        if (existing !== null) {
            decodeInto(existing, r, r.end, f.number, depth + 1);
        } else {
            const sub = new Msg(f.msgType as MsgDef);
            decodeInto(sub, r, r.end, f.number, depth + 1);
            setSlot(m, s, sub);
        }
        return;
    }
    // K.STRING / K.BYTES (an empty implicit string stays absent).
    const len = rLen32(r);
    if (len === 0 && f.presence === IMPLICIT) {
        return;
    }
    /* proto3/editions strings are validated here, at parse, like the
     * reference: a malformed value fails the decode instead of surfacing as
     * U+FFFD when the slice is materialized. proto2 keeps the loose mapping. */
    if (f.utf8 && !utf8Valid(r.b, r.pos, len)) {
        throw new CoreError('invalid UTF-8 in a string field', 1);
    }
    r.pos += len;
    setSlot(m, s, new Raw(r.b, fieldStart, r.pos - fieldStart));
}

/**
 * The submessage a repeated singular message/group field merges into, or
 * null for the first occurrence.
 */
function mergeTarget(m: Msg, f: Field, s: number): Msg | null {
    const v = m.v[s];
    if (v === undefined) {
        return null;
    }
    if (f.oneof >= 0 && m.oneofCase !== null && m.oneofCase[f.oneof] !== s + 1) {
        return null;
    }
    return v instanceof Msg ? v : null;
}

function isZero(v: unknown): boolean {
    return v === 0 || v === 0n;
}

/** One packed run of numeric elements into `arr`. */
function readPacked(r: R, f: Field, arr: unknown[]): void {
    const len = rLen32(r);
    const end = r.pos + len;
    if (f.kind === K.BOOL) {
        while (r.pos < end) {
            arr.push(rVarBool(r));
        }
    } else if (f.kind === K.FIXED32 || f.kind === K.SFIXED32 || f.kind === K.FLOAT) {
        const count = Math.floor(len / 4);
        for (let i = 0; i < count; i++) {
            const v = rFixed32(r);
            arr.push(f.kind === K.SFIXED32 ? (v | 0) : v);
        }
    } else if (f.kind === K.FIXED64 || f.kind === K.SFIXED64 || f.kind === K.DOUBLE) {
        const count = Math.floor(len / 8);
        for (let i = 0; i < count; i++) {
            const v = rFixed64(r);
            arr.push(f.kind === K.SFIXED64 ? BigInt.asIntN(64, v) : v);
        }
    } else {
        while (r.pos < end) {
            arr.push(readPackedVar(r, f.kind));
        }
    }
    if (r.pos !== end) {
        throw new CoreError('packed overrun', 1);
    }
}

function readPackedVar(r: R, kind: number): unknown {
    switch (kind) {
        case K.INT32:
        case K.ENUM:
            return rVar(r) | 0;
        case K.UINT32:
            return rVar(r);
        case K.SINT32:
            // Truncate before unzigzag: a 64-bit varint on a sint32 field
            // decodes its low 32 bits, like the reference.
            return unzig32(rVar(r));
        case K.INT64:
            return BigInt.asIntN(64, rVar64(r));
        case K.UINT64:
            return rVar64(r);
        case K.SINT64:
            return unzig64(rVar64(r));
        default:
            throw new CoreError('not numeric', 1);
    }
}

/** One numeric element in its storage form (see tree.ts). */
function readNumSingle(r: R, kind: number): unknown {
    switch (kind) {
        case K.INT32:
        case K.ENUM:
            return rVar(r) | 0;
        case K.UINT32:
            return rVar(r);
        case K.BOOL:
            return rVarBool(r);
        case K.SINT32:
            return unzig32(rVar(r));
        case K.INT64:
            return BigInt.asIntN(64, rVar64(r));
        case K.UINT64:
            return rVar64(r);
        case K.SINT64:
            return unzig64(rVar64(r));
        case K.FIXED32:
            return rFixed32(r);
        case K.SFIXED32:
            return rFixed32(r) | 0;
        case K.FLOAT:
            return rFixed32(r);
        case K.FIXED64:
            return rFixed64(r);
        case K.SFIXED64:
            return BigInt.asIntN(64, rFixed64(r));
        case K.DOUBLE:
            return rFixed64(r);
        default:
            throw new CoreError('not numeric', 1);
    }
}

/**
 * Whole-tree required check: every present nested message is checked too, so
 * a missing proto2 required field fails the decode, like the core.
 */
function checkRequired(m: Msg, cap: number, depth: number): void {
    if (depth > cap) {
        throw new CoreError('nesting exceeds ' + cap, 4);
    }
    const def = m.def;
    if (def.hasRequired) {
        for (let s = 0; s < def.fields.length; s++) {
            if (def.fields[s].presence === 2 /* REQUIRED */ && m.v[s] === undefined) {
                throw new CoreError(
                    'missing required field ' + def.fullName + '.' + def.fields[s].name,
                    6,
                );
            }
        }
    }
    for (let s = 0; s < def.fields.length; s++) {
        const f = def.fields[s];
        if (f.kind !== K.MESSAGE && f.kind !== K.GROUP) {
            continue;
        }
        const v = m.v[s];
        if (v === undefined) {
            continue;
        }
        if (f.repeated) {
            const arr = v as Msg[];
            for (let i = 0; i < arr.length; i++) {
                checkRequired(arr[i], cap, depth + 1);
            }
        } else if (v instanceof Msg) {
            checkRequired(v, cap, depth + 1);
        }
    }
}

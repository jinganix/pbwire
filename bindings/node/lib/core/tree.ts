/*
 * The dynamic message tree: one slot per declared field, plus the oneof
 * cases and the retained unknown fields. Scalars live in their wire-neutral
 * JS form (BigInt for the 64-bit kinds, raw bits for float/double so NaN
 * payloads survive a round trip, 0/1 for bool); messages live nested as Msg;
 * strings and bytes decoded off the wire stay a Raw slice of the input until
 * materialized, so invalid UTF-8 and non-minimal length prefixes re-encode
 * byte for byte.
 *
 * This is the TS twin of bindings/java/runtime/GenericCodec.DMsg.
 */

import type { Field, MsgDef } from './descriptor';
import { K, REQUIRED, slotOf } from './descriptor';
import {
    CoreError,
    R,
    bitsDouble,
    bitsFloat,
    doubleBits,
    floatBits,
    rLen,
    rVar,
    utf8Decode,
} from './wire';

/** An undecoded field image (tag + length + payload) inside the input. */
export class Raw {
    b: Uint8Array;
    off: number;
    len: number;

    constructor(b: Uint8Array, off: number, len: number) {
        this.b = b;
        this.off = off;
        this.len = len;
    }
}

export class Msg {
    def: MsgDef;
    /** Per slot: undefined = absent, an array for repeated fields. */
    v: unknown[];
    /** Slot+1 per oneof, 0 = none set; null when the message has none. */
    oneofCase: Int32Array | null;
    /** Retained unknown fields in input order; null until the first. */
    unknown: Uint8Array[] | null = null;

    constructor(def: MsgDef) {
        this.def = def;
        this.v = new Array(def.fields.length).fill(undefined);
        this.oneofCase = def.oneofCount > 0 ? new Int32Array(def.oneofCount) : null;
    }
}

/** Slot index of a field name, or -1. */
export function slotByName(def: MsgDef, name: string): number {
    const s = def.byName.get(name);
    return s === undefined ? -1 : s;
}

function requireSlot(def: MsgDef, nameOrNumber: string | number): number {
    const s = typeof nameOrNumber === 'number' ? slotOf(def, nameOrNumber) : slotByName(def, nameOrNumber);
    if (s < 0) {
        throw new CoreError(def.fullName + ' has no field ' + nameOrNumber);
    }
    return s;
}

/** Stores a value in a slot, collapsing the oneof it belongs to. */
export function setSlot(m: Msg, s: number, value: unknown): void {
    const f = m.def.fields[s];
    if (f.oneof >= 0 && m.oneofCase !== null) {
        const prev = m.oneofCase[f.oneof];
        if (prev !== 0 && prev !== s + 1) {
            m.v[prev - 1] = undefined;
        }
        m.oneofCase[f.oneof] = s + 1;
    }
    m.v[s] = value;
}

export function hasField(m: Msg, nameOrNumber: string | number): boolean {
    return m.v[requireSlot(m.def, nameOrNumber)] !== undefined;
}

/** The singular value of a field: materialized, converted, or undefined. */
export function getField(m: Msg, nameOrNumber: string | number): unknown {
    const s = requireSlot(m.def, nameOrNumber);
    const f = m.def.fields[s];
    const v = m.v[s];
    if (v === undefined) {
        return undefined;
    }
    if (f.repeated) {
        const arr = v as unknown[];
        const out = new Array(arr.length);
        for (let i = 0; i < arr.length; i++) {
            out[i] = toJs(f, arr[i]);
        }
        return out;
    }
    return toJs(f, v);
}

/** Which member of a oneof is set: its field number, or 0 for none. */
export function oneofCase(m: Msg, oneofName: string): number {
    const idx = m.def.oneofByName.get(oneofName);
    if (idx === undefined || m.oneofCase === null) {
        throw new CoreError(m.def.fullName + ' has no oneof ' + oneofName);
    }
    const c = m.oneofCase[idx];
    return c === 0 ? 0 : m.def.fields[c - 1].number;
}

/** The payload bytes of a Raw string/bytes field image. */
export function rawPayload(raw: Raw): Uint8Array {
    const r = new R(raw.b, raw.off, raw.off + raw.len);
    rVar(r); // the tag, already validated at slice time
    const n = rLen(r);
    return r.b.subarray(r.pos, r.pos + n);
}

/** Materials a Raw slice into its JS value. */
export function materialize(f: Field, raw: Raw): string | Uint8Array {
    const r = new R(raw.b, raw.off, raw.off + raw.len);
    rVar(r); // the tag
    const n = rLen(r);
    if (f.kind === K.STRING) {
        // proto3/editions strings were validated at parse, so this is the
        // loose mapping only for proto2 strings.
        return utf8Decode(r.b, r.pos, n);
    }
    return r.b.subarray(r.pos, r.pos + n);
}

/** One value in its JS form (what get()/toObject() expose). */
export function toJs(f: Field, v: unknown): unknown {
    if (v instanceof Raw) {
        return materialize(f, v);
    }
    switch (f.kind) {
        case K.BOOL:
            return (v as number) !== 0;
        case K.FLOAT:
            return bitsFloat(v as number);
        case K.DOUBLE:
            return bitsDouble(v as bigint);
        case K.MESSAGE:
        case K.GROUP:
            return v;
        default:
            return v;
    }
}

/** A plain JS object: proto field names, BigInt for 64-bit, nested objects. */
export function toObject(m: Msg): Record<string, unknown> {
    const def = m.def;
    const out: Record<string, unknown> = {};
    for (let s = 0; s < def.fields.length; s++) {
        const v = m.v[s];
        if (v === undefined) {
            continue;
        }
        const f = def.fields[s];
        if (f.repeated) {
            const arr = v as unknown[];
            if (arr.length === 0) {
                continue;
            }
            const list = new Array(arr.length);
            for (let i = 0; i < arr.length; i++) {
                const item = arr[i];
                list[i] =
                    (f.kind === K.MESSAGE || f.kind === K.GROUP) && item instanceof Msg
                        ? toObject(item)
                        : toJs(f, item);
            }
            out[f.name] = list;
            continue;
        }
        if ((f.kind === K.MESSAGE || f.kind === K.GROUP) && v instanceof Msg) {
            out[f.name] = toObject(v);
            continue;
        }
        out[f.name] = toJs(f, v);
    }
    return out;
}

/** The plain object for one field value (nested messages included). */
function fromJs(f: Field, v: unknown): unknown {
    switch (f.kind) {
        case K.INT32:
        case K.SINT32:
        case K.SFIXED32:
        case K.ENUM:
            return typeof v === 'bigint' ? Number(BigInt.asIntN(32, v)) : v;
        case K.UINT32:
        case K.FIXED32:
            return typeof v === 'bigint' ? Number(BigInt.asUintN(32, v)) : v;
        case K.INT64:
        case K.SINT64:
        case K.SFIXED64:
            return BigInt.asIntN(64, BigInt(v as bigint));
        case K.UINT64:
        case K.FIXED64:
            return BigInt.asUintN(64, BigInt(v as bigint));
        case K.BOOL:
            return v ? 1 : 0;
        case K.FLOAT:
            return floatBits(Number(v));
        case K.DOUBLE:
            return doubleBits(Number(v));
        case K.STRING:
            return typeof v === 'string' ? v : String(v);
        case K.BYTES:
            return v instanceof Uint8Array ? v : Uint8Array.from(v as ArrayLike<number>);
        default: // K.MESSAGE / K.GROUP
            return v instanceof Msg ? v : fromObject(f.msgType as MsgDef, v as object);
    }
}

/**
 * Builds a tree from a plain object (proto field names, BigInt for 64-bit;
 * absent or undefined keys stay absent, `null` is treated as absent).
 */
export function fromObject(def: MsgDef, obj: object): Msg {
    if (obj === null || typeof obj !== 'object') {
        throw new CoreError(def.fullName + ': needs an object');
    }
    const src = obj as Record<string, unknown>;
    const m = new Msg(def);
    for (let s = 0; s < def.fields.length; s++) {
        const f = def.fields[s];
        const v = src[f.name];
        if (v === undefined || v === null) {
            continue;
        }
        try {
            if (f.repeated) {
                if (!Array.isArray(v)) {
                    throw new CoreError('needs an array');
                }
                const arr = new Array(v.length);
                for (let i = 0; i < v.length; i++) {
                    arr[i] = fromJs(f, v[i]);
                }
                m.v[s] = arr;
            } else {
                setSlot(m, s, fromJs(f, v));
            }
        } catch (e) {
            const why = e instanceof Error ? e.message : String(e);
            throw new CoreError(def.fullName + '.' + f.name + ': ' + why);
        }
    }
    return m;
}

/** Throws when a REQUIRED field of this message is absent. */
export function checkPresentRequired(m: Msg, s: number): void {
    if (m.v[s] === undefined && m.def.fields[s].presence === REQUIRED) {
        const f = m.def.fields[s];
        throw new CoreError('missing required field ' + m.def.fullName + '.' + f.name, 6);
    }
}

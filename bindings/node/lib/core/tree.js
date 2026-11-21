"use strict";
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
Object.defineProperty(exports, "__esModule", { value: true });
exports.Msg = exports.Raw = void 0;
exports.slotByName = slotByName;
exports.setSlot = setSlot;
exports.hasField = hasField;
exports.getField = getField;
exports.oneofCase = oneofCase;
exports.rawPayload = rawPayload;
exports.materialize = materialize;
exports.toJs = toJs;
exports.toObject = toObject;
exports.fromObject = fromObject;
exports.checkPresentRequired = checkPresentRequired;
const descriptor_1 = require("./descriptor");
const wire_1 = require("./wire");
/** An undecoded field image (tag + length + payload) inside the input. */
class Raw {
    constructor(b, off, len) {
        this.b = b;
        this.off = off;
        this.len = len;
    }
}
exports.Raw = Raw;
class Msg {
    constructor(def) {
        /** Retained unknown fields in input order; null until the first. */
        this.unknown = null;
        this.def = def;
        this.v = new Array(def.fields.length).fill(undefined);
        this.oneofCase = def.oneofCount > 0 ? new Int32Array(def.oneofCount) : null;
    }
}
exports.Msg = Msg;
/** Slot index of a field name, or -1. */
function slotByName(def, name) {
    const s = def.byName.get(name);
    return s === undefined ? -1 : s;
}
function requireSlot(def, nameOrNumber) {
    const s = typeof nameOrNumber === 'number' ? (0, descriptor_1.slotOf)(def, nameOrNumber) : slotByName(def, nameOrNumber);
    if (s < 0) {
        throw new wire_1.CoreError(def.fullName + ' has no field ' + nameOrNumber);
    }
    return s;
}
/** Stores a value in a slot, collapsing the oneof it belongs to. */
function setSlot(m, s, value) {
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
function hasField(m, nameOrNumber) {
    return m.v[requireSlot(m.def, nameOrNumber)] !== undefined;
}
/** The singular value of a field: materialized, converted, or undefined. */
function getField(m, nameOrNumber) {
    const s = requireSlot(m.def, nameOrNumber);
    const f = m.def.fields[s];
    const v = m.v[s];
    if (v === undefined) {
        return undefined;
    }
    if (f.repeated) {
        const arr = v;
        const out = new Array(arr.length);
        for (let i = 0; i < arr.length; i++) {
            out[i] = toJs(f, arr[i]);
        }
        return out;
    }
    return toJs(f, v);
}
/** Which member of a oneof is set: its field number, or 0 for none. */
function oneofCase(m, oneofName) {
    const idx = m.def.oneofByName.get(oneofName);
    if (idx === undefined || m.oneofCase === null) {
        throw new wire_1.CoreError(m.def.fullName + ' has no oneof ' + oneofName);
    }
    const c = m.oneofCase[idx];
    return c === 0 ? 0 : m.def.fields[c - 1].number;
}
/** The payload bytes of a Raw string/bytes field image. */
function rawPayload(raw) {
    const r = new wire_1.R(raw.b, raw.off, raw.off + raw.len);
    (0, wire_1.rVar)(r); // the tag, already validated at slice time
    const n = (0, wire_1.rLen)(r);
    return r.b.subarray(r.pos, r.pos + n);
}
/** Materials a Raw slice into its JS value. */
function materialize(f, raw) {
    const r = new wire_1.R(raw.b, raw.off, raw.off + raw.len);
    (0, wire_1.rVar)(r); // the tag
    const n = (0, wire_1.rLen)(r);
    if (f.kind === descriptor_1.K.STRING) {
        // proto3/editions strings were validated at parse, so this is the
        // loose mapping only for proto2 strings.
        return (0, wire_1.utf8Decode)(r.b, r.pos, n);
    }
    return r.b.subarray(r.pos, r.pos + n);
}
/** One value in its JS form (what get()/toObject() expose). */
function toJs(f, v) {
    if (v instanceof Raw) {
        return materialize(f, v);
    }
    switch (f.kind) {
        case descriptor_1.K.BOOL:
            return v !== 0;
        case descriptor_1.K.FLOAT:
            return (0, wire_1.bitsFloat)(v);
        case descriptor_1.K.DOUBLE:
            return (0, wire_1.bitsDouble)(v);
        case descriptor_1.K.MESSAGE:
        case descriptor_1.K.GROUP:
            return v;
        default:
            return v;
    }
}
/** A plain JS object: proto field names, BigInt for 64-bit, nested objects. */
function toObject(m) {
    const def = m.def;
    const out = {};
    for (let s = 0; s < def.fields.length; s++) {
        const v = m.v[s];
        if (v === undefined) {
            continue;
        }
        const f = def.fields[s];
        if (f.repeated) {
            const arr = v;
            if (arr.length === 0) {
                continue;
            }
            const list = new Array(arr.length);
            for (let i = 0; i < arr.length; i++) {
                const item = arr[i];
                list[i] =
                    (f.kind === descriptor_1.K.MESSAGE || f.kind === descriptor_1.K.GROUP) && item instanceof Msg
                        ? toObject(item)
                        : toJs(f, item);
            }
            out[f.name] = list;
            continue;
        }
        if ((f.kind === descriptor_1.K.MESSAGE || f.kind === descriptor_1.K.GROUP) && v instanceof Msg) {
            out[f.name] = toObject(v);
            continue;
        }
        out[f.name] = toJs(f, v);
    }
    return out;
}
/** The plain object for one field value (nested messages included). */
function fromJs(f, v) {
    switch (f.kind) {
        case descriptor_1.K.INT32:
        case descriptor_1.K.SINT32:
        case descriptor_1.K.SFIXED32:
        case descriptor_1.K.ENUM:
            return typeof v === 'bigint' ? Number(BigInt.asIntN(32, v)) : v;
        case descriptor_1.K.UINT32:
        case descriptor_1.K.FIXED32:
            return typeof v === 'bigint' ? Number(BigInt.asUintN(32, v)) : v;
        case descriptor_1.K.INT64:
        case descriptor_1.K.SINT64:
        case descriptor_1.K.SFIXED64:
            return BigInt.asIntN(64, BigInt(v));
        case descriptor_1.K.UINT64:
        case descriptor_1.K.FIXED64:
            return BigInt.asUintN(64, BigInt(v));
        case descriptor_1.K.BOOL:
            return v ? 1 : 0;
        case descriptor_1.K.FLOAT:
            return (0, wire_1.floatBits)(Number(v));
        case descriptor_1.K.DOUBLE:
            return (0, wire_1.doubleBits)(Number(v));
        case descriptor_1.K.STRING:
            return typeof v === 'string' ? v : String(v);
        case descriptor_1.K.BYTES:
            return v instanceof Uint8Array ? v : Uint8Array.from(v);
        default: // K.MESSAGE / K.GROUP
            return v instanceof Msg ? v : fromObject(f.msgType, v);
    }
}
/**
 * Builds a tree from a plain object (proto field names, BigInt for 64-bit;
 * absent or undefined keys stay absent, `null` is treated as absent).
 */
function fromObject(def, obj) {
    if (obj === null || typeof obj !== 'object') {
        throw new wire_1.CoreError(def.fullName + ': needs an object');
    }
    const src = obj;
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
                    throw new wire_1.CoreError('needs an array');
                }
                const arr = new Array(v.length);
                for (let i = 0; i < v.length; i++) {
                    arr[i] = fromJs(f, v[i]);
                }
                m.v[s] = arr;
            }
            else {
                setSlot(m, s, fromJs(f, v));
            }
        }
        catch (e) {
            const why = e instanceof Error ? e.message : String(e);
            throw new wire_1.CoreError(def.fullName + '.' + f.name + ': ' + why);
        }
    }
    return m;
}
/** Throws when a REQUIRED field of this message is absent. */
function checkPresentRequired(m, s) {
    if (m.v[s] === undefined && m.def.fields[s].presence === descriptor_1.REQUIRED) {
        const f = m.def.fields[s];
        throw new wire_1.CoreError('missing required field ' + m.def.fullName + '.' + f.name, 6);
    }
}

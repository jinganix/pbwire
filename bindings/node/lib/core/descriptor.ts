/*
 * FileDescriptorSet reader and schema builder for the pure-TypeScript core:
 * the only reader of descriptor.proto the pure-JS path needs. It parses just
 * enough for a binary codec (names, numbers, labels, types, type names,
 * oneofs, extensions, packedness, proto3 optional, editions features, syntax)
 * and skips everything else by wire type, so unknown descriptor.proto
 * additions never break loading.
 *
 * The wire layout knowledge lives here; `encode`/`decode` know the message
 * wire layout and `schema` is the public facade. This mirrors
 * bindings/java/runtime/Proto.java and GenericCodec.Schema.
 */

import {
    CoreError,
    MAX_DEPTH,
    WT_LEN,
    WT_VARINT,
    R,
    rLen,
    rVar,
    skip,
    tagBytes,
    utf8Decode,
} from './wire';

/* ---------------- compiled schema (what encode/decode read) ---------------- */

/** Wire kinds of the table engine (its own numbering, not protobuf's). */
export const K = {
    INT32: 0,
    UINT32: 1,
    SINT32: 2,
    INT64: 3,
    UINT64: 4,
    SINT64: 5,
    BOOL: 6,
    ENUM: 7,
    FIXED32: 8,
    SFIXED32: 9,
    FLOAT: 10,
    FIXED64: 11,
    SFIXED64: 12,
    DOUBLE: 13,
    STRING: 14,
    BYTES: 15,
    MESSAGE: 16,
    GROUP: 17,
} as const;

/** Presence kinds. IMPLICIT = proto3 plain scalar (no has-bit). */
export const IMPLICIT = 0;
export const EXPLICIT = 1;
export const REQUIRED = 2;
export const MESSAGE_PRESENCE = 3;

/** The packable numeric kinds: packed runs cross every one of them. */
export function isPackable(kind: number): boolean {
    return kind <= K.DOUBLE;
}

export interface EnumDef {
    fullName: string;
    byName: Map<string, number>;
    byNumber: Map<number, string>;
    closed: boolean;
}

export interface Field {
    number: number;
    name: string;
    kind: number;
    /** Wire type of the declared form: LEN for packed numerics. */
    wt: number;
    /** Wire type of a single element (what an unpacked occurrence uses). */
    elemWt: number;
    repeated: boolean;
    packed: boolean;
    /** Oneof index, -1 when the field is not in a oneof. */
    oneof: number;
    presence: number;
    msgType: MsgDef | null;
    enumType: EnumDef | null;
    enumClosed: boolean;
    /** A decoded string must be valid UTF-8 (proto3/editions VERIFY). */
    utf8: boolean;
    /** Extensions only: the global name `[pkg.name]`. */
    fullName: string | null;
    /** Precomputed tag bytes (start-group tag for a group). */
    tag: Uint8Array;
    /** Groups only: the end-group tag. */
    tagEnd: Uint8Array | null;
}

export interface MsgDef {
    fullName: string;
    fields: Field[];
    /** Dense number-to-slot map, or null when field numbers are sparse. */
    slotByFnum: Int32Array | null;
    byName: Map<string, number>;
    oneofByName: Map<string, number>;
    oneofCount: number;
    hasRequired: boolean;
    /** Some transitively reachable message has a REQUIRED field. */
    deepRequired: boolean;
    /** protoc's synthesized map entry (field 1 key, field 2 value). */
    mapEntry: boolean;
    reservedNames: string[];
}

export class CoreSchema {
    messages = new Map<string, MsgDef>();
    enums = new Map<string, EnumDef>();

    message(fullName: string): MsgDef {
        const def = this.messages.get(fullName);
        if (def === undefined) {
            throw new CoreError('no such message type: ' + fullName);
        }
        return def;
    }
}

export interface Limits {
    /** Rejects inputs larger than this; 0 means no cap. */
    maxBytes: number;
    /** Nesting cap; 0 means MAX_DEPTH. A larger value clamps to it. */
    maxDepth: number;
}

export const DEFAULT_LIMITS: Limits = { maxBytes: 0, maxDepth: 0 };

export function effectiveDepth(lim: Limits | undefined): number {
    if (lim === undefined || lim.maxDepth === 0 || lim.maxDepth > MAX_DEPTH) {
        return MAX_DEPTH;
    }
    return lim.maxDepth;
}

export function checkBytes(lim: Limits | undefined, n: number, what: string): void {
    if (lim !== undefined && lim.maxBytes > 0 && n > lim.maxBytes) {
        throw new CoreError(what + ' exceeds the ' + lim.maxBytes + '-byte limit', 5);
    }
}

/* ---------------- descriptor.proto model ---------------- */

/* FieldDescriptorProto.Type values, kept numeric like the descriptor. */
const T_DOUBLE = 1;
const T_FLOAT = 2;
const T_INT64 = 3;
const T_UINT64 = 4;
const T_INT32 = 5;
const T_FIXED64 = 6;
const T_FIXED32 = 7;
const T_BOOL = 8;
const T_STRING = 9;
const T_GROUP = 10;
const T_MESSAGE = 11;
const T_BYTES = 12;
const T_UINT32 = 13;
const T_ENUM = 14;
const T_SFIXED32 = 15;
const T_SFIXED64 = 16;
const T_SINT32 = 17;
const T_SINT64 = 18;

const L_OPTIONAL = 1;
const L_REQUIRED = 2;
const L_REPEATED = 3;

interface POneof {
    name: string;
}

interface PField {
    name: string | null;
    extendee: string | null;
    number: number;
    label: number;
    type: number;
    typeName: string | null;
    oneofIndex: number;
    proto3Optional: boolean;
    packed: boolean;
    hasPackedOption: boolean;
    /** Editions FeatureSet.enum_type: 0 unset, 1 OPEN, 2 CLOSED. */
    enumOpen: number;
    /** Editions FeatureSet.utf8_validation: 0 unset, 2 VERIFY, 3 NONE. */
    utf8: number;
    /** Linked: the parsed message/enum and the oneof. */
    messageType: PMessage | null;
    enumType: PEnum | null;
    enumClosed: boolean;
    oneof: POneof | null;
    /** Linked: explicit presence (not a proto3 plain scalar). */
    hasPresence: boolean;
    /** Linked: the global extension name. */
    fullName: string | null;
}

interface PEnumValue {
    name: string | null;
    number: number;
}

interface PEnum {
    name: string | null;
    fullName: string;
    values: PEnumValue[];
    enumOpen: number;
    closed: boolean;
}

interface PMessage {
    name: string | null;
    fullName: string;
    mapEntry: boolean;
    fields: PField[];
    nested: PMessage[];
    extensions: PField[];
    oneofs: POneof[];
    enums: PEnum[];
    reservedNames: string[];
}

interface PFile {
    name: string | null;
    pkg: string;
    syntax: string;
    /** Field 14: an editions file (behaves as proto3 here). */
    edition: boolean;
    enumOpen: number;
    utf8: number;
    messages: PMessage[];
    extensions: PField[];
    enums: PEnum[];
}

function pfield(): PField {
    return {
        name: null,
        extendee: null,
        number: 0,
        label: L_OPTIONAL,
        type: 0,
        typeName: null,
        oneofIndex: -1,
        proto3Optional: false,
        packed: false,
        hasPackedOption: false,
        enumOpen: 0,
        utf8: 0,
        messageType: null,
        enumType: null,
        enumClosed: false,
        oneof: null,
        hasPresence: false,
        fullName: null,
    };
}

/* ---------------- wire parsing ---------------- */

function reader(b: Uint8Array, off: number, len: number): R {
    if (len < 0 || off < 0 || off + len > b.length) {
        throw new CoreError('descriptor overrun');
    }
    return new R(b, off, off + len);
}

function str(b: Uint8Array, off: number, len: number): string {
    return utf8Decode(b, off, len);
}

export function parseSet(bytes: Uint8Array): PFile[] {
    const r = new R(bytes);
    const files: PFile[] = [];
    while (r.pos < r.end) {
        const tag = rVar(r);
        if (tag >>> 3 === 1 && (tag & 7) === WT_LEN) {
            const n = rLen(r);
            files.push(parseFile(r.b, r.pos, n));
            r.pos += n;
        } else {
            skip(r, tag & 7);
        }
    }
    return files;
}

function parseFile(b: Uint8Array, off: number, len: number): PFile {
    const r = reader(b, off, len);
    const f: PFile = {
        name: null,
        pkg: '',
        syntax: 'proto2',
        edition: false,
        enumOpen: 0,
        utf8: 0,
        messages: [],
        extensions: [],
        enums: [],
    };
    while (r.pos < r.end) {
        const tag = rVar(r);
        const num = tag >>> 3;
        const wt = tag & 7;
        if (wt === WT_VARINT) {
            // Field 14 is the edition marker; its value is irrelevant here.
            if (num === 14) {
                rVar(r);
                f.edition = true;
            } else {
                skip(r, wt);
            }
            continue;
        }
        if (wt === WT_LEN) {
            const n = rLen(r);
            if (num === 4) {
                f.messages.push(parseMessage(r.b, r.pos, n, 1));
            } else if (num === 5) {
                f.enums.push(parseEnum(r.b, r.pos, n));
            } else if (num === 7) {
                f.extensions.push(parseField(r.b, r.pos, n));
            } else if (num === 8) {
                f.enumOpen = parseFeatures(r.b, r.pos, r.pos + n, 50);
                f.utf8 = parseFeatureSetUtf8(r.b, r.pos, r.pos + n, 50);
            } else if (num === 1) {
                f.name = str(r.b, r.pos, n);
            } else if (num === 2) {
                f.pkg = str(r.b, r.pos, n);
            } else if (num === 12) {
                f.syntax = str(r.b, r.pos, n);
            }
            r.pos += n;
        } else {
            skip(r, wt);
        }
    }
    return f;
}

function parseMessage(b: Uint8Array, off: number, len: number, depth: number): PMessage {
    // Descriptor nesting is peer data too: a FileDescriptorSet a few tens of
    // KB can nest thousands of DescriptorProtos, so cap it instead of letting
    // the recursion overflow the stack.
    if (depth > MAX_DEPTH) {
        throw new CoreError('descriptor nesting exceeds ' + MAX_DEPTH);
    }
    const r = reader(b, off, len);
    const m: PMessage = {
        name: null,
        fullName: '',
        mapEntry: false,
        fields: [],
        nested: [],
        extensions: [],
        oneofs: [],
        enums: [],
        reservedNames: [],
    };
    while (r.pos < r.end) {
        const tag = rVar(r);
        const num = tag >>> 3;
        if ((tag & 7) !== WT_LEN) {
            skip(r, tag & 7);
            continue;
        }
        const n = rLen(r);
        if (num === 1) {
            m.name = str(r.b, r.pos, n);
        } else if (num === 2) {
            m.fields.push(parseField(r.b, r.pos, n));
        } else if (num === 3) {
            m.nested.push(parseMessage(r.b, r.pos, n, depth + 1));
        } else if (num === 4) {
            m.enums.push(parseEnum(r.b, r.pos, n));
        } else if (num === 6) {
            m.extensions.push(parseField(r.b, r.pos, n));
        } else if (num === 7) {
            m.mapEntry = parseMapEntry(r.b, r.pos, n);
        } else if (num === 8) {
            m.oneofs.push(parseOneof(r.b, r.pos, n));
        } else if (num === 10) {
            m.reservedNames.push(str(r.b, r.pos, n));
        }
        r.pos += n;
    }
    if (m.name === null) {
        throw new CoreError('message without a name');
    }
    return m;
}

function parseOneof(b: Uint8Array, off: number, len: number): POneof {
    const r = reader(b, off, len);
    const o: POneof = { name: '' };
    while (r.pos < r.end) {
        const tag = rVar(r);
        if (tag >>> 3 === 1 && (tag & 7) === WT_LEN) {
            const n = rLen(r);
            o.name = str(r.b, r.pos, n);
            r.pos += n;
        } else {
            skip(r, tag & 7);
        }
    }
    return o;
}

function parseEnum(b: Uint8Array, off: number, len: number): PEnum {
    const r = reader(b, off, len);
    const e: PEnum = { name: null, fullName: '', values: [], enumOpen: 0, closed: false };
    while (r.pos < r.end) {
        const tag = rVar(r);
        const num = tag >>> 3;
        if ((tag & 7) !== WT_LEN) {
            skip(r, tag & 7);
            continue;
        }
        const n = rLen(r);
        if (num === 1) {
            e.name = str(r.b, r.pos, n);
        } else if (num === 2) {
            e.values.push(parseEnumValue(r.b, r.pos, n));
        } else if (num === 3) {
            e.enumOpen = parseFeatures(r.b, r.pos, r.pos + n, 7);
        }
        r.pos += n;
    }
    return e;
}

function parseEnumValue(b: Uint8Array, off: number, len: number): PEnumValue {
    const r = reader(b, off, len);
    const v: PEnumValue = { name: null, number: 0 };
    let hasNumber = false;
    while (r.pos < r.end) {
        const tag = rVar(r);
        const num = tag >>> 3;
        const wt = tag & 7;
        if (wt === WT_VARINT) {
            if (num === 2) {
                v.number = rVar(r) | 0;
                hasNumber = true;
            } else {
                skip(r, wt);
            }
        } else if (wt === WT_LEN) {
            const n = rLen(r);
            if (num === 1) {
                v.name = str(r.b, r.pos, n);
            }
            r.pos += n;
        } else {
            skip(r, wt);
        }
    }
    if (v.name === null || !hasNumber) {
        throw new CoreError('enum value without name/number');
    }
    return v;
}

/** MessageOptions.map_entry is field 7. */
function parseMapEntry(b: Uint8Array, off: number, len: number): boolean {
    const r = reader(b, off, len);
    let mapEntry = false;
    while (r.pos < r.end) {
        const tag = rVar(r);
        if ((tag & 7) !== WT_VARINT) {
            skip(r, tag & 7);
            continue;
        }
        if (tag >>> 3 === 7) {
            mapEntry = rVar(r) !== 0;
        } else {
            skip(r, WT_VARINT);
        }
    }
    return mapEntry;
}

/**
 * Reads one Options message and returns FeatureSet.enum_type (1 OPEN, 2
 * CLOSED) when it carries one; 0 means unset. `optionNumber` is the field
 * holding the FeatureSet (50 file, 21 field, ...).
 */
function parseFeatures(b: Uint8Array, off: number, end: number, optionNumber: number): number {
    const r = reader(b, off, end - off);
    let open = 0;
    while (r.pos < r.end) {
        const tag = rVar(r);
        const num = tag >>> 3;
        if ((tag & 7) !== WT_LEN) {
            skip(r, tag & 7);
            continue;
        }
        const n = rLen(r);
        if (num === optionNumber) {
            open = parseFeatureSetEnumType(r.b, r.pos, r.pos + n);
        }
        r.pos += n;
    }
    return open;
}

/** FileOptions.features (50) -> FeatureSet.utf8_validation. */
function parseFeatureSetUtf8(
    b: Uint8Array,
    off: number,
    end: number,
    optionNumber: number,
): number {
    const r = reader(b, off, end - off);
    let utf8 = 0;
    while (r.pos < r.end) {
        const tag = rVar(r);
        const num = tag >>> 3;
        if ((tag & 7) !== WT_LEN) {
            skip(r, tag & 7);
            continue;
        }
        const n = rLen(r);
        if (num === optionNumber) {
            utf8 = parseFeatureSetUtf8Value(r.b, r.pos, r.pos + n);
        }
        r.pos += n;
    }
    return utf8;
}

/** FeatureSet.enum_type = 2: 1 OPEN or 2 CLOSED. */
function parseFeatureSetEnumType(b: Uint8Array, off: number, end: number): number {
    const r = reader(b, off, end - off);
    let open = 0;
    while (r.pos < r.end) {
        const tag = rVar(r);
        if (tag >>> 3 === 2 && (tag & 7) === WT_VARINT) {
            const v = rVar(r) | 0;
            if (v === 1 || v === 2) {
                open = v;
            }
        } else {
            skip(r, tag & 7);
        }
    }
    return open;
}

/** FeatureSet.utf8_validation = 4: 2 VERIFY or 3 NONE. */
function parseFeatureSetUtf8Value(b: Uint8Array, off: number, end: number): number {
    const r = reader(b, off, end - off);
    let utf8 = 0;
    while (r.pos < r.end) {
        const tag = rVar(r);
        if (tag >>> 3 === 4 && (tag & 7) === WT_VARINT) {
            const v = rVar(r) | 0;
            if (v === 2 || v === 3) {
                utf8 = v;
            }
        } else {
            skip(r, tag & 7);
        }
    }
    return utf8;
}

function parseField(b: Uint8Array, off: number, len: number): PField {
    const r = reader(b, off, len);
    const f = pfield();
    while (r.pos < r.end) {
        const tag = rVar(r);
        const num = tag >>> 3;
        const wt = tag & 7;
        if (wt === WT_VARINT) {
            const v = rVar(r);
            if (num === 3) {
                f.number = v | 0;
            } else if (num === 4) {
                f.label = v | 0;
            } else if (num === 5) {
                f.type = v | 0;
            } else if (num === 9) {
                f.oneofIndex = v | 0;
            } else if (num === 17) {
                f.proto3Optional = v !== 0;
            }
        } else if (wt === WT_LEN) {
            const n = rLen(r);
            if (num === 1) {
                f.name = str(r.b, r.pos, n);
            } else if (num === 2) {
                f.extendee = str(r.b, r.pos, n);
            } else if (num === 6) {
                f.typeName = str(r.b, r.pos, n);
            } else if (num === 8) {
                parseFieldOptions(r.b, r.pos, r.pos + n, f);
            }
            r.pos += n;
        } else {
            skip(r, wt);
        }
    }
    if (f.name === null || f.type === 0 || f.number === 0) {
        throw new CoreError('field without name/type/number');
    }
    if (f.type < T_DOUBLE || f.type > T_SINT64) {
        throw new CoreError('bad field type ' + f.type);
    }
    if (f.label < L_OPTIONAL || f.label > L_REPEATED) {
        throw new CoreError('bad label ' + f.label);
    }
    return f;
}

/** FieldOptions: packed (2) and the editions FeatureSet (21). */
function parseFieldOptions(b: Uint8Array, off: number, end: number, f: PField): void {
    const r = reader(b, off, end - off);
    while (r.pos < r.end) {
        const tag = rVar(r);
        const num = tag >>> 3;
        if (num === 2 && (tag & 7) === WT_VARINT) {
            f.packed = rVar(r) !== 0;
            f.hasPackedOption = true;
        } else if (num === 21 && (tag & 7) === WT_LEN) {
            const n = rLen(r);
            f.enumOpen = parseFeatureSetEnumType(r.b, r.pos, r.pos + n);
            f.utf8 = parseFeatureSetUtf8Value(r.b, r.pos, r.pos + n);
            r.pos += n;
        } else {
            skip(r, tag & 7);
        }
    }
}

/* ---------------- linking ---------------- */

function indexMessage(
    m: PMessage,
    scope: string,
    byName: Map<string, PMessage>,
    enums: Map<string, PEnum>,
): void {
    m.fullName = scope === '' ? (m.name as string) : scope + '.' + m.name;
    if (byName.has(m.fullName)) {
        throw new CoreError('duplicate message ' + m.fullName);
    }
    byName.set(m.fullName, m);
    for (const e of m.enums) {
        indexEnum(e, m.fullName, enums);
    }
    for (const n of m.nested) {
        indexMessage(n, m.fullName, byName, enums);
    }
}

function indexEnum(e: PEnum, scope: string, enums: Map<string, PEnum>): void {
    e.fullName = scope === '' ? (e.name as string) : scope + '.' + e.name;
    if (enums.has(e.fullName)) {
        throw new CoreError('duplicate enum ' + e.fullName);
    }
    enums.set(e.fullName, e);
}

/** An enum is closed for proto2 files (and editions files asking for it). */
function linkEnum(e: PEnum, proto3: boolean, fileEnumOpen: number): void {
    if (e.name === null) {
        throw new CoreError('enum without a name');
    }
    if (e.enumOpen !== 0) {
        e.closed = e.enumOpen === 2;
    } else if (fileEnumOpen !== 0) {
        e.closed = fileEnumOpen === 2;
    } else {
        e.closed = !proto3;
    }
}

function linkEnums(m: PMessage, proto3: boolean, fileEnumOpen: number): void {
    for (const e of m.enums) {
        linkEnum(e, proto3, fileEnumOpen);
    }
    for (const n of m.nested) {
        linkEnums(n, proto3, fileEnumOpen);
    }
}

function linkMessage(
    m: PMessage,
    scope: string,
    proto3: boolean,
    fileEnumOpen: number,
    fileUtf8: number,
    byName: Map<string, PMessage>,
    enums: Map<string, PEnum>,
): void {
    for (const f of m.fields) {
        linkField(f, scope, proto3, fileUtf8, byName, enums, m);
    }
    for (const n of m.nested) {
        linkMessage(n, m.fullName, proto3, fileEnumOpen, fileUtf8, byName, enums);
    }
    for (const e of m.extensions) {
        linkField(e, scope, proto3, fileUtf8, byName, enums, null);
        e.fullName = e.name === null ? null : scope === '' ? e.name : scope + '.' + e.name;
    }
}

function isPackableType(t: number): boolean {
    return !(t === T_STRING || t === T_GROUP || t === T_MESSAGE || t === T_BYTES);
}

function linkField(
    f: PField,
    scope: string,
    proto3: boolean,
    fileUtf8: number,
    byName: Map<string, PMessage>,
    enums: Map<string, PEnum>,
    parent: PMessage | null,
): void {
    if (f.oneofIndex >= 0 && parent !== null) {
        if (f.oneofIndex >= parent.oneofs.length) {
            throw new CoreError('field ' + f.name + ': oneof_index out of range');
        }
        f.oneof = parent.oneofs[f.oneofIndex];
    }
    if ((f.type === T_MESSAGE || f.type === T_GROUP) && f.typeName !== null) {
        f.messageType = resolve(byName, scope, f.typeName);
        if (f.messageType === null) {
            throw new CoreError('field ' + f.name + ': no such type ' + f.typeName);
        }
    }
    if (f.type === T_ENUM && f.typeName !== null) {
        f.enumType = resolveEnum(enums, scope, f.typeName);
        if (f.enumType === null) {
            throw new CoreError('field ' + f.name + ': no such enum ' + f.typeName);
        }
        // Editions may close one field in an open file; that wins over the
        // enum type, exactly like the core's per-field enum_open.
        f.enumClosed = f.enumOpen !== 0 ? f.enumOpen === 2 : f.enumType.closed;
    }
    if (f.label === L_REPEATED && isPackableType(f.type) && !f.hasPackedOption) {
        f.packed = proto3; // the syntax default
    }
    /* utf8_validation: field FeatureSet, then the file's, then the dialect
     * default (VERIFY for proto3/editions, NONE for proto2). Only `string`
     * fields carry it, and only they validate on parse. */
    if (f.type === T_STRING) {
        const utf8 = f.utf8 !== 0 ? f.utf8 : fileUtf8 !== 0 ? fileUtf8 : proto3 ? 2 : 3;
        f.utf8 = utf8 === 2 ? 2 : 3;
    } else {
        f.utf8 = 3;
    }
    f.hasPresence =
        f.label !== L_REPEATED &&
        (f.type === T_MESSAGE || f.type === T_GROUP || f.label === L_REQUIRED || !proto3 || f.proto3Optional);
}

/** Leading-dot names resolve from the root; otherwise search the scope up. */
function resolve(byName: Map<string, PMessage>, scope: string, name: string): PMessage | null {
    if (name.startsWith('.')) {
        return byName.get(name.substring(1)) ?? null;
    }
    let s = scope;
    for (;;) {
        const cand = s === '' ? name : s + '.' + name;
        const m = byName.get(cand);
        if (m !== undefined) {
            return m;
        }
        // Nested scopes qualify from the inside out: C++ scoping.
        const dot = name.indexOf('.');
        if (dot >= 0) {
            const head = name.substring(0, dot);
            const rest = name.substring(dot + 1);
            const prefix = s === '' ? head : s + '.' + head;
            if (byName.has(prefix)) {
                return resolve(byName, prefix, rest);
            }
        }
        if (s === '') {
            return null;
        }
        const cut = s.lastIndexOf('.');
        s = cut < 0 ? '' : s.substring(0, cut);
    }
}

/** The enum counterpart of resolve: same C++ scoping rules. */
function resolveEnum(byName: Map<string, PEnum>, scope: string, name: string): PEnum | null {
    if (name.startsWith('.')) {
        return byName.get(name.substring(1)) ?? null;
    }
    let s = scope;
    for (;;) {
        const cand = s === '' ? name : s + '.' + name;
        const e = byName.get(cand);
        if (e !== undefined) {
            return e;
        }
        const dot = name.indexOf('.');
        if (dot >= 0) {
            const head = name.substring(0, dot);
            const rest = name.substring(dot + 1);
            const prefix = s === '' ? head : s + '.' + head;
            if (byName.has(prefix)) {
                return resolveEnum(byName, prefix, rest);
            }
        }
        if (s === '') {
            return null;
        }
        const cut = s.lastIndexOf('.');
        s = cut < 0 ? '' : s.substring(0, cut);
    }
}

function linkFiles(files: PFile[]): void {
    const byName = new Map<string, PMessage>();
    const enums = new Map<string, PEnum>();
    for (const f of files) {
        for (const m of f.messages) {
            indexMessage(m, f.pkg, byName, enums);
        }
        for (const e of f.enums) {
            indexEnum(e, f.pkg, enums);
        }
    }
    for (const f of files) {
        // Editions files carry syntax "editions" plus field 14: the core
        // treats them as proto3, so packed defaults and presence match.
        const proto3 = f.syntax === 'proto3' || f.edition;
        for (const e of f.enums) {
            linkEnum(e, proto3, f.enumOpen);
        }
        // Enums close/open before fields link: a field copies its enum's
        // closed flag, so the enum must be linked first.
        for (const m of f.messages) {
            linkEnums(m, proto3, f.enumOpen);
        }
        for (const m of f.messages) {
            linkMessage(m, f.pkg, proto3, f.enumOpen, f.utf8, byName, enums);
        }
        for (const e of f.extensions) {
            linkField(e, f.pkg, proto3, f.utf8, byName, enums, null);
            e.fullName = e.name === null ? null : f.pkg === '' ? e.name : f.pkg + '.' + e.name;
        }
    }
}

/* ---------------- compiled schema building ---------------- */

function kindOf(t: number): number {
    switch (t) {
        case T_INT32:
            return K.INT32;
        case T_UINT32:
            return K.UINT32;
        case T_SINT32:
            return K.SINT32;
        case T_INT64:
            return K.INT64;
        case T_UINT64:
            return K.UINT64;
        case T_SINT64:
            return K.SINT64;
        case T_BOOL:
            return K.BOOL;
        case T_ENUM:
            return K.ENUM;
        case T_FIXED32:
            return K.FIXED32;
        case T_SFIXED32:
            return K.SFIXED32;
        case T_FLOAT:
            return K.FLOAT;
        case T_FIXED64:
            return K.FIXED64;
        case T_SFIXED64:
            return K.SFIXED64;
        case T_DOUBLE:
            return K.DOUBLE;
        case T_STRING:
            return K.STRING;
        case T_BYTES:
            return K.BYTES;
        case T_MESSAGE:
            return K.MESSAGE;
        default:
            return K.GROUP;
    }
}

function wireType(t: number, packed: boolean): number {
    if (packed) {
        return WT_LEN;
    }
    switch (t) {
        case T_DOUBLE:
        case T_FIXED64:
        case T_SFIXED64:
            return 1;
        case T_STRING:
        case T_BYTES:
        case T_MESSAGE:
            return WT_LEN;
        case T_GROUP:
            return 3;
        case T_FIXED32:
        case T_SFIXED32:
        case T_FLOAT:
            return 5;
        default:
            return WT_VARINT;
    }
}

class Builder {
    schema = new CoreSchema();

    build(files: PFile[]): CoreSchema {
        // Enums first: message fields link their element type while the defs
        // are built.
        for (const f of files) {
            for (const e of f.enums) {
                this.buildEnum(e);
            }
            for (const m of f.messages) {
                this.buildEnums(m, 1);
            }
        }
        for (const f of files) {
            for (const m of f.messages) {
                this.buildDef(m, 1);
            }
        }
        // Extensions attach to their extendee after all defs exist.
        for (const f of files) {
            for (const e of f.extensions) {
                this.attachExtension(e);
            }
            for (const m of f.messages) {
                this.attachMessageExtensions(m);
            }
        }
        finishTransitive(this.schema);
        return this.schema;
    }

    private buildEnum(e: PEnum): void {
        const d: EnumDef = {
            fullName: e.fullName,
            byName: new Map(),
            byNumber: new Map(),
            closed: e.closed,
        };
        for (const v of e.values) {
            const name = v.name as string;
            if (d.byName.has(name)) {
                throw new CoreError('duplicate enum value ' + d.fullName + '.' + name);
            }
            d.byName.set(name, v.number);
            // Aliases repeat a number: the first declared name is printed.
            if (!d.byNumber.has(v.number)) {
                d.byNumber.set(v.number, name);
            }
        }
        this.schema.enums.set(d.fullName, d);
    }

    private buildEnums(m: PMessage, depth: number): void {
        if (depth > MAX_DEPTH) {
            throw new CoreError('descriptor nesting exceeds ' + MAX_DEPTH);
        }
        for (const e of m.enums) {
            this.buildEnum(e);
        }
        for (const n of m.nested) {
            this.buildEnums(n, depth + 1);
        }
    }

    private buildDef(m: PMessage, depth: number): MsgDef {
        if (depth > MAX_DEPTH) {
            throw new CoreError('descriptor nesting exceeds ' + MAX_DEPTH);
        }
        const done = this.schema.messages.get(m.fullName);
        if (done !== undefined) {
            return done;
        }
        const d: MsgDef = {
            fullName: m.fullName,
            fields: [],
            slotByFnum: null,
            byName: new Map(),
            oneofByName: new Map(),
            oneofCount: 0,
            hasRequired: false,
            deepRequired: false,
            mapEntry: m.mapEntry,
            reservedNames: m.reservedNames.slice(),
        };
        this.schema.messages.set(m.fullName, d); // before fields: recursion-safe
        const fs: Field[] = [];
        const oneofs = new Map<POneof, number>();
        for (const f of m.fields) {
            fs.push(this.buildField(f, oneofs));
        }
        finishDef(d, fs, oneofs);
        for (const n of m.nested) {
            this.buildDef(n, depth + 1);
        }
        return d;
    }

    private buildField(f: PField, oneofs: Map<POneof, number>): Field {
        const kind = kindOf(f.type);
        const repeated = f.label === L_REPEATED;
        let oneof = -1;
        if (f.oneof !== null) {
            const idx = oneofs.get(f.oneof);
            if (idx === undefined) {
                oneof = oneofs.size;
                oneofs.set(f.oneof, oneof);
            } else {
                oneof = idx;
            }
        }
        let presence: number;
        if (oneof >= 0) {
            presence = EXPLICIT; // reads/writes go through the oneof case
        } else if (f.label === L_REQUIRED) {
            presence = REQUIRED;
        } else if (kind === K.MESSAGE || kind === K.GROUP) {
            presence = MESSAGE_PRESENCE;
        } else if (f.hasPresence) {
            presence = EXPLICIT;
        } else {
            presence = IMPLICIT;
        }
        const packed = repeated && f.packed;
        const wt = wireType(f.type, packed);
        const number = f.number;
        const out: Field = {
            number,
            name: f.name as string,
            kind,
            wt,
            elemWt: wireType(f.type, false),
            repeated,
            packed,
            oneof,
            presence,
            msgType:
                f.messageType === null ? null : this.buildDef(f.messageType, 1),
            enumType: f.enumType === null ? null : (this.schema.enums.get(f.enumType.fullName) ?? null),
            enumClosed: f.enumClosed,
            utf8: f.type === T_STRING && f.utf8 === 2,
            fullName: f.fullName,
            tag: tagBytes(number, wt),
            tagEnd: kind === K.GROUP ? tagBytes(number, 4) : null,
        };
        return out;
    }

    private attachMessageExtensions(m: PMessage): void {
        for (const e of m.extensions) {
            this.attachExtension(e);
        }
        for (const n of m.nested) {
            this.attachMessageExtensions(n);
        }
    }

    private attachExtension(e: PField): void {
        let extendee = e.extendee;
        if (extendee === null) {
            throw new CoreError('extension without extendee');
        }
        if (extendee.startsWith('.')) {
            extendee = extendee.substring(1);
        }
        const d = this.schema.messages.get(extendee);
        if (d === undefined) {
            throw new CoreError('extendee missing: ' + e.extendee);
        }
        const s = this.buildField(e, new Map());
        s.fullName = e.fullName;
        const fs = d.fields.slice();
        fs.push(s);
        const oneofByName = new Map(d.oneofByName);
        const count = d.oneofCount;
        finishDef(d, fs, new Map());
        d.oneofByName = oneofByName; // extensions are never in a oneof
        d.oneofCount = count;
    }
}

function finishDef(d: MsgDef, fs: Field[], oneofs: Map<POneof, number>): void {
    fs.sort((a, b) => a.number - b.number);
    let max = 0;
    let req = false;
    for (const f of fs) {
        if (f.number > max) {
            max = f.number;
        }
        req = req || f.presence === REQUIRED;
    }
    d.hasRequired = req;
    d.fields = fs;
    // Dense only when bounded, like the C core's fnum map: one field
    // numbered 536870911 must not allocate a 2 GiB array.
    if (max <= 512 && max <= fs.length * 8 + 8) {
        const map = new Int32Array(max + 1).fill(-1);
        for (let i = 0; i < fs.length; i++) {
            map[fs[i].number] = i;
        }
        d.slotByFnum = map;
    } else {
        d.slotByFnum = null;
    }
    d.byName = new Map();
    for (let i = 0; i < fs.length; i++) {
        d.byName.set(fs[i].name, i);
    }
    d.oneofByName = new Map();
    for (const [oneof, idx] of oneofs) {
        d.oneofByName.set(oneof.name, idx);
    }
    d.oneofCount = Math.max(d.oneofCount, oneofs.size);
}

/** Slot of a field number, -1 when the message has no such field. */
export function slotOf(def: MsgDef, fnum: number): number {
    if (def.slotByFnum !== null) {
        return fnum >= 0 && fnum < def.slotByFnum.length ? def.slotByFnum[fnum] : -1;
    }
    for (let i = 0; i < def.fields.length; i++) {
        if (def.fields[i].number === fnum) {
            return i;
        }
    }
    return -1;
}

function finishTransitive(schema: CoreSchema): void {
    let changed = true;
    while (changed) {
        changed = false;
        for (const d of schema.messages.values()) {
            if (!d.deepRequired && needsRequired(d)) {
                d.deepRequired = true;
                changed = true;
            }
        }
    }
}

function needsRequired(d: MsgDef): boolean {
    for (const f of d.fields) {
        if (f.presence === REQUIRED) {
            return true;
        }
        if (
            (f.kind === K.MESSAGE || f.kind === K.GROUP) &&
            f.msgType !== null &&
            f.msgType.deepRequired
        ) {
            return true;
        }
    }
    return false;
}

/* ---------------- public entry ---------------- */

/** Parses and links a FileDescriptorSet into a compiled schema. */
export function loadSchema(bytes: Uint8Array, limits?: Limits): CoreSchema {
    checkBytes(limits, bytes.length, 'descriptor set');
    let files: PFile[];
    try {
        files = parseSet(bytes);
        linkFiles(files);
    } catch (e) {
        const message = e instanceof Error ? e.message : String(e);
        throw new CoreError('descriptor set: ' + message, 1);
    }
    return new Builder().build(files);
}

/** Number of declared fields in a message (descriptor test surface). */
export function fieldCount(def: MsgDef): number {
    return def.fields.length;
}

/** The field named `name`, or null. */
export function fieldByName(def: MsgDef, name: string): Field | null {
    const slot = def.byName.get(name);
    return slot === undefined ? null : def.fields[slot];
}

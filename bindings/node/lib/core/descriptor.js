"use strict";
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
Object.defineProperty(exports, "__esModule", { value: true });
exports.DEFAULT_LIMITS = exports.CoreSchema = exports.MESSAGE_PRESENCE = exports.REQUIRED = exports.EXPLICIT = exports.IMPLICIT = exports.K = void 0;
exports.isPackable = isPackable;
exports.effectiveDepth = effectiveDepth;
exports.checkBytes = checkBytes;
exports.parseSet = parseSet;
exports.slotOf = slotOf;
exports.loadSchema = loadSchema;
exports.fieldCount = fieldCount;
exports.fieldByName = fieldByName;
const wire_1 = require("./wire");
/* ---------------- compiled schema (what encode/decode read) ---------------- */
/** Wire kinds of the table engine (its own numbering, not protobuf's). */
exports.K = {
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
};
/** Presence kinds. IMPLICIT = proto3 plain scalar (no has-bit). */
exports.IMPLICIT = 0;
exports.EXPLICIT = 1;
exports.REQUIRED = 2;
exports.MESSAGE_PRESENCE = 3;
/** The packable numeric kinds: packed runs cross every one of them. */
function isPackable(kind) {
    return kind <= exports.K.DOUBLE;
}
class CoreSchema {
    constructor() {
        this.messages = new Map();
        this.enums = new Map();
    }
    message(fullName) {
        const def = this.messages.get(fullName);
        if (def === undefined) {
            throw new wire_1.CoreError('no such message type: ' + fullName);
        }
        return def;
    }
}
exports.CoreSchema = CoreSchema;
exports.DEFAULT_LIMITS = { maxBytes: 0, maxDepth: 0 };
function effectiveDepth(lim) {
    if (lim === undefined || lim.maxDepth === 0 || lim.maxDepth > wire_1.MAX_DEPTH) {
        return wire_1.MAX_DEPTH;
    }
    return lim.maxDepth;
}
function checkBytes(lim, n, what) {
    if (lim !== undefined && lim.maxBytes > 0 && n > lim.maxBytes) {
        throw new wire_1.CoreError(what + ' exceeds the ' + lim.maxBytes + '-byte limit', 5);
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
function pfield() {
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
function reader(b, off, len) {
    if (len < 0 || off < 0 || off + len > b.length) {
        throw new wire_1.CoreError('descriptor overrun');
    }
    return new wire_1.R(b, off, off + len);
}
function str(b, off, len) {
    return (0, wire_1.utf8Decode)(b, off, len);
}
function parseSet(bytes) {
    const r = new wire_1.R(bytes);
    const files = [];
    while (r.pos < r.end) {
        const tag = (0, wire_1.rVar)(r);
        if (tag >>> 3 === 1 && (tag & 7) === wire_1.WT_LEN) {
            const n = (0, wire_1.rLen)(r);
            files.push(parseFile(r.b, r.pos, n));
            r.pos += n;
        }
        else {
            (0, wire_1.skip)(r, tag & 7);
        }
    }
    return files;
}
function parseFile(b, off, len) {
    const r = reader(b, off, len);
    const f = {
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
        const tag = (0, wire_1.rVar)(r);
        const num = tag >>> 3;
        const wt = tag & 7;
        if (wt === wire_1.WT_VARINT) {
            // Field 14 is the edition marker; its value is irrelevant here.
            if (num === 14) {
                (0, wire_1.rVar)(r);
                f.edition = true;
            }
            else {
                (0, wire_1.skip)(r, wt);
            }
            continue;
        }
        if (wt === wire_1.WT_LEN) {
            const n = (0, wire_1.rLen)(r);
            if (num === 4) {
                f.messages.push(parseMessage(r.b, r.pos, n, 1));
            }
            else if (num === 5) {
                f.enums.push(parseEnum(r.b, r.pos, n));
            }
            else if (num === 7) {
                f.extensions.push(parseField(r.b, r.pos, n));
            }
            else if (num === 8) {
                f.enumOpen = parseFeatures(r.b, r.pos, r.pos + n, 50);
                f.utf8 = parseFeatureSetUtf8(r.b, r.pos, r.pos + n, 50);
            }
            else if (num === 1) {
                f.name = str(r.b, r.pos, n);
            }
            else if (num === 2) {
                f.pkg = str(r.b, r.pos, n);
            }
            else if (num === 12) {
                f.syntax = str(r.b, r.pos, n);
            }
            r.pos += n;
        }
        else {
            (0, wire_1.skip)(r, wt);
        }
    }
    return f;
}
function parseMessage(b, off, len, depth) {
    // Descriptor nesting is peer data too: a FileDescriptorSet a few tens of
    // KB can nest thousands of DescriptorProtos, so cap it instead of letting
    // the recursion overflow the stack.
    if (depth > wire_1.MAX_DEPTH) {
        throw new wire_1.CoreError('descriptor nesting exceeds ' + wire_1.MAX_DEPTH);
    }
    const r = reader(b, off, len);
    const m = {
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
        const tag = (0, wire_1.rVar)(r);
        const num = tag >>> 3;
        if ((tag & 7) !== wire_1.WT_LEN) {
            (0, wire_1.skip)(r, tag & 7);
            continue;
        }
        const n = (0, wire_1.rLen)(r);
        if (num === 1) {
            m.name = str(r.b, r.pos, n);
        }
        else if (num === 2) {
            m.fields.push(parseField(r.b, r.pos, n));
        }
        else if (num === 3) {
            m.nested.push(parseMessage(r.b, r.pos, n, depth + 1));
        }
        else if (num === 4) {
            m.enums.push(parseEnum(r.b, r.pos, n));
        }
        else if (num === 6) {
            m.extensions.push(parseField(r.b, r.pos, n));
        }
        else if (num === 7) {
            m.mapEntry = parseMapEntry(r.b, r.pos, n);
        }
        else if (num === 8) {
            m.oneofs.push(parseOneof(r.b, r.pos, n));
        }
        else if (num === 10) {
            m.reservedNames.push(str(r.b, r.pos, n));
        }
        r.pos += n;
    }
    if (m.name === null) {
        throw new wire_1.CoreError('message without a name');
    }
    return m;
}
function parseOneof(b, off, len) {
    const r = reader(b, off, len);
    const o = { name: '' };
    while (r.pos < r.end) {
        const tag = (0, wire_1.rVar)(r);
        if (tag >>> 3 === 1 && (tag & 7) === wire_1.WT_LEN) {
            const n = (0, wire_1.rLen)(r);
            o.name = str(r.b, r.pos, n);
            r.pos += n;
        }
        else {
            (0, wire_1.skip)(r, tag & 7);
        }
    }
    return o;
}
function parseEnum(b, off, len) {
    const r = reader(b, off, len);
    const e = { name: null, fullName: '', values: [], enumOpen: 0, closed: false };
    while (r.pos < r.end) {
        const tag = (0, wire_1.rVar)(r);
        const num = tag >>> 3;
        if ((tag & 7) !== wire_1.WT_LEN) {
            (0, wire_1.skip)(r, tag & 7);
            continue;
        }
        const n = (0, wire_1.rLen)(r);
        if (num === 1) {
            e.name = str(r.b, r.pos, n);
        }
        else if (num === 2) {
            e.values.push(parseEnumValue(r.b, r.pos, n));
        }
        else if (num === 3) {
            e.enumOpen = parseFeatures(r.b, r.pos, r.pos + n, 7);
        }
        r.pos += n;
    }
    return e;
}
function parseEnumValue(b, off, len) {
    const r = reader(b, off, len);
    const v = { name: null, number: 0 };
    let hasNumber = false;
    while (r.pos < r.end) {
        const tag = (0, wire_1.rVar)(r);
        const num = tag >>> 3;
        const wt = tag & 7;
        if (wt === wire_1.WT_VARINT) {
            if (num === 2) {
                v.number = (0, wire_1.rVar)(r) | 0;
                hasNumber = true;
            }
            else {
                (0, wire_1.skip)(r, wt);
            }
        }
        else if (wt === wire_1.WT_LEN) {
            const n = (0, wire_1.rLen)(r);
            if (num === 1) {
                v.name = str(r.b, r.pos, n);
            }
            r.pos += n;
        }
        else {
            (0, wire_1.skip)(r, wt);
        }
    }
    if (v.name === null || !hasNumber) {
        throw new wire_1.CoreError('enum value without name/number');
    }
    return v;
}
/** MessageOptions.map_entry is field 7. */
function parseMapEntry(b, off, len) {
    const r = reader(b, off, len);
    let mapEntry = false;
    while (r.pos < r.end) {
        const tag = (0, wire_1.rVar)(r);
        if ((tag & 7) !== wire_1.WT_VARINT) {
            (0, wire_1.skip)(r, tag & 7);
            continue;
        }
        if (tag >>> 3 === 7) {
            mapEntry = (0, wire_1.rVar)(r) !== 0;
        }
        else {
            (0, wire_1.skip)(r, wire_1.WT_VARINT);
        }
    }
    return mapEntry;
}
/**
 * Reads one Options message and returns FeatureSet.enum_type (1 OPEN, 2
 * CLOSED) when it carries one; 0 means unset. `optionNumber` is the field
 * holding the FeatureSet (50 file, 21 field, ...).
 */
function parseFeatures(b, off, end, optionNumber) {
    const r = reader(b, off, end - off);
    let open = 0;
    while (r.pos < r.end) {
        const tag = (0, wire_1.rVar)(r);
        const num = tag >>> 3;
        if ((tag & 7) !== wire_1.WT_LEN) {
            (0, wire_1.skip)(r, tag & 7);
            continue;
        }
        const n = (0, wire_1.rLen)(r);
        if (num === optionNumber) {
            open = parseFeatureSetEnumType(r.b, r.pos, r.pos + n);
        }
        r.pos += n;
    }
    return open;
}
/** FileOptions.features (50) -> FeatureSet.utf8_validation. */
function parseFeatureSetUtf8(b, off, end, optionNumber) {
    const r = reader(b, off, end - off);
    let utf8 = 0;
    while (r.pos < r.end) {
        const tag = (0, wire_1.rVar)(r);
        const num = tag >>> 3;
        if ((tag & 7) !== wire_1.WT_LEN) {
            (0, wire_1.skip)(r, tag & 7);
            continue;
        }
        const n = (0, wire_1.rLen)(r);
        if (num === optionNumber) {
            utf8 = parseFeatureSetUtf8Value(r.b, r.pos, r.pos + n);
        }
        r.pos += n;
    }
    return utf8;
}
/** FeatureSet.enum_type = 2: 1 OPEN or 2 CLOSED. */
function parseFeatureSetEnumType(b, off, end) {
    const r = reader(b, off, end - off);
    let open = 0;
    while (r.pos < r.end) {
        const tag = (0, wire_1.rVar)(r);
        if (tag >>> 3 === 2 && (tag & 7) === wire_1.WT_VARINT) {
            const v = (0, wire_1.rVar)(r) | 0;
            if (v === 1 || v === 2) {
                open = v;
            }
        }
        else {
            (0, wire_1.skip)(r, tag & 7);
        }
    }
    return open;
}
/** FeatureSet.utf8_validation = 4: 2 VERIFY or 3 NONE. */
function parseFeatureSetUtf8Value(b, off, end) {
    const r = reader(b, off, end - off);
    let utf8 = 0;
    while (r.pos < r.end) {
        const tag = (0, wire_1.rVar)(r);
        if (tag >>> 3 === 4 && (tag & 7) === wire_1.WT_VARINT) {
            const v = (0, wire_1.rVar)(r) | 0;
            if (v === 2 || v === 3) {
                utf8 = v;
            }
        }
        else {
            (0, wire_1.skip)(r, tag & 7);
        }
    }
    return utf8;
}
function parseField(b, off, len) {
    const r = reader(b, off, len);
    const f = pfield();
    while (r.pos < r.end) {
        const tag = (0, wire_1.rVar)(r);
        const num = tag >>> 3;
        const wt = tag & 7;
        if (wt === wire_1.WT_VARINT) {
            const v = (0, wire_1.rVar)(r);
            if (num === 3) {
                f.number = v | 0;
            }
            else if (num === 4) {
                f.label = v | 0;
            }
            else if (num === 5) {
                f.type = v | 0;
            }
            else if (num === 9) {
                f.oneofIndex = v | 0;
            }
            else if (num === 17) {
                f.proto3Optional = v !== 0;
            }
        }
        else if (wt === wire_1.WT_LEN) {
            const n = (0, wire_1.rLen)(r);
            if (num === 1) {
                f.name = str(r.b, r.pos, n);
            }
            else if (num === 2) {
                f.extendee = str(r.b, r.pos, n);
            }
            else if (num === 6) {
                f.typeName = str(r.b, r.pos, n);
            }
            else if (num === 8) {
                parseFieldOptions(r.b, r.pos, r.pos + n, f);
            }
            r.pos += n;
        }
        else {
            (0, wire_1.skip)(r, wt);
        }
    }
    if (f.name === null || f.type === 0 || f.number === 0) {
        throw new wire_1.CoreError('field without name/type/number');
    }
    if (f.type < T_DOUBLE || f.type > T_SINT64) {
        throw new wire_1.CoreError('bad field type ' + f.type);
    }
    if (f.label < L_OPTIONAL || f.label > L_REPEATED) {
        throw new wire_1.CoreError('bad label ' + f.label);
    }
    return f;
}
/** FieldOptions: packed (2) and the editions FeatureSet (21). */
function parseFieldOptions(b, off, end, f) {
    const r = reader(b, off, end - off);
    while (r.pos < r.end) {
        const tag = (0, wire_1.rVar)(r);
        const num = tag >>> 3;
        if (num === 2 && (tag & 7) === wire_1.WT_VARINT) {
            f.packed = (0, wire_1.rVar)(r) !== 0;
            f.hasPackedOption = true;
        }
        else if (num === 21 && (tag & 7) === wire_1.WT_LEN) {
            const n = (0, wire_1.rLen)(r);
            f.enumOpen = parseFeatureSetEnumType(r.b, r.pos, r.pos + n);
            f.utf8 = parseFeatureSetUtf8Value(r.b, r.pos, r.pos + n);
            r.pos += n;
        }
        else {
            (0, wire_1.skip)(r, tag & 7);
        }
    }
}
/* ---------------- linking ---------------- */
function indexMessage(m, scope, byName, enums) {
    m.fullName = scope === '' ? m.name : scope + '.' + m.name;
    if (byName.has(m.fullName)) {
        throw new wire_1.CoreError('duplicate message ' + m.fullName);
    }
    byName.set(m.fullName, m);
    for (const e of m.enums) {
        indexEnum(e, m.fullName, enums);
    }
    for (const n of m.nested) {
        indexMessage(n, m.fullName, byName, enums);
    }
}
function indexEnum(e, scope, enums) {
    e.fullName = scope === '' ? e.name : scope + '.' + e.name;
    if (enums.has(e.fullName)) {
        throw new wire_1.CoreError('duplicate enum ' + e.fullName);
    }
    enums.set(e.fullName, e);
}
/** An enum is closed for proto2 files (and editions files asking for it). */
function linkEnum(e, proto3, fileEnumOpen) {
    if (e.name === null) {
        throw new wire_1.CoreError('enum without a name');
    }
    if (e.enumOpen !== 0) {
        e.closed = e.enumOpen === 2;
    }
    else if (fileEnumOpen !== 0) {
        e.closed = fileEnumOpen === 2;
    }
    else {
        e.closed = !proto3;
    }
}
function linkEnums(m, proto3, fileEnumOpen) {
    for (const e of m.enums) {
        linkEnum(e, proto3, fileEnumOpen);
    }
    for (const n of m.nested) {
        linkEnums(n, proto3, fileEnumOpen);
    }
}
function linkMessage(m, scope, proto3, fileEnumOpen, fileUtf8, byName, enums) {
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
function isPackableType(t) {
    return !(t === T_STRING || t === T_GROUP || t === T_MESSAGE || t === T_BYTES);
}
function linkField(f, scope, proto3, fileUtf8, byName, enums, parent) {
    if (f.oneofIndex >= 0 && parent !== null) {
        if (f.oneofIndex >= parent.oneofs.length) {
            throw new wire_1.CoreError('field ' + f.name + ': oneof_index out of range');
        }
        f.oneof = parent.oneofs[f.oneofIndex];
    }
    if ((f.type === T_MESSAGE || f.type === T_GROUP) && f.typeName !== null) {
        f.messageType = resolve(byName, scope, f.typeName);
        if (f.messageType === null) {
            throw new wire_1.CoreError('field ' + f.name + ': no such type ' + f.typeName);
        }
    }
    if (f.type === T_ENUM && f.typeName !== null) {
        f.enumType = resolveEnum(enums, scope, f.typeName);
        if (f.enumType === null) {
            throw new wire_1.CoreError('field ' + f.name + ': no such enum ' + f.typeName);
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
    }
    else {
        f.utf8 = 3;
    }
    f.hasPresence =
        f.label !== L_REPEATED &&
            (f.type === T_MESSAGE || f.type === T_GROUP || f.label === L_REQUIRED || !proto3 || f.proto3Optional);
}
/** Leading-dot names resolve from the root; otherwise search the scope up. */
function resolve(byName, scope, name) {
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
function resolveEnum(byName, scope, name) {
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
function linkFiles(files) {
    const byName = new Map();
    const enums = new Map();
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
function kindOf(t) {
    switch (t) {
        case T_INT32:
            return exports.K.INT32;
        case T_UINT32:
            return exports.K.UINT32;
        case T_SINT32:
            return exports.K.SINT32;
        case T_INT64:
            return exports.K.INT64;
        case T_UINT64:
            return exports.K.UINT64;
        case T_SINT64:
            return exports.K.SINT64;
        case T_BOOL:
            return exports.K.BOOL;
        case T_ENUM:
            return exports.K.ENUM;
        case T_FIXED32:
            return exports.K.FIXED32;
        case T_SFIXED32:
            return exports.K.SFIXED32;
        case T_FLOAT:
            return exports.K.FLOAT;
        case T_FIXED64:
            return exports.K.FIXED64;
        case T_SFIXED64:
            return exports.K.SFIXED64;
        case T_DOUBLE:
            return exports.K.DOUBLE;
        case T_STRING:
            return exports.K.STRING;
        case T_BYTES:
            return exports.K.BYTES;
        case T_MESSAGE:
            return exports.K.MESSAGE;
        default:
            return exports.K.GROUP;
    }
}
function wireType(t, packed) {
    if (packed) {
        return wire_1.WT_LEN;
    }
    switch (t) {
        case T_DOUBLE:
        case T_FIXED64:
        case T_SFIXED64:
            return 1;
        case T_STRING:
        case T_BYTES:
        case T_MESSAGE:
            return wire_1.WT_LEN;
        case T_GROUP:
            return 3;
        case T_FIXED32:
        case T_SFIXED32:
        case T_FLOAT:
            return 5;
        default:
            return wire_1.WT_VARINT;
    }
}
class Builder {
    constructor() {
        this.schema = new CoreSchema();
    }
    build(files) {
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
    buildEnum(e) {
        const d = {
            fullName: e.fullName,
            byName: new Map(),
            byNumber: new Map(),
            closed: e.closed,
        };
        for (const v of e.values) {
            const name = v.name;
            if (d.byName.has(name)) {
                throw new wire_1.CoreError('duplicate enum value ' + d.fullName + '.' + name);
            }
            d.byName.set(name, v.number);
            // Aliases repeat a number: the first declared name is printed.
            if (!d.byNumber.has(v.number)) {
                d.byNumber.set(v.number, name);
            }
        }
        this.schema.enums.set(d.fullName, d);
    }
    buildEnums(m, depth) {
        if (depth > wire_1.MAX_DEPTH) {
            throw new wire_1.CoreError('descriptor nesting exceeds ' + wire_1.MAX_DEPTH);
        }
        for (const e of m.enums) {
            this.buildEnum(e);
        }
        for (const n of m.nested) {
            this.buildEnums(n, depth + 1);
        }
    }
    buildDef(m, depth) {
        if (depth > wire_1.MAX_DEPTH) {
            throw new wire_1.CoreError('descriptor nesting exceeds ' + wire_1.MAX_DEPTH);
        }
        const done = this.schema.messages.get(m.fullName);
        if (done !== undefined) {
            return done;
        }
        const d = {
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
        const fs = [];
        const oneofs = new Map();
        for (const f of m.fields) {
            fs.push(this.buildField(f, oneofs));
        }
        finishDef(d, fs, oneofs);
        for (const n of m.nested) {
            this.buildDef(n, depth + 1);
        }
        return d;
    }
    buildField(f, oneofs) {
        const kind = kindOf(f.type);
        const repeated = f.label === L_REPEATED;
        let oneof = -1;
        if (f.oneof !== null) {
            const idx = oneofs.get(f.oneof);
            if (idx === undefined) {
                oneof = oneofs.size;
                oneofs.set(f.oneof, oneof);
            }
            else {
                oneof = idx;
            }
        }
        let presence;
        if (oneof >= 0) {
            presence = exports.EXPLICIT; // reads/writes go through the oneof case
        }
        else if (f.label === L_REQUIRED) {
            presence = exports.REQUIRED;
        }
        else if (kind === exports.K.MESSAGE || kind === exports.K.GROUP) {
            presence = exports.MESSAGE_PRESENCE;
        }
        else if (f.hasPresence) {
            presence = exports.EXPLICIT;
        }
        else {
            presence = exports.IMPLICIT;
        }
        const packed = repeated && f.packed;
        const wt = wireType(f.type, packed);
        const number = f.number;
        const out = {
            number,
            name: f.name,
            kind,
            wt,
            elemWt: wireType(f.type, false),
            repeated,
            packed,
            oneof,
            presence,
            msgType: f.messageType === null ? null : this.buildDef(f.messageType, 1),
            enumType: f.enumType === null ? null : (this.schema.enums.get(f.enumType.fullName) ?? null),
            enumClosed: f.enumClosed,
            utf8: f.type === T_STRING && f.utf8 === 2,
            fullName: f.fullName,
            tag: (0, wire_1.tagBytes)(number, wt),
            tagEnd: kind === exports.K.GROUP ? (0, wire_1.tagBytes)(number, 4) : null,
        };
        return out;
    }
    attachMessageExtensions(m) {
        for (const e of m.extensions) {
            this.attachExtension(e);
        }
        for (const n of m.nested) {
            this.attachMessageExtensions(n);
        }
    }
    attachExtension(e) {
        let extendee = e.extendee;
        if (extendee === null) {
            throw new wire_1.CoreError('extension without extendee');
        }
        if (extendee.startsWith('.')) {
            extendee = extendee.substring(1);
        }
        const d = this.schema.messages.get(extendee);
        if (d === undefined) {
            throw new wire_1.CoreError('extendee missing: ' + e.extendee);
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
function finishDef(d, fs, oneofs) {
    fs.sort((a, b) => a.number - b.number);
    let max = 0;
    let req = false;
    for (const f of fs) {
        if (f.number > max) {
            max = f.number;
        }
        req = req || f.presence === exports.REQUIRED;
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
    }
    else {
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
function slotOf(def, fnum) {
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
function finishTransitive(schema) {
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
function needsRequired(d) {
    for (const f of d.fields) {
        if (f.presence === exports.REQUIRED) {
            return true;
        }
        if ((f.kind === exports.K.MESSAGE || f.kind === exports.K.GROUP) &&
            f.msgType !== null &&
            f.msgType.deepRequired) {
            return true;
        }
    }
    return false;
}
/* ---------------- public entry ---------------- */
/** Parses and links a FileDescriptorSet into a compiled schema. */
function loadSchema(bytes, limits) {
    checkBytes(limits, bytes.length, 'descriptor set');
    let files;
    try {
        files = parseSet(bytes);
        linkFiles(files);
    }
    catch (e) {
        const message = e instanceof Error ? e.message : String(e);
        throw new wire_1.CoreError('descriptor set: ' + message, 1);
    }
    return new Builder().build(files);
}
/** Number of declared fields in a message (descriptor test surface). */
function fieldCount(def) {
    return def.fields.length;
}
/** The field named `name`, or null. */
function fieldByName(def, name) {
    const slot = def.byName.get(name);
    return slot === undefined ? null : def.fields[slot];
}

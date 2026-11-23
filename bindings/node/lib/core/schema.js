"use strict";
/*
 * Public surface of the pure-TypeScript core: a Schema loaded from a
 * FileDescriptorSet, message types that decode and encode wire bytes, and the
 * plain-object helpers the pbench core arm uses. No native library, no
 * protoc plugin, no generated code, no npm dependency.
 *
 *   const schema = Schema.fromFile('matrix3.desc');
 *   const type = schema.message('structtest.Scalars3');
 *   const msg = type.decode(bytes);      // unknown fields retained
 *   type.encode(msg);                     // byte-exact for a decoded tree
 */
Object.defineProperty(exports, "__esModule", { value: true });
exports.Schema = exports.MessageType = exports.encode = exports.decodeSlice = exports.decode = exports.fromObject = exports.toObject = exports.rawPayload = exports.oneofCase = exports.hasField = exports.getField = exports.Raw = exports.Msg = exports.fieldCount = exports.fieldByName = exports.REQUIRED = exports.EXPLICIT = exports.IMPLICIT = exports.K = exports.CoreError = void 0;
const descriptor_1 = require("./descriptor");
const decode_1 = require("./decode");
const encode_1 = require("./encode");
const tree_1 = require("./tree");
var wire_1 = require("./wire");
Object.defineProperty(exports, "CoreError", { enumerable: true, get: function () { return wire_1.CoreError; } });
var descriptor_2 = require("./descriptor");
Object.defineProperty(exports, "K", { enumerable: true, get: function () { return descriptor_2.K; } });
Object.defineProperty(exports, "IMPLICIT", { enumerable: true, get: function () { return descriptor_2.IMPLICIT; } });
Object.defineProperty(exports, "EXPLICIT", { enumerable: true, get: function () { return descriptor_2.EXPLICIT; } });
Object.defineProperty(exports, "REQUIRED", { enumerable: true, get: function () { return descriptor_2.REQUIRED; } });
Object.defineProperty(exports, "fieldByName", { enumerable: true, get: function () { return descriptor_2.fieldByName; } });
Object.defineProperty(exports, "fieldCount", { enumerable: true, get: function () { return descriptor_2.fieldCount; } });
var tree_2 = require("./tree");
Object.defineProperty(exports, "Msg", { enumerable: true, get: function () { return tree_2.Msg; } });
Object.defineProperty(exports, "Raw", { enumerable: true, get: function () { return tree_2.Raw; } });
Object.defineProperty(exports, "getField", { enumerable: true, get: function () { return tree_2.getField; } });
Object.defineProperty(exports, "hasField", { enumerable: true, get: function () { return tree_2.hasField; } });
Object.defineProperty(exports, "oneofCase", { enumerable: true, get: function () { return tree_2.oneofCase; } });
Object.defineProperty(exports, "rawPayload", { enumerable: true, get: function () { return tree_2.rawPayload; } });
Object.defineProperty(exports, "toObject", { enumerable: true, get: function () { return tree_2.toObject; } });
Object.defineProperty(exports, "fromObject", { enumerable: true, get: function () { return tree_2.fromObject; } });
var decode_2 = require("./decode");
Object.defineProperty(exports, "decode", { enumerable: true, get: function () { return decode_2.decode; } });
Object.defineProperty(exports, "decodeSlice", { enumerable: true, get: function () { return decode_2.decodeSlice; } });
var encode_2 = require("./encode");
Object.defineProperty(exports, "encode", { enumerable: true, get: function () { return encode_2.encode; } });
/** One message type: newMessage() plus binary decode/encode. */
class MessageType {
    constructor(def) {
        this.def = def;
    }
    /** Fully qualified name, e.g. "structtest.Scalars3". */
    name() {
        return this.def.fullName;
    }
    /** An empty, mutable message of this type. */
    newMessage() {
        return (0, decode_1.newMessage)(this.def);
    }
    /**
     * Decodes wire bytes; unknown fields are kept and re-encoded. The bytes
     * are the authority on presence, so a field absent on the wire stays
     * absent.
     */
    decode(wire, limits = descriptor_1.DEFAULT_LIMITS) {
        return (0, decode_1.decode)(this.def, wire, limits);
    }
    /** Decodes one body slice (offset + length) of a larger buffer. */
    decodeSlice(wire, off, len, limits = descriptor_1.DEFAULT_LIMITS) {
        return (0, decode_1.decodeSlice)(this.def, wire, off, len, limits);
    }
    /** Encodes a tree; a tree that came from decode() re-emits byte-exactly. */
    encode(m) {
        return (0, encode_1.encode)(m);
    }
    /** Decodes straight into a plain object (proto field names, BigInt). */
    decodeObject(wire, limits = descriptor_1.DEFAULT_LIMITS) {
        return (0, tree_1.toObject)(this.decode(wire, limits));
    }
    /** Encodes a plain object (the inverse of decodeObject). */
    encodeObject(obj) {
        return (0, encode_1.encode)((0, tree_1.fromObject)(this.def, obj));
    }
    toString() {
        return this.name();
    }
}
exports.MessageType = MessageType;
/** A loaded FileDescriptorSet: the runtime schema every type comes from. */
class Schema {
    constructor(core) {
        this.core = core;
    }
    /** Loads a descriptor set written by protoc --descriptor_set_out. */
    static fromFile(path, limits = descriptor_1.DEFAULT_LIMITS) {
        const bytes = require('fs').readFileSync(path);
        return Schema.fromBytes(bytes, limits);
    }
    /** Loads descriptor-set bytes; use --include_imports so every type resolves. */
    static fromBytes(descriptorSet, limits = descriptor_1.DEFAULT_LIMITS) {
        (0, descriptor_1.checkBytes)(limits, descriptorSet.length, 'descriptor set');
        return new Schema((0, descriptor_1.loadSchema)(descriptorSet));
    }
    /** Looks up a message by its fully qualified name. */
    message(fullName) {
        return new MessageType(this.core.message(fullName));
    }
}
exports.Schema = Schema;

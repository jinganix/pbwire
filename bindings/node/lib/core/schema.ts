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

import type { CoreSchema, Limits, MsgDef } from './descriptor';
import { DEFAULT_LIMITS, checkBytes, loadSchema } from './descriptor';
import { decode, decodeSlice, newMessage } from './decode';
import { encode } from './encode';
import type { Msg } from './tree';
import { fromObject, toObject } from './tree';

export { CoreError } from './wire';
export { K, IMPLICIT, EXPLICIT, REQUIRED, fieldByName, fieldCount } from './descriptor';
export type { Field, MsgDef, EnumDef, Limits } from './descriptor';
export type { CoreSchema } from './descriptor';
export { Msg, Raw, getField, hasField, oneofCase, rawPayload, toObject, fromObject } from './tree';
export { decode, decodeSlice } from './decode';
export { encode } from './encode';

/* No @types/node in this package: the one stdlib call the core needs. */
declare function require(name: string): { readFileSync(path: string): Uint8Array };

/** One message type: newMessage() plus binary decode/encode. */
export class MessageType {
    readonly def: MsgDef;

    constructor(def: MsgDef) {
        this.def = def;
    }

    /** Fully qualified name, e.g. "structtest.Scalars3". */
    name(): string {
        return this.def.fullName;
    }

    /** An empty, mutable message of this type. */
    newMessage(): Msg {
        return newMessage(this.def);
    }

    /**
     * Decodes wire bytes; unknown fields are kept and re-encoded. The bytes
     * are the authority on presence, so a field absent on the wire stays
     * absent.
     */
    decode(wire: Uint8Array, limits: Limits = DEFAULT_LIMITS): Msg {
        return decode(this.def, wire, limits);
    }

    /** Decodes one body slice (offset + length) of a larger buffer. */
    decodeSlice(wire: Uint8Array, off: number, len: number, limits: Limits = DEFAULT_LIMITS): Msg {
        return decodeSlice(this.def, wire, off, len, limits);
    }

    /** Encodes a tree; a tree that came from decode() re-emits byte-exactly. */
    encode(m: Msg): Uint8Array {
        return encode(m);
    }

    /** Decodes straight into a plain object (proto field names, BigInt). */
    decodeObject(wire: Uint8Array, limits: Limits = DEFAULT_LIMITS): Record<string, unknown> {
        return toObject(this.decode(wire, limits));
    }

    /** Encodes a plain object (the inverse of decodeObject). */
    encodeObject(obj: object): Uint8Array {
        return encode(fromObject(this.def, obj));
    }

    toString(): string {
        return this.name();
    }
}

/** A loaded FileDescriptorSet: the runtime schema every type comes from. */
export class Schema {
    readonly core: CoreSchema;

    private constructor(core: CoreSchema) {
        this.core = core;
    }

    /** Loads a descriptor set written by protoc --descriptor_set_out. */
    static fromFile(path: string, limits: Limits = DEFAULT_LIMITS): Schema {
        const bytes = require('fs').readFileSync(path);
        return Schema.fromBytes(bytes, limits);
    }

    /** Loads descriptor-set bytes; use --include_imports so every type resolves. */
    static fromBytes(descriptorSet: Uint8Array, limits: Limits = DEFAULT_LIMITS): Schema {
        checkBytes(limits, descriptorSet.length, 'descriptor set');
        return new Schema(loadSchema(descriptorSet));
    }

    /** Looks up a message by its fully qualified name. */
    message(fullName: string): MessageType {
        return new MessageType(this.core.message(fullName));
    }
}

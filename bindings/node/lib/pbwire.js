'use strict';

/* Node.js binding: a FileDescriptorSet becomes a schema at runtime (no code
 * generation, no protoc plugin), and messages are read and written by name.
 *
 *   const { Schema } = require('./lib/pbwire');
 *   const schema = Schema.fromFile('demo.desc');
 *   const person = schema.message('demo.Person');
 *
 *   const msg = person.newMessage();
 *   msg.set('name', 'alice');
 *   msg.set('inner', { x: 1, tag: 'kid' });
 *   const wire = msg.encode();          // Buffer
 *   msg.free();
 *
 *   const back = person.decode(wire);   // Message
 *   back.get('inner').x;                // 1
 *   back.encode().equals(wire);         // true
 *
 * A message is one of two things:
 *
 *   A built message (newMessage()/set()) is a value tree in C. Writing to it
 *   is cheap after the first field, and encode() runs the core encoder into a
 *   reusable buffer.
 *
 *   A decoded message is the bytes plus one slice per wire field: decode()
 *   validates the payload once, a value is materialized only when it is read,
 *   and encode() hands the bytes back, so a round trip is byte-exact (unknown
 *   fields, field order and non-canonical encodings included).
 *
 * Writing to a decoded message turns it into a tree (one decode of the retained
 * bytes), so the API is the same either way.
 *
 * Values map per field type:
 *
 *   int32/uint32/sint32/fixed32/sfixed32/enum   number
 *   int64/uint64/sint64/fixed64/sfixed64        BigInt
 *   float/double                                number
 *   bool                                        boolean
 *   string                                      string
 *   bytes                                       Buffer
 *   message / repeated                          object / array
 *
 * There is no int64 precision loss: 64-bit fields are BigInt, and a JS number
 * is accepted when setting (rounded the way Number->int64 rounds).
 */

const fs = require('node:fs');
const path = require('node:path');

const addon = require(path.join(__dirname, '..', 'build', 'pbwire_node.node'));

class Schema {
  constructor(handle) {
    this._handle = handle;
  }

  /** Load a FileDescriptorSet (protoc --descriptor_set_out, buf, ...). */
  static fromBytes(bytes) {
    return new Schema(addon.loadDescriptorSet(bytes));
  }

  static fromFile(file) {
    return Schema.fromBytes(fs.readFileSync(file));
  }

  /** A message type of this schema; throws when the name is not a message. */
  message(name) {
    const handle = addon.descriptor(this._handle, name);
    if (handle === null) {
      throw new Error(`pbwire: ${name} is not a message in this schema`);
    }
    return new MessageType(this, name, handle);
  }
}

class MessageType {
  constructor(schema, name, handle) {
    this.schema = schema;
    this.name = name;
    this._handle = handle;
    // A proto2 `required` field has to hold in the merged message, which is
    // what the tree decoder checks; such a type decodes eagerly.
    this._eager = addon.isEager(handle);
  }

  /** An empty message of this type. */
  newMessage() {
    const message = new Message(this);
    message._handle = addon.messageNew(this._handle);
    return message;
  }

  /** Encode a plain object or a Message built by this type. */
  encode(value) {
    if (value instanceof Message) {
      if (value.type !== this) {
        throw new TypeError(`pbwire: ${value.type.name} is not ${this.name}`);
      }
      return value.encode();
    }
    if (value === null || typeof value !== 'object') {
      throw new TypeError('pbwire: encode expects an object or a Message');
    }
    // The addon builds the tree, encodes it into the type's scratch buffer and
    // frees the tree; only the bytes cross back.
    return this._copy(addon.encodeObject(this._handle, value));
  }

  /** Decode wire bytes into a Message; throws on malformed input. */
  decode(bytes) {
    if (!Buffer.isBuffer(bytes)) {
      throw new TypeError('pbwire: decode expects a Buffer');
    }
    // A private copy: the caller may reuse its Buffer, and a decoded message
    // keeps the bytes it came from.
    const wire = Buffer.from(bytes);
    const message = new Message(this);
    if (this._eager) {
      message._handle = addon.decodeEager(this._handle, wire);
    } else {
      message._wire = wire;
      message._slices = addon.decode(this._handle, wire);
    }
    return message;
  }

  /** Copies the scratch an encode just filled into a pooled Buffer. */
  _copy(len) {
    const out = Buffer.allocUnsafe(len);
    addon.copyEncoded(this._handle, out);
    return out;
  }
}

class Message {
  constructor(type) {
    this.type = type;
    this._handle = null; // tree form (newMessage/set)
    this._wire = null; // wire form (decode): the bytes
    this._slices = null; // wire form: (field index, offset, length) triples
  }

  /** Field value by name, or undefined when the field is absent. */
  get(name) {
    this._live();
    if (this._wire !== null) {
      return addon.wireGet(this.type._handle, this._wire, this._slices, name);
    }
    return addon.get(this._handle, name);
  }

  /** Store a field value by name; `undefined`/`null` clears the field. */
  set(name, value) {
    this._live();
    if (this._wire !== null) {
      // Writing needs a mutable tree: decode the retained bytes once.
      this._handle = addon.promote(this.type._handle, this._wire);
      this._wire = null;
      this._slices = null;
    }
    addon.set(this._handle, name, value);
    return this;
  }

  /** Wire bytes of this message. */
  encode() {
    this._live();
    if (this._wire !== null) {
      // Untouched since decode: the bytes are the encoding.
      return Buffer.from(this._wire);
    }
    return this.type._copy(addon.encode(this._handle));
  }

  /** Release the tree early; calling it twice is a no-op. A decoded message
   * only drops its bytes, which the garbage collector would do anyway. */
  free() {
    if (this._handle !== null) {
      addon.free(this._handle);
      this._handle = null;
    }
    this._wire = null;
    this._slices = null;
  }

  _live() {
    if (this._handle === null && this._wire === null) {
      throw new TypeError('pbwire: message was freed');
    }
  }
}

module.exports = { Schema, MessageType, Message, abiVersion: addon.abiVersion };

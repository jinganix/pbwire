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

  /** Load a FileDescriptorSet (protoc --descriptor_set_out, buf, ...).
   * An optional second argument caps untrusted input: {maxBytes, maxDepth}. */
  static fromBytes(bytes, opts) {
    return new Schema(addon.loadDescriptorSet(bytes, opts));
  }

  static fromFile(file, opts) {
    return Schema.fromBytes(fs.readFileSync(file), opts);
  }

  /** Service names of this schema, for routing. */
  services() {
    return addon.services(this._handle);
  }

  /** One service with its methods and streaming flags; throws when unknown. */
  service(name) {
    return addon.service(this._handle, name);
  }

  /** Resolve a method to its input/output types for routing; throws when
   * the service or the method is unknown. */
  resolve(svc, method) {
    const meta = this.service(svc).methods.find((m) => m.name === method);
    if (meta === undefined) {
      throw new Error(`pbwire: ${svc} has no method ${method}`);
    }
    const res = addon.resolve(this._handle, svc, method);
    return {
      input: new MessageType(this, meta.inputType, res.input),
      output: new MessageType(this, meta.outputType, res.output),
    };
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

  /** Parse JSON mapping text into a Message; throws on malformed input.
   * A second true argument skips unknown field names instead of failing;
   * a trailing {maxBytes, maxDepth} caps untrusted input. */
  fromJson(json, ignore = false, opts) {
    if (typeof json !== 'string') {
      throw new TypeError('pbwire: fromJson expects a string');
    }
    const message = new Message(this);
    message._handle = addon.jsonToMessage(this._handle, json, ignore, opts);
    return message;
  }

  /** Parse text format into a Message; throws on malformed input. A
   * trailing {maxBytes, maxDepth} caps untrusted input. */
  fromText(text, opts) {
    if (typeof text !== 'string') {
      throw new TypeError('pbwire: fromText expects a string');
    }
    const message = new Message(this);
    message._handle = addon.textToMessage(this._handle, text, opts);
    return message;
  }

  /** Decode one length-delimited frame: {message, read}, where read counts
   * the length prefix plus the frame, so concatenated frames decode in a
   * loop. Throws on a truncated frame. A trailing {maxBytes, maxDepth}
   * caps untrusted input. */
  decodeDelimited(bytes, opts) {
    if (!Buffer.isBuffer(bytes)) {
      throw new TypeError('pbwire: decodeDelimited expects a Buffer');
    }
    const res = addon.decodeDelimited(this._handle, bytes, opts);
    const message = new Message(this);
    message._handle = res.message;
    return { message, read: res.read };
  }

  /** Decode wire bytes into a plain object in one call; throws on malformed
   * input. Same values `get()` would return, without the per-field round
   * trip: the fast path when most fields are read. Nothing is retained, so
   * unlike `decode()` the result cannot re-encode. A trailing
   * {maxBytes, maxDepth} caps untrusted input. */
  decodeObject(bytes, opts) {
    if (!Buffer.isBuffer(bytes)) {
      throw new TypeError('pbwire: decodeObject expects a Buffer');
    }
    return addon.decodeObject(this._handle, bytes, opts);
  }

  /** Decode wire bytes into a Message; throws on malformed input. A
   * trailing {maxBytes, maxDepth} caps untrusted input. */
  decode(bytes, opts) {
    if (!Buffer.isBuffer(bytes)) {
      throw new TypeError('pbwire: decode expects a Buffer');
    }
    // A private copy: the caller may reuse its Buffer, and a decoded message
    // keeps the bytes it came from.
    const wire = Buffer.from(bytes);
    const message = new Message(this);
    if (this._eager) {
      message._handle = addon.decodeEager(this._handle, wire, opts);
    } else {
      message._wire = wire;
      message._slices = addon.decode(this._handle, wire, opts);
    }
    return message;
  }

  /** Single-level scan of wire bytes without building a tree: an array of
   * {number, wireType, name, value}, where value is a BigInt for scalar
   * shapes and a Buffer for LEN payloads and groups. */
  visit(bytes, opts) {
    if (!Buffer.isBuffer(bytes)) {
      throw new TypeError('pbwire: visit expects a Buffer');
    }
    return addon.visit(this._handle, bytes, opts);
  }

  /** Pack already-encoded bytes into a new google.protobuf.Any tree. */
  anyPack(typeUrl, payload) {
    if (!Buffer.isBuffer(payload)) {
      throw new TypeError('pbwire: anyPack expects a Buffer payload');
    }
    const message = new Message(this);
    message._handle = addon.anyPack(this._handle, typeUrl, payload);
    return message;
  }

  /** A new Timestamp tree holding ms since the Unix epoch. */
  timestampFromMillis(ms) {
    const message = new Message(this);
    message._handle = addon.timestampFromMillis(this._handle, ms);
    return message;
  }

  /** A new Duration tree holding ms. */
  durationFromMillis(ms) {
    const message = new Message(this);
    message._handle = addon.durationFromMillis(this._handle, ms);
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
    addon.set(this._tree(), name, value);
    return this;
  }

  /** The mutable tree, decoding the retained bytes once when needed. */
  _tree() {
    this._live();
    if (this._wire !== null) {
      // Writing needs a mutable tree: decode the retained bytes once.
      this._handle = addon.promote(this.type._handle, this._wire);
      this._wire = null;
      this._slices = null;
    }
    return this._handle;
  }

  /** JSON mapping text of this message. */
  toJson() {
    return addon.messageToJson(this._tree());
  }

  /** Protobuf text format of this message; a false argument drops unknown
   * fields instead of printing them numerically. */
  toText(printUnknown = true) {
    return addon.messageToText(this._tree(), printUnknown);
  }

  /** Field presence by name: set (even to a zero) reads true. */
  has(name) {
    return addon.has(this._tree(), name);
  }

  /** One length-delimited frame (varint length + message). */
  encodeDelimited() {
    return addon.encodeDelimited(this._tree());
  }

  /** Deterministic bytes: map entries sorted by key, so equal messages
   * encode to equal bytes regardless of insertion order. */
  encodeDeterministic() {
    return addon.encodeDeterministic(this._tree());
  }

  /** Exact byte count encode() would write for this message. */
  encodedSize() {
    return addon.encodedSize(this._tree());
  }

  /** Encode into a fresh Buffer sized encodedSize() + extra; the writer
   * over-reserves tag room, so the default extra of 12 always fits. */
  encodeTo(extra = 12) {
    const buf = Buffer.allocUnsafe(this.encodedSize() + extra);
    return buf.subarray(0, addon.encodeTo(this._tree(), buf));
  }

  /** One length-delimited frame into a fresh Buffer sized to fit. */
  encodeDelimitedTo(extra = 16) {
    const buf = Buffer.allocUnsafe(this.encodedSize() + extra);
    return buf.subarray(0, addon.encodeDelimitedTo(this._tree(), buf));
  }

  /** Stream the encoding through the callback in chunks; the callback
   * returns true to continue, false to abort (which throws). */
  encodeSink(callback) {
    addon.encodeSink(this._tree(), callback);
  }

  /** type_url of an Any message, or null when absent. */
  anyTypeUrl() {
    return addon.anyTypeUrl(this._tree());
  }

  /** value bytes of an Any message, or null when absent. */
  anyPayload() {
    return addon.anyPayload(this._tree());
  }

  /** Milliseconds since the Unix epoch of a Timestamp message. */
  timestampMillis() {
    return addon.timestampMillis(this._tree());
  }

  /** Milliseconds of a Duration message. */
  durationMillis() {
    return addon.durationMillis(this._tree());
  }

  /** Path strings of a FieldMask message. */
  fieldmaskPaths() {
    return addon.fieldmaskPaths(this._tree());
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

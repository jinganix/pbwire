'use strict';

/* Round trip through the Node.js binding: build a message, encode it, decode
 * the bytes, compare. Run it after bindings/node/build.sh:
 *
 *   node bindings/node/examples/roundtrip.js
 */

const assert = require('node:assert');
const path = require('node:path');

const { Schema, abiVersion } = require(path.join(__dirname, '..', 'lib', 'pbwire'));

const DESC = path.join(__dirname, '..', 'tests', 'data', 'demo.desc');

assert.strictEqual(abiVersion(), 1, 'core ABI version');

const schema = Schema.fromFile(DESC);
const person = schema.message('demo.Person');

// Build from a plain object (MessageType.encode) ...
const values = {
  name: 'alice',
  id: 7,
  active: true,
  big: 9223372036854775807n,
  inner: { x: 42, tag: 'first' },
  nums: [1, -5, 300],
  kids: [{ x: 1 }, { x: 2 }],
};
const wire = person.encode(values);

// ... and by hand (Message.set), which must produce the same bytes.
const built = person.newMessage();
built.set('name', values.name).set('id', values.id).set('active', true);
built.set('big', values.big);
built.set('inner', values.inner);
built.set('nums', values.nums);
built.set('kids', values.kids);
assert.ok(built.encode().equals(wire), 'both build paths encode the same');
built.free();
built.free(); // double free is safe

// Decode returns a Message over the bytes: reading is by field name ...
const message = person.decode(wire);
assert.strictEqual(message.get('name'), 'alice');
assert.strictEqual(message.get('id'), 7);
assert.strictEqual(message.get('active'), true);
assert.strictEqual(message.get('big'), 9223372036854775807n);
assert.deepStrictEqual(message.get('inner'), { x: 42, tag: 'first' });
assert.deepStrictEqual(message.get('nums'), [1, -5, 300]);
assert.deepStrictEqual(message.get('kids'), [{ x: 1 }, { x: 2 }]);
assert.strictEqual(message.get('blob'), undefined, 'absent field is undefined');

// ... and re-encoding a decoded message is byte exact.
assert.ok(message.encode().equals(wire), 'decode/encode round trip is exact');
message.free();

const last = person.decode(wire);
console.log(`roundtrip ok: ${wire.length} bytes, name=${last.get('name')}`);
last.free();

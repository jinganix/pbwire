'use strict';

/* Node.js binding self-check: node:test + node:assert, no dependencies.
 * Run via bindings/node/build.sh or `node --test bindings/node/test/`.
 *
 * The fixtures are three descriptor sets: demo (proto3, one of each field
 * kind), shapes (oneof, packed/unpacked repetition, optional, recursion) and
 * legacy (proto2: closed enum, group, required).
 */

const test = require('node:test');
const assert = require('node:assert');
const path = require('node:path');

const { Schema, Message, MessageType, abiVersion } = require(
  path.join(__dirname, '..', 'lib', 'pbwire'));

const DATA = path.join(__dirname, '..', 'tests', 'data');
const schema = Schema.fromFile(path.join(DATA, 'demo.desc'));
const person = schema.message('demo.Person');
const shapes = Schema.fromFile(path.join(DATA, 'shapes.desc'));
const one = shapes.message('shapes.One');
const node = shapes.message('shapes.Node');
const wrap = shapes.message('shapes.Wrap');
const legacy = Schema.fromFile(path.join(DATA, 'legacy.desc'));
const ancient = legacy.message('legacy.Legacy');
const strict = legacy.message('legacy.Strict');

const ONE_FIELDS = ['packed_nums', 'loose_nums', 'kinds', 'maybe', 'leaves',
                    'num', 'text', 'leaf'];

const VALUES = {
  name: 'alice',
  id: 7,
  active: true,
  blob: Buffer.from([0x00, 0xff, 0x10]),
  big: 9223372036854775807n,
  inner: { x: 42, tag: 'first' },
  nums: [1, -5, 300],
  kids: [{ x: 1, tag: 'a' }, { x: 2 }],
};

const ONE_VALUES = {
  packed_nums: [1, -2, 300, -2147483648],
  loose_nums: [4, 5, 0],
  kinds: [2, 0, 1],
  maybe: -1,
  leaves: [{ n: 1 }, { n: 2 }],
  text: 'oneof value',
};

/* A varint, for payloads that have to be built by hand. */
function varint(value) {
  const bytes = [];
  while (value > 0x7f) {
    bytes.push((value & 0x7f) | 0x80);
    value >>>= 7;
  }
  bytes.push(value);
  return Buffer.from(bytes);
}

test('loads a FileDescriptorSet at runtime', () => {
  assert.strictEqual(abiVersion(), 1);
  assert.ok(schema instanceof Schema);
  assert.ok(person instanceof MessageType);
  assert.strictEqual(person.name, 'demo.Person');

  // The same bytes load through fromBytes, and only messages are exposed.
  const fromBytes = Schema.fromBytes(require('node:fs').readFileSync(
    path.join(DATA, 'demo.desc')));
  assert.strictEqual(fromBytes.message('demo.Inner').name, 'demo.Inner');
  assert.throws(() => schema.message('demo.Inner.nope'), /not a message/);
  assert.throws(() => schema.message('demo.missing'), /not a message/);
});

test('builds, encodes and decodes every field kind', () => {
  const wire = person.encode(VALUES);
  assert.ok(Buffer.isBuffer(wire));

  const message = person.decode(wire);
  assert.ok(message instanceof Message);
  assert.strictEqual(message.get('name'), 'alice');
  assert.strictEqual(message.get('id'), 7);
  assert.strictEqual(message.get('active'), true);
  assert.ok(message.get('blob').equals(VALUES.blob));
  assert.strictEqual(message.get('big'), 9223372036854775807n, 'int64 is BigInt');
  assert.deepStrictEqual(message.get('inner'), VALUES.inner);
  assert.deepStrictEqual(message.get('nums'), [1, -5, 300]);
  assert.deepStrictEqual(message.get('kids'), VALUES.kids);
  assert.strictEqual(message.type, person);
  message.free();
});

test('a decoded message re-encodes to the same bytes', () => {
  const wire = person.encode(VALUES);
  const message = person.decode(wire);
  assert.ok(message.encode().equals(wire), 'byte-exact re-encode');

  // A second trip through the parser is stable too.
  assert.ok(person.decode(message.encode()).encode().equals(wire));
  assert.ok(person.encode(message).equals(wire), 'encode(Message)');

  // Unknown fields survive: append one and the pass-through keeps it.
  const withUnknown = Buffer.concat([wire, Buffer.from([0x98, 0x06, 0x2a])]);
  const passthrough = person.decode(withUnknown);
  assert.ok(passthrough.encode().equals(withUnknown), 'unknown field kept');
  assert.strictEqual(passthrough.get('name'), 'alice');
  message.free();
  passthrough.free();
});

test('absent fields read as undefined and are not written', () => {
  const empty = person.newMessage();
  for (const name of ['name', 'id', 'inner', 'nums', 'blob', 'big']) {
    assert.strictEqual(empty.get(name), undefined, `${name} absent`);
  }
  assert.strictEqual(empty.encode().length, 0);

  // Setting then clearing removes the field again.
  empty.set('id', 5);
  assert.strictEqual(empty.get('id'), 5);
  empty.set('id', undefined);
  assert.strictEqual(empty.get('id'), undefined);
  assert.strictEqual(empty.encode().length, 0);

  // An empty message decodes back to an empty one.
  const decoded = person.decode(Buffer.alloc(0));
  assert.strictEqual(decoded.get('name'), undefined);
  decoded.free();
  empty.free();
});

test('nested messages are set, read and replaced', () => {
  const message = person.newMessage();
  message.set('inner', { x: 1, tag: 'a' });
  assert.deepStrictEqual(message.get('inner'), { x: 1, tag: 'a' });

  message.set('inner', { x: 2 }); // a message field is replaced, not merged
  assert.deepStrictEqual(message.get('inner'), { x: 2 });

  // A repeated write replaces the whole list.
  message.set('kids', [{ x: 1 }, { x: 2 }]);
  message.set('kids', [{ x: 3 }]);
  const decoded = person.decode(message.encode());
  assert.deepStrictEqual(decoded.get('kids'), [{ x: 3 }]);

  assert.throws(() => message.set('inner', { nope: 1 }), /has no field nope/);
  assert.throws(() => message.set('inner', 5), /takes an object/);
  assert.throws(() => message.set('nums', 5), /takes an array/);
  assert.throws(() => message.set('name', 5), /does not match the field type/);
  assert.throws(() => message.set('missing', 5), /has no field missing/);
  assert.throws(() => message.get('missing'), /has no field missing/);
  message.free();
  decoded.free();
});

test('schema and wire errors are thrown, not crashes', () => {
  assert.throws(() => Schema.fromBytes(Buffer.from('not a descriptor set')),
                /pbwire:/);
  assert.throws(() => person.decode(Buffer.from([0x08, 0x80])),
                /pbwire: decode/, 'truncated varint');
  assert.throws(() => person.decode(Buffer.from([0x00])),
                /pbwire: decode/, 'field number 0');
  assert.throws(() => person.decode(Buffer.from([0x08, 0x01, 0x0e])),
                /pbwire: decode/, 'wire type 6');
  assert.throws(() => person.decode(Buffer.from([0x08, 0x01, 0x0f])),
                /pbwire: decode/, 'wire type 7');
  assert.throws(() => person.decode(Buffer.from([0x0a, 0x05, 0x61])),
                /pbwire: decode/, 'length past the end');
  assert.throws(() => person.decode(Buffer.from([0x0a, 0xff, 0xff, 0xff, 0xff,
                                                 0x1f])),
                /pbwire: decode/, 'length above 32 bits');
  assert.throws(() => person.decode(Buffer.from([0x0c])),
                /pbwire: decode/, 'stray END_GROUP');
  assert.throws(() => person.decode('not a buffer'), /expects a Buffer/);
  assert.throws(() => Schema.fromBytes('not a buffer'), /expects a Buffer/);
  assert.throws(() => Schema.fromFile('/no/such/file.desc'), /ENOENT/);
});

test('free() is safe twice and use after free throws', () => {
  const message = person.newMessage();
  message.set('id', 1);
  message.free();
  message.free(); // no-op, not a double free of the tree
  assert.throws(() => message.get('id'), /freed/);
  assert.throws(() => message.encode(), /freed/);
  assert.throws(() => message.set('id', 2), /freed/);

  // A decoded message is released the same way.
  const decoded = person.decode(person.encode(VALUES));
  decoded.free();
  decoded.free();
  assert.throws(() => decoded.get('name'), /freed/);

  // The descriptor stays usable after its message is gone.
  const next = person.newMessage();
  next.set('id', 3);
  assert.strictEqual(person.decode(next.encode()).get('id'), 3);
  next.free();
});

test('oneof: the last member on the wire is the one that reads back', () => {
  const wire = one.encode(ONE_VALUES);
  const decoded = one.decode(wire);
  assert.strictEqual(decoded.get('text'), 'oneof value');
  assert.strictEqual(decoded.get('num'), undefined, 'a later member wins');
  assert.strictEqual(decoded.get('leaf'), undefined);

  // num arrives after text, so text is gone.
  const later = one.newMessage();
  later.set('text', 'first').set('num', 4);
  const second = one.decode(later.encode());
  assert.strictEqual(second.get('num'), 4);
  assert.strictEqual(second.get('text'), undefined);

  // Reading a nested message keeps only the live member of its oneof too.
  const nested = wrap.decode(wrap.encode({ one: ONE_VALUES }));
  assert.deepStrictEqual(nested.get('one'), {
    packed_nums: ONE_VALUES.packed_nums,
    loose_nums: ONE_VALUES.loose_nums,
    kinds: ONE_VALUES.kinds,
    maybe: ONE_VALUES.maybe,
    leaves: ONE_VALUES.leaves,
    text: 'oneof value',
  });
  // Two members of one group in one payload: the nested object carries only
  // the live one, exactly like get() on the top-level message.
  const inner = Buffer.concat([one.encode({ text: 'first' }),
                               one.encode({ num: 7 })]);
  const outer = Buffer.concat([Buffer.from([0x0a]), varint(inner.length), inner]);
  assert.deepStrictEqual(wrap.decode(outer).get('one'), { num: 7 });

  // An empty sub-message that is on the wire reads as an empty object.
  assert.deepStrictEqual(wrap.decode(wrap.encode({ one: {} })).get('one'), {});

  // Setting a member on a decoded message clears its sibling.
  const edited = one.decode(wire);
  edited.set('num', 11);
  assert.strictEqual(edited.get('num'), 11);
  assert.strictEqual(edited.get('text'), undefined);
  const back = one.decode(edited.encode());
  assert.strictEqual(back.get('num'), 11);
  assert.strictEqual(back.get('text'), undefined);
});

test('packed and unpacked repeated fields read back in wire order', () => {
  const wire = one.encode(ONE_VALUES);
  const decoded = one.decode(wire);
  assert.deepStrictEqual(decoded.get('packed_nums'), ONE_VALUES.packed_nums);
  assert.deepStrictEqual(decoded.get('loose_nums'), ONE_VALUES.loose_nums);
  assert.deepStrictEqual(decoded.get('kinds'), ONE_VALUES.kinds);
  assert.deepStrictEqual(decoded.get('leaves'), ONE_VALUES.leaves);
  assert.strictEqual(decoded.get('maybe'), ONE_VALUES.maybe);

  // The same field may arrive packed and unpacked in one message.
  const mixed = Buffer.concat([
    Buffer.from([0x0a, 0x03, 0x01, 0x02, 0x03]), // packed_nums packed: 1 2 3
    Buffer.from([0x08, 0x04]), // packed_nums unpacked: 4
  ]);
  assert.deepStrictEqual(one.decode(mixed).get('packed_nums'), [1, 2, 3, 4]);

  // An empty packed run carries no value, like the tree decoder.
  assert.strictEqual(one.decode(Buffer.from([0x0a, 0x00])).get('packed_nums'),
                     undefined);
});

test('proto2 closed enums and groups read back like the tree decoder', () => {
  const built = ancient.newMessage();
  built.set('color', 1).set('colors', [1, 0]).set('plain', 7);
  built.set('items', { x: 3, y: 'grp' });
  const wire = built.encode();
  const decoded = ancient.decode(wire);
  assert.strictEqual(decoded.get('color'), 1);
  assert.deepStrictEqual(decoded.get('colors'), [1, 0]);
  assert.strictEqual(decoded.get('plain'), 7);
  assert.deepStrictEqual(decoded.get('items'), { x: 3, y: 'grp' });
  assert.ok(decoded.encode().equals(wire));

  // An enum number the declaration does not list is not a value of the field
  // (the reference parser moves it to the unknown fields).
  const unknown = ancient.newMessage();
  unknown.set('color', 9);
  const raw = unknown.encode();
  const read = ancient.decode(raw);
  assert.strictEqual(read.get('color'), undefined);
  assert.strictEqual(read.get('colors'), undefined);
  assert.ok(read.encode().equals(raw), 'the unknown value is still passed on');

  // The repeated form drops it too, and the tree decoder agrees.
  const repeated = ancient.newMessage();
  repeated.set('colors', [9, 1]);
  const mixed = repeated.encode();
  assert.deepStrictEqual(ancient.decode(mixed).get('colors'), [1]);
});

test('proto2 required fields decode eagerly and are enforced', () => {
  const built = strict.newMessage();
  built.set('id', 5).set('legacy', { plain: 2 });
  const wire = built.encode();
  const decoded = strict.decode(wire);
  assert.strictEqual(decoded.get('id'), 5);
  assert.deepStrictEqual(decoded.get('legacy'), { plain: 2 });
  assert.ok(decoded.encode().equals(wire));

  assert.throws(() => strict.decode(Buffer.alloc(0)), /required/);
  assert.throws(() => strict.decode(Buffer.from([0x12, 0x00])), /required/);
});

test('editing a decoded message keeps everything else it carried', () => {
  const wire = one.encode(ONE_VALUES);
  const withUnknown = Buffer.concat([wire, Buffer.from([0xf8, 0x06, 0x2a])]);
  const message = one.decode(withUnknown);
  message.set('maybe', 9);
  const reread = one.decode(message.encode());
  assert.strictEqual(reread.get('maybe'), 9);
  assert.deepStrictEqual(reread.get('packed_nums'), ONE_VALUES.packed_nums);
  assert.deepStrictEqual(reread.get('loose_nums'), ONE_VALUES.loose_nums);
  assert.deepStrictEqual(reread.get('kinds'), ONE_VALUES.kinds);
  assert.deepStrictEqual(reread.get('leaves'), ONE_VALUES.leaves);
  assert.strictEqual(reread.get('text'), 'oneof value');
});

test('nesting is capped in both directions instead of overflowing', () => {
  const deep = (depth) => {
    let bytes = Buffer.alloc(0);
    for (let i = 0; i < depth; i++) {
      const body = Buffer.concat([Buffer.from([0x10, 0x01]), bytes]);
      bytes = Buffer.concat([Buffer.from([0x0a]), varint(body.length), body]);
    }
    return bytes;
  };
  let value = node.decode(deep(100)).get('next');
  for (let level = 1; level < 100; level++) {
    value = value.next;
  }
  assert.deepStrictEqual(value, { n: 1 });
  assert.throws(() => node.decode(deep(400)), /nesting limit/);

  // Building past the cap fails with an error, not a stack overflow.
  let deeper = { n: 1 };
  for (let i = 0; i < 400; i++) {
    deeper = { next: deeper };
  }
  assert.throws(() => node.encode(deeper), /pbwire:/);
});

test('everything the wire view accepts, the tree decoder accepts too', () => {
  const wire = one.encode(ONE_VALUES);
  const variants = [wire];
  for (let cut = 0; cut < wire.length; cut++) {
    variants.push(wire.subarray(0, cut));
  }
  for (let i = 0; i < wire.length; i++) {
    for (const flip of [0x01, 0x40, 0x80, 0xff]) {
      const copy = Buffer.from(wire);
      copy[i] ^= flip;
      variants.push(copy);
    }
  }
  let accepted = 0;
  for (const bytes of variants) {
    let lazy;
    try {
      lazy = one.decode(bytes);
    } catch {
      continue; // rejecting is always allowed; the tree decoder may agree
    }
    accepted++;
    assert.ok(lazy.encode().equals(bytes), 'a decoded message is its bytes');

    // Promote the same bytes (a no-op write) and compare every field: the two
    // materializers have to agree on what the wire says.
    const tree = one.decode(bytes);
    tree.set('maybe', tree.get('maybe'));
    for (const name of ONE_FIELDS) {
      assert.deepStrictEqual(lazy.get(name), tree.get(name),
                             `${name} on ${bytes.toString('hex')}`);
    }
  }
  assert.ok(accepted > wire.length / 2,
            `the corpus decoded ${accepted}/${variants.length}`);
});

test('messages stay valid when their schema is collected first', () => {
  // The C side keeps the descriptor alive for a tree message, and a decoded
  // message keeps its type; either way nothing may be read after a free.
  const survivors = [];
  (function () {
    const local = Schema.fromFile(path.join(DATA, 'shapes.desc'));
    const type = local.message('shapes.One');
    const wire = type.encode(ONE_VALUES);
    for (let i = 0; i < 8; i++) {
      survivors.push(type.decode(wire));
      const built = type.newMessage();
      built.set('maybe', i);
      survivors.push(built);
    }
  })();
  if (global.gc) {
    for (let i = 0; i < 3; i++) {
      global.gc();
    }
  }
  for (const message of survivors) {
    if (message.get('maybe') === undefined) {
      assert.strictEqual(message.get('text'), 'oneof value');
    } else {
      assert.ok(Number.isInteger(message.get('maybe')));
    }
    assert.ok(Buffer.isBuffer(message.encode()));
  }
});

test('JSON mapping round-trips through the addon', () => {
  const message = person.decode(person.encode(VALUES));
  const json = message.toJson();
  assert.strictEqual(typeof json, 'string');
  assert.ok(json.includes('"name":"alice"'), 'string field prints');
  assert.ok(json.includes('"big":"9223372036854775807"'), 'int64 prints quoted');
  const back = person.fromJson(json);
  assert.deepStrictEqual(back.get('inner'), VALUES.inner);
  assert.strictEqual(back.get('big'), 9223372036854775807n);
  assert.ok(back.encode().equals(message.encode()), 'JSON round-trip is wire-equal');
  assert.throws(() => person.fromJson('{nope'), /./, 'malformed JSON throws');
  assert.throws(() => person.fromJson('{"unknown_field":1}'), /./, 'unknown JSON field throws');
  const ignored = person.fromJson('{"unknown_field":1}', true);
  assert.strictEqual(ignored.get('name'), undefined, 'ignored unknown field reads as absent');
  message.free();
  back.free();
  ignored.free();
});

test('text format round-trips through the addon', () => {
  const message = person.decode(person.encode(VALUES));
  const text = message.toText();
  assert.strictEqual(typeof text, 'string');
  assert.ok(text.includes('name: "alice"'), 'string field prints');
  const back = person.fromText(text);
  assert.ok(back.encode().equals(message.encode()), 'text round-trip is wire-equal');
  assert.throws(() => person.fromText('name: '), /./, 'malformed text throws');
  message.free();
  back.free();

  // Groups print and parse like nested messages.
  const g = ancient.decode(ancient.encode({ color: 1, items: { x: 3, y: 'g' }, plain: 9 }));
  const gback = ancient.fromText(g.toText());
  assert.ok(gback.encode().equals(g.encode()), 'group text round-trip is wire-equal');

  // Unknown fields print by default and drop on request.
  const withUnknown = person.decode(Buffer.concat([person.encode(VALUES), Buffer.from([0x98, 0x06, 0x07])]));
  assert.ok(withUnknown.toText().includes('99'), 'toText prints unknowns by default');
  assert.ok(!withUnknown.toText(false).includes('99'), 'toText(false) drops unknowns');
  withUnknown.free();
  g.free();
  gback.free();
});

test('field presence reads what was set, even zeros', () => {
  const message = person.newMessage();
  assert.strictEqual(message.has('name'), false, 'absent reads false');
  message.set('name', 'x');
  assert.strictEqual(message.has('name'), true, 'set reads true');
  message.set('id', 0);
  assert.strictEqual(message.has('id'), true, 'explicit zero is present');
  assert.throws(() => message.has('nope'), /has no field/, 'unknown field throws');
  const decoded = person.decode(person.encode(VALUES));
  assert.strictEqual(decoded.has('name'), true, 'decoded present field reads true');
  assert.strictEqual(decoded.has('active'), true, 'decoded bool reads true');
  message.free();
  decoded.free();
});

test('delimited frames concatenate and decode in a loop', () => {
  const first = person.decode(person.encode(VALUES));
  const second = person.decode(person.encode({ name: 'bob', id: 1 }));
  const frames = Buffer.concat([first.encodeDelimited(), second.encodeDelimited()]);
  let at = 0;
  const names = [];
  while (at < frames.length) {
    const { message, read } = person.decodeDelimited(frames.subarray(at));
    assert.ok(read > 0, 'frame reports its size');
    names.push(message.get('name'));
    at += read;
    message.free();
  }
  assert.deepStrictEqual(names, ['alice', 'bob']);
  assert.throws(() => person.decodeDelimited(frames.subarray(0, 2)), /./, 'truncated frame throws');
  first.free();
  second.free();
});

test('the addon rejects a slice array it did not produce', () => {
  const addon = require(path.join(__dirname, '..', 'build', 'pbwire_node.node'));
  const wire = one.encode(ONE_VALUES);
  const desc = one._handle;
  assert.throws(() => addon.wireGet(desc, wire, new Uint32Array([999, 0, 0]),
                                    'maybe'),
                /slice out of range/);
  assert.throws(() => addon.wireGet(desc, wire, new Uint32Array([0, 1, 0xffff]),
                                    'maybe'),
                /slice out of range/);
  assert.throws(() => addon.wireGet(desc, wire, Buffer.alloc(12), 'maybe'),
                /slice array/);
  assert.throws(() => addon.decode(desc, 'not a buffer'), /expects a Buffer/);
  assert.throws(() => addon.copyEncoded(desc, Buffer.alloc(8)), /without a/);
});

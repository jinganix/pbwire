'use strict';

/* Generated-conformance check for the Node binding: build matrix3.proto's
 * Scalars3 as the plain object the generated .d.ts/.js describe, encode it,
 * require the protoc reference bytes (matrix3.bin), then decode those bytes
 * back and check every field.
 *
 * The generated layer is the generated declarations: `matrix3.d.ts` for
 * TypeScript, `matrix3.js` for JSDoc/`@ts-check` plus the enum constants.
 * Nothing here is untyped by accident -- the object literal below is checked
 * by `tsc --checkJs` when the binding is type-checked.
 */

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');

const { Schema } = require(path.join(__dirname, '..', 'lib', 'pbwire.js'));
const { Structtest_Color3 } = require(path.join(__dirname, 'gen', 'matrix3.js'));

const DATA = path.join(__dirname, '..', '..', 'c', 'tests', 'data');
const REFERENCE = fs.readFileSync(path.join(DATA, 'matrix3.bin'));
const DESC = path.join(DATA, 'matrix3.desc');

/** @returns {import('./gen/matrix3.js').Scalars3} */
function build() {
  return {
    i32: -2147483648,
    i64: 9223372036854775807n,
    u32: 4294967295,
    u64: 18446744073709551615n,
    s32: -2147483648,
    s64: -9223372036854775808n,
    fx32: 3735928559,
    fx64: 72623859790382856n,
    sfx32: -2147483648,
    sfx64: -9223372036854775808n,
    f32: Infinity,
    f64: -123456.789,
    flag: true,
    str: 'hello',
    color: Structtest_Color3.COLOR3_GREEN,
    inner: { name: 'kid', value: 42 },
    nums: [1, -5, 300],
    tags: ['a', 'bb'],
    inners: [{ name: 'x', value: 1 }, { name: 'y', value: -2 }],
    opt_i32: -7,
    opt_str: 'opt',
    flags: [true, false, true],
  };
}

test('generated object encodes to the protoc reference bytes and back', () => {
  const schema = Schema.fromFile(DESC);
  const ty = schema.message('structtest.Scalars3');
  const innerTy = schema.message('structtest.Inner3');

  const wire = ty.encode(build());
  assert.ok(wire.equals(REFERENCE), 'generated encode equals the protoc reference bytes');

  const back = ty.decode(REFERENCE);
  assert.equal(back.get('i32'), -2147483648);
  assert.equal(back.get('i64'), 9223372036854775807n);
  assert.equal(back.get('u32'), 4294967295);
  assert.equal(back.get('u64'), 18446744073709551615n);
  assert.equal(back.get('s32'), -2147483648);
  assert.equal(back.get('s64'), -9223372036854775808n);
  assert.equal(back.get('fx32'), 3735928559);
  assert.equal(back.get('fx64'), 72623859790382856n);
  assert.equal(back.get('sfx32'), -2147483648);
  assert.equal(back.get('sfx64'), -9223372036854775808n);
  assert.equal(back.get('f32'), Infinity);
  assert.equal(back.get('f64'), -123456.789);
  assert.equal(back.get('flag'), true);
  assert.equal(back.get('str'), 'hello');
  assert.equal(back.get('color'), Structtest_Color3.COLOR3_GREEN);
  assert.deepEqual(back.get('inner'), { name: 'kid', value: 42 });
  assert.deepEqual(back.get('nums'), [1, -5, 300]);
  assert.deepEqual(back.get('tags'), ['a', 'bb']);
  assert.deepEqual(back.get('inners'), [{ name: 'x', value: 1 }, { name: 'y', value: -2 }]);
  assert.equal(back.get('opt_i32'), -7);
  assert.equal(back.get('opt_str'), 'opt');
  assert.deepEqual(back.get('flags'), [true, false, true]);

  assert.equal(ty.encode({}).length, 0, 'empty generated object is empty');

  const zero = ty.decode(ty.encode({ opt_i32: 0 }));
  assert.equal(zero.get('opt_i32'), 0, 'explicit zero presence round-trips');

  const innerWire = innerTy.encode({ name: 'kid' });
  assert.equal(innerTy.decode(innerWire).get('name'), 'kid', 'inner encodes standalone');

  schema.close?.();
});

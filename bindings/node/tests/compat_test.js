'use strict';

/* Typed-compat check for the Node binding (TESTSPEC T1–T6, vectors.json as
 * the single source of truth):
 *
 *   T1  generated encode == pinned protoc reference bytes
 *   T2  generated decode of the reference == vectors, field by field
 *   T3  empty encodes empty; explicit zero with presence round-trips
 *   T4  differential vs the pinned protobufjs oracle (same object, same
 *       bytes, same toObject after canonicalization, verify agrees)
 *   T5  one business snippet runs unchanged against protobufjs reflection
 *       types and against our generated classes (import swap only)
 *   T6  verify()/fromObject reject bad objects loudly, not with a crash
 *
 * Run: node --test bindings/node/tests/compat_test.js
 */

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');

const HERE = __dirname;
const VECTORS = JSON.parse(fs.readFileSync(
  path.join(HERE, '..', '..', 'typed', 'testdata', 'vectors.json'), 'utf8'));
const ORACLE_PIN = '8.8.1';

const protobufjs = require(path.join('..', 'benchmarks', 'node_modules', 'protobufjs'));
assert.equal(require(path.join('..', 'benchmarks', 'node_modules', 'protobufjs', 'package.json')).version,
  ORACLE_PIN, 'protobufjs oracle pin drifted; see testdata/ORACLES.md');

function vectorsBin(rel) {
  return fs.readFileSync(path.join(HERE, '..', '..', 'typed', 'testdata', rel));
}

function buildScalars(gen, v) {
  // vectors.json int64 values are decimal strings; f32 Infinity is a string.
  const num = (x) => (x === 'Infinity' ? Infinity : x === '-Infinity' ? -Infinity : x);
  return gen.create({
    i32: v.i32, i64: BigInt(v.i64), u32: v.u32, u64: BigInt(v.u64),
    s32: v.s32, s64: BigInt(v.s64), fx32: v.fx32, fx64: BigInt(v.fx64),
    sfx32: v.sfx32, sfx64: BigInt(v.sfx64), f32: num(v.f32), f64: v.f64,
    flag: v.flag, str: v.str, color: v.color,
    inner: v.inner, nums: v.nums.slice(), tags: v.tags.slice(),
    inners: v.inners.map((x) => ({ ...x })),
    opt_i32: v.opt_i32, opt_str: v.opt_str, flags: v.flags.slice(),
  });
}

// Structural canonicalization for cross-library comparison: int64-ish
// (BigInt, Long-like, numeric string) -> string, bytes -> base64.
function canon(x) {
  if (typeof x === 'bigint') {
    return 'i:' + x.toString();
  }
  if (typeof x === 'number' && !Number.isFinite(x)) {
    return 'f:' + String(x);
  }
  if (typeof x === 'string' && /^-?\d+$/.test(x)) {
    return 'i:' + BigInt(x).toString();
  }
  if (typeof x === 'object' && x !== null && typeof x.low === 'number' &&
      typeof x.high === 'number' && typeof x.toString === 'function' &&
      (x.unsigned !== undefined || x.toString() !== '[object Object]')) {
    try {
      return 'i:' + BigInt(x.toString()).toString();
    } catch {
      // fall through
    }
  }
  if (typeof x === 'object' && x !== null &&
      (Buffer.isBuffer(x) || x instanceof Uint8Array)) {
    return 'b:' + Buffer.from(x).toString('base64');
  }
  if (Array.isArray(x)) {
    return x.map(canon);
  }
  if (typeof x === 'object' && x !== null) {
    const out = {};
    for (const k of Object.keys(x).sort()) {
      out[k] = canon(x[k]);
    }
    return out;
  }
  return x;
}

test('T1+T2+T3 matrix3 through the generated facade', () => {
  const gen = require(path.join(HERE, 'gen', 'matrix3.js'));
  const ref = vectorsBin(VECTORS.reference.bin);

  const wire = gen.Scalars3.encode(buildScalars(gen.Scalars3, VECTORS.values)).finish();
  assert.ok(Buffer.from(wire).equals(ref), 'generated encode == protoc reference bytes');

  const back = gen.Scalars3.decode(ref);
  const v = VECTORS.values;
  assert.equal(back.i32, v.i32);
  assert.equal(String(back.i64), v.i64);
  assert.equal(back.u32, v.u32);
  assert.equal(String(back.u64), v.u64);
  assert.equal(back.s32, v.s32);
  assert.equal(String(back.s64), v.s64);
  assert.equal(back.fx32, v.fx32);
  assert.equal(String(back.fx64), v.fx64);
  assert.equal(back.sfx32, v.sfx32);
  assert.equal(String(back.sfx64), v.sfx64);
  assert.equal(back.f32, Infinity);
  assert.equal(back.f64, v.f64);
  assert.equal(back.flag, v.flag);
  assert.equal(back.str, v.str);
  assert.equal(back.color, gen.Structtest_Color3[v.color]);
  assert.deepEqual({ ...back.inner }, v.inner);
  assert.deepEqual(back.nums, v.nums);
  assert.deepEqual(back.tags, v.tags);
  assert.deepEqual(back.inners.map((x) => ({ ...x })), v.inners);
  assert.equal(back.optI32, v.opt_i32);
  assert.equal(back.optStr, v.opt_str);
  assert.deepEqual(back.flags, v.flags);

  assert.equal(gen.Scalars3.encode(gen.Scalars3.create({})).finish().length, 0,
    'empty message encodes empty');
  const zero = gen.Scalars3.decode(
    gen.Scalars3.encode(gen.Scalars3.create({ opt_i32: 0 })).finish());
  assert.equal(zero.optI32, 0, 'explicit zero presence round-trips');

  // The decode fast path must equal the validating fromObject() path.
  assert.deepEqual(
    canon(gen.Scalars3.toObject(back)),
    canon(gen.Scalars3.toObject(gen.Scalars3.fromObject(
      gen.Scalars3.toObject(back, { keepCase: true })))),
    'decode == fromObject(keepCase plain)');

  assert.equal(gen.Scalars3.verify(buildScalars(gen.Scalars3, VECTORS.values)), null);
  const json = gen.Scalars3.decode(ref).toJSON();
  assert.equal(json.color, 'COLOR3_GREEN', 'toJSON maps enums to names');
  assert.equal(json.i64, v.i64, 'toJSON maps int64 to strings');
});

test('T1+T2+T4 oneof1 extra through the generated facade', () => {
  const gen = require(path.join(HERE, 'gen', 'oneof1.js'));
  const extra = VECTORS.extra_messages[0];
  const ref = vectorsBin(extra.reference.bin);

  const wire = gen.Oneof1.encode(gen.Oneof1.create(extra.values)).finish();
  assert.ok(Buffer.from(wire).equals(ref), 'oneof encode == protoc reference bytes');

  const back = gen.Oneof1.decode(ref);
  assert.equal(back.name, 'a');
  assert.equal(back.text, 'hi');
  assert.equal(back.num, null);
  assert.equal(back.inner, null);

  const nested = gen.Oneof1.decode(gen.Oneof1.encode(
    gen.Oneof1.create({ name: 'n', inner: { v: 7 } })).finish());
  assert.deepEqual({ ...nested.inner }, { v: 7 });
});

test('T4 differential vs protobufjs', () => {
  const gen = require(path.join(HERE, 'gen', 'matrix3.js'));
  const root = protobufjs.parse(fs.readFileSync(
    path.join(HERE, '..', '..', 'typed', 'testdata', '..', '..', 'c', 'tests', 'data', 'matrix3.proto'),
    'utf8')).root;
  const PType = root.lookupType('structtest.Scalars3');
  const ref = vectorsBin(VECTORS.reference.bin);

  // Safe-integer plain for the verify comparison (the oracle's verify only
  // takes integer numbers / Long-likes for 64-bit, like ours).
  const v = VECTORS.values;
  const small = {
    i32: -7, i64: 123456789012345, u32: 99, u64: 123456789012345,
    s32: -7, s64: -123456789012345, fx32: 99, fx64: 123456789012345,
    sfx32: -7, sfx64: -123456789012345, f32: 1.5, f64: -123456.789,
    flag: true, str: 'hi', color: 2,
    inner: { name: 'kid', value: 42 }, nums: [1, -5, 300], tags: ['a', 'bb'],
    inners: [{ name: 'x', value: 1 }],
    optI32: -7, optStr: 'opt', flags: [true, false, true],
  };
  assert.equal(PType.verify(small), null, 'oracle accepts the small plain');
  assert.equal(gen.Scalars3.verify(small), null, 'facade accepts the small plain');

  const oracleSmall = Buffer.from(PType.encode(PType.fromObject(small)).finish());
  const ourSmall = Buffer.from(gen.Scalars3.encode(small).finish());
  assert.ok(oracleSmall.equals(ourSmall), 'same object, same bytes');

  // Full vectors (max int64s as strings, enum by name): both fromObjects
  // take them, and both encoders must land on the reference bytes.
  const big = {
    i32: v.i32, i64: v.i64, u32: v.u32, u64: v.u64,
    s32: v.s32, s64: v.s64, fx32: v.fx32, fx64: v.fx64,
    sfx32: v.sfx32, sfx64: v.sfx64, f32: Infinity, f64: v.f64,
    flag: v.flag, str: v.str, color: v.color,
    inner: v.inner, nums: v.nums, tags: v.tags, inners: v.inners,
    // camelCase: the oracle silently drops snake_case keys (ours takes both).
    optI32: v.opt_i32, optStr: v.opt_str, flags: v.flags,
  };
  const oracleWire = Buffer.from(PType.encode(PType.fromObject(big)).finish());
  const ourWire = Buffer.from(gen.Scalars3.encode(big).finish());
  assert.ok(oracleWire.equals(ref), 'oracle matches the reference');
  assert.ok(ourWire.equals(ref), 'facade matches the reference');

  const oracleObj = canon(PType.toObject(PType.decode(ref)));
  const ourObj = canon(gen.Scalars3.toObject(gen.Scalars3.decode(ref)));
  assert.deepEqual(ourObj, oracleObj, 'toObject agrees after canonicalization');

  // Enum names map on the way in for both.
  assert.equal(gen.Scalars3.verify({ color: 99 }), PType.verify({ color: 99 }));
  assert.equal(gen.Scalars3.verify({ i32: 2147483648 }), PType.verify({ i32: 2147483648 }),
    'range parity with the oracle (both truncate in the encoder)');
});

test('T5 one business snippet, both libraries', () => {
  const gen = require(path.join(HERE, 'gen', 'matrix3.js'));
  const root = protobufjs.parse(fs.readFileSync(
    path.join(HERE, '..', '..', 'c', 'tests', 'data', 'matrix3.proto'), 'utf8')).root;

  // Written once against the static API shape; T is either library's type.
  function workflow(T) {
    const msg = T.create({ str: 'kid', i32: -5, nums: [1, 2], inner: { name: 'x', value: 1 } });
    const bad = T.verify({ i32: 'nope' });
    const wire = T.encode(msg).finish();
    const back = T.decode(wire);
    const obj = canon(T.toObject(back));
    return JSON.stringify({ bad, len: wire.length, obj });
  }

  const P = root.lookupType('structtest.Scalars3');
  assert.equal(workflow(P), workflow(gen.Scalars3),
    'same snippet, same observable result (import swap only)');
});

test('T6 verify/fromObject reject loudly', () => {
  const gen = require(path.join(HERE, 'gen', 'matrix3.js'));
  assert.equal(gen.Scalars3.verify({ i32: 'x' }), 'i32: integer expected');
  assert.equal(gen.Scalars3.verify(null), 'object expected');
  assert.match(gen.Scalars3.verify({ u32: 1.5 }), /u32/);
  assert.match(gen.Scalars3.verify({ flag: 1 }), /flag/);
  assert.match(gen.Scalars3.verify({ str: 42 }), /str/);
  assert.match(gen.Scalars3.verify({ nums: 'no' }), /nums/);
  assert.match(gen.Scalars3.verify({ inner: 42 }), /inner/);
  assert.throws(() => gen.Scalars3.fromObject({ str: 42 }), TypeError);
  assert.throws(() => gen.Scalars3.encode({ str: 42 }), TypeError);
  assert.throws(() => gen.Scalars3.decode(Buffer.from([0xff, 0xff, 0xff])), Error);

  const oneof = require(path.join(HERE, 'gen', 'oneof1.js'));
  assert.match(oneof.Oneof1.verify({ num: 1, text: 'x' }), /kind/);
});

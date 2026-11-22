'use strict';

/* Stage 1 gate for the pure-TS core: the descriptor reader over the same
 * FileDescriptorSet subset the C core reads (src/descriptor_set.c).
 *
 * Only facts the codec depends on: field count and order, the four presence
 * kinds, repetition and packedness, kind mapping, oneofs, groups and the
 * transitive required closure. Every assertion comes from a .desc fixture the
 * C tests already pin, so the two readers cannot drift.
 */

const test = require('node:test');
const assert = require('node:assert');
const path = require('node:path');

const { Schema } = require('../../lib/core/schema.js');
const {
    EXPLICIT,
    IMPLICIT,
    K,
    MESSAGE_PRESENCE,
    REQUIRED,
    fieldByName,
    fieldCount,
} = require('../../lib/core/descriptor.js');

const data = path.join(__dirname, '..', '..', '..', 'c', 'tests', 'data');

function typeOf(file, name) {
    return Schema.fromFile(path.join(data, file)).message(name);
}

test('matrix3.desc: 22 fields, proto3 presence and packed repetition', () => {
    const type = typeOf('matrix3.desc', 'structtest.Scalars3');
    const def = type.def;
    assert.equal(fieldCount(def), 22);
    assert.equal(def.fullName, 'structtest.Scalars3');

    const numbers = def.fields.map((f) => f.number);
    assert.deepEqual(numbers, Array.from({ length: 22 }, (_, i) => i + 1));
    const names = def.fields.map((f) => f.name);
    assert.equal(names[0], 'i32');
    assert.equal(names[21], 'flags');

    // proto3 plain scalars have no has-bit...
    assert.equal(fieldByName(def, 'i32').presence, IMPLICIT);
    assert.equal(fieldByName(def, 'str').presence, IMPLICIT);
    // ...while `optional` in a proto3 file is explicit presence.
    assert.equal(fieldByName(def, 'opt_i32').presence, EXPLICIT);
    assert.equal(fieldByName(def, 'opt_str').presence, EXPLICIT);
    // ...and a message field always has it.
    assert.equal(fieldByName(def, 'inner').presence, MESSAGE_PRESENCE);

    const flags = fieldByName(def, 'flags');
    assert.equal(flags.repeated, true);
    assert.equal(flags.kind, K.BOOL);
    assert.equal(flags.packed, true, 'proto3 repeated numerics default to packed');
    assert.equal(flags.wt, 2, 'a packed field encodes under the LEN tag');

    const nums = fieldByName(def, 'nums');
    assert.equal(nums.repeated, true);
    assert.equal(nums.kind, K.INT32);
    assert.equal(nums.packed, true);
    assert.equal(nums.elemWt, 0, 'the unpacked element form is still accepted');

    assert.equal(fieldByName(def, 'i64').kind, K.INT64);
    assert.equal(fieldByName(def, 'u64').kind, K.UINT64);
    assert.equal(fieldByName(def, 's64').kind, K.SINT64);
    assert.equal(fieldByName(def, 'fx64').kind, K.FIXED64);
    assert.equal(fieldByName(def, 'sfx64').kind, K.SFIXED64);
    assert.equal(fieldByName(def, 'f32').kind, K.FLOAT);
    assert.equal(fieldByName(def, 'f64').kind, K.DOUBLE);
    assert.equal(fieldByName(def, 'color').kind, K.ENUM);
    assert.equal(fieldByName(def, 'inner').kind, K.MESSAGE);
    assert.equal(fieldByName(def, 'inner').msgType.fullName, 'structtest.Inner3');
    // proto3 strings verify UTF-8 on parse.
    assert.equal(fieldByName(def, 'str').utf8, true);
    // The enum is open (proto3), and its values are indexed by name.
    const color = fieldByName(def, 'color').enumType;
    assert.equal(color.closed, false);
    assert.equal(color.byName.get('COLOR3_GREEN'), 2);
    assert.equal(color.byNumber.get(2), 'COLOR3_GREEN');
});

test('matrix2.desc: proto2 required fields and unpacked repetition', () => {
    const type = typeOf('matrix2.desc', 'structtest.Scalars2');
    const def = type.def;
    assert.equal(fieldByName(def, 'id').presence, REQUIRED);
    assert.equal(def.hasRequired, true);
    // Innner2.name is required, so Scalars2 is required transitively too.
    assert.equal(typeOf('matrix2.desc', 'structtest.Inner2').def.hasRequired, true);
    assert.equal(def.deepRequired, true);

    // proto2 packs only with [packed = true].
    const nums = fieldByName(def, 'nums');
    assert.equal(nums.repeated, true);
    assert.equal(nums.packed, false);
    const packedNums = fieldByName(def, 'packed_nums');
    assert.equal(packedNums.packed, true);
    // proto2 strings do not verify UTF-8.
    assert.equal(fieldByName(def, 'str').utf8, false);
});

test('grouptest.desc: a group field maps to kind GROUP with an end tag', () => {
    const type = typeOf('grouptest.desc', 'structtest.GroupTest');
    const inner = fieldByName(type.def, 'inner');
    assert.equal(inner.kind, K.GROUP);
    assert.equal(inner.wt, 3, 'start-group tag');
    assert.equal(inner.msgType.fullName, 'structtest.GroupTest.Inner');
    assert.deepEqual(Array.from(inner.tag), [0x0b]); // field 1, start group
    assert.deepEqual(Array.from(inner.tagEnd), [0x0c]); // field 1, end group
    const item = fieldByName(type.def, 'item');
    assert.equal(item.kind, K.GROUP);
    assert.equal(item.repeated, true);
});

test('oneoftest.desc: oneof members share one case slot', () => {
    const type = typeOf('oneoftest.desc', 'structtest.OneOfTest');
    const def = type.def;
    assert.equal(def.oneofCount, 1);
    assert.equal(def.oneofByName.get('kind'), 0);
    for (const name of ['num', 'text', 'sub', 'blob']) {
        const f = fieldByName(def, name);
        assert.equal(f.oneof, 0, name + ' is a member of the oneof');
        assert.equal(f.presence, EXPLICIT, name + ' has explicit presence');
    }
    assert.equal(fieldByName(def, 'id').oneof, -1);
});

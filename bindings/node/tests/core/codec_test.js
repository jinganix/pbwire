'use strict';

/* Codec behaviour: byte equality against the fixtures the C tests pin,
 * decode/encode round trips, unknown retention (unknown numbers and
 * wrong-wire-type occurrences, input order), groups, packed runs, oneof
 * last-wins, presence, proto3 UTF-8 validation, proto2 required fields and
 * the byte/depth limits. */

const test = require('node:test');
const assert = require('node:assert');
const fs = require('node:fs');
const path = require('node:path');

const { Schema, fromObject, getField, oneofCase, rawPayload, toObject } = require('../../lib/core/schema.js');
const { CoreError } = require('../../lib/core/wire.js');
const { Msg } = require('../../lib/core/tree.js');

const data = path.join(__dirname, '..', '..', '..', 'c', 'tests', 'data');

function typeOf(file, name) {
    return Schema.fromFile(path.join(data, file)).message(name);
}

function fixture(file) {
    return new Uint8Array(fs.readFileSync(path.join(data, file)));
}

function u8(...bytes) {
    return Uint8Array.from(bytes);
}

/** Concatenates byte arrays into one input buffer. */
function cat(...parts) {
    const n = parts.reduce((acc, p) => acc + p.length, 0);
    const out = new Uint8Array(n);
    let off = 0;
    for (const p of parts) {
        out.set(p, off);
        off += p.length;
    }
    return out;
}

/* matrix3.txt: every scalar at an edge, both repeated forms, nested message. */
const matrix3 = {
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
    color: 2,
    inner: { name: 'kid', value: 42 },
    nums: [1, -5, 300],
    tags: ['a', 'bb'],
    inners: [
        { name: 'x', value: 1 },
        { name: 'y', value: -2 },
    ],
    opt_i32: -7,
    opt_str: 'opt',
    flags: [true, false, true],
};

test('matrix3: object -> wire is byte-identical to protoc', () => {
    const type = typeOf('matrix3.desc', 'structtest.Scalars3');
    const wire = type.encodeObject(matrix3);
    assert.deepEqual(Buffer.from(wire), Buffer.from(fixture('matrix3.bin')));
});

test('matrix3: decode exposes every value and re-encodes byte-exactly', () => {
    const type = typeOf('matrix3.desc', 'structtest.Scalars3');
    const bin = fixture('matrix3.bin');
    const msg = type.decode(bin);
    assert.equal(getField(msg, 'i32'), -2147483648);
    assert.equal(getField(msg, 'u32'), 4294967295);
    assert.equal(getField(msg, 'i64'), 9223372036854775807n);
    assert.equal(getField(msg, 'u64'), 18446744073709551615n);
    assert.equal(getField(msg, 's64'), -9223372036854775808n);
    assert.equal(getField(msg, 'fx64'), 72623859790382856n);
    assert.equal(getField(msg, 'sfx64'), -9223372036854775808n);
    assert.equal(getField(msg, 'f32'), Infinity);
    assert.equal(getField(msg, 'f64'), -123456.789);
    assert.equal(getField(msg, 'flag'), true);
    assert.equal(getField(msg, 'str'), 'hello');
    assert.equal(getField(msg, 'color'), 2);
    assert.deepEqual(getField(msg, 'nums'), [1, -5, 300]);
    assert.deepEqual(getField(msg, 'tags'), ['a', 'bb']);
    assert.deepEqual(getField(msg, 'flags'), [true, false, true]);
    assert.equal(getField(msg, 'opt_i32'), -7);
    assert.equal(getField(msg, 'opt_str'), 'opt');
    assert.equal(getField(getField(msg, 'inner'), 'value'), 42);
    assert.deepEqual(toObject(msg), matrix3);
    assert.deepEqual(Buffer.from(type.encode(msg)), Buffer.from(bin));
});

test('matrix2: proto2 required fields, defaults and unpacked runs', () => {
    const type = typeOf('matrix2.desc', 'structtest.Scalars2');
    const bin = fixture('matrix2.bin');
    const msg = type.decode(bin);
    assert.equal(getField(msg, 'id'), 99);
    assert.equal(getField(msg, 's64'), -1n);
    assert.equal(getField(msg, 'shade'), -2);
    assert.deepEqual(getField(msg, 'nums'), [4, 5]);
    assert.deepEqual(getField(msg, 'packed_nums'), [6, 7, 8]);
    assert.equal(getField(getField(msg, 'inner'), 'name'), 'req');
    assert.deepEqual(Buffer.from(type.encode(msg)), Buffer.from(bin));

    // A missing required field fails the decode with PBW_ERR_REQUIRED (6):
    // the first field of the fixture is `id`, so drop it.
    assert.throws(
        () => type.decode(bin.subarray(2)),
        (e) => e instanceof CoreError && e.status === 6 && /missing required field/.test(e.message),
    );
});

test('grouptest: groups decode and re-encode byte-exactly', () => {
    const type = typeOf('grouptest.desc', 'structtest.GroupTest');
    const bin = fixture('grouptest.bin');
    const msg = type.decode(bin);
    const inner = getField(msg, 'inner');
    assert.ok(inner instanceof Msg);
    assert.equal(inner.def.fullName, 'structtest.GroupTest.Inner');
    assert.equal(getField(inner, 'x'), 5);
    assert.equal(getField(inner, 's'), 'g');
    assert.deepEqual(
        getField(msg, 'item').map((g) => getField(g, 'v')),
        [1, -2],
    );
    assert.equal(getField(msg, 'id'), 9);
    assert.deepEqual(Buffer.from(type.encode(msg)), Buffer.from(bin));

    // A group that never closes is a parse failure, not a silent skip.
    // Dropping the last byte leaves a tag with no value behind it.
    assert.throws(() => type.decode(bin.subarray(0, bin.length - 1)), CoreError);
});

test('oneoftest: last member wins and the case follows', () => {
    const type = typeOf('oneoftest.desc', 'structtest.OneOfTest');
    const bin = fixture('oneoftest.bin');
    const msg = type.decode(bin);
    assert.equal(oneofCase(msg, 'kind'), 2);
    assert.equal(getField(msg, 'text'), 'hi');
    assert.equal(getField(msg, 'num'), undefined);
    assert.deepEqual(Buffer.from(type.encode(msg)), Buffer.from(bin));

    // num then text: the text member replaces the number, not merges.
    const both = u8(0x08, 0x03, 0x12, 0x02, 0x68, 0x69, 0x28, 0x01);
    const last = type.decode(both);
    assert.equal(oneofCase(last, 'kind'), 2);
    assert.equal(getField(last, 'num'), undefined);
    // Only the set member is emitted, so the cleared number is gone.
    assert.deepEqual(Buffer.from(type.encode(last)), Buffer.from(u8(0x12, 0x02, 0x68, 0x69, 0x28, 0x01)));
});

test('unknown fields are retained verbatim, in input order', () => {
    const type = typeOf('matrix3.desc', 'structtest.Scalars3');
    const bin = fixture('matrix3.bin');
    // Field 99 varint 5, then field 100 length-delimited "zz".
    const unknownA = u8(0x98, 0x06, 0x05);
    const unknownB = u8(0xa2, 0x06, 0x02, 0x7a, 0x7a);
    const wire = cat(unknownA, bin, unknownB);
    const msg = type.decode(wire);
    // Known fields keep their values; both unknowns survive, in input order.
    assert.equal(getField(msg, 'str'), 'hello');
    const out = Buffer.from(type.encode(msg));
    const expected = Buffer.concat([
        Buffer.from(bin),
        Buffer.from(unknownA),
        Buffer.from(unknownB),
    ]);
    assert.deepEqual(out, expected);

    // Unknowns only, nothing dropped: a message of pure unknowns re-encodes
    // to itself.
    const only = type.decode(cat(unknownA, unknownB));
    assert.deepEqual(Buffer.from(type.encode(only)), Buffer.from(cat(unknownA, unknownB)));
});

test('a known field with the wrong wire type is retained as unknown', () => {
    const type = typeOf('matrix3.desc', 'structtest.Scalars3');
    // i32 (field 1) declared varint, sent as length-delimited.
    const wrong = u8(0x0a, 0x01, 0x00);
    const msg = type.decode(wrong);
    assert.equal(getField(msg, 'i32'), undefined);
    assert.deepEqual(Buffer.from(type.encode(msg)), Buffer.from(wrong));

    // A group wire type on a scalar field is also unknown, and its body is
    // validated (field 1 start group, end group) before retention.
    const group = u8(0x0b, 0x0c);
    const kept = type.decode(group);
    assert.deepEqual(Buffer.from(type.encode(kept)), Buffer.from(group));
});

test('packed and unpacked forms are both accepted', () => {
    const type = typeOf('matrix3.desc', 'structtest.Scalars3');
    // nums = 17: unpacked elements (tag 0x88 0x01) for 1 and -5.
    const unpacked = cat(
        u8(0x88, 0x01, 0x01),
        u8(0x88, 0x01, 0xfb, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01),
    );
    const msg = type.decode(unpacked);
    assert.deepEqual(getField(msg, 'nums'), [1, -5]);
    // The encoder writes the declared packed form, which is what the
    // reference emits for the same tree.
    const packed = u8(
        0x8a, 0x01, 0x0b, 0x01, // 1
        0xfb, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01, // -5, sign-extended
    );
    assert.deepEqual(Buffer.from(type.encode(msg)), Buffer.from(packed));

    // A packed run with the wrong length fails instead of reading past it.
    assert.throws(() => type.decode(u8(0x8a, 0x01, 0x02, 0x01)), CoreError);
});

test('an empty packed run leaves the field absent', () => {
    const type = typeOf('matrix3.desc', 'structtest.Scalars3');
    const msg = type.decode(u8(0x8a, 0x01, 0x00));
    assert.equal(getField(msg, 'nums'), undefined);
    assert.equal(type.encode(msg).length, 0);
});

test('proto3: a wide varint that truncates to zero is an implicit zero', () => {
    const type = typeOf('matrix3.desc', 'structtest.Scalars3');
    // i32 field 1 with the varint 2^32: the reference truncates to int32 0
    // and drops the field as an implicit zero, so the output is empty.
    const msg = type.decode(u8(0x08, 0x80, 0x80, 0x80, 0x80, 0x10));
    assert.equal(getField(msg, 'i32'), undefined);
    assert.equal(type.encode(msg).length, 0);
});

test('proto3 strings must be valid UTF-8; proto2 strings are passed through', () => {
    const proto3 = typeOf('matrix3.desc', 'structtest.Scalars3');
    // str = 14, one 0xFF byte.
    assert.throws(() => proto3.decode(u8(0x72, 0x01, 0xff)), /invalid UTF-8/);

    // matrix2 str is proto2: the bytes are kept and re-emitted unchanged.
    const proto2 = typeOf('matrix2.desc', 'structtest.Scalars2');
    const id = u8(0x08, 0x01);
    const str = u8(0x3a, 0x02, 0xff, 0xfe);
    const msg = proto2.decode(cat(id, str));
    assert.deepEqual(Buffer.from(type2encode(proto2, msg)), Buffer.from(cat(id, str)));
});

function type2encode(type, msg) {
    return type.encode(msg);
}

function deepType() {
    const field = [
        ...ld(1, [...Buffer.from('kid')]),
        ...vint(3, 1),
        ...vint(4, 1),
        ...vint(5, 11),
        ...ld(6, [...Buffer.from('.deep.Node')]),
    ];
    const message = [...ld(1, [...Buffer.from('Node')]), ...ld(2, field)];
    const file = [
        ...ld(1, [...Buffer.from('deep.proto')]),
        ...ld(2, [...Buffer.from('deep')]),
        ...ld(4, message),
        ...ld(12, [...Buffer.from('proto3')]),
    ];
    return Schema.fromBytes(Uint8Array.from(ld(1, file))).message('deep.Node');
}

test('limits: byte cap and nesting cap', () => {
    const type = typeOf('matrix3.desc', 'structtest.Scalars3');
    const bin = fixture('matrix3.bin');
    assert.throws(
        () => type.decode(bin, { maxBytes: 10, maxDepth: 0 }),
        (e) => e instanceof CoreError && e.status === 5,
    );
    assert.deepEqual(Buffer.from(type.encode(type.decode(bin, { maxBytes: bin.length, maxDepth: 0 }))), Buffer.from(bin));

    // A self-nesting message from a synthetic descriptor: eight levels of
    // `kid` must be rejected by a cap of four and accepted by the default.
    let body = u8();
    for (let i = 0; i < 8; i++) {
        body = cat(u8(0x0a, body.length), body);
    }
    const deep = deepType();
    assert.throws(
        () => deep.decode(body, { maxBytes: 0, maxDepth: 4 }),
        (e) => e instanceof CoreError && /nesting exceeds/.test(e.message),
    );
    deep.decode(body);

    // The same cap applies to an unknown group chain, which the decoder
    // walks through skipGroup.
    let groups = u8(0x0c);
    for (let i = 0; i < 8; i++) {
        groups = cat(u8(0x0b), groups, u8(0x0c));
    }
    assert.throws(
        () => type.decode(groups, { maxBytes: 0, maxDepth: 2 }),
        (e) => e instanceof CoreError && /nesting exceeds/.test(e.message),
    );
});

test('maptest and extensiontest fixtures round trip', () => {
    const maps = typeOf('maptest.desc', 'structtest.Maps');
    const mapBin = fixture('maptest.bin');
    assert.deepEqual(Buffer.from(maps.encode(maps.decode(mapBin))), Buffer.from(mapBin));

    const ext = typeOf('extensiontest.desc', 'exttest.HasExt');
    const extBin = fixture('extensiontest.bin');
    const msg = ext.decode(extBin);
    assert.deepEqual(Buffer.from(ext.encode(msg)), Buffer.from(extBin));
});

test('rawPayload returns the bytes of a length-delimited field', () => {
    const type = typeOf('matrix3.desc', 'structtest.Scalars3');
    const msg = type.decode(fixture('matrix3.bin'));
    // The decoded bytes field (tags[0]) is still a slice until read; read
    // the raw image through the field's type instead.
    const slot = msg.def.byName.get('str');
    const raw = msg.v[slot];
    assert.deepEqual(Buffer.from(rawPayload(raw)), Buffer.from('hello'));
    assert.equal(getField(msg, 'str'), 'hello');
});

/* A hand-built FileDescriptorSet for a self-nesting proto3 message:
 *   syntax = "proto3"; package deep; message Node { Node kid = 1; }
 * The depth test needs a recursive type and no toolchain, so the descriptor
 * is assembled here instead of shelling out to protoc. */
function varint(v) {
    const out = [];
    while (v > 0x7f) {
        out.push((v & 0x7f) | 0x80);
        v >>>= 7;
    }
    out.push(v);
    return out;
}

function ld(fnum, payload) {
    return [fnum * 8 + 2, ...varint(payload.length), ...payload];
}

function vint(fnum, v) {
    return [fnum * 8, ...varint(v)];
}

test('a synthetic descriptor set loads (self-nesting message)', () => {
    const field = [
        ...ld(1, [...Buffer.from('kid')]),
        ...vint(3, 1), // number
        ...vint(4, 1), // LABEL_OPTIONAL
        ...vint(5, 11), // TYPE_MESSAGE
        ...ld(6, [...Buffer.from('.deep.Node')]),
    ];
    const message = [...ld(1, [...Buffer.from('Node')]), ...ld(2, field)];
    const file = [
        ...ld(1, [...Buffer.from('deep.proto')]),
        ...ld(2, [...Buffer.from('deep')]),
        ...ld(4, message),
        ...ld(12, [...Buffer.from('proto3')]),
    ];
    const schema = Schema.fromBytes(Uint8Array.from(ld(1, file)));
    const node = schema.message('deep.Node');
    assert.equal(node.def.fields.length, 1);
    assert.equal(node.def.fields[0].msgType, node.def, 'the field points back at Node');
});

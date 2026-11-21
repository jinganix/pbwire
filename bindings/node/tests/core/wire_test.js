'use strict';

/* Wire primitives: varints, tags, little-endian, zigzag, skipping, the
 * structural UTF-8 check and the error shapes the decoder relies on. */

const test = require('node:test');
const assert = require('node:assert');

const wire = require('../../lib/core/wire.js');

const { CoreError, MAX_DEPTH, R, W } = wire;

function writer() {
    return new W();
}

function bytes(w) {
    return Array.from(w.b.subarray(0, w.pos));
}

test('varint32 and varint64 round trip at the edges', () => {
    const cases32 = [0, 1, 127, 128, 300, 0x7fffffff, 0xffffffff];
    for (const v of cases32) {
        const w = writer();
        wire.wVar32(w, v);
        const r = new R(w.b, 0, w.pos);
        assert.equal(wire.rVar(r), v >>> 0);
        assert.equal(r.pos, w.pos);
        assert.equal(w.pos, wire.sizeVar32(v >>> 0));
    }
    const cases64 = [0n, 1n, 127n, 128n, 0xffffffffn, 0x7fffffffffffffffn, 0xffffffffffffffffn];
    for (const v of cases64) {
        const w = writer();
        wire.wVar64(w, v);
        const r = new R(w.b, 0, w.pos);
        assert.equal(wire.rVar64(r), v);
        assert.equal(w.pos, wire.sizeVar64(v));
    }
});

test('int32 negatives sign-extend to ten bytes', () => {
    const w = writer();
    wire.wVar64(w, BigInt(-1));
    assert.deepEqual(bytes(w), [0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01]);
    assert.equal(wire.sizeInt32(-1), 10);
    assert.equal(wire.sizeInt32(1), 1);
});

test('varint rejects truncation and overflow', () => {
    assert.throws(() => wire.rVar(new R(new Uint8Array([]))), CoreError);
    assert.throws(() => wire.rVar(new R(new Uint8Array([0x80]))), /truncated/);
    // Eleven continuation bytes: the tenth must terminate.
    const eleven = new Uint8Array(11).fill(0x80);
    assert.throws(() => wire.rVar(new R(eleven)), /overflow/);
    assert.throws(() => wire.rVar64(new R(eleven)), /overflow/);
});

test('fixed32 and fixed64 are little-endian', () => {
    const w = writer();
    wire.wFixed32(w, 0xdeadbeef);
    wire.wFixed64(w, 0x0102030405060708n);
    assert.deepEqual(bytes(w), [0xef, 0xbe, 0xad, 0xde, 8, 7, 6, 5, 4, 3, 2, 1]);
    const r = new R(w.b, 0, w.pos);
    assert.equal(wire.rFixed32(r), 0xdeadbeef);
    assert.equal(wire.rFixed64(r), 0x0102030405060708n);
});

test('zigzag matches the reference for the extremes', () => {
    assert.equal(wire.zig32(0), 0);
    assert.equal(wire.zig32(-1), 1);
    assert.equal(wire.zig32(1), 2);
    assert.equal(wire.zig32(-2147483648), 4294967295);
    assert.equal(wire.unzig32(4294967295), -2147483648);
    assert.equal(wire.zig64(-1n), 1n);
    assert.equal(wire.zig64(-9223372036854775808n), 0xffffffffffffffffn);
    assert.equal(wire.unzig64(0xffffffffffffffffn), -9223372036854775808n);
});

test('tags: five bytes maximum, a wide 5-byte tag truncates', () => {
    const w = writer();
    wire.wTag(w, wire.tagBytes(1, 0));
    assert.deepEqual(bytes(w), [0x08]);
    assert.deepEqual(Array.from(wire.tagBytes(536870911, 2)), [0xfa, 0xff, 0xff, 0xff, 0x0f]);

    // 0xffffffff as a 5-byte varint is a tag above UINT32_MAX: the
    // reference truncates it instead of rejecting it (the two expected
    // conformance failures pin this).
    const r = new R(new Uint8Array([0xff, 0xff, 0xff, 0xff, 0x0f]));
    assert.equal(wire.rTag(r) >>> 0, 0xffffffff);
    // A sixth tag byte is invalid.
    assert.throws(() => wire.rTag(new R(new Uint8Array([0x80, 0x80, 0x80, 0x80, 0x80, 0x00]))), /bad tag/);
});

test('lengths: strictly five bytes, the fifth only four bits', () => {
    const r = new R(new Uint8Array(200).fill(1));
    r.b[0] = 0x7f;
    assert.equal(wire.rLen32(r), 0x7f);
    assert.throws(() => wire.rLen32(new R(new Uint8Array([0xff, 0xff, 0xff, 0xff, 0x1f]))), /overflow/);
    assert.throws(() => wire.rLen32(new R(new Uint8Array([0x02, 0x00]))), /truncated/);
});

test('skip walks every non-group wire type and rejects the others', () => {
    const w = writer();
    wire.wTag(w, wire.tagBytes(1, 0));
    wire.wVar32(w, 300);
    wire.wTag(w, wire.tagBytes(2, 1));
    wire.wFixed64(w, 5n);
    wire.wTag(w, wire.tagBytes(3, 5));
    wire.wFixed32(w, 7);
    wire.wBytes(w, wire.tagBytes(4, 2), new Uint8Array([1, 2, 3]));
    const r = new R(w.b, 0, w.pos);
    for (let i = 0; i < 4; i++) {
        const tag = wire.rTag(r);
        wire.skip(r, tag & 7);
    }
    assert.equal(r.pos, w.pos);
    assert.throws(() => wire.skip(new R(new Uint8Array([0])), 3), /bad wire type/);
});

test('skipGroup validates matching end tags and nesting', () => {
    const w = writer();
    wire.wTag(w, wire.tagBytes(5, 3)); // start group 5
    wire.wTag(w, wire.tagBytes(1, 0));
    wire.wVar32(w, 1);
    wire.wTag(w, wire.tagBytes(5, 4)); // end group 5
    // skipGroup starts inside the body: the caller read the start tag.
    const r = new R(w.b, 0, w.pos);
    const start = wire.rTag(r);
    wire.skipUnknown(r, 5, start & 7, 0);
    assert.equal(r.pos, w.pos);

    const zero = new R(new Uint8Array([0x04])); // field number 0 is invalid
    assert.throws(() => wire.skipGroup(zero, 5, 0), /bad tag/);
    const mismatch = new R(new Uint8Array([0x34])); // end group 6
    assert.throws(() => wire.skipGroup(mismatch, 5, 0), /mismatched end-group/);

    // Depth is enforced on the group frame chain.
    const deep = new R(new Uint8Array([0x0b]));
    deep.cap = 0;
    assert.throws(() => wire.skipGroup(deep, 1, 1), /nesting exceeds/);
});

test('utf8Valid rejects overlongs, surrogates and out-of-range', () => {
    const ok = (s) => wire.utf8Valid(Buffer.from(s, 'utf8'), 0, Buffer.byteLength(s));
    assert.equal(ok('hello'), true);
    assert.equal(ok('héllo'), true);
    assert.equal(ok('日本語'), true);
    assert.equal(ok('😀'), true);
    const bad = (b) => wire.utf8Valid(Uint8Array.from(b), 0, b.length);
    assert.equal(bad([0xc0, 0x80]), false); // overlong NUL
    assert.equal(bad([0xe0, 0x80, 0x80]), false); // overlong
    assert.equal(bad([0xed, 0xa0, 0x80]), false); // surrogate
    assert.equal(bad([0xf4, 0x90, 0x80, 0x80]), false); // > U+10FFFF
    assert.equal(bad([0xff]), false);
    assert.equal(bad([0xe2, 0x82]), false); // truncated
});

test('utf8 encode/decode round trips and replaces malformed bytes', () => {
    const w = writer();
    const s = 'aé日😀';
    wire.ensure(w, wire.utf8Length(s));
    wire.utf8Write(w, s);
    assert.equal(w.pos, wire.utf8Length(s));
    assert.equal(wire.utf8Decode(w.b, 0, w.pos), s);
    // A lone surrogate becomes U+FFFD, and a malformed byte decodes to one.
    const lone = writer();
    wire.ensure(lone, wire.utf8Length('\ud800'));
    wire.utf8Write(lone, '\ud800');
    assert.equal(wire.utf8Decode(lone.b, 0, lone.pos), '\ufffd');
    assert.equal(wire.utf8Decode(Uint8Array.from([0x61, 0xff, 0x62]), 0, 3), 'a\ufffdb');
});

test('float and double bits keep NaN payloads', () => {
    const nan32 = 0x7fc00001;
    const back = wire.floatBits(wire.bitsFloat(nan32));
    assert.equal(back, nan32);
    assert.ok(Number.isNaN(wire.bitsFloat(nan32)));
    assert.equal(wire.floatBits(-0), 0x80000000);
    const nan64 = 0x7ff8000000000001n;
    assert.equal(wire.doubleBits(wire.bitsDouble(nan64)), nan64);
    assert.equal(wire.doubleBits(wire.bitsDouble(0x8000000000000000n)), 0x8000000000000000n);
});

test('the length placeholder patches one byte and shifts larger ones', () => {
    // patchLen fills the placeholder a nested payload was written over.
    const small = writer();
    wire.wTag(small, wire.tagBytes(1, 2));
    const lp = small.pos;
    small.b[small.pos++] = 0;
    wire.ensure(small, 3);
    small.b.set([0, 0, 0], small.pos);
    small.pos += 3;
    wire.patchLen(small, lp);
    assert.deepEqual(bytes(small), [0x0a, 0x03, 0, 0, 0]);

    const big = writer();
    wire.wTag(big, wire.tagBytes(1, 2));
    const lp2 = big.pos;
    big.b[big.pos++] = 0;
    wire.ensure(big, 300);
    big.b.fill(7, big.pos, big.pos + 300);
    big.pos += 300;
    wire.patchLen(big, lp2);
    const out = big.b.subarray(0, big.pos);
    assert.deepEqual(Array.from(out.subarray(0, 3)), [0x0a, 0xac, 0x02]); // 300 = 0xac 0x02
    assert.equal(out.length, 3 + 300);
    assert.equal(MAX_DEPTH, 256);
});

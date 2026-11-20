'use strict';

/* The pbench gate: no arm is timed before it reproduces the official
 * runtime's bytes on all four tiers and reads its own decode back field by
 * field. Runs first and stays runnable:
 *
 *   node bindings/node/pbench/gate.js
 *
 * Writes 'gate: PASS' and exits 0, or names the failing arm/tier and exits 1.
 */

const { TIERS, nativeObject, typeOf } = require('./payload');
const { Scalars3, Inner3, Color3 } = require('../benchmarks/gen/matrix3_pb.js');

let failures = 0;

function fail(what) {
    console.log(`FAIL ${what}`);
    failures++;
}

/* The official (jspb) object for `repeats`: the bytes every arm must match. */
function official(repeats) {
    const m = new Scalars3();
    m.setI32(-2147483648);
    m.setI64('9223372036854775807');
    m.setU32(4294967295);
    m.setU64('18446744073709551615');
    m.setS32(-2147483648);
    m.setS64('-9223372036854775808');
    m.setFx32(3735928559);
    m.setFx64('72623859790382856');
    m.setSfx32(-2147483648);
    m.setSfx64('-9223372036854775808');
    m.setF32(Infinity);
    m.setF64(-123456.789);
    m.setFlag(true);
    m.setStr('hello');
    m.setColor(Color3.COLOR3_GREEN);
    const inner = new Inner3();
    inner.setName('kid');
    inner.setValue(42);
    m.setInner(inner);
    m.setOptI32(-7);
    m.setOptStr('opt');
    const nums = [];
    const tags = [];
    const inners = [];
    const flags = [];
    for (let i = 0; i < repeats; i++) {
        nums.push(i - 3);
        tags.push('tag');
        const kid = new Inner3();
        kid.setName('grandkid');
        kid.setValue(i + 1);
        inners.push(kid);
        flags.push(i % 2 === 0);
    }
    m.setNumsList(nums);
    m.setTagsList(tags);
    m.setInnersList(inners);
    m.setFlagsList(flags);
    return m;
}

/* The fields benchmarks/bench.js already checks, on every arm's decode. */
function decodeOk(back, repeats) {
    const inner = back.inner;
    return back.str === 'hello'
        && inner !== undefined
        && inner.name === 'kid'
        && inner.value === 42
        && (back.nums ?? []).length === repeats
        && (back.inners ?? []).length === repeats
        && (repeats === 0 || back.inners[repeats - 1].value === repeats)
        && back.opt_i32 === -7
        && back.u64 === 18446744073709551615n
        && back.color === 2;
}

/* An arm is present only when its module exists and has the contract. */
function arm(name, file) {
    let mod;
    try {
        mod = require(file);
    } catch (err) {
        fail(`${name} arm missing: ${file} (encodeObject)`);
        return null;
    }
    if (typeof mod.marshal !== 'function' || typeof mod.unmarshal !== 'function') {
        fail(`${name} arm missing: ${file} exports marshal/unmarshal`);
        return null;
    }
    return mod;
}

function check(name, mod) {
    const type = typeOf();
    if (typeof type.encodeObject !== 'function' || typeof type.decodeToObject !== 'function') {
        fail(`${name} arm missing: encodeObject/decodeToObject`);
        return;
    }
    for (const { label, repeats, bytes } of TIERS) {
        const obj = nativeObject(repeats);
        const ref = Buffer.from(official(repeats).serializeBinary());
        if (ref.length !== bytes) {
            fail(`${label}: jspb wrote ${ref.length} bytes, expected ${bytes}`);
            continue;
        }
        const wire = mod.marshal(obj);
        if (!Buffer.from(wire).equals(ref)) {
            fail(`${label}: ${name} encode differs from jspb`);
            continue;
        }
        const back = mod.unmarshal(ref);
        if (!decodeOk(back, repeats)) {
            fail(`${label}: ${name} decode differs`);
            continue;
        }
        // The decoded object re-encodes to the same bytes: a decode the
        // caller can hand back is part of the contract.
        if (!Buffer.from(mod.marshal(back)).equals(ref)) {
            fail(`${label}: ${name} re-encode differs`);
            continue;
        }
        console.log(`OK   ${label} ${name} bytes and decode (${bytes} bytes)`);
    }
}

const a = arm('a-tree', './a-tree.js');
const b = arm('shim', './b-shim.js');

if (a !== null) {
    check('a-tree', a);
}
if (b !== null) {
    check('shim', b);
}

if (failures !== 0) {
    console.log(`\n${failures} gate check(s) failed`);
    process.exit(1);
}
console.log('\ngate: PASS');

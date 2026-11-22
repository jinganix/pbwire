'use strict';

/* Interleaved timing for the Node arms: per round, `a-tree`, `b-shim`,
 * `c-core`, jspb and protobufjs run back to back on the same payload; per tier
 * the median of five rounds is printed. The bytes are compared with jspb before anything is
 * timed (the gate does this too; this is the last guard before a number).
 *
 *   node bindings/node/pbench/compare.js
 *
 * The `tree` row here times the whole user task (build the handle with one
 * set() per field, then encode; decode into a handle, then read every field),
 * so it is not the prebuilt-handle row `make node-bench` prints as
 * `pbwire-node-tree`.
 */

const path = require('node:path');

const { TIERS, nativeObject, typeOf } = require('./payload');
const armA = require('./a-tree');
const armB = require('./b-shim');
const armC = require('./c-core');
const { Scalars3, Inner3, Color3 } = require('../benchmarks/gen/matrix3_pb.js');

const benchDir = path.join(__dirname, '..', 'benchmarks');
const protobufjs = require(path.join(benchDir, 'node_modules', 'protobufjs'));
const ProtobufJsType = protobufjs
    .loadSync(path.join(benchDir, '..', '..', 'c', 'tests', 'data', 'matrix3.proto'))
    .lookupType('structtest.Scalars3');

const ROUNDS = 5;
const PASS_SECONDS = 0.1;

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

function protobufJsObject(repeats) {
    const nums = [];
    const tags = [];
    const inners = [];
    const flags = [];
    for (let i = 0; i < repeats; i++) {
        nums.push(i - 3);
        tags.push('tag');
        inners.push({ name: 'grandkid', value: i + 1 });
        flags.push(i % 2 === 0);
    }
    return ProtobufJsType.fromObject({
        i32: -2147483648,
        i64: '9223372036854775807',
        u32: 4294967295,
        u64: '18446744073709551615',
        s32: -2147483648,
        s64: '-9223372036854775808',
        fx32: 3735928559,
        fx64: '72623859790382856',
        sfx32: -2147483648,
        sfx64: '-9223372036854775808',
        f32: Infinity,
        f64: -123456.789,
        flag: true,
        str: 'hello',
        color: 2,
        inner: { name: 'kid', value: 42 },
        nums,
        tags,
        inners,
        optI32: -7,
        optStr: 'opt',
        flags,
    });
}

function run(count, op) {
    const start = process.hrtime.bigint();
    for (let i = 0; i < count; i++) {
        op();
    }
    return Number(process.hrtime.bigint() - start) / count;
}

/* One count per op, double from the floor until one pass takes PASS_SECONDS. */
function calibrate(op) {
    let count = 1000;
    for (;;) {
        const ns = run(count, op);
        if (ns * count >= PASS_SECONDS * 1e9 || count >= 10_000_000) {
            return count;
        }
        count *= 2;
    }
}

function median(values) {
    const sorted = [...values].sort((a, b) => a - b);
    return sorted[Math.floor(sorted.length / 2)];
}

/* Ops are (name, impl, op, fn). Every round runs every op once. */
function timeOps(ops) {
    const counts = ops.map(([, , , fn]) => calibrate(fn));
    const samples = ops.map(() => []);
    for (let round = 0; round < ROUNDS; round++) {
        ops.forEach(([, , , fn], i) => {
            samples[i].push(run(counts[i], fn));
        });
    }
    return ops.map(([, impl, op], i) => ({ impl, op, ns: median(samples[i]) }));
}

function row(impl, op, bytes, ns) {
    console.log(`  ${impl.padEnd(17)} ${op.padEnd(7)} ${ns.toFixed(1).padStart(12)} ` +
        `${((bytes / ns) * 1000).toFixed(1).padStart(10)}`);
}

function main() {
    if (typeof typeOf().encodeObject !== 'function' ||
        typeof typeOf().decodeToObject !== 'function') {
        console.log('FAIL shim arm missing: encodeObject');
        process.exit(1);
    }
    const only = process.env.PBENCH_TIER || null;
    for (const { label, repeats, bytes, iterations } of TIERS) {
        if (only !== null && only !== label) continue;
        const ref = Buffer.from(official(repeats).serializeBinary());
        if (ref.length !== bytes) {
            console.log(`FAIL ${label}: jspb wrote ${ref.length} bytes, expected ${bytes}`);
            process.exit(1);
        }
        const obj = nativeObject(repeats);
        const aWire = armA.marshal(obj);
        const bWire = armB.marshal(obj);
        const cWire = armC.marshal(obj);
        const jspb = official(repeats);
        const pjs = protobufJsObject(repeats);
        const pjsWire = Buffer.from(ProtobufJsType.encode(pjs).finish());

        if (!Buffer.from(aWire).equals(ref) || !Buffer.from(bWire).equals(ref) ||
            !Buffer.from(cWire).equals(ref) || !pjsWire.equals(ref)) {
            console.log(`FAIL ${label}: arms and baselines disagree on bytes`);
            process.exit(1);
        }
        console.log(`${label}: ${bytes} bytes, ${iterations} iterations ` +
            `(repeat field x${repeats})`);
        console.log(`  ${'impl'.padEnd(17)} ${'op'.padEnd(7)} ${'ns/op'.padStart(12)} ` +
            `${'MB/s'.padStart(10)}`);
        const enc = timeOps([
            ['tree', 'pbwire-node-tree', 'encode', () => armA.marshal(obj)],
            ['shim', 'pbwire-node-shim', 'encode', () => armB.marshal(obj)],
            ['core', 'pbwire-node-core', 'encode', () => armC.marshal(obj)],
            ['official', 'google-protobuf', 'encode', () => jspb.serializeBinary()],
            ['protobufjs', 'protobufjs', 'encode', () => ProtobufJsType.encode(pjs).finish()],
        ]);
        for (const { impl, op, ns } of enc) {
            row(impl, op, bytes, ns);
        }
        const dec = timeOps([
            ['tree', 'pbwire-node-tree', 'decode', () => armA.unmarshal(ref)],
            ['shim', 'pbwire-node-shim', 'decode', () => armB.unmarshal(ref)],
            ['core', 'pbwire-node-core', 'decode', () => armC.unmarshal(ref)],
            ['official', 'google-protobuf', 'decode', () => Scalars3.deserializeBinary(ref)],
            ['protobufjs', 'protobufjs', 'decode', () => ProtobufJsType.decode(ref)],
        ]);
        for (const { impl, op, ns } of dec) {
            row(impl, op, bytes, ns);
        }
        console.log();
    }
}

main();

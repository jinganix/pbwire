'use strict';

/* Node.js binding benchmark against google-protobuf (jspb) generated code and
 * protobufjs (the fastest widely used JS runtime) on the same matrix3.proto
 * and the four standard tiers from BENCHMARKS.md: every scalar, the nested
 * message, the proto3 optionals, and the repeated fields scaled x0 / x1 /
 * x100 / x3000 (136 / 174 / 2469 / 74913 bytes). All sides must produce
 * identical bytes before anything is timed.
 *
 *   ./bench.sh        generate, build, run
 */

const path = require('node:path');

const { Schema } = require(path.join(__dirname, '..', 'lib', 'pbwire'));
const { Scalars3, Inner3, Color3 } = require(path.join(__dirname, 'gen', 'matrix3_pb.js'));

/* Second baseline: protobufjs, the fastest widely used JS runtime. Optional:
 * skipped when it is not installed (bench.sh installs it). */
let ProtobufJsType = null;
try {
    const protobufjs = require('protobufjs');
    ProtobufJsType = protobufjs
        .loadSync(path.join(__dirname, '..', '..', 'c', 'tests', 'data', 'matrix3.proto'))
        .lookupType('structtest.Scalars3');
} catch {
    /* leave null: rows below are skipped */
}

const CASES = [
    { label: 'small', repeats: 0, want: 136, iterations: 50000 },
    { label: 'medium', repeats: 1, want: 174, iterations: 50000 },
    { label: 'large', repeats: 100, want: 2469, iterations: 20000 },
    { label: 'xlarge', repeats: 3000, want: 74913, iterations: 2000 },
];

let failures = 0;

function fillOfficial(n) {
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
    for (let i = 0; i < n; i++) {
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

function fillProtobufJs(n) {
    const inners = [];
    const nums = [];
    const tags = [];
    const flags = [];
    for (let i = 0; i < n; i++) {
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

function fillPbwire(type, n) {
    const m = type.newMessage();
    m.set('i32', -2147483648)
        .set('i64', 9223372036854775807n)
        .set('u32', 4294967295)
        .set('u64', 18446744073709551615n)
        .set('s32', -2147483648)
        .set('s64', -9223372036854775808n)
        .set('fx32', 3735928559)
        .set('fx64', 72623859790382856n)
        .set('sfx32', -2147483648)
        .set('sfx64', -9223372036854775808n)
        .set('f32', Infinity)
        .set('f64', -123456.789)
        .set('flag', true)
        .set('str', 'hello')
        .set('color', 2)
        .set('opt_i32', -7)
        .set('opt_str', 'opt');
    m.set('inner', { name: 'kid', value: 42 });
    const nums = [];
    const tags = [];
    const inners = [];
    const flags = [];
    for (let i = 0; i < n; i++) {
        nums.push(i - 3);
        tags.push('tag');
        inners.push({ name: 'grandkid', value: i + 1 });
        flags.push(i % 2 === 0);
    }
    m.set('nums', nums).set('tags', tags).set('inners', inners).set('flags', flags);
    return m;
}

/* Lowest tier share of the unified standard (BENCHMARKS.md): double from the
 * floor until one pass takes 0.25 s. */
const MIN_PASS_SECONDS = 0.25;

function calibrate(op, floor) {
    let count = Math.max(1, floor);
    for (;;) {
        const start = process.hrtime.bigint();
        for (let i = 0; i < count; i++) {
            op();
        }
        if (Number(process.hrtime.bigint() - start) / 1e9 >= MIN_PASS_SECONDS ||
            count >= 100_000_000) {
            return count;
        }
        count *= 2;
    }
}

/* Every op calibrates independently from the floor: a slow op already exceeds
 * the minimum at the floor and keeps it, a fast op doubles up. */
function measure(floor, op) {
    const count = calibrate(op, floor);
    run(count, op);
    let best = Infinity;
    for (let pass = 0; pass < 3; pass++) {
        best = Math.min(best, run(count, op));
    }
    return best;
}

function run(iterations, op) {
    const start = process.hrtime.bigint();
    for (let i = 0; i < iterations; i++) {
        op();
    }
    return Number(process.hrtime.bigint() - start) / iterations;
}

function row(impl, op, bytes, ns) {
    console.log(`  ${impl.padEnd(14)} ${op.padEnd(7)} ${ns.toFixed(1).padStart(12)} ` +
        `${((bytes / ns) * 1000).toFixed(1).padStart(10)}`);
}

function main() {
    const schema = Schema.fromFile(path.join(__dirname, 'gen', 'matrix3.desc'));
    const type = schema.message('structtest.Scalars3');

    for (const { label, repeats, want, iterations } of CASES) {
        const reference = Buffer.from(fillOfficial(repeats).serializeBinary());
        if (reference.length !== want) {
            console.log(`FAIL ${label}: official encoder wrote ${reference.length} bytes, ` +
                `expected ${want}`);
            failures++;
            continue;
        }
        const message = fillPbwire(type, repeats);
        if (!message.encode().equals(reference)) {
            console.log(`FAIL ${label}: pbwire-node and google-protobuf encode differently`);
            failures++;
            message.free();
            continue;
        }
        // Decode must round-trip byte-exactly and expose the same values.
        const back = type.decode(reference);
        const backBytes = back.encode();
        const backOk = backBytes.equals(reference)
            && back.get('str') === 'hello'
            && back.get('inner').name === 'kid'
            && back.get('inner').value === 42
            && (back.get('nums') ?? []).length === repeats
            && (back.get('inners') ?? []).length === repeats
            && (repeats === 0 || back.get('inners')[repeats - 1].value === repeats);
        back.free();
        if (!backOk) {
            console.log(`FAIL ${label}: pbwire-node decode differs`);
            failures++;
            message.free();
            continue;
        }
        console.log(`OK   ${label} encoding parity, x${repeats} (${reference.length} bytes)`);

        const official = fillOfficial(repeats);
        // Tier line shows the reference op's count; slower ops keep the floor.
        const count = calibrate(() => message.encode(), iterations);
        const encodeNs = measure(iterations, () => message.encode());
        const decodeNs = measure(iterations, () => type.decode(reference).free());
        const officialEncodeNs = measure(iterations, () => official.serializeBinary());
        const officialDecodeNs = measure(iterations, () => Scalars3.deserializeBinary(reference));
        let pjs = null;
        let pjsEncodeNs = 0;
        let pjsDecodeNs = 0;
        if (ProtobufJsType !== null) {
            pjs = fillProtobufJs(repeats);
            if (!Buffer.from(ProtobufJsType.encode(pjs).finish()).equals(reference)) {
                console.log(`FAIL ${label}: protobufjs encodes differently`);
                failures++;
                message.free();
                continue;
            }
            pjsEncodeNs = measure(iterations, () => ProtobufJsType.encode(pjs).finish());
            pjsDecodeNs = measure(iterations, () => ProtobufJsType.decode(reference));
        }
        message.free();

        console.log(`${label}: ${reference.length} bytes, ${count} iterations ` +
            `(repeat field x${repeats})`);
        console.log(`  ${'impl'.padEnd(14)} ${'op'.padEnd(7)} ${'ns/op'.padStart(12)} ` +
            `${'MB/s'.padStart(10)}`);
        row('pbwire-node', 'encode', reference.length, encodeNs);
        row('google-protobuf', 'encode', reference.length, officialEncodeNs);
        if (pjs !== null) {
            row('protobufjs', 'encode', reference.length, pjsEncodeNs);
        }
        row('pbwire-node', 'decode', reference.length, decodeNs);
        row('google-protobuf', 'decode', reference.length, officialDecodeNs);
        if (pjs !== null) {
            row('protobufjs', 'decode', reference.length, pjsDecodeNs);
        }
        console.log();
    }
    if (failures !== 0) {
        console.error(`${failures} check(s) failed`);
        process.exit(1);
    }
}

main();

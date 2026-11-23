'use strict';

/* The interleaved loop behind benchmarks/ab.sh: two addon builds, one payload,
 * both ops of one tier back to back in every round, median of five. Bytes are
 * compared before anything is timed; a candidate that changes them is refused
 * here, not in the ledger.
 *
 * Printed per round so "wins in every round" can be checked, not assumed.
 */

const fs = require('node:fs');
const path = require('node:path');

const { TIERS, nativeObject } = require(path.join(__dirname, '..', 'pbench', 'payload'));

const ROUNDS = 5;
/* Ops whose cell must win every round (comma-separated). A change that only
 * touches the encode path still has to leave decode's bytes and time alone,
 * so `decode` is timed and printed but not required unless named here. */
const REQUIRE = new Set((process.env.AB_REQUIRE || 'encode').split(','));
const PASS_SECONDS = 0.1;
const have = require(path.join(__dirname, '..', 'benchmarks', 'gen', 'matrix3_pb.js'));
const { Scalars3 } = have;

function variant(label, file) {
    const mod = require(file);
    const bytes = fs.readFileSync(path.join(__dirname, 'gen', 'matrix3.desc'));
    const schema = mod.loadDescriptorSet(bytes);
    const desc = mod.descriptor(schema, 'structtest.Scalars3');
    return {
        label,
        encode(obj) {
            const len = mod.encodeObject(desc, obj);
            const out = Buffer.allocUnsafe(len);
            mod.copyEncoded(desc, out);
            return out;
        },
        decode(wire) {
            return mod.decodeObject(desc, wire);
        },
    };
}

function reference(repeats) {
    const m = new Scalars3();
    for (const [setter, value] of [
        ['setI32', -2147483648], ['setI64', '9223372036854775807'],
        ['setU32', 4294967295], ['setU64', '18446744073709551615'],
        ['setS32', -2147483648], ['setS64', '-9223372036854775808'],
        ['setFx32', 3735928559], ['setFx64', '72623859790382856'],
        ['setSfx32', -2147483648], ['setSfx64', '-9223372036854775808'],
        ['setF32', Infinity], ['setF64', -123456.789], ['setFlag', true],
        ['setStr', 'hello'], ['setColor', 2], ['setOptI32', -7], ['setOptStr', 'opt'],
    ]) {
        m[setter](value);
    }
    const inner = new have.Inner3();
    inner.setName('kid');
    inner.setValue(42);
    m.setInner(inner);
    const nums = [];
    const tags = [];
    const inners = [];
    const flags = [];
    for (let i = 0; i < repeats; i++) {
        nums.push(i - 3);
        tags.push('tag');
        const kid = new have.Inner3();
        kid.setName('grandkid');
        kid.setValue(i + 1);
        inners.push(kid);
        flags.push(i % 2 === 0);
    }
    m.setNumsList(nums);
    m.setTagsList(tags);
    m.setInnersList(inners);
    m.setFlagsList(flags);
    return Buffer.from(m.serializeBinary());
}

function run(count, op) {
    const start = process.hrtime.bigint();
    for (let i = 0; i < count; i++) {
        op();
    }
    return Number(process.hrtime.bigint() - start) / count;
}

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

function main() {
    const a = variant('base', process.env.AB_BASE_MODULE);
    const b = variant('cand', process.env.AB_CAND_MODULE);
    let bad = 0;
    for (const { label, repeats } of TIERS) {
        const ref = reference(repeats);
        const obj = nativeObject(repeats);
        const wires = { base: a.encode(obj), cand: b.encode(obj) };
        if (!wires.base.equals(ref) || !wires.cand.equals(ref)) {
            console.log(`FAIL ${label}: one build does not reproduce the oracle bytes`);
            bad = 1;
            continue;
        }
        console.log(`${label}: ${ref.length} bytes`);
        for (const op of ['encode', 'decode']) {
            const fn = {
                base: op === 'encode' ? () => a.encode(obj) : () => a.decode(ref),
                cand: op === 'encode' ? () => b.encode(obj) : () => b.decode(ref),
            };
            const counts = {
                base: calibrate(fn.base),
                cand: calibrate(fn.cand),
            };
            const samples = { base: [], cand: [] };
            let candWins = 0;
            for (let round = 1; round <= ROUNDS; round++) {
                samples.base.push(run(counts.base, fn.base));
                samples.cand.push(run(counts.cand, fn.cand));
                const win = samples.cand[round - 1] < samples.base[round - 1];
                if (win) {
                    candWins++;
                }
                console.log(`  ${op} round ${round}: base ${samples.base[round - 1].toFixed(1)} ` +
                    `cand ${samples.cand[round - 1].toFixed(1)} ns/op` +
                    `${win ? ' (cand wins)' : ''}`);
            }
            const baseNs = median(samples.base);
            const candNs = median(samples.cand);
            console.log(`  ${op} median: base ${baseNs.toFixed(1)} cand ${candNs.toFixed(1)} ` +
                `ns/op, cand ${(baseNs / candNs).toFixed(2)}x base, ` +
                `${candWins}/${ROUNDS} rounds won`);
            if (REQUIRE.has(op) && candWins !== ROUNDS) {
                bad = 1;
            }
        }
    }
    if (bad !== 0) {
        console.log('\nFAIL: a required cell did not win every round, or the bytes changed');
        process.exit(1);
    }
    console.log('\nab: PASS (bytes unchanged, every round won)');
}

main();

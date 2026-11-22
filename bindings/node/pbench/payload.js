'use strict';

/* The one payload every pbench arm times, and the one the official runtime
 * (jspb) encodes: structtest.Scalars3 scaled over the four standard tiers.
 *
 * A native object in, wire bytes out; wire bytes in, a fully readable native
 * object out. The object is plain: proto field names, 64-bit fields are
 * BigInt, nested messages and repeated fields are plain objects/arrays, so
 * every arm receives the same thing a user of that path would hand it.
 */

const path = require('node:path');
const Module = require('node:module');

const benchDir = path.join(__dirname, '..', 'benchmarks');
// benchmarks/gen/matrix3_pb.js requires google-protobuf, which bench.sh (or
// pbench/setup.sh) installs into benchmarks/node_modules. NODE_PATH is read
// at startup, so add the harness install the way `NODE_PATH=... node` would.
{
    const modules = path.join(benchDir, 'node_modules');
    process.env.NODE_PATH = process.env.NODE_PATH
        ? modules + path.delimiter + process.env.NODE_PATH
        : modules;
    if (typeof Module._initPaths === 'function') {
        Module._initPaths();
    }
}

const { Schema } = require(path.join(__dirname, '..', 'lib', 'pbwire'));

const TIERS = [
    { label: 'small', repeats: 0, bytes: 136, iterations: 50000 },
    { label: 'medium', repeats: 1, bytes: 174, iterations: 50000 },
    { label: 'large', repeats: 100, bytes: 2469, iterations: 20000 },
    { label: 'xlarge', repeats: 3000, bytes: 74913, iterations: 2000 },
];

/* The type's field names in descriptor order: what an arm reads back after
 * a decode. Kept here so both arm modules and the gate agree on one list. */
const FIELDS = [
    'i32', 'i64', 'u32', 'u64', 's32', 's64', 'fx32', 'fx64', 'sfx32',
    'sfx64', 'f32', 'f64', 'flag', 'str', 'color', 'inner', 'nums', 'tags',
    'inners', 'opt_i32', 'opt_str', 'flags',
];

let cached = null;

/** The Scalars3 type of benchmarks/gen/matrix3.desc. */
function typeOf() {
    if (cached === null) {
        const schema = Schema.fromFile(path.join(benchDir, 'gen', 'matrix3.desc'));
        cached = schema.message('structtest.Scalars3');
    }
    return cached;
}

/** The native object for `repeats` scaled elements. */
function nativeObject(repeats) {
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
    // `undefined` would mean absent: the proto3 optional fields and the
    // implicit zeros are all present here, as the matrix3 payload states.
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
        color: 2,
        inner: { name: 'kid', value: 42 },
        nums,
        tags,
        inners,
        opt_i32: -7,
        opt_str: 'opt',
        flags,
    };
}

module.exports = { TIERS, FIELDS, typeOf, nativeObject };

'use strict';

/* The `core` arm: a pure-TypeScript port of the C core. `marshal` builds the
 * dynamic tree from the native object and encodes it; `unmarshal` decodes the
 * wire bytes into that tree and reads it back as a plain object. No native
 * library, no N-API call, no per-field boundary crossing: the whole task runs
 * in the JS heap.
 */

const path = require('node:path');

const { Schema } = require(path.join(__dirname, '..', 'lib', 'core', 'schema.js'));

let cached = null;

/** The Scalars3 type of benchmarks/gen/matrix3.desc, loaded once. */
function typeOf() {
    if (cached === null) {
        const schema = Schema.fromFile(path.join(__dirname, '..', 'benchmarks', 'gen', 'matrix3.desc'));
        cached = schema.message('structtest.Scalars3');
    }
    return cached;
}

module.exports = {
    name: 'c-core',

    marshal(obj) {
        return typeOf().encodeObject(obj);
    },

    unmarshal(wire) {
        return typeOf().decodeObject(wire);
    },
};

'use strict';

/* The `shim` arm: one N-API downcall per op. `encodeObject` hands the native
 * object to the addon, which walks the schema's field table in C and fills a
 * tree it frees again; `decodeToObject` validates and materializes straight
 * into a plain object. No per-field set()/get() round trip either way.
 */

const { typeOf } = require('./payload');

module.exports = {
    name: 'b-shim',

    marshal(obj) {
        return typeOf().encodeObject(obj);
    },

    unmarshal(wire) {
        return typeOf().decodeToObject(wire);
    },
};

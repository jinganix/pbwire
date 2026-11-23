'use strict';

/* The `tree` arm: the data in the binding's own message handle. Marshal
 * builds a pbw_message with one set() per field and encodes it; Unmarshal
 * decodes into a handle and reads every field back out. What a user of the
 * per-field API pays for the whole task.
 */

const { FIELDS, typeOf } = require('./payload');

module.exports = {
    name: 'a-tree',

    marshal(obj) {
        const type = typeOf();
        const message = type.newMessage();
        for (const name of FIELDS) {
            const value = obj[name];
            if (value !== undefined) {
                message.set(name, value);
            }
        }
        const wire = message.encode();
        message.free();
        return wire;
    },

    unmarshal(wire) {
        const type = typeOf();
        const message = type.decode(wire);
        const out = {};
        for (const name of FIELDS) {
            const value = message.get(name);
            if (value !== undefined) {
                out[name] = value;
            }
        }
        message.free();
        return out;
    },
};

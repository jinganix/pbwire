'use strict';

/* The C example (examples/addressbook.c), in Node, through the generated types:
 * the plain object below is what `matrix3.d.ts`-style declarations describe, and
 * MessageType.encode()/decode() are the whole codec. The encoded bytes must
 * equal the pinned reference examples/addressbook.bin.
 *
 *   node bindings/node/examples/roundtrip.js
 */

const fs = require('node:fs');
const path = require('node:path');

const { Schema } = require(path.join(__dirname, '..', 'lib', 'pbwire.js'));
const { Tutorial_Person_PhoneType } = require(path.join(__dirname, 'gen', 'addressbook.js'));

const ROOT = path.join(__dirname, '..', '..', '..');
const DESC = path.join(ROOT, 'examples', 'addressbook.desc');
const BIN = path.join(ROOT, 'examples', 'addressbook.bin');

/** @returns {import('./gen/addressbook.js').AddressBook} */
function build() {
  return {
    people: [
      {
        name: 'Alice',
        id: 1,
        phones: [{ number: '555-0100', type: Tutorial_Person_PhoneType.MOBILE }],
      },
      {
        name: 'Bob',
        id: 2,
        email: 'bob@example.com',
        phones: [{ number: '555-0200', type: Tutorial_Person_PhoneType.HOME }],
      },
    ],
  };
}

const schema = Schema.fromFile(DESC);
const bookType = schema.message('tutorial.AddressBook');

const wire = bookType.encode(build());
if (fs.existsSync(BIN) && !wire.equals(fs.readFileSync(BIN))) {
  throw new Error(`encode does not match ${BIN}`);
}

const book = bookType.decode(wire);
const people = book.get('people');
console.log(`addressbook: ${wire.length} bytes, ${people.length} people`);
for (const person of people) {
  console.log(`  ${person.name} (${person.id}) ${person.email ?? ''}`);
  for (const phone of person.phones) {
    console.log(`    ${phone.number}`);
  }
}

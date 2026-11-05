'use strict';

/* The C example (examples/addressbook.c), in Node, through the generated types:
 * the plain object below is what the generated `addressbook.d.ts` describes,
 * and AddressBook.encode()/decode() are the whole codec. The schema travels
 * embedded in the generated code; no .desc file is read. The encoded bytes
 * must equal the pinned reference examples/addressbook.bin.
 *
 *   node bindings/node/examples/roundtrip.js
 */

const fs = require('node:fs');
const path = require('node:path');

const { AddressBook, Tutorial_Person_PhoneType } = require(path.join(__dirname, 'gen', 'addressbook.js'));

const ROOT = path.join(__dirname, '..', '..', '..');
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

const wire = AddressBook.encode(build()).finish();
if (fs.existsSync(BIN) && !wire.equals(fs.readFileSync(BIN))) {
  throw new Error(`encode does not match ${BIN}`);
}

const book = AddressBook.toObject(AddressBook.decode(wire));
const people = book.people;
console.log(`addressbook: ${wire.length} bytes, ${people.length} people`);
for (const person of people) {
  console.log(`  ${person.name} (${person.id}) ${person.email ?? ''}`);
  for (const phone of person.phones) {
    console.log(`    ${phone.number}`);
  }
}

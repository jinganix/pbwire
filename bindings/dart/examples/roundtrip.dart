// The C example (examples/addressbook.c), in Dart, through the generated typed
// classes: assign fields, encode, decode, compare. The encoded bytes must equal
// the pinned reference examples/addressbook.bin.
//
//   cd bindings/dart && PBWIRE_LIB=../../build/libpbwire.dylib \
//     dart run examples/roundtrip.dart

import 'dart:io';

import 'gen/addressbook.dart';

void main(List<String> args) {
  final bin = File(args.isNotEmpty ? args[0] : '../../examples/addressbook.bin');

  final book = build();
  final wire = book.writeToBuffer();
  if (bin.existsSync()) {
    final reference = bin.readAsBytesSync();
    if (wire.length != reference.length) {
      throw StateError('encode does not match ${bin.path}');
    }
    for (var i = 0; i < wire.length; i++) {
      if (wire[i] != reference[i]) {
        throw StateError('encode does not match ${bin.path}');
      }
    }
  }

  final back = AddressBook.fromBuffer(wire);
  final decoded = back.people;
  print('addressbook: ${wire.length} bytes, ${decoded.length} people');
  for (final person in decoded) {
    print(
        '  ${person.name} (${person.id}) ${person.hasEmail() ? person.email : ''}');
    for (final phone in person.phones) {
      print('    ${phone.number}');
    }
  }
}

AddressBook build() => AddressBook(people: [
      Person(
        name: 'Alice',
        id: 1,
        phones: [phone('555-0100', Person_PhoneType.MOBILE)],
      ),
      Person(
        name: 'Bob',
        id: 2,
        email: 'bob@example.com',
        phones: [phone('555-0200', Person_PhoneType.HOME)],
      ),
    ]);

Person_PhoneNumber phone(String number, Person_PhoneType type) =>
    Person_PhoneNumber(number: number, type_: type);

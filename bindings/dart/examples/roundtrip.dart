// The C example (examples/addressbook.c), in Dart, through the generated typed
// classes: assign fields, encode, decode, compare. The encoded bytes must equal
// the pinned reference examples/addressbook.bin.
//
//   cd bindings/dart && PBWIRE_LIB=../../build/libpbwire.dylib \
//     dart run examples/roundtrip.dart

import 'dart:io';

import 'package:pbwire/pbwire.dart';

import 'gen/addressbook.dart';

void main(List<String> args) {
  final desc = args.isNotEmpty ? args[0] : '../../examples/addressbook.desc';
  final bin = File(args.length > 1 ? args[1] : '../../examples/addressbook.bin');

  final schema = Schema.load(desc);
  try {
    final book = build();
    final wire = book.encode(schema);
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

    final back = AddressBook.decode(schema, wire);
    final decoded = back.people;
    print('addressbook: ${wire.length} bytes, ${decoded.length} people');
    for (final person in decoded) {
      print('  ${person.name} (${person.id}) ${person.hasEmail ? person.email : ''}');
      for (final phone in person.phones) {
        print('    ${phone.number}');
      }
    }
  } finally {
    schema.close();
  }
}

AddressBook build() => AddressBook()
  ..people = [
    Person()
      ..name = 'Alice'
      ..id = 1
      ..phones = [phone('555-0100', Person_PhoneType.MOBILE)],
    Person()
      ..name = 'Bob'
      ..id = 2
      ..hasEmail = true
      ..email = 'bob@example.com'
      ..phones = [phone('555-0200', Person_PhoneType.HOME)],
  ];

Person_PhoneNumber phone(String number, int type) => Person_PhoneNumber()
  ..number = number
  ..hasType = true
  ..type_ = type;

"""The C example (examples/addressbook.c), in Python, through the generated
typed dataclasses: assign fields, encode, decode, compare. The encoded bytes
must equal the pinned reference examples/addressbook.bin.

    python3 bindings/python/examples/addressbook.py
"""

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
sys.path.insert(0, os.path.join(HERE, "gen"))

import pbwire  # noqa: E402

import addressbook_pbwire as ab  # noqa: E402

DESC = os.path.join(HERE, "..", "..", "..", "examples", "addressbook.desc")
BIN = os.path.join(HERE, "..", "..", "..", "examples", "addressbook.bin")


def phone(number, kind):
    return ab.Person_PhoneNumber(number=number, type=kind)


def build():
    alice = ab.Person(
        name="Alice",
        id=1,
        phones=[phone("555-0100", ab.Person_PhoneType.MOBILE)],
    )
    bob = ab.Person(
        name="Bob",
        id=2,
        email="bob@example.com",
        phones=[phone("555-0200", ab.Person_PhoneType.HOME)],
    )
    return ab.AddressBook(people=[alice, bob])


def main():
    with pbwire.Schema.from_file(DESC) as schema:
        book_type = schema.message("tutorial.AddressBook")
        wire = build().encode(book_type)
        if os.path.exists(BIN):
            with open(BIN, "rb") as f:
                if wire != f.read():
                    raise SystemExit(f"encode does not match {BIN}")

        book = ab.AddressBook.decode(book_type, wire)
        print(f"addressbook: {len(wire)} bytes, {len(book.people)} people")
        for person in book.people:
            print(f"  {person.name} ({person.id}) {person.email or ''}")
            for entry in person.phones:
                print(f"    {entry.number}")


if __name__ == "__main__":
    main()

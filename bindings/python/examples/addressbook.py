"""AddressBook example (Python): schema at runtime, messages parsed from bytes.

    python bindings/python/examples/addressbook.py

Needs the CPython extension (`make python-bind`) and the descriptor set that
`make` also builds for the C example (examples/addressbook.desc).
"""

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))

import pbwire

DESC = os.path.join(HERE, "..", "..", "..", "examples", "addressbook.desc")


def main():
    schema = pbwire.Schema.from_file(DESC)
    person = schema.message("tutorial.Person")
    values = {
        "name": "alice",
        "id": 1234,
        "email": "alice@example.com",
        "phones": [{"number": "555-1234", "type": 1},
                   {"number": "555-9999"}],
    }

    wire = person.encode(values)

    # decode() parses the bytes once; a field materializes when it is read.
    message = person.decode(wire)
    print("%d bytes, name=%s, %d phone(s), phone[0]=%s"
          % (len(wire), message.name, len(message.phones),
             message.phones[0].number))

    # The eager form: plain dicts, and a byte-exact round trip of the wire.
    assert message.to_dict() == values
    assert person.encode(message) == wire
    return 0


if __name__ == "__main__":
    sys.exit(main())

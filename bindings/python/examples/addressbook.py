"""AddressBook example (Python): schema at runtime, messages as dicts.

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
    assert person.decode(wire) == values

    # Lazy view: parse once, materialize only what is read.
    view = person.parse(wire)
    print("%d bytes, name=%s, %d phone(s), phone[0]=%s"
          % (len(wire), view.name, len(view.phones),
             view.phones[0].number))
    return 0


if __name__ == "__main__":
    sys.exit(main())

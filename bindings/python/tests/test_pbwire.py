"""Python binding self-check: no framework, plain asserts (run from the repo
root via `make python-test`, which builds the shared library first).

The oracle is the same one the C binding uses: `matrix3.txt` encoded by
`protoc --encode` into `matrix3.bin` (proto3, every scalar kind, string, enum,
message, three repeated kinds, proto3 optional).
"""

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))

import pbwire as pbw

DATA = "bindings/c/tests/data"
SCALARS3 = {
    "i32": -2147483648,
    "i64": 9223372036854775807,
    "u32": 4294967295,
    "u64": 18446744073709551615,
    "s32": -2147483648,
    "s64": -9223372036854775808,
    "fx32": 3735928559,
    "fx64": 72623859790382856,
    "sfx32": -2147483648,
    "sfx64": -9223372036854775808,
    "f32": float("inf"),
    "f64": -123456.789,
    "flag": True,
    "str": "hello",
    "color": 2,
    "inner": {"name": "kid", "value": 42},
    "nums": [1, -5, 300],
    "tags": ["a", "bb"],
    "inners": [{"name": "x", "value": 1}, {"name": "y", "value": -2}],
    "opt_i32": -7,
    "opt_str": "opt",
    "flags": [True, False, True],
}

failures = 0


def check(cond, what):
    global failures
    if not cond:
        print("FAIL %s" % what)
        failures += 1


def check_raises(exc, fn, what):
    global failures
    try:
        fn()
    except exc:
        return
    except Exception as error:  # noqa: BLE001 - report the wrong exception
        print("FAIL %s: raised %r" % (what, error))
        failures += 1
        return
    print("FAIL %s: no exception" % what)
    failures += 1


def main():
    check(pbw.abi_version() == 1, "ABI version matches the core")

    with open(os.path.join(DATA, "matrix3.bin"), "rb") as handle:
        reference = handle.read()

    with pbw.Schema.from_file(os.path.join(DATA, "matrix3.desc")) as schema:
        scalars = schema.message("structtest.Scalars3")

        got = scalars.encode(SCALARS3)
        check(got == reference, "encode equals protoc reference bytes (%d vs %d)"
              % (len(got), len(reference)))
        if got != reference:
            for i, (a, b) in enumerate(zip(got, reference)):
                if a != b:
                    print("  first diff at %d: %02x vs %02x" % (i, a, b))
                    break

        decoded = scalars.decode(reference)
        check(decoded == SCALARS3, "decode of the reference bytes")
        if decoded != SCALARS3:
            for key in sorted(set(decoded) | set(SCALARS3)):
                if decoded.get(key) != SCALARS3.get(key):
                    print("  %s: %r != %r"
                          % (key, decoded.get(key), SCALARS3.get(key)))

        check(scalars.decode(got) == SCALARS3, "decode of our own bytes")
        check(scalars.decode(b"") == {}, "empty wire decodes to no fields")
        check(scalars.encode({}) == b"", "empty dict encodes to no bytes")

        # Lazy view: attribute access materializes, dict(m) is the eager form.
        view = scalars.parse(reference)
        check(view.i32 == SCALARS3["i32"], "lazy view: scalar attribute")
        check(view.str == SCALARS3["str"], "lazy view: string attribute")
        check(view.nums == SCALARS3["nums"], "lazy view: packed repeated")
        check(view.inners[1].value == -2, "lazy view: nested message")
        check(view.inner.name == "kid", "lazy view: singular message")
        check(view.to_dict() == SCALARS3, "lazy view: to_dict")
        check(dict(view) == SCALARS3, "lazy view: dict(view)")
        check(len(view) == len(SCALARS3), "lazy view: length")
        check("nums" in view and "nope" not in view, "lazy view: contains")
        check_raises(KeyError, lambda: view["nope"], "lazy view: absent key")
        check(scalars.parse(b"").to_dict() == {}, "lazy view: empty wire")
        check(view.i32 == SCALARS3["i32"], "lazy view: cached read")

        # Unknown fields in the wire are skipped, not an error.
        # Field 999 is not in the schema: its bytes are skipped.
        check(scalars.decode(reference + b"\xb8\x3e\x2a") == SCALARS3,
              "unknown field in the wire is skipped")
        check(scalars.parse(reference + b"\xb8\x3e\x2a").to_dict() == SCALARS3,
              "unknown field in a lazy view is skipped")

        check_raises(KeyError, lambda: scalars.encode({"nope": 1}),
                     "unknown field name")
        check_raises(ValueError, lambda: scalars.decode(reference[:-1]),
                     "truncated wire")

        inner = schema.message("structtest.Inner3")
        check(inner.decode(inner.encode({"name": "x", "value": 1})) ==
              {"name": "x", "value": 1}, "nested type round trip")

    with pbw.Schema.from_file("examples/addressbook.desc") as schema:
        person = schema.message("tutorial.Person")
        values = {"name": "alice", "id": 1, "email": "a@example.com",
                  "phones": [{"number": "555", "type": 1},
                             {"number": "666"}]}
        check(person.decode(person.encode(values)) == values,
              "proto2 addressbook round trip")
        check_raises(ValueError, lambda: person.encode({"id": 1}),
                     "proto2 required field missing")
        check(person.parse(person.encode(values)).phones[1].number == "666",
              "proto2 lazy view")

    print("test_pbwire: %s" % ("all checks passed" if not failures
                               else "%d FAILURES" % failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())

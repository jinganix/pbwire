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

        message = scalars.decode(reference)
        check(message.to_dict() == SCALARS3, "decode(to_dict) of the reference")
        if message.to_dict() != SCALARS3:
            eager = message.to_dict()
            for key in sorted(set(eager) | set(SCALARS3)):
                if eager.get(key) != SCALARS3.get(key):
                    print("  %s: %r != %r"
                          % (key, eager.get(key), SCALARS3.get(key)))

        check(scalars.decode(got).to_dict() == SCALARS3,
              "decode of our own bytes")
        check(scalars.decode(b"").to_dict() == {}, "empty wire decodes to {}")
        check(scalars.encode({}) == b"", "empty dict encodes to no bytes")

        # A parsed message reads fields from the wire on demand.
        check(message.i32 == SCALARS3["i32"], "scalar attribute")
        check(message.str == SCALARS3["str"], "string attribute")
        check(message.nums == SCALARS3["nums"], "packed repeated")
        check(message.flags == SCALARS3["flags"], "packed repeated bool")
        check(message.inners[1].value == -2, "nested message")
        check(message.inners[1].to_dict() == {"name": "y", "value": -2},
              "nested message to_dict")
        check(message.inner.name == "kid", "singular message")
        check(message["i32"] == SCALARS3["i32"], "item access")
        check(message.get("nope") is None, "get() of an absent field")
        check(message.get("nope", 7) == 7, "get() default")
        check(message.get("nums") == SCALARS3["nums"], "get() of a field")
        check(len(message) == len(SCALARS3), "length is the field count")
        check(sorted(message.keys()) == sorted(SCALARS3),
              "keys() are the present fields")
        check(len(message.items()) == len(SCALARS3), "items() pair count")
        check(dict(message.items())["inner"].to_dict() == SCALARS3["inner"],
              "items() values")
        check("nums" in message and "nope" not in message, "contains")
        check_raises(KeyError, lambda: message["nope"], "absent key")
        check(message.type == "structtest.Scalars3", "message type name")

        # An absent field is absent in every view, repeated ones included.
        sparse = scalars.decode(b"")
        check(sparse.inner is None, "absent singular message reads as None")
        check(sparse.nums is None, "absent repeated field reads as None")
        check(sparse.get("nums") is None, "absent repeated field get()")
        check(not sparse, "empty message is empty")
        check(len(sparse) == 0, "empty message has no keys")
        check_raises(KeyError, lambda: sparse["nums"],
                     "absent repeated field is a KeyError")
        check(scalars.decode(b"").to_dict() == {}, "empty wire")

        # encode() of a Message hands back the bytes it was parsed from, so a
        # round trip keeps unknown fields that a dict round trip cannot.
        check(scalars.encode(message) == reference, "encode(Message) round trip")
        inner_type = schema.message("structtest.Inner3")
        check(inner_type.encode(message.inners[1]) ==
              inner_type.encode({"name": "y", "value": -2}),
              "encode(sub-message)")
        check(inner_type.encode(message.inner) == inner_type.encode(
                  {"name": "kid", "value": 42}), "encode(singular message)")
        check_raises(TypeError, lambda: inner_type.encode(message),
                     "encode() rejects another message type")

        # Unknown fields in the wire are skipped by the dict view, and kept by
        # the Message (it owns the bytes).
        unknown = reference + b"\xb8\x3e\x2a"
        check(scalars.decode(unknown).to_dict() == SCALARS3,
              "unknown field is skipped by to_dict")
        check(scalars.encode(scalars.decode(unknown)) == unknown,
              "unknown field survives encode(Message)")

        check_raises(KeyError, lambda: scalars.encode({"nope": 1}),
                     "unknown field name")
        check_raises(ValueError, lambda: scalars.decode(reference[:-1]),
                     "truncated wire")
        check_raises(ValueError,
                     lambda: scalars.decode(scalars.encode(
                         {"inner": {"name": "x"}})[:-1]),
                     "truncated nested message")
        check_raises(ValueError, lambda: scalars.decode(b"\x0f"),
                     "field number 0")

        inner = schema.message("structtest.Inner3")
        check(inner.decode(inner.encode({"name": "x", "value": 1})).to_dict() ==
              {"name": "x", "value": 1}, "nested type round trip")

        # Merge semantics: a singular message field that shows up twice on the
        # wire merges instead of replacing.
        merged = scalars.decode(b"\x82\x01\x03\x0a\x01a"      # inner.name = "a"
                                b"\x82\x01\x02\x10\x07")     # inner.value = 7
        check(merged.inner.to_dict() == {"name": "a", "value": 7},
              "singular message seen twice merges")
        check(merged.to_dict()["inner"] == {"name": "a", "value": 7},
              "merged message as a dict")

        # schema.message() hands out one MessageType per name.
        check(schema.message("structtest.Scalars3") is scalars,
              "message() is cached per schema")

    # Oneof: a member set later on the wire clears the earlier ones, and the
    # cleared member is absent from every view.
    with pbw.Schema.from_file("bindings/python/tests/data/oneof.desc") as schema:
        pick = schema.message("oneoftest.Pick")
        wire = b"\x08\x01\x12\x01z\x20\x05"  # a=1, b="z", tail=5
        message = pick.decode(wire)
        check(message.to_dict() == {"b": "z", "tail": 5},
              "oneof keeps only the last member")
        check(message.b == "z" and message.tail == 5, "oneof member value")
        check(message.a is None, "cleared oneof member reads as None")
        check("a" not in message and "b" in message, "cleared oneof contains")
        check(list(message.keys()) == ["b", "tail"], "cleared oneof in keys()")
        check(len(message) == 2, "cleared oneof not counted")
        check(pick.encode(message) == wire, "oneof round trip")

        # A singular message in a oneof merges with itself, like any other.
        merged = pick.decode(b"\x1a\x02\x08\x07\x1a\x02\x08\x08")
        check(merged.c.to_dict() == {"x": 8}, "oneof sub-message last wins")
        check(merged.to_dict() == {"c": {"x": 8}}, "oneof sub-message as dict")

    with pbw.Schema.from_file("examples/addressbook.desc") as schema:
        person = schema.message("tutorial.Person")
        values = {"name": "alice", "id": 1, "email": "a@example.com",
                  "phones": [{"number": "555", "type": 1},
                             {"number": "666"}]}
        message = person.decode(person.encode(values))
        check(message.to_dict() == values, "proto2 addressbook round trip")
        check(message.phones[1].number == "666", "proto2 nested message")
        check_raises(ValueError, lambda: person.encode({"id": 1}),
                     "proto2 required field missing")
        check_raises(ValueError,
                     lambda: person.decode(b"\x08\x01").to_dict(),
                     "proto2 required field missing on materialize")

    print("test_pbwire: %s" % ("all checks passed" if not failures
                               else "%d FAILURES" % failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())

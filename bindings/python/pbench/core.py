"""The `core` arm: the pure-Python core (`../pure/`), a port of the C core.

`marshal` builds the dynamic tree from the dict and encodes it; `unmarshal`
decodes the wire bytes into that tree and reads it back as a plain dict. No
CPython extension, no C call, no per-field boundary crossing: the whole task
runs in the interpreter.
"""

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))

from pure.schema import Schema  # noqa: E402

SCHEMA = Schema.from_file(os.path.join(HERE, "..", "..", "..", "bindings", "c",
                                       "tests", "data", "matrix3.desc"))
TYPE = SCHEMA.message("structtest.Scalars3")

NAME = "pbwire-py-core"


def marshal(obj):
    return TYPE.encode_object(obj)


def unmarshal(wire):
    return TYPE.decode_object(wire)

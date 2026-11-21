"""The `tree` arm: the shipped per-field full task through the Message handle.

`marshal` takes a plain dict (the native object) and encodes it; `unmarshal`
decodes the wire bytes and reads every gate-checked field back with one
`get()` per field into a fresh dict, recursing into the nested message and
the repeated message list. No prebuilt handle before the timer: the dict is
the only input, the wire the only output, and CPython's GC owns the handle
(no manual free exists on this path, same as the Java arm).

The per-field `get()` loop is the point: `to_dict()` walks the tree in one
call (that is the `shim` arm), while this arm pays one boundary crossing per
field, the same shape as the Node `a-tree.js` arm.
"""

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, ".."))  # pbwire extension

import payload  # noqa: E402

import pbwire as pbw  # noqa: E402

TYPE = pbw.Schema.from_file(payload.DESC).message("structtest.Scalars3")

NAME = "pbwire-py-tree"

# Every scalar the gate checks, in wire order. Repeated and message fields
# are materialized separately below (they need list/dict handling).
FIELDS = (
    "i32", "i64", "u32", "u64", "s32", "s64",
    "fx32", "fx64", "sfx32", "sfx64",
    "f32", "f64", "flag", "str", "color",
    "opt_i32", "opt_str",
)


def marshal(obj):
    return TYPE.encode(obj)


def unmarshal(wire):
    message = TYPE.decode(wire)
    out = {}
    for name in FIELDS:
        value = message.get(name)
        if value is not None:
            out[name] = value
    inner = message.get("inner")
    if inner is not None:
        out["inner"] = {"name": inner.get("name"), "value": inner.get("value")}
    for name in ("nums", "tags", "flags"):
        value = message.get(name)
        if value is not None:
            out[name] = list(value)
    inners = message.get("inners")
    if inners is not None:
        out["inners"] = [
            {"name": item.get("name"), "value": item.get("value")} for item in inners
        ]
    return out

"""Pure-Python core: the descriptor gate.

The gate every later stage builds on: the pure core's descriptor reader must
answer the same field facts for `structtest.Scalars3` that the generated model
and the C extension answer, from the same `matrix3.desc` the other suites use.
Nothing here imports `pbwire` (the CPython extension) or `google.protobuf`:
`bindings/python/pure/` is stdlib only.

    python3 bindings/python/tests/pure/test_descriptor.py     # or: pytest -q

Runs standalone (plain asserts, exit 1 on failure) so `make python-test` needs
no test framework.
"""

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", ".."))  # bindings/python

from pure.descriptor import (  # noqa: E402
    EXPLICIT,
    IMPLICIT,
    K,
    MESSAGE_PRESENCE,
    REQUIRED,
)
from pure.schema import Schema  # noqa: E402

DESC = os.path.join(HERE, "..", "..", "..", "..", "bindings", "c", "tests",
                    "data", "matrix3.desc")


def test_matrix3():
    schema = Schema.from_file(DESC)
    scalars = schema.message("structtest.Scalars3")
    def_ = scalars.def_

    assert def_.full_name == "structtest.Scalars3"
    # matrix3.proto: 22 declared fields.
    assert len(def_.fields) == 22, [f.name for f in def_.fields]

    # Fields are stored in field-number order, one slot each; the by-name
    # index points at the slot.
    numbers = [f.number for f in def_.fields]
    assert numbers == sorted(numbers)
    assert def_.by_name["opt_i32"] == 19  # field 20
    assert def_.by_name["flags"] == 21  # field 22

    by_name = {f.name: f for f in def_.fields}
    # Scalar kinds and their wire types.
    assert by_name["i32"].kind == K.INT32 and by_name["i32"].number == 1
    assert by_name["i64"].kind == K.INT64
    assert by_name["u32"].kind == K.UINT32
    assert by_name["u64"].kind == K.UINT64
    assert by_name["s32"].kind == K.SINT32
    assert by_name["s64"].kind == K.SINT64
    assert by_name["fx32"].kind == K.FIXED32
    assert by_name["fx64"].kind == K.FIXED64
    assert by_name["sfx32"].kind == K.SFIXED32
    assert by_name["sfx64"].kind == K.SFIXED64
    assert by_name["f32"].kind == K.FLOAT
    assert by_name["f64"].kind == K.DOUBLE
    assert by_name["flag"].kind == K.BOOL
    assert by_name["str"].kind == K.STRING and by_name["str"].utf8
    assert by_name["color"].kind == K.ENUM
    assert by_name["inner"].kind == K.MESSAGE
    assert by_name["inner"].msg_type.full_name == "structtest.Inner3"

    # proto3 optional: explicit presence, and not a oneof member the caller
    # sees (`proto3_optional` synthesizes a single-field oneof).
    assert by_name["opt_i32"].presence == EXPLICIT
    assert by_name["opt_str"].presence == EXPLICIT
    # A plain proto3 scalar has implicit presence; a message has its own.
    assert by_name["i32"].presence == IMPLICIT
    assert by_name["inner"].presence == MESSAGE_PRESENCE

    # Repeated fields: `nums` is packed int32, `flags` packed bool, `tags`
    # repeated string (never packed), `inners` repeated message.
    assert by_name["nums"].repeated and by_name["nums"].packed
    assert by_name["nums"].wt == 2 and by_name["nums"].elem_wt == 0
    assert by_name["flags"].repeated and by_name["flags"].packed
    assert by_name["tags"].repeated and not by_name["tags"].packed
    assert by_name["inners"].repeated and not by_name["inners"].packed
    assert by_name["inners"].msg_type.full_name == "structtest.Inner3"

    # The descriptor knows the enum's open/closed dialect (proto3 = open).
    assert not by_name["color"].enum_closed
    # A proto3 file has no required fields anywhere.
    assert not def_.has_required and not def_.deep_required
    # The packed tag bytes are precomputed: field 17, wire type 2.
    assert bytes(by_name["nums"].tag) == b"\x8a\x01"
    assert by_name["inner"].tag_end is None


DATA = os.path.join(HERE, "..", "..", "..", "..", "bindings", "c", "tests", "data")


def _type(desc, name):
    return Schema.from_file(os.path.join(DATA, desc)).message(name)


def test_proto2():
    """A proto2 file: explicit presence everywhere, closed enums, required."""
    matrix2 = _type("matrix2.desc", "structtest.Scalars2").def_
    by_name = {f.name: f for f in matrix2.fields}
    assert by_name["i32"].presence == EXPLICIT  # proto2 has has-bits
    assert by_name["shade"].enum_closed  # proto2 enum is closed
    assert matrix2.deep_required  # Inner carries a required field
    inner = by_name["inner"]
    assert inner.msg_type.deep_required
    assert any(f.presence == REQUIRED for f in inner.msg_type.fields)
    # An explicit `packed = true` option survives; the default does not pack.
    assert by_name["packed_nums"].packed
    assert not by_name["nums"].packed


def test_groups():
    group = _type("grouptest.desc", "structtest.GroupTest").def_
    by_name = {f.name: f for f in group.fields}
    inner = by_name["inner"]
    assert inner.kind == K.GROUP and inner.wt == 3
    assert bytes(inner.tag) == b"\x0b" and bytes(inner.tag_end) == b"\x0c"
    assert inner.msg_type.full_name == "structtest.GroupTest.Inner"
    assert by_name["item"].repeated and by_name["item"].kind == K.GROUP


def test_oneof_and_map():
    oneof = _type("oneoftest.desc", "structtest.OneOfTest").def_
    assert oneof.oneof_count == 1
    assert oneof.oneof_by_name == {"kind": 0}
    for f in oneof.fields:
        if f.name == "id":
            assert f.oneof == -1 and f.presence == IMPLICIT
        else:
            assert f.oneof == 0 and f.presence == EXPLICIT
    maps = _type("maptest.desc", "structtest.Maps").def_
    entry = {f.name: f for f in maps.fields}["by_msg"].msg_type
    assert entry.map_entry and entry.full_name == "structtest.Maps.ByMsgEntry"
    assert [f.number for f in entry.fields] == [1, 2]


def test_extensions():
    has_ext = _type("extensiontest.desc", "exttest.HasExt").def_
    by_name = {f.name: f for f in has_ext.fields}
    # Extensions ride the extendee's slot table by field number.
    assert by_name["ext_int"].number == 100
    assert by_name["ext_int"].full_name == "exttest.ext_int"
    assert by_name["ext_inner"].msg_type.full_name == "exttest.Inner"
    assert by_name["ext_nums"].repeated and not by_name["ext_nums"].packed


def test_bad_descriptors():
    """A truncated or hostile descriptor set is rejected, not half-loaded (an
    empty set is valid: it declares no files)."""
    with open(os.path.join(DATA, "matrix3.desc"), "rb") as handle:
        good = handle.read()
    assert Schema.from_bytes(b"").core.messages == {}
    for bad in (good[:len(good) // 2], b"\xff" * 16, b"\x0a" * 400):
        try:
            Schema.from_bytes(bad)
        except Exception:  # noqa: BLE001 - any refusal is fine, a crash is not
            pass
        else:
            raise AssertionError("accepted a broken descriptor set")


TESTS = [test_matrix3, test_proto2, test_groups, test_oneof_and_map,
         test_extensions, test_bad_descriptors]


def main():
    failed = 0
    for test in TESTS:
        try:
            test()
        except Exception as error:  # noqa: BLE001 - a gate reports, never crashes
            failed += 1
            print("FAIL %s: %s: %s" % (test.__name__, type(error).__name__, error))
        else:
            print("ok   %s" % test.__name__)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())

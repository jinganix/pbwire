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


def _varint(value):
    out = bytearray()
    while value >= 0x80:
        out.append((value & 0x7f) | 0x80)
        value >>= 7
    out.append(value)
    return bytes(out)


def _ld(field, payload):
    return _varint((field << 3) | 2) + _varint(len(payload)) + payload


def _required_desc(count):
    """A FileDescriptorSet with one proto2 message of `count` required int32
    fields, built here so the fixture needs no protoc and no binary file."""
    message = _ld(1, b"Many")
    for i in range(1, count + 1):
        field = (_ld(1, b"r%d" % i) + _varint((3 << 3) | 0) + _varint(i)
                 + _varint((4 << 3) | 0) + _varint(2)  # LABEL_REQUIRED
                 + _varint((5 << 3) | 0) + _varint(5))  # TYPE_INT32
        message += _ld(2, field)
    file_desc = (_ld(1, b"many.proto") + _ld(2, b"p") + _ld(4, message)
                 + _ld(12, b"proto2"))
    return _ld(1, file_desc)


def _required_wire(first, last):
    return b"".join(_varint((i << 3) | 0) + _varint(i) for i in range(first, last + 1))


def _field(name, number, label, kind, type_name=None):
    """One FieldDescriptorProto, so a fixture needs no protoc."""
    out = (_ld(1, name.encode()) + _varint((3 << 3) | 0) + _varint(number)
           + _varint((4 << 3) | 0) + _varint(label) + _varint((5 << 3) | 0)
           + _varint(kind))
    if type_name is not None:
        out += _ld(6, type_name.encode())
    return out


def _msg(name, fields):
    return _ld(1, name.encode()) + b"".join(_ld(2, f) for f in fields)


def _method(name, input_type, output_type, client=False, server=False):
    out = (_ld(1, name.encode()) + _ld(2, input_type.encode())
           + _ld(3, output_type.encode()))
    if client:
        out += _varint((5 << 3) | 0) + _varint(1)
    if server:
        out += _varint((6 << 3) | 0) + _varint(1)
    return out


def _wkt_desc():
    """Any/Timestamp/FieldMask mirrors: same field numbers as the real
    well-known types, so the core helpers accept them."""
    any_msg = _msg("Any", [_field("type_url", 1, 1, 9),
                              _field("value", 2, 1, 12)])
    ts_msg = _msg("Timestamp", [_field("seconds", 1, 1, 3),
                                   _field("nanos", 2, 1, 5)])
    fm_msg = _msg("FieldMask", [_field("paths", 1, 3, 9)])
    file_desc = (_ld(1, b"wkt.proto") + _ld(2, b"google.protobuf")
                 + _ld(4, any_msg) + _ld(4, ts_msg) + _ld(4, fm_msg)
                 + _ld(12, b"proto3"))
    return _ld(1, file_desc)


def _svc_desc():
    job = _msg("Job", [_field("id", 1, 1, 9)])
    svc = (_ld(1, b"Jobs")
           + _ld(2, _method("Get", ".pbwire.svc.Job", ".pbwire.svc.Job"))
           + _ld(2, _method("Chat", ".pbwire.svc.Job", ".pbwire.svc.Job",
                             True, True)))
    file_desc = (_ld(1, b"svc.proto") + _ld(2, b"pbwire.svc")
                 + _ld(4, job) + _ld(6, svc) + _ld(12, b"proto2"))
    return _ld(1, file_desc)


def _node_desc():
    """A recursive message for the max_depth bound: two nesting levels."""
    node = _msg("Node", [_field("child", 1, 1, 11, ".t.Node"),
                           _field("v", 2, 1, 5)])
    file_desc = (_ld(1, b"n.proto") + _ld(2, b"t") + _ld(4, node)
                 + _ld(12, b"proto3"))
    return _ld(1, file_desc)


def required_many():
    """Required tracking uses a 64-slot mask; exactly 64 must not shift by 64
    (undefined) and more than 64 must still be enforced, not silently
    dropped."""
    import tempfile

    for count in (64, 65):
        with tempfile.NamedTemporaryFile(suffix=".desc", delete=False) as out:
            out.write(_required_desc(count))
            path = out.name
        try:
            with pbw.Schema.from_file(path) as schema:
                many = schema.message("p.Many")
                check_raises(ValueError, lambda: many.decode(b""),
                             "%d required fields: empty wire fails" % count)
                # The last one missing: for 64 it is the highest mask slot,
                # for 65 it is the field the mask cannot hold.
                check_raises(ValueError,
                             lambda: many.decode(_required_wire(1, count - 1)),
                             "%d required fields: last field enforced" % count)
                full = many.decode(_required_wire(1, count))
                check(len(full.to_dict()) == count,
                      "%d required fields decode fully" % count)
        finally:
            os.unlink(path)


def json_text():
    with pbw.Schema.from_file(os.path.join(DATA, "matrix3.desc")) as schema:
        scalars = schema.message("structtest.Scalars3")
        with open(os.path.join(DATA, "matrix3.bin"), "rb") as handle:
            reference = handle.read()

        text = scalars.to_json(SCALARS3)
        check('"i64":"9223372036854775807"' in text, "json quotes int64")
        check('"color":"COLOR3_GREEN"' in text, "json names the enum")
        check(scalars.from_json(text).to_dict() == SCALARS3,
              "json round trip")
        check(scalars.decode(reference).to_json() == text,
              "Message.to_json matches")
        check_raises(ValueError, lambda: scalars.from_json('{"nope":1}'),
                     "json unknown field fails")
        check(scalars.from_json('{"nope":1,"i32":3}', True).to_dict() ==
              {"i32": 3}, "json ignore_unknown skips")
        check_raises(ValueError, lambda: scalars.from_json('{"i32":}'),
                     "json malformed fails")

        flat = scalars.to_text({"i32": 1})
        check(flat.strip() == "i32: 1", "text of one field")
        message = scalars.decode(reference)
        check(scalars.from_text(message.to_text()).to_dict() == SCALARS3,
              "text round trip")
        check(scalars.to_text(SCALARS3) == message.to_text(),
              "Message.to_text matches")
        unknown = reference + b"\xb8\x3e\x2a"  # field 999, varint 42
        kept = scalars.decode(unknown)
        check("999" in kept.to_text(), "text keeps unknown fields")
        check("999" not in kept.to_text(False),
              "text drops unknown fields on request")
        check_raises(ValueError, lambda: scalars.from_text("i32: x"),
                     "text malformed fails")


def deterministic():
    with pbw.Schema.from_file(os.path.join(DATA, "maptest.desc")) as schema:
        maps = schema.message("structtest.Maps")
        shuffled = {"by_int": [{"key": 2, "value": "b"},
                                 {"key": 1, "value": "a"}]}
        ordered = {"by_int": [{"key": 1, "value": "a"},
                                {"key": 2, "value": "b"}]}
        check(maps.encode_deterministic(shuffled) == maps.encode(ordered),
              "deterministic encode sorts map entries")
        check(maps.encode(shuffled) != maps.encode_deterministic(shuffled),
              "plain encode keeps insertion order")
        back = maps.decode(maps.encode_deterministic(shuffled))
        check(back.to_dict() == ordered, "deterministic bytes decode")
        check(maps.encode_deterministic({}) == b"",
              "deterministic empty is empty")


def delimited():
    with pbw.Schema.from_file(os.path.join(DATA, "matrix3.desc")) as schema:
        scalars = schema.message("structtest.Scalars3")
        frame = scalars.encode_delimited(SCALARS3)
        wire = scalars.encode(SCALARS3)
        check(frame == _varint(len(wire)) + wire,
              "delimited frame is a length prefix plus bytes")
        message, consumed = scalars.decode_delimited(frame + b"\x00")
        check(message.to_dict() == SCALARS3 and consumed == len(frame),
              "decode_delimited returns the message and its length")
        check_raises(ValueError,
                     lambda: scalars.decode_delimited(frame[:-1]),
                     "truncated delimited frame fails")
        check_raises(ValueError, lambda: scalars.decode_delimited(b""),
                     "empty delimited input fails")


def sized():
    with pbw.Schema.from_file(os.path.join(DATA, "matrix3.desc")) as schema:
        scalars = schema.message("structtest.Scalars3")
        wire = scalars.encode(SCALARS3)
        check(scalars.encoded_size(SCALARS3) == len(wire),
              "encoded_size matches encode")
        check(scalars.encoded_size(scalars.decode(wire)) == len(wire),
              "encoded_size of a Message matches")
        buf = bytearray(len(wire))
        check(scalars.encode_to(SCALARS3, buf) == len(wire) and
              bytes(buf) == wire, "encode_to fills the buffer")
        check(scalars.encode_to(scalars.decode(wire), buf) == len(wire),
              "encode_to of a Message matches")
        check_raises(ValueError,
                     lambda: scalars.encode_to(SCALARS3, bytearray(2)),
                     "encode_to into a small buffer fails")
        parts = []
        check(scalars.encode_sink(SCALARS3, parts.append, 7) == len(wire)
              and b"".join(parts) == wire,
              "encode_sink streams chunks in order")
        check_raises(TypeError,
                     lambda: scalars.encode_sink(SCALARS3, None),
                     "encode_sink needs a callable")


def visit():
    with pbw.Schema.from_file(os.path.join(DATA, "matrix3.desc")) as schema:
        scalars = schema.message("structtest.Scalars3")
        with open(os.path.join(DATA, "matrix3.bin"), "rb") as handle:
            reference = handle.read()
        seen = []
        check(scalars.visit(reference, lambda n, f, w, v:
                            seen.append((n, f, w, v))) is None,
              "visit returns None")
        by_number = dict((f, (n, w, v)) for n, f, w, v in seen)
        check(by_number[1][0] == "i32" and by_number[1][1] == 0,
              "visit names field 1 with its wire type")
        check(isinstance(by_number[1][2], int),
              "visit varint comes back as int")
        unk = reference + b"\xb8\x3e\x2a"  # field 999, varint 42
        seen.clear()
        scalars.visit(unk, lambda n, f, w, v: seen.append((n, f, w, v)))
        check(seen[-1] == (None, 999, 0, 42),
              "visit reports unknown fields with no name")

        def abort(name, number, wire_type, value):
            raise RuntimeError("stop")

        check_raises(RuntimeError, lambda: scalars.visit(reference, abort),
                     "visit propagates a callback exception")
        check_raises(ValueError,
                     lambda: scalars.visit(b"\x08",
                                           lambda n, f, w, v: None),
                     "visit of truncated wire fails")
        check_raises(TypeError,
                     lambda: scalars.visit(reference, None),
                     "visit needs a callable")
        check_raises(ValueError,
                     lambda: scalars.visit(reference,
                                            lambda n, f, w, v: None,
                                            {"max_bytes": 5}),
                     "visit honors max_bytes")


def limits():
    with pbw.Schema.from_file(os.path.join(DATA, "matrix3.desc")) as schema:
        scalars = schema.message("structtest.Scalars3")
        with open(os.path.join(DATA, "matrix3.bin"), "rb") as handle:
            reference = handle.read()
        check_raises(ValueError,
                     lambda: scalars.decode(reference, {"max_bytes": 5}),
                     "decode honors max_bytes")
        check(scalars.decode(reference, {"max_bytes": len(reference),
                                         "max_depth": 256}).to_dict() ==
              SCALARS3, "limits at the boundary still decode")
        check_raises(TypeError, lambda: scalars.decode(reference, 7),
                     "limits must be a dict")
        check_raises(ValueError,
                     lambda: pbw.Schema.load(reference, {"max_bytes": 5}),
                     "descriptor load honors max_bytes")
    with pbw.Schema.load(_node_desc()) as schema:
        node = schema.message("t.Node")
        # outer.child.child.v: two nesting levels.
        inner = b"\x10\x01"  # v = 1
        wire = b"\x0a" + bytes([len(b"\x0a" + bytes([len(inner)]) +
                                          inner)]) + b"\x0a" + bytes(
            [len(inner)]) + inner
        check(node.decode(wire).to_dict() ==
              {"child": {"child": {"v": 1}}}, "recursive message decodes")
        check_raises(ValueError,
                     lambda: node.decode(wire, {"max_depth": 1}),
                     "decode honors max_depth")
        check(node.decode(wire, {"max_depth": 2}).to_dict() ==
              {"child": {"child": {"v": 1}}},
              "max_depth at the boundary still decodes")


def wkt():
    with pbw.Schema.load(_wkt_desc()) as schema:
        ts = schema.message("google.protobuf.Timestamp")
        check(ts.from_millis(1500).to_millis() == 1500,
              "Timestamp millis round trip")
        check(ts.from_millis(1500).to_dict() ==
              {"seconds": 1, "nanos": 500000000},
              "Timestamp millis split")
        check(ts.from_millis(-1500).to_millis() == -1500,
              "negative Timestamp millis round trip")
        payload = ts.encode({"seconds": 1, "nanos": 500000000})
        any_t = schema.message("google.protobuf.Any")
        packed = any_t.any_pack(
            "type.googleapis.com/google.protobuf.Timestamp", payload)
        check(packed.any_type_url() ==
              "type.googleapis.com/google.protobuf.Timestamp",
              "Any type_url")
        check(packed.any_payload() == payload, "Any payload")
        check(packed.to_json() == '{"@type":"type.googleapis.com/'
              'google.protobuf.Timestamp",'
              '"value":"1970-01-01T00:00:01.500Z"}',
              "Any JSON form")
        empty = any_t.decode(b"")
        check(empty.any_type_url() is None and empty.any_payload() is None,
              "absent Any fields read as None")
        fm = schema.message("google.protobuf.FieldMask")
        mask = fm.decode(fm.encode({"paths": ["a.b", "c"]}))
        check(mask.fieldmask_paths() == ["a.b", "c"], "FieldMask paths")
        check(fm.from_json('{"paths":["x"]}').to_dict() ==
              {"paths": ["x"]}, "FieldMask from JSON")
        check_raises(TypeError, lambda: fm.decode(b"").to_millis(),
                     "to_millis rejects other messages")


def service():
    with pbw.Schema.load(_svc_desc()) as schema:
        check(schema.services() == ["pbwire.svc.Jobs"], "services list")
        check(schema.service_methods("pbwire.svc.Jobs") == ["Get", "Chat"],
              "service methods list")
        resolved = schema.resolve("pbwire.svc.Jobs", "Chat")
        check(resolved["input"].name == "pbwire.svc.Job" and
              resolved["output"].name == "pbwire.svc.Job" and
              resolved["input_type"] == "pbwire.svc.Job" and
              resolved["output_type"] == "pbwire.svc.Job" and
              resolved["client_streaming"] is True and
              resolved["server_streaming"] is True,
              "resolve maps a streaming method to its types")
        unary = schema.resolve("pbwire.svc.Jobs", "Get")
        check(unary["client_streaming"] is False and
              unary["server_streaming"] is False,
              "resolve marks unary methods")
        job = resolved["input"]
        check(job.decode(job.encode({"id": "7"})).to_dict() ==
              {"id": "7"}, "resolved type encodes")
        check_raises(KeyError,
                     lambda: schema.service_methods("pbwire.svc.Nope"),
                     "unknown service is a KeyError")
        check_raises(KeyError,
                     lambda: schema.resolve("pbwire.svc.Jobs", "Nope"),
                     "unknown method is a KeyError")


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
        # An empty packed run carries no element: the lazy list must be empty
        # (no NULL slot behind PyList_New), and the eager view agrees.
        empty_run = scalars.decode(b"\x8a\x01\x00\xb2\x01\x00")
        check(empty_run.nums is None, "empty packed run leaves the field absent")
        check(empty_run.flags is None, "empty packed bool run leaves it absent")
        check(empty_run.to_dict() == {}, "empty packed run stays absent")
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

    # Groups map like messages: the same dicts encode through START_GROUP
    # framing and must equal the protoc reference bytes.
    with pbw.Schema.from_file(os.path.join(DATA, "grouptest.desc")) as schema:
        gtest = schema.message("structtest.GroupTest")
        with open(os.path.join(DATA, "grouptest.bin"), "rb") as handle:
            greference = handle.read()
        gvalues = {"inner": {"x": 5, "s": "g"},
                   "item": [{"v": 1}, {"v": -2}], "id": 9}
        got = gtest.encode(gvalues)
        check(got == greference, "group encode equals protoc reference")
        message = gtest.decode(greference)
        check(message.to_dict() == gvalues, "group decode(to_dict)")
        check(message.inner.x == 5 and message.inner.s == "g",
              "singular group attribute")
        check(message.item[1].v == -2, "repeated group attribute")
        check(gtest.encode(message) == greference, "group Message round trip")
        # A singular group seen twice merges, like a singular message.
        merged = gtest.decode(b"\x0b\x10\x05\x0c\x0b\x1a\x01g\x0c")
        check(merged.inner.to_dict() == {"x": 5, "s": "g"},
              "singular group seen twice merges")
        check(gtest.encode({}) == b"", "empty group message is empty")

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
        # decode() is lazy, but a missing required field still fails it: every
        # other binding and the reference reject at parse time.
        check_raises(ValueError, lambda: person.decode(b"\x08\x01"),
                     "proto2 required field missing on decode")
        check_raises(ValueError, lambda: person.decode(b"\x5a\x00"),
                     "proto2 nested required field missing on decode")
        check_raises(ValueError, lambda: person.decode(b"\x0a\x00"),
                     "required field with the wrong wire type does not count")

    required_many()
    json_text()
    deterministic()
    delimited()
    sized()
    visit()
    limits()
    wkt()
    service()

    print("test_pbwire: %s" % ("all checks passed" if not failures
                               else "%d FAILURES" % failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())

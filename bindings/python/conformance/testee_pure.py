"""Google conformance testee driven by the pure-Python core.

Same wire protocol as the extension testee (4-byte little-endian length prefix,
then a ConformanceRequest / ConformanceResponse) and the same schema-driven
shape: the request names the message type and the descriptor set answers with
the schema. Nothing here touches the CPython extension or `google.protobuf`:
the whole codec is `bindings/python/pure/`.

Binary in -> binary out is what this core implements. JSON and text requests
answer ``skipped`` (the runner does not count a skip as a failure), so the pass
set is the binary suite, not the JSON/text mappings.

    python3 testee_pure.py build/conformance.desc     # from the repo root
"""

import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))

from pure.schema import Schema, get_field, has_field  # noqa: E402

PROTOBUF = 1

DESC = sys.argv[1] if len(sys.argv) > 1 else "build/conformance.desc"
SCHEMA = Schema.from_file(DESC)
REQUEST = SCHEMA.message("conformance.ConformanceRequest")
RESPONSE = SCHEMA.message("conformance.ConformanceResponse")


def read_request(stream):
    header = stream.read(4)
    if len(header) < 4:
        return None
    (size,) = struct.unpack("<I", header)
    body = stream.read(size)
    if len(body) < size:
        return None
    return body


def send(stream, fields):
    try:
        payload = RESPONSE.encode_object(fields)
    except Exception as error:  # noqa: BLE001 - report, never crash
        payload = RESPONSE.encode_object(
            {"runtime_error": "cannot encode response: %s" % error})
    stream.write(struct.pack("<I", len(payload)))
    stream.write(payload)
    stream.flush()


def serve(stream, body):
    try:
        request = REQUEST.decode(body)
    except Exception as error:  # noqa: BLE001
        send(stream, {"runtime_error": "cannot parse ConformanceRequest: %s" % error})
        return
    wanted = get_field(request, "requested_output_format") or 0
    if wanted != PROTOBUF:
        # No JSON or text printer in this core: let the runner skip.
        send(stream, {"skipped": "output format %d is not supported" % wanted})
        return
    if has_field(request, "json_payload") or has_field(request, "text_payload"):
        send(stream, {"skipped": "JSON and text input are not supported by the pure core"})
        return
    if not has_field(request, "protobuf_payload"):
        send(stream, {"skipped": "no binary payload"})
        return
    name = get_field(request, "message_type") or ""
    try:
        message_type = SCHEMA.message(name)
    except Exception:  # noqa: BLE001 - unknown type is a runtime error
        send(stream, {"runtime_error": "unknown message type %r" % name})
        return
    try:
        message = message_type.decode(get_field(request, "protobuf_payload"))
    except Exception as error:  # noqa: BLE001
        send(stream, {"parse_error": str(error)})
        return
    try:
        payload = message_type.encode(message)
    except Exception as error:  # noqa: BLE001
        send(stream, {"serialize_error": str(error)})
        return
    send(stream, {"protobuf_payload": payload})


def main():
    stdin, stdout = sys.stdin.buffer, sys.stdout.buffer
    while True:
        body = read_request(stdin)
        if body is None:
            return 0
        serve(stdout, body)


if __name__ == "__main__":
    sys.exit(main())

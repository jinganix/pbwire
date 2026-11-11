"""Google conformance testee driven by the pbwire Python binding.

Part of the repo-wide conformance story: `conformance/testee.c` proves the
dynamic core, this script proves the Python binding on the same requests.

It speaks the runner's wire protocol (4-byte little-endian length prefix, then
a ConformanceRequest / ConformanceResponse) and, because the binding is
schema-driven, it needs no per-message code: the request names the message
type, the descriptor set answers with the schema.

Supported: binary, JSON and text in -> binary, JSON or text out. JSPB answers
`skipped`, like every other testee here.

`decode()` walks and validates the whole payload before returning, and a parsed
message keeps the bytes it was parsed from, so `encode()` hands them back: a
round trip preserves unknown fields and repeated fields in wire order, which is
why this testee has no expected failures.

    python testee.py build/conformance.desc     # from the repo root
"""

import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))

import pbwire  # noqa: E402

PROTOBUF = 1
JSON = 2
JSPB = 3
TEXT = 4

JSON_IGNORE_UNKNOWN = 3


def read_request(stream):
    header = stream.read(4)
    if len(header) < 4:
        return None
    (size,) = struct.unpack("<I", header)
    body = stream.read(size)
    if len(body) < size:
        return None
    return body


def write_response(stream, payload):
    stream.write(struct.pack("<I", len(payload)))
    stream.write(payload)
    stream.flush()


def main(argv):
    if len(argv) < 2:
        print("usage: testee.py <conformance.desc>", file=sys.stderr)
        return 2

    schema = pbwire.Schema.from_file(argv[1])
    request_type = schema.message("conformance.ConformanceRequest")
    response_type = schema.message("conformance.ConformanceResponse")
    types = {}

    def message_of(name):
        if name not in types:
            types[name] = schema.message(name)
        return types[name]

    stdin, stdout = sys.stdin.buffer, sys.stdout.buffer
    while True:
        body = read_request(stdin)
        if body is None:
            return 0
        try:
            request = request_type.decode(body)
        except Exception as error:  # noqa: BLE001 - report, never crash
            write_response(stdout, response_type.encode(
                {"runtime_error": "cannot parse ConformanceRequest: %s" % error}))
            continue

        dump = os.environ.get("PBWIRE_CONFORMANCE_DUMP")
        if dump:
            with open(dump, "ab") as log:
                log.write(b"REQ " + body.hex().encode() + b"\n")
        wanted = request.get("requested_output_format", 0)
        name = request.get("message_type", "")
        category = request.get("test_category", 0)
        response = {}
        if wanted not in (PROTOBUF, JSON, TEXT):
            response["skipped"] = "output format %d is not supported" % wanted
        else:
            try:
                message = message_of(name)
            except KeyError:
                response["runtime_error"] = "unknown message type %r" % name
            else:
                try:
                    if "json_payload" in request:
                        parsed = message.from_json(request["json_payload"],
                                                   category == JSON_IGNORE_UNKNOWN)
                    elif "text_payload" in request:
                        parsed = message.from_text(request["text_payload"])
                    elif "protobuf_payload" in request:
                        parsed = message.decode(request["protobuf_payload"])
                    else:
                        response["skipped"] = "unsupported payload"
                except Exception as error:  # noqa: BLE001
                    response["parse_error"] = str(error)
                else:
                    try:
                        if wanted == JSON:
                            response["json_payload"] = parsed.to_json()
                        elif wanted == TEXT:
                            response["text_payload"] = parsed.to_text(
                                request.get("print_unknown_fields", False))
                        else:
                            response["protobuf_payload"] = message.encode(parsed)
                    except Exception as error:  # noqa: BLE001
                        response["serialize_error"] = str(error)
        try:
            payload = response_type.encode(response)
        except Exception as error:  # noqa: BLE001
            payload = response_type.encode(
                {"runtime_error": "cannot encode response: %s" % error})
        write_response(stdout, payload)


if __name__ == "__main__":
    sys.exit(main(sys.argv))

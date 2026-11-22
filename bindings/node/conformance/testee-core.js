'use strict';

/* Google conformance testee driven by the pure-TypeScript core.
 *
 * Same wire protocol as the addon testee (4-byte little-endian length prefix,
 * then a ConformanceRequest / ConformanceResponse) and the same
 * schema-driven shape: the request names the message type and the descriptor
 * set answers with the schema. Nothing here touches the native library.
 *
 * Binary in -> binary out is what this core implements. JSON and text
 * requests answer `skipped` (the runner does not count a skip as a failure),
 * so the pass set is the binary suite, not the JSON/text mappings.
 *
 *   node testee-core.js build/conformance.desc   # from bindings/node
 */

const { Schema, fromObject, getField, hasField, rawPayload } = require('../lib/core/schema.js');

const schema = Schema.fromFile(process.argv[2]);
const requestType = schema.message('conformance.ConformanceRequest');
const responseType = schema.message('conformance.ConformanceResponse');

const PROTOBUF = 1;

/** The request's payload as bytes, or undefined when the field is absent. */
function payloadOf(request) {
    if (!hasField(request, 'protobuf_payload')) {
        return undefined;
    }
    for (let s = 0; s < request.def.fields.length; s++) {
        if (request.def.fields[s].name === 'protobuf_payload') {
            const v = request.v[s];
            return v === undefined ? undefined : rawPayload(v);
        }
    }
    return undefined;
}

function send(fields) {
    const response = fromObject(responseType.def, fields);
    const bytes = responseType.encode(response);
    const header = Buffer.allocUnsafe(4);
    header.writeUInt32LE(bytes.length, 0);
    process.stdout.write(Buffer.concat([header, Buffer.from(bytes)]));
}

function serve(body) {
    let request;
    try {
        request = requestType.decode(body);
    } catch (err) {
        send({ runtime_error: 'cannot parse ConformanceRequest: ' + String((err && err.message) || err) });
        return;
    }
    const wanted = getField(request, 'requested_output_format');
    const messageType = getField(request, 'message_type');
    if (wanted !== PROTOBUF) {
        // No JSON or text printer in this core: let the runner skip.
        send({ skipped: 'output format ' + wanted + ' is not supported' });
        return;
    }
    if (hasField(request, 'json_payload') || hasField(request, 'text_payload')) {
        send({ skipped: 'JSON and text input are not supported by the TS core' });
        return;
    }
    const payload = payloadOf(request);
    if (payload === undefined) {
        send({ skipped: 'no binary payload' });
        return;
    }
    let type;
    try {
        type = schema.message(String(messageType));
    } catch (err) {
        send({ runtime_error: "unknown message type '" + messageType + "'" });
        return;
    }
    let msg;
    try {
        // Byte offsets are preserved: the payload is a view into the
        // request's buffer, so decode from its slice.
        const view = Buffer.from(payload.buffer, payload.byteOffset, payload.length);
        msg = type.decode(new Uint8Array(view));
    } catch (err) {
        send({ parse_error: String((err && err.message) || err) });
        return;
    }
    try {
        send({ protobuf_payload: type.encode(msg) });
    } catch (err) {
        send({ serialize_error: String((err && err.message) || err) });
    }
}

let pending = Buffer.alloc(0);
process.stdin.on('data', (chunk) => {
    pending = Buffer.concat([pending, chunk]);
    while (pending.length >= 4) {
        const len = pending.readUInt32LE(0);
        if (pending.length < 4 + len) {
            break;
        }
        const body = pending.subarray(4, 4 + len);
        pending = pending.subarray(4 + len);
        try {
            serve(body);
        } catch (err) {
            // Never crash the runner: a misbehaving request is a verdict.
            send({ runtime_error: 'testee failed: ' + String((err && err.message) || err) });
        }
    }
});

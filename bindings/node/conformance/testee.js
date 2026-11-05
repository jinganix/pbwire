'use strict';

/* Google conformance testee driven by the pbwire Node binding.
 *
 * Speaks the runner's wire protocol (4-byte little-endian length prefix,
 * then a ConformanceRequest / ConformanceResponse) and, because the binding
 * is schema-driven, needs no per-message code: the request names the message
 * type and the descriptor set answers with the schema.
 *
 * Supported: binary, JSON and text in -> binary, JSON or text out. Binary
 * input decodes to a tree (unknown fields survive, oneof collapses to the
 * last member) and every output renders that same tree, so the suite sees
 * one canonical behavior. JSON input with unknown fields only parses when
 * the test category asks to ignore them. JSPB answers `skipped`.
 *
 *   node testee.js build/conformance.desc   # from bindings/node
 */

const fs = require('node:fs');
const path = require('node:path');

const { Schema } = require('../lib/pbwire');

const schema = Schema.fromFile(process.argv[2]);
const requestType = schema.message('conformance.ConformanceRequest');
const responseType = schema.message('conformance.ConformanceResponse');

const PROTOBUF = 1;
const JSON = 2;
const TEXT = 4;
// The runner sends 3 (JSON_IGNORE_UNKNOWN_PARSE_TEST) when JSON input with
// unknown fields must parse; anything else rejects them.
const JSON_IGNORE_UNKNOWN = 3;

function answer(fields) {
  const response = responseType.newMessage();
  for (const [name, value] of Object.entries(fields)) {
    response.set(name, value);
  }
  const bytes = response.encode();
  response.free();
  const header = Buffer.allocUnsafe(4);
  header.writeUInt32LE(bytes.length, 0);
  process.stdout.write(Buffer.concat([header, bytes]));
}

function runtimeError(messageType, err) {
  const text = String((err && err.message) || err);
  if (text.includes('is not a message')) {
    answer({ runtime_error: `unknown message type ${messageType}` });
  } else {
    answer({ parse_error: text });
  }
}

function serve(body) {
  let request;
  try {
    request = requestType.decode(body);
  } catch (err) {
    answer({ runtime_error: 'cannot decode request' });
    return;
  }
  const wanted = request.get('requested_output_format');
  const messageType = request.get('message_type');
  let message;
  try {
    message = schema.message(messageType);
  } catch (err) {
    answer({ runtime_error: `unknown message type ${messageType}` });
    return;
  }
  if (wanted !== PROTOBUF && wanted !== JSON && wanted !== TEXT) {
    answer({ skipped: `output format ${wanted} is not supported` });
    return;
  }
  // One input payload wins: binary beats JSON beats text.
  let tree = null;
  try {
    const binary = request.get('protobuf_payload');
    const json = request.get('json_payload');
    const text = request.get('text_payload');
    if (binary !== undefined) {
      tree = message.decode(binary);
      tree._tree(); // trees collapse oneof; wire echoes would not
    } else if (json !== undefined) {
      tree = message.fromJson(json, request.get('test_category') === JSON_IGNORE_UNKNOWN);
    } else if (text !== undefined) {
      tree = message.fromText(text);
    } else {
      throw new Error('pbwire: no input payload in the request');
    }
  } catch (err) {
    runtimeError(messageType, err);
    return;
  }
  try {
    if (wanted === PROTOBUF) {
      answer({ protobuf_payload: tree.encode() });
    } else if (wanted === JSON) {
      answer({ json_payload: tree.toJson() });
    } else {
      // The runner's print_unknown_fields flag decides whether unknown
      // fields print; absent means drop them.
      answer({ text_payload: tree.toText(request.get('print_unknown_fields') === true) });
    }
  } catch (err) {
    answer({ serialize_error: String((err && err.message) || err) });
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
    serve(body);
  }
});

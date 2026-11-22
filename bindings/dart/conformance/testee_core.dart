// Google conformance testee driven by the pure-Dart core.
//
// Same wire protocol as the FFI testee (4-byte little-endian length prefix,
// then a ConformanceRequest / ConformanceResponse) and the same schema-driven
// shape: the request names the message type and the descriptor set answers
// with the schema. Nothing here touches `dart:ffi` or `package:pbwire/pbwire.dart`:
// the whole codec is `package:pbwire/core/`.
//
// Binary in -> binary out is what this core implements. JSON and text requests
// answer `skipped` (the runner does not count a skip as a failure), so the
// pass set is the binary suite, not the JSON/text mappings.
//
//   dart run conformance/testee_core.dart build/conformance.desc

import 'dart:async';
import 'dart:io';
import 'dart:typed_data';

import 'package:pbwire/core/schema.dart';

const int _protobuf = 1;

/// Reads the runner's length-prefixed frames off stdin, across chunk
/// boundaries, without assuming one read per message.
final class _Frames {
  _Frames(Stream<List<int>> input) : _chunks = StreamIterator(input);

  final StreamIterator<List<int>> _chunks;
  List<int> _chunk = const <int>[];
  int _at = 0;

  Future<bool> _fill() async {
    if (_at < _chunk.length) {
      return true;
    }
    if (!await _chunks.moveNext()) {
      return false;
    }
    _chunk = _chunks.current;
    _at = 0;
    return _fill();
  }

  Future<bool> _take(int count, Uint8List into, int offset) async {
    var done = 0;
    while (done < count) {
      if (!await _fill()) {
        return false;
      }
      final left = _chunk.length - _at;
      final n = count - done < left ? count - done : left;
      into.setRange(offset + done, offset + done + n, _chunk, _at);
      _at += n;
      done += n;
    }
    return true;
  }

  /// The next frame, or null once stdin ends.
  Future<Uint8List?> next() async {
    final header = Uint8List(4);
    if (!await _take(4, header, 0)) {
      return null;
    }
    final size =
        header[0] | (header[1] << 8) | (header[2] << 16) | (header[3] << 24);
    final body = Uint8List(size);
    if (size > 0 && !await _take(size, body, 0)) {
      return null;
    }
    return body;
  }
}

Uint8List _frame(Uint8List payload) {
  final out = Uint8List(4 + payload.length)
    ..[0] = payload.length & 0xff
    ..[1] = (payload.length >> 8) & 0xff
    ..[2] = (payload.length >> 16) & 0xff
    ..[3] = (payload.length >> 24) & 0xff;
  out.setRange(4, 4 + payload.length, payload);
  return out;
}

Future<void> main(List<String> args) async {
  final desc = args.isNotEmpty ? args[0] : 'build/conformance.desc';
  final schema = Schema.fromBytes(File(desc).readAsBytesSync());
  final request = schema.message('conformance.ConformanceRequest');
  final response = schema.message('conformance.ConformanceResponse');
  final types = <String, MessageType>{};
  final frames = _Frames(stdin);

  while (true) {
    final body = await frames.next();
    if (body == null) {
      break;
    }
    Uint8List payload;
    try {
      payload = _answer(schema, request, response, types, body);
    } on CoreError catch (error) {
      payload = response.encodeObject(
          <String, Object?>{'runtime_error': 'testee failed: ${error.message}'});
    }
    stdout.add(_frame(payload));
    await stdout.flush();
  }
}

Uint8List _answer(Schema schema, MessageType request, MessageType response,
    Map<String, MessageType> types, Uint8List body) {
  final Msg decoded;
  try {
    decoded = request.decode(body);
  } on CoreError catch (error) {
    return response.encodeObject(<String, Object?>{
      'runtime_error': 'cannot parse ConformanceRequest: ${error.message}'
    });
  }

  final wanted = getField(decoded, 'requested_output_format') ?? 0;
  if (wanted != _protobuf) {
    // No JSON or text printer in this core: let the runner skip.
    return response.encodeObject(<String, Object?>{
      'skipped': 'output format $wanted is not supported'
    });
  }
  if (getField(decoded, 'json_payload') != null ||
      getField(decoded, 'text_payload') != null) {
    return response.encodeObject(<String, Object?>{
      'skipped': 'JSON and text input are not supported by the pure core'
    });
  }
  final wire = getField(decoded, 'protobuf_payload') as Uint8List?;
  if (wire == null) {
    return response.encodeObject(<String, Object?>{'skipped': 'no binary payload'});
  }
  final name = getField(decoded, 'message_type') as String? ?? '';
  MessageType? type;
  try {
    type = types[name] ??= schema.message(name);
  } on CoreError {
    return response.encodeObject(
        <String, Object?>{'runtime_error': 'unknown message type $name'});
  }

  final Msg parsed;
  try {
    parsed = type.decode(wire);
  } on CoreError catch (error) {
    return response.encodeObject(
        <String, Object?>{'parse_error': error.message});
  }
  try {
    return response.encodeObject(
        <String, Object?>{'protobuf_payload': type.encode(parsed)});
  } on CoreError catch (error) {
    return response.encodeObject(
        <String, Object?>{'serialize_error': error.message});
  }
}

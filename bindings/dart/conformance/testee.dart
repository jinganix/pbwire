// Google conformance testee driven by the pbwire Dart binding.
//
// Part of the repo-wide conformance story: `conformance/testee.c` proves the
// dynamic core, this script proves the Dart binding on the same requests. It
// speaks the runner's wire protocol (4-byte little-endian length prefix, then a
// ConformanceRequest / ConformanceResponse) and, because the binding is
// schema-driven, it needs no per-message code: the request names the message
// type and the descriptor set answers with the schema.
//
// Binary, JSON and text in -> binary, JSON or text out; JSPB answers `skipped`,
// like every other testee here.
//
//   dart run bindings/dart/conformance/testee.dart build/conformance.desc

import 'dart:async';
import 'dart:io';
import 'dart:typed_data';

import 'package:pbwire/pbwire.dart';

const _protobuf = 1;
const _json = 2;
const _text = 4;

const _jsonIgnoreUnknown = 3;

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
  out.setAll(4, payload);
  return out;
}

Future<void> main(List<String> args) async {
  final desc = args.isNotEmpty ? args[0] : 'build/conformance.desc';
  final schema = Schema.load(desc);
  final requestType = schema.message('conformance.ConformanceRequest');
  final responseType = schema.message('conformance.ConformanceResponse');
  final types = <String, Type>{};
  final frames = _Frames(stdin);

  while (true) {
    final body = await frames.next();
    if (body == null) {
      break;
    }
    Uint8List payload;
    Message? request;
    try {
      request = requestType.decode(body);
      payload = _answer(schema, request, responseType, types);
    } catch (error) {
      payload = _error(responseType, 'cannot parse ConformanceRequest: $error');
    } finally {
      request?.close();
    }
    stdout.add(_frame(payload));
    await stdout.flush();
  }
  schema.close();
}

Uint8List _error(Type responseType, String what) {
  final response = responseType.newMessage();
  try {
    response.setString('runtime_error', what);
    return response.encode();
  } finally {
    response.close();
  }
}

Uint8List _answer(
    Schema schema, Message request, Type responseType, Map<String, Type> types) {
  final wanted = request.getInt('requested_output_format');
  final name = request.getString('message_type') ?? '';
  final category = request.getInt('test_category');
  final response = responseType.newMessage();
  try {
    if (wanted != _protobuf && wanted != _json && wanted != _text) {
      response.setString('skipped', 'output format $wanted is not supported');
      return response.encode();
    }
    final type = types[name] ??= schema.message(name);
    Message? parsed;
    try {
      if (request.has('json_payload')) {
        parsed = type.parseJson(request.getString('json_payload') ?? '',
            ignoreUnknown: category == _jsonIgnoreUnknown);
      } else if (request.has('text_payload')) {
        parsed = type.parseText(request.getString('text_payload') ?? '');
      } else if (request.has('protobuf_payload')) {
        parsed = type.decode(request.getBytes('protobuf_payload') ?? Uint8List(0));
      } else {
        response.setString('skipped', 'unsupported payload');
        return response.encode();
      }
    } catch (error) {
      response.setString('parse_error', '$error');
      return response.encode();
    }
    try {
      if (wanted == _json) {
        response.setString('json_payload', parsed.printJson());
      } else if (wanted == _text) {
        response.setString(
            'text_payload',
            parsed.printText(request.getBool('print_unknown_fields')));
      } else {
        response.setBytes('protobuf_payload', parsed.encode());
      }
    } catch (error) {
      response.setString('serialize_error', '$error');
    }
    return response.encode();
  } finally {
    response.close();
  }
}

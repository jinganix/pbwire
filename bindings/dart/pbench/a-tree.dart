// The `tree` arm: the prebuilt `Message` handle. The whole user task is timed
// -- one `set` per field while building, `encode()`; decode, then one `get` per
// field while reading -- so it is not the prebuilt-handle op `make dart-bench`
// prints as `pbwire-dart-tree`.

import 'dart:typed_data';

import 'package:pbwire/pbwire.dart';

import 'payload.dart';

int _failures = 0;

Uint8List encode(Pbench p, int repeats) {
  final msg = p.handle(repeats);
  try {
    return msg.encode();
  } finally {
    msg.close();
  }
}

/// Decode and read every field back, like the other arms' field compare.
bool read(Message msg, int repeats) {
  final ok = msg.getInt('i32') == -2147483648 &&
      msg.getInt('i64') == 9223372036854775807 &&
      msg.getInt('u32') == 4294967295 &&
      msg.getInt('u64') == -1 &&
      msg.getInt('s32') == -2147483648 &&
      msg.getInt('s64') == -9223372036854775808 &&
      msg.getInt('fx32') == 3735928559 &&
      msg.getInt('fx64') == 72623859790382856 &&
      msg.getInt('sfx32') == -2147483648 &&
      msg.getInt('sfx64') == -9223372036854775808 &&
      msg.getFloat('f32') == double.infinity &&
      msg.getDouble('f64') == -123456.789 &&
      msg.getBool('flag') &&
      msg.getString('str') == 'hello' &&
      msg.getInt('color') == 2 &&
      msg.has('opt_i32') &&
      msg.getInt('opt_i32') == -7 &&
      msg.has('opt_str') &&
      msg.getString('opt_str') == 'opt' &&
      msg.count('nums') == repeats &&
      msg.count('tags') == repeats &&
      msg.count('inners') == repeats &&
      msg.count('flags') == repeats &&
      (repeats == 0 ||
          (msg.at('nums', repeats - 1)?.asInt() == repeats - 4 &&
              msg.at('tags', repeats - 1)?.asString() == 'tag' &&
              msg.at('flags', repeats - 1)?.asBool() == ((repeats - 1) % 2 == 0)));
  final inner = msg.getMessage('inner');
  final kid = inner == null
      ? false
      : inner.getString('name') == 'kid' && inner.getInt('value') == 42;
  inner?.close();
  if (!ok || !kid) {
    _failures++;
    return false;
  }
  return true;
}

Uint8List? decode(Pbench p, Uint8List wire, int repeats) {
  final msg = p.ty.decode(wire);
  try {
    if (!read(msg, repeats)) {
      return null;
    }
    return msg.encode();
  } finally {
    msg.close();
  }
}

int failures() => _failures;

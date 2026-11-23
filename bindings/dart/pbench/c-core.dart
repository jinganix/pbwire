// The `core` arm: the pure-Dart core of `bindings/dart/lib/core/` -- no
// `dart:ffi`, no native library, one `set` per field on a `Msg` tree and then
// `encode()`; `decode()` and then one `get` per field. It is the whole user
// task, exactly like the `tree` arm, so the two rows differ only in which
// codec does the work.

import 'dart:typed_data';

import 'package:pbwire/core/schema.dart';

import 'payload.dart';

Uint8List encode(Pbench p, int repeats) {
  final msg = p.coreMessage(repeats);
  return p.coreType.encode(msg);
}

/// Reads the decoded tree back; the gate uses this, the timer does not (the
/// decode already produced every value).
bool read(Msg msg, int repeats) {
  final inner = getField(msg, 'inner') as Msg?;
  // An absent repeated field is an empty one, exactly like the tree arm's
  // count() == 0 on the x0 tier.
  final nums = (getField(msg, 'nums') as List<Object?>?) ?? const <Object?>[];
  final tags = (getField(msg, 'tags') as List<Object?>?) ?? const <Object?>[];
  final flags = (getField(msg, 'flags') as List<Object?>?) ?? const <Object?>[];
  final inners =
      (getField(msg, 'inners') as List<Object?>?) ?? const <Object?>[];
  final ok = getField(msg, 'i32') == -2147483648 &&
      getField(msg, 'i64') == BigInt.parse('9223372036854775807') &&
      getField(msg, 'u32') == 4294967295 &&
      getField(msg, 'u64') == BigInt.parse('18446744073709551615') &&
      getField(msg, 's32') == -2147483648 &&
      getField(msg, 's64') == BigInt.parse('-9223372036854775808') &&
      getField(msg, 'fx32') == 3735928559 &&
      getField(msg, 'fx64') == BigInt.parse('72623859790382856') &&
      getField(msg, 'sfx32') == -2147483648 &&
      getField(msg, 'sfx64') == BigInt.parse('-9223372036854775808') &&
      getField(msg, 'f32') == double.infinity &&
      getField(msg, 'f64') == -123456.789 &&
      getField(msg, 'flag') == true &&
      getField(msg, 'str') == 'hello' &&
      getField(msg, 'color') == 2 &&
      getField(msg, 'opt_i32') == -7 &&
      getField(msg, 'opt_str') == 'opt' &&
      nums.length == repeats &&
      tags.length == repeats &&
      flags.length == repeats &&
      inners.length == repeats &&
      (repeats == 0 ||
          (nums[repeats - 1] == repeats - 4 &&
              tags[repeats - 1] == 'tag' &&
              flags[repeats - 1] == ((repeats - 1) % 2 == 0) &&
              getField(inners[repeats - 1] as Msg, 'name') == 'grandkid' &&
              getField(inners[repeats - 1] as Msg, 'value') == repeats));
  final kid =
      inner != null && getField(inner, 'name') == 'kid' && getField(inner, 'value') == 42;
  return ok && kid;
}

Uint8List? decode(Pbench p, Uint8List wire, int repeats) {
  final msg = p.coreType.decode(wire);
  if (!read(msg, repeats)) {
    return null;
  }
  return p.coreType.encode(msg);
}

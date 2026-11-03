// A round trip over the pinned matrix3.desc: build, encode, decode, read back.
//
//   dart run examples/roundtrip.dart [descriptor-set]

import 'package:pbwire/pbwire.dart';

void main(List<String> args) {
  final desc = args.isNotEmpty ? args[0] : '../c/tests/data/matrix3.desc';
  final schema = Schema.load(desc);
  final ty = schema.message('structtest.Scalars3');
  final innerTy = schema.message('structtest.Inner3');

  final msg = ty.newMessage()
    ..setInt('i32', -42)
    ..setString('str', 'alice')
    ..setBool('flag', true);
  for (final tag in ['a', 'bb']) {
    msg.setString('tags', tag); // repeated: appends
  }
  msg.setMessage('inner') // nested: created in place
    ..setString(innerTy.field('name'), 'kid')
    ..setInt('value', 42);

  final wire = msg.encode();
  print('${wire.length} bytes: ${wire.map((b) => b.toRadixString(16).padLeft(2, '0')).join()}');
  msg.close();

  final decoded = ty.decode(wire);
  print('i32   = ${decoded.getInt('i32')}');
  print('str   = ${decoded.getString('str')}');
  print('inner = ${decoded.getMessage('inner')?.getString('name')}');
  final tags = <String?>[];
  for (var i = 0; i < decoded.count('tags'); i++) {
    tags.add(decoded.at('tags', i)?.asString());
  }
  print('tags  = $tags');
  if (!_equals(decoded.encode(), wire)) {
    throw StateError('round trip differs');
  }
  decoded.close();
  schema.close();
  print('OK   byte-exact round trip');
}

bool _equals(List<int> a, List<int> b) {
  if (a.length != b.length) {
    return false;
  }
  for (var i = 0; i < a.length; i++) {
    if (a[i] != b[i]) {
      return false;
    }
  }
  return true;
}

// Self-check against the pinned protoc bytes: build structtest.Scalars3 from
// bindings/c/tests/data/matrix3.txt, encode, compare with matrix3.bin (the same
// oracle the C binding uses), then decode it back and check the values.
//
//   dart run tests/selftest.dart [descriptor-set]

import 'dart:io';
import 'dart:typed_data';

import 'package:pbwire/pbwire.dart';

var _checks = 0;

void check(bool ok, String what) {
  _checks++;
  if (!ok) {
    stderr.writeln('FAIL $what');
    exit(1);
  }
}

void main(List<String> args) {
  final desc = args.isNotEmpty ? args[0] : '../c/tests/data/matrix3.desc';
  final oracle = File(File(desc).parent.path + '/matrix3.bin').readAsBytesSync();

  final schema = Schema.load(desc);
  final ty = schema.message('structtest.Scalars3');
  final innerTy = schema.message('structtest.Inner3');
  final name = innerTy.field('name');
  final value = innerTy.field('value');

  final msg = ty.newMessage()
    ..setInt('i32', -2147483648)
    ..setInt('i64', 9223372036854775807)
    ..setInt('u32', 4294967295)
    ..setInt('u64', -1) // u64::MAX bits
    ..setInt('s32', -2147483648)
    ..setInt('s64', -9223372036854775808)
    ..setInt('fx32', 0xdeadbeef)
    ..setInt('fx64', 72623859790382856)
    ..setInt('sfx32', -2147483648)
    ..setInt('sfx64', -9223372036854775808)
    ..setFloat('f32', double.infinity)
    ..setDouble('f64', -123456.789)
    ..setBool('flag', true)
    ..setString('str', 'hello')
    ..setInt('color', 2)
    ..setInt('opt_i32', -7)
    ..setString('opt_str', 'opt');

  for (final n in [1, -5, 300]) {
    msg.setInt('nums', n);
  }
  for (final tag in ['a', 'bb']) {
    msg.setString('tags', tag);
  }
  for (final pair in [['x', 1], ['y', -2]]) {
    msg.setMessage('inners').setString(name, pair[0] as String).setInt(value, pair[1] as int);
  }
  for (final flag in [true, false, true]) {
    msg.setBool('flags', flag);
  }
  msg.setMessage('inner').setString(name, 'kid').setInt(value, 42);

  final wire = msg.encode();
  check(_equals(wire, oracle), 'encoded bytes differ from protoc (${wire.length} vs ${oracle.length})');
  print('OK   encode matches protoc, ${oracle.length} bytes');
  msg.close();

  final decoded = ty.decode(oracle);
  check(decoded.getInt('i32') == -2147483648, 'i32');
  check(decoded.getBool('flag'), 'flag');
  check(decoded.getString('str') == 'hello', 'str');
  check(decoded.getDouble('f64') == -123456.789, 'f64');
  check(decoded.getInt('color') == 2, 'color');
  check(decoded.getInt('opt_i32') == -7, 'opt_i32');
  check(decoded.has('opt_str') && !ty.newMessage().has('i32'), 'presence');
  check(decoded.count('nums') == 3, 'nums count');
  check(decoded.at('nums', 1)?.asInt() == -5, 'nums[1]');
  check(decoded.at('tags', 1)?.asString() == 'bb', 'tags[1]');
  check(decoded.at('inners', 1)?.asMessage()?.getInt(value) == -2, 'inners[1].value');
  check(decoded.at('flags', 1)?.asBool() == false, 'flags[1]');
  check(decoded.getMessage('inner')?.getString(name) == 'kid', 'inner.name');
  check(_equals(decoded.encode(), oracle), 'decode -> encode is byte-exact');
  print('OK   decode round trip, repeated and nested fields');
  decoded.close();

  // A field the schema does not declare (99, varint 7) survives re-encoding.
  final unknown = Uint8List.fromList([0x98, 0x06, 0x07]);
  final innerOnly = innerTy.decode(unknown);
  check(_equals(innerOnly.encode(), unknown), 'unknown field preserved');
  innerOnly.close();

  // Bad input is an error, not a crash.
  check(_throws(() => ty.decode([0x80])), 'truncated tag accepted');
  check(_throws(() => schema.message('structtest.Nope')), 'unknown message accepted');
  check(_throws(() => ty.field('nope')), 'unknown field accepted');

  // The cases a binding can get wrong even when the core is right: payloads with
  // embedded NULs / invalid UTF-8, non-ASCII text, a negative enum, an empty
  // repeated element, proto3 optional presence with an empty value, and oneof
  // switching. tests/data/edge.bin is protoc --encode.
  final edgeOracle = File('../../tests/data/edge.bin').readAsBytesSync();
  final edgeSchema = Schema.load('../../tests/data/edge.desc');
  final edgeTy = edgeSchema.message('edge.Edge');
  final edgeInner = edgeSchema.message('edge.Inner');

  final edge = edgeTy.newMessage()
    ..setBytes('raw', [0x00, 0x01, 0xff])
    ..setString('text', 'héllo ☃')
    ..setInt('sign', -3)
    ..setString('maybe', '')
    ..setInt('pick_num', 5) // oneof: replaced below
    ..setString('pick_str', 'one');
  for (final tag in ['a', '', 'b']) {
    edge.setString('tags', tag);
  }
  edge.setBytes('blobs', [0x00]);
  edge.setBytes('blobs', []);
  edge.setMessage('inner').setString('tag', 'kid').setInt('n', -1);

  check(_equals(edge.encode(), edgeOracle), 'edge payload differs from protoc');
  edge.close();

  final edgeDecoded = edgeTy.decode(edgeOracle);
  check(_bytes(edgeDecoded.getBytes('raw'), [0x00, 0x01, 0xff]), 'bytes with NUL and 0xff');
  check(edgeDecoded.getString('text') == 'héllo ☃', 'non-ASCII string');
  check(edgeDecoded.getInt('sign') == -3, 'negative enum');
  check(edgeDecoded.count('tags') == 3, 'empty repeated element count');
  check(edgeDecoded.at('tags', 1)?.asString() == '', 'empty repeated element');
  check(_bytes(edgeDecoded.at('blobs', 0)?.asBytes(), [0x00]), 'bytes element with NUL');
  check(_bytes(edgeDecoded.at('blobs', 1)?.asBytes(), []), 'empty bytes element');
  check(edgeDecoded.getMessage('inner')?.getInt('n') == -1, 'nested negative');
  check(edgeDecoded.has('maybe') && edgeDecoded.getString('maybe') == '', 'optional present and empty');
  check(edgeDecoded.has('pick_str') && !edgeDecoded.has('pick_num'), 'oneof keeps only the last member');
  check(_equals(edgeDecoded.encode(), edgeOracle), 'edge decode -> encode is byte-exact');
  edgeDecoded.close();
  print('OK   edge payload: bytes, UTF-8, negative enum, empty elements, oneof');

  // A default message is zero bytes both ways.
  check(edgeTy.newMessage().encode().isEmpty, 'empty message encodes to zero bytes');
  final emptyDecoded = edgeTy.decode([]);
  check(emptyDecoded.encode().isEmpty, 'empty wire decodes and re-encodes to zero bytes');
  check(!emptyDecoded.has('text'), 'empty message has no fields');
  emptyDecoded.close();

  // A wrong value kind is refused, not written.
  check(
    _rejects(() => edgeTy.newMessage().setString('pick_num', 'not a number'), 'type mismatch'),
    'wrong-typed value rejected',
  );
  check(
    _rejects(() => edgeTy.newMessage().setMessage('text'), 'not a message field'),
    'setMessage on a scalar rejected',
  );

  edgeSchema.close();
  schema.close();
  print('OK   $_checks checks passed');
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

bool _rejects(void Function() op, String expected) {
  try {
    op();
    return false;
  } on PbwireException catch (e) {
    return e.message.contains(expected);
  }
}

bool _bytes(List<int>? got, List<int> want) =>
    got != null && got.length == want.length && _equals(got, want);

bool _throws(void Function() op) {
  try {
    op();
    return false;
  } on PbwireException {
    return true;
  }
}

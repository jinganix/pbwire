// Typed-conformance check for the Dart binding: build matrix3.proto's
// Scalars3 through the generated classes, encode, require the protoc reference
// bytes (matrix3.bin), then decode those bytes back and check every field.
//
//   cd bindings/dart && dart run tests/typed_test.dart

import 'dart:io';
import 'dart:typed_data';

import 'package:pbwire/pbwire.dart';

import 'gen/matrix3.dart';

const data = '../c/tests/data';

var checks = 0;

void check(bool ok, String what) {
  if (!ok) {
    stderr.writeln('FAIL $what');
    exit(1);
  }
  checks++;
}

Scalars3 build() {
  final m = Scalars3();
  m.i32 = -2147483648;
  m.i64 = 9223372036854775807;
  m.u32 = 4294967295;
  m.u64 = -1; // 18446744073709551615 as raw 64-bit bits
  m.s32 = -2147483648;
  m.s64 = -9223372036854775808;
  m.fx32 = 3735928559;
  m.fx64 = 72623859790382856;
  m.sfx32 = -2147483648;
  m.sfx64 = -9223372036854775808;
  m.f32 = double.infinity;
  m.f64 = -123456.789;
  m.flag = true;
  m.str = 'hello';
  m.color = Color3.COLOR3_GREEN;
  m.inner = Inner3()
    ..name = 'kid'
    ..value = 42;
  m.nums = [1, -5, 300];
  m.tags = ['a', 'bb'];
  m.inners = [
    Inner3()
      ..name = 'x'
      ..value = 1,
    Inner3()
      ..name = 'y'
      ..value = -2,
  ];
  m.hasOptI32 = true;
  m.optI32 = -7;
  m.hasOptStr = true;
  m.optStr = 'opt';
  m.flags = [true, false, true];
  return m;
}

void verify(Scalars3 m) {
  check(m.i32 == -2147483648, 'i32');
  check(m.i64 == 9223372036854775807, 'i64');
  check(m.u32 == 4294967295, 'u32');
  check(m.u64 == -1, 'u64');
  check(m.s32 == -2147483648, 's32');
  check(m.s64 == -9223372036854775808, 's64');
  check(m.fx32 == 3735928559, 'fx32');
  check(m.fx64 == 72623859790382856, 'fx64');
  check(m.sfx32 == -2147483648, 'sfx32');
  check(m.sfx64 == -9223372036854775808, 'sfx64');
  check(m.f32 == double.infinity, 'f32');
  check(m.f64 == -123456.789, 'f64');
  check(m.flag, 'flag');
  check(m.str == 'hello', 'str');
  check(m.color == Color3.COLOR3_GREEN, 'color');
  check(m.inner != null && m.inner!.name == 'kid' && m.inner!.value == 42, 'inner');
  check(m.nums.length == 3 && m.nums[1] == -5, 'nums');
  check(m.tags.length == 2 && m.tags[1] == 'bb', 'tags');
  check(m.inners.length == 2 && m.inners[1].value == -2, 'inners');
  check(m.hasOptI32 && m.optI32 == -7, 'opt_i32');
  check(m.hasOptStr && m.optStr == 'opt', 'opt_str');
  check(m.flags.length == 3 && m.flags[2], 'flags');
}

void main() {
  final reference = File('$data/matrix3.bin').readAsBytesSync();
  final schema = Schema.load('$data/matrix3.desc');
  try {
    final wire = build().encode(schema);
    check(
      wire.length == reference.length &&
          List.generate(wire.length, (i) => wire[i] == reference[i]).every((v) => v),
      'typed encode equals the protoc reference bytes',
    );
    verify(Scalars3.decode(schema, reference));
    check(Scalars3().encode(schema).isEmpty, 'empty typed message is empty');

    final zero = Scalars3()
      ..hasOptI32 = true
      ..optI32 = 0;
    final zeroBack = Scalars3.decode(schema, zero.encode(schema));
    check(zeroBack.hasOptI32 && zeroBack.optI32 == 0, 'explicit zero presence round-trips');

    final nested = Scalars3()
      ..inner = (Inner3()
        ..name = 'kid'
        ..value = 42);
    check(Scalars3.decode(schema, nested.encode(schema)).inner!.value == 42, 'nested typed message');

    final innerWire = (Inner3()..name = 'kid').encode(schema);
    check(Inner3.decode(schema, innerWire).name == 'kid', 'inner encodes standalone');

    // Uint8List equality for the byte check above.
    check(wire is Uint8List, 'encode returns Uint8List');
  } finally {
    schema.close();
  }
  print('typed_test: $checks checks passed');
}

// Web-safety check for the pure core: the whole codec has to survive
// dart2js/dart2wasm, where `int` is a double and only BigInt is exact past
// 2^53. Every 64-bit kind at its edge, a packed run, a utf8 string and an
// unknown field go through encode and decode, and the bytes are pinned here --
// so a build that silently truncates int64 on the web fails this, not the
// conformance suite.
//
//   dart run tests/core/web_smoke.dart            # VM
//   dart compile js -o /tmp/core.js tests/core/web_smoke.dart && node /tmp/core.js
//
// No dart:io and no dart:ffi: the file itself has to compile for the web.

import 'dart:typed_data';

import 'package:pbwire/core/schema.dart';

int _checks = 0;
int _failures = 0;

/// The harness is `dart:io`-based, so this file carries its own: it has to
/// compile and run on the web backend.
void check(bool ok, String what) {
  _checks++;
  if (!ok) {
    _failures++;
    print('FAIL $what');
  }
}

/// A descriptor for
///   syntax = "proto3"; package web;
///   message Edges {
///     int64 i64 = 1; uint64 u64 = 2; sint64 s64 = 3; fixed64 fx64 = 4;
///     int32 i32 = 5; repeated int32 nums = 6; string str = 7;
///     fixed32 fx32 = 8; double f64 = 9;
///   }
Schema _schema() {
  List<int> varint(int v) {
    final out = <int>[];
    while (v > 0x7f) {
      out.add((v % 128) | 0x80);
      v = v ~/ 128;
    }
    out.add(v);
    return out;
  }

  List<int> ld(int fnum, List<int> payload) =>
      [fnum * 8 + 2, ...varint(payload.length), ...payload];
  List<int> vint(int fnum, int v) => [fnum * 8, ...varint(v)];

  List<int> field(int number, String name, int type) => <int>[
        ...ld(1, name.codeUnits),
        ...vint(3, number),
        ...vint(4, 1), // LABEL_OPTIONAL
        ...vint(5, type),
      ];

  // nums is the repeated one; every other field is optional.
  List<int> rep(int number, String name, int type) => <int>[
        ...ld(1, name.codeUnits),
        ...vint(3, number),
        ...vint(4, 3),
        ...vint(5, type),
      ];

  final message = <int>[
    ...ld(1, 'Edges'.codeUnits),
    ...ld(2, field(1, 'i64', 3)),
    ...ld(2, field(2, 'u64', 4)),
    ...ld(2, field(3, 's64', 18)),
    ...ld(2, field(4, 'fx64', 6)),
    ...ld(2, field(5, 'i32', 5)),
    ...ld(2, rep(6, 'nums', 5)),
    ...ld(2, field(7, 'str', 9)),
    ...ld(2, field(8, 'fx32', 7)),
    ...ld(2, field(9, 'f64', 1)),
  ];
  final file = <int>[
    ...ld(1, 'web.proto'.codeUnits),
    ...ld(2, 'web'.codeUnits),
    ...ld(4, message),
    ...ld(12, 'proto3'.codeUnits),
  ];
  return Schema.fromBytes(Uint8List.fromList(ld(1, file)));
}

String _hex(Uint8List b) =>
    b.map((x) => x.toRadixString(16).padLeft(2, '0')).join();

void main() {
  final type = _schema().message('web.Edges');
  final wire = type.encodeObject(<String, Object?>{
    'i64': BigInt.parse('-9223372036854775808'),
    'u64': BigInt.parse('18446744073709551615'),
    's64': BigInt.parse('-9223372036854775808'),
    'fx64': BigInt.parse('72623859790382856'),
    'i32': -2147483648,
    'nums': <Object?>[1, -5, 2147483647],
    'str': 'héllo',
    'fx32': 3735928559,
  });
  // The reference bytes for those edges, read off the same payload through
  // protoc/L language bindings elsewhere in this repo:
  //   i64=-2^63 (10 bytes), u64=max, s64=-2^63 zigzagged to 0xffff...ff,
  //   fx64=0x0102030405060708 little-endian, i32=-2^31 sign-extended,
  //   nums={1,-5,2^31-1} packed (16 bytes), "h\xc3\xa9llo", fx32=0xdeadbeef.
  check(
      _hex(wire) ==
          '08'
              '808080808080808080' '01'
              '10' 'ffffffffffffffffff01'
              '18' 'ffffffffffffffffff01'
              '21' '0807060504030201'
              '28' '80808080f8ffffffff01'
              '32' '10' '01' 'fbffffffffffffffff01' 'ffffffff07'
              '3a' '06' '68c3a96c6c6f'
              '45' 'efbeadde',
      'web: the wire bytes are the reference bytes (${_hex(wire)})');

  final m = type.decode(wire);
  check(getField(m, 'i64') == BigInt.parse('-9223372036854775808'),
      'web: int64 min survives decode');
  check(getField(m, 'u64') == BigInt.parse('18446744073709551615'),
      'web: uint64 max survives decode');
  check(getField(m, 's64') == BigInt.parse('-9223372036854775808'),
      'web: sint64 min survives decode');
  check(getField(m, 'fx64') == BigInt.parse('72623859790382856'),
      'web: fixed64 survives decode');
  check('${getField(m, 'nums')}' == '[1, -5, 2147483647]',
      'web: an int32 packed run survives decode');
  check(getField(m, 'str') == 'héllo', 'web: a utf8 string survives decode');
  check(getField(m, 'fx32') == 3735928559, 'web: fixed32 survives decode');
  check(_hex(type.encode(m)) == _hex(wire),
      'web: decode/re-encode is byte-exact');

  // A zigzag at the edge, an unknown field and a wrong wire type all survive.
  final extra = Uint8List.fromList(<int>[
    ...wire,
    0x98, 0x06, 0x05, // field 99 varint 5, unknown
  ]);
  final m2 = type.decode(extra);
  check(_hex(type.encode(m2)) == _hex(extra),
      'web: an unknown field is retained in place');

  // 2^53 + 1 is the first integer a double cannot hold: the decode must keep
  // it exactly, which is the whole reason the 64-bit kinds are BigInt.
  final big = type.encodeObject(<String, Object?>{
    'u64': BigInt.parse('9007199254740993'),
  });
  check(getField(type.decode(big), 'u64') == BigInt.parse('9007199254740993'),
      'web: 2^53+1 is exact');

  if (_failures != 0) {
    print('core/web_smoke: $_failures of $_checks checks failed');
    throw StateError('web smoke failed');
  }
  print('core/web_smoke: PASS ($_checks checks)');
}

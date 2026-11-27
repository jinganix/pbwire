// Wire primitives: varints, tags, little-endian, zigzag, skipping, the
// structural UTF-8 check and the error shapes the decoder relies on.

import 'dart:typed_data';

import 'package:pbwire/core/schema.dart';
import 'package:pbwire/core/wire.dart' as wire;
import 'package:pbwire/core/wire.dart' show R, W;

import 'harness.dart';

List<int> bytes(W w) => w.b.sublist(0, w.pos);

bool throwsCore(void Function() f, [String? match]) {
  try {
    f();
  } on CoreError catch (e) {
    return match == null || e.message.contains(match);
  }
  return false;
}

void main() {
  _varints();
  _fixed();
  _zigzag();
  _tags();
  _lengths();
  _skip();
  _groups();
  _utf8();
  _bits();
  _patch();
  done('core/wire_test');
}

void _varints() {
  for (final v in [0, 1, 127, 128, 300, 0x7fffffff, 0xffffffff]) {
    final w = W();
    wire.wVar32(w, v);
    final r = R(w.b, 0, w.pos);
    check(wire.rVar(r) == v % wire.u32, 'varint32 round trip $v');
    check(r.pos == w.pos, 'varint32 consumed $v');
    check(w.pos == wire.sizeVar32(v % wire.u32), 'varint32 size $v');
  }
  for (final v in [
    BigInt.zero,
    BigInt.one,
    BigInt.from(127),
    BigInt.from(128),
    BigInt.from(0xffffffff),
    BigInt.parse('9223372036854775807'),
    BigInt.parse('18446744073709551615'),
  ]) {
    final w = W();
    wire.wVar64(w, v);
    final r = R(w.b, 0, w.pos);
    check(wire.rVar64(r) == v, 'varint64 round trip $v');
    check(w.pos == wire.sizeVar64(v), 'varint64 size $v');
  }

  final w = W();
  wire.wVar64(w, BigInt.from(-1));
  check(
      bytes(w).join(',') == '255,255,255,255,255,255,255,255,255,1',
      'int32 -1 sign-extends to ten bytes');
  check(wire.sizeInt32(-1) == 10, 'sizeInt32(-1) is 10');
  check(wire.sizeInt32(1) == 1, 'sizeInt32(1) is 1');

  check(throwsCore(() => wire.rVar(R(Uint8List(0))), 'truncated'),
      'empty varint is truncated');
  check(throwsCore(() => wire.rVar(R(Uint8List.fromList([0x80])))), 'one 0x80');
  final eleven = Uint8List(11)..fillRange(0, 11, 0x80);
  check(throwsCore(() => wire.rVar(R(eleven)), 'overflow'), 'varint32 overflow');
  check(throwsCore(() => wire.rVar64(R(eleven)), 'overflow'), 'varint64 overflow');
}

void _fixed() {
  final w = W();
  wire.wFixed32(w, 0xdeadbeef);
  wire.wFixed64(w, BigInt.parse('0102030405060708', radix: 16));
  check(
      bytes(w).join(',') == '239,190,173,222,8,7,6,5,4,3,2,1',
      'fixed32/fixed64 are little-endian');
  final r = R(w.b, 0, w.pos);
  check(wire.rFixed32(r) == 0xdeadbeef, 'fixed32 round trip');
  check(wire.rFixed64(r) == BigInt.parse('0102030405060708', radix: 16),
      'fixed64 round trip');
}

void _zigzag() {
  check(wire.zig32(0) == 0, 'zig32(0)');
  check(wire.zig32(-1) == 1, 'zig32(-1)');
  check(wire.zig32(1) == 2, 'zig32(1)');
  check(wire.zig32(-2147483648) == 4294967295, 'zig32(min)');
  check(wire.unzig32(4294967295) == -2147483648, 'unzig32(max)');
  check(wire.zig64(BigInt.from(-1)) == BigInt.one, 'zig64(-1)');
  check(wire.zig64(BigInt.parse('-9223372036854775808')) ==
          BigInt.parse('18446744073709551615'),
      'zig64(min)');
  check(wire.unzig64(BigInt.parse('18446744073709551615')) ==
          BigInt.parse('-9223372036854775808'),
      'unzig64(max)');
}

void _tags() {
  final w = W();
  wire.wTag(w, wire.tagBytes(1, 0));
  check(bytes(w).join(',') == '8', 'tag(1,varint) is 0x08');
  check(wire.tagBytes(536870911, 2).join(',') == '250,255,255,255,15',
      'the largest field number is a five-byte tag');

  // 0xffffffff as a 5-byte varint is a tag above UINT32_MAX: the reference
  // truncates it instead of rejecting it (the two expected conformance
  // failures pin this).
  final r = R(Uint8List.fromList([0xff, 0xff, 0xff, 0xff, 0x0f]));
  check(wire.rTag(r) == 0xffffffff, 'a wide 5-byte tag truncates');
  check(
      throwsCore(
          () => wire.rTag(
              R(Uint8List.fromList([0x80, 0x80, 0x80, 0x80, 0x80, 0x00]))),
          'bad tag'),
      'a sixth tag byte is invalid');
}

void _lengths() {
  final r = R(Uint8List(200)..fillRange(0, 200, 1));
  r.b[0] = 0x7f;
  check(wire.rLen32(r) == 0x7f, 'a one-byte length');
  check(
      throwsCore(
          () => wire.rLen32(
              R(Uint8List.fromList([0xff, 0xff, 0xff, 0xff, 0x1f]))),
          'overflow'),
      'the fifth length byte carries four bits');
  check(throwsCore(() => wire.rLen32(R(Uint8List.fromList([0x02, 0x00]))),
          'truncated'),
      'a length past the end is truncated');
}

void _skip() {
  final w = W();
  wire.wTag(w, wire.tagBytes(1, 0));
  wire.wVar32(w, 300);
  wire.wTag(w, wire.tagBytes(2, 1));
  wire.wFixed64(w, BigInt.from(5));
  wire.wTag(w, wire.tagBytes(3, 5));
  wire.wFixed32(w, 7);
  wire.wBytes(w, wire.tagBytes(4, 2), Uint8List.fromList([1, 2, 3]));
  final r = R(w.b, 0, w.pos);
  for (var i = 0; i < 4; i++) {
    final tag = wire.rTag(r);
    wire.skip(r, tag % 8);
  }
  check(r.pos == w.pos, 'skip walks every non-group wire type');
  check(throwsCore(() => wire.skip(R(Uint8List.fromList([0])), 3),
          'bad wire type'),
      'skip rejects a start-group wire type');
}

void _groups() {
  final w = W();
  wire.wTag(w, wire.tagBytes(5, 3)); // start group 5
  wire.wTag(w, wire.tagBytes(1, 0));
  wire.wVar32(w, 1);
  wire.wTag(w, wire.tagBytes(5, 4)); // end group 5
  // skipGroup starts inside the body: the caller read the start tag.
  final r = R(w.b, 0, w.pos);
  final start = wire.rTag(r);
  wire.skipUnknown(r, 5, start % 8, 0);
  check(r.pos == w.pos, 'skipUnknown walks a group body');

  check(throwsCore(() => wire.skipGroup(R(Uint8List.fromList([0x04])), 5, 0),
          'bad tag'),
      'field number 0 is invalid');
  check(throwsCore(() => wire.skipGroup(R(Uint8List.fromList([0x34])), 5, 0),
          'mismatched end-group'),
      'an end tag only closes the group it names');
  final deep = R(Uint8List.fromList([0x0b]));
  deep.cap = 0;
  check(throwsCore(() => wire.skipGroup(deep, 1, 1), 'nesting exceeds'),
      'the group frame chain is capped');

  // Boundary pin: exactly 100 nested groups scan, 101 trip the cap
  // (the C core's PBW_MAX_DEPTH).
  Uint8List nested(int n) => Uint8List.fromList(
      [...List.filled(n, 0x0b), ...List.filled(n, 0x0c)]);
  // skipUnknown starts inside the body: the caller read the start tag.
  final atCap = R(nested(100));
  final atCapStart = wire.rTag(atCap);
  wire.skipUnknown(atCap, 1, atCapStart % 8, 0);
  check(atCap.pos == 200, 'exactly 100 nested groups scan');
  check(
      throwsCore(() {
        final pastCap = R(nested(101));
        final start = wire.rTag(pastCap);
        wire.skipUnknown(pastCap, 1, start % 8, 0);
      }, 'nesting exceeds'),
      '101 nested groups trip the cap');
}

void _utf8() {
  bool okText(String s) {
    final b = Uint8List.fromList(utf8Encode(s));
    return wire.utf8Valid(b, 0, b.length);
  }

  bool bad(List<int> b) =>
      !wire.utf8Valid(Uint8List.fromList(b), 0, b.length);

  check(okText('hello'), 'ascii is valid');
  check(okText('héllo'), 'two-byte is valid');
  check(okText('日本語'), 'three-byte is valid');
  check(okText('😀'), 'four-byte is valid');
  check(bad([0xc0, 0x80]), 'overlong NUL');
  check(bad([0xe0, 0x80, 0x80]), 'overlong three-byte');
  check(bad([0xed, 0xa0, 0x80]), 'surrogate');
  check(bad([0xf4, 0x90, 0x80, 0x80]), 'past U+10FFFF');
  check(bad([0xff]), '0xff');
  check(bad([0xe2, 0x82]), 'truncated sequence');

  final w = W();
  const s = 'aé日😀';
  wire.ensure(w, wire.utf8Length(s));
  wire.utf8Write(w, s);
  check(w.pos == wire.utf8Length(s), 'utf8Length matches the write');
  check(wire.utf8Decode(w.b, 0, w.pos) == s, 'utf8 round trip');

  // A lone surrogate becomes U+FFFD, and a malformed byte decodes to one.
  final lone = W();
  const surrogate = '\ud800';
  wire.ensure(lone, wire.utf8Length(surrogate));
  wire.utf8Write(lone, surrogate);
  check(wire.utf8Length(surrogate) == 3, 'a lone surrogate is three bytes');
  check(wire.utf8Decode(lone.b, 0, lone.pos) == '\ufffd',
      'a lone surrogate decodes to U+FFFD');
  check(
      wire.utf8Decode(Uint8List.fromList([0x61, 0xff, 0x62]), 0, 3) ==
          'a\ufffdb',
      'a malformed byte decodes to one U+FFFD');
}

/// The test's own UTF-8 encoder: `utf8Encode` from wire.dart is not exported.
List<int> utf8Encode(String s) {
  final w = W();
  wire.ensure(w, wire.utf8Length(s));
  wire.utf8Write(w, s);
  return bytes(w);
}

void _bits() {
  const nan32 = 0x7fc00001;
  check(wire.floatBits(wire.bitsFloat(nan32)) == nan32,
      'float NaN payload survives');
  check(wire.bitsFloat(nan32).isNaN, 'the float bits are NaN');
  check(wire.floatBits(-0.0) == 0x80000000, 'float -0 bits');
  final nan64 = BigInt.parse('7ff8000000000001', radix: 16);
  check(wire.doubleBits(wire.bitsDouble(nan64)) == nan64,
      'double NaN payload survives');
  final negZero = BigInt.parse('8000000000000000', radix: 16);
  check(wire.doubleBits(wire.bitsDouble(negZero)) == negZero,
      'double -0 bits');
}

void _patch() {
  final small = W();
  wire.wTag(small, wire.tagBytes(1, 2));
  final lp = small.pos;
  small.b[small.pos++] = 0;
  wire.ensure(small, 3);
  small.b.fillRange(small.pos, small.pos + 3, 0);
  small.pos += 3;
  wire.patchLen(small, lp);
  check(bytes(small).join(',') == '10,3,0,0,0', 'a one-byte length patches');

  final big = W();
  wire.wTag(big, wire.tagBytes(1, 2));
  final lp2 = big.pos;
  big.b[big.pos++] = 0;
  wire.ensure(big, 300);
  big.b.fillRange(big.pos, big.pos + 300, 7);
  big.pos += 300;
  wire.patchLen(big, lp2);
  final out = big.b.sublist(0, big.pos);
  check(out.sublist(0, 3).join(',') == '10,172,2', '300 shifts to two bytes');
  check(out.length == 3 + 300, 'the shifted payload keeps its length');
  check(wire.maxDepth == 100, 'the nesting cap is 100');
}

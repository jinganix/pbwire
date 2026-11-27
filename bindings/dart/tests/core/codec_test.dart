// Codec behaviour: byte equality against the fixtures the C tests pin,
// decode/encode round trips, unknown retention (unknown numbers and
// wrong-wire-type occurrences, input order), groups, packed runs, oneof
// last-wins, presence, proto3 UTF-8 validation, proto2 required fields and
// the byte/depth limits.
//
//   dart run tests/core/codec_test.dart [data dir]

import 'dart:io';
import 'dart:typed_data';

import 'package:pbwire/core/schema.dart';

import 'harness.dart';

late String data;

MessageType typeOf(String file, String name) =>
    Schema.fromBytes(File('$data/$file').readAsBytesSync()).message(name);

Uint8List fixture(String file) => File('$data/$file').readAsBytesSync();

Uint8List u8(List<int> bytes) => Uint8List.fromList(bytes);

/// Concatenates byte arrays into one input buffer.
Uint8List cat(List<List<int>> parts) {
  var n = 0;
  for (final p in parts) {
    n += p.length;
  }
  final out = Uint8List(n);
  var off = 0;
  for (final p in parts) {
    out.setRange(off, off + p.length, p);
    off += p.length;
  }
  return out;
}

bool same(List<int> a, List<int> b) {
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

/// The status of the CoreError `f` throws, or null when it does not throw.
int? statusOf(void Function() f) {
  try {
    f();
  } on CoreError catch (e) {
    return e.status;
  }
  return null;
}

/* matrix3.txt: every scalar at an edge, both repeated forms, nested message. */
Map<String, Object?> matrix3(int n) => <String, Object?>{
      'i32': -2147483648,
      'i64': BigInt.parse('9223372036854775807'),
      'u32': 4294967295,
      'u64': BigInt.parse('18446744073709551615'),
      's32': -2147483648,
      's64': BigInt.parse('-9223372036854775808'),
      'fx32': 3735928559,
      'fx64': BigInt.parse('72623859790382856'),
      'sfx32': -2147483648,
      'sfx64': BigInt.parse('-9223372036854775808'),
      'f32': double.infinity,
      'f64': -123456.789,
      'flag': true,
      'str': 'hello',
      'color': 2,
      'inner': <String, Object?>{'name': 'kid', 'value': 42},
      'nums': <Object?>[1, -5, 300],
      'tags': <Object?>['a', 'bb'],
      'inners': <Object?>[
        <String, Object?>{'name': 'x', 'value': 1},
        <String, Object?>{'name': 'y', 'value': -2},
      ],
      'opt_i32': -7,
      'opt_str': 'opt',
      'flags': <Object?>[true, false, true],
    };

void main(List<String> args) {
  data = args.isNotEmpty
      ? args[0]
      : '${File.fromUri(Platform.script).parent.parent.parent.path}/../c/tests/data';

  _matrix3Object();
  _matrix3Decode();
  _matrix2();
  _groups();
  _oneof();
  _unknowns();
  _wrongWireType();
  _packedForms();
  _implicitZero();
  _utf8Strings();
  _limits();
  _mapsAndExtensions();
  _rawPayload();

  done('core/codec_test');
}

void _matrix3Object() {
  final type = typeOf('matrix3.desc', 'structtest.Scalars3');
  final wire = type.encodeObject(matrix3(0));
  check(same(wire, fixture('matrix3.bin')),
      'matrix3: object -> wire is byte-identical to protoc');
}

void _matrix3Decode() {
  final type = typeOf('matrix3.desc', 'structtest.Scalars3');
  final bin = fixture('matrix3.bin');
  final m = type.decode(bin);
  check(getField(m, 'i32') == -2147483648, 'decode i32');
  check(getField(m, 'u32') == 4294967295, 'decode u32');
  check(getField(m, 'i64') == BigInt.parse('9223372036854775807'), 'decode i64');
  check(getField(m, 'u64') == BigInt.parse('18446744073709551615'),
      'decode u64');
  check(getField(m, 's64') == BigInt.parse('-9223372036854775808'),
      'decode s64');
  check(getField(m, 'fx64') == BigInt.parse('72623859790382856'), 'decode fx64');
  check(getField(m, 'sfx64') == BigInt.parse('-9223372036854775808'),
      'decode sfx64');
  check(getField(m, 'f32') == double.infinity, 'decode f32');
  check(getField(m, 'f64') == -123456.789, 'decode f64');
  check(getField(m, 'flag') == true, 'decode flag');
  check(getField(m, 'str') == 'hello', 'decode str');
  check(getField(m, 'color') == 2, 'decode color');
  check('${getField(m, 'nums')}' == '[1, -5, 300]', 'decode nums');
  check('${getField(m, 'tags')}' == '[a, bb]', 'decode tags');
  check('${getField(m, 'flags')}' == '[true, false, true]', 'decode flags');
  check(getField(m, 'opt_i32') == -7, 'decode opt_i32');
  check(getField(m, 'opt_str') == 'opt', 'decode opt_str');
  final inner = getField(m, 'inner') as Msg;
  check(getField(inner, 'name') == 'kid', 'decode inner.name');
  check(getField(inner, 'value') == 42, 'decode inner.value');
  check(toObject(m).toString() == matrix3(0).toString(), 'toObject matches');
  check(same(type.encode(m), bin), 'matrix3: decode/re-encode is byte-exact');
}

void _matrix2() {
  final type = typeOf('matrix2.desc', 'structtest.Scalars2');
  final bin = fixture('matrix2.bin');
  final m = type.decode(bin);
  check(getField(m, 'id') == 99, 'proto2 id');
  check(getField(m, 's64') == BigInt.from(-1), 'proto2 s64');
  check(getField(m, 'shade') == -2, 'proto2 shade');
  check('${getField(m, 'nums')}' == '[4, 5]', 'proto2 unpacked nums');
  check('${getField(m, 'packed_nums')}' == '[6, 7, 8]', 'proto2 packed run');
  check(getField(getField(m, 'inner') as Msg, 'name') == 'req',
      'proto2 inner.name');
  check(same(type.encode(m), bin), 'proto2 decode/re-encode is byte-exact');

  // A missing required field fails the decode with PBW_ERR_REQUIRED (6): the
  // first field of the fixture is `id`, so drop it.
  check(statusOf(() => type.decode(Uint8List.sublistView(bin, 2))) == 6,
      'a missing required field fails with status 6');
}

void _groups() {
  final type = typeOf('grouptest.desc', 'structtest.GroupTest');
  final bin = fixture('grouptest.bin');
  final m = type.decode(bin);
  final inner = getField(m, 'inner') as Msg;
  check(inner.def.fullName == 'structtest.GroupTest.Inner', 'group type');
  check(getField(inner, 'x') == 5, 'group inner.x');
  check(getField(inner, 's') == 'g', 'group inner.s');
  final items = (getField(m, 'item') as List<Object?>).cast<Msg>();
  check(items.map((g) => getField(g, 'v')).join(',') == '1,-2', 'group items');
  check(getField(m, 'id') == 9, 'group id');
  check(same(type.encode(m), bin), 'groups decode/re-encode byte-exactly');

  // A group that never closes is a parse failure, not a silent skip.
  check(statusOf(() => type.decode(Uint8List.sublistView(bin, 0, bin.length - 1))) !=
          null,
      'a group that never closes fails');
}

void _oneof() {
  final type = typeOf('oneoftest.desc', 'structtest.OneOfTest');
  final bin = fixture('oneoftest.bin');
  final m = type.decode(bin);
  check(oneofCase(m, 'kind') == 2, 'oneof case follows the last member');
  check(getField(m, 'text') == 'hi', 'oneof text');
  check(getField(m, 'num') == null, 'oneof num stays clear');
  check(same(type.encode(m), bin), 'oneof decode/re-encode byte-exactly');

  // num then text: the text member replaces the number, not merges.
  final both = u8([0x08, 0x03, 0x12, 0x02, 0x68, 0x69, 0x28, 0x01]);
  final last = type.decode(both);
  check(oneofCase(last, 'kind') == 2, 'last oneof member wins');
  check(getField(last, 'num') == null, 'the cleared member is gone');
  // Only the set member is emitted, so the cleared number is gone.
  check(same(type.encode(last), u8([0x12, 0x02, 0x68, 0x69, 0x28, 0x01])),
      'only the set oneof member is emitted');
}

void _unknowns() {
  final type = typeOf('matrix3.desc', 'structtest.Scalars3');
  final bin = fixture('matrix3.bin');
  // Field 99 varint 5, then field 100 length-delimited "zz".
  final unknownA = u8([0x98, 0x06, 0x05]);
  final unknownB = u8([0xa2, 0x06, 0x02, 0x7a, 0x7a]);
  final wire = cat([unknownA, bin, unknownB]);
  final m = type.decode(wire);
  // Known fields keep their values; both unknowns survive, in input order.
  check(getField(m, 'str') == 'hello', 'known field survives an unknown');
  check(same(type.encode(m), cat([bin, unknownA, unknownB])),
      'unknown fields are retained after the known ones, in input order');

  // Unknowns only, nothing dropped: a message of pure unknowns re-encodes to
  // itself.
  final only = type.decode(cat([unknownA, unknownB]));
  check(same(type.encode(only), cat([unknownA, unknownB])),
      'a message of unknowns re-encodes to itself');
}

void _wrongWireType() {
  final type = typeOf('matrix3.desc', 'structtest.Scalars3');
  // i32 (field 1) declared varint, sent as length-delimited.
  final wrong = u8([0x0a, 0x01, 0x00]);
  final m = type.decode(wrong);
  check(getField(m, 'i32') == null, 'a wrong wire type leaves the field absent');
  check(same(type.encode(m), wrong), 'a wrong wire type is retained verbatim');

  // A group wire type on a scalar field is also unknown, and its body is
  // validated (field 1 start group, end group) before retention.
  final group = u8([0x0b, 0x0c]);
  check(same(type.encode(type.decode(group)), group),
      'an unknown group is validated and retained');
}

void _packedForms() {
  final type = typeOf('matrix3.desc', 'structtest.Scalars3');
  // nums = 17: unpacked elements (tag 0x88 0x01) for 1 and -5.
  final unpacked = cat([
    u8([0x88, 0x01, 0x01]),
    u8([0x88, 0x01, 0xfb, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01]),
  ]);
  final m = type.decode(unpacked);
  check('${getField(m, 'nums')}' == '[1, -5]', 'unpacked runs are accepted');
  // The encoder writes the declared packed form, which is what the reference
  // emits for the same tree.
  final packed = u8([
    0x8a, 0x01, 0x0b, 0x01, // 1
    0xfb, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01, // -5
  ]);
  check(same(type.encode(m), packed), 'a decoded unpacked run re-encodes packed');

  // A packed run with the wrong length fails instead of reading past it.
  check(statusOf(() => type.decode(u8([0x8a, 0x01, 0x02, 0x01]))) != null,
      'a short packed run fails');

  // An empty packed run adds nothing.
  final empty = type.decode(u8([0x8a, 0x01, 0x00]));
  check(getField(empty, 'nums') == null, 'an empty packed run stays absent');
  check(type.encode(empty).isEmpty, 'an empty packed run encodes to nothing');
}

void _implicitZero() {
  final type = typeOf('matrix3.desc', 'structtest.Scalars3');
  // i32 field 1 with the varint 2^32: the reference truncates to int32 0 and
  // drops the field as an implicit zero, so the output is empty.
  final m = type.decode(u8([0x08, 0x80, 0x80, 0x80, 0x80, 0x10]));
  check(getField(m, 'i32') == null, 'a wide varint truncates to an implicit zero');
  check(type.encode(m).isEmpty, 'the implicit zero is not emitted');
}

void _utf8Strings() {
  final proto3 = typeOf('matrix3.desc', 'structtest.Scalars3');
  // str = 14, one 0xFF byte.
  check(statusOf(() => proto3.decode(u8([0x72, 0x01, 0xff]))) != null,
      'proto3 strings must be valid UTF-8');

  // matrix2 str is proto2: the bytes are kept and re-emitted unchanged.
  final proto2 = typeOf('matrix2.desc', 'structtest.Scalars2');
  final id = u8([0x08, 0x01]);
  final str = u8([0x3a, 0x02, 0xff, 0xfe]);
  final m = proto2.decode(cat([id, str]));
  check(same(proto2.encode(m), cat([id, str])),
      'proto2 strings are passed through byte for byte');
}

void _limits() {
  final type = typeOf('matrix3.desc', 'structtest.Scalars3');
  final bin = fixture('matrix3.bin');
  check(statusOf(() => type.decode(bin, const Limits(maxBytes: 10))) == 5,
      'the byte cap fails with status 5');
  check(
      same(
          type.encode(
              type.decode(bin, Limits(maxBytes: bin.length))),
          bin),
      'a cap at the exact size still decodes');

  // A self-nesting message from a synthetic descriptor: eight levels of `kid`
  // must be rejected by a cap of four and accepted by the default.
  var body = u8(<int>[]);
  for (var i = 0; i < 8; i++) {
    body = cat([
      [0x0a, body.length],
      body,
    ]);
  }
  final deep = _deepType();
  check(
      statusOf(() => deep.decode(body, const Limits(maxDepth: 4))) != null,
      'the nesting cap rejects eight levels with a cap of four');
  check(deep.decode(body).def.fullName == 'deep.Node',
      'the default cap accepts eight levels');

  // The same cap applies to an unknown group chain, which the decoder walks
  // through skipGroup.
  var groups = u8([0x0c]);
  for (var i = 0; i < 8; i++) {
    groups = cat([
      [0x0b],
      groups,
      [0x0c],
    ]);
  }
  check(
      statusOf(() => type.decode(groups, const Limits(maxDepth: 2))) != null,
      'the nesting cap applies to unknown groups too');

  // Boundary pin: exactly 100 nestings decode, 101 trip the cap
  // (the C core's PBW_MAX_DEPTH).
  List<int> lenPrefix(int n) {
    final out = <int>[];
    var v = n;
    while (v > 0x7f) {
      out.add((v % 128) | 0x80);
      v = v ~/ 128;
    }
    out.add(v);
    return out;
  }

  var atCap = u8(<int>[]);
  for (var i = 0; i < 100; i++) {
    atCap = cat([
      [0x0a, ...lenPrefix(atCap.length)],
      atCap,
    ]);
  }
  check(statusOf(() => deep.decode(atCap)) == null,
      'exactly 100 nestings decode');
  var pastCap = u8(<int>[]);
  for (var i = 0; i < 101; i++) {
    pastCap = cat([
      [0x0a, ...lenPrefix(pastCap.length)],
      pastCap,
    ]);
  }
  check(statusOf(() => deep.decode(pastCap)) == 4,
      '101 nestings trip the cap');
}

void _mapsAndExtensions() {
  final maps = typeOf('maptest.desc', 'structtest.Maps');
  final mapBin = fixture('maptest.bin');
  check(same(maps.encode(maps.decode(mapBin)), mapBin),
      'maptest fixture round trips');

  final ext = typeOf('extensiontest.desc', 'exttest.HasExt');
  final extBin = fixture('extensiontest.bin');
  check(same(ext.encode(ext.decode(extBin)), extBin),
      'extensiontest fixture round trips');
}

void _rawPayload() {
  final type = typeOf('matrix3.desc', 'structtest.Scalars3');
  final m = type.decode(fixture('matrix3.bin'));
  // The decoded string is still a slice until read.
  final slot = m.def.byName['str']!;
  final raw = m.v[slot] as Raw;
  check(String.fromCharCodes(rawPayload(raw)) == 'hello',
      'rawPayload returns the payload bytes');
  check(getField(m, 'str') == 'hello', 'materializing the slice reads the value');
}

/// A hand-built FileDescriptorSet for a self-nesting proto3 message:
///   syntax = "proto3"; package deep; message Node { Node kid = 1; }
MessageType _deepType() {
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

  final field = <int>[
    ...ld(1, 'kid'.codeUnits),
    ...vint(3, 1), // number
    ...vint(4, 1), // LABEL_OPTIONAL
    ...vint(5, 11), // TYPE_MESSAGE
    ...ld(6, '.deep.Node'.codeUnits),
  ];
  final message = <int>[...ld(1, 'Node'.codeUnits), ...ld(2, field)];
  final file = <int>[
    ...ld(1, 'deep.proto'.codeUnits),
    ...ld(2, 'deep'.codeUnits),
    ...ld(4, message),
    ...ld(12, 'proto3'.codeUnits),
  ];
  return Schema.fromBytes(Uint8List.fromList(ld(1, file))).message('deep.Node');
}

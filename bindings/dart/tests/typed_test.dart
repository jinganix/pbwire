// Typed-compat check for the Dart binding (TESTSPEC T1–T6, vectors.json as
// the single source of truth):
//
//   T1  generated writeToBuffer == pinned protoc reference bytes
//   T2  fromBuffer of the reference reads every fixture value back
//   T3  create() encodes empty; explicit optional zero round-trips
//   T4  differential vs the pinned protobuf-dart oracle: same bytes, same
//       fields, cross-decode agrees
//   T5  one business workflow runs against both libraries' messages with
//       identical observable results (import path is the only difference)
//   T6  errors are loud, not crashes: bad wire throws on both sides,
//       oneof keeps the last member, has/clear agree
//
//   cd bindings/dart && dart run tests/typed_test.dart

import 'dart:convert';
import 'dart:io';

import 'package:fixnum/fixnum.dart';

import 'gen/matrix3.dart' as pb;
import 'gen/oneof1.dart' as pb1;
import 'gen_oracle/matrix3.pb.dart' as oracle;
import 'gen_oracle/oneof1.pb.dart' as oracle1;

// Pinned by bindings/dart/pubspec.lock; the test fails loudly on drift
// (see ORACLES.md in bindings/typed/testdata).
const oraclePin = '6.1.0';

var checks = 0;

void check(bool ok, String what) {
  if (!ok) {
    stderr.writeln('FAIL $what');
    exit(1);
  }
  checks++;
}

bool bytesEqual(List<int> a, List<int> b) {
  if (a.length != b.length) return false;
  for (var i = 0; i < a.length; i++) {
    if (a[i] != b[i]) return false;
  }
  return true;
}

/// int64 fixture values arrive as JSON strings (they do not fit a JSON
/// number exactly); mask into signed 64 bits like the wire does.
Int64 parseI64(dynamic v) {
  if (v is int) return Int64(v);
  final masked =
      (BigInt.parse(v as String) & BigInt.from(0xFFFFFFFFFFFFFFFF)).toSigned(64);
  return Int64(masked.toInt());
}

double parseF64(dynamic v) {
  if (v is num) return v.toDouble();
  return switch (v as String) {
    'Infinity' => double.infinity,
    '-Infinity' => double.negativeInfinity,
    'NaN' => double.nan,
    _ => throw StateError('bad f64 fixture $v'),
  };
}

pb.Color3 parseColor(String name) =>
    pb.Color3.values.firstWhere((e) => e.name == name);

/// Builds Scalars3 through the generated facade only, from vectors.json.
pb.Scalars3 buildFacade(Map<String, dynamic> v) {
  final inner = v['inner'] as Map<String, dynamic>;
  final m = pb.Scalars3(
    i32: v['i32'] as int,
    i64: parseI64(v['i64']),
    u32: v['u32'] as int,
    u64: parseI64(v['u64']),
    s32: v['s32'] as int,
    s64: parseI64(v['s64']),
    fx32: v['fx32'] as int,
    fx64: parseI64(v['fx64']),
    sfx32: v['sfx32'] as int,
    sfx64: parseI64(v['sfx64']),
    f32: parseF64(v['f32']),
    f64: (v['f64'] as num).toDouble(),
    flag: v['flag'] as bool,
    str: v['str'] as String,
    color: parseColor(v['color'] as String),
    inner: pb.Inner3(
        name: inner['name'] as String, value: inner['value'] as int),
    nums: (v['nums'] as List).cast<int>(),
    tags: (v['tags'] as List).cast<String>(),
    optI32: v['opt_i32'] as int,
    optStr: v['opt_str'] as String,
  );
  for (final e in v['inners'] as List) {
    final eMap = e as Map<String, dynamic>;
    m.inners.add(pb.Inner3(name: eMap['name'] as String, value: eMap['value'] as int));
  }
  for (final f in v['flags'] as List) {
    m.flags.add(f as bool);
  }
  return m;
}

oracle.Scalars3 buildOracle() => oracle.Scalars3(
      i32: -2147483648,
      i64: Int64(9223372036854775807),
      u32: 4294967295,
      u64: Int64(-1), // 18446744073709551615 bits
      s32: -2147483648,
      s64: Int64(-9223372036854775807 - 1),
      fx32: 0xdeadbeef,
      fx64: Int64(72623859790382856),
      sfx32: -2147483648,
      sfx64: Int64(-9223372036854775807 - 1),
      f32: double.infinity,
      f64: -123456.789,
      flag: true,
      str: 'hello',
      color: oracle.Color3.COLOR3_GREEN,
      inner: oracle.Inner3(name: 'kid', value: 42),
      nums: [1, -5, 300],
      tags: ['a', 'bb'],
      inners: [
        oracle.Inner3(name: 'x', value: 1),
        oracle.Inner3(name: 'y', value: -2),
      ],
      optI32: -7,
      optStr: 'opt',
      flags: [true, false, true],
    );

/// The protobuf version pub resolved: the oracle pin lives in pubspec.lock.
String lockVersion() {
  var inProtobuf = false;
  for (final line in File('pubspec.lock').readAsLinesSync()) {
    final t = line.trim();
    if (t == 'protobuf:') {
      inProtobuf = true;
    } else if (inProtobuf && t.startsWith('version: ')) {
      return t.split('"')[1];
    } else if (inProtobuf && !line.startsWith(' ') && !line.startsWith('\t')) {
      inProtobuf = false;
    }
  }
  return 'unknown';
}

bool throws(void Function() op) {
  try {
    op();
    return false;
  } catch (_) {
    return true;
  }
}

void main() {
  final testdata = '../typed/testdata';
  final reference = File('../c/tests/data/matrix3.bin').readAsBytesSync();
  final referenceOneof = File('$testdata/oneof1.bin').readAsBytesSync();
  final vectors = (jsonDecode(File('$testdata/vectors.json').readAsStringSync())
      as Map<String, dynamic>)['values'] as Map<String, dynamic>;

  // T1: encode equals the protoc reference bytes.
  final wire = buildFacade(vectors).writeToBuffer();
  check(bytesEqual(wire, reference), 'T1 facade != reference');
  final oneWire =
      (pb1.Oneof1.create()..name = 'a'..text = 'hi').writeToBuffer();
  check(bytesEqual(oneWire, referenceOneof), 'T1 oneof != reference');

  // T2: decode reads every fixture value back.
  final back = pb.Scalars3.fromBuffer(reference);
  check(back.i32 == vectors['i32'], 'T2 i32');
  check(back.i64 == parseI64(vectors['i64']), 'T2 i64');
  check(back.u32 == vectors['u32'], 'T2 u32');
  check(back.u64 == parseI64(vectors['u64']), 'T2 u64');
  check(back.s32 == vectors['s32'] && back.s64 == parseI64(vectors['s64']),
      'T2 s32/s64');
  check(back.fx32 == vectors['fx32'] && back.fx64 == parseI64(vectors['fx64']),
      'T2 fx32/fx64');
  check(back.sfx32 == vectors['sfx32'] &&
      back.sfx64 == parseI64(vectors['sfx64']), 'T2 sfx32/sfx64');
  check(back.f32 == double.infinity && back.f64 == vectors['f64'],
      'T2 f32/f64');
  check(back.flag == vectors['flag'] && back.str == vectors['str'],
      'T2 flag/str');
  check(back.color == parseColor(vectors['color'] as String), 'T2 color');
  check(back.hasInner() &&
      back.inner.name == 'kid' && back.inner.value == 42, 'T2 inner');
  final nums = vectors['nums'] as List;
  check(back.nums.length == nums.length &&
      back.nums[0] == nums[0] &&
      back.nums[1] == nums[1] &&
      back.nums[2] == nums[2], 'T2 nums');
  final tags = vectors['tags'] as List;
  check(back.tags.length == 2 && back.tags[0] == tags[0] &&
      back.tags[1] == tags[1], 'T2 tags');
  check(back.inners.length == 2 &&
      back.inners[0].name == 'x' && back.inners[1].value == -2,
      'T2 inners');
  check(back.hasOptI32() && back.optI32 == vectors['opt_i32'],
      'T2 opt_i32 presence');
  check(back.hasOptStr() && back.optStr == vectors['opt_str'],
      'T2 opt_str presence');
  final flags = vectors['flags'] as List;
  check(back.flags.length == 3 &&
      back.flags[0] == flags[0] &&
      back.flags[1] == flags[1] &&
      back.flags[2] == flags[2], 'T2 flags');
  check(back.hasI32() && !pb.Scalars3.create().hasI32(), 'T2 has/clear');
  final ob = pb1.Oneof1.fromBuffer(referenceOneof);
  check(ob.name == 'a' && ob.text == 'hi' &&
      ob.whichKind() == pb1.Oneof1_Kind.text, 'T2 oneof decode');
  final nested = pb1.Oneof1.create()
    ..name = 'n'
    ..inner = (pb1.Oneof1_Inner.create()..v = 7);
  check(pb1.Oneof1.fromBuffer(nested.writeToBuffer()).inner.v == 7,
      'T2 nested oneof member');

  // T3: empty encodes empty; explicit zero keeps presence.
  check(pb.Scalars3.create().writeToBuffer().isEmpty, 'T3 empty encodes empty');
  final zero = pb.Scalars3.create()..optI32 = 0;
  final zeroBack = pb.Scalars3.fromBuffer(zero.writeToBuffer());
  check(zeroBack.hasOptI32() &&
      zeroBack.optI32 == 0 && !zeroBack.hasOptStr(), 'T3 explicit zero');

  // T4: differential vs the pinned oracle.
  check(lockVersion() == oraclePin,
      'T4 oracle pin drifted (got ${lockVersion()}); see ORACLES.md');
  check(bytesEqual(buildOracle().writeToBuffer(), reference),
      'T4 oracle != reference');
  check(bytesEqual(buildFacade(vectors).writeToBuffer(), reference),
      'T4 facade != reference');
  final om = oracle.Scalars3.fromBuffer(wire);
  final fm = pb.Scalars3.fromBuffer(buildOracle().writeToBuffer());
  check(om.i32 == fm.i32 && om.i64 == fm.i64 &&
      om.u32 == fm.u32 && om.u64 == fm.u64, 'T4 ints');
  check(om.flag == fm.flag && om.str == fm.str &&
      om.color.value == fm.color.value, 'T4 flag/str/color');
  check(om.inner.name == fm.inner.name && om.optI32 == fm.optI32,
      'T4 inner/opt');
  check(om.nums.length == fm.nums.length &&
      om.inners.length == fm.inners.length, 'T4 repeated');
  check(om.hasOptI32() == fm.hasOptI32(), 'T4 optional presence agrees');
  check(bytesEqual(
      (oracle1.Oneof1()
        ..name = 'a'
        ..text = 'hi').writeToBuffer(), referenceOneof),
      'T4 oracle oneof != reference');

  // T5: one workflow, both libraries, identical results.
  (List<int> w, String s, int i, int n, String inner, bool hasOpt)
  workflow<M>(M Function() create, void Function(M) fill,
          List<int> Function(M) enc, M Function(List<int>) dec,
          (String, int, int, String, bool) Function(M) get) {
    final m = create();
    fill(m);
    final bytes = enc(m);
    final g = get(dec(bytes));
    return (bytes, g.$1, g.$2, g.$3, g.$4, g.$5);
  }

  final a = workflow(
    () => oracle.Scalars3(),
    (m) {
      m.str = 'kid';
      m.i32 = -5;
      m.nums.addAll([1, 2]);
      m.inner = oracle.Inner3(name: 'x', value: 1);
    },
    (m) => m.writeToBuffer(),
    (b) => oracle.Scalars3.fromBuffer(b),
    (m) => (m.str, m.i32, m.nums.length, m.inner.name, m.hasOptI32()),
  );
  final f = workflow(
    () => pb.Scalars3.create(),
    (m) {
      m.str = 'kid';
      m.i32 = -5;
      m.nums.addAll([1, 2]);
      m.inner = pb.Inner3(name: 'x', value: 1);
    },
    (m) => m.writeToBuffer(),
    (b) => pb.Scalars3.fromBuffer(b),
    (m) => (m.str, m.i32, m.nums.length, m.inner.name, m.hasOptI32()),
  );
  check(bytesEqual(a.$1, f.$1) &&
      a.$2 == f.$2 && a.$3 == f.$3 && a.$4 == f.$4 &&
      a.$5 == f.$5 && a.$6 == f.$6, 'T5 mismatch');

  // T6: loud errors, not crashes.
  check(throws(() => pb.Scalars3.fromBuffer([0xff, 0xff, 0xff])),
      'T6 bad wire fails');
  check(throws(() => oracle.Scalars3.fromBuffer([0xff, 0xff, 0xff])),
      'T6 oracle bad wire fails');
  check(pb.Scalars3.fromBuffer([]).writeToBuffer().isEmpty &&
      oracle.Scalars3.fromBuffer([]).writeToBuffer().isEmpty,
      'T6 empty decodes to default');
  final multi = pb1.Oneof1.create()
    ..num = 1
    ..text = 'hi';
  check(multi.whichKind() == pb1.Oneof1_Kind.text && !multi.hasNum(),
      'T6 oneof keeps the last member');
  check(pb1.Oneof1.fromBuffer(multi.writeToBuffer()).text == 'hi',
      'T6 oneof member round-trips');
  final oMulti = oracle1.Oneof1()
    ..num = 1
    ..text = 'hi';
  check(oMulti.whichKind() == oracle1.Oneof1_Kind.text,
      'T6 oracle oneof keeps the last member');
  final opt = pb.Scalars3.create()..optI32 = 5;
  check(opt.hasOptI32(), 'T6 has tracks set');
  opt.clearOptI32();
  check(!opt.hasOptI32() && opt.writeToBuffer().isEmpty, 'T6 clear drops');

  print('typed_test: $checks checks passed');
}

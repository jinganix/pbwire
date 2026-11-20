// Dart binding benchmark against the official protobuf Dart runtime and
// generated code on the same matrix3.proto and the four standard tiers from
// BENCHMARKS.md.
//
// The same payload as the C/Go/Java/Rust rows: every scalar kind, the nested
// message, the two proto3 optionals, and the repeated fields scaled x0 / x1 /
// x100 / x3000, which lands the tiers on 136 / 174 / 2469 / 74913 bytes. All
// three encoding paths must produce identical bytes before anything is timed.
// Two pbwire rows: `pbwire-dart-tree` is the prebuilt `Message` handle
// (`encode()` / `decode()`), and `pbwire-dart-shim` is the generated flat
// `Struct` image read in place by the C reader (one FFI call each).
//
//   ./bench.sh          generate, build, run

import 'dart:ffi' as ffi;
import 'dart:io';

import 'package:ffi/ffi.dart';
import 'package:fixnum/fixnum.dart';
import 'package:pbwire/pbwire.dart';

import 'gen/matrix3.dart' as model;
import 'gen/matrix3.pb.dart' as proto;

const _cases = ['small', 'medium', 'large', 'xlarge'];
const _repeats = [0, 1, 100, 3000];
const _want = [136, 174, 2469, 74913];
/// Floor iterations per tier; doubled until one pass takes 0.25 s.
const _iterations = [50000, 50000, 5000, 500];
const _minPassSeconds = 0.25;
const _passes = 3;

var _failures = 0;

void main(List<String> args) {
  if (args.length != 1) {
    stderr.writeln('usage: bench <descriptor-set>');
    exit(2);
  }
  final schema = Schema.load(args[0]);
  final ty = schema.message('structtest.Scalars3');
  final innerTy = schema.message('structtest.Inner3');

  // The shim arm: the same payload as a flat layout image, one FFI call per op.
  final layoutSchema = PbwLayoutSchema.load(args[0]);
  // Children register first: the parent's message row resolves it by name.
  layoutSchema.type(
      'structtest.Inner3', model.inner3LayoutRows, ffi.sizeOf<model.Inner3Layout>());
  final imageType = layoutSchema.type('structtest.Scalars3',
      model.scalars3LayoutRows, ffi.sizeOf<model.Scalars3Layout>());
  final imageArena = Arena();

  for (var tier = 0; tier < _cases.length; tier++) {
    final label = _cases[tier];
    final n = _repeats[tier];
    final count = _iterations[tier];

    final reference = _fillOfficial(n).writeToBuffer();
    if (reference.length != _want[tier]) {
      _fail(label, 'official encoder wrote ${reference.length} bytes, expected ${_want[tier]}');
      continue;
    }
    final message = _fillPbwire(ty, innerTy, n);
    final got = message.encode();
    if (!_equals(got, reference)) {
      _fail(label, 'pbwire-dart wrote ${got.length} bytes, protobuf-dart ${reference.length}');
      continue;
    }
    final image = _fillLayout(imageArena, n);
    final shimBytes = PbwireLayout.encode(image, imageType);
    if (!_equals(shimBytes, reference)) {
      _fail(label, 'pbwire-dart-shim wrote ${shimBytes.length} bytes, protobuf-dart ${reference.length}');
      continue;
    }
    // Decode must round-trip byte-exactly and expose the same values.
    final back = ty.decode(reference);
    final backInner = back.getMessage('inner');
    final backOk = _equals(back.encode(), reference) &&
        back.getString('str') == 'hello' &&
        backInner?.getString('name') == 'kid' &&
        back.count('nums') == n &&
        (n == 0 || back.at('nums', n - 1)?.asInt() == n - 4);
    back.close();
    if (!backOk) {
      _fail(label, 'pbwire-dart decode differs');
      message.close();
      continue;
    }
    stdout.writeln('OK   $label encoding parity, x$n (${reference.length} bytes)');

    final official = _fillOfficial(n);
    // Tier line shows the reference op's count; slower ops keep the floor.
    final tierCount = _calibrate(() => message.encode(), count);
    final encode = _measure(count, () => message.encode());
    final decode = _measure(count, () => ty.decode(reference)..close());
    final out = calloc<ffi.Uint8>(ffi.sizeOf<model.Scalars3Layout>()).cast<model.Scalars3Layout>();
    final decodeArena = PbwArena(reference.length);
    double shimEncode;
    double shimDecode;
    try {
      // Same count as the tree row, so the two codec rows share a loop shape.
      shimEncode = _measure(count, () => PbwireLayout.encode(image, imageType));
      shimDecode = _measure(count, () {
        PbwireLayout.decodeInto(reference, out, imageType, decodeArena);
      });
    } finally {
      decodeArena.release();
      calloc.free(out);
    }
    // The decoded image must expose the same fields the tree decode checks.
    final officialEncode = _measure(count, () => official.writeToBuffer());
    final officialDecode = _measure(count, () => proto.Scalars3.fromBuffer(reference));

    stdout.writeln('$label: ${reference.length} bytes, $tierCount iterations (repeat field x$n)');
    stdout.writeln('  ${'impl'.padRight(16)} ${'op'.padRight(7)} '
        '${'ns/op'.padLeft(12)} ${'MB/s'.padLeft(10)}');
    _row('pbwire-dart-tree', 'encode', reference.length, encode);
    _row('pbwire-dart-shim', 'encode', reference.length, shimEncode);
    _row('protobuf-dart', 'encode', reference.length, officialEncode);
    _row('pbwire-dart-tree', 'decode', reference.length, decode);
    _row('pbwire-dart-shim', 'decode', reference.length, shimDecode);
    _row('protobuf-dart', 'decode', reference.length, officialDecode);
    stdout.writeln();
    message.close();
  }

  imageArena.releaseAll();
  layoutSchema.close();
  schema.close();
  if (_failures != 0) {
    stderr.writeln('$_failures check(s) failed');
    exit(1);
  }
}

proto.Scalars3 _fillOfficial(int n) => proto.Scalars3(
      i32: -2147483648,
      i64: Int64(9223372036854775807),
      u32: 4294967295,
      u64: Int64(-1),
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
      color: proto.Color3.COLOR3_GREEN,
      inner: proto.Inner3(name: 'kid', value: 42),
      optI32: -7,
      optStr: 'opt',
      nums: [for (var i = 0; i < n; i++) i - 3],
      tags: [for (var i = 0; i < n; i++) 'tag'],
      inners: [for (var i = 0; i < n; i++) proto.Inner3(name: 'grandkid', value: i + 1)],
      flags: [for (var i = 0; i < n; i++) i % 2 == 0],
    );

Message _fillPbwire(Type ty, Type innerTy, int n) {
  final name = innerTy.field('name');
  final value = innerTy.field('value');
  final msg = ty.newMessage()
    ..setInt('i32', -2147483648)
    ..setInt('i64', 9223372036854775807)
    ..setInt('u32', 4294967295)
    ..setInt('u64', -1)
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

  msg.setMessage('inner').setString(name, 'kid').setInt(value, 42);
  for (var i = 0; i < n; i++) {
    msg
      ..setInt('nums', i - 3)
      ..setString('tags', 'tag')
      ..setBool('flags', i % 2 == 0);
    msg.setMessage('inners').setString(name, 'grandkid').setInt(value, i + 1);
  }
  return msg;
}

/// The shim arm's native object: one Scalars3 flat image, with its strings and
/// arrays allocated in [arena] (the image is reused across the timed calls).
ffi.Pointer<model.Scalars3Layout> _fillLayout(Arena arena, int n) {
  final image = arena
      .allocate<ffi.Uint8>(ffi.sizeOf<model.Scalars3Layout>())
      .cast<model.Scalars3Layout>();
  image.ref
    ..i32 = -2147483648
    ..i64 = 9223372036854775807
    ..u32 = 4294967295
    ..u64 = -1 // 18446744073709551615 as the raw 64-bit pattern
    ..s32 = -2147483648
    ..s64 = -9223372036854775808
    ..fx32 = 3735928559
    ..fx64 = 72623859790382856
    ..sfx32 = -2147483648
    ..sfx64 = -9223372036854775808
    ..f32 = double.infinity
    ..f64 = -123456.789
    ..flag = true
    ..str = 'hello'.toNativeUtf8(allocator: arena)
    ..color = 2
    ..optI32 = -7
    ..hasOptI32 = 1
    ..optStr = 'opt'.toNativeUtf8(allocator: arena);
  final inner = arena
      .allocate<ffi.Uint8>(ffi.sizeOf<model.Inner3Layout>())
      .cast<model.Inner3Layout>();
  inner.ref
    ..name = 'kid'.toNativeUtf8(allocator: arena)
    ..value = 42;
  image.ref.inner = inner;
  final nums = arena.allocate<ffi.Int32>(n * ffi.sizeOf<ffi.Int32>());
  final tags = arena.allocate<ffi.Pointer<Utf8>>(n * ffi.sizeOf<ffi.IntPtr>());
  final inners = arena
      .allocate<ffi.Uint8>(n * ffi.sizeOf<model.Inner3Layout>())
      .cast<model.Inner3Layout>();
  final flags = arena.allocate<ffi.Bool>(n);
  for (var i = 0; i < n; i++) {
    nums[i] = i - 3;
    tags[i] = 'tag'.toNativeUtf8(allocator: arena);
    inners[i]
      ..name = 'grandkid'.toNativeUtf8(allocator: arena)
      ..value = i + 1;
    flags[i] = i % 2 == 0;
  }
  image.ref
    ..nums = nums
    ..numsCount = n
    ..tags = tags
    ..tagsCount = n
    ..inners = inners
    ..innersCount = n
    ..flags = flags
    ..flagsCount = n;
  return image;
}

double _run(int iterations, void Function() op) {
  final watch = Stopwatch()..start();
  for (var i = 0; i < iterations; i++) {
    op();
  }
  return watch.elapsedMicroseconds * 1000.0 / iterations;
}

/// Double from the tier floor until one pass takes [_minPassSeconds].
int _calibrate(void Function() op, int floor) {
  var count = floor > 0 ? floor : 1;
  for (;;) {
    final watch = Stopwatch()..start();
    for (var i = 0; i < count; i++) {
      op();
    }
    if (watch.elapsedMicroseconds / 1e6 >= _minPassSeconds || count >= 100000000) {
      return count;
    }
    count *= 2;
  }
}

/// Every op calibrates independently from the floor (a slow op already
/// exceeds the minimum at the floor and keeps it). Warm up, then keep the
/// best of [_passes] runs.
double _measure(int floor, void Function() op) {
  final count = _calibrate(op, floor);
  _run(count, op);
  var best = double.maxFinite;
  for (var pass = 0; pass < _passes; pass++) {
    final ns = _run(count, op);
    if (ns < best) {
      best = ns;
    }
  }
  return best;
}

void _row(String impl, String op, int bytes, double ns) {
  stdout.writeln('  ${impl.padRight(16)} ${op.padRight(7)} '
      '${ns.toStringAsFixed(1).padLeft(12)} ${(bytes / ns * 1000.0).toStringAsFixed(1).padLeft(10)}');
}

void _fail(String label, String what) {
  stdout.writeln('FAIL $label: $what');
  _failures++;
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

// The `shim` arm's binding self-check (plan 06): fill the generated
// Scalars3Layout flat image, encode it with one FFI call, compare the bytes
// with the pinned protoc oracle (matrix3.bin), decode the same bytes back into
// another flat image, compare field by field, then re-encode and compare again.
//
//   dart run tests/layout_test.dart [descriptor-set]

import 'dart:ffi';
import 'dart:io';

import 'package:ffi/ffi.dart';
import 'package:pbwire/pbwire.dart';

import 'gen/matrix3.dart';

var _checks = 0;

void check(bool ok, String what) {
  _checks++;
  if (!ok) {
    stderr.writeln('FAIL $what');
    exit(1);
  }
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

/// The native object of the shim arm: the Scalars3 image filled from
/// matrix3.txt. Strings and arrays live in [arena], exactly where a user's
/// native object would keep them.
Pointer<Scalars3Layout> _image(Arena arena, {bool optional = true}) {
  final image = arena.allocate<Uint8>(sizeOf<Scalars3Layout>()).cast<Scalars3Layout>();
  image.ref
    ..i32 = -2147483648
    ..i64 = 9223372036854775807
    ..u32 = 4294967295
    ..u64 = -1 // 18446744073709551615: the raw 64-bit pattern
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
    ..color = 2;

  final inner = arena.allocate<Uint8>(sizeOf<Inner3Layout>()).cast<Inner3Layout>();
  inner.ref
    ..name = 'kid'.toNativeUtf8(allocator: arena)
    ..value = 42;
  image.ref.inner = inner;

  final nums = arena.allocate<Int32>(3 * sizeOf<Int32>());
  nums[0] = 1;
  nums[1] = -5;
  nums[2] = 300;
  image.ref
    ..nums = nums
    ..numsCount = 3;

  final tags = arena.allocate<Pointer<Utf8>>(2 * sizeOf<IntPtr>());
  tags[0] = 'a'.toNativeUtf8(allocator: arena);
  tags[1] = 'bb'.toNativeUtf8(allocator: arena);
  image.ref
    ..tags = tags
    ..tagsCount = 2;

  final inners = arena.allocate<Uint8>(2 * sizeOf<Inner3Layout>()).cast<Inner3Layout>();
  inners[0]
    ..name = 'x'.toNativeUtf8(allocator: arena)
    ..value = 1;
  inners[1]
    ..name = 'y'.toNativeUtf8(allocator: arena)
    ..value = -2;
  image.ref
    ..inners = inners
    ..innersCount = 2;

  if (optional) {
    image.ref
      ..optI32 = -7
      ..hasOptI32 = 1
      ..optStr = 'opt'.toNativeUtf8(allocator: arena);
  }

  final flags = arena.allocate<Bool>(3);
  flags[0] = true;
  flags[1] = false;
  flags[2] = true;
  image.ref
    ..flags = flags
    ..flagsCount = 3;
  return image;
}

void main(List<String> args) {
  final desc = args.isNotEmpty ? args[0] : '../c/tests/data/matrix3.desc';
  final oracle =
      File(File(desc).parent.path + '/matrix3.bin').readAsBytesSync();

  final layoutSchema = PbwLayoutSchema.load(desc);
  // Children first: a message row resolves its sub-layout by proto full name.
  final innerType = layoutSchema.type(
      'structtest.Inner3', inner3LayoutRows, sizeOf<Inner3Layout>());
  final type = layoutSchema.type(
      'structtest.Scalars3', scalars3LayoutRows, sizeOf<Scalars3Layout>());
  check(identical(
      layoutSchema.type('structtest.Scalars3', scalars3LayoutRows,
          sizeOf<Scalars3Layout>()),
      type), 'one type per message');
  check(identical(
      layoutSchema.type('structtest.Inner3', inner3LayoutRows,
          sizeOf<Inner3Layout>()),
      innerType), 'registered once');

  final arena = Arena();
  final image = _image(arena);
  final wire = PbwireLayout.encode(image, type);
  check(_equals(wire, oracle),
      'encode: ${wire.length} bytes, oracle ${oracle.length}');
  check(!identical(PbwireLayout.encode(image, type), wire),
      'encode returns a fresh list each call');

  final out = calloc<Uint8>(sizeOf<Scalars3Layout>()).cast<Scalars3Layout>();
  final decodeArena = PbwArena(oracle.length);
  try {
    PbwireLayout.decodeInto(oracle, out, type, decodeArena);
    final inner = out.ref.inner;
    check(out.ref.i32 == -2147483648, 'decode i32');
    check(out.ref.i64 == 9223372036854775807, 'decode i64');
    check(out.ref.u64 == -1, 'decode u64 raw bits');
    check(out.ref.fx64 == 72623859790382856, 'decode fx64');
    check(out.ref.f32 == double.infinity, 'decode f32');
    check(out.ref.f64 == -123456.789, 'decode f64');
    check(out.ref.flag, 'decode flag');
    check(out.ref.str.toDartString() == 'hello', 'decode str');
    check(out.ref.color == 2, 'decode enum');
    check(inner.ref.name.toDartString() == 'kid' && inner.ref.value == 42,
        'decode inner');
    check(out.ref.numsCount == 3 && out.ref.nums[0] == 1 && out.ref.nums[2] == 300,
        'decode nums');
    check(out.ref.tagsCount == 2 &&
        out.ref.tags[0].toDartString() == 'a' &&
        out.ref.tags[1].toDartString() == 'bb', 'decode tags');
    check(out.ref.innersCount == 2 &&
        out.ref.inners[1].name.toDartString() == 'y' &&
        out.ref.inners[1].value == -2, 'decode inners');
    check(out.ref.optI32 == -7 && out.ref.hasOptI32 == 1, 'decode opt_i32 presence');
    check(out.ref.optStr.toDartString() == 'opt', 'decode opt_str');
    check(out.ref.flagsCount == 3 && out.ref.flags[0] && !out.ref.flags[1],
        'decode flags');
    final again = PbwireLayout.encode(out, type);
    check(_equals(again, oracle), 'decoded image re-encodes to the oracle bytes');

    // Absent proto3 optional: the has flag and the value stay zero and the
    // encoder writes no opt_i32 tag.
    final plain = PbwireLayout.encode(_image(arena, optional: false), type);
    final plainOut = calloc<Uint8>(sizeOf<Scalars3Layout>()).cast<Scalars3Layout>();
    final plainArena = PbwArena(plain.length);
    try {
      PbwireLayout.decodeInto(plain, plainOut, type, plainArena);
      check(plainOut.ref.hasOptI32 == 0 && plainOut.ref.optI32 == 0,
          'absent opt_i32 stays absent');
      check(plainOut.ref.optStr == nullptr, 'absent opt_str stays absent');
      // opt_i32 (-7) is a 2-byte tag plus a 10-byte varint; opt_str is a
      // 2-byte tag, a 1-byte length and 3 bytes.
      check(plain.length == oracle.length - 18,
          'absent optionals shorten the wire (${plain.length} vs ${oracle.length})');
    } finally {
      plainArena.release();
      calloc.free(plainOut);
    }
  } finally {
    decodeArena.release();
    calloc.free(out);
    arena.releaseAll();
    layoutSchema.close();
  }

  stdout.writeln('layout: OK ($_checks checks)');
}

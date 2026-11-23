// The one payload every Dart pbench arm times, plus the handles the arms and
// the gate share: the shim's flat `Scalars3Layout` image, the tree arm's
// `Message` handle, the pure core's `core.Msg` tree and the official protobuf
// package's message.
//
// Values are bindings/c/tests/data/matrix3.txt; the repeats scale the three
// repeated fields so the four tiers land on the standard wire sizes.

import 'dart:ffi';
import 'dart:io';

import 'package:ffi/ffi.dart';
import 'package:fixnum/fixnum.dart' as fixnum;
import 'package:pbwire/core/schema.dart' as core;
import 'package:pbwire/pbwire.dart';

import 'gen/matrix3.dart' as pbw;
import 'gen/matrix3.pb.dart' as proto;

/// One standard tier: label, repeated-element count, wire bytes, iterations.
final class Tier {
  const Tier(this.label, this.repeats, this.bytes, this.iterations);

  final String label;
  final int repeats;
  final int bytes;
  final int iterations;
}

const tiers = <Tier>[
  Tier('small', 0, 136, 50000),
  Tier('medium', 1, 174, 50000),
  Tier('large', 100, 2469, 5000),
  Tier('xlarge', 3000, 74913, 500),
];

/// The payload's 64-bit values, parsed once: a per-op BigInt.parse of ten
/// decimal strings would be measuring the parser, not the codec.
final BigInt _i64Max = BigInt.parse('9223372036854775807');
final BigInt _u64Max = BigInt.parse('18446744073709551615');
final BigInt _i64Min = BigInt.parse('-9223372036854775808');
final BigInt _fx64Value = BigInt.parse('72623859790382856');

/// The message fields every arm reads back after a decode: the gate compares
/// them field by field, so all three arms answer the same list.
const checkFields = <String>[
  'i32', 'i64', 'u32', 'u64', 's32', 's64', 'fx32', 'fx64', 'sfx32', 'sfx64',
  'f32', 'f64', 'flag', 'str', 'color', 'inner', 'nums', 'tags', 'inners',
  'opt_i32', 'opt_str', 'flags',
];

/// Owns the two schemas and their types for one pbench run.
final class Pbench {
  Pbench(String descriptor)
      : schema = Schema.load(descriptor),
        layoutSchema = PbwLayoutSchema.load(descriptor) {
    ty = schema.message('structtest.Scalars3');
    innerTy = schema.message('structtest.Inner3');
    coreType = core.Schema.fromBytes(
            File(descriptor).readAsBytesSync())
        .message('structtest.Scalars3');
    // Child first: a message row resolves its sub-layout by proto full name.
    innerType = layoutSchema.type(
        'structtest.Inner3', pbw.inner3LayoutRows, sizeOf<pbw.Inner3Layout>());
    imageType = layoutSchema.type('structtest.Scalars3',
        pbw.scalars3LayoutRows, sizeOf<pbw.Scalars3Layout>());
  }

  final Schema schema;
  final PbwLayoutSchema layoutSchema;
  late final Type ty;
  late final Type innerTy;
  late final core.MessageType coreType;
  late final PbwType innerType;
  late final PbwType imageType;

  /// A fresh flat image holding the native object, with its strings and arrays
  /// allocated in [arena] (the caller owns both).
  Pointer<pbw.Scalars3Layout> image(Arena arena, int repeats) {
    final image = arena
        .allocate<Uint8>(sizeOf<pbw.Scalars3Layout>())
        .cast<pbw.Scalars3Layout>();
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

    final inner =
        arena.allocate<Uint8>(sizeOf<pbw.Inner3Layout>()).cast<pbw.Inner3Layout>();
    inner.ref
      ..name = 'kid'.toNativeUtf8(allocator: arena)
      ..value = 42;
    image.ref.inner = inner;

    final nums = arena.allocate<Int32>(repeats * sizeOf<Int32>());
    final tags = arena
        .allocate<Pointer<Utf8>>(repeats * sizeOf<IntPtr>());
    final inners = arena
        .allocate<Uint8>(repeats * sizeOf<pbw.Inner3Layout>())
        .cast<pbw.Inner3Layout>();
    final flags = arena.allocate<Bool>(repeats);
    for (var i = 0; i < repeats; i++) {
      nums[i] = i - 3;
      tags[i] = 'tag'.toNativeUtf8(allocator: arena);
      inners[i]
        ..name = 'grandkid'.toNativeUtf8(allocator: arena)
        ..value = i + 1;
      flags[i] = i % 2 == 0;
    }
    image.ref
      ..nums = nums
      ..numsCount = repeats
      ..tags = tags
      ..tagsCount = repeats
      ..inners = inners
      ..innersCount = repeats
      ..flags = flags
      ..flagsCount = repeats;
    return image;
  }

  /// The tree arm's native object: a `Message` handle built before the timer,
  /// one `set` per field (the whole user-visible build).
  Message handle(int repeats) {
    final msg = ty.newMessage()
      ..setInt('i32', -2147483648)
      ..setInt('i64', 9223372036854775807)
      ..setInt('u32', 4294967295)
      ..setInt('u64', -1)
      ..setInt('s32', -2147483648)
      ..setInt('s64', -9223372036854775808)
      ..setInt('fx32', 3735928559)
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
    msg
        .setMessage('inner')
        .setString('name', 'kid')
        .setInt('value', 42);
    for (var i = 0; i < repeats; i++) {
      msg
        ..setInt('nums', i - 3)
        ..setString('tags', 'tag')
        ..setBool('flags', i % 2 == 0);
      msg.setMessage('inners').setString('name', 'grandkid').setInt('value', i + 1);
    }
    return msg;
  }

  /// The core arm's native object: a `core.Msg` tree built before the timer,
  /// one entry per field (the same whole user-visible build as [handle]).
  core.Msg coreMessage(int repeats) => core.fromObject(coreType.def, {
        'i32': -2147483648,
        'i64': _i64Max,
        'u32': 4294967295,
        'u64': _u64Max,
        's32': -2147483648,
        's64': _i64Min,
        'fx32': 3735928559,
        'fx64': _fx64Value,
        'sfx32': -2147483648,
        'sfx64': _i64Min,
        'f32': double.infinity,
        'f64': -123456.789,
        'flag': true,
        'str': 'hello',
        'color': 2,
        'opt_i32': -7,
        'opt_str': 'opt',
        'inner': <String, Object?>{'name': 'kid', 'value': 42},
        'nums': <Object?>[for (var i = 0; i < repeats; i++) i - 3],
        'tags': <Object?>[for (var i = 0; i < repeats; i++) 'tag'],
        'flags': <Object?>[for (var i = 0; i < repeats; i++) i % 2 == 0],
        'inners': <Object?>[
          for (var i = 0; i < repeats; i++)
            <String, Object?>{'name': 'grandkid', 'value': i + 1}
        ],
      });

  /// The official baseline's object: the same payload through protobuf-dart.
  proto.Scalars3 official(int repeats) => proto.Scalars3(
        i32: -2147483648,
        i64: fixnum.Int64(9223372036854775807),
        u32: 4294967295,
        u64: fixnum.Int64(-1),
        s32: -2147483648,
        s64: fixnum.Int64(-9223372036854775808),
        fx32: 3735928559,
        fx64: fixnum.Int64(72623859790382856),
        sfx32: -2147483648,
        sfx64: fixnum.Int64(-9223372036854775808),
        f32: double.infinity,
        f64: -123456.789,
        flag: true,
        str: 'hello',
        color: proto.Color3.COLOR3_GREEN,
        inner: proto.Inner3(name: 'kid', value: 42),
        optI32: -7,
        optStr: 'opt',
        nums: [for (var i = 0; i < repeats; i++) i - 3],
        tags: [for (var i = 0; i < repeats; i++) 'tag'],
        inners: [
          for (var i = 0; i < repeats; i++)
            proto.Inner3(name: 'grandkid', value: i + 1)
        ],
        flags: [for (var i = 0; i < repeats; i++) i % 2 == 0],
      );

  /// The official baseline's decode (static factory on the generated class).
  proto.Scalars3 officialFromBuffer(List<int> wire) =>
      proto.Scalars3.fromBuffer(wire);

  void close() {
    layoutSchema.close();
    schema.close();
  }
}

bool equals(List<int> a, List<int> b) {
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

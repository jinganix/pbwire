// Stage 1 gate for the pure-Dart core: the descriptor reader over the same
// FileDescriptorSet subset the C core reads (src/descriptor_set.c).
//
// Only facts the codec depends on: field count and order, the four presence
// kinds, repetition and packedness, kind mapping, oneofs, groups and the
// transitive required closure. Every assertion comes from a .desc fixture the
// C tests already pin, so the two readers cannot drift.
//
//   dart run tests/core/descriptor_test.dart [data dir]

import 'dart:io';
import 'dart:typed_data';

import 'package:pbwire/core/schema.dart';

import 'harness.dart';

late String data;

MessageType typeOf(String file, String name) {
  final bytes = File('$data/$file').readAsBytesSync();
  return Schema.fromBytes(bytes).message(name);
}

void main(List<String> args) {
  data = args.isNotEmpty
      ? args[0]
      : File.fromUri(Platform.script).parent.parent.parent.path +
          '/../c/tests/data';

  _matrix3();
  _matrix2();
  _grouptest();
  _oneoftest();
  _synthetic();

  done('core/descriptor_test');
}

void _matrix3() {
  final def = typeOf('matrix3.desc', 'structtest.Scalars3').def;
  check(fieldCount(def) == 22, 'matrix3.desc: 22 fields');
  check(def.fullName == 'structtest.Scalars3', 'matrix3.desc: full name');
  for (var i = 0; i < 22; i++) {
    check(def.fields[i].number == i + 1,
        'field ${i + 1} is numbered ${def.fields[i].number}');
  }
  check(def.fields[0].name == 'i32', 'first field is i32');
  check(def.fields[21].name == 'flags', 'last field is flags');

  // proto3 plain scalars have no has-bit...
  check(fieldByName(def, 'i32')!.presence == presenceImplicit, 'i32 implicit');
  check(fieldByName(def, 'str')!.presence == presenceImplicit, 'str implicit');
  // ...while `optional` in a proto3 file is explicit presence.
  check(fieldByName(def, 'opt_i32')!.presence == presenceExplicit,
      'opt_i32 explicit');
  check(fieldByName(def, 'opt_str')!.presence == presenceExplicit,
      'opt_str explicit');
  // ...and a message field always has it.
  check(fieldByName(def, 'inner')!.presence == presenceMessage,
      'inner message presence');

  final flags = fieldByName(def, 'flags')!;
  check(flags.repeated, 'flags repeated');
  check(flags.kind == K.bool_, 'flags is bool');
  check(flags.packed, 'proto3 repeated numerics default to packed');
  check(flags.wt == 2, 'a packed field encodes under the LEN tag');

  final nums = fieldByName(def, 'nums')!;
  check(nums.repeated, 'nums repeated');
  check(nums.kind == K.int32, 'nums is int32');
  check(nums.packed, 'nums packed');
  check(nums.elemWt == 0, 'the unpacked element form is still accepted');

  check(fieldByName(def, 'i64')!.kind == K.int64, 'i64');
  check(fieldByName(def, 'u64')!.kind == K.uint64, 'u64');
  check(fieldByName(def, 's64')!.kind == K.sint64, 's64');
  check(fieldByName(def, 'fx64')!.kind == K.fixed64, 'fx64');
  check(fieldByName(def, 'sfx64')!.kind == K.sfixed64, 'sfx64');
  check(fieldByName(def, 'f32')!.kind == K.float, 'f32');
  check(fieldByName(def, 'f64')!.kind == K.double_, 'f64');
  check(fieldByName(def, 'color')!.kind == K.enum_, 'color');
  check(fieldByName(def, 'inner')!.kind == K.message, 'inner');
  check(fieldByName(def, 'inner')!.msgType!.fullName == 'structtest.Inner3',
      'inner type');
  // proto3 strings verify UTF-8 on parse.
  check(fieldByName(def, 'str')!.utf8, 'proto3 str verifies utf8');
  // The enum is open (proto3), and its values are indexed by name.
  final color = fieldByName(def, 'color')!.enumType!;
  check(!color.closed, 'proc3 enum is open');
  check(color.byName['COLOR3_GREEN'] == 2, 'COLOR3_GREEN == 2');
  check(color.byNumber[2] == 'COLOR3_GREEN', 'enum value 2 is COLOR3_GREEN');
}

void _matrix2() {
  final type = typeOf('matrix2.desc', 'structtest.Scalars2');
  final def = type.def;
  check(fieldByName(def, 'id')!.presence == presenceRequired, 'proto2 required');
  check(def.hasRequired, 'Scalars2 has a required field');
  // Inner2.name is required, so Scalars2 is required transitively too.
  check(typeOf('matrix2.desc', 'structtest.Inner2').def.hasRequired,
      'Inner2 has a required field');
  check(def.deepRequired, 'Scalars2 is transitively required');

  // proto2 packs only with [packed = true].
  final nums = fieldByName(def, 'nums')!;
  check(nums.repeated && !nums.packed, 'proto2 nums is unpacked');
  check(fieldByName(def, 'packed_nums')!.packed, 'proto2 packed_nums is packed');
  // proto2 strings do not verify UTF-8.
  check(!fieldByName(def, 'str')!.utf8, 'proto2 str does not verify utf8');
}

void _grouptest() {
  final def = typeOf('grouptest.desc', 'structtest.GroupTest').def;
  final inner = fieldByName(def, 'inner')!;
  check(inner.kind == K.group, 'inner is a group');
  check(inner.wt == 3, 'start-group tag');
  check(inner.msgType!.fullName == 'structtest.GroupTest.Inner', 'group type');
  check(inner.tag.length == 1 && inner.tag[0] == 0x0b, 'start tag 0x0b');
  check(inner.tagEnd!.length == 1 && inner.tagEnd![0] == 0x0c,
      'end tag 0x0c');
  final item = fieldByName(def, 'item')!;
  check(item.kind == K.group, 'item is a group');
  check(item.repeated, 'item is repeated');
}

void _oneoftest() {
  final def = typeOf('oneoftest.desc', 'structtest.OneOfTest').def;
  check(def.oneofCount == 1, 'one oneof');
  check(def.oneofByName['kind'] == 0, 'oneof kind is slot 0');
  for (final name in ['num', 'text', 'sub', 'blob']) {
    final f = fieldByName(def, name)!;
    check(f.oneof == 0, '$name is a member of the oneof');
    check(f.presence == presenceExplicit, '$name has explicit presence');
  }
  check(fieldByName(def, 'id')!.oneof == -1, 'id is not in the oneof');
}

/// A hand-built FileDescriptorSet for a self-nesting proto3 message:
///   syntax = "proto3"; package deep; message Node { Node kid = 1; }
/// The depth test needs a recursive type and no toolchain, so the descriptor
/// is assembled here instead of shelling out to protoc.
void _synthetic() {
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
  final node =
      Schema.fromBytes(Uint8List.fromList(ld(1, file))).message('deep.Node');
  check(node.def.fields.length == 1, 'synthetic: one field');
  check(identical(node.def.fields[0].msgType, node.def),
      'the field points back at Node');
  check(!node.def.fields[0].utf8, 'a message field carries no utf8 flag');
  check(!node.def.hasRequired, 'proto3 message has no required field');
}

// This is a generated file - do not edit.
//
// Generated from matrix3.proto.

// @dart = 3.3

// ignore_for_file: annotate_overrides, camel_case_types, comment_references
// ignore_for_file: constant_identifier_names
// ignore_for_file: curly_braces_in_flow_control_structures
// ignore_for_file: deprecated_member_use_from_same_package, library_prefixes
// ignore_for_file: non_constant_identifier_names, prefer_relative_imports

import 'dart:core' as $core;

import 'package:fixnum/fixnum.dart' as $fixnum;
import 'package:protobuf/protobuf.dart' as $pb;

import 'matrix3.pbenum.dart';

export 'package:protobuf/protobuf.dart' show GeneratedMessageGenericExtensions;

export 'matrix3.pbenum.dart';

class Inner3 extends $pb.GeneratedMessage {
  factory Inner3({
    $core.String? name,
    $core.int? value,
  }) {
    final result = Inner3._();
    if (name != null) result.name = name;
    if (value != null) result.value = value;
    return result;
  }

  Inner3._();

  factory Inner3.fromBuffer($core.List<$core.int> data,
          [$pb.ExtensionRegistry registry = $pb.ExtensionRegistry.EMPTY]) =>
      Inner3()..mergeFromBuffer(data, registry);
  factory Inner3.fromJson($core.String json,
          [$pb.ExtensionRegistry registry = $pb.ExtensionRegistry.EMPTY]) =>
      Inner3()..mergeFromJson(json, registry);

  static final $pb.BuilderInfo _i = $pb.BuilderInfo(
      _omitMessageNames ? '' : 'Inner3',
      package: const $pb.PackageName(_omitMessageNames ? '' : 'structtest'),
      createEmptyInstance: Inner3.$_createMessage)
    ..aOS(1, _omitFieldNames ? '' : 'name')
    ..aI(2, _omitFieldNames ? '' : 'value')
    ..hasRequiredFields = false;

  @$core.Deprecated('See https://github.com/google/protobuf.dart/issues/998.')
  Inner3 clone() => deepCopy();
  @$core.Deprecated('See https://github.com/google/protobuf.dart/issues/998.')
  Inner3 copyWith(void Function(Inner3) updates) =>
      super.copyWith((message) => updates(message as Inner3)) as Inner3;

  @$core.override
  $pb.BuilderInfo get info_ => _i;

  @$core.pragma('dart2js:noInline')
  @$core.Deprecated('Use Inner3() / Inner3.new instead')
  static Inner3 create() => Inner3._();
  static $pb.GeneratedMessage $_createMessage() => Inner3._();
  @$core.override
  Inner3 createEmptyInstance() => Inner3._();
  @$core.pragma('dart2js:noInline')
  static Inner3 getDefault() => _defaultInstance ??=
      $pb.GeneratedMessage.$_defaultFor<Inner3>(Inner3.$_createMessage);
  static Inner3? _defaultInstance;

  @$pb.TagNumber(1)
  $core.String get name => $_getSZ(0);
  @$pb.TagNumber(1)
  set name($core.String value) => $_setString(0, value);
  @$pb.TagNumber(1)
  $core.bool hasName() => $_has(0);
  @$pb.TagNumber(1)
  void clearName() => $_clearField(1);

  @$pb.TagNumber(2)
  $core.int get value => $_getIZ(1);
  @$pb.TagNumber(2)
  set value($core.int value) => $_setSignedInt32(1, value);
  @$pb.TagNumber(2)
  $core.bool hasValue() => $_has(1);
  @$pb.TagNumber(2)
  void clearValue() => $_clearField(2);
}

class Scalars3 extends $pb.GeneratedMessage {
  factory Scalars3({
    $core.int? i32,
    $fixnum.Int64? i64,
    $core.int? u32,
    $fixnum.Int64? u64,
    $core.int? s32,
    $fixnum.Int64? s64,
    $core.int? fx32,
    $fixnum.Int64? fx64,
    $core.int? sfx32,
    $fixnum.Int64? sfx64,
    $core.double? f32,
    $core.double? f64,
    $core.bool? flag,
    $core.String? str,
    Color3? color,
    Inner3? inner,
    $core.Iterable<$core.int>? nums,
    $core.Iterable<$core.String>? tags,
    $core.Iterable<Inner3>? inners,
    $core.int? optI32,
    $core.String? optStr,
    $core.Iterable<$core.bool>? flags,
  }) {
    final result = Scalars3._();
    if (i32 != null) result.i32 = i32;
    if (i64 != null) result.i64 = i64;
    if (u32 != null) result.u32 = u32;
    if (u64 != null) result.u64 = u64;
    if (s32 != null) result.s32 = s32;
    if (s64 != null) result.s64 = s64;
    if (fx32 != null) result.fx32 = fx32;
    if (fx64 != null) result.fx64 = fx64;
    if (sfx32 != null) result.sfx32 = sfx32;
    if (sfx64 != null) result.sfx64 = sfx64;
    if (f32 != null) result.f32 = f32;
    if (f64 != null) result.f64 = f64;
    if (flag != null) result.flag = flag;
    if (str != null) result.str = str;
    if (color != null) result.color = color;
    if (inner != null) result.inner = inner;
    if (nums != null) result.nums.addAll(nums);
    if (tags != null) result.tags.addAll(tags);
    if (inners != null) result.inners.addAll(inners);
    if (optI32 != null) result.optI32 = optI32;
    if (optStr != null) result.optStr = optStr;
    if (flags != null) result.flags.addAll(flags);
    return result;
  }

  Scalars3._();

  factory Scalars3.fromBuffer($core.List<$core.int> data,
          [$pb.ExtensionRegistry registry = $pb.ExtensionRegistry.EMPTY]) =>
      Scalars3()..mergeFromBuffer(data, registry);
  factory Scalars3.fromJson($core.String json,
          [$pb.ExtensionRegistry registry = $pb.ExtensionRegistry.EMPTY]) =>
      Scalars3()..mergeFromJson(json, registry);

  static final $pb.BuilderInfo _i = $pb.BuilderInfo(
      _omitMessageNames ? '' : 'Scalars3',
      package: const $pb.PackageName(_omitMessageNames ? '' : 'structtest'),
      createEmptyInstance: Scalars3.$_createMessage)
    ..aI(1, _omitFieldNames ? '' : 'i32')
    ..aInt64(2, _omitFieldNames ? '' : 'i64')
    ..aI(3, _omitFieldNames ? '' : 'u32', fieldType: $pb.PbFieldType.OU3)
    ..a<$fixnum.Int64>(4, _omitFieldNames ? '' : 'u64', $pb.PbFieldType.OU6,
        defaultOrMaker: $fixnum.Int64.ZERO)
    ..aI(5, _omitFieldNames ? '' : 's32', fieldType: $pb.PbFieldType.OS3)
    ..a<$fixnum.Int64>(6, _omitFieldNames ? '' : 's64', $pb.PbFieldType.OS6,
        defaultOrMaker: $fixnum.Int64.ZERO)
    ..aI(7, _omitFieldNames ? '' : 'fx32', fieldType: $pb.PbFieldType.OF3)
    ..a<$fixnum.Int64>(8, _omitFieldNames ? '' : 'fx64', $pb.PbFieldType.OF6,
        defaultOrMaker: $fixnum.Int64.ZERO)
    ..aI(9, _omitFieldNames ? '' : 'sfx32', fieldType: $pb.PbFieldType.OSF3)
    ..a<$fixnum.Int64>(10, _omitFieldNames ? '' : 'sfx64', $pb.PbFieldType.OSF6,
        defaultOrMaker: $fixnum.Int64.ZERO)
    ..aD(11, _omitFieldNames ? '' : 'f32', fieldType: $pb.PbFieldType.OF)
    ..aD(12, _omitFieldNames ? '' : 'f64')
    ..aOB(13, _omitFieldNames ? '' : 'flag')
    ..aOS(14, _omitFieldNames ? '' : 'str')
    ..aE<Color3>(15, _omitFieldNames ? '' : 'color', enumValues: Color3.values)
    ..aOM<Inner3>(16, _omitFieldNames ? '' : 'inner',
        subBuilder: Inner3.$_createMessage)
    ..p<$core.int>(17, _omitFieldNames ? '' : 'nums', $pb.PbFieldType.K3)
    ..pPS(18, _omitFieldNames ? '' : 'tags')
    ..pPM<Inner3>(19, _omitFieldNames ? '' : 'inners',
        subBuilder: Inner3.$_createMessage)
    ..aI(20, _omitFieldNames ? '' : 'optI32')
    ..aOS(21, _omitFieldNames ? '' : 'optStr')
    ..p<$core.bool>(22, _omitFieldNames ? '' : 'flags', $pb.PbFieldType.KB)
    ..hasRequiredFields = false;

  @$core.Deprecated('See https://github.com/google/protobuf.dart/issues/998.')
  Scalars3 clone() => deepCopy();
  @$core.Deprecated('See https://github.com/google/protobuf.dart/issues/998.')
  Scalars3 copyWith(void Function(Scalars3) updates) =>
      super.copyWith((message) => updates(message as Scalars3)) as Scalars3;

  @$core.override
  $pb.BuilderInfo get info_ => _i;

  @$core.pragma('dart2js:noInline')
  @$core.Deprecated('Use Scalars3() / Scalars3.new instead')
  static Scalars3 create() => Scalars3._();
  static $pb.GeneratedMessage $_createMessage() => Scalars3._();
  @$core.override
  Scalars3 createEmptyInstance() => Scalars3._();
  @$core.pragma('dart2js:noInline')
  static Scalars3 getDefault() => _defaultInstance ??=
      $pb.GeneratedMessage.$_defaultFor<Scalars3>(Scalars3.$_createMessage);
  static Scalars3? _defaultInstance;

  @$pb.TagNumber(1)
  $core.int get i32 => $_getIZ(0);
  @$pb.TagNumber(1)
  set i32($core.int value) => $_setSignedInt32(0, value);
  @$pb.TagNumber(1)
  $core.bool hasI32() => $_has(0);
  @$pb.TagNumber(1)
  void clearI32() => $_clearField(1);

  @$pb.TagNumber(2)
  $fixnum.Int64 get i64 => $_getI64(1);
  @$pb.TagNumber(2)
  set i64($fixnum.Int64 value) => $_setInt64(1, value);
  @$pb.TagNumber(2)
  $core.bool hasI64() => $_has(1);
  @$pb.TagNumber(2)
  void clearI64() => $_clearField(2);

  @$pb.TagNumber(3)
  $core.int get u32 => $_getIZ(2);
  @$pb.TagNumber(3)
  set u32($core.int value) => $_setUnsignedInt32(2, value);
  @$pb.TagNumber(3)
  $core.bool hasU32() => $_has(2);
  @$pb.TagNumber(3)
  void clearU32() => $_clearField(3);

  @$pb.TagNumber(4)
  $fixnum.Int64 get u64 => $_getI64(3);
  @$pb.TagNumber(4)
  set u64($fixnum.Int64 value) => $_setInt64(3, value);
  @$pb.TagNumber(4)
  $core.bool hasU64() => $_has(3);
  @$pb.TagNumber(4)
  void clearU64() => $_clearField(4);

  @$pb.TagNumber(5)
  $core.int get s32 => $_getIZ(4);
  @$pb.TagNumber(5)
  set s32($core.int value) => $_setSignedInt32(4, value);
  @$pb.TagNumber(5)
  $core.bool hasS32() => $_has(4);
  @$pb.TagNumber(5)
  void clearS32() => $_clearField(5);

  @$pb.TagNumber(6)
  $fixnum.Int64 get s64 => $_getI64(5);
  @$pb.TagNumber(6)
  set s64($fixnum.Int64 value) => $_setInt64(5, value);
  @$pb.TagNumber(6)
  $core.bool hasS64() => $_has(5);
  @$pb.TagNumber(6)
  void clearS64() => $_clearField(6);

  @$pb.TagNumber(7)
  $core.int get fx32 => $_getIZ(6);
  @$pb.TagNumber(7)
  set fx32($core.int value) => $_setUnsignedInt32(6, value);
  @$pb.TagNumber(7)
  $core.bool hasFx32() => $_has(6);
  @$pb.TagNumber(7)
  void clearFx32() => $_clearField(7);

  @$pb.TagNumber(8)
  $fixnum.Int64 get fx64 => $_getI64(7);
  @$pb.TagNumber(8)
  set fx64($fixnum.Int64 value) => $_setInt64(7, value);
  @$pb.TagNumber(8)
  $core.bool hasFx64() => $_has(7);
  @$pb.TagNumber(8)
  void clearFx64() => $_clearField(8);

  @$pb.TagNumber(9)
  $core.int get sfx32 => $_getIZ(8);
  @$pb.TagNumber(9)
  set sfx32($core.int value) => $_setSignedInt32(8, value);
  @$pb.TagNumber(9)
  $core.bool hasSfx32() => $_has(8);
  @$pb.TagNumber(9)
  void clearSfx32() => $_clearField(9);

  @$pb.TagNumber(10)
  $fixnum.Int64 get sfx64 => $_getI64(9);
  @$pb.TagNumber(10)
  set sfx64($fixnum.Int64 value) => $_setInt64(9, value);
  @$pb.TagNumber(10)
  $core.bool hasSfx64() => $_has(9);
  @$pb.TagNumber(10)
  void clearSfx64() => $_clearField(10);

  @$pb.TagNumber(11)
  $core.double get f32 => $_getN(10);
  @$pb.TagNumber(11)
  set f32($core.double value) => $_setFloat(10, value);
  @$pb.TagNumber(11)
  $core.bool hasF32() => $_has(10);
  @$pb.TagNumber(11)
  void clearF32() => $_clearField(11);

  @$pb.TagNumber(12)
  $core.double get f64 => $_getN(11);
  @$pb.TagNumber(12)
  set f64($core.double value) => $_setDouble(11, value);
  @$pb.TagNumber(12)
  $core.bool hasF64() => $_has(11);
  @$pb.TagNumber(12)
  void clearF64() => $_clearField(12);

  @$pb.TagNumber(13)
  $core.bool get flag => $_getBF(12);
  @$pb.TagNumber(13)
  set flag($core.bool value) => $_setBool(12, value);
  @$pb.TagNumber(13)
  $core.bool hasFlag() => $_has(12);
  @$pb.TagNumber(13)
  void clearFlag() => $_clearField(13);

  @$pb.TagNumber(14)
  $core.String get str => $_getSZ(13);
  @$pb.TagNumber(14)
  set str($core.String value) => $_setString(13, value);
  @$pb.TagNumber(14)
  $core.bool hasStr() => $_has(13);
  @$pb.TagNumber(14)
  void clearStr() => $_clearField(14);

  @$pb.TagNumber(15)
  Color3 get color => $_getN(14);
  @$pb.TagNumber(15)
  set color(Color3 value) => $_setField(15, value);
  @$pb.TagNumber(15)
  $core.bool hasColor() => $_has(14);
  @$pb.TagNumber(15)
  void clearColor() => $_clearField(15);

  @$pb.TagNumber(16)
  Inner3 get inner => $_getN(15);
  @$pb.TagNumber(16)
  set inner(Inner3 value) => $_setField(16, value);
  @$pb.TagNumber(16)
  $core.bool hasInner() => $_has(15);
  @$pb.TagNumber(16)
  void clearInner() => $_clearField(16);
  @$pb.TagNumber(16)
  Inner3 ensureInner() => $_ensure(15);

  @$pb.TagNumber(17)
  $pb.PbList<$core.int> get nums => $_getList(16);

  @$pb.TagNumber(18)
  $pb.PbList<$core.String> get tags => $_getList(17);

  @$pb.TagNumber(19)
  $pb.PbList<Inner3> get inners => $_getList(18);

  @$pb.TagNumber(20)
  $core.int get optI32 => $_getIZ(19);
  @$pb.TagNumber(20)
  set optI32($core.int value) => $_setSignedInt32(19, value);
  @$pb.TagNumber(20)
  $core.bool hasOptI32() => $_has(19);
  @$pb.TagNumber(20)
  void clearOptI32() => $_clearField(20);

  @$pb.TagNumber(21)
  $core.String get optStr => $_getSZ(20);
  @$pb.TagNumber(21)
  set optStr($core.String value) => $_setString(20, value);
  @$pb.TagNumber(21)
  $core.bool hasOptStr() => $_has(20);
  @$pb.TagNumber(21)
  void clearOptStr() => $_clearField(21);

  @$pb.TagNumber(22)
  $pb.PbList<$core.bool> get flags => $_getList(21);
}

const $core.bool _omitFieldNames =
    $core.bool.fromEnvironment('protobuf.omit_field_names');
const $core.bool _omitMessageNames =
    $core.bool.fromEnvironment('protobuf.omit_message_names');

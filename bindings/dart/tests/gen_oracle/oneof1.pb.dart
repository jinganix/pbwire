// This is a generated file - do not edit.
//
// Generated from oneof1.proto.

// @dart = 3.3

// ignore_for_file: annotate_overrides, camel_case_types, comment_references
// ignore_for_file: constant_identifier_names
// ignore_for_file: curly_braces_in_flow_control_structures
// ignore_for_file: deprecated_member_use_from_same_package, library_prefixes
// ignore_for_file: non_constant_identifier_names, prefer_relative_imports

import 'dart:core' as $core;

import 'package:protobuf/protobuf.dart' as $pb;

export 'package:protobuf/protobuf.dart' show GeneratedMessageGenericExtensions;

class Oneof1_Inner extends $pb.GeneratedMessage {
  factory Oneof1_Inner({
    $core.int? v,
  }) {
    final result = Oneof1_Inner._();
    if (v != null) result.v = v;
    return result;
  }

  Oneof1_Inner._();

  factory Oneof1_Inner.fromBuffer($core.List<$core.int> data,
          [$pb.ExtensionRegistry registry = $pb.ExtensionRegistry.EMPTY]) =>
      Oneof1_Inner()..mergeFromBuffer(data, registry);
  factory Oneof1_Inner.fromJson($core.String json,
          [$pb.ExtensionRegistry registry = $pb.ExtensionRegistry.EMPTY]) =>
      Oneof1_Inner()..mergeFromJson(json, registry);

  static final $pb.BuilderInfo _i = $pb.BuilderInfo(
      _omitMessageNames ? '' : 'Oneof1.Inner',
      package: const $pb.PackageName(_omitMessageNames ? '' : 'spectest'),
      createEmptyInstance: Oneof1_Inner.$_createMessage)
    ..aI(1, _omitFieldNames ? '' : 'v')
    ..hasRequiredFields = false;

  @$core.Deprecated('See https://github.com/google/protobuf.dart/issues/998.')
  Oneof1_Inner clone() => deepCopy();
  @$core.Deprecated('See https://github.com/google/protobuf.dart/issues/998.')
  Oneof1_Inner copyWith(void Function(Oneof1_Inner) updates) =>
      super.copyWith((message) => updates(message as Oneof1_Inner))
          as Oneof1_Inner;

  @$core.override
  $pb.BuilderInfo get info_ => _i;

  @$core.pragma('dart2js:noInline')
  @$core.Deprecated('Use Oneof1_Inner() / Oneof1_Inner.new instead')
  static Oneof1_Inner create() => Oneof1_Inner._();
  static $pb.GeneratedMessage $_createMessage() => Oneof1_Inner._();
  @$core.override
  Oneof1_Inner createEmptyInstance() => Oneof1_Inner._();
  @$core.pragma('dart2js:noInline')
  static Oneof1_Inner getDefault() =>
      _defaultInstance ??= $pb.GeneratedMessage.$_defaultFor<Oneof1_Inner>(
          Oneof1_Inner.$_createMessage);
  static Oneof1_Inner? _defaultInstance;

  @$pb.TagNumber(1)
  $core.int get v => $_getIZ(0);
  @$pb.TagNumber(1)
  set v($core.int value) => $_setSignedInt32(0, value);
  @$pb.TagNumber(1)
  $core.bool hasV() => $_has(0);
  @$pb.TagNumber(1)
  void clearV() => $_clearField(1);
}

enum Oneof1_Kind { num, text, inner, notSet }

class Oneof1 extends $pb.GeneratedMessage {
  factory Oneof1({
    $core.String? name,
    $core.int? num,
    $core.String? text,
    Oneof1_Inner? inner,
  }) {
    final result = Oneof1._();
    if (name != null) result.name = name;
    if (num != null) result.num = num;
    if (text != null) result.text = text;
    if (inner != null) result.inner = inner;
    return result;
  }

  Oneof1._();

  factory Oneof1.fromBuffer($core.List<$core.int> data,
          [$pb.ExtensionRegistry registry = $pb.ExtensionRegistry.EMPTY]) =>
      Oneof1()..mergeFromBuffer(data, registry);
  factory Oneof1.fromJson($core.String json,
          [$pb.ExtensionRegistry registry = $pb.ExtensionRegistry.EMPTY]) =>
      Oneof1()..mergeFromJson(json, registry);

  static const $core.Map<$core.int, Oneof1_Kind> _Oneof1_KindByTag = {
    2: Oneof1_Kind.num,
    3: Oneof1_Kind.text,
    4: Oneof1_Kind.inner,
    0: Oneof1_Kind.notSet
  };
  static final $pb.BuilderInfo _i = $pb.BuilderInfo(
      _omitMessageNames ? '' : 'Oneof1',
      package: const $pb.PackageName(_omitMessageNames ? '' : 'spectest'),
      createEmptyInstance: Oneof1.$_createMessage)
    ..oo(0, [2, 3, 4])
    ..aOS(1, _omitFieldNames ? '' : 'name')
    ..aI(2, _omitFieldNames ? '' : 'num')
    ..aOS(3, _omitFieldNames ? '' : 'text')
    ..aOM<Oneof1_Inner>(4, _omitFieldNames ? '' : 'inner',
        subBuilder: Oneof1_Inner.$_createMessage)
    ..hasRequiredFields = false;

  @$core.Deprecated('See https://github.com/google/protobuf.dart/issues/998.')
  Oneof1 clone() => deepCopy();
  @$core.Deprecated('See https://github.com/google/protobuf.dart/issues/998.')
  Oneof1 copyWith(void Function(Oneof1) updates) =>
      super.copyWith((message) => updates(message as Oneof1)) as Oneof1;

  @$core.override
  $pb.BuilderInfo get info_ => _i;

  @$core.pragma('dart2js:noInline')
  @$core.Deprecated('Use Oneof1() / Oneof1.new instead')
  static Oneof1 create() => Oneof1._();
  static $pb.GeneratedMessage $_createMessage() => Oneof1._();
  @$core.override
  Oneof1 createEmptyInstance() => Oneof1._();
  @$core.pragma('dart2js:noInline')
  static Oneof1 getDefault() => _defaultInstance ??=
      $pb.GeneratedMessage.$_defaultFor<Oneof1>(Oneof1.$_createMessage);
  static Oneof1? _defaultInstance;

  @$pb.TagNumber(2)
  @$pb.TagNumber(3)
  @$pb.TagNumber(4)
  Oneof1_Kind whichKind() => _Oneof1_KindByTag[$_whichOneof(0)]!;
  @$pb.TagNumber(2)
  @$pb.TagNumber(3)
  @$pb.TagNumber(4)
  void clearKind() => $_clearField($_whichOneof(0));

  @$pb.TagNumber(1)
  $core.String get name => $_getSZ(0);
  @$pb.TagNumber(1)
  set name($core.String value) => $_setString(0, value);
  @$pb.TagNumber(1)
  $core.bool hasName() => $_has(0);
  @$pb.TagNumber(1)
  void clearName() => $_clearField(1);

  @$pb.TagNumber(2)
  $core.int get num => $_getIZ(1);
  @$pb.TagNumber(2)
  set num($core.int value) => $_setSignedInt32(1, value);
  @$pb.TagNumber(2)
  $core.bool hasNum() => $_has(1);
  @$pb.TagNumber(2)
  void clearNum() => $_clearField(2);

  @$pb.TagNumber(3)
  $core.String get text => $_getSZ(2);
  @$pb.TagNumber(3)
  set text($core.String value) => $_setString(2, value);
  @$pb.TagNumber(3)
  $core.bool hasText() => $_has(2);
  @$pb.TagNumber(3)
  void clearText() => $_clearField(3);

  @$pb.TagNumber(4)
  Oneof1_Inner get inner => $_getN(3);
  @$pb.TagNumber(4)
  set inner(Oneof1_Inner value) => $_setField(4, value);
  @$pb.TagNumber(4)
  $core.bool hasInner() => $_has(3);
  @$pb.TagNumber(4)
  void clearInner() => $_clearField(4);
  @$pb.TagNumber(4)
  Oneof1_Inner ensureInner() => $_ensure(3);
}

const $core.bool _omitFieldNames =
    $core.bool.fromEnvironment('protobuf.omit_field_names');
const $core.bool _omitMessageNames =
    $core.bool.fromEnvironment('protobuf.omit_message_names');

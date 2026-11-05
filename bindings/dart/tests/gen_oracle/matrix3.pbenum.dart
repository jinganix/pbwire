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

import 'package:protobuf/protobuf.dart' as $pb;

class Color3 extends $pb.ProtobufEnum {
  static const Color3 COLOR3_UNSPECIFIED =
      Color3._(0, _omitEnumNames ? '' : 'COLOR3_UNSPECIFIED');
  static const Color3 COLOR3_RED =
      Color3._(1, _omitEnumNames ? '' : 'COLOR3_RED');
  static const Color3 COLOR3_GREEN =
      Color3._(2, _omitEnumNames ? '' : 'COLOR3_GREEN');

  static const $core.List<Color3> values = <Color3>[
    COLOR3_UNSPECIFIED,
    COLOR3_RED,
    COLOR3_GREEN,
  ];

  static final $core.List<Color3?> _byValue =
      $pb.ProtobufEnum.$_initByValueList(values, 2);
  static Color3? valueOf($core.int value) =>
      value < 0 || value >= _byValue.length ? null : _byValue[value];

  const Color3._(super.value, super.name);
}

const $core.bool _omitEnumNames =
    $core.bool.fromEnvironment('protobuf.omit_enum_names');

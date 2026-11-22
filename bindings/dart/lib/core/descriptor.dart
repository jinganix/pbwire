// FileDescriptorSet reader and schema builder for the pure-Dart core: the
// only reader of descriptor.proto the pure path needs. It parses just enough
// for a binary codec (names, numbers, labels, types, type names, oneofs,
// extensions, packedness, proto3 optional, editions features, syntax) and
// skips everything else by wire type, so unknown descriptor.proto additions
// never break loading.
//
// The wire layout knowledge lives here; `encode`/`decode` know the message
// wire layout and `schema` is the public facade. This mirrors
// bindings/java/runtime/Proto.java and bindings/node/lib/core/descriptor.ts.

import 'dart:typed_data';

import 'wire.dart';

/* ---------------- compiled schema (what encode/decode read) ---------------- */

/// Wire kinds of the table engine (its own numbering, not protobuf's).
abstract final class K {
  static const int int32 = 0;
  static const int uint32 = 1;
  static const int sint32 = 2;
  static const int int64 = 3;
  static const int uint64 = 4;
  static const int sint64 = 5;
  static const int bool_ = 6;
  static const int enum_ = 7;
  static const int fixed32 = 8;
  static const int sfixed32 = 9;
  static const int float = 10;
  static const int fixed64 = 11;
  static const int sfixed64 = 12;
  static const int double_ = 13;
  static const int string = 14;
  static const int bytes = 15;
  static const int message = 16;
  static const int group = 17;
}

/// Presence kinds. [presenceImplicit] = proto3 plain scalar (no has-bit).
const int presenceImplicit = 0;
const int presenceExplicit = 1;
const int presenceRequired = 2;
const int presenceMessage = 3;

/// The packable numeric kinds: packed runs cross every one of them.
bool isPackable(int kind) => kind <= K.double_;

final class EnumDef {
  EnumDef(this.fullName, this.byName, this.byNumber, this.closed);

  final String fullName;
  final Map<String, int> byName;
  final Map<int, String> byNumber;
  final bool closed;
}

final class Field {
  Field({
    required this.number,
    required this.name,
    required this.kind,
    required this.wt,
    required this.elemWt,
    required this.repeated,
    required this.packed,
    required this.oneof,
    required this.presence,
    required this.msgType,
    required this.enumType,
    required this.enumClosed,
    required this.utf8,
    required this.fullName,
    required this.tag,
    required this.tagEnd,
  });

  final int number;
  final String name;
  final int kind;

  /// Wire type of the declared form: LEN for packed numerics.
  final int wt;

  /// Wire type of a single element (what an unpacked occurrence uses).
  final int elemWt;
  final bool repeated;
  final bool packed;

  /// Oneof index, -1 when the field is not in a oneof.
  final int oneof;
  final int presence;
  final MsgDef? msgType;
  final EnumDef? enumType;
  final bool enumClosed;

  /// A decoded string must be valid UTF-8 (proto3/editions VERIFY).
  final bool utf8;

  /// Extensions only: the global name `[pkg.name]`.
  String? fullName;

  /// Precomputed tag bytes (start-group tag for a group).
  final Uint8List tag;

  /// Groups only: the end-group tag.
  final Uint8List? tagEnd;
}

final class MsgDef {
  MsgDef(this.fullName, this.mapEntry, this.reservedNames);

  final String fullName;
  List<Field> fields = const <Field>[];

  /// Dense number-to-slot map, or null when field numbers are sparse.
  Int32List? slotByFnum;
  Map<String, int> byName = const <String, int>{};
  Map<String, int> oneofByName = const <String, int>{};
  int oneofCount = 0;
  bool hasRequired = false;

  /// Some transitively reachable message has a REQUIRED field.
  bool deepRequired = false;

  /// protoc's synthesized map entry (field 1 key, field 2 value).
  final bool mapEntry;
  final List<String> reservedNames;
}

final class CoreSchema {
  final Map<String, MsgDef> messages = <String, MsgDef>{};
  final Map<String, EnumDef> enums = <String, EnumDef>{};

  MsgDef message(String fullName) {
    final def = messages[fullName];
    if (def == null) {
      throw CoreError('no such message type: $fullName');
    }
    return def;
  }
}

final class Limits {
  const Limits({this.maxBytes = 0, this.maxDepth = 0});

  /// Rejects inputs larger than this; 0 means no cap.
  final int maxBytes;

  /// Nesting cap; 0 means [maxDepth] (256). A larger value clamps to it.
  final int maxDepth;
}

const Limits defaultLimits = Limits();

int effectiveDepth(Limits? lim) {
  if (lim == null || lim.maxDepth == 0 || lim.maxDepth > maxDepth) {
    return maxDepth;
  }
  return lim.maxDepth;
}

void checkBytes(Limits? lim, int n, String what) {
  if (lim != null && lim.maxBytes > 0 && n > lim.maxBytes) {
    throw CoreError('$what exceeds the ${lim.maxBytes}-byte limit', 5);
  }
}

/* ---------------- descriptor.proto model ---------------- */

/* FieldDescriptorProto.Type values, kept numeric like the descriptor. */
const int tDouble = 1;
const int tFloat = 2;
const int tInt64 = 3;
const int tUint64 = 4;
const int tInt32 = 5;
const int tFixed64 = 6;
const int tFixed32 = 7;
const int tBool = 8;
const int tString = 9;
const int tGroup = 10;
const int tMessage = 11;
const int tBytes = 12;
const int tUint32 = 13;
const int tEnum = 14;
const int tSfixed32 = 15;
const int tSfixed64 = 16;
const int tSint32 = 17;
const int tSint64 = 18;

const int lOptional = 1;
const int lRequired = 2;
const int lRepeated = 3;

final class POneof {
  String name = '';
}

final class PField {
  String? name;
  String? extendee;
  int number = 0;
  int label = lOptional;
  int type = 0;
  String? typeName;
  int oneofIndex = -1;
  bool proto3Optional = false;
  bool packed = false;
  bool hasPackedOption = false;

  /// Editions FeatureSet.enum_type: 0 unset, 1 OPEN, 2 CLOSED.
  int enumOpen = 0;

  /// Editions FeatureSet.utf8_validation: 0 unset, 2 VERIFY, 3 NONE.
  int utf8 = 0;

  /// Linked: the parsed message/enum and the oneof.
  PMessage? messageType;
  PEnum? enumType;
  bool enumClosed = false;
  POneof? oneof;

  /// Linked: explicit presence (not a proto3 plain scalar).
  bool hasPresence = false;

  /// Linked: the global extension name.
  String? fullName;
}

final class PEnumValue {
  String? name;
  int number = 0;
}

final class PEnum {
  String? name;
  String fullName = '';
  final List<PEnumValue> values = <PEnumValue>[];
  int enumOpen = 0;
  bool closed = false;
}

final class PMessage {
  String? name;
  String fullName = '';
  bool mapEntry = false;
  final List<PField> fields = <PField>[];
  final List<PMessage> nested = <PMessage>[];
  final List<PField> extensions = <PField>[];
  final List<POneof> oneofs = <POneof>[];
  final List<PEnum> enums = <PEnum>[];
  final List<String> reservedNames = <String>[];
}

final class PFile {
  String? name;
  String pkg = '';
  String syntax = 'proto2';

  /// Field 14: an editions file (behaves as proto3 here).
  bool edition = false;
  int enumOpen = 0;
  int utf8 = 0;
  final List<PMessage> messages = <PMessage>[];
  final List<PField> extensions = <PField>[];
  final List<PEnum> enums = <PEnum>[];
}

/* ---------------- wire parsing ---------------- */

R _reader(Uint8List b, int off, int len) {
  if (len < 0 || off < 0 || off + len > b.length) {
    throw CoreError('descriptor overrun');
  }
  return R(b, off, off + len);
}

String _str(Uint8List b, int off, int len) => utf8Decode(b, off, len);

List<PFile> parseSet(Uint8List bytes) {
  final r = R(bytes);
  final files = <PFile>[];
  while (r.pos < r.end) {
    final tag = rVar(r);
    if (tag ~/ 8 == 1 && tag % 8 == wtLen) {
      final n = rLen(r);
      files.add(parseFile(r.b, r.pos, n));
      r.pos += n;
    } else {
      skip(r, tag % 8);
    }
  }
  return files;
}

PFile parseFile(Uint8List b, int off, int len) {
  final r = _reader(b, off, len);
  final f = PFile();
  while (r.pos < r.end) {
    final tag = rVar(r);
    final num = tag ~/ 8;
    final wt = tag % 8;
    if (wt == wtVarint) {
      // Field 14 is the edition marker; its value is irrelevant here.
      if (num == 14) {
        rVar(r);
        f.edition = true;
      } else {
        skip(r, wt);
      }
      continue;
    }
    if (wt == wtLen) {
      final n = rLen(r);
      if (num == 4) {
        f.messages.add(parseMessage(r.b, r.pos, n, 1));
      } else if (num == 5) {
        f.enums.add(parseEnum(r.b, r.pos, n));
      } else if (num == 7) {
        f.extensions.add(parseField(r.b, r.pos, n));
      } else if (num == 8) {
        f.enumOpen = parseFeatures(r.b, r.pos, r.pos + n, 50);
        f.utf8 = parseFeaturesUtf8(r.b, r.pos, r.pos + n, 50);
      } else if (num == 1) {
        f.name = _str(r.b, r.pos, n);
      } else if (num == 2) {
        f.pkg = _str(r.b, r.pos, n);
      } else if (num == 12) {
        f.syntax = _str(r.b, r.pos, n);
      }
      r.pos += n;
    } else {
      skip(r, wt);
    }
  }
  return f;
}

PMessage parseMessage(Uint8List b, int off, int len, int depth) {
  // Descriptor nesting is peer data too: a FileDescriptorSet a few tens of
  // KB can nest thousands of DescriptorProtos, so cap it instead of letting
  // the recursion overflow the stack.
  if (depth > maxDepth) {
    throw CoreError('descriptor nesting exceeds $maxDepth');
  }
  final r = _reader(b, off, len);
  final m = PMessage();
  while (r.pos < r.end) {
    final tag = rVar(r);
    final num = tag ~/ 8;
    if (tag % 8 != wtLen) {
      skip(r, tag % 8);
      continue;
    }
    final n = rLen(r);
    if (num == 1) {
      m.name = _str(r.b, r.pos, n);
    } else if (num == 2) {
      m.fields.add(parseField(r.b, r.pos, n));
    } else if (num == 3) {
      m.nested.add(parseMessage(r.b, r.pos, n, depth + 1));
    } else if (num == 4) {
      m.enums.add(parseEnum(r.b, r.pos, n));
    } else if (num == 6) {
      m.extensions.add(parseField(r.b, r.pos, n));
    } else if (num == 7) {
      m.mapEntry = parseMapEntry(r.b, r.pos, n);
    } else if (num == 8) {
      m.oneofs.add(parseOneof(r.b, r.pos, n));
    } else if (num == 10) {
      m.reservedNames.add(_str(r.b, r.pos, n));
    }
    r.pos += n;
  }
  if (m.name == null) {
    throw CoreError('message without a name');
  }
  return m;
}

POneof parseOneof(Uint8List b, int off, int len) {
  final r = _reader(b, off, len);
  final o = POneof();
  while (r.pos < r.end) {
    final tag = rVar(r);
    if (tag ~/ 8 == 1 && tag % 8 == wtLen) {
      final n = rLen(r);
      o.name = _str(r.b, r.pos, n);
      r.pos += n;
    } else {
      skip(r, tag % 8);
    }
  }
  return o;
}

PEnum parseEnum(Uint8List b, int off, int len) {
  final r = _reader(b, off, len);
  final e = PEnum();
  while (r.pos < r.end) {
    final tag = rVar(r);
    final num = tag ~/ 8;
    if (tag % 8 != wtLen) {
      skip(r, tag % 8);
      continue;
    }
    final n = rLen(r);
    if (num == 1) {
      e.name = _str(r.b, r.pos, n);
    } else if (num == 2) {
      e.values.add(parseEnumValue(r.b, r.pos, n));
    } else if (num == 3) {
      e.enumOpen = parseFeatures(r.b, r.pos, r.pos + n, 7);
    }
    r.pos += n;
  }
  return e;
}

PEnumValue parseEnumValue(Uint8List b, int off, int len) {
  final r = _reader(b, off, len);
  final v = PEnumValue();
  var hasNumber = false;
  while (r.pos < r.end) {
    final tag = rVar(r);
    final num = tag ~/ 8;
    final wt = tag % 8;
    if (wt == wtVarint) {
      if (num == 2) {
        v.number = rVar(r).toSigned(32);
        hasNumber = true;
      } else {
        skip(r, wt);
      }
    } else if (wt == wtLen) {
      final n = rLen(r);
      if (num == 1) {
        v.name = _str(r.b, r.pos, n);
      }
      r.pos += n;
    } else {
      skip(r, wt);
    }
  }
  if (v.name == null || !hasNumber) {
    throw CoreError('enum value without name/number');
  }
  return v;
}

/// MessageOptions.map_entry is field 7.
bool parseMapEntry(Uint8List b, int off, int len) {
  final r = _reader(b, off, len);
  var mapEntry = false;
  while (r.pos < r.end) {
    final tag = rVar(r);
    if (tag % 8 != wtVarint) {
      skip(r, tag % 8);
      continue;
    }
    if (tag ~/ 8 == 7) {
      mapEntry = rVar(r) != 0;
    } else {
      skip(r, wtVarint);
    }
  }
  return mapEntry;
}

/// Reads one Options message and returns FeatureSet.enum_type (1 OPEN, 2
/// CLOSED) when it carries one; 0 means unset. `optionNumber` is the field
/// holding the FeatureSet (50 file, 21 field, ...).
int parseFeatures(Uint8List b, int off, int end, int optionNumber) {
  final r = _reader(b, off, end - off);
  var open = 0;
  while (r.pos < r.end) {
    final tag = rVar(r);
    final num = tag ~/ 8;
    if (tag % 8 != wtLen) {
      skip(r, tag % 8);
      continue;
    }
    final n = rLen(r);
    if (num == optionNumber) {
      open = parseFeatureSetEnumType(r.b, r.pos, r.pos + n);
    }
    r.pos += n;
  }
  return open;
}

/// FileOptions.features (50) -> FeatureSet.utf8_validation.
int parseFeaturesUtf8(Uint8List b, int off, int end, int optionNumber) {
  final r = _reader(b, off, end - off);
  var utf8 = 0;
  while (r.pos < r.end) {
    final tag = rVar(r);
    final num = tag ~/ 8;
    if (tag % 8 != wtLen) {
      skip(r, tag % 8);
      continue;
    }
    final n = rLen(r);
    if (num == optionNumber) {
      utf8 = parseFeatureSetUtf8Value(r.b, r.pos, r.pos + n);
    }
    r.pos += n;
  }
  return utf8;
}

/// FeatureSet.enum_type = 2: 1 OPEN or 2 CLOSED.
int parseFeatureSetEnumType(Uint8List b, int off, int end) {
  final r = _reader(b, off, end - off);
  var open = 0;
  while (r.pos < r.end) {
    final tag = rVar(r);
    if (tag ~/ 8 == 2 && tag % 8 == wtVarint) {
      final v = rVar(r).toSigned(32);
      if (v == 1 || v == 2) {
        open = v;
      }
    } else {
      skip(r, tag % 8);
    }
  }
  return open;
}

/// FeatureSet.utf8_validation = 4: 2 VERIFY or 3 NONE.
int parseFeatureSetUtf8Value(Uint8List b, int off, int end) {
  final r = _reader(b, off, end - off);
  var utf8 = 0;
  while (r.pos < r.end) {
    final tag = rVar(r);
    if (tag ~/ 8 == 4 && tag % 8 == wtVarint) {
      final v = rVar(r).toSigned(32);
      if (v == 2 || v == 3) {
        utf8 = v;
      }
    } else {
      skip(r, tag % 8);
    }
  }
  return utf8;
}

PField parseField(Uint8List b, int off, int len) {
  final r = _reader(b, off, len);
  final f = PField();
  while (r.pos < r.end) {
    final tag = rVar(r);
    final num = tag ~/ 8;
    final wt = tag % 8;
    if (wt == wtVarint) {
      final v = rVar(r).toSigned(32);
      if (num == 3) {
        f.number = v;
      } else if (num == 4) {
        f.label = v;
      } else if (num == 5) {
        f.type = v;
      } else if (num == 9) {
        f.oneofIndex = v;
      } else if (num == 17) {
        f.proto3Optional = v != 0;
      }
    } else if (wt == wtLen) {
      final n = rLen(r);
      if (num == 1) {
        f.name = _str(r.b, r.pos, n);
      } else if (num == 2) {
        f.extendee = _str(r.b, r.pos, n);
      } else if (num == 6) {
        f.typeName = _str(r.b, r.pos, n);
      } else if (num == 8) {
        parseFieldOptions(r.b, r.pos, r.pos + n, f);
      }
      r.pos += n;
    } else {
      skip(r, wt);
    }
  }
  if (f.name == null || f.type == 0 || f.number == 0) {
    throw CoreError('field without name/type/number');
  }
  if (f.type < tDouble || f.type > tSint64) {
    throw CoreError('bad field type ${f.type}');
  }
  if (f.label < lOptional || f.label > lRepeated) {
    throw CoreError('bad label ${f.label}');
  }
  return f;
}

/// FieldOptions: packed (2) and the editions FeatureSet (21).
void parseFieldOptions(Uint8List b, int off, int end, PField f) {
  final r = _reader(b, off, end - off);
  while (r.pos < r.end) {
    final tag = rVar(r);
    final num = tag ~/ 8;
    if (num == 2 && tag % 8 == wtVarint) {
      f.packed = rVar(r) != 0;
      f.hasPackedOption = true;
    } else if (num == 21 && tag % 8 == wtLen) {
      final n = rLen(r);
      f.enumOpen = parseFeatureSetEnumType(r.b, r.pos, r.pos + n);
      f.utf8 = parseFeatureSetUtf8Value(r.b, r.pos, r.pos + n);
      r.pos += n;
    } else {
      skip(r, tag % 8);
    }
  }
}

/* ---------------- linking ---------------- */

void _indexMessage(
    PMessage m, String scope, Map<String, PMessage> byName,
    Map<String, PEnum> enums) {
  m.fullName = scope.isEmpty ? (m.name as String) : '${scope}.${m.name}';
  if (byName.containsKey(m.fullName)) {
    throw CoreError('duplicate message ${m.fullName}');
  }
  byName[m.fullName] = m;
  for (final e in m.enums) {
    _indexEnum(e, m.fullName, enums);
  }
  for (final n in m.nested) {
    _indexMessage(n, m.fullName, byName, enums);
  }
}

void _indexEnum(PEnum e, String scope, Map<String, PEnum> enums) {
  e.fullName = scope.isEmpty ? (e.name as String) : '${scope}.${e.name}';
  if (enums.containsKey(e.fullName)) {
    throw CoreError('duplicate enum ${e.fullName}');
  }
  enums[e.fullName] = e;
}

/// An enum is closed for proto2 files (and editions files asking for it).
void _linkEnum(PEnum e, bool proto3, int fileEnumOpen) {
  if (e.name == null) {
    throw CoreError('enum without a name');
  }
  if (e.enumOpen != 0) {
    e.closed = e.enumOpen == 2;
  } else if (fileEnumOpen != 0) {
    e.closed = fileEnumOpen == 2;
  } else {
    e.closed = !proto3;
  }
}

void _linkEnums(PMessage m, bool proto3, int fileEnumOpen) {
  for (final e in m.enums) {
    _linkEnum(e, proto3, fileEnumOpen);
  }
  for (final n in m.nested) {
    _linkEnums(n, proto3, fileEnumOpen);
  }
}

void _linkMessage(PMessage m, String scope, bool proto3, int fileEnumOpen,
    int fileUtf8, Map<String, PMessage> byName, Map<String, PEnum> enums) {
  for (final f in m.fields) {
    _linkField(f, scope, proto3, fileUtf8, byName, enums, m);
  }
  for (final n in m.nested) {
    _linkMessage(
        n, m.fullName, proto3, fileEnumOpen, fileUtf8, byName, enums);
  }
  for (final e in m.extensions) {
    _linkField(e, scope, proto3, fileUtf8, byName, enums, null);
    e.fullName =
        e.name == null ? null : (scope.isEmpty ? e.name : '$scope.${e.name}');
  }
}

bool _isPackableType(int t) =>
    !(t == tString || t == tGroup || t == tMessage || t == tBytes);

void _linkField(PField f, String scope, bool proto3, int fileUtf8,
    Map<String, PMessage> byName, Map<String, PEnum> enums, PMessage? parent) {
  if (f.oneofIndex >= 0 && parent != null) {
    if (f.oneofIndex >= parent.oneofs.length) {
      throw CoreError('field ${f.name}: oneof_index out of range');
    }
    f.oneof = parent.oneofs[f.oneofIndex];
  }
  if ((f.type == tMessage || f.type == tGroup) && f.typeName != null) {
    f.messageType = _resolve(byName, scope, f.typeName!);
    if (f.messageType == null) {
      throw CoreError('field ${f.name}: no such type ${f.typeName}');
    }
  }
  if (f.type == tEnum && f.typeName != null) {
    f.enumType = _resolveEnum(enums, scope, f.typeName!);
    if (f.enumType == null) {
      throw CoreError('field ${f.name}: no such enum ${f.typeName}');
    }
    // Editions may close one field in an open file; that wins over the enum
    // type, exactly like the core's per-field enum_open.
    f.enumClosed = f.enumOpen != 0 ? f.enumOpen == 2 : f.enumType!.closed;
  }
  if (f.label == lRepeated && _isPackableType(f.type) && !f.hasPackedOption) {
    f.packed = proto3; // the syntax default
  }
  /* utf8_validation: field FeatureSet, then the file's, then the dialect
   * default (VERIFY for proto3/editions, NONE for proto2). Only `string`
   * fields carry it, and only they validate on parse. */
  if (f.type == tString) {
    final utf8 = f.utf8 != 0
        ? f.utf8
        : (fileUtf8 != 0 ? fileUtf8 : (proto3 ? 2 : 3));
    f.utf8 = utf8 == 2 ? 2 : 3;
  } else {
    f.utf8 = 3;
  }
  f.hasPresence = f.label != lRepeated &&
      (f.type == tMessage ||
          f.type == tGroup ||
          f.label == lRequired ||
          !proto3 ||
          f.proto3Optional);
}

/// Leading-dot names resolve from the root; otherwise search the scope up.
PMessage? _resolve(
    Map<String, PMessage> byName, String scope, String name) {
  if (name.startsWith('.')) {
    return byName[name.substring(1)];
  }
  var s = scope;
  while (true) {
    final cand = s.isEmpty ? name : '$s.$name';
    final m = byName[cand];
    if (m != null) {
      return m;
    }
    // Nested scopes qualify from the inside out: C++ scoping.
    final dot = name.indexOf('.');
    if (dot >= 0) {
      final head = name.substring(0, dot);
      final rest = name.substring(dot + 1);
      final prefix = s.isEmpty ? head : '$s.$head';
      if (byName.containsKey(prefix)) {
        return _resolve(byName, prefix, rest);
      }
    }
    if (s.isEmpty) {
      return null;
    }
    final cut = s.lastIndexOf('.');
    s = cut < 0 ? '' : s.substring(0, cut);
  }
}

/// The enum counterpart of [_resolve]: same C++ scoping rules.
PEnum? _resolveEnum(Map<String, PEnum> byName, String scope, String name) {
  if (name.startsWith('.')) {
    return byName[name.substring(1)];
  }
  var s = scope;
  while (true) {
    final cand = s.isEmpty ? name : '$s.$name';
    final e = byName[cand];
    if (e != null) {
      return e;
    }
    final dot = name.indexOf('.');
    if (dot >= 0) {
      final head = name.substring(0, dot);
      final rest = name.substring(dot + 1);
      final prefix = s.isEmpty ? head : '$s.$head';
      if (byName.containsKey(prefix)) {
        return _resolveEnum(byName, prefix, rest);
      }
    }
    if (s.isEmpty) {
      return null;
    }
    final cut = s.lastIndexOf('.');
    s = cut < 0 ? '' : s.substring(0, cut);
  }
}

void linkFiles(List<PFile> files) {
  final byName = <String, PMessage>{};
  final enums = <String, PEnum>{};
  for (final f in files) {
    for (final m in f.messages) {
      _indexMessage(m, f.pkg, byName, enums);
    }
    for (final e in f.enums) {
      _indexEnum(e, f.pkg, enums);
    }
  }
  for (final f in files) {
    // Editions files carry syntax "editions" plus field 14: the core treats
    // them as proto3, so packed defaults and presence match.
    final proto3 = f.syntax == 'proto3' || f.edition;
    for (final e in f.enums) {
      _linkEnum(e, proto3, f.enumOpen);
    }
    // Enums close/open before fields link: a field copies its enum's closed
    // flag, so the enum must be linked first.
    for (final m in f.messages) {
      _linkEnums(m, proto3, f.enumOpen);
    }
    for (final m in f.messages) {
      _linkMessage(m, f.pkg, proto3, f.enumOpen, f.utf8, byName, enums);
    }
    for (final e in f.extensions) {
      _linkField(e, f.pkg, proto3, f.utf8, byName, enums, null);
      e.fullName = e.name == null
          ? null
          : (f.pkg.isEmpty ? e.name : '${f.pkg}.${e.name}');
    }
  }
}

/* ---------------- compiled schema building ---------------- */

int _kindOf(int t) {
  switch (t) {
    case tInt32:
      return K.int32;
    case tUint32:
      return K.uint32;
    case tSint32:
      return K.sint32;
    case tInt64:
      return K.int64;
    case tUint64:
      return K.uint64;
    case tSint64:
      return K.sint64;
    case tBool:
      return K.bool_;
    case tEnum:
      return K.enum_;
    case tFixed32:
      return K.fixed32;
    case tSfixed32:
      return K.sfixed32;
    case tFloat:
      return K.float;
    case tFixed64:
      return K.fixed64;
    case tSfixed64:
      return K.sfixed64;
    case tDouble:
      return K.double_;
    case tString:
      return K.string;
    case tBytes:
      return K.bytes;
    case tMessage:
      return K.message;
    default:
      return K.group;
  }
}

int _wireType(int t, bool packed) {
  if (packed) {
    return wtLen;
  }
  switch (t) {
    case tDouble:
    case tFixed64:
    case tSfixed64:
      return 1;
    case tString:
    case tBytes:
    case tMessage:
      return wtLen;
    case tGroup:
      return 3;
    case tFixed32:
    case tSfixed32:
    case tFloat:
      return 5;
    default:
      return wtVarint;
  }
}

final class _Builder {
  final CoreSchema schema = CoreSchema();

  CoreSchema build(List<PFile> files) {
    // Enums first: message fields link their element type while the defs are
    // built.
    for (final f in files) {
      for (final e in f.enums) {
        _buildEnum(e);
      }
      for (final m in f.messages) {
        _buildEnums(m, 1);
      }
    }
    for (final f in files) {
      for (final m in f.messages) {
        _buildDef(m, 1);
      }
    }
    // Extensions attach to their extendee after all defs exist.
    for (final f in files) {
      for (final e in f.extensions) {
        _attachExtension(e);
      }
      for (final m in f.messages) {
        _attachMessageExtensions(m);
      }
    }
    _finishTransitive(schema);
    return schema;
  }

  void _buildEnum(PEnum e) {
    final byName = <String, int>{};
    final byNumber = <int, String>{};
    for (final v in e.values) {
      final name = v.name as String;
      if (byName.containsKey(name)) {
        throw CoreError('duplicate enum value ${e.fullName}.$name');
      }
      byName[name] = v.number;
      // Aliases repeat a number: the first declared name is printed.
      byNumber.putIfAbsent(v.number, () => name);
    }
    schema.enums[e.fullName] = EnumDef(e.fullName, byName, byNumber, e.closed);
  }

  void _buildEnums(PMessage m, int depth) {
    if (depth > maxDepth) {
      throw CoreError('descriptor nesting exceeds $maxDepth');
    }
    for (final e in m.enums) {
      _buildEnum(e);
    }
    for (final n in m.nested) {
      _buildEnums(n, depth + 1);
    }
  }

  MsgDef _buildDef(PMessage m, int depth) {
    if (depth > maxDepth) {
      throw CoreError('descriptor nesting exceeds $maxDepth');
    }
    final done = schema.messages[m.fullName];
    if (done != null) {
      return done;
    }
    final d = MsgDef(m.fullName, m.mapEntry, List<String>.of(m.reservedNames));
    schema.messages[m.fullName] = d; // before fields: recursion-safe
    final fs = <Field>[];
    final oneofs = <POneof, int>{};
    for (final f in m.fields) {
      fs.add(_buildField(f, oneofs));
    }
    _finishDef(d, fs, oneofs);
    for (final n in m.nested) {
      _buildDef(n, depth + 1);
    }
    return d;
  }

  Field _buildField(PField f, Map<POneof, int> oneofs) {
    final kind = _kindOf(f.type);
    final repeated = f.label == lRepeated;
    var oneof = -1;
    if (f.oneof != null) {
      final idx = oneofs[f.oneof];
      if (idx == null) {
        oneof = oneofs.length;
        oneofs[f.oneof!] = oneof;
      } else {
        oneof = idx;
      }
    }
    int presence;
    if (oneof >= 0) {
      presence = presenceExplicit; // reads/writes go through the oneof case
    } else if (f.label == lRequired) {
      presence = presenceRequired;
    } else if (kind == K.message || kind == K.group) {
      presence = presenceMessage;
    } else if (f.hasPresence) {
      presence = presenceExplicit;
    } else {
      presence = presenceImplicit;
    }
    final packed = repeated && f.packed;
    final wt = _wireType(f.type, packed);
    final number = f.number;
    return Field(
      number: number,
      name: f.name as String,
      kind: kind,
      wt: wt,
      elemWt: _wireType(f.type, false),
      repeated: repeated,
      packed: packed,
      oneof: oneof,
      presence: presence,
      msgType: f.messageType == null ? null : _buildDef(f.messageType!, 1),
      enumType:
          f.enumType == null ? null : schema.enums[f.enumType!.fullName],
      enumClosed: f.enumClosed,
      utf8: f.type == tString && f.utf8 == 2,
      fullName: f.fullName,
      tag: tagBytes(number, wt),
      tagEnd: kind == K.group ? tagBytes(number, 4) : null,
    );
  }

  void _attachMessageExtensions(PMessage m) {
    for (final e in m.extensions) {
      _attachExtension(e);
    }
    for (final n in m.nested) {
      _attachMessageExtensions(n);
    }
  }

  void _attachExtension(PField e) {
    var extendee = e.extendee;
    if (extendee == null) {
      throw CoreError('extension without extendee');
    }
    if (extendee.startsWith('.')) {
      extendee = extendee.substring(1);
    }
    final d = schema.messages[extendee];
    if (d == null) {
      throw CoreError('extendee missing: ${e.extendee}');
    }
    final s = _buildField(e, <POneof, int>{});
    s.fullName = e.fullName;
    final fs = List<Field>.of(d.fields)..add(s);
    final oneofByName = d.oneofByName;
    final count = d.oneofCount;
    _finishDef(d, fs, <POneof, int>{});
    d.oneofByName = oneofByName; // extensions are never in a oneof
    d.oneofCount = count;
  }
}

void _finishDef(MsgDef d, List<Field> fs, Map<POneof, int> oneofs) {
  fs.sort((a, b) => a.number - b.number);
  var max = 0;
  var req = false;
  for (final f in fs) {
    if (f.number > max) {
      max = f.number;
    }
    req = req || f.presence == presenceRequired;
  }
  d.hasRequired = req;
  d.fields = fs;
  // Dense only when bounded, like the C core's fnum map: one field numbered
  // 536870911 must not allocate a 2 GiB array.
  if (max <= 512 && max <= fs.length * 8 + 8) {
    final map = Int32List(max + 1)..fillRange(0, max + 1, -1);
    for (var i = 0; i < fs.length; i++) {
      map[fs[i].number] = i;
    }
    d.slotByFnum = map;
  } else {
    d.slotByFnum = null;
  }
  final byName = <String, int>{};
  for (var i = 0; i < fs.length; i++) {
    byName[fs[i].name] = i;
  }
  d.byName = byName;
  final oneofByName = <String, int>{};
  for (final entry in oneofs.entries) {
    oneofByName[entry.key.name] = entry.value;
  }
  d.oneofByName = oneofByName;
  d.oneofCount = d.oneofCount > oneofs.length ? d.oneofCount : oneofs.length;
}

/// Slot of a field number, -1 when the message has no such field.
int slotOf(MsgDef def, int fnum) {
  final map = def.slotByFnum;
  if (map != null) {
    return fnum >= 0 && fnum < map.length ? map[fnum] : -1;
  }
  final fs = def.fields;
  for (var i = 0; i < fs.length; i++) {
    if (fs[i].number == fnum) {
      return i;
    }
  }
  return -1;
}

void _finishTransitive(CoreSchema schema) {
  var changed = true;
  while (changed) {
    changed = false;
    for (final d in schema.messages.values) {
      if (!d.deepRequired && _needsRequired(d)) {
        d.deepRequired = true;
        changed = true;
      }
    }
  }
}

bool _needsRequired(MsgDef d) {
  for (final f in d.fields) {
    if (f.presence == presenceRequired) {
      return true;
    }
    if ((f.kind == K.message || f.kind == K.group) &&
        f.msgType != null &&
        f.msgType!.deepRequired) {
      return true;
    }
  }
  return false;
}

/* ---------------- public entry ---------------- */

/// Parses and links a FileDescriptorSet into a compiled schema.
CoreSchema loadSchema(Uint8List bytes, [Limits? limits]) {
  checkBytes(limits, bytes.length, 'descriptor set');
  List<PFile> files;
  try {
    files = parseSet(bytes);
    linkFiles(files);
  } on CoreError catch (e) {
    throw CoreError('descriptor set: ${e.message}', 1);
  }
  return _Builder().build(files);
}

/// Number of declared fields in a message (descriptor test surface).
int fieldCount(MsgDef def) => def.fields.length;

/// The field named `name`, or null.
Field? fieldByName(MsgDef def, String name) {
  final slot = def.byName[name];
  return slot == null ? null : def.fields[slot];
}

// The dynamic message tree: one slot per declared field, plus the oneof cases
// and the retained unknown fields. Scalars live in their wire-neutral Dart
// form (BigInt for the 64-bit kinds, raw bits for float/double so NaN payloads
// survive a round trip, 0/1 for bool); messages live nested as [Msg]; strings
// and bytes decoded off the wire stay a [Raw] slice of the input until
// materialized, so invalid UTF-8 and non-minimal length prefixes re-encode
// byte for byte.
//
// This is the Dart twin of bindings/java/runtime/GenericCodec.DMsg.

import 'dart:typed_data';

import 'descriptor.dart';
import 'wire.dart';

/// An undecoded field image (tag + length + payload) inside the input.
final class Raw {
  Raw(this.b, this.off, this.len);

  final Uint8List b;
  final int off;
  final int len;
}

final class Msg {
  Msg(this.def)
      : v = List<Object?>.filled(def.fields.length, null, growable: false),
        oneofCase = def.oneofCount > 0 ? List<int>.filled(def.oneofCount, 0) : null;

  final MsgDef def;

  /// Per slot: null = absent, a growable list for repeated fields.
  final List<Object?> v;

  /// Slot+1 per oneof, 0 = none set; null when the message has none.
  final List<int>? oneofCase;

  /// Retained unknown fields in input order; null until the first.
  List<Uint8List>? unknown;
}

/// Slot index of a field name, or -1.
int slotByName(MsgDef def, String name) {
  final s = def.byName[name];
  return s == null ? -1 : s;
}

int _requireSlot(MsgDef def, Object nameOrNumber) {
  final s = nameOrNumber is int
      ? slotOf(def, nameOrNumber)
      : slotByName(def, nameOrNumber as String);
  if (s < 0) {
    throw CoreError('${def.fullName} has no field $nameOrNumber');
  }
  return s;
}

/// Stores a value in a slot, collapsing the oneof it belongs to.
void setSlot(Msg m, int s, Object value) {
  final f = m.def.fields[s];
  final cases = m.oneofCase;
  if (f.oneof >= 0 && cases != null) {
    final prev = cases[f.oneof];
    if (prev != 0 && prev != s + 1) {
      m.v[prev - 1] = null;
    }
    cases[f.oneof] = s + 1;
  }
  m.v[s] = value;
}

bool hasField(Msg m, Object nameOrNumber) =>
    m.v[_requireSlot(m.def, nameOrNumber)] != null;

/// The singular value of a field: materialized, converted, or null.
Object? getField(Msg m, Object nameOrNumber) {
  final s = _requireSlot(m.def, nameOrNumber);
  final f = m.def.fields[s];
  final v = m.v[s];
  if (v == null) {
    return null;
  }
  if (f.repeated) {
    final arr = v as List<Object?>;
    final out = List<Object?>.filled(arr.length, null);
    for (var i = 0; i < arr.length; i++) {
      out[i] = toJs(f, arr[i]);
    }
    return out;
  }
  return toJs(f, v);
}

/// Which member of a oneof is set: its field number, or 0 for none.
int oneofCase(Msg m, String oneofName) {
  final idx = m.def.oneofByName[oneofName];
  final cases = m.oneofCase;
  if (idx == null || cases == null) {
    throw CoreError('${m.def.fullName} has no oneof $oneofName');
  }
  final c = cases[idx];
  return c == 0 ? 0 : m.def.fields[c - 1].number;
}

/// The payload bytes of a Raw string/bytes field image.
Uint8List rawPayload(Raw raw) {
  final r = R(raw.b, raw.off, raw.off + raw.len);
  rVar(r); // the tag, already validated at slice time
  final n = rLen(r);
  return Uint8List.sublistView(r.b, r.pos, r.pos + n);
}

/// Materializes a Raw slice into its Dart value.
Object materialize(Field f, Raw raw) {
  final r = R(raw.b, raw.off, raw.off + raw.len);
  rVar(r); // the tag
  final n = rLen(r);
  if (f.kind == K.string) {
    // proto3/editions strings were validated at parse, so this is the loose
    // mapping only for proto2 strings.
    return utf8Decode(r.b, r.pos, n);
  }
  return Uint8List.sublistView(r.b, r.pos, r.pos + n);
}

/// One value in its Dart form (what get()/toObject() expose).
Object? toJs(Field f, Object? v) {
  if (v is Raw) {
    return materialize(f, v);
  }
  switch (f.kind) {
    case K.bool_:
      return (v as int) != 0;
    case K.float:
      return bitsFloat(v as int);
    case K.double_:
      return bitsDouble(v as BigInt);
    case K.message:
    case K.group:
      return v;
    default:
      return v;
  }
}

/// A plain map: proto field names, BigInt for 64-bit, nested maps.
Map<String, Object?> toObject(Msg m) {
  final def = m.def;
  final out = <String, Object?>{};
  for (var s = 0; s < def.fields.length; s++) {
    final v = m.v[s];
    if (v == null) {
      continue;
    }
    final f = def.fields[s];
    if (f.repeated) {
      final arr = v as List<Object?>;
      if (arr.isEmpty) {
        continue;
      }
      final list = List<Object?>.filled(arr.length, null);
      for (var i = 0; i < arr.length; i++) {
        final item = arr[i];
        list[i] = (f.kind == K.message || f.kind == K.group) && item is Msg
            ? toObject(item)
            : toJs(f, item);
      }
      out[f.name] = list;
      continue;
    }
    if ((f.kind == K.message || f.kind == K.group) && v is Msg) {
      out[f.name] = toObject(v);
      continue;
    }
    out[f.name] = toJs(f, v);
  }
  return out;
}

/// The stored form for one field value from a plain map.
Object? _fromJs(Field f, Object? v) {
  switch (f.kind) {
    case K.int32:
    case K.sint32:
    case K.sfixed32:
    case K.enum_:
      return v is BigInt ? v.toSigned(32).toInt() : v as int;
    case K.uint32:
    case K.fixed32:
      return v is BigInt ? v.toUnsigned(32).toInt() : (v as int) % u32;
    case K.int64:
    case K.sint64:
    case K.sfixed64:
      return (v as BigInt).toSigned(64);
    case K.uint64:
    case K.fixed64:
      return (v as BigInt).toUnsigned(64);
    case K.bool_:
      return v == true || v == 1 ? 1 : 0;
    case K.float:
      return floatBits((v as num).toDouble());
    case K.double_:
      return doubleBits((v as num).toDouble());
    case K.string:
      return v is String ? v : '$v';
    case K.bytes:
      return v is Uint8List ? v : Uint8List.fromList(v as List<int>);
    default: // K.message / K.group
      return v is Msg ? v : fromObject(f.msgType!, v as Map<String, Object?>);
  }
}

/// Builds a tree from a plain map (proto field names, BigInt for 64-bit;
/// absent or null keys stay absent).
Msg fromObject(MsgDef def, Map<String, Object?> obj) {
  final m = Msg(def);
  for (var s = 0; s < def.fields.length; s++) {
    final f = def.fields[s];
    final v = obj[f.name];
    if (v == null) {
      continue;
    }
    try {
      if (f.repeated) {
        if (v is! List) {
          throw CoreError('needs a list');
        }
        final arr = List<Object?>.filled(v.length, null);
        for (var i = 0; i < v.length; i++) {
          arr[i] = _fromJs(f, v[i]);
        }
        m.v[s] = arr;
      } else {
        setSlot(m, s, _fromJs(f, v) as Object);
      }
    } on CoreError catch (e) {
      throw CoreError('${def.fullName}.${f.name}: ${e.message}');
    }
  }
  return m;
}

/// Throws when a REQUIRED field of this message is absent.
void checkPresentRequired(Msg m, int s) {
  if (m.v[s] == null && m.def.fields[s].presence == presenceRequired) {
    final f = m.def.fields[s];
    throw CoreError(
        'missing required field ${m.def.fullName}.${f.name}', 6);
  }
}

// Decoder: strict tags and lengths (like the C core), unknown field numbers
// AND wrong-wire-type occurrences retained as raw tag+payload bytes in input
// order, groups decoded to their matching end tag, packed runs accepted for
// every numeric kind (and the unpacked element form for a packed field),
// oneofs collapsed to the last member, presence following the field's own
// zero, proto2 required fields checked over the whole tree, and the nesting
// cap enforced on every recursive path.
//
// The Dart twin of bindings/java/runtime/GenericCodec.decodeInto.

import 'dart:typed_data';

import 'descriptor.dart';
import 'tree.dart';
import 'wire.dart';

/// Decodes a whole message, unknown fields retained.
Msg decode(MsgDef def, Uint8List bytes, [Limits? limits]) {
  checkBytes(limits, bytes.length, 'message');
  final r = R(bytes);
  r.cap = effectiveDepth(limits);
  final m = Msg(def);
  _decodeInto(m, r, bytes.length, -1, 0);
  if (def.deepRequired) {
    _checkRequired(m, r.cap, 0);
  }
  return m;
}

/// Decodes a message body that already starts at `off` and ends at `off+len`.
Msg decodeSlice(MsgDef def, Uint8List bytes, int off, int len,
    [Limits? limits]) {
  checkBytes(limits, len, 'message');
  final r = R(bytes, off, off + len);
  r.cap = effectiveDepth(limits);
  final m = Msg(def);
  _decodeInto(m, r, off + len, -1, 0);
  if (def.deepRequired) {
    _checkRequired(m, r.cap, 0);
  }
  return m;
}

/// An empty message of `def`.
Msg newMessage(MsgDef def) => Msg(def);

/// True when the field does not accept this wire type. A repeated numeric
/// field always accepts the LEN form (a packed run, whatever the declared
/// form) and a packed field also accepts its unpacked element form.
bool _wtIncompatible(Field f, int wt) {
  if (f.kind == K.group) {
    return wt != wtStartGroup;
  }
  if (f.repeated && isPackable(f.kind)) {
    if (wt == wtLen) {
      return false;
    }
    if (f.packed) {
      return wt != f.elemWt;
    }
  }
  return wt != f.wt;
}

/// Decodes into m up to `end` (or the matching end-group when groupEnd >= 0).
/// One shared reader threads through the recursion, so positions never need
/// saving: a nested payload consumes exactly its slice.
void _decodeInto(Msg m, R r, int end, int groupEnd, int depth) {
  if (depth > r.cap) {
    throw CoreError('nesting exceeds ${r.cap}', 4);
  }
  final def = m.def;
  final outerEnd = r.end;
  r.end = end;
  var closed = groupEnd < 0;
  while (r.pos < r.end) {
    final fieldStart = r.pos;
    final tag = rTag(r);
    final fnum = tag ~/ 8;
    final wt = tag % 8;
    if (fnum == 0 || wt > wtFixed32) {
      throw CoreError('bad tag $tag', 1);
    }
    if (wt == wtEndGroup) {
      // Only our own number closes us; any other end tag is stray.
      if (groupEnd < 0 || fnum != groupEnd) {
        throw CoreError('stray end-group for $fnum', 1);
      }
      closed = true;
      break;
    }
    final s = slotOf(def, fnum);
    if (s < 0 || _wtIncompatible(def.fields[s], wt)) {
      _retainUnknown(m, r, fieldStart, fnum, wt, depth);
      continue;
    }
    _readKnown(m, r, def.fields[s], s, wt, fieldStart, depth);
  }
  r.end = outerEnd;
  if (!closed) {
    throw CoreError('missing end-group for $groupEnd', 1);
  }
}

/// Wrong wire type (or unknown number) keeps the raw blob, like the core.
/// Groups validate to their matching end tag; anything else only checks value
/// shape -- unknown LEN payloads stay opaque.
void _retainUnknown(
    Msg m, R r, int fieldStart, int fnum, int wt, int depth) {
  skipUnknown(r, fnum, wt, depth);
  (m.unknown ??= <Uint8List>[]).add(r.b.sublist(fieldStart, r.pos));
}

void _readKnown(Msg m, R r, Field f, int s, int wt, int fieldStart, int depth) {
  if (f.repeated) {
    var arr = m.v[s] as List<Object?>?;
    if (arr == null) {
      arr = <Object?>[];
      m.v[s] = arr;
    }
    if (isPackable(f.kind)) {
      if (wt == wtLen) {
        _readPacked(r, f, arr);
        if (arr.isEmpty) {
          // An empty packed run adds nothing: presence follows length, so
          // drop the holder again.
          m.v[s] = null;
        }
        return;
      }
      arr.add(_readNumSingle(r, f.kind));
      return;
    }
    if (f.kind == K.message) {
      final len = rLen32(r);
      final sub = Msg(f.msgType!);
      _decodeInto(sub, r, r.pos + len, -1, depth + 1);
      arr.add(sub);
      return;
    }
    if (f.kind == K.group) {
      final sub = Msg(f.msgType!);
      _decodeInto(sub, r, r.end, f.number, depth + 1);
      arr.add(sub);
      return;
    }
    // K.string / K.bytes element: validate, then record one slice.
    final len = rLen32(r);
    if (f.utf8 && !utf8Valid(r.b, r.pos, len)) {
      throw CoreError('invalid UTF-8 in a string field', 1);
    }
    r.pos += len;
    arr.add(Raw(r.b, fieldStart, r.pos - fieldStart));
    return;
  }
  if (isPackable(f.kind)) {
    final v = _readNumSingle(r, f.kind);
    // Implicit presence is decided on the *field's* zero, not the raw wire
    // bits: an int32/uint32 varint wider than 32 bits truncates and the
    // reference then drops the field as an implicit zero.
    if (f.presence == presenceImplicit && _isZero(v)) {
      return;
    }
    setSlot(m, s, v);
    return;
  }
  // Ref-kind singular: a second occurrence merges (like the core).
  if (f.kind == K.message) {
    final len = rLen32(r);
    final existing = _mergeTarget(m, f, s);
    if (existing != null) {
      _decodeInto(existing, r, r.pos + len, -1, depth + 1);
    } else {
      final sub = Msg(f.msgType!);
      _decodeInto(sub, r, r.pos + len, -1, depth + 1);
      setSlot(m, s, sub);
    }
    return;
  }
  if (f.kind == K.group) {
    final existing = _mergeTarget(m, f, s);
    if (existing != null) {
      _decodeInto(existing, r, r.end, f.number, depth + 1);
    } else {
      final sub = Msg(f.msgType!);
      _decodeInto(sub, r, r.end, f.number, depth + 1);
      setSlot(m, s, sub);
    }
    return;
  }
  // K.string / K.bytes (an empty implicit string stays absent).
  final len = rLen32(r);
  if (len == 0 && f.presence == presenceImplicit) {
    return;
  }
  /* proto3/editions strings are validated here, at parse, like the
   * reference: a malformed value fails the decode instead of surfacing as
   * U+FFFD when the slice is materialized. proto2 keeps the loose mapping. */
  if (f.utf8 && !utf8Valid(r.b, r.pos, len)) {
    throw CoreError('invalid UTF-8 in a string field', 1);
  }
  r.pos += len;
  setSlot(m, s, Raw(r.b, fieldStart, r.pos - fieldStart));
}

/// The submessage a repeated singular message/group field merges into, or
/// null for the first occurrence.
Msg? _mergeTarget(Msg m, Field f, int s) {
  final v = m.v[s];
  if (v == null) {
    return null;
  }
  final cases = m.oneofCase;
  if (f.oneof >= 0 && cases != null && cases[f.oneof] != s + 1) {
    return null;
  }
  return v is Msg ? v : null;
}

bool _isZero(Object? v) => v == 0 || v == BigInt.zero;

/// One packed run of numeric elements into `arr`.
void _readPacked(R r, Field f, List<Object?> arr) {
  final len = rLen32(r);
  final end = r.pos + len;
  if (f.kind == K.bool_) {
    while (r.pos < end) {
      arr.add(rVarBool(r));
    }
  } else if (f.kind == K.fixed32 ||
      f.kind == K.sfixed32 ||
      f.kind == K.float) {
    final count = len ~/ 4;
    for (var i = 0; i < count; i++) {
      final v = rFixed32(r);
      arr.add(f.kind == K.sfixed32 ? v.toSigned(32) : v);
    }
  } else if (f.kind == K.fixed64 ||
      f.kind == K.sfixed64 ||
      f.kind == K.double_) {
    final count = len ~/ 8;
    for (var i = 0; i < count; i++) {
      final v = rFixed64(r);
      arr.add(f.kind == K.sfixed64 ? v.toSigned(64) : v);
    }
  } else {
    while (r.pos < end) {
      arr.add(_readPackedVar(r, f.kind));
    }
  }
  if (r.pos != end) {
    throw CoreError('packed overrun', 1);
  }
}

Object _readPackedVar(R r, int kind) {
  switch (kind) {
    case K.int32:
    case K.enum_:
      return rVar(r).toSigned(32);
    case K.uint32:
      return rVar(r);
    case K.sint32:
      // Truncate before unzigzag: a 64-bit varint on a sint32 field decodes
      // its low 32 bits, like the reference.
      return unzig32(rVar(r));
    case K.int64:
      return rVar64(r).toSigned(64);
    case K.uint64:
      return rVar64(r);
    case K.sint64:
      return unzig64(rVar64(r));
    default:
      throw CoreError('not numeric', 1);
  }
}

/// One numeric element in its storage form (see tree.dart).
Object _readNumSingle(R r, int kind) {
  switch (kind) {
    case K.int32:
    case K.enum_:
      return rVar(r).toSigned(32);
    case K.uint32:
      return rVar(r);
    case K.bool_:
      return rVarBool(r);
    case K.sint32:
      return unzig32(rVar(r));
    case K.int64:
      return rVar64(r).toSigned(64);
    case K.uint64:
      return rVar64(r);
    case K.sint64:
      return unzig64(rVar64(r));
    case K.fixed32:
      return rFixed32(r);
    case K.sfixed32:
      return rFixed32(r).toSigned(32);
    case K.float:
      return rFixed32(r);
    case K.fixed64:
      return rFixed64(r);
    case K.sfixed64:
      return rFixed64(r).toSigned(64);
    case K.double_:
      return rFixed64(r);
    default:
      throw CoreError('not numeric', 1);
  }
}

/// Whole-tree required check: every present nested message is checked too, so
/// a missing proto2 required field fails the decode, like the core.
void _checkRequired(Msg m, int cap, int depth) {
  if (depth > cap) {
    throw CoreError('nesting exceeds $cap', 4);
  }
  final def = m.def;
  if (def.hasRequired) {
    for (var s = 0; s < def.fields.length; s++) {
      if (def.fields[s].presence == presenceRequired && m.v[s] == null) {
        throw CoreError(
            'missing required field ${def.fullName}.${def.fields[s].name}', 6);
      }
    }
  }
  for (var s = 0; s < def.fields.length; s++) {
    final f = def.fields[s];
    if (f.kind != K.message && f.kind != K.group) {
      continue;
    }
    final v = m.v[s];
    if (v == null) {
      continue;
    }
    if (f.repeated) {
      final arr = v as List<Object?>;
      for (var i = 0; i < arr.length; i++) {
        _checkRequired(arr[i] as Msg, cap, depth + 1);
      }
    } else if (v is Msg) {
      _checkRequired(v, cap, depth + 1);
    }
  }
}

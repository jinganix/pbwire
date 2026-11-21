// Encoder: one pass into a reusable buffer, fields in schema (field-number)
// order, unknown fields appended verbatim in input order. Nested payloads use
// a one-byte length placeholder patched afterwards, packed runs use a
// five-byte placeholder shifted down once, and untouched Raw slices copy
// verbatim (tag included), so a decode/encode round trip is byte-exact.
//
// The Dart twin of bindings/java/runtime/GenericCodec.encodeInto.

import 'dart:typed_data';

import 'descriptor.dart';
import 'tree.dart';
import 'wire.dart';

/* One workspace buffer: an encode pays no allocation but the result copy. */
final W _workspace = W();

Uint8List encode(Msg m) {
  final w = _workspace;
  w.pos = 0;
  _encodeInto(m, w, 0);
  return Uint8List.fromList(Uint8List.sublistView(w.b, 0, w.pos));
}

bool _isZero(Object? v) => v == 0 || v == BigInt.zero;

void _encodeInto(Msg m, W w, int depth) {
  if (depth > maxDepth) {
    throw CoreError('nesting exceeds $maxDepth', 4);
  }
  final def = m.def;
  final noRequired = !def.hasRequired; // proto3: skip the check
  final fields = def.fields;
  for (var s = 0; s < fields.length; s++) {
    final v = m.v[s];
    final f = fields[s];
    if (v == null) {
      if (!noRequired && f.presence == presenceRequired) {
        checkPresentRequired(m, s); // throws
      }
      continue;
    }
    if (f.repeated) {
      final arr = v as List<Object?>;
      if (arr.isEmpty) {
        continue;
      }
      if (isPackable(f.kind)) {
        _encodeRepNum(w, f, arr);
      } else {
        for (var i = 0; i < arr.length; i++) {
          _writeValue(w, f, arr[i], depth);
        }
      }
      continue;
    }
    if (isPackable(f.kind)) {
      // Only implicit defaults skip here; explicit defaults (and required)
      // emit once set.
      if (_isZero(v) && f.presence == presenceImplicit) {
        continue;
      }
      _writeSingularNum(w, f, v);
      continue;
    }
    // Implicit empty strings and bytes stay silent, everything else emits.
    if (f.presence == presenceImplicit) {
      if (v is String && v.isEmpty) {
        continue;
      }
      if (v is Uint8List && v.isEmpty) {
        continue;
      }
    }
    _writeValue(w, f, v, depth);
  }
  final unknown = m.unknown;
  if (unknown != null) {
    // Unknown fields last, in input order.
    for (final u in unknown) {
      ensure(w, u.length);
      w.b.setRange(w.pos, w.pos + u.length, u);
      w.pos += u.length;
    }
  }
}

void _writeValue(W w, Field f, Object? v, int depth) {
  if (v is Raw) {
    // Tag included: a verbatim copy stays byte-exact.
    ensure(w, v.len);
    w.b.setRange(w.pos, w.pos + v.len, Uint8List.sublistView(v.b, v.off, v.off + v.len));
    w.pos += v.len;
    return;
  }
  switch (f.kind) {
    case K.string:
      final s = v as String;
      final n = utf8Length(s);
      ensure(w, f.tag.length + 5 + n);
      var p = w.pos;
      final b = w.b;
      for (var i = 0; i < f.tag.length; i++) {
        b[p++] = f.tag[i];
      }
      var x = n;
      while (x > 0x7f) {
        b[p++] = (x % 128) | 0x80;
        x = x ~/ 128;
      }
      b[p++] = x;
      w.pos = p;
      utf8Write(w, s);
      return;
    case K.bytes:
      wBytes(w, f.tag, v as Uint8List);
      return;
    case K.message:
      // Inlined tag + length placeholder + patch: a repeated message field
      // is the hottest nesting shape.
      final tag = f.tag;
      ensure(w, tag.length + 1);
      var p = w.pos;
      final b = w.b;
      for (var i = 0; i < tag.length; i++) {
        b[p++] = tag[i];
      }
      final lp = p;
      b[p++] = 0;
      w.pos = p;
      _encodeInto(v as Msg, w, depth + 1);
      patchLen(w, lp);
      return;
    default:
      // K.group: start tag, body, end tag.
      wTag(w, f.tag);
      _encodeInto(v as Msg, w, depth + 1);
      wTag(w, f.tagEnd as Uint8List);
      return;
  }
}

/// Singular numeric write: one fused tag+value call per field.
void _writeSingularNum(W w, Field f, Object? v) {
  switch (f.kind) {
    case K.int32:
    case K.enum_:
      // Encode the int32 interpretation, sign-extended: the reference
      // truncates a wide varint to int32 and re-emits a negative as 10 bytes.
      final x = v as int;
      if (x >= 0) {
        wTagVar32(w, f.tag, x);
      } else {
        wTagVar64(w, f.tag, BigInt.from(x));
      }
      return;
    case K.uint32:
      wTagVar32(w, f.tag, v as int);
      return;
    case K.sint32:
      wTagVar32(w, f.tag, zig32(v as int));
      return;
    case K.int64:
    case K.uint64:
      wTagVar64(w, f.tag, v as BigInt);
      return;
    case K.sint64:
      wTagVar64(w, f.tag, zig64(v as BigInt));
      return;
    case K.bool_:
      wTagVar32(w, f.tag, (v as int) == 0 ? 0 : 1);
      return;
    case K.fixed32:
    case K.sfixed32:
    case K.float:
      wTagFixed32(w, f.tag, v as int);
      return;
    default: // K.fixed64, K.sfixed64, K.double_
      wTagFixed64(w, f.tag, v as BigInt);
      return;
  }
}

/// One dispatch per field; the loops below it are branch-free writes.
void _encodeRepNum(W w, Field f, List<Object?> arr) {
  final n = arr.length;
  if (f.packed) {
    if (f.kind == K.bool_) {
      wTag(w, f.tag);
      wVar32(w, n);
      ensure(w, n);
      for (var i = 0; i < n; i++) {
        w.b[w.pos++] = (arr[i] as int) == 0 ? 0 : 1;
      }
      return;
    }
    if (f.kind == K.fixed32 || f.kind == K.sfixed32 || f.kind == K.float) {
      wTag(w, f.tag);
      wVar32(w, n * 4);
      ensure(w, n * 4);
      for (var i = 0; i < n; i++) {
        wFixed32(w, arr[i] as int);
      }
      return;
    }
    if (f.kind == K.fixed64 || f.kind == K.sfixed64 || f.kind == K.double_) {
      wTag(w, f.tag);
      wVar32(w, n * 8);
      ensure(w, n * 8);
      for (var i = 0; i < n; i++) {
        wFixed64(w, arr[i] as BigInt);
      }
      return;
    }
    // Varint kinds: write the payload over a 5-byte placeholder, then shift it
    // down: one move beats a size pre-pass over thousands of elements.
    wTag(w, f.tag);
    final lp = w.pos;
    ensure(w, n * 10 + 5);
    w.pos += 5;
    for (var i = 0; i < n; i++) {
      _writePackedVar(w, f.kind, arr[i]);
    }
    final len = w.pos - lp - 5;
    final llen = sizeVar32(len);
    // Dest is below the source, so the copy runs ascending (memmove order).
    for (var i = lp + 5; i < w.pos; i++) {
      w.b[i - (5 - llen)] = w.b[i];
    }
    var p = lp;
    var x = len;
    while (x > 0x7f) {
      w.b[p++] = (x % 128) | 0x80;
      x = x ~/ 128;
    }
    w.b[p++] = x;
    w.pos = lp + llen + len;
    return;
  }
  for (var i = 0; i < n; i++) {
    wTag(w, f.tag);
    _writeNumSingle(w, f.kind, arr[i]);
  }
}

void _writePackedVar(W w, int kind, Object? v) {
  switch (kind) {
    case K.int32:
    case K.enum_:
      final x = v as int;
      if (x >= 0) {
        wVar32(w, x);
      } else {
        wVar64(w, BigInt.from(x));
      }
      return;
    case K.uint32:
      wVar32(w, v as int);
      return;
    case K.sint32:
      wVar32(w, zig32(v as int));
      return;
    case K.int64:
    case K.uint64:
      wVar64(w, v as BigInt);
      return;
    case K.sint64:
      wVar64(w, zig64(v as BigInt));
      return;
    default:
      throw CoreError('not packable');
  }
}

void _writeNumSingle(W w, int kind, Object? v) {
  switch (kind) {
    case K.int32:
    case K.enum_:
      final x = v as int;
      if (x >= 0) {
        wVar32(w, x);
      } else {
        wVar64(w, BigInt.from(x));
      }
      return;
    case K.uint32:
      wVar32(w, v as int);
      return;
    case K.sint32:
      wVar32(w, zig32(v as int));
      return;
    case K.int64:
    case K.uint64:
      wVar64(w, v as BigInt);
      return;
    case K.sint64:
      wVar64(w, zig64(v as BigInt));
      return;
    case K.bool_:
      ensure(w, 1);
      w.b[w.pos++] = (v as int) == 0 ? 0 : 1;
      return;
    case K.fixed32:
    case K.sfixed32:
    case K.float:
      wFixed32(w, v as int);
      return;
    default: // K.fixed64, K.sfixed64, K.double_
      wFixed64(w, v as BigInt);
      return;
  }
}

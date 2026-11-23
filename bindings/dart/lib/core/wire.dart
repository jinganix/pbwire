// Protobuf wire primitives for the pure-Dart core: varint/tag/little-endian
// read and write, zigzag, skipping (with group frames) and the structural
// UTF-8 check. One home for everything the descriptor reader, the encoder and
// the decoder do to bytes, so they cannot diverge.
//
// The rules are the C core's (src/wire.c, src/decode.c): strict tags and
// lengths, unknown fields skippable by shape, groups validated to their
// matching end tag, one nesting cap for groups and messages alike.
//
// Web safety (dart2js/dart2wasm): `int` is 64-bit on the VM but a double on
// the web, so no value that can exceed 2^31 is ever built with `<<`. 32-bit
// kinds stay `int` (their range is exact on every target) and arithmetic that
// would need more uses multiplication/division; the 64-bit kinds are BigInt
// everywhere.

import 'dart:typed_data';

const int wtVarint = 0;
const int wtFixed64 = 1;
const int wtLen = 2;
const int wtStartGroup = 3;
const int wtEndGroup = 4;
const int wtFixed32 = 5;

/// Nesting cap, the same bound as the C core's PBW_MAX_DEPTH.
const int maxDepth = 256;

/// 2^32: `x % u32` is exact on every target where `1 << 32` is not.
const int u32 = 4294967296;

/// A pbwire failure: the C core's pbw_status where one exists, else -1.
final class CoreError implements Exception {
  CoreError(this.message, [this.status = -1]);

  final String message;
  final int status;

  @override
  String toString() => message;
}

/// Growable output buffer, reset per encode (the C core's pbw_ctx twin).
final class W {
  Uint8List b = Uint8List(256);
  int pos = 0;

  void reset() {
    pos = 0;
  }
}

/// Cursor over an input slice.
final class R {
  R(this.b, [int pos = 0, int? end]) : pos = pos, end = end ?? b.length;

  Uint8List b;
  int pos;
  int end;

  /// Nesting cap for this decode; set from Limits at entry.
  int cap = maxDepth;
}

void ensure(W w, int extra) {
  if (w.pos + extra > w.b.length) {
    _grow(w, extra);
  }
}

void _grow(W w, int extra) {
  var n = w.b.length * 2;
  final need = w.pos + extra;
  while (n < need) {
    n *= 2;
  }
  final next = Uint8List(n);
  next.setRange(0, w.pos, w.b);
  w.b = next;
}

/// The (field-number, wire-type) tag as bytes.
Uint8List tagBytes(int fnum, int wt) {
  var tag = fnum * 8 + wt;
  final out = Uint8List(5);
  var n = 0;
  while (tag > 0x7f) {
    out[n++] = (tag % 128) | 0x80;
    tag = tag ~/ 128;
  }
  out[n++] = tag;
  return Uint8List.sublistView(out, 0, n);
}

void wTag(W w, Uint8List tag) {
  ensure(w, tag.length);
  w.b.setRange(w.pos, w.pos + tag.length, tag);
  w.pos += tag.length;
}

/// varint32 write of an unsigned 32-bit value.
void wVar32(W w, int v) {
  ensure(w, 5);
  final b = w.b;
  var p = w.pos;
  var x = v % u32;
  while (x > 0x7f) {
    b[p++] = (x % 128) | 0x80;
    x = x ~/ 128;
  }
  b[p++] = x;
  w.pos = p;
}

/// varint64 write of an unsigned 64-bit value.
void wVar64(W w, BigInt v) {
  ensure(w, 10);
  final b = w.b;
  var p = w.pos;
  var x = v.toUnsigned(64);
  final m127 = BigInt.from(0x7f);
  while (x > m127) {
    b[p++] = (x & m127).toInt() | 0x80;
    x = x >> 7;
  }
  b[p++] = x.toInt();
  w.pos = p;
}

void wFixed32(W w, int v) {
  ensure(w, 4);
  final b = w.b;
  var p = w.pos;
  var x = v % u32;
  b[p++] = x % 256;
  x = x ~/ 256;
  b[p++] = x % 256;
  x = x ~/ 256;
  b[p++] = x % 256;
  b[p++] = x ~/ 256 % 256;
  w.pos = p;
}

void wFixed64(W w, BigInt v) {
  ensure(w, 8);
  final b = w.b;
  var p = w.pos;
  var x = v.toUnsigned(64);
  final m255 = BigInt.from(0xff);
  for (var i = 0; i < 8; i++) {
    b[p++] = (x & m255).toInt();
    x = x >> 8;
  }
  w.pos = p;
}

/// Fused tag+varint32: one capacity check per field.
void wTagVar32(W w, Uint8List tag, int v) {
  ensure(w, tag.length + 5);
  final b = w.b;
  var p = w.pos;
  for (var i = 0; i < tag.length; i++) {
    b[p++] = tag[i];
  }
  var x = v % u32;
  while (x > 0x7f) {
    b[p++] = (x % 128) | 0x80;
    x = x ~/ 128;
  }
  b[p++] = x;
  w.pos = p;
}

void wTagVar64(W w, Uint8List tag, BigInt v) {
  ensure(w, tag.length + 10);
  final b = w.b;
  var p = w.pos;
  for (var i = 0; i < tag.length; i++) {
    b[p++] = tag[i];
  }
  var x = v.toUnsigned(64);
  final m127 = BigInt.from(0x7f);
  while (x > m127) {
    b[p++] = (x & m127).toInt() | 0x80;
    x = x >> 7;
  }
  b[p++] = x.toInt();
  w.pos = p;
}

void wTagFixed32(W w, Uint8List tag, int v) {
  ensure(w, tag.length + 4);
  final b = w.b;
  var p = w.pos;
  for (var i = 0; i < tag.length; i++) {
    b[p++] = tag[i];
  }
  var x = v % u32;
  b[p++] = x % 256;
  x = x ~/ 256;
  b[p++] = x % 256;
  x = x ~/ 256;
  b[p++] = x % 256;
  b[p++] = x ~/ 256 % 256;
  w.pos = p;
}

void wTagFixed64(W w, Uint8List tag, BigInt v) {
  ensure(w, tag.length + 8);
  final b = w.b;
  var p = w.pos;
  for (var i = 0; i < tag.length; i++) {
    b[p++] = tag[i];
  }
  var x = v.toUnsigned(64);
  final m255 = BigInt.from(0xff);
  for (var i = 0; i < 8; i++) {
    b[p++] = (x & m255).toInt();
    x = x >> 8;
  }
  w.pos = p;
}

/// Length-delimited payload under `tag` (string/bytes/message).
void wBytes(W w, Uint8List? tag, Uint8List v) {
  ensure(w, (tag == null ? 0 : tag.length) + 5 + v.length);
  final b = w.b;
  var p = w.pos;
  if (tag != null) {
    for (var i = 0; i < tag.length; i++) {
      b[p++] = tag[i];
    }
  }
  var n = v.length;
  while (n > 0x7f) {
    b[p++] = (n % 128) | 0x80;
    n = n ~/ 128;
  }
  b[p++] = n;
  b.setRange(p, p + v.length, v);
  w.pos = p + v.length;
}

/// Fills a 1-byte length placeholder written at `lp`: the single-byte fast
/// path, or a memmove when the length needs more bytes.
void patchLen(W w, int lp) {
  final len = w.pos - lp - 1;
  if (len < 128) {
    w.b[lp] = len;
    return;
  }
  final lb = Uint8List(5);
  var n = 0;
  var v = len;
  while (v > 0x7f) {
    lb[n++] = (v % 128) | 0x80;
    v = v ~/ 128;
  }
  lb[n++] = v;
  ensure(w, n - 1);
  // Manual backward shift: a shared-buffer setRange is not guaranteed to
  // copy backwards on every target.
  for (var i = w.pos - 1; i >= lp + 1; i--) {
    w.b[i + n - 1] = w.b[i];
  }
  w.b.setRange(lp, lp + n, lb);
  w.pos += n - 1;
}

int sizeVar32(int v) {
  var n = 1;
  var x = v % u32;
  while (x > 0x7f) {
    x = x ~/ 128;
    n++;
  }
  return n;
}

/// varint64 of an unsigned value.
int sizeVar64(BigInt v) {
  var n = 1;
  var x = v.toUnsigned(64);
  final m127 = BigInt.from(0x7f);
  while (x > m127) {
    x = x >> 7;
    n++;
  }
  return n;
}

/// int32: negatives sign-extend to 10 bytes, same as libprotobuf.
int sizeInt32(int v) => v >= 0 ? sizeVar32(v) : 10;

/// zigzag32 without a 32-bit shift (exact on the web).
int zig32(int v) => v >= 0 ? 2 * v : -2 * v - 1;

BigInt zig64(BigInt v) => ((v << 1) ^ (v >> 63)).toUnsigned(64);

int unzig32(int v) => (v % 2) == 1 ? -((v ~/ 2) + 1) : v ~/ 2;

BigInt unzig64(BigInt v) => ((v >> 1) ^ -(v & BigInt.one)).toSigned(64);

/* ---------------- reading ---------------- */

/// varint, low 32 bits (unsigned). Validates the encoding's length only.
int rVar(R r) {
  final b = r.b;
  final end = r.end;
  var p = r.pos;
  var lo = 0;
  var mul = 1;
  var j = 0;
  while (true) {
    if (p >= end) {
      throw CoreError('truncated varint');
    }
    final c = b[p++];
    if (j < 5) {
      // The 32-bit cut discards the high bits on purpose: only the low 32
      // are ever visible for the 32-bit kinds.
      lo += (c & 0x7f) * mul;
      mul *= 128;
    }
    if ((c & 0x80) == 0) {
      r.pos = p;
      return lo % u32;
    }
    j++;
    if (j >= 10) {
      throw CoreError('varint overflow');
    }
  }
}

/// varint, full 64 bits (unsigned).
BigInt rVar64(R r) {
  final b = r.b;
  final end = r.end;
  var p = r.pos;
  var v = BigInt.zero;
  var shift = 0;
  while (true) {
    if (p >= end) {
      throw CoreError('truncated varint');
    }
    final c = b[p++];
    v = v | (BigInt.from(c & 0x7f) << shift);
    if ((c & 0x80) == 0) {
      r.pos = p;
      return v.toUnsigned(64);
    }
    shift += 7;
    if (shift >= 64) {
      throw CoreError('varint overflow');
    }
  }
}

/// varint, reduced to its bool value: any set payload bit means true.
int rVarBool(R r) {
  final b = r.b;
  final end = r.end;
  var p = r.pos;
  var nz = 0;
  var j = 0;
  while (true) {
    if (p >= end) {
      throw CoreError('truncated varint');
    }
    final c = b[p++];
    nz |= c & 0x7f;
    if ((c & 0x80) == 0) {
      r.pos = p;
      return nz != 0 ? 1 : 0;
    }
    j++;
    if (j >= 10) {
      throw CoreError('varint overflow');
    }
  }
}

/// Strict length read, like the core's length reader: at most five bytes and
/// the fifth only carries four bits; an out-of-range length is rejected, not
/// truncated. Leaves r.pos after the length.
int rLen32(R r) {
  final b = r.b;
  final end = r.end;
  var p = r.pos;
  if (p >= end) {
    throw CoreError('truncated length');
  }
  var c = b[p++];
  var v = c & 0x7f;
  var mul = 128;
  var shift = 7;
  while ((c & 0x80) != 0) {
    if (shift >= 35 || p >= end) {
      throw CoreError('bad length');
    }
    c = b[p++];
    if (shift == 28 && (c & 0x7f) > 0x0f) {
      throw CoreError('length overflow');
    }
    v += (c & 0x7f) * mul;
    mul *= 128;
    shift += 7;
  }
  if (v < 0 || p + v > end) {
    throw CoreError('truncated length-delimited');
  }
  r.pos = p;
  return v;
}

/// Loose length read for descriptor parsing: any varint up to 2^31-1, bounds
/// checked.
int rLen(R r) {
  final v = rVar64(r);
  if (v > BigInt.from(0x7fffffff)) {
    throw CoreError('length overflow');
  }
  final n = v.toInt();
  if (r.pos + n > r.end) {
    throw CoreError('truncated length-delimited');
  }
  return n;
}

/// Strict tag read, like the core's tag reader: a varint32 of at most five
/// bytes. A 5-byte tag above UINT32_MAX truncates instead of failing, exactly
/// what the reference parser does (the official suite expects two such cases
/// to parse).
int rTag(R r) {
  final b = r.b;
  final end = r.end;
  var p = r.pos;
  if (p >= end) {
    throw CoreError('truncated tag');
  }
  var c = b[p++];
  var v = c & 0x7f;
  var mul = 128;
  var shift = 7;
  while ((c & 0x80) != 0) {
    if (shift >= 35 || p >= end) {
      throw CoreError('bad tag');
    }
    c = b[p++];
    v += (c & 0x7f) * mul; // 32-bit: truncates like the reference
    mul *= 128;
    shift += 7;
  }
  r.pos = p;
  return v % u32;
}

int rFixed32(R r) {
  if (r.pos + 4 > r.end) {
    throw CoreError('truncated fixed32');
  }
  final b = r.b;
  final p = r.pos;
  r.pos = p + 4;
  return b[p] + b[p + 1] * 256 + b[p + 2] * 65536 + b[p + 3] * 16777216;
}

/// Raw 64 bits, unsigned.
BigInt rFixed64(R r) {
  if (r.pos + 8 > r.end) {
    throw CoreError('truncated fixed64');
  }
  final b = r.b;
  final p = r.pos;
  r.pos = p + 8;
  final lo = b[p] + b[p + 1] * 256 + b[p + 2] * 65536 + b[p + 3] * 16777216;
  final hi =
      b[p + 4] + b[p + 5] * 256 + b[p + 6] * 65536 + b[p + 7] * 16777216;
  return BigInt.from(lo) | (BigInt.from(hi) << 32);
}

/// Skips one field value of a non-group wire type.
void skip(R r, int wt) {
  switch (wt) {
    case wtVarint:
      rVarBool(r);
      return;
    case wtFixed64:
      if (r.pos + 8 > r.end) {
        throw CoreError('truncated fixed64');
      }
      r.pos += 8;
      return;
    case wtLen:
      final n = rLen32(r);
      r.pos += n;
      return;
    case wtFixed32:
      if (r.pos + 4 > r.end) {
        throw CoreError('truncated fixed32');
      }
      r.pos += 4;
      return;
    default:
      throw CoreError('bad wire type $wt');
  }
}

/// Validates a group body up to its matching end tag, like the core's group
/// skipper: every tag is strict, field 0 and wire types above 5 are rejected,
/// and an end tag only closes the group it names.
void skipGroup(R r, int number, int depth) {
  if (depth > r.cap) {
    throw CoreError('nesting exceeds ${r.cap}', 4);
  }
  while (true) {
    final tag = rTag(r);
    final fnum = tag ~/ 8;
    final wt = tag % 8;
    if (fnum == 0 || wt > wtFixed32) {
      throw CoreError('bad tag $tag');
    }
    if (wt == wtEndGroup) {
      if (fnum != number) {
        throw CoreError('mismatched end-group for $fnum, want $number');
      }
      return;
    }
    if (wt == wtStartGroup) {
      skipGroup(r, fnum, depth + 1);
    } else {
      skip(r, wt);
    }
  }
}

/// Skips one unknown field: groups validate to their matching end tag,
/// anything else only checks value shape. End-group tags never reach here;
/// the decode loop owns them.
void skipUnknown(R r, int fnum, int wt, int depth) {
  if (wt == wtStartGroup) {
    skipGroup(r, fnum, depth + 1);
    return;
  }
  skip(r, wt);
}

/// Structural UTF-8 check: rejects overlong forms, surrogates and values past
/// U+10FFFF, like the core's pbw_utf8_valid and the reference parsers.
bool utf8Valid(Uint8List b, int off, int len) {
  var i = off;
  final end = off + len;
  while (i < end) {
    final c = b[i];
    if (c < 0x80) {
      i++;
    } else if ((c & 0xe0) == 0xc0) {
      if (c < 0xc2 || i + 1 >= end || (b[i + 1] & 0xc0) != 0x80) {
        return false;
      }
      i += 2;
    } else if ((c & 0xf0) == 0xe0) {
      if (i + 2 >= end ||
          (b[i + 1] & 0xc0) != 0x80 ||
          (b[i + 2] & 0xc0) != 0x80) {
        return false;
      }
      final v = ((c & 15) << 12) | ((b[i + 1] & 63) << 6) | (b[i + 2] & 63);
      if (v < 0x800 || (v >= 0xd800 && v <= 0xdfff)) {
        return false;
      }
      i += 3;
    } else if ((c & 0xf8) == 0xf0) {
      if (c > 0xf4 ||
          i + 3 >= end ||
          (b[i + 1] & 0xc0) != 0x80 ||
          (b[i + 2] & 0xc0) != 0x80 ||
          (b[i + 3] & 0xc0) != 0x80) {
        return false;
      }
      final v = ((c & 7) << 18) |
          ((b[i + 1] & 63) << 12) |
          ((b[i + 2] & 63) << 6) |
          (b[i + 3] & 63);
      if (v < 0x10000 || v > 0x10ffff) {
        return false;
      }
      i += 4;
    } else {
      return false;
    }
  }
  return true;
}

/// UTF-8 length of a Dart string; lone surrogates count as U+FFFD.
int utf8Length(String s) {
  var n = 0;
  for (var i = 0; i < s.length; i++) {
    final c = s.codeUnitAt(i);
    if (c < 0x80) {
      n += 1;
    } else if (c < 0x800) {
      n += 2;
    } else if (c >= 0xd800 && c <= 0xdbff && i + 1 < s.length) {
      final d = s.codeUnitAt(i + 1);
      if (d >= 0xdc00 && d <= 0xdfff) {
        n += 4;
        i++;
      } else {
        n += 3; // lone high surrogate: U+FFFD
      }
    } else {
      n += 3;
    }
  }
  return n;
}

/// Writes `s` as UTF-8 at w.pos (capacity must already be ensured).
void utf8Write(W w, String s) {
  final b = w.b;
  var p = w.pos;
  for (var i = 0; i < s.length; i++) {
    var c = s.codeUnitAt(i);
    if (c < 0x80) {
      b[p++] = c;
    } else if (c < 0x800) {
      b[p++] = 0xc0 | (c >> 6);
      b[p++] = 0x80 | (c & 0x3f);
    } else if (c >= 0xd800 && c <= 0xdfff) {
      // A surrogate pair is one code point; a lone surrogate is not a
      // character and encodes as U+FFFD, like the reference.
      final d = i + 1 < s.length ? s.codeUnitAt(i + 1) : 0;
      if (c <= 0xdbff && d >= 0xdc00 && d <= 0xdfff) {
        c = 0x10000 + ((c - 0xd800) << 10) + (d - 0xdc00);
        i++;
        b[p++] = 0xf0 | (c >> 18);
        b[p++] = 0x80 | ((c >> 12) & 0x3f);
        b[p++] = 0x80 | ((c >> 6) & 0x3f);
        b[p++] = 0x80 | (c & 0x3f);
      } else {
        b[p++] = 0xef;
        b[p++] = 0xbf;
        b[p++] = 0xbd;
      }
    } else {
      b[p++] = 0xe0 | (c >> 12);
      b[p++] = 0x80 | ((c >> 6) & 0x3f);
      b[p++] = 0x80 | (c & 0x3f);
    }
  }
  w.pos = p;
}

/// UTF-8 decodes a slice, replacing each malformed byte with U+FFFD.
String utf8Decode(Uint8List b, int off, int len) {
  final end = off + len;
  final units = <int>[];
  var i = off;
  while (i < end) {
    final c = b[i];
    if (c < 0x80) {
      units.add(c);
      i++;
    } else if ((c & 0xe0) == 0xc0 && i + 1 < end && (b[i + 1] & 0xc0) == 0x80) {
      units.add(((c & 0x1f) << 6) | (b[i + 1] & 0x3f));
      i += 2;
    } else if ((c & 0xf0) == 0xe0 &&
        i + 2 < end &&
        (b[i + 1] & 0xc0) == 0x80 &&
        (b[i + 2] & 0xc0) == 0x80) {
      final v = ((c & 0x0f) << 12) | ((b[i + 1] & 0x3f) << 6) | (b[i + 2] & 0x3f);
      if (v < 0x800 || (v >= 0xd800 && v <= 0xdfff)) {
        units.add(0xfffd); // overlong or surrogate: one malformed unit
      } else {
        units.add(v);
      }
      i += 3;
    } else if ((c & 0xf8) == 0xf0 &&
        i + 3 < end &&
        (b[i + 1] & 0xc0) == 0x80 &&
        (b[i + 2] & 0xc0) == 0x80 &&
        (b[i + 3] & 0xc0) == 0x80) {
      var v = ((c & 7) << 18) |
          ((b[i + 1] & 0x3f) << 12) |
          ((b[i + 2] & 0x3f) << 6) |
          (b[i + 3] & 0x3f);
      if (v < 0x10000 || v > 0x10ffff) {
        units.add(0xfffd);
      } else {
        v -= 0x10000;
        units.add(0xd800 + (v >> 10));
        units.add(0xdc00 + (v & 0x3ff));
      }
      i += 4;
    } else {
      units.add(0xfffd);
      i++;
    }
  }
  return String.fromCharCodes(units);
}

/* ---------------- float/double bit patterns ---------------- */

final ByteData _f32 = ByteData(4);

/// The float32 bit pattern of a double (IEEE round-to-nearest).
int floatBits(double v) {
  _f32.setFloat32(0, v);
  return _f32.getUint32(0);
}

double bitsFloat(int bits) {
  _f32.setUint32(0, bits % u32);
  return _f32.getFloat32(0);
}

final ByteData _f64 = ByteData(8);

/// The float64 bit pattern of a double, as raw 64 bits.
BigInt doubleBits(double v) {
  _f64.setFloat64(0, v);
  return BigInt.from(_f64.getUint32(0)) << 32 | BigInt.from(_f64.getUint32(4));
}

double bitsDouble(BigInt bits) {
  final x = bits.toUnsigned(64);
  _f64.setUint32(0, (x >> 32).toInt());
  _f64.setUint32(4, (x & BigInt.from(0xffffffff)).toInt());
  return _f64.getFloat64(0);
}

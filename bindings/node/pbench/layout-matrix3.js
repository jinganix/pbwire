'use strict';

/* Flat-image layout for structtest.Scalars3 (matrix3 spike).
 *
 * One ArrayBuffer holds the whole message, so the addon crosses N-API once
 * per op and walks the image in C instead of reading one JS property per
 * field. Field order matches payload.js nativeObject throughout.
 *
 * Layout (little-endian; the C side assembles multi-byte values byte by
 * byte, so no alignment is assumed):
 *
 *   Fixed header (83 bytes):
 *     0   i32    int32   4
 *     4   i64    int64   8
 *     12  u32    uint32  4
 *     16  u64    uint64  8
 *     24  s32    int32   4
 *     28  s64    int64   8
 *     36  fx32   uint32  4
 *     40  fx64   uint64  8
 *     48  sfx32  int32   4
 *     52  sfx64  int64   8
 *     60  f32    float32 4
 *     64  f64    float64 8
 *     72  flag   u8      1
 *     73  color  int32   4
 *     77  opt_i32 present u8 1 (0 = absent, else the int32 at 78 applies)
 *     78  opt_i32 int32  4
 *     82  opt_str present u8 1 (0 = absent, else a tail payload follows)
 *   Variable tail (cursor from 83), in field order:
 *     str:    u32 length + UTF-8 bytes
 *     inner:  u32 name length + name bytes + int32 value (always present)
 *     nums:   u32 count + int32 each
 *     tags:   u32 count + (u32 length + bytes) each
 *     inners: u32 count + (u32 name length + name bytes + int32 value) each
 *     opt_str (only when present): u32 length + bytes
 *     flags:  u32 count + u8 each (0/1)
 *
 * Spike limits (documented, not bugs to fix here): every singular field
 * except the two proto3 optionals is assumed present (the matrix3 payload
 * carries them all); an absent singular decodes as zero/empty rather than
 * undefined. encodeImage rejects no unknown data beyond a trailing-size
 * check: the image is positional, so there are no names to reject.
 */

const HEADER_LEN = 83;

const OFF = {
  i32: 0,
  i64: 4,
  u32: 12,
  u64: 16,
  s32: 24,
  s64: 28,
  fx32: 36,
  fx64: 40,
  sfx32: 48,
  sfx64: 52,
  f32: 60,
  f64: 64,
  flag: 72,
  color: 73,
  optI32Present: 77,
  optI32: 78,
  optStrPresent: 82,
};

const encoder = new TextEncoder();
const decoder = new TextDecoder();

/** Build the flat image of a nativeObject-shaped value. Returns ArrayBuffer. */
function encode(obj) {
  const strB = encoder.encode(obj.str ?? '');
  if (obj.inner === undefined || obj.inner === null) {
    throw new TypeError('layout-matrix3: inner is required');
  }
  const innerNameB = encoder.encode(obj.inner.name ?? '');
  const nums = obj.nums ?? [];
  const tags = obj.tags ?? [];
  const tagsB = tags.map((t) => encoder.encode(t));
  const inners = obj.inners ?? [];
  const innersB = inners.map((k) => encoder.encode(k.name ?? ''));
  const hasOptStr = obj.opt_str !== undefined;
  const optStrB = hasOptStr ? encoder.encode(obj.opt_str) : null;
  const flags = obj.flags ?? [];
  let len = HEADER_LEN;
  len += 4 + strB.length;
  len += 4 + innerNameB.length + 4;
  len += 4 + nums.length * 4;
  len += 4;
  for (const b of tagsB) {
    len += 4 + b.length;
  }
  len += 4;
  for (let i = 0; i < inners.length; i++) {
    len += 4 + innersB[i].length + 4;
  }
  if (optStrB !== null) {
    len += 4 + optStrB.length;
  }
  len += 4 + flags.length;
  const out = new ArrayBuffer(len);
  const head = new DataView(out, 0, HEADER_LEN);
  head.setInt32(OFF.i32, obj.i32, true);
  head.setBigInt64(OFF.i64, BigInt(obj.i64), true);
  head.setUint32(OFF.u32, obj.u32, true);
  head.setBigUint64(OFF.u64, BigInt(obj.u64), true);
  head.setInt32(OFF.s32, obj.s32, true);
  head.setBigInt64(OFF.s64, BigInt(obj.s64), true);
  head.setUint32(OFF.fx32, obj.fx32, true);
  head.setBigUint64(OFF.fx64, BigInt(obj.fx64), true);
  head.setInt32(OFF.sfx32, obj.sfx32, true);
  head.setBigInt64(OFF.sfx64, BigInt(obj.sfx64), true);
  head.setFloat32(OFF.f32, obj.f32, true);
  head.setFloat64(OFF.f64, obj.f64, true);
  head.setUint8(OFF.flag, obj.flag ? 1 : 0);
  head.setInt32(OFF.color, obj.color, true);
  head.setUint8(OFF.optI32Present, obj.opt_i32 !== undefined ? 1 : 0);
  head.setInt32(OFF.optI32, obj.opt_i32 ?? 0, true);
  head.setUint8(OFF.optStrPresent, hasOptStr ? 1 : 0);
  const view = new DataView(out);
  const bytes = new Uint8Array(out);
  let pos = HEADER_LEN;
  const putBytes = (b) => {
    view.setUint32(pos, b.length, true);
    bytes.set(b, pos + 4);
    pos += 4 + b.length;
  };
  putBytes(strB);
  view.setUint32(pos, innerNameB.length, true);
  bytes.set(innerNameB, pos + 4);
  pos += 4 + innerNameB.length;
  view.setInt32(pos, obj.inner.value, true);
  pos += 4;
  view.setUint32(pos, nums.length, true);
  pos += 4;
  for (const n of nums) {
    view.setInt32(pos, n, true);
    pos += 4;
  }
  view.setUint32(pos, tagsB.length, true);
  pos += 4;
  for (const b of tagsB) {
    putBytes(b);
  }
  view.setUint32(pos, inners.length, true);
  pos += 4;
  for (let i = 0; i < inners.length; i++) {
    view.setUint32(pos, innersB[i].length, true);
    bytes.set(innersB[i], pos + 4);
    pos += 4 + innersB[i].length;
    view.setInt32(pos, inners[i].value, true);
    pos += 4;
  }
  if (optStrB !== null) {
    putBytes(optStrB);
  }
  view.setUint32(pos, flags.length, true);
  pos += 4;
  for (const f of flags) {
    view.setUint8(pos, f ? 1 : 0);
    pos += 1;
  }
  if (pos !== len) {
    throw new Error('layout-matrix3: size miscount');
  }
  return out;
}

/** Read a flat image back into a nativeObject-shaped value. */
function decode(buffer) {
  const view = new DataView(buffer);
  const bytes = new Uint8Array(buffer);
  const takeBytes = (at, n) => bytes.subarray(at, at + n);
  const get = (pos, n) => {
    if (pos + n > bytes.length) {
      throw new RangeError('layout-matrix3: image too small');
    }
    return pos;
  };
  if (bytes.length < HEADER_LEN) {
    throw new RangeError('layout-matrix3: image too small');
  }
  const head = new DataView(buffer, 0, HEADER_LEN);
  const hasOptStr = head.getUint8(OFF.optStrPresent) !== 0;
  let pos = HEADER_LEN;
  const takeString = () => {
    get(pos, 4);
    const n = view.getUint32(pos, true);
    get(pos + 4, n);
    const s = decoder.decode(takeBytes(pos + 4, n));
    pos += 4 + n;
    return s;
  };
  const str = takeString();
  get(pos, 4);
  const innerNameLen = view.getUint32(pos, true);
  get(pos + 4, innerNameLen + 4);
  const innerName = decoder.decode(takeBytes(pos + 4, innerNameLen));
  pos += 4 + innerNameLen;
  const innerValue = view.getInt32(pos, true);
  pos += 4;
  get(pos, 4);
  const numsCount = view.getUint32(pos, true);
  get(pos + 4, numsCount * 4);
  pos += 4;
  const nums = [];
  for (let i = 0; i < numsCount; i++) {
    nums.push(view.getInt32(pos, true));
    pos += 4;
  }
  get(pos, 4);
  const tagsCount = view.getUint32(pos, true);
  pos += 4;
  const tags = [];
  for (let i = 0; i < tagsCount; i++) {
    tags.push(takeString());
  }
  get(pos, 4);
  const innersCount = view.getUint32(pos, true);
  pos += 4;
  const inners = [];
  for (let i = 0; i < innersCount; i++) {
    get(pos, 4);
    const n = view.getUint32(pos, true);
    get(pos + 4, n + 4);
    const name = decoder.decode(takeBytes(pos + 4, n));
    pos += 4 + n;
    const value = view.getInt32(pos, true);
    pos += 4;
    inners.push({ name, value });
  }
  let optStr;
  if (hasOptStr) {
    optStr = takeString();
  }
  get(pos, 4);
  const flagsCount = view.getUint32(pos, true);
  get(pos + 4, flagsCount);
  pos += 4;
  const flags = [];
  for (let i = 0; i < flagsCount; i++) {
    flags.push(view.getUint8(pos) !== 0);
    pos += 1;
  }
  const obj = {
    i32: head.getInt32(OFF.i32, true),
    i64: head.getBigInt64(OFF.i64, true),
    u32: head.getUint32(OFF.u32, true),
    u64: head.getBigUint64(OFF.u64, true),
    s32: head.getInt32(OFF.s32, true),
    s64: head.getBigInt64(OFF.s64, true),
    fx32: head.getUint32(OFF.fx32, true),
    fx64: head.getBigUint64(OFF.fx64, true),
    sfx32: head.getInt32(OFF.sfx32, true),
    sfx64: head.getBigInt64(OFF.sfx64, true),
    f32: head.getFloat32(OFF.f32, true),
    f64: head.getFloat64(OFF.f64, true),
    flag: head.getUint8(OFF.flag) !== 0,
    str,
    color: head.getInt32(OFF.color, true),
    inner: { name: innerName, value: innerValue },
    nums,
    tags,
    inners,
    flags,
  };
  if (head.getUint8(OFF.optI32Present) !== 0) {
    obj.opt_i32 = head.getInt32(OFF.optI32, true);
  }
  if (optStr !== undefined) {
    obj.opt_str = optStr;
  }
  return obj;
}

module.exports = { HEADER_LEN, OFF, encode, decode };

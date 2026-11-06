'use strict';

/* pbwire compat runtime: the verify/fromObject/toObject engine every generated
 * facade delegates to. Schemas are cached per embedded descriptor set, so any
 * number of generated files share this module with no global-state surprises:
 * one entry per distinct FileDescriptorSet, keyed by its own bytes.
 */

const { Schema } = require('./pbwire.js');

const _schemas = new Map();
function _type(descB64, full) {
  let byName = _schemas.get(descB64);
  if (byName === undefined) {
    byName = new Map();
    byName.set('', Schema.fromBytes(Buffer.from(descB64, 'base64')));
    _schemas.set(descB64, byName);
  }
  let t = byName.get(full);
  if (t === undefined) {
    t = byName.get('').message(full);
    byName.set(full, t);
  }
  return t;
}

function _isLongLike(v) {
  return typeof v === 'object' && v !== null &&
    Number.isInteger(v.low) && Number.isInteger(v.high);
}

function _longToBigInt(v) {
  return (BigInt(v.high >>> 0) << 32n) | BigInt(v.low >>> 0);
}

// _toBigInt normalizes an int64-ish value for the encoder. BigInt passes
// through, numbers pass through (the addon casts them, same as plain
// objects), numeric strings and Long-like {low, high} become BigInt.
function _toBigInt(v, path, unsigned) {
  if (typeof v === 'bigint') {
    // fall through to the range check below
  } else if (typeof v === 'number') {
    if (!Number.isInteger(v)) {
      throw new TypeError('pbwire: ' + path + ' is not an integer');
    }
    return v;
  } else if (typeof v === 'string') {
    if (!/^-?\d+$/.test(v)) {
      throw new TypeError('pbwire: ' + path + ' is not an integer string');
    }
    v = BigInt(v);
  } else if (_isLongLike(v)) {
    v = _longToBigInt(v);
  } else {
    throw new TypeError('pbwire: ' + path + ' is not an int64 value');
  }
  if (unsigned) {
    if (v < 0n || v > 18446744073709551615n) {
      throw new TypeError('pbwire: ' + path + ' out of uint64 range');
    }
  } else if (v < -9223372036854775808n || v > 9223372036854775807n) {
    throw new TypeError('pbwire: ' + path + ' out of int64 range');
  }
  return v;
}

// Shape check only (any BigInt passes); range is enforced when the value
// is normalized for the encoder in _toBigInt.
function _checkInt64(v) {
  if (typeof v === 'bigint') {
    return true;
  }
  if (typeof v === 'number') {
    return Number.isInteger(v);
  }
  if (typeof v === 'string') {
    return /^-?\d+$/.test(v);
  }
  return _isLongLike(v);
}

// _verifyField returns null when v is valid, else a reason string.
function _verifyField(fd, v, path, enums, classes) {
  if (fd.rep) {
    if (!Array.isArray(v)) {
      return path + ': array expected';
    }
    for (let i = 0; i < v.length; i++) {
      const bad = _verifyOne(fd, v[i], path + '[' + i + ']', enums, classes);
      if (bad !== null) {
        return bad;
      }
    }
    return null;
  }
  return _verifyOne(fd, v, path, enums, classes);
}

function _verifyOne(fd, v, path, enums, classes) {
  switch (fd.k) {
  // Reason strings mirror protobufjs verify() verbatim so mixed code reads
  // the same errors from either library. Like protobufjs, 32-bit checks are
  // integer-only (range truncation happens in the encoder, identically).
  case 'i32':
  case 's32':
  case 'sfx32':
  case 'u32':
  case 'fx32':
    return Number.isInteger(v) ? null : path + ': integer expected';
  case 'i64':
  case 's64':
  case 'sfx64':
  case 'u64':
  case 'fx64':
    return _checkInt64(v) ? null : path + ': integer|Long expected';
  case 'f32':
  case 'f64':
    return (typeof v === 'number') ? null : path + ': number expected';
  case 'bool':
    return (typeof v === 'boolean') ? null : path + ': boolean expected';
  case 'str':
    return (typeof v === 'string') ? null : path + ': string expected';
  case 'bytes':
    return (typeof v === 'object' && v !== null &&
      (Buffer.isBuffer(v) || v instanceof Uint8Array))
      ? null : path + ': buffer expected';
  case 'enum': {
    const names = enums[fd.en];
    if (typeof v === 'number' && Number.isInteger(v)) {
      // Open enums (proto3) carry unknown values through the round trip.
      if (!fd.closed || names === undefined) {
        return null;
      }
      for (const key in names) {
        if (names[key] === v) {
          return null;
        }
      }
      return path + ': enum value expected';
    }
    return path + ': enum value expected';
  }
  case 'msg':
    if (typeof v !== 'object' || v === null || Array.isArray(v)) {
      return path + ': object expected';
    }
    return classes[fd.msg].verify(v);
  default:
    return null;
  }
}

function _fromOne(fd, v, path, enums, classes) {
  switch (fd.k) {
  case 'i64':
  case 's64':
  case 'sfx64':
    return _toBigInt(v, path, false);
  case 'u64':
  case 'fx64':
    return _toBigInt(v, path, true);
  case 'bytes':
    if (Buffer.isBuffer(v)) {
      return v;
    }
    if (v instanceof Uint8Array) {
      return Buffer.from(v);
    }
    throw new TypeError('pbwire: ' + path + ' is not bytes');
  case 'enum': {
    if (typeof v === 'string') {
      const names = enums[fd.en];
      if (names !== undefined && names[v] !== undefined) {
        return names[v];
      }
      throw new TypeError('pbwire: ' + path + ' unknown enum name ' + v);
    }
    if (typeof v === 'number' && Number.isInteger(v)) {
      return v;
    }
    throw new TypeError('pbwire: ' + path + ' is not an enum value');
  }
  case 'msg':
    if (v !== null && typeof v === 'object' && !(v instanceof classes[fd.msg])) {
      return classes[fd.msg].fromObject(v);
    }
    return v;
  default:
    return v;
  }
}

function _fromValue(fd, v, path, enums, classes) {
  if (v === null || v === undefined) {
    return fd.rep ? [] : null;
  }
  if (fd.rep) {
    if (!Array.isArray(v)) {
      throw new TypeError('pbwire: ' + path + ' is not an array');
    }
    return v.map((item, i) => _fromOne(fd, item, path + '[' + i + ']', enums, classes));
  }
  return _fromOne(fd, v, path, enums, classes);
}

function _toInt64(v, mode) {
  const big = typeof v === 'bigint' ? v
    : typeof v === 'number' ? BigInt(Math.trunc(v))
    : _longToBigInt(v);
  if (mode === 'String') {
    return big.toString();
  }
  if (mode === 'Number') {
    return Number(big);
  }
  return big;
}

function _toOne(fd, v, opts, enums, revEnums) {
  switch (fd.k) {
  case 'i64':
  case 's64':
  case 'sfx64':
  case 'u64':
  case 'fx64':
    return _toInt64(v, opts.longs || 'BigInt');
  case 'enum':
    if ((opts.enums || 'Number') === 'String') {
      const rev = revEnums[fd.en];
      if (rev !== undefined && rev[v] !== undefined) {
        return rev[v];
      }
    }
    return v;
  case 'bytes':
    return (opts.bytes || 'Buffer') === 'base64' ? Buffer.from(v).toString('base64') : v;
  case 'msg':
    return v.toObject(opts);
  default:
    return v;
  }
}

// Output keys are camelCase like protobufjs toObject; keepCase:true keeps
// the proto names (the dynamic API's convention).
function _toPlain(cls, msg, opts, enums, revEnums) {
  const out = {};
  const keyOf = (fd) => (opts.keepCase ? fd.n : fd.js);
  const marked = msg._present instanceof Set ? msg._present : null;
  for (const fd of cls._FIELDS) {
    const v = msg[fd.js];
    if (v === null || v === undefined) {
      continue;
    }
    if (fd.rep && v.length === 0) {
      continue;
    }
    // Unmarked fields are constructor defaults or direct post-hoc writes:
    // include non-default values (direct post-hoc writes work), drop default
    // ones (so create({}) encodes empty). Marked fields always go on the wire.
    if (!opts.defaults && !fd.req && marked !== null &&
        !marked.has(fd.js) && _isDefault(fd, v)) {
      continue;
    }
    out[keyOf(fd)] = fd.rep
      ? v.map((item) => _toOne(fd, item, opts, enums, revEnums))
      : _toOne(fd, v, opts, enums, revEnums);
  }
  if (opts.defaults) {
    for (const fd of cls._FIELDS) {
      if (out[keyOf(fd)] !== undefined) {
        continue;
      }
      if (fd.rep) {
        out[keyOf(fd)] = [];
      } else if (fd.k === 'msg') {
        out[keyOf(fd)] = null;
      } else {
        out[keyOf(fd)] = _toOne(fd, _defaultFor(fd), opts, enums, revEnums);
      }
    }
  }
  return out;
}

function _isDefault(fd, v) {
  if (fd.rep) {
    return v.length === 0;
  }
  switch (fd.k) {
  case 'i64':
  case 's64':
  case 'sfx64':
  case 'u64':
  case 'fx64':
    return v === 0 || v === 0n;
  case 'bool':
    return v === false;
  case 'str':
    return v === '';
  case 'bytes':
    return v.length === 0;
  case 'msg':
    return v === null || v === undefined;
  default:
    return v === 0;
  }
}

function _defaultFor(fd, classes) {
  if (fd.rep) {
    return [];
  }
  // Presence fields (explicit presence or sub-message) default to absent.
  if (fd.has) {
    return null;
  }
  switch (fd.k) {
  case 'i64':
  case 's64':
  case 'sfx64':
  case 'u64':
  case 'fx64':
    return 0n;
  case 'f32':
  case 'f64':
  case 'i32':
  case 's32':
  case 'sfx32':
  case 'u32':
  case 'fx32':
  case 'enum':
    return 0;
  case 'bool':
    return false;
  case 'str':
    return '';
  case 'bytes':
    return Buffer.alloc(0);
  case 'msg':
    return null;
  default:
    return null;
  }
}

function _encodeVarint(n) {
  const parts = [];
  while (n >= 128) {
    parts.push((n % 128) | 128);
    n = Math.floor(n / 128);
  }
  parts.push(n);
  return Buffer.from(parts);
}

function _decodeVarint(buf, pos) {
  let n = 0;
  let shift = 0;
  while (pos < buf.length) {
    const b = buf[pos++];
    n += (b & 127) * 2 ** shift;
    shift += 7;
    if ((b & 128) === 0) {
      return [n, pos];
    }
    if (shift >= 64) {
      throw new Error('pbwire: varint too long');
    }
  }
  throw new Error('pbwire: truncated varint');
}

// _makeClass builds one message class over a MessageType. cls._FIELDS is the
// table the engine above reads; wire bytes always come from the C core.
// _fromWire converts C-validated wire values into an instance. Unlike
// fromObject it performs no validation and no coercion: the decoder has
// already checked every value against the schema, so nested plains only
// need wrapping and every present field only needs marking. The encode
// boundary (addon tree build) remains the loudness net for bad data.
function _fromWire(cls, plain) {
  // new keeps V8's hidden-class shape (measured faster than Object.create
  // + defineProperty here); the constructor's defaults stand for absent
  // fields, present ones are converted and marked below.
  const inst = new cls();
  const classes = cls._CLASSES;
  for (const fd of cls._FIELDS) {
    const v = plain[fd.n];
    if (v === undefined) {
      continue;
    }
    if (fd.k === 'msg') {
      if (fd.rep) {
        inst[fd.js] = v.map((item) => _fromWire(classes[fd.msg], item));
      } else {
        inst[fd.js] = _fromWire(classes[fd.msg], v);
      }
    } else if (fd.rep) {
      inst[fd.js] = v.slice();
    } else {
      inst[fd.js] = v;
    }
    inst._present.add(fd.js);
  }
  return inst;
}

function _makeClass(full, schemaOrDesc, fields) {
  // schemaOrDesc is either a legacy descriptor-set string (auto-loaded and
  // cached, as before) or a zero-arg getter returning the Schema to use.
  // Generated code passes its meta file's getter, so use_schema/init_schema
  // (register=false: explicit first) take effect per operation.
  const typeOf = (typeof schemaOrDesc === 'string')
    ? (() => _type(schemaOrDesc, full))
    : (() => schemaOrDesc().message(full));
  class Compat {
    constructor(props) {
      props = props || {};
      Object.defineProperty(this, '_present', { value: new Set(), enumerable: false, writable: true });
      for (const fd of fields) {
        const v = props[fd.js] !== undefined ? props[fd.js] : props[fd.n];
        if (v === undefined) {
          this[fd.js] = _defaultFor(fd);
        } else {
          this[fd.js] = v;
          // fromObject normalizes below; direct construction marks as set.
          this._present.add(fd.js);
        }
      }
    }
    static create(props) {
      return this.fromObject(props || {});
    }
    static encode(message) {
      const inst = (message instanceof this) ? message : this.fromObject(message);
      // The dynamic encoder takes proto names; toObject() shows camelCase.
      const plain = _toPlain(this, inst, { keepCase: true }, this._ENUMS, this._REVENUMS);
      const wire = typeOf().encode(plain);
      return { finish: () => wire };
    }
    static encodeDelimited(message) {
      const wire = this.encode(message).finish();
      const out = Buffer.concat([_encodeVarint(wire.length), wire]);
      return { finish: () => out };
    }
    static decode(bytes) {
      const buf = Buffer.isBuffer(bytes) ? bytes : Buffer.from(bytes);
      const m = typeOf().decode(buf);
      const plain = {};
      for (const fd of fields) {
        const v = m.get(fd.n);
        if (v !== undefined) {
          plain[fd.n] = v;
        }
      }
      return _fromWire(Compat, plain);
    }
    static decodeDelimited(bytes) {
      const buf = Buffer.isBuffer(bytes) ? bytes : Buffer.from(bytes);
      const pair = _decodeVarint(buf, 0);
      return this.decode(buf.subarray(pair[1], pair[1] + pair[0]));
    }
    static verify(obj) {
      if (obj === null || typeof obj !== 'object' || Array.isArray(obj)) {
        return 'object expected';
      }
      const seen = new Map();
      for (const fd of fields) {
        let v = obj[fd.js];
        if (v === undefined) {
          v = obj[fd.n];
        }
        if (v === null || v === undefined) {
          if (fd.req) {
            return fd.n + ': required field missing';
          }
          continue;
        }
        const bad = _verifyField(fd, v, fd.n, this._ENUMS, this._CLASSES);
        if (bad !== null) {
          return bad;
        }
        if (fd.oneof !== null) {
          if (seen.has(fd.oneof)) {
            return fd.oneof + ': multiple values';
          }
          seen.set(fd.oneof, true);
        }
      }
      return null;
    }
    static fromObject(obj) {
      if (obj === null || typeof obj !== 'object' || Array.isArray(obj)) {
        throw new TypeError('pbwire: fromObject expects an object');
      }
      const inst = new this();
      for (const fd of fields) {
        let v = obj[fd.js] !== undefined ? obj[fd.js] : obj[fd.n];
        if (v === undefined) {
          continue;
        }
        if (v === null) {
          inst[fd.js] = fd.rep ? [] : null;
          continue;
        }
        // One validation logic for verify() and fromObject(): normalize,
        // then reject with the same reason verify() would give.
        const nv = _fromValue(fd, v, fd.n, this._ENUMS, this._CLASSES);
        const bad = _verifyField(fd, nv, fd.n, this._ENUMS, this._CLASSES);
        if (bad !== null) {
          throw new TypeError('pbwire: ' + bad);
        }
        inst[fd.js] = nv;
        inst._present.add(fd.js);
      }
      return inst;
    }
    static toObject(message, opts) {
      const inst = (message instanceof this) ? message : this.fromObject(message);
      return _toPlain(this, inst, opts || {}, this._ENUMS, this._REVENUMS);
    }
    toObject(opts) {
      return this.constructor.toObject(this, opts);
    }
    toJSON() {
      return this.constructor.toObject(this, { enums: 'String', longs: 'String', bytes: 'base64' });
    }
  }
  Compat._FIELDS = fields;
  return Compat;
}
module.exports = { _makeClass };

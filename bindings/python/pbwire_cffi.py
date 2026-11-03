"""cffi binding for pbwire, the dependency-free protobuf library.

No generated Python code and no build step of its own: a `FileDescriptorSet`
(the file ``protoc --descriptor_set_out`` writes) becomes a schema at runtime,
and messages go in and out as plain dicts.

    import pbwire_cffi as pbw
    lib = pbw.load()
    schema = pbw.Schema.from_file(lib, "examples/addressbook.desc")
    person = schema.message("tutorial.Person")
    wire = person.encode({"name": "alice", "id": 1,
                          "phones": [{"number": "555"}]})
    assert person.decode(wire) == {"name": "alice", "id": 1,
                                   "phones": [{"number": "555"}]}

Build the shared library first (``make shared``, plus ``make python-bind`` for
the bulk helpers). Value mapping:

| protobuf | Python |
|---|---|
| int32/int64/sint*/sfixed* | int (signed) |
| uint32/uint64/fixed* | int |
| bool | bool |
| float/double | float |
| string | str (utf-8; undecodable bytes come back as bytes) |
| bytes | bytes |
| enum | int |
| message | dict |
| repeated | list |

A field missing from the dict is absent from the message and vice versa, so
proto2 presence and proto3 implicit presence behave like the generated
runtimes. Unknown fields survive decode->encode untouched.

Two paths do the same work: with ``libpbwirepy`` loaded one C call fills or
drains every scalar of a message from flat arrays; without it the binding
falls back to one cffi call per value (same results, ~10x the time on large
messages: 12000 cffi crossings for a 75KB message).
"""

import array
import os

_CDEF = """
typedef ... pbw_schema;
typedef ... pbw_ctx;
typedef ... pbw_descriptor;
typedef ... pbw_field;
typedef ... pbw_value;
typedef ... pbw_message;
typedef enum { PBW_OK = 0, PBW_ERR_INVALID = 1, PBW_ERR_NOMEM = 2,
               PBW_ERR_NOT_FOUND = 3, PBW_ERR_TYPE = 4, PBW_ERR_LIMIT = 5,
               PBW_ERR_REQUIRED = 6 } pbw_status;
typedef enum { PBW_TYPE_DOUBLE = 1, PBW_TYPE_FLOAT = 2, PBW_TYPE_INT64 = 3,
               PBW_TYPE_UINT64 = 4, PBW_TYPE_INT32 = 5, PBW_TYPE_FIXED64 = 6,
               PBW_TYPE_FIXED32 = 7, PBW_TYPE_BOOL = 8, PBW_TYPE_STRING = 9,
               PBW_TYPE_GROUP = 10, PBW_TYPE_MESSAGE = 11, PBW_TYPE_BYTES = 12,
               PBW_TYPE_UINT32 = 13, PBW_TYPE_ENUM = 14, PBW_TYPE_SFIXED32 = 15,
               PBW_TYPE_SFIXED64 = 16, PBW_TYPE_SINT32 = 17,
               PBW_TYPE_SINT64 = 18 } pbw_type;
typedef enum { PBW_LABEL_OPTIONAL = 1, PBW_LABEL_REQUIRED = 2,
               PBW_LABEL_REPEATED = 3 } pbw_label;

int pbw_abi_version(void);
pbw_schema *pbw_schema_new(void);
void pbw_schema_free(pbw_schema *schema);
pbw_descriptor *pbw_schema_add_message(pbw_schema *schema, const char *name);
pbw_descriptor *pbw_schema_find(const pbw_schema *schema, const char *name);
pbw_field *pbw_message_add_field(pbw_descriptor *msg, const char *name,
                                 uint32_t number, pbw_type type,
                                 pbw_label label);
pbw_status pbw_schema_link(pbw_schema *schema);
const char *pbw_schema_error(const pbw_schema *schema);
pbw_status pbw_descriptor_set_load(const void *data, size_t len,
                                   pbw_schema **out);
const char *pbw_descriptor_name(const pbw_descriptor *desc);
size_t pbw_descriptor_field_count(const pbw_descriptor *desc);
const pbw_field *pbw_descriptor_field_at(const pbw_descriptor *desc, size_t index);
const char *pbw_field_name(const pbw_field *field);
uint32_t pbw_field_number(const pbw_field *field);
pbw_type pbw_field_type(const pbw_field *field);
pbw_label pbw_field_label(const pbw_field *field);
const pbw_descriptor *pbw_field_message_type(const pbw_field *field);

pbw_message *pbw_message_new(const pbw_descriptor *desc);
void pbw_message_free(pbw_message *msg);
pbw_value *pbw_message_get(const pbw_message *msg, uint32_t number);
pbw_status pbw_message_set(pbw_message *msg, uint32_t number, pbw_value *value);
pbw_message *pbw_message_set_message(pbw_message *msg, uint32_t number);
int pbw_message_is_initialized(const pbw_message *msg);

pbw_value *pbw_value_new_uint(uint64_t bits);
pbw_value *pbw_value_new_float(float value);
pbw_value *pbw_value_new_double(double value);
pbw_value *pbw_value_new_bytes(const void *data, size_t len);
pbw_value *pbw_value_new_message(pbw_message *msg);
void pbw_value_free(pbw_value *value);
uint64_t pbw_value_get_uint(const pbw_value *value);
float pbw_value_get_float(const pbw_value *value);
double pbw_value_get_double(const pbw_value *value);
const void *pbw_value_get_bytes(const pbw_value *value, size_t *len);
pbw_message *pbw_value_get_message(const pbw_value *value);
size_t pbw_value_count(const pbw_value *value);
pbw_value *pbw_value_at(const pbw_value *value, size_t index);

pbw_status pbw_encode(const pbw_message *msg, uint8_t **out, size_t *len);
pbw_status pbw_decode(const pbw_descriptor *desc, const void *buf, size_t len,
                      pbw_message **out);
void pbw_free(void *ptr);
const char *pbw_strerror(pbw_status status);
"""

# Bulk helpers from bindings/python/csrc/pbw_py.c, optional at runtime.
_CDEF_SHIM = """
int pbw_py_set_uints(pbw_message *const *msgs, const uint32_t *nums,
                     const uint64_t *vals, size_t n);
int pbw_py_set_floats(pbw_message *const *msgs, const uint32_t *nums,
                      const float *vals, size_t n);
int pbw_py_set_doubles(pbw_message *const *msgs, const uint32_t *nums,
                       const double *vals, size_t n);
int pbw_py_set_bytes(pbw_message *const *msgs, const uint32_t *nums,
                     const char *blob, const size_t *offs, const size_t *lens,
                     size_t n);
size_t pbw_py_get_bytes(const pbw_message *msg, uint32_t number, char *blob,
                        size_t blob_cap, size_t *offs, size_t *lens,
                        size_t cap);
size_t pbw_py_get_uints(const pbw_message *msg, uint32_t number, uint64_t *out,
                        size_t cap, int sign);
size_t pbw_py_get_messages(const pbw_message *msg, uint32_t number,
                           pbw_message **out, size_t cap);
size_t pbw_py_get_floats(const pbw_message *msg, uint32_t number, float *out,
                         size_t cap);
size_t pbw_py_get_doubles(const pbw_message *msg, uint32_t number, double *out,
                          size_t cap);
"""

_TYPE_IDS = {
    "double": 1, "float": 2, "int64": 3, "uint64": 4, "int32": 5,
    "fixed64": 6, "fixed32": 7, "bool": 8, "string": 9, "group": 10,
    "message": 11, "bytes": 12, "uint32": 13, "enum": 14, "sfixed32": 15,
    "sfixed64": 16, "sint32": 17, "sint64": 18,
}

# Wire type -> (bulk group, signedness for decode). Every integer the dynamic
# API stores is a uint bit pattern, so one group covers int/uint/bool/enum and
# the decoder only has to widen the signs back.
_GROUPS = {
    1: ("d", None), 2: ("f", None), 3: ("u", 64), 4: ("u", None),
    5: ("u", 32), 6: ("u", None), 7: ("u", None), 8: ("u", None),
    9: ("s", None), 10: ("m", None), 11: ("m", None), 12: ("s", None),
    13: ("u", None), 14: ("u", 32), 15: ("u", 32), 16: ("u", 64),
    17: ("u", 32), 18: ("u", 64),
}

_MASK = 0xFFFFFFFFFFFFFFFF
_LIB_NAMES = ("libpbwire.so.1", "libpbwire.1.dylib", "libpbwire.so",
              "libpbwire.dylib")
_SHIM_NAMES = ("libpbwirepy.so", "libpbwirepy.dylib")


def _signed(value, bits):
    if bits == 32:
        value &= 0xFFFFFFFF
        return value - 0x100000000 if value >= 0x80000000 else value
    value &= _MASK
    return value - 0x10000000000000000 if value >= 0x8000000000000000 else value


def _find(names, what):
    here = os.path.dirname(os.path.abspath(__file__))
    for directory in (os.path.join(here, "build"),
                      os.path.join(here, "..", "build"),
                      os.path.join(here, "..", "..", "build")):
        for name in names:
            path = os.path.join(directory, name)
            if os.path.exists(path):
                return path
    raise OSError("%s not found; run `make shared` from the repo root" % what)


class Lib:
    """The cffi library plus its FFI; `shim` is None without the bulk helpers."""

    def __init__(self, raw, ffi, shim):
        self.__dict__["_raw"] = raw
        self.__dict__["_ffi"] = ffi
        self.__dict__["shim"] = shim

    def __getattr__(self, name):
        # cffi builds one accessor per symbol; keep it, so the next call is a
        # plain attribute load instead of a __getattr__ + lookup round trip.
        value = getattr(self.__dict__["_raw"], name)
        self.__dict__[name] = value
        return value

    def check(self, status):
        if status != 0:
            raise ValueError(self._ffi.string(self.pbw_strerror(status)).decode())


def load(path=None, shim_path=None):
    """Open libpbwire and, when built, the bulk helpers."""
    from cffi import FFI

    ffi = FFI()
    ffi.cdef(_CDEF)
    raw = ffi.dlopen(path or _find(_LIB_NAMES, "libpbwire shared library"))
    if raw.pbw_abi_version() != 1:
        raise OSError("libpbwire ABI mismatch")
    shim = None
    try:
        ffi.cdef(_CDEF_SHIM)
        shim = ffi.dlopen(shim_path or _find(_SHIM_NAMES, "libpbwirepy"))
    except OSError:
        pass  # slow path: one cffi call per value
    return Lib(raw, ffi, shim)


class Field:
    """One schema field, resolved once from the descriptor."""

    __slots__ = ("name", "number", "group", "sign", "repeated", "type_name",
                 "text")

    def __init__(self, name, number, type_id, repeated, type_name):
        self.name = name
        self.number = number
        self.group, self.sign = _GROUPS[type_id]
        self.repeated = repeated
        self.type_name = type_name
        # STRING decodes to str; BYTES (and everything else) to bytes.
        self.text = type_id == 9


class MessageType:
    """A message type: encode a dict to bytes, decode bytes to a dict."""

    def __init__(self, schema, desc, fields):
        self.schema = schema
        self._lib = schema._lib
        self._ffi = self._lib._ffi
        self._shim = self._lib.shim
        self._desc = desc
        self._by_name = {field.name: field for field in fields}
        self.fields = self._by_name

    def __repr__(self):
        return "<%s>" % self._lib._ffi.string(
            self._lib.pbw_descriptor_name(self._desc)).decode()

    # ------------------------------------------------------------ encode

    def _build(self, values):
        lib, ffi = self._lib, self._ffi
        msg = lib.pbw_message_new(self._desc)
        if msg == ffi.NULL:
            raise MemoryError("pbw_message_new")
        try:
            self._fill(msg, values)
        except BaseException:
            lib.pbw_message_free(msg)
            raise
        return msg

    def _fill(self, msg, values):
        """dict -> message tree, one C call per value kind for the whole tree."""
        if self._shim is None:
            self._fill_slow(msg, values)
            return
        # ops[group] = ([target message, ...], [field number, ...], [value, ...])
        # Children are created depth-first; their scalars land in the same flat
        # lists, so the crossings do not grow with the number of submessages.
        ops = {"u": ([], [], []), "f": ([], [], []), "d": ([], [], []),
               "s": ([], [], [])}
        self._collect(msg, values, ops)
        ffi, lib, shim = self._ffi, self._lib, self._shim
        for group, setter, ctype in (
                ("u", shim.pbw_py_set_uints, "uint64_t"),
                ("f", shim.pbw_py_set_floats, "float"),
                ("d", shim.pbw_py_set_doubles, "double")):
            targets, nums, vals = ops[group]
            if vals:
                lib.check(setter(ffi.new("pbw_message *[]", targets),
                                 ffi.new("uint32_t[]", nums),
                                 ffi.new(ctype + "[]", vals), len(vals)))
        targets, nums, blobs = ops["s"]
        if blobs:
            blob, offs, lens = _pack(blobs)
            lib.check(shim.pbw_py_set_bytes(
                ffi.new("pbw_message *[]", targets),
                ffi.new("uint32_t[]", nums), blob,
                ffi.new("size_t[]", offs), ffi.new("size_t[]", lens),
                len(blobs)))

    def _collect(self, msg, values, ops):
        lib, ffi = self._lib, self._ffi
        for name, value in values.items():
            field = self._by_name.get(name)
            if field is None:
                raise KeyError("%r is not a field of %r" % (name, self))
            if field.group == "m":
                sub = self.schema.message(field.type_name)
                for item in (value if field.repeated else (value,)):
                    child = lib.pbw_message_set_message(msg, field.number)
                    if child == ffi.NULL:
                        raise ValueError("cannot set %r" % field.name)
                    sub._collect(child, item, ops)
                continue
            targets, nums, vals = ops[field.group]
            if field.group == "s":
                for item in (value if field.repeated else (value,)):
                    targets.append(msg)
                    nums.append(field.number)
                    vals.append(item.encode("utf-8")
                                if isinstance(item, str) else bytes(item))
            elif field.group == "u":
                for item in (value if field.repeated else (value,)):
                    targets.append(msg)
                    nums.append(field.number)
                    vals.append(int(item) & _MASK)
            else:
                for item in (value if field.repeated else (value,)):
                    targets.append(msg)
                    nums.append(field.number)
                    vals.append(float(item))

    def _fill_slow(self, msg, values):
        """No bulk helpers: one cffi call per value, enough to stay usable."""
        lib, ffi = self._lib, self._ffi
        for name, value in values.items():
            field = self._by_name.get(name)
            if field is None:
                raise KeyError("%r is not a field of %r" % (name, self))
            if field.group == "m":
                sub = self.schema.message(field.type_name)
                for item in (value if field.repeated else (value,)):
                    child = lib.pbw_message_set_message(msg, field.number)
                    if child == ffi.NULL:
                        raise ValueError("cannot set %r" % field.name)
                    sub._fill_slow(child, item)
                continue
            for item in (value if field.repeated else (value,)):
                group = field.group
                if group == "s":
                    blob = (item.encode("utf-8")
                            if isinstance(item, str) else bytes(item))
                    made = lib.pbw_value_new_bytes(blob, len(blob))
                elif group == "f":
                    made = lib.pbw_value_new_float(float(item))
                elif group == "d":
                    made = lib.pbw_value_new_double(float(item))
                else:
                    made = lib.pbw_value_new_uint(int(item) & _MASK)
                lib.check(lib.pbw_message_set(msg, field.number, made))

    def encode(self, values):
        lib, ffi = self._lib, self._ffi
        msg = self._build(values)
        if not lib.pbw_message_is_initialized(msg):
            # Same contract as pbw_struct_encode: a proto2 required field that
            # the dict does not carry is an error, not a silent partial write.
            lib.pbw_message_free(msg)
            raise ValueError("required field missing in %r" % self)
        out = ffi.new("uint8_t **")
        length = ffi.new("size_t *")
        try:
            lib.check(lib.pbw_encode(msg, out, length))
            return bytes(ffi.buffer(out[0], length[0]))
        finally:
            lib.pbw_free(out[0])
            lib.pbw_message_free(msg)

    # ------------------------------------------------------------ decode

    def _read(self, msg, scratch):
        result = {}
        for field in self.fields.values():
            if field.group == "m":
                found = self._read_message(msg, field, scratch)
                if found is not _MISSING:
                    result[field.name] = found
                continue
            values = self._read_scalars(msg, field, scratch)
            if values is not None:
                result[field.name] = values
        return result

    def _read_message(self, msg, field, scratch):
        lib, ffi, shim = self._lib, self._ffi, self._shim
        sub = self.schema.message(field.type_name)
        if shim is None:
            value = lib.pbw_message_get(msg, field.number)
            if value == ffi.NULL:
                return _MISSING
            if not field.repeated:
                return sub._read(lib.pbw_value_get_message(value), scratch)
            return [sub._read(lib.pbw_value_get_message(
                lib.pbw_value_at(value, i)), scratch)
                for i in range(lib.pbw_value_count(value))]
        count = shim.pbw_py_get_messages(msg, field.number, ffi.NULL, 0)
        if count == 0:
            return _MISSING
        children = ffi.new("pbw_message *[]", count)
        shim.pbw_py_get_messages(msg, field.number, children, count)
        if not field.repeated:
            return sub._read(children[0], scratch)
        return [sub._read(children[i], scratch) for i in range(count)]

    def _read_scalars(self, msg, field, scratch):
        """Decoded value(s) for a present scalar field, None when absent."""
        lib, ffi = self._lib, self._ffi
        shim = self._shim
        number = field.number
        if field.group == "s":
            values = self._read_bytes(msg, field)
        elif shim is not None:
            getter = _SCALARS[field.group][0]
            sign = field.sign or 0
            buf, cap = scratch.scalar(field.group, sign)
            count = getter(shim, msg, number, buf, cap, sign)
            if count == 0:
                return None
            if count > cap:  # rare: grow once and read again
                buf, cap = scratch.grow(field.group, sign, count)
                getter(shim, msg, number, buf, cap, sign)
            if count > 8:
                values = scratch.unpack(field.group, sign, buf, count)
            elif sign:
                values = [_signed(buf[i], sign) for i in range(count)]
            else:
                values = [buf[i] for i in range(count)]
        else:
            value = lib.pbw_message_get(msg, number)
            if value == ffi.NULL:
                return None
            count = lib.pbw_value_count(value)
            items = ([lib.pbw_value_at(value, i) for i in range(count)]
                     if count else [value])
            values = [_scalar_of(lib, field.group, item) for item in items]
            if field.sign:
                sign = field.sign
                values = [_signed(v, sign) for v in values]
        if values is None:
            return None
        if field.group == "s" and field.text:
            values = [_as_text(v) for v in values]
        return values if field.repeated else values[0]

    def _read_bytes(self, msg, field):
        """Strings/bytes without a per-value call; falls back to the slow path."""
        lib, ffi = self._lib, self._ffi
        if self._shim is None:
            value = lib.pbw_message_get(msg, field.number)
            if value == ffi.NULL:
                return None
            count = lib.pbw_value_count(value)
            items = ([lib.pbw_value_at(value, i) for i in range(count)]
                     if count else [value])
            return [_bytes_of(lib, item) for item in items]
        if not field.repeated:
            value = lib.pbw_message_get(msg, field.number)
            if value == ffi.NULL:
                return None
            return [_bytes_of(lib, value)]
        count = self._shim.pbw_py_get_bytes(msg, field.number, ffi.NULL, 0,
                                            ffi.NULL, ffi.NULL, 0)
        if count == 0:
            return None
        offs = ffi.new("size_t[]", count)
        lens = ffi.new("size_t[]", count)
        self._shim.pbw_py_get_bytes(msg, field.number, ffi.NULL, 0, offs, lens,
                                    count)
        total = offs[count - 1] + lens[count - 1]
        if not total:
            return [b""] * count if field.repeated else [b""]
        blob = ffi.new("char[]", total)
        self._shim.pbw_py_get_bytes(msg, field.number, blob, total, offs, lens,
                                    count)
        view = ffi.buffer(blob, total)[:]
        return [view[offs[i]:offs[i] + lens[i]] for i in range(count)]

    def decode(self, wire):
        lib, ffi = self._lib, self._ffi
        out = ffi.new("pbw_message **")
        lib.check(lib.pbw_decode(self._desc, wire, len(wire), out))
        try:
            return self._read(out[0], _Scratch(ffi))
        finally:
            lib.pbw_message_free(out[0])


_MISSING = object()

# group -> (shim getter, C element type)
_SCALARS = {
    "u": (lambda shim, msg, number, out, cap, sign: shim.pbw_py_get_uints(
        msg, number, out, cap, sign), "uint64_t"),
    "f": (lambda shim, msg, number, out, cap, sign: shim.pbw_py_get_floats(
        msg, number, out, cap), "float"),
    "d": (lambda shim, msg, number, out, cap, sign: shim.pbw_py_get_doubles(
        msg, number, out, cap), "double"),
}
_ARRAY_CODES = {"u": "Q", "f": "f", "d": "d"}


class _Scratch:
    """Per-decode cached buffers: one C call per scalar field, not two."""

    __slots__ = ("ffi", "bufs")

    def __init__(self, ffi):
        self.ffi = ffi
        self.bufs = {}

    def _slot(self, group, sign):
        key = (group, sign)
        slot = self.bufs.get(key)
        if slot is None:
            slot = self.bufs[key] = [self.ffi.new(
                _SCALARS[group][1] + "[]", 64), 64]
        return slot

    def scalar(self, group, sign):
        slot = self._slot(group, sign)
        return slot[0], slot[1]

    def grow(self, group, sign, count):
        slot = self.bufs[(group, sign)] = [self.ffi.new(
            _SCALARS[group][1] + "[]", count), count]
        return slot[0], slot[1]

    def unpack(self, group, sign, buf, count):
        """C buffer -> Python list at C speed, no per-element cffi access."""
        code = "q" if group == "u" and sign else _ARRAY_CODES[group]
        values = array.array(code)
        values.frombytes(self.ffi.buffer(buf, count * values.itemsize))
        return values.tolist()


def _pack(blobs):
    """One bytes object plus (offset, length) per payload for the bulk setter."""
    offs, lens, chunk = [], [], 0
    for blob in blobs:
        offs.append(chunk)
        lens.append(len(blob))
        chunk += len(blob)
    return b"".join(blobs), offs, lens


def _scalar_of(lib, group, value):
    if group == "f":
        return lib.pbw_value_get_float(value)
    if group == "d":
        return lib.pbw_value_get_double(value)
    return lib.pbw_value_get_uint(value)


def _bytes_of(lib, value):
    size = lib._ffi.new("size_t *")
    raw = lib.pbw_value_get_bytes(value, size)
    try:
        return lib._ffi.buffer(raw, size[0])[:]
    finally:
        pass


def _as_text(blob):
    """STRING fields decode to str, falling back to bytes when not UTF-8."""
    try:
        return blob.decode("utf-8")
    except UnicodeDecodeError:
        return blob


class Schema:
    """A loaded schema: FileDescriptorSet bytes -> message types."""

    def __init__(self, lib, data):
        ffi = lib._ffi
        self._lib = lib
        out = ffi.new("pbw_schema **")
        lib.check(lib.pbw_descriptor_set_load(data, len(data), out))
        self._schema = out[0]
        self._types = {}

    @classmethod
    def from_file(cls, lib, path):
        with open(path, "rb") as handle:
            return cls(lib, handle.read())

    def message(self, name):
        found = self._types.get(name)
        if found is not None:
            return found
        lib, ffi = self._lib, self._lib._ffi
        desc = lib.pbw_schema_find(self._schema, name.encode())
        if desc == ffi.NULL:
            raise KeyError("no message %r in this schema" % name)
        fields = []
        for index in range(lib.pbw_descriptor_field_count(desc)):
            raw = lib.pbw_descriptor_field_at(desc, index)
            type_name = None
            sub = lib.pbw_field_message_type(raw)
            if sub != ffi.NULL:
                type_name = ffi.string(lib.pbw_descriptor_name(sub)).decode()
            fields.append(Field(
                ffi.string(lib.pbw_field_name(raw)).decode(),
                lib.pbw_field_number(raw),
                int(lib.pbw_field_type(raw)),
                lib.pbw_field_label(raw) == 3,
                type_name))
        message_type = MessageType(self, desc, fields)
        self._types[name] = message_type
        return message_type

    def close(self):
        if self._schema is not None:
            self._lib.pbw_schema_free(self._schema)
            self._schema = None

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.close()


def smoke(desc_path="examples/addressbook.desc"):
    """Round-trip the repo's addressbook through the binding."""
    lib = load()
    with Schema.from_file(lib, desc_path) as schema:
        person = schema.message("tutorial.Person")
        values = {"name": "alice", "id": 1,
                  "phones": [{"number": "555", "type": 1}]}
        got = person.decode(person.encode(values))
        assert got == values, got
    print("pbwire_cffi: smoke OK (bulk helpers: %s)"
          % ("yes" if lib.shim is not None else "no"))


if __name__ == "__main__":
    smoke()

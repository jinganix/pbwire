"""Minimal cffi binding for pbwire (ABI mode: no compiler needed).

Build the library first so the loader finds it::

    make shared            # build/libpbwire.so.1 (Linux) / .1.dylib (macOS)

Then::

    import pbwire_cffi as pbw
    lib = pbw.load()
    schema = pbw.Schema(lib)
    person = schema.message("Person", [("name", 1, "string"), ("id", 2, "int32")])
    wire = person.encode({"name": "alice", "id": 1})
    assert person.decode(wire) == {"name": "alice", "id": 1}

Only scalar fields are wrapped (int/uint/bool/double/string/bytes); that is
enough to prove the ABI path end to end. Anything richer belongs in a real
host binding, not in this smoke cover.
"""

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
typedef enum { PBW_VALUE_UINT = 0, PBW_VALUE_BYTES = 1, PBW_VALUE_MESSAGE = 2,
               PBW_VALUE_LIST = 3 } pbw_value_kind;

int pbw_abi_version(void);
pbw_schema *pbw_schema_new(void);
void pbw_schema_free(pbw_schema *schema);
pbw_descriptor *pbw_schema_add_message(pbw_schema *schema, const char *name);
pbw_field *pbw_message_add_field(pbw_descriptor *msg, const char *name,
                                 uint32_t number, pbw_type type,
                                 pbw_label label);
int pbw_schema_link(pbw_schema *schema);
const pbw_descriptor *pbw_schema_find(const pbw_schema *schema,
                                      const char *name);
pbw_message *pbw_message_new(const pbw_descriptor *desc);
void pbw_message_free(pbw_message *msg);
pbw_value *pbw_message_get(const pbw_message *msg, uint32_t number);
int pbw_message_set(pbw_message *msg, uint32_t number, pbw_value *value);
pbw_value *pbw_value_new_int(int64_t value);
pbw_value *pbw_value_new_uint(uint64_t bits);
pbw_value *pbw_value_new_bool(int value);
pbw_value *pbw_value_new_double(double value);
pbw_value *pbw_value_new_bytes(const void *data, size_t len);
pbw_value *pbw_value_new_string(const char *str);
void pbw_value_free(pbw_value *value);
int pbw_value_get_kind(const pbw_value *value);
uint64_t pbw_value_get_uint(const pbw_value *value);
const void *pbw_value_get_bytes(const pbw_value *value, size_t *len);
const char *pbw_value_get_string(const pbw_value *value);
int pbw_encode(const pbw_message *msg, uint8_t **out, size_t *len);
int pbw_decode(const pbw_descriptor *desc, const void *buf, size_t len,
               pbw_message **out);
void pbw_free(void *ptr);
const char *pbw_strerror(int status);
"""

_TYPES = {
    "double": (1, "uint"),
    "float": (2, "uint"),
    "int64": (3, "uint"),
    "uint64": (4, "uint"),
    "int32": (5, "uint"),
    "fixed64": (6, "uint"),
    "fixed32": (7, "uint"),
    "bool": (8, "uint"),
    "string": (9, "bytes"),
    "bytes": (12, "bytes"),
    "uint32": (13, "uint"),
    "sfixed32": (15, "uint"),
    "sfixed64": (16, "uint"),
    "sint32": (17, "uint"),
    "sint64": (18, "uint"),
}

_LIB_NAMES = ("libpbwire.so.1", "libpbwire.1.dylib", "libpbwire.so",
              "libpbwire.dylib")


def _find_lib():
    here = os.path.dirname(os.path.abspath(__file__))
    for directory in (os.path.join(here, "..", "build"),
                      os.path.join(here, "..", "..", "build")):
        for name in _LIB_NAMES:
            path = os.path.join(directory, name)
            if os.path.exists(path):
                return path
    raise OSError("libpbwire shared library not found; run `make shared`")


def load(path=None):
    """Open the shared library with cffi in ABI mode."""
    from cffi import FFI

    ffi = FFI()
    ffi.cdef(_CDEF)
    raw = ffi.dlopen(path or _find_lib())
    if raw.pbw_abi_version() != 1:
        raise OSError("libpbwire ABI mismatch")
    return _Lib(raw, ffi)


class _Lib:
    """The cffi library plus its FFI: attribute access hits the C API."""

    def __init__(self, raw, ffi):
        self.__dict__["_raw"] = raw
        self.__dict__["_ffi"] = ffi

    def __getattr__(self, name):
        return getattr(self.__dict__["_raw"], name)


class Message:
    """A message type bound to a schema: encode/decode plain dicts."""

    def __init__(self, lib, desc, fields):
        self._lib = lib
        self._ffi = lib._ffi
        self._desc = desc
        # name -> (number, kind) with kind "uint" or "bytes"
        self._fields = {name: (number, _TYPES[typename][1])
                        for name, number, typename in fields}
        self._numbers = {number: (name, kind)
                         for name, (number, kind) in self._fields.items()}

    def encode(self, values):
        lib, ffi = self._lib, self._ffi
        msg = lib.pbw_message_new(self._desc)
        if msg == ffi.NULL:
            raise MemoryError("pbw_message_new")
        try:
            for name, python in values.items():
                number, kind = self._fields[name]
                if kind == "uint":
                    if isinstance(python, bool):
                        value = lib.pbw_value_new_bool(1 if python else 0)
                    elif isinstance(python, float):
                        value = lib.pbw_value_new_double(python)
                    elif python < 0:
                        value = lib.pbw_value_new_int(python)
                    else:
                        value = lib.pbw_value_new_uint(python)
                elif isinstance(python, str):
                    value = lib.pbw_value_new_string(
                        python.encode("utf-8"))
                else:
                    value = lib.pbw_value_new_bytes(bytes(python),
                                                    len(bytes(python)))
                if value == ffi.NULL:
                    raise MemoryError("pbw_value_new")
                status = lib.pbw_message_set(msg, number, value)
                if status != 0:
                    reason = ffi.string(lib.pbw_strerror(status)).decode()
                    lib.pbw_value_free(value)
                    raise ValueError(reason)
            out = ffi.new("uint8_t **")
            length = ffi.new("size_t *")
            if lib.pbw_encode(msg, out, length) != 0:
                raise ValueError("encode failed")
            try:
                return bytes(ffi.buffer(out[0], length[0]))
            finally:
                lib.pbw_free(out[0])
        finally:
            lib.pbw_message_free(msg)

    def decode(self, wire):
        lib, ffi = self._lib, self._ffi
        out = ffi.new("pbw_message **")
        if lib.pbw_decode(self._desc, wire, len(wire), out) != 0:
            raise ValueError("decode failed")
        try:
            result = {}
            for number, (name, kind) in self._numbers.items():
                value = lib.pbw_message_get(out[0], number)
                if value == ffi.NULL:
                    continue
                if kind == "uint":
                    result[name] = lib.pbw_value_get_uint(value)
                else:
                    size = ffi.new("size_t *")
                    raw = lib.pbw_value_get_bytes(value, size)
                    blob = bytes(ffi.buffer(raw, size[0]))
                    try:
                        result[name] = blob.decode("utf-8")
                    except UnicodeDecodeError:
                        result[name] = blob
            return result
        finally:
            lib.pbw_message_free(out[0])


class Schema:
    def __init__(self, lib):
        ffi = lib._ffi
        self._lib = lib
        self._schema = lib.pbw_schema_new()
        if self._schema == ffi.NULL:
            raise MemoryError("pbw_schema_new")

    def message(self, name, fields):
        lib = self._lib
        desc = lib.pbw_schema_add_message(self._schema, name.encode())
        for field_name, number, typename in fields:
            lib.pbw_message_add_field(desc, field_name.encode(), number,
                                      _TYPES[typename][0], 1)
        if lib.pbw_schema_link(self._schema) != 0:
            raise ValueError("link failed")
        found = lib.pbw_schema_find(self._schema, name.encode())
        return Message(lib, found, fields)

    def close(self):
        if self._schema is not None:
            self._lib.pbw_schema_free(self._schema)
            self._schema = None

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.close()


def smoke():
    lib = load()
    with Schema(lib) as schema:
        person = schema.message("Person", [("name", 1, "string"),
                                           ("id", 2, "int32")])
        wire = person.encode({"name": "alice", "id": 1})
        assert person.decode(wire) == {"name": "alice", "id": 1}, \
            person.decode(wire)
        assert person.decode(person.encode({"name": "x" * 300,
                                             "id": 300}))["name"] == "x" * 300
    print("pbwire_cffi: smoke OK")


if __name__ == "__main__":
    smoke()

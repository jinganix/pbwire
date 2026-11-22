"""Public surface of the pure-Python core: a ``Schema`` loaded from a
FileDescriptorSet, message types that decode and encode wire bytes, and the
plain-dict helpers the pbench core arm uses. No C extension, no protoc plugin,
no generated code, no dependency at all -- stdlib only.

    from pure.schema import Schema

    schema = Schema.from_file("matrix3.desc")
    type = schema.message("structtest.Scalars3")
    msg = type.decode(bytes)        # unknown fields retained
    type.encode(msg)                 # byte-exact for a decoded tree
    type.encode_object(dict)         # plain dict in, wire bytes out
    type.decode_object(bytes)        # wire bytes in, plain dict out

Nothing here imports the CPython extension (`bindings/python/runtime/`) or
`google.protobuf`: this is the no-extension mode (pure-Python, PyPy, a sandboxed
interpreter), never the fast path.
"""

from .decode import decode, decode_slice, new_message
from .descriptor import (
    DEFAULT_LIMITS,
    K,
    CoreError,
    Limits,
    check_bytes,
    load_schema,
)
from .encode import encode
from .tree import (
    Message,
    Raw,
    from_object,
    get_field,
    has_field,
    oneof_case,
    raw_payload,
    to_object,
)

__all__ = [
    "CoreError", "K", "Limits", "Message", "MessageType", "Raw", "Schema",
    "decode", "decode_slice", "encode", "from_object", "get_field", "has_field",
    "oneof_case", "raw_payload", "to_object",
]


class MessageType:
    """One message type: new_message() plus binary decode/encode."""

    __slots__ = ("def_",)

    def __init__(self, def_):
        self.def_ = def_

    def name(self):
        """Fully qualified name, e.g. "structtest.Scalars3"."""
        return self.def_.full_name

    def new_message(self):
        """An empty, mutable message of this type."""
        return new_message(self.def_)

    def decode(self, data, limits=DEFAULT_LIMITS):
        """Decodes wire bytes; unknown fields are kept and re-encoded. The
        bytes are the authority on presence, so a field absent on the wire
        stays absent."""
        return decode(self.def_, data, limits)

    def decode_slice(self, data, off, length, limits=DEFAULT_LIMITS):
        """Decodes one body slice (offset + length) of a larger buffer."""
        return decode_slice(self.def_, data, off, length, limits)

    def encode(self, m):
        """Encodes a tree; a tree that came from decode() re-emits
        byte-exactly."""
        return encode(m)

    def decode_object(self, data, limits=DEFAULT_LIMITS):
        """Decodes straight into a plain dict (proto field names)."""
        return to_object(self.decode(data, limits))

    def encode_object(self, obj):
        """Encodes a plain dict (the inverse of decode_object)."""
        return encode(from_object(self.def_, obj))

    def get_field(self, m, name_or_number):
        """The value of a field of ``m`` (materialized), or None."""
        return get_field(m, name_or_number)

    def has_field(self, m, name_or_number):
        """True when ``m`` carries that field (present on the wire)."""
        return has_field(m, name_or_number)

    def oneof_case(self, m, oneof_name):
        """The field number set in a oneof of ``m``, 0 for none."""
        return oneof_case(m, oneof_name)

    def to_object(self, m):
        """A plain dict for a tree, exactly like decode_object's result."""
        return to_object(m)

    def __repr__(self):
        return "<MessageType %s>" % self.name()


class Schema:
    """A loaded FileDescriptorSet: the runtime schema every type comes from."""

    __slots__ = ("core", "path")

    def __init__(self, core, path=None):
        self.core = core
        self.path = path

    @classmethod
    def from_file(cls, path, limits=DEFAULT_LIMITS):
        """Loads a descriptor set written by protoc --descriptor_set_out."""
        with open(path, "rb") as handle:
            data = handle.read()
        return cls.from_bytes(data, limits, path)

    @classmethod
    def from_bytes(cls, descriptor_set, limits=DEFAULT_LIMITS, path=None):
        """Loads descriptor-set bytes; use --include_imports so every type
        resolves."""
        check_bytes(limits, len(descriptor_set), "descriptor set")
        return cls(load_schema(descriptor_set), path)

    def message(self, full_name):
        """Looks up a message by its fully qualified name."""
        return MessageType(self.core.message(full_name))

    def __repr__(self):
        return "<Schema %s>" % (self.path or "<bytes>")

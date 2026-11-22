"""Pure-Python pbwire core: descriptor reader, value tree, wire primitives,
encoder and decoder in stdlib-only Python.

The no-extension mode of the Python binding (pure-Python interpreters, PyPy,
sandboxed runtimes). The CPython extension in ``bindings/python/runtime/`` stays
the shipped codec; this package exists so a host with no C toolchain still has
the same wire format, proven by the same official binary conformance suite
(``make conformance-python-pure``).
"""

from .schema import MessageType, Schema

__all__ = ["MessageType", "Schema"]

"""
    PbwireCore

A pure-Julia wire core: descriptor reader, value tree, wire primitives, encoder
and a decoder with unknown retention, groups, packed runs, oneofs, presence and
limits. Stdlib only -- no `ccall`, no native library, no generated code.

This is the `core` arm for `ccall`-only hosts and consumers who want no native
library (containers without a C toolchain, wasm hosts, sandboxes). It is proven
by the same official binary conformance suite as the C core
(`make conformance-julia-core`) and is an interpreter loop: published as the
no-native path, never as the fast one. `Pbwire` (the `ccall` runtime) stays the
shipped codec.

    using PbwireCore
    schema = from_file("build/conformance.desc")
    ty = msgtype(schema, "conformance.ConformanceRequest")
    request = decode(ty, bytes)
"""
module PbwireCore

export CoreError, EnumDef, Field, Limits, Message, MsgDef, MsgType, Schema,
    Raw
export as_bool, as_int, as_message, as_string, at, decode, decode_dict, decode_slice, encode, encode_dict,
    field_by_name, field_count, from_bytes, from_dict, from_file, get_bool,
    get_bytes, get_double, get_field, get_float, get_int, get_message,
    get_string, has, has_field, msgtype, name, new_message, oneof_case,
    raw_payload, set_bool!, set_bytes!, set_double!, set_float!, set_int!,
    set_message!, set_string!, to_dict, to_value
export K_BOOL, K_BYTES, K_DOUBLE, K_ENUM, K_FIXED32, K_FIXED64, K_FLOAT,
    K_GROUP, K_INT32, K_INT64, K_MESSAGE, K_SFIXED32, K_SFIXED64, K_SINT32,
    K_SINT64, K_STRING, K_UINT32, K_UINT64
export IMPLICIT, EXPLICIT, REQUIRED, MESSAGE_PRESENCE
export DEFAULT_LIMITS, MAX_DEPTH
export check_bytes, effective_depth, is_packable, slot_of, wt_incompatible
export utf8_encode, utf8_lossy, utf8_valid, tag_bytes
export bits_double, bits_float, double_bits, float_bits

include("core/wire.jl")
include("core/descriptor.jl")
include("core/tree.jl")
include("core/encode.jl")
include("core/decode.jl")
include("core/schema.jl")

end # module

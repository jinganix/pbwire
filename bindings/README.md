# Bindings

Every binding ships the same pieces (`plugin/` only where the language needs
it): a runtime, examples, tests, benchmarks and a conformance entry.

```
bindings/<lang>/
    runtime/        the binding's codec (rebar3 layout: c_src/ + src/ + priv/)
    plugin/         optional: schema/code generator
    examples/       a runnable round trip
    tests/          self-checks
    benchmarks/     the four standard tiers from ../BENCHMARKS.md
    conformance/    how the official suite covers this binding
```

## Where the schema comes from

The rule is the same for all of them, and it decides the design more than
anything else:

- **Compiled languages bake the schema in at build time.** A protoc plugin
  turns the `.proto` into the language's own structs and wire facts, so no
  binding hand-builds a schema. C does that for layout/kind/type columns on
  top of the dynamic core; Go generates straight-line codecs that carry every
  wire fact themselves (`bindings/go/plugin`), which is also where the
  performance comes from.
- **A `FileDescriptorSet` is the runtime schema for the dynamic core and for
  interpreted bindings.** C, C++, the Python extension and the JSON/text paths
  load `protoc --descriptor_set_out` bytes at run time; the C struct entry
  points also validate the generated layout against that schema. Go needs no
  descriptor at run time: its generated code is the complete schema.
- **Erlang needs neither.** gpb already is that plugin: `gpb_compile` /
  `protoc-erl` generates the record definitions plus `get_msg_defs/0`, and
  `enif_protobuf:load_cache/1` consumes exactly that. Adding a second
  generator or a descriptor loader would duplicate gpb.

## Status

| binding | schema source | compliant | note |
|---|---|---|---|
| C | `pbw_struct_schema_load(path)` on a `FileDescriptorSet`; `plugin/` additionally emits structs + layout tables for the compile-time side | ✅ | struct layout is codegen, the schema itself is loaded |
| C++ | the C runtime and its generated headers, unchanged | ✅ | inherits C |
| Python | `pbwire.Schema.from_file/load` on a `FileDescriptorSet` | ✅ | the extension is compiled but the schema is always loaded; `decode()` returns a parsed message, `to_dict()`/`encode(dict)` are the dict path |
| Go | `protoc-gen-pbwire-go` at build time: structs + straight-line size/encode/decode (`bindings/go/plugin`) | ✅ | no cgo, no descriptor at run time; maps/oneofs/groups/extensions rejected |
| Erlang | `gpb_compile` → records + `get_msg_defs/0` → `enif_protobuf:load_cache/1` | ✅ | gpb is the plugin, epb needs no descriptor path |

Compiler-level code generation is never hand-written: C's layout tables and
Go's codecs both come from the same descriptor `protoc` parsed, so field
numbers, wire types and proto2/proto3 semantics are the schema's. The
difference is where they live -- C loads the descriptor set at run time as
well (the dynamic core needs it and the struct entry points validate against
it), Go bakes the facts into the generated code, and Erlang's records are a
layout over gpb's definitions.

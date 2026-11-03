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

- **Ahead-of-time compiled languages load the descriptor set.** C, C++ and Go
  compile the binding, so the message shape cannot be baked into the binary:
  they load a `FileDescriptorSet` (`protoc --descriptor_set_out`) at runtime
  and drive everything from it. A hand-built schema in a compiled binding is a
  bug, not a shortcut.
- **Languages that can load code at runtime get a plugin instead.** The plugin
  turns `.proto` into the host's own schema definition, which is then loaded
  once at startup.
- **Erlang needs neither.** gpb already is that plugin: `gpb_compile` /
  `protoc-erl` generates the record definitions plus `get_msg_defs/0`, and
  `enif_protobuf:load_cache/1` consumes exactly that. Adding a second
  generator or a descriptor loader would duplicate gpb.

## Status

| binding | schema source | compliant | note |
|---|---|---|---|
| C | `pbw_struct_schema_load(path)` on a `FileDescriptorSet`; `plugin/` additionally emits structs + layout tables for the compile-time side | ✅ | struct layout is codegen, the schema itself is loaded |
| C++ | the C runtime and its generated headers, unchanged | ✅ | inherits C |
| Python | `pbwire.Schema.from_file/load` on a `FileDescriptorSet` | ✅ | the extension is compiled but the schema is always loaded |
| Go | `pbwire.Load/LoadFile` on a `FileDescriptorSet` (`runtime/pbwire.go`) | ✅ | hand-built schemas were removed; scalars only so far |
| Erlang | `gpb_compile` → records + `get_msg_defs/0` → `enif_protobuf:load_cache/1` | ✅ | gpb is the plugin, epb needs no descriptor path |

Compiler-level code generation (C structs, Erlang records) is a *layout*, not a
schema: the field numbers, wire types and proto2/proto3 semantics always come
from the schema the binding loads or is handed by the host.

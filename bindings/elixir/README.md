# Pbwire Elixir binding

An Elixir wrapper over the `pbwire` NIF for Elixir (and any BEAM language):
`elixir-protobuf` generates the structs as usual, `Pbwire.load_schema/1`
consumes them, and `Pbwire.encode/1` / `decode/2` run in C instead of
Elixir — same structs, same wire format, byte-identical output. No
`FileDescriptorSet`, no per-message native build, no second wire
implementation.

The Erlang binding (`bindings/erlang`, Hex package `pbwire`) owns the NIF
and the C core; this package (`pbwire_elixir`) owns the Elixir half only:
translating the generated `__message_props__/0` into the defs `:pbwire`
consumes, and translating structs to the record tuples the NIF encodes and
back. That split is deliberate — adding a second NIF that re-parses
`elixir-protobuf` terms would duplicate the whole codec for no new wire
fact. Elixir runs on the BEAM like Erlang, so it reuses the same NIF the
same way, the way the Erlang binding reuses gpb instead of duplicating it.

| Piece | Path | Role |
|---|---|---|
| runtime | [`lib/`](lib/) | `Pbwire` (public API + struct↔tuple translation) and `Pbwire.Schema` (generated-module → NIF defs) |
| examples | [`examples/addressbook.exs`](examples/addressbook.exs) | hand-written generated-style structs, struct round trip |
| tests | [`test/`](test/) | ExUnit self-check, `elixir-protobuf` as the oracle |
| benchmarks | [`benchmarks/bench.exs`](benchmarks/bench.exs) | four standard tiers vs `Protobuf` (`make bench`) |
| conformance | [`conformance/`](conformance/) | status and plan: blocked on test-message module-name collisions |
| plugin | none | `elixir-protobuf` (`protoc-gen-elixir`) is the generator; `__message_props__/0` is the schema source |

## Build and run

Needs Elixir (see [`.tool-versions`](.tool-versions)) and a C compiler; `mix`
fetches `pbwire` (the NIF) and `protobuf` (the oracle) from Hex. The NIF
links the repo's C core, which this binding compiles from `src/` into
`build/` here — nothing outside `bindings/elixir/` is written.

```shell
make          # core lib + deps + compile
make test     # ExUnit: byte-identity with Protobuf.encode, struct equality with Protobuf.decode
make bench    # four standard tiers vs elixir-protobuf on matrix3.proto
```

With `mix` directly (after one `make setup`, which builds `build/libpbwire.a`
the `:pbwire` dep compiles against):

```shell
mix test
mix run examples/addressbook.exs
mix run benchmarks/bench.exs
```

## Quick start

Given `person.proto`:

```protobuf
message Person {
  required string name = 1;
  required int32 id = 2;
  optional string email = 3;
}
```

Generate Elixir structs with `protoc-gen-elixir`, then load the schema once
and encode/decode:

```elixir
:ok = Pbwire.load_schema([MyApp.Person])
msg = %MyApp.Person{name: "abc def", id: 345, email: "a@example.com"}
bin = Pbwire.encode(msg) == Protobuf.encode(msg)  # true
%MyApp.Person{} = Pbwire.decode(bin, MyApp.Person)
```

Convenience wrappers that load the schema on each call:

```elixir
Pbwire.encode_msg(msg, [MyApp.Person])
Pbwire.decode_msg(bin, MyApp.Person, [MyApp.Person])
```

Referencing another message, enum or map entry pulls it into the schema
transitively: `load_schema([MyApp.Person])` is enough when `Person` embeds
the rest. Message and enum names in the NIF cache are the Elixir module
atoms themselves, so two schemas can never collide the way unqualified
record names do.

## API reference

| Function | Returns | Notes |
|---|---|---|
| `Pbwire.load_schema(mods, opts \\ [])` | `:ok` | `mods` is a list of generated modules; replaces any previously loaded schema for the whole node |
| `Pbwire.purge_schema()` | `:ok` | Forgets the schema; later `encode/1` / `decode/2` raise until `load_schema/2` runs again |
| `Pbwire.encode(struct)` | `binary()` | struct from a loaded generated module; byte-identical to `Protobuf.encode/1` |
| `Pbwire.decode(bin, mod)` | struct | `mod` is the generated module; equal to `Protobuf.decode(bin, mod)` |
| `Pbwire.encode_msg(struct, mods)` | `binary()` | `load_schema(mods)` + `encode(struct)` |
| `Pbwire.decode_msg(bin, mod, mods)` | struct | `load_schema(mods ++ [mod])` + `decode(bin, mod)` |

Calling `encode/1` or `decode/2` with a module that has no loaded schema
raises `ArgumentError` before touching the NIF. NIF-level failures
(malformed wire data, a missing `required` field, an unknown message name)
surface as `ErlangError`, the same exceptions the Erlang binding raises.

## Supported protobuf features

| Feature | Support |
|---|---|
| Scalar types | `int32/64`, `uint32/64`, `sint32/64`, `fixed32/64`, `sfixed32/64`, `float`, `double`, `bool`, `string`, `bytes` |
| Enums | Yes, including unknown numbers (decode to integers, like the oracle); negative values |
| Occurrence | `required`, `optional` (proto2 + explicit proto3), implicit presence, `repeated` (`packed` and unpacked) |
| Composite | Nested messages, `oneof` (`{member, value}` tuples, like the struct), `map<K, V>` (Elixir maps) |
| proto3 | Yes, including zero-omission and `proto3_optional` presence |
| Unknown fields | Dropped on decode (the tuple form has no storage for them); structs decode with `__unknown_fields__: []` |

Not covered: groups and extensions (`elixir-protobuf` generates neither),
`NaN` bit-identity (any quiet-NaN encoding decodes to `:nan`; the NIF and
the oracle pick differentpayload bits), and missing-`required` tolerance
(`Pbwire.decode/2` raises where the oracle fills the default).

## Options

`Pbwire.load_schema/2` passes its options to `:pbwire.set_opts/1` before
loading:

| Option | Default | Effect |
|---|---|---|
| `:with_utf8` | `true` | Allow non-Latin-1 UTF-8 in string fields when encoding (Elixir strings are UTF-8; the Erlang default is `false`) |
| `:string_as_list` | unset | Decode string fields as charlists instead of binaries (leave off: Elixir strings are binaries) |

## Benchmark

`benchmarks/bench.exs` measures `Pbwire` against `elixir-protobuf` (pure
Elixir) on the same schema and payload as every other binding in this repo:
`bindings/c/tests/data/matrix3.proto` with the repeated fields scaled
x0 / x1 / x100 / x3000 (136 / 174 / 2469 / 74913 bytes on the wire). Both
backends build the same struct, so the timer measures the codec, not the
tree. Every backend must match byte for byte (encode, plus re-encoding a
decoded reference) before anything is timed.

<!-- BENCHMARK_RESULTS_START -->
Last run: 2026-11-05 (matrix3.proto, the four standard tiers; Elixir 1.20.5 / OTP 29, Apple M4 Pro)

| payload | op | pbwire-elixir (MB/s, x protobuf) | protobuf (MB/s) |
|---|---|---|---|
| 136 B | encode | 154.1 (3.3x) | 47.3 |
| 136 B | decode | 84.8 (1.4x) | 59.5 |
| 174 B | encode | 157.3 (3.0x) | 51.6 |
| 174 B | decode | 102.0 (1.6x) | 63.4 |
| 2469 B | encode | 166.8 (2.8x) | 60.2 |
| 2469 B | decode | 99.0 (1.7x) | 57.8 |
| 74913 B | encode | 145.6 (2.3x) | 63.2 |
| 74913 B | decode | 84.7 (1.4x) | 60.6 |

Higher is better. Throughput in MB/s on serialized wire size.
<!-- BENCHMARK_RESULTS_END -->

The baseline is pure Elixir (~50 MB/s flat across tiers: per-field
bookkeeping in the interpreter), so the NIF wins every cell; xlarge
decode is the closest race (1.40x), where the struct↔tuple translation the
Elixir half owns is most visible. Rerun with `make bench` and paste the new
table over the block above.

## FAQ

### Do I need to regenerate my `.proto` files or change my structs?

No. Keep `protoc-gen-elixir` output and struct definitions; only the
encode/decode calls change.

### Does it produce the same bytes as `Protobuf.encode/1`?

Yes for the messages covered by the test suite (which asserts `==` on every
case, including a 3000-element repeated field). Both follow the protobuf
wire format, so any conforming decoder reads either.

### How is this different from the Erlang binding?

Same NIF, same C core, same cache semantics — plus a translation layer:
generated modules in, record tuples over the NIF, structs back out. The
Erlang binding consumes gpb defs; this one consumes
`__message_props__/0`. Neither parses `.proto` text and neither owns wire
code.

### Why does `encode/1` raise `ArgumentError`?

No schema is loaded for the struct's module (or `purge_schema/0` was
called). Call `Pbwire.load_schema/1` first, or use `encode_msg/2` /
`decode_msg/3`.

### Can several processes or applications use it at once?

Encoding and decoding are safe from any number of processes. The schema
cache is global to the node, so two applications with different `.proto`
schemas need separate nodes — same rule as the Erlang binding.

### Why is there no `c_src/` here?

There is no Elixir-side C to write: the NIF ships in the `:pbwire` Hex
package and links the repo's C core (built into `build/` by `make`). An
Elixir NIF that re-parses `elixir-protobuf` terms would duplicate the
Erlang binding's whole codec for identical bytes.

## Development

```shell
make tests   # ExUnit (byte parity + struct parity with the oracle)
make bench   # the four standard tiers
make c-format       # mix format
make c-format-check # mix format --check-formatted
```

Layout: `lib/pbwire.ex` (public API + struct↔tuple translation),
`lib/pbwire/schema.ex` (generated-module → gpb-style defs, cached in
`persistent_term` beside the NIF's own global cache), `test/` (ExUnit plus
`test/support/` generated-style modules), `benchmarks/bench.exs`
(byte-gated harness), `conformance/README.md` (status and plan).

## Generated models

`protoc-gen-pbwire` (in `../../codegen`, `--pbwire_opt=lang=elixir`) emits
one `.ex` file per `.proto`: `elixir-protobuf` modules (the binding's schema
source, no descriptor set embedded — the modules are the schema) plus a
per-file `Registry` module holding `message_modules/0`, the whole-file
list to pass straight to `Pbwire.load_schema/1` (`load_schema` resolves
referenced modules transitively). `[,model=bool][,register=bool]`
`[,registry=bool]` (all default `true`): `model=false` leaves the list
without modules; `registry=false` drops it; `register` is a no-op (loading
is always explicit). The Registry is per-file — qualified
(`Spectest.Maptest.Registry`) when the file stem differs from the package
prefix, short (`Xdep.Registry`) otherwise — so two files in one package
never share one.

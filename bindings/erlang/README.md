# pbwire Erlang binding

A NIF over the C core for Erlang/OTP (and Elixir): gpb generates the record
definitions as usual, `pbwire:load_cache/1` consumes them, and
`pbwire:encode/1` / `decode/2` run in C instead of Erlang — same record
tuples, same wire format, byte-identical output. No libprotobuf, no per-message
native build.

Previously published standalone as `enif_protobuf`; it is now developed here
as `bindings/erlang` and published to Hex as `pbwire`. If you depended on the
old repository, point your checkout at this one and replace `enif_protobuf:`
with `pbwire:` — the API and the wire format are unchanged.

| Piece | Path | Role |
|---|---|---|
| runtime | [`c_src/`](c_src/), [`src/`](src/), [`priv/`](priv/) | the NIF (`ep_encoder.c` / `ep_decoder.c`) and its Erlang module |
| examples | [`examples/addressbook.escript`](examples/addressbook.escript) | hand-built defs, record round trip |
| tests | [`test/`](test/), [`c_src_tests/`](c_src_tests/) | EUnit suites and the NIF's C tests (`make erlang-test`) |
| benchmarks | [`benchmarks/`](benchmarks/) | vs gpb and gpb NIF (`make erlang-bench`) |
| conformance | [`conformance/`](conformance/) | status and plan: blocked on gpb record-name collisions |
| plugin | none | epb takes gpb definitions; `gpb_compile` / `protoc-erl` is the generator |

This is the rebar3 layout (`src/`, `c_src/`, `include/`, `priv/`, `test/`), the
local equivalent of the other bindings' `runtime/` + `tests/`.

## Build and run

Needs Erlang/OTP (see [`.tool-versions`](.tool-versions)) and a C compiler;
`rebar3` is bootstrapped by the binding's own `Makefile`.

```shell
make erlang-test    # from the repo root: EUnit + the NIF C tests
make erlang-bench   # from the repo root: epb vs gpb (+ gpb NIF, needs protoc + libprotobuf)
```

With rebar3 directly (from `bindings/erlang`):

```shell
rebar3 compile
rebar3 eunit
```

## Quick start

Given `x.proto`:

```protobuf
message Person {
  required string name = 1;
  required int32 id = 2;
  optional string email = 3;
}
```

Generate and compile Erlang with gpb:

```shell
export GPB_PATH=/path/to/gpb
${GPB_PATH}/bin/protoc-erl -I. x.proto
erlc -I${GPB_PATH}/include x.erl
```

Load the schema cache once, then encode and decode:

```erlang
1> rr("x.hrl").
['Person']
2> Msg = #'Person'{name = "abc def", id = 345, email = "a@example.com"}.
3> gpb_bin = x:encode_msg(Msg).
4> pbwire:load_cache(x:get_msg_defs()).
ok
5> epb_bin = pbwire:encode(Msg).
6> gpb_bin =:= epb_bin.
true
7> pbwire:decode(epb_bin, 'Person').
#'Person'{name = <<"abc def">>, id = 345, email = <<"a@example.com">>}
```

Convenience wrappers that load the cache on each call:

```erlang
pbwire:encode_msg(Msg, x:get_msg_defs()).
pbwire:decode_msg(Bin, 'Person', x:get_msg_defs()).
```

On gpb 5+ the introspection list was split: `get_msg_defs/0` keeps only
messages and enums, while `get_proto_defs/0` (compile with
`{introspect_get_proto_defs, true}`) also carries `{syntax, "proto3"}` and
`proto3_msgs`. Pass `Module:get_proto_defs()` there: without the proto3
markers a proto3 message is read as proto2 and its zero-valued singular
fields are written out instead of omitted. `get_msg_defs/0` still works for
proto2.

## API reference

| Function | Returns | Notes |
|---|---|---|
| `pbwire:load_cache(Defs)` | `ok` | `Defs` is `Module:get_msg_defs()`, or `Module:get_proto_defs()` on gpb 5+ (it adds the proto3 markers); replaces any previously loaded schema for the whole node |
| `pbwire:purge_cache()` | `ok` | Frees the loaded schema; later `encode/1` / `decode/2` raise `cache_not_exists` |
| `pbwire:set_opts(Opts)` | `ok` | Call before `load_cache/1`; unknown options or non-boolean values raise `badarg` |
| `pbwire:encode(Msg)` | `binary()` | `Msg` is a gpb record or map tuple; raises `badarg` if it is not a tuple |
| `pbwire:decode(Bin, MsgName)` | record/map tuple | `MsgName` is the message atom, e.g. `'Person'`; raises `unknown_msg` if the schema has no such message |
| `pbwire:encode_msg(Msg, Defs)` | `binary()` | `load_cache(Defs)` + `encode(Msg)` |
| `pbwire:decode_msg(Bin, Name, Defs)` | tuple | `load_cache(Defs)` + `decode(Bin, Name)` |
| `pbwire:debug_term(Term)` | term | Prints the internal node layout of the loaded schema (debugging aid) |

Calling `encode/1` or `decode/2` before `load_cache/1` raises `cache_not_exists`. All functions are
safe to call concurrently from many processes: the schema cache is shared and read-locked, and each
scheduler gets its own scratch buffers.

## Supported protobuf features

| Feature | Support |
|---|---|
| Scalar types | `int32/64`, `uint32/64`, `sint32/64`, `fixed32/64`, `sfixed32/64`, `float`, `double`, `bool`, `string`, `bytes` |
| Enums | Yes, including aliases and negative values (encoded as 64-bit varints, as protobuf requires) |
| Occurrence | `required`, `optional`, `repeated` (`packed` and unpacked) |
| Composite | Nested messages, `oneof`, `map<K, V>`, `group` (legacy) |
| Unknown fields | Preserved on decode and re-emitted on encode |
| proto3 | Yes, including default-value and `optional` semantics matching gpb |

## Options

Call `pbwire:set_opts/1` before `load_cache/1`:

| Option                   | Default | Effect                                                 |
|--------------------------|---------|--------------------------------------------------------|
| `{string_as_list, true}` | `false` | Decode string fields as lists instead of binaries      |
| `{with_utf8, true}`      | `false` | Allow non-Latin-1 UTF-8 in string fields when encoding |

`string` fields decode to binaries by default; non-Latin-1 text needs
`{with_utf8, true}` or encoding raises.

## Benchmark

`benchmarks/proto-bench` measures epb against **gpb** (pure Erlang) and the
**gpb NIF** (libprotobuf behind gpb's own NIF generator) on the same schema and
payload as every other binding in this repo:
`bindings/c/tests/data/matrix3.proto` with the repeated fields scaled
x0 / x1 / x100 / x3000 (136 / 174 / 2469 / 74913 bytes on the wire). Both
backends build the same gpb record, so the timer measures the codec, not the
tree. Every backend must match gpb byte for byte (encoding the record and
re-encoding a decoded reference) before anything is timed.

The gpb NIF baseline needs `protoc` plus the protobuf C++ runtime; the suite
builds and runs it when `pkg-config --exists protobuf` succeeds, otherwise the
table simply has no `gpb-nif` column. Each op is the best of three timed
samples (`BENCH_TARGET_TIME` seconds each, default 2) after one warm-up pass.

Copied from [`BENCHMARKS.md`](../../BENCHMARKS.md), which is the single source
for all performance numbers; higher is better, MB/s over wire bytes. See
[benchmarks/README.md](benchmarks/README.md) for all targets (per-backend
runs, quick mode, the gpb NIF baseline).

<!-- BENCHMARK_RESULTS_START -->
Last run: 2026-11-04 10:29:33 UTC (matrix3.proto, the four standard tiers; Erlang 29.0.1 per `.tool-versions`; machine Apple M4 Pro)

| payload | op | pbwire-erlang (MB/s, x gpb) | gpb (MB/s) | gpb-nif (MB/s) |
|---|---|---|---|---|
| 136 B | encode | 491.2 (2.47x) | 198.9 | 56.5 |
| 136 B | decode | 512.0 (2.18x) | 234.9 | 60.9 |
| 174 B | encode | 472.0 (2.44x) | 193.5 | 63.7 |
| 174 B | decode | 525.6 (2.21x) | 238.2 | 80.3 |
| 2469 B | encode | 407.9 (3.60x) | 113.4 | 43.9 |
| 2469 B | decode | 326.1 (1.81x) | 180.0 | 135.5 |
| 74913 B | encode | 441.7 (4.21x) | 104.9 | 109.6 |
| 74913 B | decode | 354.1 (1.98x) | 178.8 | 182.8 |

Higher is better. Throughput in MB/s on serialized wire size. Case names are the
four standard tiers at the repo root ([BENCHMARKS.md](../../BENCHMARKS.md)).
<!-- BENCHMARK_RESULTS_END -->

Refresh the table after a run (rewrites this block and the one in
`benchmarks/README.md`):

```shell
make update-readme    # from bindings/erlang/benchmarks (runs the suite)
```

## FAQ

### Do I need to regenerate my `.proto` files or change my records?

No. Keep gpb's `protoc-erl` output and record definitions; only the encode/decode calls change.

### Does it produce the same bytes as gpb?

Yes for the messages covered by the test suite (the tests assert `gpb_bin =:= epb_bin`). Both follow
the protobuf wire format, so any conforming decoder reads either.

### Can I use it from Elixir?

Yes. It is an Erlang library with no non-Erlang API: generate the schema module with gpb's
`protoc-erl`, load it once with `:pbwire.load_cache(X.get_msg_defs())`, and call
`:pbwire.decode(bin, :'Person')` instead of the generated decode.

### Why does `encode/1` or `decode/2` raise `cache_not_exists`?

No schema is loaded yet (or `purge_cache/0` was called). Call `pbwire:load_cache/1` first, or
use `encode_msg/2` / `decode_msg/3`.

### Why do I get `badarg` from `set_opts/1`?

Only `{string_as_list, boolean()}` and `{with_utf8, boolean()}` are accepted, and the argument must be
a proper list of such tuples.

### Can several processes or applications use it at once?

Encoding and decoding are safe from any number of processes. The schema cache is global to the node,
so two applications with different `.proto` schemas must not share one VM — give each its own node, or
coordinate the single `load_cache/1` call.

### Does it run on dirty schedulers?

No; calls execute on normal schedulers and hold a NIF read lock for the duration of the call. Keep
individual messages reasonably sized, or shard very large payload work across pools.

### Why does memory stay high after encoding one huge message?

The per-scheduler encode buffer grows geometrically to fit the largest message encoded and is not
shrunk afterwards; it is released when the NIF is unloaded (VM exit). This is a deliberate trade-off
for the fast path.

### How does it handle malformed wire data?

Decoding is strict, following the reference C++/Java parsers: a tag is a varint32 (at most
five bytes, bits above bit 31 dropped), a length prefix must be a strict varint32, group
nesting is capped at 100 and reserved wire types raise. A malformed binary raises instead of
returning a partially decoded message, so `decode/2` is safe on untrusted input. gpb is more
lenient about oversized tags; for those inputs the two implementations may disagree.

## Development

```shell
make tests              # EUnit (gpb compatibility and PropEr round-trip tests included)
make c-tests            # C unit tests
make c-format           # clang-format >= 19 applied to c_src/ and c_src_tests/
```

Layout: `src/pbwire.erl` (public API + NIF stubs) → `c_src/pbwire.c` (NIF entry
points, per-thread contexts, cache swap), `c_src/ep_encoder.c` / `c_src/ep_decoder.c`
(single-pass wire codec over the schema cache), `test/` (EUnit, gpb parity, PropEr fuzzing),
`benchmarks/` (proto-bench suite). Each thread also owns a core `pbw_ctx` (arena +
reusable buffers) whose user slot points back at the thread data.

See [`AGENTS.md`](AGENTS.md) for the invariants that must hold in this codebase — an uncaught C
exception or a bad term handled in the NIF kills the whole VM.

## Typed models

gpb records are the typed model: `protoc-erl` generates the record definitions and `get_msg_defs/0`,
and the binding encodes/decodes those records directly. There is no second generator and no
`bindings/typed` output for Erlang.

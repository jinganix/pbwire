# pbwire Lua binding

A C module (`runtime/pbwire.c`, Lua 5.1 through 5.5 and LuaJIT) that links the
pbwire C core statically. No luarocks dependency, no `protoc` plugin, no
generated code: a `FileDescriptorSet` (`protoc --descriptor_set_out`) is loaded
at runtime and drives every field lookup, so the field numbers, types and
presence rules are the descriptor's, not a hand-written copy.

Messages are plain Lua tables, which is what game code (skynet, cocos, xLua)
already passes around:

```lua
package.cpath = "./?.so;" .. package.cpath
local pbwire = require("pbwire")
local schema = pbwire.loadfile("sample.desc")

local wire = schema:encode("pbwire.lua.Sample", {
    i32 = -7, name = "alice", inner = { s = "nested" }, nums = { 1, 2, 3 },
})
local back = schema:decode("pbwire.lua.Sample", wire)

print(back.name)            -- alice
print(back.inner.s)         -- nested
assert(schema:encode("pbwire.lua.Sample", back) == wire)

-- Hot path: build once, encode per send (game broadcasts).
local m = schema:message("pbwire.lua.Sample")
m:set({ i32 = -7, name = "alice" })
local w1 = m:encode()       -- byte-identical to schema:encode of the table
```

## Build and run

```shell
./build.sh                   # or: make lua-bind (LUA= picks the interpreter)
make lua-test                # builds, then runs tests/test.lua
lua examples/roundtrip.lua
```

`LUA=` selects the interpreter and `LUA_INCDIR=` overrides its headers:

```shell
LUA=luajit LUA_INCDIR=/usr/include/luajit-2.1 ./build.sh
```

The test prints one `ok`/`FAIL` line per check and exits non-zero on a failure;
`tests/data/sample.desc` is checked in, so no `protoc` is needed at test time.

## Design: tables, one arena per schema, builders for hot loops

The schema owns one `pbw_ctx` (arena + encode buffer, reset per call), so
steady use stops calling `malloc` after warmup: values allocate by bump
pointer, the encoder reuses its buffer, and nothing is freed per call. Fill
and decode run no metamethods (raw table access only: `lua_next` never
consults `__index`/`__pairs`), so no reentrant call can reset the arena
mid-use, and Lua states are single-threaded anyway.

Fields are read in whatever order the table carries them: the core emits
fields in descriptor order no matter the insertion order, so equal tables
encode to equal bytes and match `protoc --encode` byte for byte (verified on
the `matrix3` small tier). Unknown keys fail instead of being silently
dropped.

`schema:message()` returns a builder for loops that encode far more often
than they rebuild: `set(tbl)` merges a table into its tree (assigning a
repeated field replaces it, like Ruby), `encode()` is one core walk plus one
buffer malloc. That is the ~7-8x row in the benchmark below.

## API

| member | meaning |
|---|---|
| `pbwire.loadfile(path)` | load a `FileDescriptorSet` file; errors with the core's `pbw_schema_error()` text when the set is rejected |
| `pbwire.load(bytes)` | same from a string holding the set |
| `schema:encode("pkg.Name", tbl)` | wire bytes as a Lua string |
| `schema:decode("pkg.Name", wire)` | a fresh table; unknown fields are skipped, so a re-encode is canonical rather than byte-exact |
| `schema:roundtrip("pkg.Name", wire)` | gateway passthrough: decode and re-encode in the core, unknown fields survive |
| `schema:message("pkg.Name")` | an empty builder message (anchors the schema until collected) |
| `msg:set(tbl)` | merge a table into the builder (chainable); repeated fields replace |
| `msg:encode()` | wire bytes of the builder |

Every `pbw_status` is checked and raised as a Lua error; a wrong Lua type for a
value raises; malformed wire data is rejected by `decode`. Nothing aborts or
segfaults on a bad call. Nesting is capped at `PBW_MAX_DEPTH` (256) both ways.

### Value mapping

| protobuf | Lua |
|---|---|
| int32/int64/sint*/sfixed*/enum | numbers (integer); enum also accepts `"NAME"` strings, resolved through the enum |
| uint32/uint64/fixed* | numbers; `u64` max is passed as `-1` (same bit pattern) |
| bool | boolean |
| float/double | numbers |
| string/bytes | strings (embedded NULs survive) |
| message | nested table |
| repeated field | array table (1-based); `{}` clears, absent reads as `{}` |

An absent singular field decodes as `nil`, and a field set to `false`/`0` is
present and round trips as such (explicit presence, like the C API and the Ruby
binding: the reference encoder would omit an explicit proto3 zero, both parsers
read it back the same). Oneof: the last member in descriptor order wins on
encode; decode carries the live member only. Groups raise. Maps are arrays of
`{key=, value=}` entry tables.

64-bit integers need Lua 5.3+ integers for exactness; on Lua 5.1/LuaJIT (all
numbers are doubles) values past 2^53 round-trip approximately, the same caveat
`lua-protobuf` documents. Decode pushes a `uint64` past `LUA_MAXINTEGER` as the
nearest double.

## Deliberately skipped

- **No `protoc` plugin, no codegen, no second wire implementation.** The
  descriptor set is the schema; tables convert through the C core's message
  tree, builders encode it directly.
- **No JSON/text format, no deterministic encoding, no streaming.** All of
  those exist in the C API; this binding stays the smallest thing that
  reads and writes binary messages (plus gateway passthrough).

## Conformance

`conformance/testee.lua` drives the binary wire format of the official
`conformance_test_runner` through this binding (`make conformance-lua`,
reusing the cached protobuf checkout from `make conformance-setup`):

```
CONFORMANCE SUITE PASSED: 1389 successes, 1403 skipped, 2 expected failures,
0 unexpected failures.
```

Binary in -> binary out goes through `schema:roundtrip()`, so unknown fields
survive and a request is accepted exactly when the core accepts it. The 2
expected failures are the shared `conformance/failure_list_pbwire.txt` (tag
varints the reference C++ testee truncates identically); JSON/text/JSPB
requests answer `skipped` (the binding has no printers), and the text suite
passes 445/445 skipped the same way.

## Benchmark

`benchmarks/bench.sh` runs the four standard tiers from
[`BENCHMARKS.md`](../../BENCHMARKS.md) against `lua-protobuf` 0.6.0 on the same
`matrix3.proto` (small / medium / large / xlarge scale the repeated fields
x0 / x1 / x100 / x3000). The gate is tier size (our bytes are canonical) plus a
full-field expectation check; the baseline only has to agree on size and decode,
because `lua-protobuf` writes nested messages in table hash order, so raw bytes
are not comparable. Needs only Lua and a C compiler, plus the baseline for its
columns (`luarocks install lua-protobuf`). Copied from that run (Apple M4 Pro,
Lua 5.5.1); higher is better, MB/s over wire bytes:

| payload | op | pbwire-lua (MB/s, x lua-protobuf) | lua-protobuf (MB/s) |
|---|---|---|---|
| 136 B | encode | 84.7 (0.64x) | 132.3 |
| 136 B | encode_msg | 933.9 (7.06x) | 132.3 |
| 136 B | decode | 101.2 (1.14x) | 88.6 |
| 174 B | encode | 97.3 (0.68x) | 144.0 |
| 174 B | encode_msg | 995.0 (6.91x) | 144.0 |
| 174 B | decode | 101.6 (1.14x) | 89.3 |
| 2469 B | encode | 135.3 (0.79x) | 170.4 |
| 2469 B | encode_msg | 1209.5 (7.10x) | 170.4 |
| 2469 B | decode | 108.3 (1.20x) | 90.3 |
| 74913 B | encode | 150.0 (0.79x) | 189.9 |
| 74913 B | encode_msg | 1550.3 (8.17x) | 189.9 |
| 74913 B | decode | 108.0 (1.15x) | 94.0 |

`encode_msg` is the builder hot path: one core walk plus one buffer malloc
per send, no table conversion, which is why it runs at the core's own
~1.0-1.5 GB/s. The table path converts table→tree→wire (two passes where
the baseline writes one), so it trails ~0.6-0.8x -- the same structural
reason the Python dict path trails upb. Decode wins at every tier because it
is one validating parse straight into tables with no tree kept.

## Layout

| Piece | Path |
|---|---|
| the binding | [`runtime/pbwire.c`](runtime/pbwire.c) |
| build | [`build.sh`](build.sh) |
| example | [`examples/roundtrip.lua`](examples/roundtrip.lua) |
| self-check | [`tests/test.lua`](tests/test.lua) + [`tests/test_fuzz.lua`](tests/test_fuzz.lua) (300 seeded cases vs `lua-protobuf`) |
| benchmark | [`benchmarks/bench.lua`](benchmarks/bench.lua) | the four standard tiers against `lua-protobuf` |
| conformance | [`conformance/testee.lua`](conformance/testee.lua) | binary wire format through the official runner |
| fixture | [`tests/data/sample.proto`](tests/data/sample.proto) and the checked-in `sample.desc`; [`tests/data/sample2.proto`](tests/data/sample2.proto) (`sample2.desc`: proto2 required/closed-enum/groups) |

Regenerate the descriptor set after editing the fixture (it is checked in, so the
tests need no `protoc`):

```shell
(cd bindings/lua/tests/data && protoc --descriptor_set_out=sample.desc sample.proto)
```

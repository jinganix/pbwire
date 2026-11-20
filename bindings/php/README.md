# pbwire PHP binding

Two versions, one API (`Pbwire\Schema`, `Type`, `Field`, `Message`, `Value`):

- `runtime/` is the default: an `FFI` wrapper over the C ABI in
[`include/pbwire.h`](../../include/pbwire.h). PHP ships `ext-ffi`, so there is no
C extension to compile and no composer package: the shared library is
`dlopen()`ed, and a `FileDescriptorSet` (`protoc --descriptor_set_out`) becomes
the schema at run time. No generated code, no build step.
- `ext/` is the native sibling: the same classes as a C extension with the
  core compiled in (like the Ruby C extension), so small payloads skip the
  per-call FFI trampoline. `runtime/pbwire.php` returns early when it is
  loaded, so the two never declare the same class twice.

```php
use Pbwire\Schema;

$schema = Schema::fromFile('tests/data/matrix3.desc');
$ty = $schema->message('structtest.Scalars3');
$innerTy = $schema->message('structtest.Inner3');

$msg = $ty->newMessage();
$msg->setInt('i32', -42)
    ->setString('str', 'alice');
foreach (['a', 'bb'] as $tag) {
    $msg->setString('tags', $tag);                  // repeated: appends
}
$msg->setMessage('inner')                           // nested: created in place
    ->setString($innerTy->field('name'), 'kid')
    ->setInt('value', 42);

$wire = $msg->encode();
$decoded = $ty->decode($wire);                      // unknown fields preserved
assert($decoded->encode() === $wire);               // byte-exact round trip
assert($decoded->at('tags', 1)?->asString() === 'bb');
```

Fields are addressed by name (`'i32'`) or by a `Field` handle from
`$ty->field('i32')`; the handle skips the name lookup and is what a hot loop
should reuse.

## Requirements

PHP 8.0+ with `ext-ffi` (bundled by default; `php -m | grep ffi`) and the shared
library: `make shared` builds `build/libpbwire.dylib` / `.so`. `PBWIRE_LIB`
points at it; otherwise the binding looks next to the checkout and in `./build`.

## Build and run

```shell
make shared
php tests/test.php                                  # FFI self-check vs pinned protoc bytes
make php-ext-test                                   # same checks through the C extension
php examples/roundtrip.php                          # example
```

```shell
cd ext && ./build.sh                               # builds ext/modules/pbwire.so
php -d extension=$PWD/ext/modules/pbwire.so tests/test.php
```

`composer.json` requires `ext-ffi` for the default wrapper; the C extension
needs only the `.so` above (no `PBWIRE_LIB`, no shared library).

## Benchmarks

```shell
./benchmarks/bench.sh    # FFI, then the C extension when built, vs the official runtime (pure + C ext)
```

`bench.sh` builds `ext/` when `phpize` exists and times both versions back
to back. Rows are named by the arm: `pbwire-php-tree` (prebuilt `Message`),
`pbwire-php-shim` (native array through one C-extension entry),
`pbwire-php-shim-ffi` (the same array API on `ext-ffi`, the no-compiler
deployment).

The official side is the `php/src` tree of the cached protobuf checkout that
`make conformance-setup` clones (or `PBWIRE_PROTOBUF_SRC`), loaded through a
plain SPL autoloader: no composer, no PEAR. When the protobuf C extension is
installed the harness times it first (`-c` rows), then re-runs under `php -n`
for the pure-PHP baseline. The four tiers are small / medium /
large / xlarge (the repeated fields scaled x0 / x1 / x100 / x3000). Copied from
[BENCHMARKS.md](../../BENCHMARKS.md), which is the single source for all
performance numbers; higher is better, MB/s over wire bytes:

| payload | op | pbwire-php-tree (MB/s, x protobuf-php-c) | pbwire-php-shim (MB/s, x) | protobuf-php-c (MB/s) |
|---|---|---|---|---|
| 136 B | encode | 1519.8 (1.71x) | 186.5 (0.21x) | 886.2 |
| 136 B | decode | 787.3 (1.84x) | 168.0 (0.39x) | 428.7 |
| 174 B | encode | 1518.2 (1.55x) | 201.2 (0.20x) | 982.0 |
| 174 B | decode | 753.5 (1.75x) | 178.0 (0.41x) | 431.1 |
| 2469 B | encode | 1787.7 (1.34x) | 196.0 (0.15x) | 1332.4 |
| 2469 B | decode | 680.5 (1.29x) | 189.4 (0.36x) | 525.7 |
| 74913 B | encode | 1639.1 (1.23x) | 214.6 (0.16x) | 1332.0 |
| 74913 B | decode | 750.8 (1.29x) | 206.9 (0.36x) | 580.5 |

The tree handle is the shipped path and wins every tier. The native array
entry is below the official C extension because each field costs a hashtable
lookup plus a C tree store, and decode materializes the array eagerly.
`bench.sh` also times the `ext-ffi` deployment separately: the same
`Type::encodeArray`/`decodeArray` API with the walk in PHP, one FFI call per
field, which is interpreter-scale (~5 MB/s) and published as
`pbwire-php-shim-ffi` so the no-compiler cost is not quoted as the C extension.

Without the C extension the same harness measures the pure-PHP runtime
instead (single `php -n`-style run): xlarge `pbwire-php-tree` 1552.5 MB/s vs
3.7 (encode) and 748.9 vs 5.4 (decode). The ~140-420x gap is the interpreter,
not the codec.

`pbench/` is the re-runnable arm comparison; `pbench/README.md` has the
provisional table, the deferred-core TODO and the verdict.

Strings cross with explicit lengths, so embedded NUL bytes survive both
ways (`setString()` and `setBytes()` accept them; a `string` field accepts
byte payloads too, which is how protobuf's wire format works).

## Threads and lifetimes

No global state beyond the library handle. A `Schema` or `Message` belongs to one
thread; PHP's common runtimes (php-fpm, CLI) are one request per process, so this
matches how it is used.

A `MessageType` and every `Message` keep the `Schema` object reachable, so
dropping your own reference does not free the native schema under them. An
explicit `close()` is still the caller's promise that nothing else uses it --
close messages and types first, like any arena.

## Generated models

`protoc-gen-pbwire` (in `../../codegen`) generates official-runtime-shaped
classes over this dynamic API: `getX()`/`setX()`, `hasX()`/`clearX()` for
optional and message fields, `RepeatedField` for repeated, `getKind()` /
`whichOneof()` for oneofs, enums as class constants, and
`serializeToString()`/`mergeFromString()`. Each file splits in two:
`<base>.php` (models) and `PbwireFile_<Stem>.php` (the holder: embedded
set plus its loader). `--pbwire_opt=lang=php[,model=bool][,register=bool]`
`[,registry=bool]` (all default `true`): `model=false` leaves the holder
for dynamic-only users; `register=false` drops the lazy load so `init()`
(embedded bytes) or `use_schema()` (an external `Schema`, dual
registration) must run first; `registry=false` drops `MESSAGE_NAMES` +
`preload()`. The committed output is `tests/gen/` and
`examples/gen/` plus the protoc `--php_out` oracle in `tests/gen_oracle/`;
`tests/compat_test.php` runs TESTSPEC T1–T6 against the pinned runtime
(protobuf v36.2 `php/src`), `tests/generated_test.php` is the oracle-free
smoke, and `examples/roundtrip.php` assigns typed fields only. See
`../../codegen/README.md`.

## Core coverage

Everything the C ABI exposes is reachable from PHP, through the same core (no
second codec):

| member | what it does |
|---|---|
| `Type::fromJson($json[, $ignoreUnknown[, $limits]])` / `Message::toJson()` | JSON mapping in and out |
| `Type::fromText($text[, $limits])` / `Message::toText([$printUnknown])` | text format in and out |
| `Message::encodeDeterministic()` | map entries sorted by key |
| `Message::encodeDelimited()` / `Message::encodeDelimitedTo($cap)` / `Type::decodeDelimited($wire)` | length-prefixed framing |
| `Message::encodedSize()` / `Message::encodeTo($cap)` | exact size, caller-owned buffer |
| `Message::timestampMillis()` / `Type::timestampFromMillis($ms)` (and the duration twins) | Timestamp/Duration as milliseconds |
| `Message::anyPack($typeUrl, $payload)`, `Message::anyTypeUrl()`, `Message::anyPayload()` | `google.protobuf.Any` |
| `Message::fieldmaskPaths()` | `google.protobuf.FieldMask` |
| `Type::encodeArray($array)` / `Type::decodeArray($wire)` | native array in/out; one entry call in the C extension, the same API on `ext-ffi` |
| `Schema::services()`, `Schema::service($name)`, `Schema::resolve($service, $method)` | routing metadata, no RPC |
| `$limits = ['max_bytes' => n, 'max_depth' => n]` | bounds on every decode/parse/load entry point |

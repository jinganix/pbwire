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
to back (`pbwire-php` rows for FFI, `pbwire-php-c` for the native extension).

The official side is the `php/src` tree of the cached protobuf checkout that
`make conformance-setup` clones (or `PBWIRE_PROTOBUF_SRC`), loaded through a
plain SPL autoloader: no composer, no PEAR. When the protobuf C extension is
installed the harness times it first (`-c` rows), then re-runs under `php -n`
for the pure-PHP baseline. The four tiers are small / medium /
large / xlarge (the repeated fields scaled x0 / x1 / x100 / x3000). Copied from
[BENCHMARKS.md](../../BENCHMARKS.md), which is the single source for all
performance numbers; higher is better, MB/s over wire bytes:

| payload | op | pbwire-php-c (MB/s, x protobuf-php-c) | protobuf-php-c (MB/s) |
|---|---|---|---|
| 136 B | encode | 1370.4 (1.63x) | 842.4 |
| 136 B | decode | 683.6 (1.65x) | 414.6 |
| 174 B | encode | 1299.6 (1.41x) | 921.4 |
| 174 B | decode | 637.9 (1.56x) | 407.8 |
| 2469 B | encode | 1618.7 (1.24x) | 1300.5 |
| 2469 B | decode | 607.8 (1.21x) | 501.0 |
| 74913 B | encode | 1478.3 (1.11x) | 1326.1 |
| 74913 B | decode | 661.1 (1.15x) | 572.7 |

The rows above are the C extension (`pbwire-php-c`); `bench.sh` times the
FFI wrapper first (`pbwire-php` rows) when `phpize` is missing or the build
fails. The FFI version pays one trampoline per call, so it trails on the
small tiers and converges as the payload grows (xlarge 1.09x / 1.23x vs the
same C-extension baseline in the same harness).

Without the C extension the same harness measures the pure-PHP runtime
instead (single `php -n`-style run): xlarge 1384.2 MB/s vs 3.7 (encode) and
658.7 vs 5.2 (decode). The ~300x gap is the interpreter, not the codec.

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
`serializeToString()`/`mergeFromString()`. Each file embeds its own
`FileDescriptorSet`, so no `.desc` is needed at use time; the wire format
stays in the C core. The committed output is `tests/gen/` and
`examples/gen/` plus the protoc `--php_out` oracle in `tests/gen_oracle/`;
`tests/compat_test.php` runs TESTSPEC T1–T6 against the pinned runtime
(protobuf v36.2 `php/src`), `tests/generated_test.php` is the oracle-free
smoke, and `examples/roundtrip.php` assigns typed fields only. See
`../../codegen/README.md`.

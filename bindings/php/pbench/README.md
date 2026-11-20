# PHP pbench: array in, one extension call

The arms, the gate and the timing loop from
[`bindings/README.md`](../../README.md#pbench-three-arms-one-contract), applied
to `structtest.Scalars3` from `bindings/c/tests/data/matrix3.proto` and the four
standard tiers. Every arm does the same user-visible task: a native object (a
PHP array in the generated model's shape) in and wire bytes out; wire bytes in
and a fully readable PHP array out.

| arm | file | mechanism | what it answers |
|---|---|---|---|
| `tree` | `a-tree.php` | the shipped builder/wire-view handle: a prebuilt `Pbwire\Message` encoded, `Type::decode()` for wire -> tree | what the codec costs once the tree exists |
| `shim` | `b-shim.php` | `Type::encodeArray()` / `decodeArray()` in the C extension: one extension entry walks the PHP array in C (`zend_hash_str_find` per field, recursion) and fills the tree, or materializes the array | is a boundary worth paying for at all |
| `shim-ffi` | `b-shim.php` | the same methods on the shipped `ext-ffi` wrapper (the no-compiler deployment): the walk is PHP-side, one FFI call per field | what the same task costs without a compiler |
| `core` | `n/a` | a pure-PHP port of the C core | deferred: the official pure-PHP implementation is already the baseline, so a second one would measure our code against the competitor |

`core` prints `n/a` until a no-compiler deployment needs a PHP-native engine;
there is no second wire implementation in this binding.

TODO in `runtime/pbwire.php`: `shortcut: no pure-PHP core, reopen when a
no-compiler deployment needs it`.

## Native object

A PHP array keyed by proto field name: singular scalars as PHP values, a nested
associative array for a message, and a list for a repeated field (an absent
repeated field reads back as an empty list). `int` is platform width, so a
`uint64` past `PHP_INT_MAX` must arrive as its **decimal string**; any other
form is rejected rather than truncated, and it decodes back as the same string.

## Gate

```
make php-ext                                       # the C extension
php bindings/php/pbench/gate.php                   # ext-ffi arm: PASS / exit 1
php -d extension=.../pbwire.so bindings/php/pbench/gate.php  # C arm
```

The gate asserts, per tier, that `tree` and the loaded shim arm reproduce the
official runtime's bytes, that each decode reads every field
`benchmarks/bench.php` checks (`i32`, `i64`, `u64`, `f32`, `f64`, `str`,
`color`, `inner.name`, `inner.value`, the repeated lengths and last elements,
the proto3 optionals), that it re-encodes to the same bytes, and that it equals
the official runtime's own materialization field for field. Both published
shim builds (C extension and `ext-ffi`) must pass and produce identical bytes.

## Timing

`compare.php` interleaves the loaded shim arm, the tree handle and the official
runtime in one loop per tier and reports the median of five rounds. The formal,
recorded run is `make php-bench`, which times `pbwire-php-shim` (C extension),
`pbwire-php-shim-ffi` (`ext-ffi`) and `pbwire-php-tree`.

## Provisional table (Stage 3)

`php bindings/php/pbench/compare.php` in two runs (the shipped `ext-ffi` wrapper,
then the C extension with `-d extension=.../pbwire.so`), median of five
interleaved rounds, 2026-11-21, MB/s over wire bytes. The C extension row is
`pbwire-php-shim` (one extension entry, the array walk in C); the FFI row is
`pbwire-php-shim-ffi` (the same API without a compiler, one FFI call per field).
`tree` is the prebuilt `Message` handle. The baseline is the official C
extension.

| tier | arm | encode | decode |
|---|---|---|---|
| 136 B | `pbwire-php-tree` | 1319.5 | 649.2 |
| 136 B | `pbwire-php-shim` | 151.2 | 159.8 |
| 136 B | `pbwire-php-shim-ffi` | 5.0 | 5.8 |
| 136 B | `protobuf-php-c` | 869.9 | 413.9 |
| 174 B | `pbwire-php-tree` | 833.7 | 472.8 |
| 174 B | `pbwire-php-shim` | 122.8 | 119.5 |
| 174 B | `pbwire-php-shim-ffi` | 5.2 | 5.6 |
| 174 B | `protobuf-php-c` | 556.2 | 339.4 |
| 2469 B | `pbwire-php-tree` | 1713.0 | 644.0 |
| 2469 B | `pbwire-php-shim` | 149.9 | 144.2 |
| 2469 B | `pbwire-php-shim-ffi` | 5.1 | 4.4 |
| 2469 B | `protobuf-php-c` | 1213.8 | 498.3 |
| 74913 B | `pbwire-php-tree` | 1711.9 | 711.8 |
| 74913 B | `pbwire-php-shim` | 190.1 | 211.9 |
| 74913 B | `pbwire-php-shim-ffi` | 5.5 | 4.7 |
| 74913 B | `protobuf-php-c` | 1301.5 | 537.7 |

The reading: the tree handle stays ahead of the official C extension because it
never crosses into PHP data; the array path loses because every field is a
hashtable lookup plus a C tree store, and the FFI version additionally pays one
trampoline per field (interpreter-scale, ~5 MB/s). Both shim rows are below the
baseline, so Stage 6 has cells to target.

## Verdict

Filled in after Stage 4; the formal `make php-bench` values are the ones
published in [BENCHMARKS.md](../../../BENCHMARKS.md).

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

## Run table

Provisional numbers and the verdict live here after Stage 3/4; the formal
`make php-bench` values are the ones published in
[BENCHMARKS.md](../../../BENCHMARKS.md).

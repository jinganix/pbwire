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

## Formal table (Stage 5/6)

Final `make php-bench` (`results/bench-php-20261120T234351Z.json`), MB/s over
wire bytes, with the Stage 6 encode scratch context in place:

| tier | `pbwire-php-tree` enc/dec | `pbwire-php-shim` enc/dec | `pbwire-php-shim-ffi` enc/dec | `protobuf-php-c` enc/dec |
|---|---|---|---|---|
| 136 B | 1519.8 / 787.3 | 186.5 / 168.0 | 5.0 / 5.7 | 886.2 / 428.7 |
| 174 B | 1518.2 / 753.5 | 201.2 / 178.0 | 5.3 / 5.6 | 982.0 / 431.1 |
| 2469 B | 1787.7 / 680.5 | 196.0 / 189.4 | 5.1 / 4.4 | 1332.4 / 525.7 |
| 74913 B | 1639.1 / 750.8 | 214.6 / 206.9 | 5.6 / 4.7 | 1332.0 / 580.5 |

Stage 6 A/B (two `.so` builds, xlarge) accepted one change: one scratch
`pbw_ctx` per `Type`, reset per `encodeArray` call, with `pbw_encode_ctx`
writing once into its reused buffer; it won every round (390346.6 -> 340963.3
ns/op and the four rounds after it). Decoding through `pbw_decode_ctx` on the
same context was rejected (3 of 5 rounds lost). Details and all rounds are in
[docs/perf-attempts.md](../../../docs/perf-attempts.md).

## Verdict

**Keep the shipped builder/wire-view runtime and publish the array rows.**
`pbwire-php-tree` (prebuilt `Message`) is above the official C extension at every
tier (136 B 1519.8 / 787.3 MB/s, 74913 B 1639.1 / 750.8 against 886.2 / 428.7 and
1332.0 / 580.5). The native array `shim` is one C-extension entry, but it is
below the baseline on every cell (136 B 186.5 / 168.0, 74913 B 214.6 / 206.9),
and `shim-ffi` is interpreter-scale (~5 MB/s) because it pays one FFI trampoline
per field.

It is not (a) -- "replace the shipped fast path": the array walk costs a
hashtable lookup plus a C tree store per field, while the tree handle writes its
already-built tree; replacing the handle would slow every encode/decode caller.

It is not (c) -- "split by payload size": there is no crossover. The tree handle
is above the official C extension at all four tiers and the array path is below
it at all four; larger payloads do not make the array walk cheaper per byte.

`core` stays `n/a`: the official pure-PHP runtime is already the baseline, so a
second pure-PHP engine would measure our code against the competitor; the TODO
in `runtime/pbwire.php` names the condition that would reopen it.

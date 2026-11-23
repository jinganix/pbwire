# PHP pbench: array in, one extension call

The arms, the gate and the timing loop from
[`bindings/README.md`](../../README.md#pbench-three-arms-one-contract), applied
to `structtest.Scalars3` from `bindings/c/tests/data/matrix3.proto` and the four
standard tiers. Every arm does the same user-visible task: a native object (a
PHP array in the generated model's shape) in and wire bytes out; wire bytes in
and a fully readable PHP array out.

| arm | file | mechanism | what it answers |
|---|---|---|---|
| `tree` | `a-tree.php` | the shipped per-field builder/wire-view API doing the whole task: a fresh `Pbwire\Message` per call, one `set` per field from the native array, `encode`, one `get` per field back out, `close`; no prebuilt tree before the timer | what a user of the per-field API pays for the whole task |
| `proxy` | `a-tree.php` | the prebuilt task: a held `Pbwire\Message` re-encoded, decode-and-free with no read-back | what the codec costs once the tree exists (same-run timing only, never the headline) |
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

The gate asserts, per tier, that `tree` (full task) and the loaded shim arm
reproduce the official runtime's bytes, that each decode reads every field
`benchmarks/bench.php` checks (`i32`, `i64`, `u64`, `f32`, `f64`, `str`,
`color`, `inner.name`, `inner.value`, the repeated lengths and last elements,
the proto3 optionals), that it re-encodes to the same bytes, and that it equals
the official runtime's own materialization field for field. The prebuilt
`proxy` ops keep their old checks (bytes plus a spot read-back). Both
published shim builds (C extension and `ext-ffi`) must pass and produce
identical bytes.

## Timing

`compare.php` interleaves the full-task tree arm, the prebuilt proxy, the
loaded shim arm and the official runtime in one loop per tier and reports the
median of five rounds. The formal, recorded run is `make php-bench`, which
times `pbwire-php-tree` (full task), `pbwire-php-proxy` (prebuilt),
`pbwire-php-shim` (C extension) and `pbwire-php-shim-ffi` (`ext-ffi`).

The full-task encode and the shim encode read the app-owned native array.
Reusing a plain-data fixture across iterations is standard harness practice
(Task 5 ledger ruling, Python worker D3, merged 99ac9ac); only the Message
-- fresh per call, closed inside the timer -- is timed. The tree decode
builds a fresh array and closes the Message before returning.

## Provisional table (Go-parity)

`php bindings/php/pbench/compare.php` in two runs (the shipped `ext-ffi` wrapper,
then the C extension with `-d extension=.../pbwire.so`), median of five
interleaved rounds, 2026-11-22, MB/s over wire bytes. The C extension row is
`pbwire-php-shim` (one extension entry, the array walk in C); the FFI row is
`pbwire-php-shim-ffi` (the same API without a compiler, one FFI call per field).
`tree` is the per-field full task; `proxy` is the prebuilt `Message` handle
(same-run timing only, never the headline). The baseline is the official C
extension.

| tier | arm | encode | decode |
|---|---|---|---|
| 136 B | `pbwire-php-tree` | 76.6 | 123.4 |
| 136 B | `pbwire-php-shim` | 174.7 | 159.3 |
| 136 B | `pbwire-php-shim-ffi` | 4.8 | 5.5 |
| 136 B | `pbwire-php-proxy` | 1269.0 | 642.1 |
| 136 B | `protobuf-php-c` | 859.5 | 422.8 |
| 174 B | `pbwire-php-tree` | 82.5 | 104.3 |
| 174 B | `pbwire-php-shim` | 191.0 | 166.2 |
| 174 B | `pbwire-php-shim-ffi` | 5.2 | 5.6 |
| 174 B | `pbwire-php-proxy` | 1330.1 | 638.8 |
| 174 B | `protobuf-php-c` | 941.7 | 416.0 |
| 2469 B | `pbwire-php-tree` | 82.1 | 53.2 |
| 2469 B | `pbwire-php-shim` | 195.2 | 191.6 |
| 2469 B | `pbwire-php-shim-ffi` | 5.1 | 4.3 |
| 2469 B | `pbwire-php-proxy` | 1720.1 | 661.7 |
| 2469 B | `protobuf-php-c` | 1309.4 | 503.2 |
| 74913 B | `pbwire-php-tree` | 87.7 | 56.5 |
| 74913 B | `pbwire-php-shim` | 215.2 | 208.1 |
| 74913 B | `pbwire-php-shim-ffi` | 5.5 | 4.7 |
| 74913 B | `pbwire-php-proxy` | 1864.9 | 731.0 |
| 74913 B | `protobuf-php-c` | 1318.8 | 556.0 |

The reading: the per-field full task sits below the official C extension on
every tier (one call per field against generated code), while the prebuilt
proxy stays above it. The one-entry shim is the fastest native-in/native-out
path but still below the baseline; the FFI version pays one trampoline per
field (interpreter-scale, ~5 MB/s). All native cells below the baseline go
to the optimization pass as candidates; nothing is attempted here.

## Formal table (Go-parity)

Final `make php-bench` (`results/bench-php-20261122T022541Z.json`), MB/s over
wire bytes, with the Stage 6 encode scratch context in place. The `proxy`
column is the prebuilt handle: same-run timing only, never the headline.

| tier | `pbwire-php-tree` enc/dec | `pbwire-php-shim` enc/dec | `pbwire-php-shim-ffi` enc/dec | `pbwire-php-proxy` enc/dec | `protobuf-php-c` enc/dec |
|---|---|---|---|---|---|
| 136 B | 73.9 / 117.0 | 173.8 / 159.0 | 5.0 / 5.7 | 1387.6 / 747.3 | 830.2 / 404.0 |
| 174 B | 83.1 / 103.4 | 197.9 / 176.1 | 5.2 / 5.7 | 1465.5 / 739.2 | 963.2 / 430.0 |
| 2469 B | 82.2 / 54.0 | 196.7 / 187.9 | 5.0 / 4.3 | 1672.0 / 674.8 | 1308.3 / 503.9 |
| 74913 B | 84.7 / 53.3 | 211.7 / 212.5 | 5.5 / 4.4 | 1612.8 / 713.2 | 1282.2 / 568.9 |

Throughput ratios are pbwire MB/s over baseline MB/s (higher is faster).
At xlarge against the official C extension: `tree` 0.07x encode / 0.09x
decode, `shim` 0.17x / 0.37x, `proxy` 1.26x / 1.25x (same run, never the
headline). The proxy/tree gap the old prebuilt `tree` row was hiding is
19.0x encode / 13.4x decode at xlarge. Against the pure-PHP baseline
(`php -n` run) at xlarge: `tree` 2.54x encode / 0.89x decode, `shim-ffi`
1.49x / 0.82x.

Stage 6 A/B (two `.so` builds, xlarge) accepted one change: one scratch
`pbw_ctx` per `Type`, reset per `encodeArray` call, with `pbw_encode_ctx`
writing once into its reused buffer; it won every round (390346.6 -> 340963.3
ns/op and the four rounds after it). Decoding through `pbw_decode_ctx` on the
same context was rejected (3 of 5 rounds lost). Details and all rounds are in
[docs/perf-attempts.md](../../../docs/perf-attempts.md).

## Verdict

**Keep the shipped builder/wire-view runtime and publish every row.**
`pbwire-php-tree` now times the per-field full task (fresh `Message` per call,
one `set`/`get` per field, `close` inside the timer) and sits below the
official C extension on every tier (xlarge 84.7 / 53.3 MB/s against 1282.2 /
568.9: 0.07x encode, 0.09x decode; small 0.09x / 0.29x, medium 0.09x / 0.24x,
large 0.06x / 0.11x). One call per field cannot match generated code, and
that is the honest price of the per-field API. The `shim` one-entry path is
the recommended native-object route -- fastest native-in/native-out on every
tier both ops (xlarge 211.7 / 212.5 against tree 84.7 / 53.3) -- but it is
also below the baseline on every cell (encode 0.15x-0.21x, decode 0.37x-0.41x
across tiers), and `shim-ffi` stays interpreter-scale (~5 MB/s, one FFI
trampoline per field). The prebuilt `proxy` stays the hot-loop form: above
the official C extension at every tier (xlarge 1.26x / 1.25x, same run, never
the headline).

It is not (a) -- "replace the shipped fast path": the held-handle path still
beats the official C extension at every tier while the per-field walk costs
~19x the re-encode at xlarge; replacing the handle would slow every
encode/decode caller that already holds a tree.

It is not (c) -- "split by payload size": there is no crossover. The full
task is below the official C extension at all four tiers and the proxy above
it at all four; larger payloads do not make the per-field walk cheaper per
byte.

`core` stays `n/a`: the official pure-PHP runtime is already the baseline, so a
second pure-PHP engine would measure our code against the competitor; the TODO
in `runtime/pbwire.php` names the condition that would reopen it.

Below-baseline native cells (`tree` and `shim` on every tier against the
official C extension) are candidates for the optimization pass; no
byte-changing or timer-shaping change is attempted here.

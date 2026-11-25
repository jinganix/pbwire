<?php

declare(strict_types=1);

/*
 * Provisional interleaved timing of the PHP pbench arms against the official
 * runtime: one round runs every op back to back per tier, five rounds, median
 * reported.
 *
 *   php bindings/php/pbench/compare.php
 *   php -d extension=.../pbwire.so bindings/php/pbench/compare.php
 *
 * `tree` is the per-field full task (fresh Message per call, one set per
 * field, encode, read back per field, close), `proxy` is the prebuilt Message
 * handle, the loaded shim arm is the array path (C extension or ext-ffi,
 * named by the row), the baseline is the official runtime with its model
 * materialized on decode.
 */

require __DIR__ . '/payload.php';
require __DIR__ . '/a-tree.php';
require __DIR__ . '/b-shim.php';

const PB_ROUNDS = 5;
const PB_MIN_SECONDS = 0.2;

function pbwire_pbench_calibrate(callable $op): int
{
    $count = 1;

    for (;;) {
        $start = hrtime(true);

        for ($i = 0; $i < $count; $i++) {
            $op();
        }

        if ((hrtime(true) - $start) / 1e9 >= PB_MIN_SECONDS || $count >= 100000000) {
            return $count;
        }
        $count *= 2;
    }
}

function pbwire_pbench_pass(callable $op, int $count): float
{
    $start = hrtime(true);

    for ($i = 0; $i < $count; $i++) {
        $op();
    }

    return (hrtime(true) - $start) / $count;
}

$shim = shim_impl();
$sink = 0;
$pbType = pbwire_pbench_type();
$pbInnerTy = $pbType->nestedType($pbType->field('inner'));
$medians = [];

/* Decode fairness (docs/perf-fairness.md section 4): the shim's C loop
 * already materializes every repeated element and every scalar before
 * returning, while the official mergeFromString is a bare parse. Both sides
 * fold the same gate-checked fields into the same live sink, so every decode
 * timer observes the full payload; the tree arm folds its full array too. */
function pbwire_pbench_fold_official(\Structtest\Scalars3 $m): int|float
{
    $acc = $m->getI32() + $m->getI64() + ($m->getU32() & 0xFFFFFFFF) + $m->getU64()
        + $m->getS32() + $m->getS64() + ($m->getFx32() & 0xFFFFFFFF) + $m->getFx64()
        + $m->getSfx32() + $m->getSfx64() + ($m->getFlag() ? 1 : 0)
        + strlen($m->getStr()) + $m->getColor()
        + strlen($m->getInner()->getName()) + $m->getInner()->getValue()
        + count($m->getNums()) + count($m->getTags())
        + count($m->getInners()) + count($m->getFlags())
        + $m->getOptI32() + strlen($m->getOptStr())
        + strlen((string) $m->getF32()) + strlen((string) $m->getF64());
    foreach ($m->getNums() as $v) {
        $acc += $v;
    }
    foreach ($m->getTags() as $s) {
        $acc += strlen($s);
    }
    foreach ($m->getInners() as $in) {
        $acc += strlen($in->getName()) + $in->getValue();
    }
    foreach ($m->getFlags() as $f) {
        $acc += $f ? 1 : 0;
    }

    return $acc;
}

function pbwire_pbench_fold_tree(array $back): int|float
{
    $acc = $back['i32'] + $back['i64'] + $back['u32'] + $back['s32'] + $back['s64']
        + $back['fx32'] + $back['fx64'] + $back['sfx32'] + $back['sfx64']
        + ($back['flag'] ? 1 : 0) + strlen($back['str']) + $back['color']
        + strlen($back['inner']['name']) + $back['inner']['value']
        + count($back['nums']) + count($back['tags'])
        + count($back['inners']) + count($back['flags'])
        + $back['opt_i32'] + strlen($back['opt_str'])
        + strlen((string) $back['f32']) + strlen((string) $back['f64']);
    foreach ($back['nums'] as $v) {
        $acc += $v;
    }
    foreach ($back['tags'] as $s) {
        $acc += strlen($s);
    }
    foreach ($back['inners'] as $in) {
        $acc += strlen($in['name']) + $in['value'];
    }
    foreach ($back['flags'] as $f) {
        $acc += $f ? 1 : 0;
    }

    return $acc;
}

foreach (PB_TIERS as $tier) {
    $label = $tier['label'];
    $repeats = $tier['repeats'];
    $payload = pbwire_pbench_payload($repeats);
    $wire = pbwire_pbench_official_encode($repeats);
    $tree = tree_prepare($wire);
    $official = pbwire_pbench_official($repeats);
    $officialClass = \Structtest\Scalars3::class;
    $ops = [
        'pbwire-php-tree' => [
            'encode' => static fn () => tree_encode_full($pbType, $pbInnerTy, $payload),
            'decode' => static function () use ($pbType, $wire, &$sink) {
                $out = tree_decode_full($pbType, $wire);
                $sink += pbwire_pbench_fold_tree($out);

                return $out;
            },
        ],
        'pbwire-php-proxy' => [
            'encode' => static fn () => tree_encode($tree),
            'decode' => static fn () => tree_decode($wire),
        ],
        $shim => [
            'encode' => static fn () => shim_encode($payload),
            'decode' => static fn () => shim_decode($wire),
        ],
        (extension_loaded('protobuf') ? 'protobuf-php-c' : 'protobuf-php') => [
            'encode' => static fn () => $official->serializeToString(),
            'decode' => static function () use ($officialClass, $wire, &$sink) {
                $m = new $officialClass();
                $m->mergeFromString($wire);
                $sink += pbwire_pbench_fold_official($m);

                return $m;
            },
        ],
    ];
    $counts = [];

    foreach ($ops as $impl => $implOps) {
        foreach (['encode', 'decode'] as $op) {
            $counts[$impl][$op] = pbwire_pbench_calibrate($implOps[$op]);
        }
    }
    for ($round = 0; $round < PB_ROUNDS; $round++) {
        foreach ($ops as $impl => $implOps) {
            foreach (['encode', 'decode'] as $op) {
                $medians[$label][$op][$impl][] =
                    pbwire_pbench_pass($implOps[$op], $counts[$impl][$op]);
            }
        }
    }
}

echo 'provisional: median of ' . PB_ROUNDS . " interleaved rounds, ns/op and MB/s\n";
printf("  %-8s %-6s %-20s %12s %10s\n", 'tier', 'op', 'impl', 'ns/op', 'MB/s');
foreach (PB_TIERS as $tier) {
    $bytes = $tier['bytes'];

    foreach (['encode', 'decode'] as $op) {
        foreach (array_keys($medians[$tier['label']][$op]) as $impl) {
            $samples = $medians[$tier['label']][$op][$impl];
            sort($samples);
            $ns = $samples[intdiv(count($samples), 2)];
            printf("  %-8s %-6s %-20s %12.1f %10.1f\n",
                $tier['label'], $op, $impl, $ns, $bytes / $ns * 1000);
        }
    }
}
echo "sink: $sink\n";

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
 * `tree` is the prebuilt Message handle, the loaded shim arm is the array path
 * (C extension or ext-ffi, named by the row), the baseline is the official
 * runtime with its model materialized on decode.
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
$medians = [];

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
            'encode' => static fn () => tree_encode($tree),
            'decode' => static fn () => tree_decode($wire),
        ],
        $shim => [
            'encode' => static fn () => shim_encode($payload),
            'decode' => static fn () => shim_decode($wire),
        ],
        'protobuf-php' => [
            'encode' => static fn () => $official->serializeToString(),
            'decode' => static function () use ($officialClass, $wire) {
                $m = new $officialClass();
                $m->mergeFromString($wire);

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

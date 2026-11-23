<?php

declare(strict_types=1);

/*
 * One A/B sample of the native-array entry: encode an xlarge (or named tier)
 * payload and decode its wire, best of three passes after calibration. It gates
 * bytes first, so a candidate that changes the wire aborts instead of timing.
 *
 *   php -d extension=<one .so> bindings/php/pbench/ab.php [tier]
 *
 * Prints "<encode ns/op> <decode ns/op>". ab.sh alternates two builds.
 */

require __DIR__ . '/payload.php';

$want = $argv[1] ?? 'xlarge';
$tier = null;

foreach (PB_TIERS as $candidate) {
    if ($candidate['label'] === $want) {
        $tier = $candidate;
    }
}
if ($tier === null) {
    fwrite(STDERR, "ab: no such tier: $want\n");
    exit(2);
}

$type = pbwire_pbench_type();
$wire = pbwire_pbench_official_encode($tier['repeats']);
$payload = pbwire_pbench_payload($tier['repeats']);

if (strlen($wire) !== $tier['bytes'] || $type->encodeArray($payload) !== $wire) {
    fwrite(STDERR, "ab: bytes differ from the official runtime\n");
    exit(3);
}
$back = $type->decodeArray($wire);
if ($type->encodeArray($back) !== $wire) {
    fwrite(STDERR, "ab: decode does not re-encode to the same bytes\n");
    exit(4);
}

/** Calibrate to ~0.2 s, then keep the best of three timed passes. */
function ab_best(callable $op): float
{
    $count = 1;

    for (;;) {
        $start = hrtime(true);

        for ($i = 0; $i < $count; $i++) {
            $op();
        }
        if ((hrtime(true) - $start) / 1e9 >= 0.2 || $count >= 100000000) {
            break;
        }
        $count *= 2;
    }
    $best = INF;

    for ($pass = 0; $pass < 3; $pass++) {
        $start = hrtime(true);

        for ($i = 0; $i < $count; $i++) {
            $op();
        }
        $best = min($best, (hrtime(true) - $start) / $count);
    }

    return $best;
}

$enc = ab_best(static fn () => $type->encodeArray($payload));
$dec = ab_best(static fn () => $type->decodeArray($wire));
printf("%.1f %.1f\n", $enc, $dec);

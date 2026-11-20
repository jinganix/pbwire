<?php

declare(strict_types=1);

/*
 * The pbench gate: no arm is timed before it reproduces the official runtime's
 * bytes on all four tiers and reads its decode back field by field. Runs first
 * and stays runnable:
 *
 *   php bindings/php/pbench/gate.php                          # ext-ffi arm
 *   php -d extension=.../pbwire.so bindings/php/pbench/gate.php  # C ext arm
 *
 * Writes 'gate: PASS' and exits 0, or names the failing arm/tier and exits 1.
 */

require __DIR__ . '/payload.php';

$failures = 0;

function gate_check(bool $ok, string $what): void
{
    global $failures;

    if ($ok) {
        return;
    }

    echo "FAIL $what\n";
    $failures++;
}

$arms = [
    'tree' => ['file' => 'a-tree.php', 'need' => ['tree_prepare', 'tree_encode', 'tree_decode']],
    'shim' => ['file' => 'b-shim.php', 'need' => ['shim_encode', 'shim_decode', 'shim_impl']],
];
foreach ($arms as $name => $arm) {
    if (!is_file(__DIR__ . '/' . $arm['file'])) {
        gate_check(false, "$name arm missing: {$arm['file']} (" . implode(', ', $arm['need']) . ')');
        continue;
    }
    require __DIR__ . '/' . $arm['file'];
    foreach ($arm['need'] as $fn) {
        gate_check(function_exists($fn), "$name arm missing function: $fn");
    }
}
if ($failures > 0) {
    echo "$failures gate check(s) failed\n";
    exit(1);
}

$type = pbwire_pbench_type();
$shim = shim_impl();

foreach (PB_TIERS as $tier) {
    $label = $tier['label'];
    $repeats = $tier['repeats'];
    $ref = pbwire_pbench_official_encode($repeats);

    if (strlen($ref) !== $tier['bytes']) {
        gate_check(false, "$label: official wrote " . strlen($ref) . " bytes, expected {$tier['bytes']}");
        continue;
    }

    $payload = pbwire_pbench_payload($repeats);

    $tree = tree_prepare($ref);
    gate_check(tree_encode($tree) === $ref, "$label: tree bytes differ from the official runtime");
    $treeBack = tree_decode($ref);
    gate_check(tree_encode($treeBack) === $ref, "$label: tree re-encode differs");

    gate_check(shim_encode($payload) === $ref, "$label: shim bytes differ from the official runtime");
    $back = shim_decode($ref);
    gate_check(pbwire_pbench_reads_ok($back, $repeats), "$label: shim decode differs");
    gate_check(shim_encode($back) === $ref, "$label: shim re-encode differs");
    gate_check($back === pbwire_pbench_official_materialize($ref),
        "$label: shim decode differs from the official materialization");

    echo "OK   $label: " . strlen($ref) . " bytes, tree/$shim equal, x$repeats\n";
}

if ($failures === 0) {
    echo "gate: PASS\n";
} else {
    echo "$failures gate check(s) failed\n";
    exit(1);
}

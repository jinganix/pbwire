<?php

declare(strict_types=1);

/*
 * PHP binding benchmark against the official google/protobuf PHP runtime and
 * generated code on the same matrix3.proto and the four standard tiers from
 * BENCHMARKS.md.
 *
 * The same payload as the C/Go/Java/Rust rows: every scalar kind, the nested
 * message, the two proto3 optionals, and the repeated fields scaled x0 / x1 /
 * x100 / x3000, which lands the tiers on 136 / 174 / 2469 / 74913 bytes. Both
 * sides must produce identical bytes before anything is timed, and both
 * allocate per call (pbwire fresh-Message build/encode and decode/read-back/
 * close against serializeToString()/mergeFromString()), so the comparison
 * is like for like. The prebuilt-handle re-encode is kept as the `proxy`
 * row: same-run timing only, never the headline.
 *
 * The official runtime comes from the cached protobuf checkout (its php/src
 * tree), loaded through a plain SPL autoloader: no composer needed. When the
 * protobuf C extension is loaded it transparently accelerates the same API,
 * so the harness labels its rows `protobuf-php-c`; without it they are the
 * pure-PHP `protobuf-php` rows. bench.sh runs both when the extension is
 * present (the pure run via `php -n`).
 *
 *   ./bench.sh          generate, build, run
 */

require_once __DIR__ . '/../runtime/pbwire.php';
require_once __DIR__ . '/../pbench/payload.php';
require_once __DIR__ . '/../pbench/a-tree.php';
require_once __DIR__ . '/../pbench/b-shim.php';

use Pbwire\Schema;

$gen = __DIR__ . '/gen';
$src = getenv('PBWIRE_PROTOBUF_SRC') ?: dirname(__DIR__, 3) . '/.tmp/protobuf-v36.2/php/src';

spl_autoload_register(static function (string $class) use ($gen, $src): void {
    foreach ([$gen, $src] as $dir) {
        $file = $dir . '/' . str_replace('\\', '/', $class) . '.php';

        if (is_file($file)) {
            require $file;

            return;
        }
    }
});

const CASES = ['small', 'medium', 'large', 'xlarge'];
const REPEATS = [0, 1, 100, 3000];
const WANT = [136, 174, 2469, 74913];
// Floor iterations per tier; doubled until one pass takes 0.25 s (shared
// standard, see BENCHMARKS.md). Floors stay below the 50k/50k/5k/500 default
// because the pure-PHP ops are millisecond-scale: 200 iterations already take
// seconds there, which satisfies the time leg of the standard.
const ITERATIONS = [20000, 20000, 2000, 200];
const MIN_PASS_SECONDS = 0.25;
const PASSES = 3;

$desc = $argv[1] ?? "$gen/matrix3.desc";
// The C extension (when loaded) backs the same generated classes, so one
// label covers whichever official implementation is actually timed.
$officialImpl = extension_loaded('protobuf') ? 'protobuf-php-c' : 'protobuf-php';
// Rows are named by the arm they time: the per-field full task (fresh Message
// per iteration, one set per field, encode, read back per field, close), the
// prebuilt proxy (a held Message handle, re-encode out), the native-array
// entry (one C extension call) or the same API on ext-ffi.
//
// Headline encode rows are fresh-task: each timed iteration builds a fresh
// native array (pbwire) / fresh generated message (official) inside the
// timer, then encodes. Decode rows are full-materialization on both sides
// (decodeArray vs mergeFromString into a fresh message).
$treeImpl = 'pbwire-php-tree';
$proxyImpl = 'pbwire-php-proxy';
$shimImpl = extension_loaded('pbwire') ? 'pbwire-php-shim' : 'pbwire-php-shim-ffi';
echo 'impl: ' . (extension_loaded('pbwire') ? 'C extension ' . phpversion('pbwire') : 'ext-ffi') . "\n";
echo 'official: ' . ($officialImpl === 'protobuf-php-c' ? 'C extension ' . phpversion('protobuf') : 'pure PHP') . "\n\n";
$schema = Schema::fromFile($desc);
$ty = $schema->message('structtest.Scalars3');
$innerTy = $schema->message('structtest.Inner3');
$failures = 0;
$sink = 0;

for ($tier = 0; $tier < count(CASES); $tier++) {
    $label = CASES[$tier];
    $n = REPEATS[$tier];
    $iterations = ITERATIONS[$tier];

    $reference = fillOfficial($n)->serializeToString();

    if (strlen($reference) !== WANT[$tier]) {
        fail($label, 'official encoder wrote ' . strlen($reference) . ' bytes, expected ' . WANT[$tier]);
        continue;
    }

    // The proxy task: a prebuilt Message handle, re-encoded out. Same-run
    // timing only, never the headline.
    $message = fillPbwire($ty, $innerTy, $n);
    $got = $message->encode();

    if ($got !== $reference) {
        fail($label, "$proxyImpl wrote " . strlen($got) . ' bytes, protobuf-php ' . strlen($reference));
        continue;
    }

    // Proxy decode round-trips byte-exactly with no read-back.
    $back = $ty->decode($reference);
    $backOk = $back->encode() === $reference
        && $back->getString('str') === 'hello'
        && $back->getMessage('inner')?->getString('name') === 'kid'
        && $back->count('nums') === $n
        && ($n === 0 || $back->at('nums', $n - 1)?->asInt() === $n - 4);
    $back->close();
    if (!$backOk) {
        fail($label, "$proxyImpl decode differs");
        continue;
    }

    // The tree task: a native array in, fresh Message per call, one set per
    // field, encode, close; wire in, one get per field back out, close.
    $arrayPayload = pbwire_pbench_payload($n);
    $fullWire = tree_encode_full($ty, $innerTy, $arrayPayload);
    if ($fullWire !== $reference) {
        fail($label, "$treeImpl wrote " . strlen($fullWire) . ' bytes, protobuf-php ' . strlen($reference));
        continue;
    }
    $fullBack = tree_decode_full($ty, $reference);
    if (!pbwire_pbench_reads_ok($fullBack, $n)
        || $ty->encodeArray($fullBack) !== $reference
        || $fullBack !== pbwire_pbench_official_materialize($reference)) {
        fail($label, "$treeImpl decode differs");
        continue;
    }

    // The native-object arm: a PHP array in, one entry call; wire in, the
    // fully materialized array out. The same gate as the tree arm.
    $arrayWire = $ty->encodeArray($arrayPayload);
    if ($arrayWire !== $reference) {
        fail($label, "$shimImpl wrote " . strlen($arrayWire) . ' bytes, protobuf-php ' . strlen($reference));
        continue;
    }
    $arrayBack = $ty->decodeArray($reference);
    if (!pbwire_pbench_reads_ok($arrayBack, $n) || $ty->encodeArray($arrayBack) !== $reference) {
        fail($label, "$shimImpl decode differs");
        continue;
    }

    echo "OK   $label encoding parity, x$n (" . strlen($reference) . " bytes)\n";

    $officialClass = \Structtest\Scalars3::class;
    // Tier line shows the reference op's count; slower ops keep the floor.
    $count = calibrate(static fn () => $message->encode(), $iterations);
    $proxyEncode = measure($iterations, static fn () => $message->encode());
    $proxyDecode = measure($iterations, static fn () => $ty->decode($reference));
    $treeEncode = measure($iterations, static fn () => tree_encode_full($ty, $innerTy, $arrayPayload));
    $treeDecode = measure($iterations, static function () use ($ty, $reference, &$sink) {
        $out = tree_decode_full($ty, $reference);
        $sink += count($out);

        return $out;
    });
    $shimEncode = measure($iterations, static function () use ($ty, $n) {
        return $ty->encodeArray(pbwire_pbench_payload($n));
    });
    $shimDecode = measure($iterations, static fn () => $ty->decodeArray($reference));
    $officialEncode = measure($iterations, static fn () => fillOfficial($n)->serializeToString());
    $officialDecode = measure($iterations, static function () use ($officialClass, $reference) {
        $parsed = new $officialClass();
        $parsed->mergeFromString($reference);

        return $parsed;
    });

    $bytes = strlen($reference);
    printf("%s: %d bytes, %d iterations (repeat field x%d)\n", $label, $bytes, $count, $n);
    printf("  %-12s %-7s %12s %10s\n", 'impl', 'op', 'ns/op', 'MB/s');
    row($treeImpl, 'encode', $bytes, $treeEncode);
    row($shimImpl, 'encode', $bytes, $shimEncode);
    row($officialImpl, 'encode', $bytes, $officialEncode);
    row($proxyImpl, 'encode', $bytes, $proxyEncode);
    row($treeImpl, 'decode', $bytes, $treeDecode);
    row($shimImpl, 'decode', $bytes, $shimDecode);
    row($officialImpl, 'decode', $bytes, $officialDecode);
    row($proxyImpl, 'decode', $bytes, $proxyDecode);
    echo "\n";
}

echo "sink: $sink\n";

if ($failures !== 0) {
    fwrite(STDERR, "$failures check(s) failed\n");
    exit(1);
}

function fillOfficial(int $n): \Structtest\Scalars3
{
    $m = new \Structtest\Scalars3();
    $m->setI32(-2147483648);
    $m->setI64(PHP_INT_MAX);
    $m->setU32(4294967295);
    $m->setU64(-1);
    $m->setS32(-2147483648);
    $m->setS64(PHP_INT_MIN);
    $m->setFx32(0xdeadbeef);
    $m->setFx64(72623859790382856);
    $m->setSfx32(-2147483648);
    $m->setSfx64(PHP_INT_MIN);
    $m->setF32(INF);
    $m->setF64(-123456.789);
    $m->setFlag(true);
    $m->setStr('hello');
    $m->setColor(\Structtest\Color3::COLOR3_GREEN);
    $m->setInner((new \Structtest\Inner3())->setName('kid')->setValue(42));
    $m->setOptI32(-7);
    $m->setOptStr('opt');

    $nums = [];
    $tags = [];
    $inners = [];
    $flags = [];

    for ($i = 0; $i < $n; $i++) {
        $nums[] = $i - 3;
        $tags[] = 'tag';
        $inners[] = (new \Structtest\Inner3())->setName('grandkid')->setValue($i + 1);
        $flags[] = $i % 2 === 0;
    }

    $m->setNums($nums);
    $m->setTags($tags);
    $m->setInners($inners);
    $m->setFlags($flags);

    return $m;
}

/** The proxy task's prebuilt handle: built once outside the timer. */
function fillPbwire(Pbwire\Type $ty, Pbwire\Type $innerTy, int $n): Pbwire\Message
{
    $m = $ty->newMessage();
    $name = $innerTy->field('name');
    $value = $innerTy->field('value');

    $m->setInt('i32', -2147483648)
        ->setInt('i64', PHP_INT_MAX)
        ->setInt('u32', 4294967295)
        ->setInt('u64', -1)
        ->setInt('s32', -2147483648)
        ->setInt('s64', PHP_INT_MIN)
        ->setInt('fx32', 0xdeadbeef)
        ->setInt('fx64', 72623859790382856)
        ->setInt('sfx32', -2147483648)
        ->setInt('sfx64', PHP_INT_MIN)
        ->setFloat('f32', INF)
        ->setDouble('f64', -123456.789)
        ->setBool('flag', true)
        ->setString('str', 'hello')
        ->setInt('color', 2)
        ->setInt('opt_i32', -7)
        ->setString('opt_str', 'opt');

    $m->setMessage('inner')->setString($name, 'kid')->setInt($value, 42);

    for ($i = 0; $i < $n; $i++) {
        $m->setInt('nums', $i - 3)
            ->setString('tags', 'tag')
            ->setBool('flags', $i % 2 === 0);
        $m->setMessage('inners')->setString($name, 'grandkid')->setInt($value, $i + 1);
    }

    return $m;
}

/** Warm up, then keep the best of PASSES runs. */
function calibrate(callable $op, int $floor): int
{
    $count = max(1, $floor);

    for (;;) {
        $start = hrtime(true);

        for ($i = 0; $i < $count; $i++) {
            $op();
        }

        if ((hrtime(true) - $start) / 1e9 >= MIN_PASS_SECONDS || $count >= 100000000) {
            return $count;
        }

        $count *= 2;
    }
}

// Every op calibrates independently from the floor (a slow op already exceeds
// the minimum at the floor and keeps it), then warms up and keeps the best
// of PASSES timed passes.
function measure(int $floor, callable $op): float
{
    $count = calibrate($op, $floor);
    run($count, $op);
    $best = INF;

    for ($pass = 0; $pass < PASSES; $pass++) {
        $best = min($best, run($count, $op));
    }

    return $best;
}

function run(int $iterations, callable $op): float
{
    $start = hrtime(true);

    for ($i = 0; $i < $iterations; $i++) {
        $op();
    }

    return (hrtime(true) - $start) / $iterations;
}

function row(string $impl, string $op, int $bytes, float $ns): void
{
    printf("  %-12s %-7s %12.1f %10.1f\n", $impl, $op, $ns, $bytes / $ns * 1000.0);
}

function fail(string $label, string $what): void
{
    global $failures;

    echo "FAIL $label: $what\n";
    $failures++;
}

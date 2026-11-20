<?php

declare(strict_types=1);

/*
 * The one payload every PHP pbench arm times, and the official-runtime oracle
 * it has to match: structtest.Scalars3 from bindings/c/tests/data/matrix3.proto
 * scaled over the four standard tiers.
 *
 * The native object is a PHP array in the generated model's shape: field name
 * => value, a nested array for `inner`, lists for the repeated fields. PHP int
 * is platform width, so a uint64 past PHP_INT_MAX arrives as its decimal string
 * (and only that form is accepted); it decodes back as the same string.
 *
 * The oracle is the official runtime's generated Structtest\Scalars3. The
 * committed protoc PHP output under tests/gen_oracle is the class source, so the
 * gate needs no protoc run; the official runtime itself comes from the cached
 * protobuf checkout.
 *
 *   require __DIR__ . '/payload.php';
 */

require __DIR__ . '/../runtime/pbwire.php';

use Pbwire\Schema;
use Pbwire\Type;

$pbwire_pbench_gen = __DIR__ . '/../tests/gen_oracle';
$pbwire_pbench_src = getenv('PBWIRE_PROTOBUF_SRC')
    ?: dirname(__DIR__, 3) . '/.tmp/protobuf-v36.2/php/src';

spl_autoload_register(static function (string $class) use ($pbwire_pbench_gen, $pbwire_pbench_src): void {
    foreach ([$pbwire_pbench_gen, $pbwire_pbench_src] as $dir) {
        $file = $dir . '/' . str_replace('\\', '/', $class) . '.php';

        if (is_file($file)) {
            require $file;

            return;
        }
    }
});

const PB_TIERS = [
    ['label' => 'small', 'repeats' => 0, 'bytes' => 136],
    ['label' => 'medium', 'repeats' => 1, 'bytes' => 174],
    ['label' => 'large', 'repeats' => 100, 'bytes' => 2469],
    ['label' => 'xlarge', 'repeats' => 3000, 'bytes' => 74913],
];

const PB_MSG = 'structtest.Scalars3';
const PB_U64_MAX = '18446744073709551615';

function pbwire_pbench_type(): Type
{
    static $type = null;

    if ($type === null) {
        $desc = dirname(__DIR__, 2) . '/c/tests/data/matrix3.desc';
        $type = Schema::fromFile($desc)->message(PB_MSG);
    }

    return $type;
}

/** The payload for `repeats` scaled elements, in the generated model's shape. */
function pbwire_pbench_payload(int $repeats): array
{
    $nums = [];
    $tags = [];
    $inners = [];
    $flags = [];

    for ($i = 0; $i < $repeats; $i++) {
        $nums[] = $i - 3;
        $tags[] = 'tag';
        $inners[] = ['name' => 'grandkid', 'value' => $i + 1];
        $flags[] = $i % 2 === 0;
    }

    return [
        'i32' => -2147483648,
        'i64' => PHP_INT_MAX,
        'u32' => 4294967295,
        'u64' => PB_U64_MAX,
        's32' => -2147483648,
        's64' => PHP_INT_MIN,
        'fx32' => 0xdeadbeef,
        'fx64' => 72623859790382856,
        'sfx32' => -2147483648,
        'sfx64' => PHP_INT_MIN,
        'f32' => INF,
        'f64' => -123456.789,
        'flag' => true,
        'str' => 'hello',
        'color' => 2,
        'inner' => ['name' => 'kid', 'value' => 42],
        'nums' => $nums,
        'tags' => $tags,
        'inners' => $inners,
        'opt_i32' => -7,
        'opt_str' => 'opt',
        'flags' => $flags,
    ];
}

/** Every field the benchmark reads, so a decode is gated field by field. */
function pbwire_pbench_reads_ok(array $back, int $repeats): bool
{
    return ($back['i32'] ?? null) === -2147483648
        && ($back['i64'] ?? null) === PHP_INT_MAX
        && ($back['u32'] ?? null) === 4294967295
        && ($back['u64'] ?? null) === PB_U64_MAX
        && ($back['s32'] ?? null) === -2147483648
        && ($back['s64'] ?? null) === PHP_INT_MIN
        && ($back['fx32'] ?? null) === 0xdeadbeef
        && ($back['fx64'] ?? null) === 72623859790382856
        && ($back['sfx32'] ?? null) === -2147483648
        && ($back['sfx64'] ?? null) === PHP_INT_MIN
        && is_float($back['f32'] ?? null) && is_infinite($back['f32'])
        && abs(($back['f64'] ?? 0.0) + 123456.789) < 1e-9
        && ($back['flag'] ?? null) === true
        && ($back['str'] ?? null) === 'hello'
        && ($back['color'] ?? null) === 2
        && ($back['inner']['name'] ?? null) === 'kid'
        && ($back['inner']['value'] ?? null) === 42
        && ($back['opt_i32'] ?? null) === -7
        && ($back['opt_str'] ?? null) === 'opt'
        && count($back['nums'] ?? []) === $repeats
        && count($back['tags'] ?? []) === $repeats
        && count($back['inners'] ?? []) === $repeats
        && count($back['flags'] ?? []) === $repeats
        && ($repeats === 0
            || (($back['nums'][$repeats - 1] ?? null) === $repeats - 4
                && ($back['tags'][$repeats - 1] ?? null) === 'tag'
                && ($back['inners'][$repeats - 1]['value'] ?? null) === $repeats
                && ($back['flags'][$repeats - 1] ?? null) === (($repeats - 1) % 2 === 0)));
}

function pbwire_pbench_official(int $repeats): \Structtest\Scalars3
{
    $m = new \Structtest\Scalars3();
    $m->setI32(-2147483648);
    $m->setI64(PHP_INT_MAX);
    $m->setU32(4294967295);
    $m->setU64(-1); /* u64::MAX bit pattern */
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

    for ($i = 0; $i < $repeats; $i++) {
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

function pbwire_pbench_official_encode(int $repeats): string
{
    return pbwire_pbench_official($repeats)->serializeToString();
}

/** The official decode materialized into the same array shape as the arms. */
function pbwire_pbench_official_materialize(string $wire): array
{
    $m = new \Structtest\Scalars3();
    $m->mergeFromString($wire);
    $list = static function (iterable $items, callable $map): array {
        $out = [];

        foreach ($items as $item) {
            $out[] = $map($item);
        }

        return $out;
    };
    $inner = static fn (\Structtest\Inner3 $x): array => ['name' => $x->getName(), 'value' => $x->getValue()];

    return [
        'i32' => $m->getI32(),
        'i64' => $m->getI64(),
        'u32' => $m->getU32() & 0xFFFFFFFF,
        'u64' => sprintf('%u', $m->getU64()),
        's32' => $m->getS32(),
        's64' => $m->getS64(),
        'fx32' => $m->getFx32() & 0xFFFFFFFF,
        'fx64' => $m->getFx64(),
        'sfx32' => $m->getSfx32(),
        'sfx64' => $m->getSfx64(),
        'f32' => $m->getF32(),
        'f64' => $m->getF64(),
        'flag' => $m->getFlag(),
        'str' => $m->getStr(),
        'color' => $m->getColor(),
        'inner' => $inner($m->getInner()),
        'nums' => $list($m->getNums(), static fn ($x) => $x),
        'tags' => $list($m->getTags(), static fn ($x) => $x),
        'inners' => $list($m->getInners(), $inner),
        'opt_i32' => $m->getOptI32(),
        'opt_str' => $m->getOptStr(),
        'flags' => $list($m->getFlags(), static fn ($x) => $x),
    ];
}

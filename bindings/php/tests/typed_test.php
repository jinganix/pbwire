<?php

declare(strict_types=1);

/**
 * Typed-model smoke check for the PHP binding: build matrix3.proto's Scalars3
 * through the generated classes (official-runtime shape, embedded schema),
 * encode, require the protoc reference bytes (matrix3.bin), then decode those
 * bytes back and check every field. No oracle needed; see compat_test.php for
 * the full T1–T6 differential against the pinned runtime.
 *
 *   php bindings/php/tests/typed_test.php
 */

require_once __DIR__ . '/../runtime/pbwire.php';
require_once __DIR__ . '/../runtime/typed.php';
require_once __DIR__ . '/gen/matrix3.php';

use Pbwire\Gen\Structtest_Color3;
use Pbwire\Gen\Structtest_Inner3;
use Pbwire\Gen\Structtest_Scalars3;

$reference = (string) file_get_contents(__DIR__ . '/../../c/tests/data/matrix3.bin');
$checks = 0;

function check(bool $ok, string $what): void
{
    global $checks;

    if (!$ok) {
        fwrite(STDERR, "FAIL $what\n");
        exit(1);
    }

    $checks++;
}

function build(): Structtest_Scalars3
{
    return (new Structtest_Scalars3())
        ->setI32(-2147483648)
        ->setI64(9223372036854775807)
        ->setU32(4294967295)
        ->setU64(-1) // 18446744073709551615 as raw 64-bit bits
        ->setS32(-2147483648)
        ->setS64(PHP_INT_MIN)
        ->setFx32(3735928559)
        ->setFx64(72623859790382856)
        ->setSfx32(-2147483648)
        ->setSfx64(PHP_INT_MIN)
        ->setF32(INF)
        ->setF64(-123456.789)
        ->setFlag(true)
        ->setStr('hello')
        ->setColor(Structtest_Color3::COLOR3_GREEN)
        ->setInner((new Structtest_Inner3())->setName('kid')->setValue(42))
        ->setNums([1, -5, 300])
        ->setTags(['a', 'bb'])
        ->setInners([
            (new Structtest_Inner3())->setName('x')->setValue(1),
            (new Structtest_Inner3())->setName('y')->setValue(-2),
        ])
        ->setOptI32(-7)
        ->setOptStr('opt')
        ->setFlags([true, false, true]);
}

function verify(Structtest_Scalars3 $m): void
{
    check($m->getI32() === -2147483648, 'i32');
    check($m->getI64() === 9223372036854775807, 'i64');
    check($m->getU32() === 4294967295, 'u32');
    check($m->getU64() === -1, 'u64');
    check($m->getS32() === -2147483648, 's32');
    check($m->getS64() === PHP_INT_MIN, 's64');
    check($m->getFx32() === 3735928559, 'fx32');
    check($m->getFx64() === 72623859790382856, 'fx64');
    check($m->getSfx32() === -2147483648, 'sfx32');
    check($m->getSfx64() === PHP_INT_MIN, 'sfx64');
    check($m->getF32() === INF, 'f32');
    check($m->getF64() === -123456.789, 'f64');
    check($m->getFlag() === true, 'flag');
    check($m->getStr() === 'hello', 'str');
    check($m->getColor() === Structtest_Color3::COLOR3_GREEN, 'color');
    check($m->hasInner() && $m->getInner()->getName() === 'kid' && $m->getInner()->getValue() === 42, 'inner');
    check(iterator_to_array($m->getNums()) === [1, -5, 300], 'nums');
    check(iterator_to_array($m->getTags()) === ['a', 'bb'], 'tags');
    check(count($m->getInners()) === 2 && $m->getInners()[1]->getValue() === -2, 'inners');
    check($m->hasOptI32() && $m->getOptI32() === -7, 'opt_i32');
    check($m->hasOptStr() && $m->getOptStr() === 'opt', 'opt_str');
    check(iterator_to_array($m->getFlags()) === [true, false, true], 'flags');
}

check(build()->serializeToString() === $reference, 'typed encode equals the protoc reference bytes');
verify(Structtest_Scalars3::fromString($reference));
check((new Structtest_Scalars3())->serializeToString() === '', 'empty typed message is empty');

$zero = new Structtest_Scalars3();
$zero->setOptI32(0);
$zeroBack = Structtest_Scalars3::fromString($zero->serializeToString());
check($zeroBack->hasOptI32() && $zeroBack->getOptI32() === 0, 'explicit zero presence round-trips');

$nested = (new Structtest_Scalars3())->setInner((new Structtest_Inner3())->setName('kid'));
check(Structtest_Scalars3::fromString($nested->serializeToString())->getInner()->getValue() === 0, 'nested typed message');

printf("typed_test: %d checks passed\n", $checks);

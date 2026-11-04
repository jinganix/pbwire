<?php

declare(strict_types=1);

/**
 * Typed-conformance check for the PHP binding: build matrix3.proto's Scalars3
 * through the generated classes, encode, require the protoc reference bytes
 * (matrix3.bin), then decode those bytes back and check every field.
 *
 *   php bindings/php/tests/typed_test.php
 */

require_once __DIR__ . '/../runtime/pbwire.php';
require_once __DIR__ . '/gen/matrix3.php';

use Pbwire\Gen\Structtest_Color3;
use Pbwire\Gen\Structtest_Inner3;
use Pbwire\Gen\Structtest_Scalars3;
use Pbwire\Schema;

$data = __DIR__ . '/../../c/tests/data';
$reference = file_get_contents($data . '/matrix3.bin');
$schema = Schema::fromFile($data . '/matrix3.desc');
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
    $kid = new Structtest_Inner3();
    $kid->name = 'kid';
    $kid->value = 42;

    $x = new Structtest_Inner3();
    $x->name = 'x';
    $x->value = 1;
    $y = new Structtest_Inner3();
    $y->name = 'y';
    $y->value = -2;

    $m = new Structtest_Scalars3();
    $m->i32 = -2147483648;
    $m->i64 = 9223372036854775807;
    $m->u32 = 4294967295;
    $m->u64 = -1; // 18446744073709551615 as raw 64-bit bits
    $m->s32 = -2147483648;
    $m->s64 = PHP_INT_MIN;
    $m->fx32 = 3735928559;
    $m->fx64 = 72623859790382856;
    $m->sfx32 = -2147483648;
    $m->sfx64 = PHP_INT_MIN;
    $m->f32 = INF;
    $m->f64 = -123456.789;
    $m->flag = true;
    $m->str = 'hello';
    $m->color = Structtest_Color3::COLOR3_GREEN;
    $m->inner = $kid;
    $m->nums = [1, -5, 300];
    $m->tags = ['a', 'bb'];
    $m->inners = [$x, $y];
    $m->hasOptI32 = true;
    $m->optI32 = -7;
    $m->hasOptStr = true;
    $m->optStr = 'opt';
    $m->flags = [true, false, true];
    return $m;
}

function verify(Structtest_Scalars3 $m): void
{
    check($m->i32 === -2147483648, 'i32');
    check($m->i64 === 9223372036854775807, 'i64');
    check($m->u32 === 4294967295, 'u32');
    check($m->u64 === -1, 'u64');
    check($m->s32 === -2147483648, 's32');
    check($m->s64 === PHP_INT_MIN, 's64');
    check($m->fx32 === 3735928559, 'fx32');
    check($m->fx64 === 72623859790382856, 'fx64');
    check($m->sfx32 === -2147483648, 'sfx32');
    check($m->sfx64 === PHP_INT_MIN, 'sfx64');
    check($m->f32 === INF, 'f32');
    check($m->f64 === -123456.789, 'f64');
    check($m->flag === true, 'flag');
    check($m->str === 'hello', 'str');
    check($m->color === Structtest_Color3::COLOR3_GREEN, 'color');
    check($m->inner !== null && $m->inner->name === 'kid' && $m->inner->value === 42, 'inner');
    check($m->nums === [1, -5, 300], 'nums');
    check($m->tags === ['a', 'bb'], 'tags');
    check(count($m->inners) === 2 && $m->inners[1]->value === -2, 'inners');
    check($m->hasOptI32 && $m->optI32 === -7, 'opt_i32');
    check($m->hasOptStr && $m->optStr === 'opt', 'opt_str');
    check($m->flags === [true, false, true], 'flags');
}

$wire = build()->encode($schema);
check($wire === $reference, 'typed encode equals the protoc reference bytes');
verify(Structtest_Scalars3::decode($schema, $reference));
check((new Structtest_Scalars3())->encode($schema) === '', 'empty typed message is empty');

$zero = new Structtest_Scalars3();
$zero->hasOptI32 = true;
$zero->optI32 = 0;
$zeroBack = Structtest_Scalars3::decode($schema, $zero->encode($schema));
check($zeroBack->hasOptI32 && $zeroBack->optI32 === 0, 'explicit zero presence round-trips');

$kid = new Structtest_Inner3();
$kid->name = 'kid';
$nested = new Structtest_Scalars3();
$nested->inner = $kid;
check(Structtest_Scalars3::decode($schema, $nested->encode($schema))->inner->value === 0, 'nested typed message');

$schema->close();
printf("typed_test: %d checks passed\n", $checks);

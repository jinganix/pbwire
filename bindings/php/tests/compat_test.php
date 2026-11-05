<?php

declare(strict_types=1);

/**
 * Typed-compat check for the PHP binding (TESTSPEC T1–T6, vectors.json as
 * the single source of truth):
 *
 *   T1  generated serializeToString == pinned protoc reference bytes
 *   T2  fromString of the reference reads every fixture value back
 *   T3  new encodes empty; explicit optional zero round-trips
 *   T4  differential vs the pinned official runtime (php/src at v36.2 plus
 *       committed protoc --php_out oracle): same bytes, same fields,
 *       cross-decode agrees
 *   T5  one business snippet runs against both libraries' messages with
 *       identical observable results (import swap only)
 *   T6  errors are loud, not crashes: bad wire, wrong types, out-of-range
 *       integers, unknown oneof; implicit presence has no hasX
 *
 *   php bindings/php/tests/compat_test.php
 */

use Pbwire\Gen\Spectest_Oneof1;
use Pbwire\Gen\Spectest_Oneof1_Inner;
use Pbwire\Gen\Structtest_Color3;
use Pbwire\Gen\Structtest_Inner3;
use Pbwire\Gen\Structtest_Scalars3;

// The official runtime ships in the cached protobuf checkout (no composer):
// `make conformance-setup` clones it, or set PBWIRE_PROTOBUF_SRC.
$protoSrc = getenv('PBWIRE_PROTOBUF_SRC');
if ($protoSrc === false || $protoSrc === '') {
    $protoSrc = __DIR__ . '/../../../.tmp/protobuf-v36.2/php/src';
}
spl_autoload_register(static function (string $class) use ($protoSrc): void {
    $path = $protoSrc . '/' . str_replace('\\', '/', $class) . '.php';
    if (is_file($path)) {
        require $path;
    }
});

require_once __DIR__ . '/../runtime/pbwire.php';
require_once __DIR__ . '/../runtime/generated.php';
require_once __DIR__ . '/gen/matrix3.php';
require_once __DIR__ . '/gen/oneof1.php';

$oracleDir = __DIR__ . '/gen_oracle';
foreach (['Structtest/Color3.php', 'Structtest/Inner3.php', 'Structtest/Scalars3.php',
    'Spectest/Oneof1.php', 'Spectest/Oneof1/Inner.php',
    'GPBMetadata/Matrix3.php', 'GPBMetadata/Oneof1.php'] as $file) {
    require_once $oracleDir . '/' . $file;
}

const ORACLE_PIN = 'v36.2';

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

function throws(callable $fn): bool
{
    try {
        $fn();
    } catch (\Throwable) {
        return true;
    }

    return false;
}

// The pin is the protobuf checkout tag plus the protoc that stamped the
// oracle; either drifting fails loudly instead of silently.
$checkout = dirname((string) $protoSrc, 2); // .../protobuf-v36.2 from .../php/src
$tag = trim((string) shell_exec('git -C ' . escapeshellarg((string) $checkout) . ' describe --tags 2>/dev/null'));
check($tag === ORACLE_PIN, "oracle checkout tag is " . ORACLE_PIN . " (got $tag)");
$protoc = trim((string) shell_exec('protoc --version 2>/dev/null'));
check(str_contains($protoc, '36.2'), "protoc is 36.2 (got $protoc)");

$testdata = __DIR__ . '/../../../codegen/testdata';
$vectors = json_decode((string) file_get_contents($testdata . '/vectors.json'), true);
$reference = (string) file_get_contents(dirname(__DIR__, 2) . '/c/tests/data/matrix3.bin');
$referenceOneof = (string) file_get_contents($testdata . '/oneof1.bin');

function buildScalars(): Structtest_Scalars3
{
    global $vectors;

    $v = $vectors['values'];

    return new Structtest_Scalars3([
        'i32' => $v['i32'],
        'i64' => (string) $v['i64'],
        'u32' => $v['u32'],
        'u64' => (string) $v['u64'], // decimal text wraps to raw bits, like the core
        's32' => $v['s32'],
        's64' => (string) $v['s64'],
        'fx32' => $v['fx32'],
        'fx64' => (string) $v['fx64'],
        'sfx32' => $v['sfx32'],
        'sfx64' => (string) $v['sfx64'],
        'f32' => INF,
        'f64' => $v['f64'],
        'flag' => $v['flag'],
        'str' => $v['str'],
        'color' => Structtest_Color3::value($v['color']),
        'inner' => ['name' => $v['inner']['name'], 'value' => $v['inner']['value']],
        'nums' => $v['nums'],
        'tags' => $v['tags'],
        'inners' => array_map(
            static fn(array $x): array => ['name' => $x['name'], 'value' => $x['value']],
            $v['inners'],
        ),
        'opt_i32' => $v['opt_i32'],
        'opt_str' => $v['opt_str'],
        'flags' => $v['flags'],
    ]);
}

function buildOracle(): Structtest\Scalars3
{
    global $vectors;

    $v = $vectors['values'];
    $m = new Structtest\Scalars3();
    $m->setI32($v['i32']);
    $m->setI64((int) $v['i64']);
    $m->setU32($v['u32']);
    $m->setU64(-1); // bits of 18446744073709551615, like the facade
    $m->setS32($v['s32']);
    $m->setS64((int) $v['s64']);
    $m->setFx32($v['fx32']);
    $m->setFx64((int) $v['fx64']);
    $m->setSfx32($v['sfx32']);
    $m->setSfx64((int) $v['sfx64']);
    $m->setF32(INF);
    $m->setF64($v['f64']);
    $m->setFlag($v['flag']);
    $m->setStr($v['str']);
    $m->setColor(Structtest\Color3::value($v['color']));
    $m->setInner((new Structtest\Inner3())->setName($v['inner']['name'])->setValue($v['inner']['value']));
    $m->setNums($v['nums']);
    $m->setTags($v['tags']);
    foreach ($v['inners'] as $x) {
        $m->getInners()[] = (new Structtest\Inner3())->setName($x['name'])->setValue($x['value']);
    }
    $m->setOptI32($v['opt_i32']);
    $m->setOptStr($v['opt_str']);
    $m->setFlags($v['flags']);

    return $m;
}

// T1: encode equals the protoc reference bytes.
check(buildScalars()->serializeToString() === $reference, 'T1 facade != reference');

$one = new Spectest_Oneof1(['name' => 'a']);
$one->setText('hi');
check($one->serializeToString() === $referenceOneof, 'T1 oneof != reference');

// T2: decode reads every fixture value back.
$back = Structtest_Scalars3::fromString($reference);
$v = $vectors['values'];
check($back->getI32() === $v['i32'], 'T2 i32');
check($back->getI64() === (int) $v['i64'], 'T2 i64');
check($back->getU32() === $v['u32'], 'T2 u32');
check($back->getU64() === -1, 'T2 u64 keeps its bits');
check($back->getS32() === $v['s32'] && $back->getS64() === (int) $v['s64'], 'T2 s32/s64');
check($back->getFx32() === $v['fx32'] && $back->getFx64() === (int) $v['fx64'], 'T2 fx32/fx64');
check($back->getSfx32() === $v['sfx32'] && $back->getSfx64() === (int) $v['sfx64'], 'T2 sfx32/sfx64');
check(is_infinite($back->getF32()) && $back->getF32() > 0, 'T2 f32');
check($back->getF64() === $v['f64'], 'T2 f64');
check($back->getFlag() === true && $back->getStr() === 'hello', 'T2 flag/str');
check($back->getColor() === Structtest_Color3::COLOR3_GREEN, 'T2 color');
check($back->getColor() === Structtest\Color3::COLOR3_GREEN, 'T2 color matches oracle numbering');
check($back->hasInner() && $back->getInner()->getName() === 'kid' && $back->getInner()->getValue() === 42, 'T2 inner');
check(iterator_to_array($back->getNums()) === $v['nums'], 'T2 nums');
check(iterator_to_array($back->getTags()) === $v['tags'], 'T2 tags');
check(count($back->getInners()) === 2 && $back->getInners()[0]->getName() === 'x' && $back->getInners()[1]->getValue() === -2, 'T2 inners');
check($back->hasOptI32() && $back->getOptI32() === $v['opt_i32'], 'T2 opt_i32 presence');
check($back->hasOptStr() && $back->getOptStr() === $v['opt_str'], 'T2 opt_str presence');
check(iterator_to_array($back->getFlags()) === $v['flags'], 'T2 flags');
check(throws(static fn() => $back->hasI32()), 'T2 implicit presence has no hasX');
check(Structtest_Color3::name(2) === 'COLOR3_GREEN', 'T2 enum name');
check(throws(static fn() => Structtest_Color3::name(99)), 'T2 unknown enum number is loud');

$ob = Spectest_Oneof1::fromString($referenceOneof);
check($ob->getName() === 'a' && $ob->getText() === 'hi' && $ob->getKind() === 'text', 'T2 oneof decode');
check($ob->whichOneof('kind') === 'text', 'T2 whichOneof');
$nested = new Spectest_Oneof1(['name' => 'n']);
$nested->setInner((new Spectest_Oneof1_Inner())->setV(7));
$nb = Spectest_Oneof1::fromString($nested->serializeToString());
check($nb->getKind() === 'inner' && $nb->getInner()->getV() === 7, 'T2 nested oneof member');

// T3: empty encodes empty; explicit optional zero round-trips.
check((new Structtest_Scalars3())->serializeToString() === '', 'T3 empty encodes empty');
$zero = new Structtest_Scalars3();
$zero->setOptI32(0);
$zeroBack = Structtest_Scalars3::fromString($zero->serializeToString());
check($zeroBack->hasOptI32() && $zeroBack->getOptI32() === 0 && !$zeroBack->hasOptStr(), 'T3 explicit zero keeps presence');

// T4: differential vs the pinned oracle.
check(buildOracle()->serializeToString() === $reference, 'T4 oracle != reference');
$fromOurs = new Structtest\Scalars3();
$fromOurs->mergeFromString(buildScalars()->serializeToString());
$fromTheirs = Structtest_Scalars3::fromString(buildOracle()->serializeToString());
foreach (['I32', 'I64', 'U64', 'S32', 'S64', 'Fx64', 'Sfx32', 'Sfx64'] as $int) {
    $get = 'get' . $int;
    check($fromOurs->$get() === $fromTheirs->$get(), "T4 $int agrees");
}
// The oracle sign-extends uint32/fixed32 (4294967295 reads back as -1);
// compare the low 32 bits instead of the PHP int.
foreach (['U32', 'Fx32'] as $int) {
    $get = 'get' . $int;
    check(($fromOurs->$get() & 0xFFFFFFFF) === ($fromTheirs->$get() & 0xFFFFFFFF), "T4 $int agrees");
}
check($fromOurs->getFlag() === $fromTheirs->getFlag(), 'T4 flag agrees');
check($fromOurs->getStr() === $fromTheirs->getStr(), 'T4 str agrees');
check($fromOurs->getColor() === $fromTheirs->getColor(), 'T4 color agrees');
check($fromOurs->getInner()->getName() === $fromTheirs->getInner()->getName(), 'T4 inner agrees');
check($fromOurs->getOptI32() === $fromTheirs->getOptI32(), 'T4 opt agrees');
check($fromOurs->hasOptI32() === $fromTheirs->hasOptI32(), 'T4 presence agrees');
check(count($fromOurs->getNums()) === count($fromTheirs->getNums()), 'T4 repeated length agrees');
check(count($fromOurs->getInners()) === count($fromTheirs->getInners()), 'T4 message repeated agrees');
$oracleOne = new Spectest\Oneof1();
$oracleOne->setName('a');
$oracleOne->setText('hi');
check($oracleOne->serializeToString() === $referenceOneof, 'T4 oracle oneof != reference');

// T5: one business snippet against both libraries.
$workflow = static function (callable $new, callable $set, callable $enc, callable $dec, callable $get): array {
    $m = $new();
    $set($m);
    $wire = $enc($m);
    $g = $get($dec($wire));

    return [$wire, ...$g];
};

$oracleResult = $workflow(
    static fn(): Structtest\Scalars3 => new Structtest\Scalars3(),
    static function (Structtest\Scalars3 $m): void {
        $m->setStr('kid');
        $m->setI32(-5);
        $m->setNums([1, 2]);
        $m->setInner((new Structtest\Inner3())->setName('x')->setValue(1));
    },
    static fn(Structtest\Scalars3 $m): string => $m->serializeToString(),
    static function (string $wire): Structtest\Scalars3 {
        $m = new Structtest\Scalars3();
        $m->mergeFromString($wire);

        return $m;
    },
    static fn(Structtest\Scalars3 $m): array => [$m->getStr(), $m->getI32(),
        iterator_to_array($m->getNums()), $m->getInner()->getName(), $m->hasOptI32()],
);

$facadeResult = $workflow(
    static fn(): Structtest_Scalars3 => new Structtest_Scalars3(),
    static function (Structtest_Scalars3 $m): void {
        $m->setStr('kid');
        $m->setI32(-5);
        $m->setNums([1, 2]);
        $m->setInner((new Structtest_Inner3())->setName('x')->setValue(1));
    },
    static fn(Structtest_Scalars3 $m): string => $m->serializeToString(),
    static fn(string $wire): Structtest_Scalars3 => Structtest_Scalars3::fromString($wire),
    static fn(Structtest_Scalars3 $m): array => [$m->getStr(), $m->getI32(),
        iterator_to_array($m->getNums()), $m->getInner()->getName(), $m->hasOptI32()],
);

check($oracleResult === $facadeResult, 'T5 mismatch: same steps, different results');

// T6: errors are loud.
check(throws(static fn() => Structtest_Scalars3::fromString("\xff\xff\xff")), 'T6 bad wire fails');
$oracleBad = new Structtest\Scalars3();
check(throws(static function () use ($oracleBad): void { $oracleBad->mergeFromString("\xff\xff\xff"); }), 'T6 oracle bad wire fails');
check(throws(static fn() => (new Structtest_Scalars3())->setI32('nope')), 'T6 wrong field type fails');
check(throws(static fn() => (new Structtest_Scalars3())->setI32(1 << 40)), 'T6 out-of-range integer fails');
check(throws(static fn() => Structtest_Color3::value('NOPE')), 'T6 unknown enum name fails');
check(throws(static fn() => (new Spectest_Oneof1())->whichOneof('nope')), 'T6 unknown oneof fails');
$switch = new Spectest_Oneof1(['name' => 'n']);
$switch->setNum(1);
$switch->setText('hi');
check($switch->getKind() === 'text' && !$switch->hasNum() && $switch->hasText(), 'T6 oneof keeps only the last member');
$cleared = buildScalars();
$cleared->clearOptI32();
check(!$cleared->hasOptI32() && $cleared->getOptI32() === 0, 'T6 clear drops presence');

printf("compat: %d checks passed (oracle %s)\n", $checks, ORACLE_PIN);

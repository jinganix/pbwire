<?php

declare(strict_types=1);

/*
 * Self-check against the pinned protoc bytes: build structtest.Scalars3 from
 * bindings/c/tests/data/matrix3.txt, encode, compare with matrix3.bin (the same
 * oracle the C binding uses), then decode it back and check the values.
 *
 *   php tests/test.php [descriptor-set] [wire-bytes]
 */

require __DIR__ . '/../runtime/pbwire.php';

use Pbwire\Error;
use Pbwire\Schema;

$data = dirname(__DIR__, 2) . '/c/tests/data';
$desc = $argv[1] ?? "$data/matrix3.desc";
$wire = fopen($argv[2] ?? "$data/matrix3.bin", 'rb');

if ($wire === false) {
    fwrite(STDERR, "cannot read the oracle bytes\n");
    exit(2);
}

$oracle = stream_get_contents($wire);
$schema = Schema::fromFile($desc);
$ty = $schema->message('structtest.Scalars3');
$innerTy = $schema->message('structtest.Inner3');
$checks = 0;

function check(bool $ok, string $what): void
{
    global $checks;

    $checks++;

    if (!$ok) {
        fwrite(STDERR, "FAIL $what\n");
        exit(1);
    }
}

$msg = $ty->newMessage();
$msg->setInt('i32', -2147483648)
    ->setInt('i64', PHP_INT_MAX)
    ->setInt('u32', 4294967295)
    ->setInt('u64', -1) /* u64::MAX bits */
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

foreach ([1, -5, 300] as $value) {
    $msg->setInt('nums', $value);
}
foreach (['a', 'bb'] as $tag) {
    $msg->setString('tags', $tag);
}
foreach ([['x', 1], ['y', -2]] as [$name, $value]) {
    $msg->setMessage('inners')->setString($innerTy->field('name'), $name)->setInt('value', $value);
}
foreach ([true, false, true] as $flag) {
    $msg->setBool('flags', $flag);
}
$msg->setMessage('inner')->setString('name', 'kid')->setInt('value', 42);

$encoded = $msg->encode();
check($encoded === $oracle, 'encoded bytes differ from protoc (' . strlen($encoded) . ' vs ' . strlen($oracle) . ')');
echo 'OK   encode matches protoc, ' . strlen($oracle) . " bytes\n";

$decoded = $ty->decode($oracle);
check($decoded->getInt('i32') === -2147483648, 'i32');
check($decoded->getBool('flag') === true, 'flag');
check($decoded->getString('str') === 'hello', 'str');
check(abs($decoded->getDouble('f64') + 123456.789) < 1e-9, 'f64');
check($decoded->getInt('color') === 2, 'color');
check($decoded->getInt('opt_i32') === -7, 'opt_i32');
check($decoded->has('opt_str') && !$ty->newMessage()->has('i32'), 'presence');
check($decoded->count('nums') === 3, 'nums count');
check($decoded->at('nums', 1)?->asInt() === -5, 'nums[1]');
check($decoded->at('tags', 1)?->asString() === 'bb', 'tags[1]');
check($decoded->at('inners', 1)?->asMessage()?->getInt('value') === -2, 'inners[1].value');
check($decoded->at('flags', 1)?->asBool() === false, 'flags[1]');
check($decoded->getMessage('inner')?->getString('name') === 'kid', 'inner.name');
check($decoded->encode() === $oracle, 'decode -> encode is byte-exact');
echo "OK   decode round trip, repeated and nested fields\n";

/* A field the schema does not declare (99, varint 7) survives re-encoding. */
$unknown = "\x98\x06\x07";
$innerOnly = $innerTy->decode($unknown);
check($innerOnly->encode() === $unknown, 'unknown field preserved');

/* Bad input is an error, not a crash. */
try {
    $ty->decode("\x80");
    check(false, 'truncated tag accepted');
} catch (Error) {
    check(true, 'truncated tag');
}

try {
    $schema->message('structtest.Nope');
    check(false, 'unknown message accepted');
} catch (Error) {
    check(true, 'unknown message');
}

try {
    $ty->field('nope');
    check(false, 'unknown field accepted');
} catch (Error) {
    check(true, 'unknown field');
}

echo "OK   $checks checks passed\n";

/* ------------------------------------------------------------------ edge cases */

/*
 * The cases a binding can get wrong even when the core is right: payloads with
 * embedded NULs / invalid UTF-8, non-ASCII text, a negative enum, an empty
 * repeated element, proto3 optional presence with an empty value, and oneof
 * switching. tests/data/edge.bin is protoc --encode.
 */
$root = dirname(__DIR__, 3);
$edgeOracle = file_get_contents("$root/tests/data/edge.bin");
$edgeSchema = Schema::fromFile("$root/tests/data/edge.desc");
$edgeTy = $edgeSchema->message('edge.Edge');
$edgeInner = $edgeSchema->message('edge.Inner');

$edge = $edgeTy->newMessage();
$edge->setBytes('raw', "\x00\x01\xff")
    ->setString('text', 'héllo ☃')
    ->setInt('sign', -3)
    ->setString('maybe', '')
    ->setInt('pick_num', 5) /* oneof: replaced below */
    ->setString('pick_str', 'one');
foreach (['a', '', 'b'] as $tag) {
    $edge->setString('tags', $tag);
}
$edge->setBytes('blobs', "\x00");
$edge->setBytes('blobs', '');
$edge->setMessage('inner')->setString('tag', 'kid')->setInt('n', -1);

check($edge->encode() === $edgeOracle, 'edge payload differs from protoc');
$edgeDecoded = $edgeTy->decode($edgeOracle);
check($edgeDecoded->getBytes('raw') === "\x00\x01\xff", 'bytes with NUL and 0xff');
check($edgeDecoded->getString('text') === 'héllo ☃', 'non-ASCII string');
check($edgeDecoded->getInt('sign') === -3, 'negative enum');
check($edgeDecoded->count('tags') === 3, 'empty repeated element count');
check($edgeDecoded->at('tags', 1)?->asString() === '', 'empty repeated element');
check($edgeDecoded->at('blobs', 0)?->asBytes() === "\x00", 'bytes element with NUL');
check($edgeDecoded->at('blobs', 1)?->asBytes() === '', 'empty bytes element');
check($edgeDecoded->getMessage('inner')?->getInt('n') === -1, 'nested negative');
check($edgeDecoded->has('maybe') && $edgeDecoded->getString('maybe') === '', 'optional present and empty');
check($edgeDecoded->has('pick_str') && !$edgeDecoded->has('pick_num'), 'oneof keeps only the last member');
check($edgeDecoded->encode() === $edgeOracle, 'edge decode -> encode is byte-exact');
echo "OK   edge payload: bytes, UTF-8, negative enum, empty elements, oneof\n";

/* Strings are length-delimited: embedded NUL bytes survive both ways. */
$nul = $edgeTy->newMessage();
$nul->setString('text', "a\x00b");
$nul->setString('tags', "x\x00y");
check($nul->getString('text') === "a\x00b", 'string with NUL');
check($nul->at('tags', 0)?->asString() === "x\x00y", 'repeated string with NUL');
$nulBack = $edgeTy->decode($nul->encode());
check($nulBack->getString('text') === "a\x00b", 'decoded string with NUL');
check($nulBack->at('tags', 0)?->asString() === "x\x00y", 'decoded repeated string with NUL');
check($nulBack->encode() === $nul->encode(), 'NUL payload re-encodes byte-exact');

/* A default message is zero bytes both ways. */
check($edgeTy->newMessage()->encode() === '', 'empty message encodes to zero bytes');
$emptyDecoded = $edgeTy->decode('');
check($emptyDecoded->encode() === '', 'empty wire decodes and re-encodes to zero bytes');
check(!$emptyDecoded->has('text'), 'empty message has no fields');

/* A wrong value kind is refused, not written. */
try {
    $edgeTy->newMessage()->setString('pick_num', 'not a number');
    check(false, 'wrong-typed value accepted');
} catch (Error $e) {
    check(str_contains($e->getMessage(), 'type mismatch'), 'wrong-typed value rejected');
}

try {
    $edgeTy->newMessage()->setMessage('text');
    check(false, 'setMessage on a scalar accepted');
} catch (Error $e) {
    check(str_contains($e->getMessage(), 'not a message field'), 'setMessage on a scalar rejected');
}

/* Lifetime: a Type keeps its Schema alive after Schema::close(), and a
 * nested view refuses to write once its root is closed (the arena is gone
 * with it). */
$closeSchema = Schema::fromFile($desc);
$closeTy = $closeSchema->message('structtest.Inner3');
$closeSchema->close();
$stillWorks = $closeTy->newMessage();
$stillWorks->setInt('value', 3);
check($closeTy->decode($stillWorks->encode())->getInt('value') === 3,
    'a Type outlives Schema::close');
unset($closeTy, $closeSchema);

$root = $edgeTy->newMessage();
$child = $root->setMessage('inner');
$root->close();
try {
    $child->setInt('n', 1);
    check(false, 'closed root still accepted a child write');
} catch (Error $e) {
    check(str_contains($e->getMessage(), 'closed'), 'closed root rejects child writes');
}

/* A borrowed view closes itself, not its parent: closing the view then writing
 * reports "message is closed" instead of reaching the freed arena. */
$root = $edgeTy->newMessage();
$view = $root->setMessage('inner');
$view->close();
try {
    $view->setInt('n', 1);
    check(false, 'closed view still accepted a write');
} catch (Error $e) {
    check(str_contains($e->getMessage(), 'closed'), 'closed view rejects writes');
}
$root->close();

/* A Value read after its owner message closed must throw, not read the arena. */
$owner = $edgeTy->newMessage();
$owner->setString('tags', 'x')->setString('tags', 'y');
$val = $owner->at('tags', 1);
$owner->close();
try {
    $val->asString();
    check(false, 'Value read after owner close succeeded');
} catch (Error $e) {
    check(str_contains($e->getMessage(), 'closed'), 'Value after owner close throws');
}

/* ------------------------------------------------- verify-and-fill extras */

/*
 * Core coverage the FFI wrapper now passes through: deterministic encode,
 * encoded_size/encode_to, delimited framing, JSON and text mappings, input
 * limits, services, and the well-known-type helpers. (visit and encode_sink
 * have no PHP form: FFI cannot pass a PHP closure as a C callback.)
 */

$det = $ty->newMessage();
$det->setInt('i32', -7)->setString('str', 'det');
check($det->encodeDeterministic() === $det->encode(), 'deterministic matches encode');
check($det->encodedSize() === strlen($det->encode()), 'encoded_size matches');
$full = $det->encode();
check($det->encodeTo(strlen($full) + 12) === $full, 'encode_to sized cap works');
try {
    $det->encodeTo(4);
    check(false, 'encode_to short cap accepted');
} catch (Error) {
    check(true, 'encode_to rejects a short cap');
}

$frame = $det->encodeDelimited();
$part = $ty->decodeDelimited($frame);
check($part['read'] === strlen($frame), 'decodeDelimited reads the frame');
check($part['message']->encode() === $full, 'delimited round trip is wire-equal');
check($det->encodeDelimitedTo(strlen($frame) + 12) === $frame, 'encode_delimited_to matches');
try {
    $det->encodeDelimitedTo(4);
    check(false, 'encode_delimited_to short cap accepted');
} catch (Error) {
    check(true, 'encode_delimited_to rejects a short cap');
}
try {
    $ty->decodeDelimited(substr($frame, 0, 2));
    check(false, 'truncated frame accepted');
} catch (Error) {
    check(true, 'truncated frame');
}

$json = $det->toJson();
check(str_contains($json, '"str":"det"'), 'toJson prints');
check($ty->fromJson($json)->encode() === $full, 'JSON round trip is wire-equal');
try {
    $ty->fromJson('{nope');
    check(false, 'malformed JSON accepted');
} catch (Error) {
    check(true, 'malformed JSON');
}
try {
    $ty->fromJson('{"unknown_field":1}');
    check(false, 'unknown JSON field accepted');
} catch (Error) {
    check(true, 'unknown JSON field');
}
check($ty->fromJson('{"unknown_field":1}', true)->getInt('i32') === 0, 'ignored unknown JSON field reads as absent');

$text = $det->toText();
check(str_contains($text, 'str: "det"'), 'toText prints');
check($ty->fromText($text)->encode() === $full, 'text round trip is wire-equal');
try {
    $ty->fromText('str: ');
    check(false, 'malformed text accepted');
} catch (Error) {
    check(true, 'malformed text');
}

/* Limits reject oversized input on every entry point. */
foreach ([
    ['decode', fn() => $ty->decode($full, ['max_bytes' => 4])],
    ['fromJson', fn() => $ty->fromJson('{"str":"x"}', false, ['max_bytes' => 4])],
    ['fromText', fn() => $ty->fromText('str: "x"', ['max_bytes' => 4])],
    ['decodeDelimited', fn() => $ty->decodeDelimited($frame, ['max_bytes' => 2])],
    ['fromBytes', fn() => Schema::fromBytes(file_get_contents($desc), ['max_bytes' => 4])],
] as [$name, $fn]) {
    try {
        $fn();
        check(false, "$name max_bytes accepted");
    } catch (Error) {
        check(true, "$name max_bytes");
    }
}
check($ty->decode($full, [])->getString('str') === 'det', 'empty limits mean defaults');

/* Services and well-known types through the shared fixture. */
$adv = Schema::fromFile(dirname(__DIR__, 3) . '/tests/data/adv.desc');
check($adv->services() === ['pbwire.adv.Jobs'], 'services lists Jobs');
$jobs = $adv->service('pbwire.adv.Jobs');
check($jobs['name'] === 'pbwire.adv.Jobs' && count($jobs['methods']) === 2, 'service has two methods');
check($jobs['methods'][0]['name'] === 'Get' && $jobs['methods'][0]['input_type'] === 'pbwire.adv.Job', 'unary resolves');
check($jobs['methods'][0]['client_streaming'] === false && $jobs['methods'][0]['server_streaming'] === false, 'unary flags decode');
check($jobs['methods'][1]['client_streaming'] === true && $jobs['methods'][1]['server_streaming'] === true, 'streaming flags decode');
$routed = $adv->resolve('pbwire.adv.Jobs', 'Get');
check($routed['input']->name() === 'pbwire.adv.Job' && $routed['output']->name() === 'pbwire.adv.Job', 'resolve maps names');
try {
    $adv->service('pbwire.adv.Nope');
    check(false, 'unknown service accepted');
} catch (Error) {
    check(true, 'unknown service');
}
try {
    $adv->resolve('pbwire.adv.Jobs', 'Nope');
    check(false, 'unknown method accepted');
} catch (Error) {
    check(true, 'unknown method');
}

$jobTy = $routed['input'];
$job = $jobTy->newMessage();
$job->setString('id', '7');
$packed = $adv->message('google.protobuf.Any')->anyPack('type.googleapis.com/pbwire.adv.Job', $job->encode());
check($packed->anyTypeUrl() === 'type.googleapis.com/pbwire.adv.Job', 'any keeps the type url');
check($adv->message('pbwire.adv.Job')->decode($packed->anyPayload())->getString('id') === '7', 'any keeps the payload');
$ts = $adv->message('google.protobuf.Timestamp')->timestampFromMillis(1501);
check($ts->timestampMillis() === 1501, 'timestamp millis round trip');
$du = $adv->message('google.protobuf.Duration')->durationFromMillis(-2500);
check($du->durationMillis() === -2500, 'duration millis round trip');
$fm = $adv->message('google.protobuf.FieldMask')->fromJson('{"paths":["a.b","c"]}');
check($fm->fieldmaskPaths() === ['a.b', 'c'], 'fieldmask paths read');
try {
    $ty->anyPack('t', 'p');
    check(false, 'anyPack on a non-Any type accepted');
} catch (Error) {
    check(true, 'anyPack rejects a non-Any type');
}

echo "OK   $checks checks passed\n";

/* A Type caches its Field handles; that cycle must be collectable, or the
 * native schema behind an unreachable Type never goes away. */
$cycleSchema = Schema::fromFile($desc);
$cycleTy = $cycleSchema->message('structtest.Inner3');
$cycleField = $cycleTy->field('value');
$weakSchema = WeakReference::create($cycleSchema);
$weakType = WeakReference::create($cycleTy);
unset($cycleField, $cycleTy, $cycleSchema);
gc_collect_cycles();
check($weakType->get() === null, 'Type/Field cycle is collected');

echo "OK   $checks checks passed\n";

<?php

declare(strict_types=1);

/*
 * A round trip over the pinned matrix3.desc: build, encode, decode, read back.
 *
 *   php examples/roundtrip.php [descriptor-set]
 */

require __DIR__ . '/../runtime/pbwire.php';

use Pbwire\Schema;

$desc = $argv[1] ?? dirname(__DIR__, 2) . '/c/tests/data/matrix3.desc';
$schema = Schema::fromFile($desc);
$ty = $schema->message('structtest.Scalars3');
$innerTy = $schema->message('structtest.Inner3');

$msg = $ty->newMessage();
$msg->setInt('i32', -42)
    ->setString('str', 'alice')
    ->setBool('flag', true);

foreach (['a', 'bb'] as $tag) {
    $msg->setString('tags', $tag); /* repeated: appends */
}

$msg->setMessage('inner')
    ->setString($innerTy->field('name'), 'kid')
    ->setInt('value', 42);

$wire = $msg->encode();
echo strlen($wire) . ' bytes: ' . bin2hex($wire) . "\n";

$decoded = $ty->decode($wire);
echo 'i32   = ' . $decoded->getInt('i32') . "\n";
echo 'str   = ' . var_export($decoded->getString('str'), true) . "\n";
echo 'inner = ' . var_export($decoded->getMessage('inner')?->getString('name'), true) . "\n";
echo 'tags  = ';
for ($i = 0; $i < $decoded->count('tags'); $i++) {
    echo var_export($decoded->at('tags', $i)?->asString(), true) . ' ';
}
echo "\n";

if ($decoded->encode() !== $wire) {
    fwrite(STDERR, "round trip differs\n");
    exit(1);
}

echo "OK   byte-exact round trip\n";

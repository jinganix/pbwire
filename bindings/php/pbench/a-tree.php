<?php

declare(strict_types=1);

/*
 * tree arm: the shipped per-field builder/wire-view API doing the whole task.
 * No prebuilt handle before the timer: every call builds a fresh Message one
 * set per field from the native array, encodes it, and closes it; every
 * decode builds a fresh Message from the wire, reads every gate-checked field
 * one get per field back into a fresh native array, and closes it. That is
 * what a user of the per-field API pays for the whole task.
 *
 * The native array itself is app-owned and reused across iterations: it is
 * plain PHP data, and reusing a plain-data fixture is standard harness
 * practice (Task 5 ledger ruling, Python worker D3, merged 99ac9ac). Only
 * the Message -- allocation, the whole per-field walk, encode, close -- sits
 * inside the timer, matching the timer boundary every binding times under.
 * The decode closes the Message before returning, so no view outlives it.
 *
 * The prebuilt ops below (tree_prepare/tree_encode/tree_decode: encode a
 * held handle, decode-and-free with no read-back) are the old task, kept for
 * the `pbwire-php-proxy` row: same-run timing only, never the headline.
 *
 *   tree_encode_full(type, innerType, array) -> wire bytes
 *   tree_decode_full(type, wire)             -> native array
 *   tree_prepare(wire) -> Message (proxy task only)
 *   tree_encode(Message) -> string (proxy task only)
 *   tree_decode(wire)   -> Message (proxy task only)
 */

function tree_encode_full(Pbwire\Type $ty, Pbwire\Type $innerTy, array $payload): string
{
    $name = $innerTy->field('name');
    $value = $innerTy->field('value');

    $msg = $ty->newMessage();
    $msg->setInt('i32', $payload['i32'])
        ->setInt('i64', $payload['i64'])
        ->setInt('u32', $payload['u32'])
        ->setInt('u64', tree_u64_bits($payload['u64']))
        ->setInt('s32', $payload['s32'])
        ->setInt('s64', $payload['s64'])
        ->setInt('fx32', $payload['fx32'])
        ->setInt('fx64', $payload['fx64'])
        ->setInt('sfx32', $payload['sfx32'])
        ->setInt('sfx64', $payload['sfx64'])
        ->setFloat('f32', $payload['f32'])
        ->setDouble('f64', $payload['f64'])
        ->setBool('flag', $payload['flag'])
        ->setString('str', $payload['str'])
        ->setInt('color', $payload['color'])
        ->setInt('opt_i32', $payload['opt_i32'])
        ->setString('opt_str', $payload['opt_str']);

    $inner = $payload['inner'];
    $msg->setMessage('inner')->setString($name, $inner['name'])->setInt($value, $inner['value']);

    $n = count($payload['nums']);
    for ($i = 0; $i < $n; $i++) {
        $msg->setInt('nums', $payload['nums'][$i])
            ->setString('tags', $payload['tags'][$i])
            ->setBool('flags', $payload['flags'][$i]);
        $sub = $payload['inners'][$i];
        $msg->setMessage('inners')->setString($name, $sub['name'])->setInt($value, $sub['value']);
    }

    $wire = $msg->encode();
    $msg->close();

    return $wire;
}

/*
 * Decode the wire and read every gate-checked field back one get per field
 * into a fresh native array in the same shape (and key order) as
 * Type::decodeArray, then close the Message. Key order matches the shipped
 * decodeArray output so the gate can compare the two strictly.
 */
function tree_decode_full(Pbwire\Type $ty, string $wire): array
{
    $msg = $ty->decode($wire);

    $u64 = $msg->getInt('u64');

    $out = [
        'i32' => $msg->getInt('i32'),
        'i64' => $msg->getInt('i64'),
        'u32' => $msg->getInt('u32'),
        'u64' => $u64 < 0 ? sprintf('%u', $u64) : $u64,
        's32' => $msg->getInt('s32'),
        's64' => $msg->getInt('s64'),
        'fx32' => $msg->getInt('fx32'),
        'fx64' => $msg->getInt('fx64'),
        'sfx32' => $msg->getInt('sfx32'),
        'sfx64' => $msg->getInt('sfx64'),
        'f32' => $msg->getFloat('f32'),
        'f64' => $msg->getDouble('f64'),
        'flag' => $msg->getBool('flag'),
        'str' => $msg->getString('str') ?? '',
        'color' => $msg->getInt('color'),
        'inner' => [
            'name' => $msg->getMessage('inner')?->getString('name') ?? '',
            'value' => $msg->getMessage('inner')?->getInt('value') ?? 0,
        ],
    ];

    $nums = [];
    $tags = [];
    $flags = [];
    $inners = [];
    $n = $msg->count('nums');
    for ($i = 0; $i < $n; $i++) {
        $nums[] = $msg->at('nums', $i)->asInt();
        $tags[] = $msg->at('tags', $i)->asString() ?? '';
        $flags[] = $msg->at('flags', $i)->asBool();
        $child = $msg->at('inners', $i)->asMessage();
        $inners[] = [
            'name' => $child?->getString('name') ?? '',
            'value' => $child?->getInt('value') ?? 0,
        ];
    }
    $out['nums'] = $nums;
    $out['tags'] = $tags;
    $out['inners'] = $inners;
    $out['opt_i32'] = $msg->getInt('opt_i32');
    $out['opt_str'] = $msg->getString('opt_str') ?? '';
    $out['flags'] = $flags;

    $msg->close();

    return $out;
}

/*
 * A decimal uint64 string -> the int whose 64 bits are the value. Same
 * digit loop as the shipped Type::uint64Bits behind encodeArray; arm-local
 * because that helper is private to Type while the bridge (newMessage, one
 * set per field, encode, close) stays the shipped API.
 */
function tree_u64_bits(string $text): int
{
    if ($text === '') {
        throw new Pbwire\Error('tree: uint64 decimal string is empty');
    }

    $lo = 0;
    $hi = 0;
    $len = strlen($text);

    for ($i = 0; $i < $len; $i++) {
        $digit = ord($text[$i]) - 48;

        if ($digit < 0 || $digit > 9) {
            throw new Pbwire\Error('tree: uint64 must be a decimal string');
        }

        $t = $lo * 10 + $digit;
        $lo = $t & 0xFFFFFFFF;
        $hi = $hi * 10 + intdiv($t - $lo, 4294967296);

        if ($hi > 0xFFFFFFFF) {
            throw new Pbwire\Error('tree: uint64 decimal string overflows');
        }
    }

    return ($hi << 32) | $lo;
}

/* The prebuilt proxy task: a held handle in, re-encode out. */

function tree_prepare(string $wire): Pbwire\Message
{
    return pbwire_pbench_type()->decode($wire);
}

function tree_encode(Pbwire\Message $tree): string
{
    return $tree->encode();
}

function tree_decode(string $wire): Pbwire\Message
{
    return pbwire_pbench_type()->decode($wire);
}

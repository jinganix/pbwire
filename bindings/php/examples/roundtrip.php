<?php

declare(strict_types=1);

/*
 * The C example (examples/addressbook.c), in PHP, through the generated typed
 * classes: assign fields, encode, decode, compare. The encoded bytes must equal
 * the pinned reference examples/addressbook.bin.
 *
 *   php bindings/php/examples/roundtrip.php
 */

require_once __DIR__ . '/../runtime/pbwire.php';
require_once __DIR__ . '/gen/addressbook.php';

use Pbwire\Gen\Tutorial_AddressBook;
use Pbwire\Gen\Tutorial_Person;
use Pbwire\Gen\Tutorial_Person_PhoneNumber;
use Pbwire\Gen\Tutorial_Person_PhoneType;
use Pbwire\Schema;

$desc = $argv[1] ?? __DIR__ . '/../../../examples/addressbook.desc';
$bin = $argv[2] ?? __DIR__ . '/../../../examples/addressbook.bin';

$schema = Schema::fromFile($desc);
$book = build();
$wire = $book->encode($schema);

if (is_file($bin) && $wire !== file_get_contents($bin)) {
    fwrite(STDERR, "encode does not match $bin\n");
    exit(1);
}

$back = Tutorial_AddressBook::decode($schema, $wire);
printf("addressbook: %d bytes, %d people\n", strlen($wire), count($back->people));
foreach ($back->people as $person) {
    printf("  %s (%d) %s\n", $person->name, $person->id, $person->hasEmail ? $person->email : '');
    foreach ($person->phones as $phone) {
        printf("    %s\n", $phone->number);
    }
}
$schema->close();

function build(): Tutorial_AddressBook
{
    $alice = new Tutorial_Person();
    $alice->name = 'Alice';
    $alice->id = 1;
    $alice->phones = [phone('555-0100', Tutorial_Person_PhoneType::MOBILE)];

    $bob = new Tutorial_Person();
    $bob->name = 'Bob';
    $bob->id = 2;
    $bob->hasEmail = true;
    $bob->email = 'bob@example.com';
    $bob->phones = [phone('555-0200', Tutorial_Person_PhoneType::HOME)];

    $book = new Tutorial_AddressBook();
    $book->people = [$alice, $bob];
    return $book;
}

function phone(string $number, int $type): Tutorial_Person_PhoneNumber
{
    $phone = new Tutorial_Person_PhoneNumber();
    $phone->number = $number;
    $phone->hasType = true;
    $phone->type = $type;
    return $phone;
}

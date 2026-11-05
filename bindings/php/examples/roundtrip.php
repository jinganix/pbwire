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
require_once __DIR__ . '/../runtime/generated.php';
require_once __DIR__ . '/gen/addressbook.php';

use Pbwire\Gen\Tutorial_AddressBook;
use Pbwire\Gen\Tutorial_Person;
use Pbwire\Gen\Tutorial_Person_PhoneNumber;
use Pbwire\Gen\Tutorial_Person_PhoneType;

$bin = $argv[1] ?? __DIR__ . '/../../../examples/addressbook.bin';

$book = build();
$wire = $book->serializeToString();

if (is_file($bin) && $wire !== file_get_contents($bin)) {
    fwrite(STDERR, "encode does not match $bin\n");
    exit(1);
}

$back = Tutorial_AddressBook::fromString($wire);
printf("addressbook: %d bytes, %d people\n", strlen($wire), count($back->getPeople()));
foreach ($back->getPeople() as $person) {
    printf("  %s (%d) %s\n", $person->getName(), $person->getId(), $person->hasEmail() ? $person->getEmail() : '');
    foreach ($person->getPhones() as $phone) {
        printf("    %s\n", $phone->getNumber());
    }
}

function build(): Tutorial_AddressBook
{
    $alice = (new Tutorial_Person())
        ->setName('Alice')
        ->setId(1)
        ->setPhones([phone('555-0100', Tutorial_Person_PhoneType::MOBILE)]);

    $bob = (new Tutorial_Person())
        ->setName('Bob')
        ->setId(2)
        ->setEmail('bob@example.com')
        ->setPhones([phone('555-0200', Tutorial_Person_PhoneType::HOME)]);

    return (new Tutorial_AddressBook())->setPeople([$alice, $bob]);
}

function phone(string $number, int $type): Tutorial_Person_PhoneNumber
{
    return (new Tutorial_Person_PhoneNumber())->setNumber($number)->setType($type);
}

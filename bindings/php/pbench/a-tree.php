<?php

declare(strict_types=1);

/*
 * tree arm: the shipped Message handle. The data is already in a tree outside
 * the timer -- one Type::decode() of the reference wire -- and the timed ops
 * are one encode of it and one wire -> tree decode. That is the codec cost once
 * the application already holds a pbwire tree.
 *
 *   tree_prepare(wire) -> Message
 *   tree_encode(Message) -> string
 *   tree_decode(wire)   -> Message
 */

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

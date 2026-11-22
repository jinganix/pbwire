<?php

declare(strict_types=1);

/*
 * shim arm: a native PHP array in and wire bytes out (or wire bytes in and the
 * fully materialized array out). With the C extension loaded this is ONE
 * extension entry that walks the array in C; with the shipped ext-ffi wrapper
 * it is the same API with the walk in PHP and one FFI call per field, which is
 * why that build is published as its own row (`pbwire-php-shim-ffi`).
 *
 * The input array is app-owned and reused across iterations: plain-data
 * fixture reuse is standard harness practice (Task 5 ledger ruling, Python
 * worker D3, merged 99ac9ac). The decode is documented-eager: decodeArray
 * builds every repeated element and converts every scalar before returning,
 * so the timed decode prices the full materialization.
 *
 *   shim_impl()             -> row name for the loaded runtime
 *   shim_encode(array)      -> wire bytes
 *   shim_decode(wire)       -> array
 */

function shim_impl(): string
{
    return extension_loaded('pbwire') ? 'pbwire-php-shim' : 'pbwire-php-shim-ffi';
}

function shim_encode(array $message): string
{
    return pbwire_pbench_type()->encodeArray($message);
}

function shim_decode(string $wire): array
{
    return pbwire_pbench_type()->decodeArray($wire);
}

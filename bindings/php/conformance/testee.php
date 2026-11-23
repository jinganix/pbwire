<?php

declare(strict_types=1);

/*
 * Google conformance testee driven by the pbwire PHP binding.
 *
 * Part of the repo-wide conformance story: `conformance/testee.c` proves the
 * dynamic core, this script proves the PHP binding on the same requests. It
 * runs on the shipped `ext-ffi` wrapper (the default `make php-test` path),
 * so it needs no compiler and no `.so` build.
 *
 * It speaks the runner's wire protocol (4-byte little-endian length prefix,
 * then a ConformanceRequest / ConformanceResponse) and, because the binding
 * is schema-driven, it needs no per-message code: the request names the
 * message type, the descriptor set answers with the schema.
 *
 * Supported: binary, JSON and text in -> binary, JSON or text out. JSPB
 * answers `skipped`, like every other testee here.
 *
 * decode() validates the whole payload in one scan and keeps the bytes it
 * was parsed from, so encode() hands them back byte for byte: a request is
 * accepted exactly when the reference implementation accepts it.
 *
 *   php bindings/php/conformance/testee.php build/conformance.desc
 */

require __DIR__ . '/../runtime/pbwire.php';

use Pbwire\Error;
use Pbwire\Message;
use Pbwire\Schema;
use Pbwire\Type;

const PROTOBUF = 1;
const JSON = 2;
const JSPB = 3;
const TEXT = 4;

/* The runner sends 3 (JSON_IGNORE_UNKNOWN_PARSE_TEST) when JSON input with
 * unknown fields must parse; anything else rejects them. */
const JSON_IGNORE_UNKNOWN = 3;

function readBytes($stream, int $n): ?string
{
    $out = '';

    while (strlen($out) < $n && !feof($stream)) {
        $chunk = fread($stream, $n - strlen($out));

        if ($chunk === false || $chunk === '') {
            break;
        }

        $out .= $chunk;
    }

    return strlen($out) === $n ? $out : null;
}

function readRequest($stream): ?string
{
    $header = readBytes($stream, 4);

    if ($header === null) {
        return null;
    }

    $size = unpack('V', $header)[1];

    return readBytes($stream, $size);
}

function writeResponse($stream, string $payload): void
{
    fwrite($stream, pack('V', strlen($payload)));
    fwrite($stream, $payload);
}

/* One input payload wins: binary beats JSON beats text, matching the
 * reference testee's precedence. Returns a message holding the parsed tree:
 * binary decodes materialize, JSON/text parses already arrive as trees. */
function parseInput(Type $message, Message $request): Message
{
    if ($request->has('protobuf_payload')) {
        $payload = $request->getBytes('protobuf_payload');

        if ($payload === null) {
            throw new Error('no binary payload in the request');
        }

        return $message->decode($payload);
    }

    if ($request->has('json_payload')) {
        $ignore = $request->getInt('test_category') === JSON_IGNORE_UNKNOWN;

        return $message->fromJson($request->getString('json_payload') ?? '', $ignore);
    }

    if ($request->has('text_payload')) {
        return $message->fromText($request->getString('text_payload') ?? '');
    }

    throw new Error('no input payload in the request');
}

function answer(Type $responseType, $stdout, array $fields): void
{
    $response = $responseType->newMessage();

    foreach ($fields as $name => $value) {
        if (is_bool($value)) {
            $response->setBool($name, $value);
        } elseif (is_int($value)) {
            $response->setInt($name, $value);
        } elseif ($name === 'protobuf_payload') {
            $response->setBytes($name, $value);
        } else {
            $response->setString($name, $value);
        }
    }

    try {
        $payload = $response->encode();
    } catch (Throwable $error) {
        try {
            $fallback = $responseType->newMessage();
            $fallback->setString('runtime_error', 'cannot encode response: ' . $error->getMessage());
            $payload = $fallback->encode();
        } catch (Throwable $fallbackError) {
            $payload = '';
        }
    }

    writeResponse($stdout, $payload);
}

if ($argc < 2) {
    fwrite(STDERR, "usage: testee.php <conformance.desc>\n");
    exit(2);
}

$schema = Schema::fromFile($argv[1]);
$requestType = $schema->message('conformance.ConformanceRequest');
$responseType = $schema->message('conformance.ConformanceResponse');
$types = [];

$stdin = fopen('php://stdin', 'rb');
$stdout = fopen('php://stdout', 'wb');

if ($stdin === false || $stdout === false) {
    fwrite(STDERR, "cannot open stdio\n");
    exit(2);
}

while (($body = readRequest($stdin)) !== null) {
    try {
        $request = $requestType->decode($body);
    } catch (Throwable $error) {
        answer($responseType, $stdout, ['runtime_error' => 'cannot parse ConformanceRequest: ' . $error->getMessage()]);
        continue;
    }

    try {
        $wanted = $request->getInt('requested_output_format');
        $name = $request->getString('message_type') ?? '';

        if (!isset($types[$name])) {
            $types[$name] = $schema->message($name);
        }

        $message = $types[$name];
    } catch (Throwable $error) {
        answer($responseType, $stdout, ['runtime_error' => "unknown message type '" . ($name ?? '') . "'"]);
        continue;
    }

    if ($wanted === JSPB) {
        answer($responseType, $stdout, ['skipped' => 'JSPB is not supported']);
        continue;
    }

    if ($wanted !== PROTOBUF && $wanted !== JSON && $wanted !== TEXT) {
        answer($responseType, $stdout, ['skipped' => "output format {$wanted} is not supported"]);
        continue;
    }

    try {
        $parsed = parseInput($message, $request);
    } catch (Throwable $error) {
        answer($responseType, $stdout, ['parse_error' => $error->getMessage()]);
        continue;
    }

    try {
        if ($wanted === PROTOBUF) {
            answer($responseType, $stdout, ['protobuf_payload' => $parsed->encode()]);
        } elseif ($wanted === JSON) {
            answer($responseType, $stdout, ['json_payload' => $parsed->toJson()]);
        } else {
            $printUnknown = $request->getBool('print_unknown_fields');
            answer($responseType, $stdout, ['text_payload' => $parsed->toText($printUnknown)]);
        }
    } catch (Throwable $error) {
        answer($responseType, $stdout, ['serialize_error' => $error->getMessage()]);
    }
}

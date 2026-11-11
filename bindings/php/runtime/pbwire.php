<?php

declare(strict_types=1);

/*
 * The PHP binding: an FFI wrapper over the C ABI in include/pbwire.h. PHP ships
 * ext-ffi, so there is no C extension and no build step; the shared library is
 * dlopen()ed and a FileDescriptorSet (protoc --descriptor_set_out) becomes the
 * schema at run time. No generated code, no composer dependency.
 *
 * The wire format lives in the C core, so a PHP message and a C message are the
 * same bytes by construction.
 *
 * Per-thread state: none. A Schema or Message belongs to one thread.
 */

namespace Pbwire;

if (\extension_loaded('pbwire')) {
    return;
}

use FFI;
use FFI\CData;

final class Error extends \RuntimeException
{
}

/**
 * The dlopen()ed library plus the one FFI handle every call goes through.
 */
final class Lib
{
    private const HEADER = <<<'C'
        typedef struct { size_t max_bytes; unsigned int max_depth; } pbw_limits;
        typedef struct { int ignore_unknown_fields; } pbw_json_parse_options;
        int pbw_abi_version(void);
        const char *pbw_strerror(int status);
        void pbw_free(void *ptr);
        int pbw_descriptor_set_load(const void *data, size_t len, void **out);
        void pbw_schema_free(void *schema);
        const char *pbw_schema_error(void *schema);
        void *pbw_schema_find(void *schema, const char *name);
        const char *pbw_descriptor_name(void *desc);
        void *pbw_descriptor_find_field_name(void *desc, const char *name);
        void *pbw_descriptor_find_field(void *desc, unsigned int number);
        const char *pbw_field_name(void *field);
        unsigned int pbw_field_number(void *field);
        int pbw_field_type(void *field);
        int pbw_field_label(void *field);
        void *pbw_field_message_type(void *field);
        void *pbw_message_new(void *desc);
        void pbw_message_free(void *msg);
        void *pbw_message_get_field(void *msg, void *field);
        int pbw_message_set_field(void *msg, void *field, void *value);
        void *pbw_message_set_message(void *msg, unsigned int number);
        void pbw_message_clear(void *msg, unsigned int number);
        void *pbw_value_new_int(long long value);
        void *pbw_value_new_bool(int value);
        void *pbw_value_new_float(float value);
        void *pbw_value_new_double(double value);
        void *pbw_value_new_bytes(const void *data, size_t len);
        void *pbw_value_new_string(const char *str);
        void *pbw_ctx_new(void);
        void pbw_ctx_free(void *ctx);
        void *pbw_message_new_ctx(void *ctx, void *desc);
        void *pbw_value_new_uint_ctx(void *ctx, long long bits);
        void *pbw_value_new_bytes_ctx(void *ctx, const void *data, size_t len);
        void pbw_value_free(void *value);
        long long pbw_value_get_int(void *value);
        int pbw_value_get_bool(void *value);
        float pbw_value_get_float(void *value);
        double pbw_value_get_double(void *value);
        const void *pbw_value_get_bytes(void *value, size_t *len);
        const char *pbw_value_get_string(void *value);
        void *pbw_value_get_message(void *value);
        size_t pbw_value_count(void *value);
        void *pbw_value_at(void *value, size_t index);
        int pbw_encode(void *msg, void **out, size_t *len);
        int pbw_encode_to(void *msg, void *out, size_t cap, size_t *len);
        int pbw_encode_deterministic(void *msg, void **out, size_t *len);
        int pbw_message_encoded_size(void *msg, size_t *len);
        int pbw_encode_delimited(void *msg, void **out, size_t *len);
        int pbw_encode_delimited_to(void *msg, void *out, size_t cap, size_t *len);
        int pbw_decode(void *desc, const void *buf, size_t len, void **out);
        int pbw_decode_limits(void *desc, const void *buf, size_t len, const pbw_limits *limits, void **out);
        int pbw_decode_delimited(void *desc, const void *buf, size_t len, void **out, size_t *consumed);
        int pbw_decode_delimited_limits(void *desc, const void *buf, size_t len, const pbw_limits *limits, void **out, size_t *consumed);
        int pbw_descriptor_set_load_limits(const void *data, size_t len, const pbw_limits *limits, void **out);
        int pbw_json_print(void *msg, void **out, size_t *len);
        int pbw_json_parse_opts(void *desc, const char *text, size_t len, const pbw_json_parse_options *options, void **out);
        int pbw_json_parse_limits(void *desc, const char *text, size_t len, const pbw_limits *limits, const pbw_json_parse_options *options, void **out);
        int pbw_text_print_opts(void *msg, int print_unknown_fields, void **out, size_t *len);
        int pbw_text_parse(void *desc, const char *text, size_t len, void **out);
        int pbw_text_parse_limits(void *desc, const char *text, size_t len, const pbw_limits *limits, void **out);
        size_t pbw_schema_service_count(void *schema);
        void *pbw_schema_service_at(void *schema, size_t index);
        void *pbw_schema_service_find(void *schema, const char *name);
        const char *pbw_service_name(void *svc);
        size_t pbw_service_method_count(void *svc);
        void *pbw_service_method_at(void *svc, size_t index);
        const char *pbw_method_name(void *method);
        const char *pbw_method_input_type(void *method);
        const char *pbw_method_output_type(void *method);
        int pbw_method_client_streaming(void *method);
        int pbw_method_server_streaming(void *method);
        int pbw_service_resolve(void *schema, const char *service, const char *method, void **in_desc, void **out_desc);
        int pbw_any_pack(void *any_desc, const char *type_url, const void *payload, size_t len, void **out);
        const char *pbw_any_type_url(void *msg);
        const void *pbw_any_payload(void *msg, size_t *len);
        int pbw_timestamp_to_millis(void *ts, long long *out);
        int pbw_duration_to_millis(void *dur, long long *out);
        int pbw_timestamp_from_millis(void *desc, long long ms, void **out);
        int pbw_duration_from_millis(void *desc, long long ms, void **out);
        size_t pbw_fieldmask_path_count(void *mask);
        const char *pbw_fieldmask_path_at(void *mask, size_t index);
        C;

    private static ?FFI $ffi = null;

    private function __construct()
    {
    }

    public static function ffi(): FFI
    {
        if (self::$ffi === null) {
            self::$ffi = FFI::cdef(self::HEADER, self::path());
            $abi = self::$ffi->pbw_abi_version();

            if ($abi !== 1) {
                throw new Error("pbwire: ABI version $abi, this binding needs 1");
            }
        }

        return self::$ffi;
    }

    /** The shared library: PBWIRE_LIB, else build/libpbwire.{dylib,so} next to the checkout. */
    public static function path(): string
    {
        $env = getenv('PBWIRE_LIB');

        if ($env !== false && $env !== '') {
            return $env;
        }

        $root = dirname(__DIR__, 3);

        foreach (['dylib', 'so'] as $suffix) {
            foreach ([getcwd() . "/build/libpbwire.$suffix", "$root/build/libpbwire.$suffix"] as $candidate) {
                if (is_file($candidate)) {
                    return $candidate;
                }
            }
        }

        throw new Error('pbwire: no shared library found (set PBWIRE_LIB; run `make shared`)');
    }

    /** An out-parameter slot of the given C type, e.g. newSlot('size_t'). */
    public static function newSlot(string $type): CData
    {
        return self::ffi()->new($type);
    }

    /** A reused out-parameter slot, one per C type. The hot paths (encode,
     * decode, byte getters) must use this, not newSlot: addr() of a freshly
     * allocated slot is never collected, so a loop over newSlot leaks ~100 B
     * per call. Shared mutable state: single-threaded use only, which the
     * binding already assumes. */
    private static array $slots = [];
    public static function slot(string $type): CData
    {
        return self::$slots[$type] ??= self::ffi()->new($type);
    }

    /* `const char *` returns arrive as PHP strings (null when the C side returns NULL). */
    public static function errorText(int $status): string
    {
        return (string) self::ffi()->pbw_strerror($status);
    }

    /** The struct an limitsPtr() pointer points into: FFI::addr() does not
     * keep its source alive, so the pointer dangles once the struct's scope
     * ends. Calls are synchronous (and the binding is single-threaded), so a
     * reused slot keeps every live pointer valid and costs no allocation
     * (newSlot per call leaks, see slot()). */
    private static ?CData $limitsHold = null;

    /** One limits entry: a non-negative integer, or 0 when absent. */
    private static function limitValue(array $limits, string $key, int $max): int
    {
        if (!isset($limits[$key])) {
            return 0;
        }
        $v = $limits[$key];
        if (!is_int($v)) {
            throw new \InvalidArgumentException(
                "pbwire: limits['$key'] must be an integer");
        }
        if ($v < 0 || $v > $max) {
            throw new \InvalidArgumentException(
                "pbwire: limits['$key'] must be between 0 and $max");
        }

        return $v;
    }

    /** A pbw_limits pointer for ['max_bytes' => n, 'max_depth' => n], or
     * null for the core defaults (no byte cap, PBW_MAX_DEPTH nesting). */
    public static function limitsPtr(?array $limits): ?CData
    {
        if ($limits === null) {
            return null;
        }
        $l = self::$limitsHold ??= self::ffi()->new('pbw_limits');
        /* A reused slot keeps the previous call's values: zero both fields
         * first so an omitted key means the core default, not a stale cap.
         * A negative value would wrap into an unsigned C field and silently
         * disable the cap, so each one is validated as an integer first. */
        $l->max_bytes = self::limitValue($limits, 'max_bytes', PHP_INT_MAX);
        $l->max_depth = self::limitValue($limits, 'max_depth', 0xFFFFFFFF);

        return FFI::addr($l);
    }
}

/**
 * A loaded FileDescriptorSet: the schema every MessageType comes from.
 */
final class Schema
{
    private ?CData $handle;
    /** Reference count: this object plus every Type it handed out. `close()`
     * drops the object's own reference; the last Type frees the native
     * schema, so a Type (and its Field/Message objects) never points into
     * freed memory. */
    private int $refs = 1;
    private bool $closed = false;

    private function __construct(CData $handle)
    {
        $this->handle = $handle;
    }

    public static function fromFile(string $path, ?array $limits = null): self
    {
        $bytes = @file_get_contents($path);

        if ($bytes === false) {
            throw new Error("pbwire: cannot read $path");
        }

        return self::fromBytes($bytes, $limits);
    }

    /** Load descriptor-set bytes; use --include_imports so every type resolves.
     * A trailing ['max_bytes' => n, 'max_depth' => n] caps untrusted input. */
    public static function fromBytes(string $bytes, ?array $limits = null): self
    {
        $ffi = Lib::ffi();
        $out = Lib::slot('void*[1]');
        $status = $ffi->pbw_descriptor_set_load_limits($bytes, strlen($bytes), Lib::limitsPtr($limits), FFI::addr($out[0]));
        $handle = $out[0];

        if ($status !== 0) {
            $why = $handle !== null ? (string) $ffi->pbw_schema_error($handle) : '';

            if ($handle !== null) {
                $ffi->pbw_schema_free($handle);
            }

            throw new Error('descriptor set: ' . ($why !== '' ? $why : Lib::errorText($status)));
        }

        if ($handle === null) {
            throw new Error('descriptor set: no schema returned');
        }

        return new self($handle);
    }

    /** Service names of this schema, for routing. */
    public function services(): array
    {
        $ffi = Lib::ffi();
        $count = $ffi->pbw_schema_service_count($this->cdata());
        $names = [];
        for ($i = 0; $i < $count; $i++) {
            $svc = $ffi->pbw_schema_service_at($this->cdata(), $i);
            if ($svc === null) {
                throw new Error('services: no service returned');
            }
            $names[] = (string) $ffi->pbw_service_name($svc);
        }

        return $names;
    }

    /** One service with its methods and streaming flags; throws when unknown. */
    public function service(string $name): array
    {
        $ffi = Lib::ffi();
        $svc = $ffi->pbw_schema_service_find($this->cdata(), $name);
        if ($svc === null) {
            throw new Error("no such service: $name");
        }
        $methods = [];
        $count = $ffi->pbw_service_method_count($svc);
        for ($i = 0; $i < $count; $i++) {
            $m = $ffi->pbw_service_method_at($svc, $i);
            if ($m === null) {
                throw new Error("service $name: no method returned");
            }
            $methods[] = [
                'name' => (string) $ffi->pbw_method_name($m),
                'input_type' => (string) $ffi->pbw_method_input_type($m),
                'output_type' => (string) $ffi->pbw_method_output_type($m),
                'client_streaming' => $ffi->pbw_method_client_streaming($m) !== 0,
                'server_streaming' => $ffi->pbw_method_server_streaming($m) !== 0,
            ];
        }

        return ['name' => (string) $ffi->pbw_service_name($svc), 'methods' => $methods];
    }

    /** Resolve a method to its input/output types for routing; throws when
     * the service or the method is unknown. */
    public function resolve(string $service, string $method): array
    {
        $ffi = Lib::ffi();
        $in = Lib::newSlot('void*[1]');
        $out = Lib::newSlot('void*[1]');
        $status = $ffi->pbw_service_resolve($this->cdata(), $service, $method, FFI::addr($in[0]), FFI::addr($out[0]));
        if ($status !== 0) {
            throw new Error("resolve $service/$method: " . Lib::errorText($status));
        }
        if ($in[0] === null || $out[0] === null) {
            throw new Error("resolve $service/$method: no type returned");
        }

        return ['input' => new Type($this, $in[0]), 'output' => new Type($this, $out[0])];
    }

    /** Looks up a message by fully qualified name, e.g. "structtest.Scalars3". */
    public function message(string $fullName): Type
    {
        if ($this->closed) {
            throw new Error('schema is closed');
        }
        $desc = Lib::ffi()->pbw_schema_find($this->cdata(), $fullName);

        if ($desc === null) {
            throw new Error("no such message type: $fullName");
        }

        /* The Type keeps the Schema object (and so the native schema) alive: a
         * MessageType must not outlive the descriptor it points into. */
        return new Type($this, $desc);
    }

    public function cdata(): CData
    {
        if ($this->handle === null || $this->closed) {
            throw new Error('schema is closed');
        }

        return $this->handle;
    }

    /** @internal A Type takes a reference on construction. */
    public function retain(): void
    {
        $this->refs++;
    }

    /** @internal A Type drops its reference on destruction. */
    public function release(): void
    {
        if ($this->refs <= 0) {
            return;
        }
        if (--$this->refs === 0 && $this->handle !== null) {
            Lib::ffi()->pbw_schema_free($this->handle);
            $this->handle = null;
        }
    }

    public function close(): void
    {
        if ($this->closed) {
            return;
        }
        $this->closed = true;
        /* With a Type alive this only drops the object's own reference. */
        $this->release();
    }

    public function __destruct()
    {
        $this->close();
    }
}

/**
 * A message type from a Schema.
 */
final class Type
{
    private const TYPE_MESSAGE = 11;
    private const TYPE_GROUP = 10;

    private CData $desc;
    private Schema $schema;

    /** @var array<string, Field> */
    private array $fields = [];

    /** @internal Holds $schema so the descriptor cannot outlive its schema. */
    public function __construct(Schema $schema, CData $desc)
    {
        $schema->retain();
        $this->schema = $schema;
        $this->desc = $desc;
    }

    public function __destruct()
    {
        $this->schema->release();
    }

    public function name(): string
    {
        return (string) Lib::ffi()->pbw_descriptor_name($this->desc);
    }

    /** Resolves a field once and caches it: a set/get then costs one native call. */
    public function field(string $name): Field
    {
        if (!isset($this->fields[$name])) {
            $ptr = Lib::ffi()->pbw_descriptor_find_field_name($this->desc, $name);

            if ($ptr === null) {
                throw new Error("{$this->name()}: no such field: $name");
            }

            $this->fields[$name] = new Field($ptr, $this);
        }

        return $this->fields[$name];
    }

    public function fieldByNumber(int $number): Field
    {
        $ptr = Lib::ffi()->pbw_descriptor_find_field($this->desc, $number);

        if ($ptr === null) {
            throw new Error("{$this->name()}: no field $number");
        }

        return new Field($ptr, $this);
    }

    public function newMessage(): Message
    {
        /* Built trees live in their own context arena: one bump allocator
         * instead of a malloc per value, sequential layout for the encoder,
         * and a single free on close. The context is never decoded into,
         * so the tree stays mutable for its whole life. */
        $ctx = Lib::ffi()->pbw_ctx_new();

        if ($ctx === null) {
            throw new Error('pbw_ctx_new: out of memory');
        }

        $ptr = Lib::ffi()->pbw_message_new_ctx($ctx, $this->desc);

        if ($ptr === null) {
            Lib::ffi()->pbw_ctx_free($ctx);
            throw new Error('pbw_message_new: out of memory');
        }

        return Message::root($this, $ptr, $ctx);
    }

    /** Decodes wire bytes; unknown fields are preserved and re-encoded.
     * A trailing ['max_bytes' => n, 'max_depth' => n] caps untrusted input. */
    public function decode(string $wire, ?array $limits = null): Message
    {
        $out = Lib::slot('void*[1]');
        if ($limits === null) {
            $status = Lib::ffi()->pbw_decode($this->desc, $wire, strlen($wire), FFI::addr($out[0]));
        } else {
            $status = Lib::ffi()->pbw_decode_limits($this->desc, $wire, strlen($wire), Lib::limitsPtr($limits), FFI::addr($out[0]));
        }

        if ($status !== 0) {
            throw new Error('decode: ' . Lib::errorText($status));
        }

        if ($out[0] === null) {
            throw new Error('decode: no message returned');
        }

        return Message::root($this, $out[0]);
    }

    /** Decodes one length-delimited frame; 'read' counts the length prefix
     * plus the frame, so concatenated frames decode in a loop. */
    public function decodeDelimited(string $wire, ?array $limits = null): array
    {
        $out = Lib::slot('void*[1]');
        $used = Lib::newSlot('size_t');
        if ($limits === null) {
            $status = Lib::ffi()->pbw_decode_delimited($this->desc, $wire, strlen($wire), FFI::addr($out[0]), FFI::addr($used));
        } else {
            $status = Lib::ffi()->pbw_decode_delimited_limits($this->desc, $wire, strlen($wire), Lib::limitsPtr($limits), FFI::addr($out[0]), FFI::addr($used));
        }

        if ($status !== 0) {
            throw new Error('decodeDelimited: ' . Lib::errorText($status));
        }

        if ($out[0] === null) {
            throw new Error('decodeDelimited: no message returned');
        }

        return ['message' => Message::root($this, $out[0]), 'read' => $used->cdata];
    }

    /** Parses JSON mapping text; $ignore skips unknown field names instead
     * of failing. A trailing limits array caps untrusted input. */
    public function fromJson(string $json, bool $ignore = false, ?array $limits = null): Message
    {
        $ffi = Lib::ffi();
        $opts = Lib::newSlot('pbw_json_parse_options');
        $opts->ignore_unknown_fields = $ignore ? 1 : 0;
        $out = Lib::slot('void*[1]');
        if ($limits === null) {
            $status = $ffi->pbw_json_parse_opts($this->desc, $json, strlen($json), FFI::addr($opts), FFI::addr($out[0]));
        } else {
            $status = $ffi->pbw_json_parse_limits($this->desc, $json, strlen($json), Lib::limitsPtr($limits), FFI::addr($opts), FFI::addr($out[0]));
        }

        if ($status !== 0) {
            throw new Error('fromJson: ' . Lib::errorText($status));
        }

        if ($out[0] === null) {
            throw new Error('fromJson: no message returned');
        }

        return Message::root($this, $out[0]);
    }

    /** Parses text format. A trailing limits array caps untrusted input. */
    public function fromText(string $text, ?array $limits = null): Message
    {
        $out = Lib::slot('void*[1]');
        if ($limits === null) {
            $status = Lib::ffi()->pbw_text_parse($this->desc, $text, strlen($text), FFI::addr($out[0]));
        } else {
            $status = Lib::ffi()->pbw_text_parse_limits($this->desc, $text, strlen($text), Lib::limitsPtr($limits), FFI::addr($out[0]));
        }

        if ($status !== 0) {
            throw new Error('fromText: ' . Lib::errorText($status));
        }

        if ($out[0] === null) {
            throw new Error('fromText: no message returned');
        }

        return Message::root($this, $out[0]);
    }

    /** Packs already-encoded bytes into a new google.protobuf.Any tree. */
    public function anyPack(string $typeUrl, string $payload): Message
    {
        $out = Lib::slot('void*[1]');
        $status = Lib::ffi()->pbw_any_pack($this->desc, $typeUrl, $payload, strlen($payload), FFI::addr($out[0]));

        if ($status !== 0) {
            throw new Error('anyPack: ' . Lib::errorText($status));
        }

        if ($out[0] === null) {
            throw new Error('anyPack: no message returned');
        }

        return Message::root($this, $out[0]);
    }

    /** A new Timestamp tree holding ms since the Unix epoch. */
    public function timestampFromMillis(int $ms): Message
    {
        $out = Lib::slot('void*[1]');
        $status = Lib::ffi()->pbw_timestamp_from_millis($this->desc, $ms, FFI::addr($out[0]));

        if ($status !== 0) {
            throw new Error('timestampFromMillis: ' . Lib::errorText($status));
        }

        if ($out[0] === null) {
            throw new Error('timestampFromMillis: no message returned');
        }

        return Message::root($this, $out[0]);
    }

    /** A new Duration tree holding ms. */
    public function durationFromMillis(int $ms): Message
    {
        $out = Lib::slot('void*[1]');
        $status = Lib::ffi()->pbw_duration_from_millis($this->desc, $ms, FFI::addr($out[0]));

        if ($status !== 0) {
            throw new Error('durationFromMillis: ' . Lib::errorText($status));
        }

        if ($out[0] === null) {
            throw new Error('durationFromMillis: no message returned');
        }

        return Message::root($this, $out[0]);
    }

    /** The message type of a MESSAGE/GROUP field, for nested access. */
    public function nestedType(Field $field): ?self
    {
        $desc = Lib::ffi()->pbw_field_message_type($field->cdata());

        return $desc === null ? null : new self($this->schema, $desc);
    }

    public function cdata(): CData
    {
        return $this->desc;
    }

    public function isMessageField(Field $field): bool
    {
        $type = $field->type();

        return $type === self::TYPE_MESSAGE || $type === self::TYPE_GROUP;
    }
}

/**
 * A resolved field handle.
 */
final class Field
{
    /** Keeps the Type (and through it the Schema) alive while the Field is in
     * use: the C field pointer points into the schema. */
    private Type $type;

    /** @internal */
    public function __construct(private CData $ptr, Type $type)
    {
        $this->type = $type;
    }

    public function name(): string
    {
        return (string) Lib::ffi()->pbw_field_name($this->ptr);
    }

    public function number(): int
    {
        return Lib::ffi()->pbw_field_number($this->ptr);
    }

    /** pbw_type: 5 = int32, 11 = message, ... */
    public function type(): int
    {
        return Lib::ffi()->pbw_field_type($this->ptr);
    }

    /** 3 = repeated. */
    public function label(): int
    {
        return Lib::ffi()->pbw_field_label($this->ptr);
    }

    public function cdata(): CData
    {
        return $this->ptr;
    }
}

/**
 * One dynamic message. A root owns its tree; a nested message borrows it.
 */
final class Message
{
    private ?CData $handle;
    private bool $owned;

    /** Build arena for context-backed roots; null for decoded trees and views. */
    private ?CData $ctx;

    /** Keeps the parent (and its tree) alive while a nested message is in use. */
    private ?Message $parent;

    /** @internal */
    private function __construct(
        private Type $type,
        CData $handle,
        bool $owned,
        ?Message $parent,
        ?CData $ctx,
    ) {
        $this->handle = $handle;
        $this->owned = $owned;
        $this->parent = $parent;
        $this->ctx = $ctx;
    }

    /** @internal */
    public static function root(Type $type, CData $handle, ?CData $ctx = null): self
    {
        return new self($type, $handle, true, null, $ctx);
    }

    /** @internal A nested message: the owner's tree stays alive through $parent. */
    public static function viewFor(Type $type, CData $handle, Message $parent): self
    {
        /* The build arena is inherited from the root: a value set on a nested
         * message has to live in the same arena as its parent (a context arena
         * can only own arena payloads, and a plain decode takes either). */
        return new self($type, $handle, false, $parent, $parent->ctx);
    }

    /* ---------------------------------------------------------------- setting */

    /** A value constructor that builds into the message's arena when it has one. */
    private function newUint(int $bits): mixed
    {
        if ($this->ctx !== null) {
            $this->cdata(); /* throws when an ancestor already closed */
            return Lib::ffi()->pbw_value_new_uint_ctx($this->ctx, $bits);
        }

        return Lib::ffi()->pbw_value_new_int($bits);
    }

    /** Bytes builder with explicit length, so embedded NULs survive. */
    private function newBytes(string $value): mixed
    {
        if ($this->ctx !== null) {
            $this->cdata(); /* throws when an ancestor already closed */
            return Lib::ffi()->pbw_value_new_bytes_ctx($this->ctx, $value, strlen($value));
        }

        return Lib::ffi()->pbw_value_new_bytes($value, strlen($value));
    }

    /** Any integer or enum field; the value is stored as raw 64-bit bits. */
    public function setInt(Field|string $field, int $value): self
    {
        return $this->store($this->resolve($field), $this->newUint($value));
    }

    public function setBool(Field|string $field, bool $value): self
    {
        return $this->store($this->resolve($field), $this->newUint($value ? 1 : 0));
    }

    public function setFloat(Field|string $field, float $value): self
    {
        $bits = unpack('L', pack('f', $value))[1];

        return $this->store($this->resolve($field), $this->newUint($bits));
    }

    public function setDouble(Field|string $field, float $value): self
    {
        $bits = unpack('Q', pack('d', $value))[1];

        return $this->store($this->resolve($field), $this->newUint($bits));
    }

    public function setString(Field|string $field, string $value): self
    {
        return $this->store($this->resolve($field), $this->newBytes($value));
    }

    public function setBytes(Field|string $field, string $value): self
    {
        return $this->store($this->resolve($field), $this->newBytes($value));
    }

    /**
     * Nested message for a MESSAGE field, created in place: the child borrows
     * this message, so it is not freed on its own.
     */
    public function setMessage(Field|string $field): self
    {
        $resolved = $this->resolve($field);
        $ptr = Lib::ffi()->pbw_message_set_message($this->cdata(), $resolved->number());

        if ($ptr === null) {
            throw new Error("set {$resolved->name()}: not a message field");
        }

        $type = $this->type->nestedType($resolved);

        if ($type === null) {
            throw new Error("set {$resolved->name()}: no nested type");
        }

        return self::viewFor($type, $ptr, $this);
    }

    /** Appends to a repeated field or replaces a singular one. */
    private function store(Field $field, mixed $value): self
    {
        if ($value === null) {
            throw new Error("set {$field->name()}: out of memory");
        }

        $status = Lib::ffi()->pbw_message_set_field($this->cdata(), $field->cdata(), $value);

        if ($status !== 0) {
            /* A rejected value stays owned by the caller. */
            Lib::ffi()->pbw_value_free($value);
            throw new Error("set {$field->name()}: " . Lib::errorText($status));
        }

        return $this;
    }

    public function clear(Field|string $field): void
    {
        Lib::ffi()->pbw_message_clear($this->cdata(), $this->resolve($field)->number());
    }

    /* ---------------------------------------------------------------- reading */

    private function value(Field $field): ?CData
    {
        return Lib::ffi()->pbw_message_get_field($this->cdata(), $field->cdata());
    }

    public function has(Field|string $field): bool
    {
        return $this->value($this->resolve($field)) !== null;
    }

    public function getInt(Field|string $field): int
    {
        $value = $this->value($this->resolve($field));

        return $value === null ? 0 : Lib::ffi()->pbw_value_get_int($value);
    }

    public function getBool(Field|string $field): bool
    {
        $value = $this->value($this->resolve($field));

        return $value !== null && Lib::ffi()->pbw_value_get_bool($value) !== 0;
    }

    public function getFloat(Field|string $field): float
    {
        $value = $this->value($this->resolve($field));

        return $value === null ? 0.0 : Lib::ffi()->pbw_value_get_float($value);
    }

    public function getDouble(Field|string $field): float
    {
        $value = $this->value($this->resolve($field));

        return $value === null ? 0.0 : Lib::ffi()->pbw_value_get_double($value);
    }

    public function getString(Field|string $field): ?string
    {
        $value = $this->value($this->resolve($field));

        if ($value === null) {
            return null;
        }

        /* Length-based: values with embedded NUL bytes round-trip intact. */
        $len = Lib::slot('size_t');
        $ptr = Lib::ffi()->pbw_value_get_bytes($value, FFI::addr($len));

        return $ptr === null ? null : FFI::string($ptr, $len->cdata);
    }

    public function getBytes(Field|string $field): ?string
    {
        $value = $this->value($this->resolve($field));

        if ($value === null) {
            return null;
        }

        $len = Lib::slot('size_t');
        $ptr = Lib::ffi()->pbw_value_get_bytes($value, FFI::addr($len));

        return $ptr === null ? null : FFI::string($ptr, $len->cdata);
    }

    public function getMessage(Field|string $field): ?self
    {
        $resolved = $this->resolve($field);
        $value = $this->value($resolved);

        if ($value === null) {
            return null;
        }

        $ptr = Lib::ffi()->pbw_value_get_message($value);
        $type = $this->type->nestedType($resolved);

        if ($ptr === null || $type === null) {
            return null;
        }

        return self::viewFor($type, $ptr, $this);
    }

    /** Items in a repeated field, or 0 when the field is absent. */
    public function count(Field|string $field): int
    {
        $value = $this->value($this->resolve($field));

        return $value === null ? 0 : Lib::ffi()->pbw_value_count($value);
    }

    /** One item of a repeated field. */
    public function at(Field|string $field, int $index): ?Value
    {
        $value = $this->value($this->resolve($field));

        if ($value === null) {
            return null;
        }

        $item = Lib::ffi()->pbw_value_at($value, $index);

        return $item === null ? null : new Value($item, $this, $this->type->nestedType($this->resolve($field)));
    }

    /* ------------------------------------------------------------------- wire */

    /** Encoded bytes (one allocation by the core, one copy into the PHP string). */
    public function encode(): string
    {
        $ffi = Lib::ffi();
        $out = Lib::slot('void*[1]');
        $len = Lib::slot('size_t');
        $status = $ffi->pbw_encode($this->cdata(), FFI::addr($out[0]), FFI::addr($len));

        if ($status !== 0) {
            throw new Error('encode: ' . Lib::errorText($status));
        }

        $bytes = FFI::string($out[0], $len->cdata);
        $ffi->pbw_free($out[0]);

        return $bytes;
    }

    /** Deterministic bytes: map entries sorted by key, so equal messages
     * encode to equal bytes regardless of insertion order. */
    public function encodeDeterministic(): string
    {
        $ffi = Lib::ffi();
        $out = Lib::slot('void*[1]');
        $len = Lib::slot('size_t');
        $status = $ffi->pbw_encode_deterministic($this->cdata(), FFI::addr($out[0]), FFI::addr($len));

        if ($status !== 0) {
            throw new Error('encodeDeterministic: ' . Lib::errorText($status));
        }

        $bytes = FFI::string($out[0], $len->cdata);
        $ffi->pbw_free($out[0]);

        return $bytes;
    }

    /** Exact byte count encode() would write for this message. */
    public function encodedSize(): int
    {
        $len = Lib::slot('size_t');
        $status = Lib::ffi()->pbw_message_encoded_size($this->cdata(), FFI::addr($len));

        if ($status !== 0) {
            throw new Error('encodedSize: ' . Lib::errorText($status));
        }

        return $len->cdata;
    }

    /** Encodes into a $cap-byte buffer: the cap is the hard limit, so a
     * short buffer fails instead of growing. */
    public function encodeTo(int $cap): string
    {
        if ($cap < 1) {
            throw new Error('encodeTo: cap must be positive');
        }
        $buf = str_repeat("\0", $cap);
        $len = Lib::slot('size_t');
        $status = Lib::ffi()->pbw_encode_to($this->cdata(), $buf, $cap, FFI::addr($len));

        if ($status !== 0) {
            throw new Error('encodeTo: ' . Lib::errorText($status));
        }

        return substr($buf, 0, $len->cdata);
    }

    /** One length-delimited frame (varint length + message). */
    public function encodeDelimited(): string
    {
        $ffi = Lib::ffi();
        $out = Lib::slot('void*[1]');
        $len = Lib::slot('size_t');
        $status = $ffi->pbw_encode_delimited($this->cdata(), FFI::addr($out[0]), FFI::addr($len));

        if ($status !== 0) {
            throw new Error('encodeDelimited: ' . Lib::errorText($status));
        }

        $bytes = FFI::string($out[0], $len->cdata);
        $ffi->pbw_free($out[0]);

        return $bytes;
    }

    /** One length-delimited frame into a $cap-byte buffer. */
    public function encodeDelimitedTo(int $cap): string
    {
        if ($cap < 1) {
            throw new Error('encodeDelimitedTo: cap must be positive');
        }
        $buf = str_repeat("\0", $cap);
        $len = Lib::slot('size_t');
        $status = Lib::ffi()->pbw_encode_delimited_to($this->cdata(), $buf, $cap, FFI::addr($len));

        if ($status !== 0) {
            throw new Error('encodeDelimitedTo: ' . Lib::errorText($status));
        }

        return substr($buf, 0, $len->cdata);
    }

    /** JSON mapping text of this message. */
    public function toJson(): string
    {
        $ffi = Lib::ffi();
        $out = Lib::slot('void*[1]');
        $len = Lib::slot('size_t');
        $status = $ffi->pbw_json_print($this->cdata(), FFI::addr($out[0]), FFI::addr($len));

        if ($status !== 0) {
            throw new Error('toJson: ' . Lib::errorText($status));
        }

        $text = FFI::string($out[0], $len->cdata);
        $ffi->pbw_free($out[0]);

        return $text;
    }

    /** Protobuf text format; $printUnknown drops unknown fields when false. */
    public function toText(bool $printUnknown = true): string
    {
        $ffi = Lib::ffi();
        $out = Lib::slot('void*[1]');
        $len = Lib::slot('size_t');
        $status = $ffi->pbw_text_print_opts($this->cdata(), $printUnknown ? 1 : 0, FFI::addr($out[0]), FFI::addr($len));

        if ($status !== 0) {
            throw new Error('toText: ' . Lib::errorText($status));
        }

        $text = FFI::string($out[0], $len->cdata);
        $ffi->pbw_free($out[0]);

        return $text;
    }

    /** type_url of an Any message, or null when absent. */
    public function anyTypeUrl(): ?string
    {
        $url = Lib::ffi()->pbw_any_type_url($this->cdata());

        return $url === null ? null : (string) $url;
    }

    /** value bytes of an Any message, or null when absent. */
    public function anyPayload(): ?string
    {
        $len = Lib::slot('size_t');
        $ptr = Lib::ffi()->pbw_any_payload($this->cdata(), FFI::addr($len));

        return $ptr === null ? null : FFI::string($ptr, $len->cdata);
    }

    /** Milliseconds since the Unix epoch of a Timestamp message. */
    public function timestampMillis(): int
    {
        $ms = Lib::slot('long long');
        $status = Lib::ffi()->pbw_timestamp_to_millis($this->cdata(), FFI::addr($ms));

        if ($status !== 0) {
            throw new Error('timestampMillis: ' . Lib::errorText($status));
        }

        return $ms->cdata;
    }

    /** Milliseconds of a Duration message. */
    public function durationMillis(): int
    {
        $ms = Lib::slot('long long');
        $status = Lib::ffi()->pbw_duration_to_millis($this->cdata(), FFI::addr($ms));

        if ($status !== 0) {
            throw new Error('durationMillis: ' . Lib::errorText($status));
        }

        return $ms->cdata;
    }

    /** Path strings of a FieldMask message. */
    public function fieldmaskPaths(): array
    {
        $ffi = Lib::ffi();
        $count = $ffi->pbw_fieldmask_path_count($this->cdata());
        $paths = [];
        for ($i = 0; $i < $count; $i++) {
            $paths[] = (string) $ffi->pbw_fieldmask_path_at($this->cdata(), $i);
        }

        return $paths;
    }

    public function cdata(): CData
    {
        /* A nested view lives inside its root's arena: closing any ancestor
         * frees the memory the child points into, so refuse before touching
         * the handle. */
        for ($p = $this->parent; $p !== null; $p = $p->parent) {
            if ($p->handle === null) {
                throw new Error('parent message is closed');
            }
        }
        if ($this->handle === null) {
            throw new Error('message is closed');
        }

        return $this->handle;
    }

    public function close(): void
    {
        if ($this->parent === null && $this->ctx !== null) {
            /* Context-built root: freeing the context releases the whole
             * tree at once. A nested view inherits the context to build
             * into, never to free: the root owns it. */
            Lib::ffi()->pbw_ctx_free($this->ctx);
            $this->ctx = null;
            $this->handle = null;
        } elseif ($this->parent === null && $this->handle !== null && $this->owned) {
            Lib::ffi()->pbw_message_free($this->handle);
            $this->handle = null;
        } else {
            /* A borrowed view: close only itself. Keep the ancestor link so
             * cdata() can still tell that the parent was closed. */
            $this->handle = null;
        }
    }

    public function __destruct()
    {
        $this->close();
    }

    private function resolve(Field|string $field): Field
    {
        return is_string($field) ? $this->type->field($field) : $field;
    }
}

/**
 * A field value; only repeated items need it, hence the tiny surface.
 */
final class Value
{
    /** @internal */
    public function __construct(
        private CData $ptr,
        private Message $owner,
        private ?Type $type,
    ) {
    }

    /** The value pointer, after proving its owner is still live. */
    private function native(): CData
    {
        $this->owner->cdata();

        return $this->ptr;
    }

    public function asInt(): int
    {
        return Lib::ffi()->pbw_value_get_int($this->native());
    }

    public function asBool(): bool
    {
        return Lib::ffi()->pbw_value_get_bool($this->native()) !== 0;
    }

    public function asFloat(): float
    {
        return Lib::ffi()->pbw_value_get_float($this->native());
    }

    public function asDouble(): float
    {
        return Lib::ffi()->pbw_value_get_double($this->native());
    }

    public function asString(): ?string
    {
        /* Length-based: values with embedded NUL bytes round-trip intact. */
        $len = Lib::slot('size_t');
        $ptr = Lib::ffi()->pbw_value_get_bytes($this->native(), FFI::addr($len));

        return $ptr === null ? null : FFI::string($ptr, $len->cdata);
    }

    public function asBytes(): ?string
    {
        $len = Lib::slot('size_t');
        $ptr = Lib::ffi()->pbw_value_get_bytes($this->native(), FFI::addr($len));

        return $ptr === null ? null : FFI::string($ptr, $len->cdata);
    }

    public function asMessage(): ?Message
    {
        $ptr = Lib::ffi()->pbw_value_get_message($this->native());

        if ($ptr === null || $this->type === null) {
            return null;
        }

        return Message::viewFor($this->type, $ptr, $this->owner);
    }
}

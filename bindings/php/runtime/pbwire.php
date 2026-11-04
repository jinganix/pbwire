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
        int pbw_decode(void *desc, const void *buf, size_t len, void **out);
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
}

/**
 * A loaded FileDescriptorSet: the schema every MessageType comes from.
 */
final class Schema
{
    private ?CData $handle;

    private function __construct(CData $handle)
    {
        $this->handle = $handle;
    }

    public static function fromFile(string $path): self
    {
        $bytes = @file_get_contents($path);

        if ($bytes === false) {
            throw new Error("pbwire: cannot read $path");
        }

        return self::fromBytes($bytes);
    }

    /** Load descriptor-set bytes; use --include_imports so every type resolves. */
    public static function fromBytes(string $bytes): self
    {
        $ffi = Lib::ffi();
        $out = Lib::slot('void*[1]');
        $status = $ffi->pbw_descriptor_set_load($bytes, strlen($bytes), FFI::addr($out[0]));
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

    /** Looks up a message by fully qualified name, e.g. "structtest.Scalars3". */
    public function message(string $fullName): Type
    {
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
        if ($this->handle === null) {
            throw new \LogicException('schema is closed');
        }

        return $this->handle;
    }

    public function close(): void
    {
        if ($this->handle !== null) {
            Lib::ffi()->pbw_schema_free($this->handle);
            $this->handle = null;
        }
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
        $this->schema = $schema;
        $this->desc = $desc;
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

            $this->fields[$name] = new Field($ptr);
        }

        return $this->fields[$name];
    }

    public function fieldByNumber(int $number): Field
    {
        $ptr = Lib::ffi()->pbw_descriptor_find_field($this->desc, $number);

        if ($ptr === null) {
            throw new Error("{$this->name()}: no field $number");
        }

        return new Field($ptr);
    }

    public function newMessage(): Message
    {
        $ptr = Lib::ffi()->pbw_message_new($this->desc);

        if ($ptr === null) {
            throw new Error('pbw_message_new: out of memory');
        }

        return Message::root($this, $ptr);
    }

    /** Decodes wire bytes; unknown fields are preserved and re-encoded. */
    public function decode(string $wire): Message
    {
        $out = Lib::slot('void*[1]');
        $status = Lib::ffi()->pbw_decode($this->desc, $wire, strlen($wire), FFI::addr($out[0]));

        if ($status !== 0) {
            throw new Error('decode: ' . Lib::errorText($status));
        }

        if ($out[0] === null) {
            throw new Error('decode: no message returned');
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
    /** @internal */
    public function __construct(private CData $ptr)
    {
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

    /** Keeps the parent (and its tree) alive while a nested message is in use. */
    private ?Message $parent;

    /** @internal */
    private function __construct(
        private Type $type,
        CData $handle,
        bool $owned,
        ?Message $parent,
    ) {
        $this->handle = $handle;
        $this->owned = $owned;
        $this->parent = $parent;
    }

    /** @internal */
    public static function root(Type $type, CData $handle): self
    {
        return new self($type, $handle, true, null);
    }

    /** @internal A nested message: the owner's tree stays alive through $parent. */
    public static function viewFor(Type $type, CData $handle, Message $parent): self
    {
        return new self($type, $handle, false, $parent);
    }

    /* ---------------------------------------------------------------- setting */

    /** Any integer or enum field; the value is stored as raw 64-bit bits. */
    public function setInt(Field|string $field, int $value): self
    {
        return $this->store($this->resolve($field), Lib::ffi()->pbw_value_new_int($value));
    }

    public function setBool(Field|string $field, bool $value): self
    {
        return $this->store($this->resolve($field), Lib::ffi()->pbw_value_new_bool($value ? 1 : 0));
    }

    public function setFloat(Field|string $field, float $value): self
    {
        return $this->store($this->resolve($field), Lib::ffi()->pbw_value_new_float($value));
    }

    public function setDouble(Field|string $field, float $value): self
    {
        return $this->store($this->resolve($field), Lib::ffi()->pbw_value_new_double($value));
    }

    public function setString(Field|string $field, string $value): self
    {
        return $this->store($this->resolve($field), Lib::ffi()->pbw_value_new_string($value));
    }

    public function setBytes(Field|string $field, string $value): self
    {
        return $this->store($this->resolve($field), Lib::ffi()->pbw_value_new_bytes($value, strlen($value)));
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

        return Lib::ffi()->pbw_value_get_string($value);
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

    public function cdata(): CData
    {
        if ($this->handle === null) {
            throw new \LogicException('message is closed');
        }

        return $this->handle;
    }

    public function close(): void
    {
        if ($this->handle !== null && $this->owned) {
            Lib::ffi()->pbw_message_free($this->handle);
        }

        $this->handle = null;
        $this->parent = null;
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

    public function asInt(): int
    {
        return Lib::ffi()->pbw_value_get_int($this->ptr);
    }

    public function asBool(): bool
    {
        return Lib::ffi()->pbw_value_get_bool($this->ptr) !== 0;
    }

    public function asFloat(): float
    {
        return Lib::ffi()->pbw_value_get_float($this->ptr);
    }

    public function asDouble(): float
    {
        return Lib::ffi()->pbw_value_get_double($this->ptr);
    }

    public function asString(): ?string
    {
        return Lib::ffi()->pbw_value_get_string($this->ptr);
    }

    public function asBytes(): ?string
    {
        $len = Lib::slot('size_t');
        $ptr = Lib::ffi()->pbw_value_get_bytes($this->ptr, FFI::addr($len));

        return $ptr === null ? null : FFI::string($ptr, $len->cdata);
    }

    public function asMessage(): ?Message
    {
        $ptr = Lib::ffi()->pbw_value_get_message($this->ptr);

        if ($ptr === null || $this->type === null) {
            return null;
        }

        return Message::viewFor($this->type, $ptr, $this->owner);
    }
}

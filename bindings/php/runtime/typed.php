<?php

declare(strict_types=1);

/*
 * Typed-model support: the pieces a protoc-gen-pbwire-typed class needs to
 * look like the official google/protobuf runtime (RepeatedField, GPBUtil
 * range checks) while every byte still goes through the dynamic API.
 *
 * Load after runtime/pbwire.php (or with the C extension, which declares the
 * same Pbwire\Schema/Message classes): require this file, then the generated
 * model. No FFI here, so it works with both.
 */

namespace Pbwire;

/** Integer/float/string checks mirroring Google\Protobuf\Internal\GPBUtil. */
final class TypedUtil
{
    private function __construct()
    {
    }

    public static function checkInt32(int $v): int
    {
        if ($v < -2147483648 || $v > 2147483647) {
            throw new \InvalidArgumentException("out of int32 range: $v");
        }

        return $v;
    }

    public static function checkUint32(int $v): int
    {
        if ($v < 0 || $v > 4294967295) {
            throw new \InvalidArgumentException("out of uint32 range: $v");
        }

        return $v;
    }

    /** int64 as raw 64-bit bits: an int passes through, a decimal string wraps. */
    public static function checkInt64(int|string $v): int
    {
        if (is_int($v)) {
            return $v;
        }

        return self::parseInt64($v, false);
    }

    /** uint64 as raw 64-bit bits: -1 is 18446744073709551615. */
    public static function checkUint64(int|string $v): int
    {
        if (is_int($v)) {
            return $v;
        }

        return self::parseInt64($v, true);
    }

    public static function checkFloat(int|float $v): float
    {
        return unpack('f', pack('f', (float) $v))[1];
    }

    public static function checkDouble(int|float $v): float
    {
        return (float) $v;
    }

    public static function checkBool(bool $v): bool
    {
        return $v;
    }

    /** A protobuf `string` must be valid UTF-8, like the official runtime. */
    public static function checkString(string $v): string
    {
        if (!preg_match('//u', $v)) {
            throw new \InvalidArgumentException('string field must be valid UTF-8');
        }

        return $v;
    }

    public static function checkBytes(string $v): string
    {
        return $v;
    }

    /**
     * Open enums (proto3) accept any int32; a closed enum also needs its
     * value list, and anything outside is rejected instead of rounding.
     */
    public static function checkEnum(int $v, ?array $allowed): int
    {
        self::checkInt32($v);

        if ($allowed !== null && !in_array($v, $allowed, true)) {
            throw new \InvalidArgumentException("unknown closed-enum value: $v");
        }

        return $v;
    }

    /** A decoded 32-bit signed field arrives as unsigned 64-bit bits. */
    public static function toSigned32(int $v): int
    {
        return ($v & 0xFFFFFFFF) - (($v & 0x80000000) << 1);
    }

    /**
     * Decimal text to raw 64-bit bits, pure PHP (no bcmath): values inside
     * the signed range cast directly, larger magnitudes accumulate in two
     * 32-bit halves so 2^64-1 keeps its bits instead of saturating.
     */
    private static function parseInt64(string $s, bool $unsigned): int
    {
        $t = trim($s);

        if (!preg_match('/^-?\d+$/', $t)) {
            throw new \InvalidArgumentException("not an integer: $s");
        }

        $neg = $t[0] === '-';
        $digits = ltrim($neg ? substr($t, 1) : $t, '0');

        if ($digits === '') {
            return 0;
        }

        // strcmp is only numeric for equal-length digit strings.
        $len = strlen($digits);

        if ($neg) {
            if ($digits === '9223372036854775808') {
                return PHP_INT_MIN;
            }

            if ($len < 19 || ($len === 19 && strcmp($digits, '9223372036854775808') < 0)) {
                return -((int) $digits);
            }

            throw new \InvalidArgumentException("out of int64 range: $s");
        }

        if ($len < 19 || ($len === 19 && strcmp($digits, '9223372036854775807') <= 0)) {
            return (int) $digits;
        }

        if (!$unsigned || $len > 20 || ($len === 20 && strcmp($digits, '18446744073709551615') > 0)) {
            throw new \InvalidArgumentException("out of uint64 range: $s");
        }

        $hi = 0;
        $lo = 0;

        for ($i = 0, $n = strlen($digits); $i < $n; $i++) {
            $d = ord($digits[$i]) - 48;
            $t0 = $lo * 10 + $d;
            $lo = $t0 & 0xFFFFFFFF;
            $t1 = $hi * 10 + ($t0 >> 32);
            $hi = $t1 & 0xFFFFFFFF;
        }

        return ($hi << 32) | $lo;
    }
}

/**
 * Repeated-field container mirroring Google\Protobuf\RepeatedField: array
 * access with append, foreach, count. Elements are validated on the way in,
 * so a bad element throws instead of corrupting the next encode.
 */
final class RepeatedField implements \ArrayAccess, \IteratorAggregate, \Countable
{
    /** @var list<mixed> */
    private array $items = [];

    /**
     * @param 'int32'|'uint32'|'int64'|'uint64'|'float'|'double'|'bool'|'string'|'bytes'|'enum'|'message' $type
     */
    public function __construct(
        private string $type,
        array $items = [],
        private ?string $class = null,
        private ?array $allowed = null,
    ) {
        foreach ($items as $v) {
            $this->items[] = $this->check($v);
        }
    }

    private function check(mixed $v): mixed
    {
        return match ($this->type) {
            'int32' => TypedUtil::checkInt32($v),
            'uint32' => TypedUtil::checkUint32($v),
            'int64' => TypedUtil::checkInt64($v),
            'uint64' => TypedUtil::checkUint64($v),
            'float' => TypedUtil::checkFloat($v),
            'double' => TypedUtil::checkDouble($v),
            'bool' => TypedUtil::checkBool($v),
            'string' => TypedUtil::checkString($v),
            'bytes' => TypedUtil::checkBytes($v),
            'enum' => TypedUtil::checkEnum($v, $this->allowed),
            'message' => $this->checkMessage($v),
            default => throw new \InvalidArgumentException("bad RepeatedField type {$this->type}"),
        };
    }

    private function checkMessage(mixed $v): mixed
    {
        if ($this->class !== null && $v instanceof $this->class) {
            return $v;
        }

        throw new \InvalidArgumentException('repeated message element must be ' . $this->class);
    }

    #[\ReturnTypeWillChange]
    public function offsetGet($offset)
    {
        return $this->items[$offset];
    }

    #[\ReturnTypeWillChange]
    public function offsetSet($offset, $value)
    {
        $value = $this->check($value);

        if ($offset === null) {
            $this->items[] = $value;

            return;
        }

        if (!is_int($offset) || $offset < 0 || $offset >= count($this->items)) {
            throw new \OutOfRangeException("cannot set element at index $offset");
        }

        $this->items[$offset] = $value;
    }

    #[\ReturnTypeWillChange]
    public function offsetUnset($offset)
    {
        throw new \LogicException('repeated elements cannot be unset; set a new array instead');
    }

    public function offsetExists($offset): bool
    {
        return isset($this->items[$offset]);
    }

    public function getIterator(): \Traversable
    {
        return new \ArrayIterator($this->items);
    }

    public function count(): int
    {
        return count($this->items);
    }
}

/*
 * Typed-conformance check for the Kotlin binding: build matrix3.proto's
 * Scalars3 through the generated data classes, encode, require the protoc
 * reference bytes (matrix3.bin), then decode those bytes back and check every
 * field.
 *
 *   java --enable-native-access=ALL-UNNAMED -Dpbwire.library=... \
 *        -cp out/pbwire.jar:../../java/out TypedTestKt matrix3.desc
 */

import java.nio.file.Files
import java.nio.file.Path

private var checks = 0

private fun check(ok: Boolean, what: String) {
    if (!ok) {
        System.err.println("FAIL $what")
        System.exit(1)
    }
    checks++
}

private fun build(): Structtest_Scalars3 = Structtest_Scalars3(
    i32 = Int.MIN_VALUE,
    i64 = Long.MAX_VALUE,
    u32 = -1,                              // 4294967295 as raw 32-bit bits
    u64 = -1L,                             // 18446744073709551615 as raw bits
    s32 = Int.MIN_VALUE,
    s64 = Long.MIN_VALUE,
    fx32 = 0xdeadbeef.toInt(),
    fx64 = 72623859790382856L,
    sfx32 = Int.MIN_VALUE,
    sfx64 = Long.MIN_VALUE,
    f32 = Float.POSITIVE_INFINITY,
    f64 = -123456.789,
    flag = true,
    str = "hello",
    color = Structtest_Color3.COLOR3_GREEN,
    inner = Structtest_Inner3(name = "kid", value = 42),
    nums = mutableListOf(1, -5, 300),
    tags = mutableListOf("a", "bb"),
    inners = mutableListOf(
        Structtest_Inner3(name = "x", value = 1),
        Structtest_Inner3(name = "y", value = -2),
    ),
    hasOptI32 = true,
    optI32 = -7,
    hasOptStr = true,
    optStr = "opt",
    flags = mutableListOf(true, false, true),
)

private fun verify(m: Structtest_Scalars3) {
    check(m.i32 == Int.MIN_VALUE, "i32")
    check(m.i64 == Long.MAX_VALUE, "i64")
    check(m.u32 == -1, "u32")
    check(m.u64 == -1L, "u64")
    check(m.s32 == Int.MIN_VALUE, "s32")
    check(m.s64 == Long.MIN_VALUE, "s64")
    check(m.fx32 == 0xdeadbeef.toInt(), "fx32")
    check(m.fx64 == 72623859790382856L, "fx64")
    check(m.sfx32 == Int.MIN_VALUE, "sfx32")
    check(m.sfx64 == Long.MIN_VALUE, "sfx64")
    check(m.f32 == Float.POSITIVE_INFINITY, "f32")
    check(m.f64 == -123456.789, "f64")
    check(m.flag, "flag")
    check(m.str == "hello", "str")
    check(m.color == Structtest_Color3.COLOR3_GREEN, "color")
    check(m.inner?.name == "kid" && m.inner?.value == 42, "inner")
    check(m.nums == mutableListOf(1, -5, 300), "nums")
    check(m.tags == mutableListOf("a", "bb"), "tags")
    check(m.inners.size == 2 && m.inners[1].value == -2, "inners")
    check(m.hasOptI32 && m.optI32 == -7, "opt_i32")
    check(m.hasOptStr && m.optStr == "opt", "opt_str")
    check(m.flags == mutableListOf(true, false, true), "flags")
}

fun main(args: Array<String>) {
    val desc = Path.of(args.firstOrNull() ?: "bindings/c/tests/data/matrix3.desc")
    val reference = Files.readAllBytes(desc.parent.resolve("matrix3.bin"))

    Schema.fromFile(desc.toString()).use { schema ->
        val wire = build().encode(schema)
        check(wire.contentEquals(reference), "typed encode equals the protoc reference bytes")
        verify(Structtest_Scalars3.decode(schema, reference))
        check(Structtest_Scalars3().encode(schema).isEmpty(), "empty typed message is empty")

        val zero = Structtest_Scalars3(hasOptI32 = true, optI32 = 0)
        val zeroBack = Structtest_Scalars3.decode(schema, zero.encode(schema))
        check(zeroBack.hasOptI32 && zeroBack.optI32 == 0, "explicit zero presence round-trips")

        val nested = Structtest_Scalars3(inner = Structtest_Inner3(name = "kid", value = 42))
        check(Structtest_Scalars3.decode(schema, nested.encode(schema)).inner?.value == 42, "nested typed message")

        val innerWire = Structtest_Inner3(name = "kid").encode(schema)
        check(Structtest_Inner3.decode(schema, innerWire).name == "kid", "inner encodes standalone")
    }

    println("TypedTest: $checks checks passed")
}

/*
 * Self-check against the pinned protoc bytes: build structtest.Scalars3 from
 * bindings/c/tests/data/matrix3.txt, encode, compare with matrix3.bin (the same
 * oracle the C binding uses), then decode it back and check the values.
 *
 *   java --enable-native-access=ALL-UNNAMED -Dpbwire.library=... \
 *        -cp out/pbwire.jar:../../java/out TestPbwireKt matrix3.desc
 */
import java.nio.file.Files
import java.nio.file.Path

private var checks = 0

private fun check(ok: Boolean, what: String) {
    checks++
    if (!ok) {
        System.err.println("FAIL $what")
        System.exit(1)
    }
}

private fun fill(ty: MessageType): Message = ty.build {
    set("i32", Int.MIN_VALUE.toLong())
    set("i64", Long.MAX_VALUE)
    set("u32", 0xffffffffL)
    set("u64", -1L) // u64::MAX bits
    set("s32", Int.MIN_VALUE.toLong())
    set("s64", Long.MIN_VALUE)
    set("fx32", 0xdeadbeefL)
    set("fx64", 72623859790382856L)
    set("sfx32", Int.MIN_VALUE.toLong())
    set("sfx64", Long.MIN_VALUE)
    set("f32", Double.POSITIVE_INFINITY)
    set("f64", -123456.789)
    set("flag", true)
    set("str", "hello")
    set("color", 2L)
    set("opt_i32", -7L)
    set("opt_str", "opt")
    for (value in longArrayOf(1, -5, 300)) {
        set("nums", value)
    }
    for (tag in listOf("a", "bb")) {
        set("tags", tag)
    }
    for ((name, value) in listOf("x" to 1L, "y" to -2L)) {
        message("inners") {
            set("name", name)
            set("value", value)
        }
    }
    for (flag in booleanArrayOf(true, false, true)) {
        set("flags", flag)
    }
    message("inner") {
        set("name", "kid")
        set("value", 42L)
    }
}

fun main(args: Array<String>) {
    val desc = args.getOrNull(0) ?: "../c/tests/data/matrix3.desc"
    val oracle = Files.readAllBytes(Path.of(desc).resolveSibling("matrix3.bin"))

    schema(desc).use { schema ->
        val ty = schema.message("structtest.Scalars3")
        val innerTy = schema.message("structtest.Inner3")

        fill(ty).use { msg ->
            val wire = msg.encode()
            check(wire.contentEquals(oracle), "encoded bytes differ from protoc (${wire.size} vs ${oracle.size})")
            println("OK   encode matches protoc, ${oracle.size} bytes")
        }

        ty.parse(oracle).use { decoded ->
            check(decoded.getLong("i32") == Int.MIN_VALUE.toLong(), "i32")
            check(decoded.getBool("flag"), "flag")
            check(decoded.getString("str") == "hello", "str")
            check(decoded.getDouble("f64") == -123456.789, "f64")
            check(decoded.getLong("color") == 2L, "color")
            check(decoded.has("opt_str") && ty.newMessage().use { !it.has("i32") }, "presence")
            check(decoded.getList("nums") == listOf(1L, -5L, 300L), "nums")
            check(decoded.getList("tags") == listOf("a", "bb"), "tags")
            check(decoded.getList("flags") == listOf(true, false, true), "flags")
            check(decoded.getMessage("inner")!!.getString("name") == "kid", "inner.name")
            val inners = decoded.getList("inners") as List<*>
            check(inners.size == 2, "inners")
            check(decoded.encode().contentEquals(oracle), "decode -> encode is byte-exact")
            println("OK   decode round trip, repeated and nested fields")
        }

        /* A field the schema does not declare (99, varint 7) survives re-encoding. */
        val unknown = byteArrayOf(0x98.toByte(), 0x06, 0x07)
        innerTy.parse(unknown).use { decoded ->
            check(decoded.encode().contentEquals(unknown), "unknown field preserved")
        }

        /* Bad input is an error, not a crash. */
        check(
            runCatching { ty.parse(byteArrayOf(0x80.toByte())) }.isFailure,
            "truncated tag accepted",
        )
        check(runCatching { schema.message("structtest.Nope") }.isFailure, "unknown message accepted")
    }

    /*
     * The cases a binding can get wrong even when the core is right: payloads
     * with embedded NULs / invalid UTF-8, non-ASCII text, a negative enum, an
     * empty repeated element, proto3 optional presence with an empty value, and
     * oneof switching. tests/data/edge.bin is protoc --encode.
     */
    val root = Path.of("").toAbsolutePath().resolve("../..").normalize()
    val edgeOracle = Files.readAllBytes(root.resolve("tests/data/edge.bin"))
    schema(root.resolve("tests/data/edge.desc").toString()).use { edgeSchema ->
        val edgeTy = edgeSchema.message("edge.Edge")
        val edgeInner = edgeSchema.message("edge.Inner")

        val edge = edgeTy.build {
            set("raw", byteArrayOf(0x00, 0x01, 0xff.toByte()))
            set("text", "héllo ☃")
            set("sign", -3L)
            set("maybe", "")
            set("pick_num", 5L) // oneof: replaced below
            set("pick_str", "one")
            for (tag in listOf("a", "", "b")) {
                set("tags", tag)
            }
            set("blobs", byteArrayOf(0x00))
            set("blobs", byteArrayOf())
            message("inner") {
                set("tag", "kid")
                set("n", -1L)
            }
        }
        edge.use {
            check(it.encode().contentEquals(edgeOracle), "edge payload differs from protoc")
        }

        edgeTy.parse(edgeOracle).use { decoded ->
            check(decoded.getBytes("raw")!!.contentEquals(byteArrayOf(0x00, 0x01, 0xff.toByte())), "bytes with NUL and 0xff")
            check(decoded.getString("text") == "héllo ☃", "non-ASCII string")
            check(decoded.getLong("sign") == -3L, "negative enum")
            check(decoded.getList("tags") == listOf("a", "", "b"), "empty repeated element")
            val blobs = decoded.getList("blobs") as List<*>
            check((blobs[0] as ByteArray).contentEquals(byteArrayOf(0x00)), "bytes element with NUL")
            check((blobs[1] as ByteArray).isEmpty(), "empty bytes element")
            check(decoded.getMessage("inner")!!.getLong("n") == -1L, "nested negative")
            check(decoded.has("maybe") && decoded.getString("maybe") == "", "optional present and empty")
            check(decoded.has("pick_str") && !decoded.has("pick_num"), "oneof keeps only the last member")
            check(decoded.encode().contentEquals(edgeOracle), "edge decode -> encode is byte-exact")
        }
        println("OK   edge payload: bytes, UTF-8, negative enum, empty elements, oneof")

        /* A default message is zero bytes both ways. */
        edgeTy.newMessage().use { check(it.encode().isEmpty(), "empty message encodes to zero bytes") }
        edgeTy.decode(ByteArray(0)).use { empty ->
            check(empty.encode().isEmpty(), "empty wire decodes and re-encodes to zero bytes")
            check(!empty.has("text"), "empty message has no fields")
        }

        /* A wrong value kind is refused, not written. */
        check(
            runCatching { edgeTy.newMessage().set("pick_num", "not a number") }
                .exceptionOrNull()?.message?.contains("type mismatch") == true,
            "wrong-typed value rejected",
        )
        check(
            runCatching { edgeTy.newMessage().newMessage("text") }
                .exceptionOrNull()?.message?.contains("not a message field") == true,
            "setMessage on a scalar rejected",
        )
    }
    println("OK   $checks checks passed")
}

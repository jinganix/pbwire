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
import java.util.Collections
import java.util.concurrent.CountDownLatch
import java.util.concurrent.Executors

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

/*
 * 4 workers x 2000 iters over one shared MessageType, three phases per
 * worker: (a) independent-message encode == want bytes, (b) SHARED
 * read-only message concurrent encode == want bytes, (c) fresh-target decode
 * of want with field checks, NOT re-encode. Only the type, the shared message
 * and the want bytes cross threads; every worker owns its other messages.
 * Timing prints only, never asserted; failures name the worker and the phase.
 */
fun threads(ty: MessageType) {
    val want = fill(ty).use { it.encode() }
    // Serial oracle pin: must hold before any worker starts.
    fill(ty).use { check(it.encode().contentEquals(want), "serial encode is the oracle") }
    // 1-thread throughput baseline (print only).
    val serialIters = 2000
    val serialStart = System.nanoTime()
    repeat(serialIters) { fill(ty).use { it.encode() } }
    val serialSecs = (System.nanoTime() - serialStart) / 1e9
    // Shared read-only fixture for phase (b): filled once here, never mutated
    // while workers run, closed after they join.
    val shared = fill(ty)
    val problems = Collections.synchronizedList(mutableListOf<String>())
    val pool = Executors.newFixedThreadPool(4)
    val done = CountDownLatch(4)
    val start = System.nanoTime()
    repeat(4) { worker ->
        pool.execute {
            try {
                repeat(2000) {
                    try {
                        fill(ty).use {
                            if (!it.encode().contentEquals(want)) {
                                problems.add("worker $worker: phase (a) independent-encode != want bytes")
                            }
                        }
                    } catch (e: RuntimeException) {
                        problems.add("worker $worker: phase (a) threw ${e.javaClass.simpleName}")
                    }
                    try {
                        if (!shared.encode().contentEquals(want)) {
                            problems.add("worker $worker: phase (b) shared-read-only-encode != want bytes")
                        }
                    } catch (e: RuntimeException) {
                        problems.add("worker $worker: phase (b) threw ${e.javaClass.simpleName}")
                    }
                    try {
                        ty.parse(want).use { back ->
                            if (back.getLong("i32") != Int.MIN_VALUE.toLong()
                                || !back.getBool("flag")
                                || back.getString("str") != "hello"
                                || back.getDouble("f64") != -123456.789
                                || back.getLong("color") != 2L
                                || back.getList("nums") != listOf(1L, -5L, 300L)
                                || back.getMessage("inner")!!.getString("name") != "kid"
                            ) {
                                problems.add("worker $worker: phase (c) fresh-decode field mismatch")
                            }
                        }
                    } catch (e: RuntimeException) {
                        problems.add("worker $worker: phase (c) threw ${e.javaClass.simpleName}")
                    }
                }
            } finally {
                done.countDown()
            }
        }
    }
    done.await()
    pool.shutdown()
    shared.close()
    check(problems.isEmpty(), "4 threads share one type: ${problems.take(8)}")
    val parallelSecs = (System.nanoTime() - start) / 1e9
    val total = 4 * 2000 * 3
    println("OK   threads: serial $serialIters encodes in %.3f s (%.0f/s), 4-thread $total phase ops in %.3f s (%.0f/s)".format(serialSecs, serialIters / serialSecs, parallelSecs, total / parallelSecs))
}

fun main(args: Array<String>) {
    val desc = args.getOrNull(0) ?: "../c/tests/data/matrix3.desc"
    val oracle = Files.readAllBytes(Path.of(desc).resolveSibling("matrix3.bin"))

    schema(desc).use { schema ->
        val ty = schema.message("structtest.Scalars3")
        val innerTy = schema.message("structtest.Inner3")

        threads(ty)

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
                .exceptionOrNull()?.message?.contains("needs a string") == true,
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

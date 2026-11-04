/*
 * Kotlin binding benchmark against the official Kotlin generated code
 * (protoc --kotlin_out, which drives protobuf-java) on the same matrix3.proto
 * and the four standard tiers from BENCHMARKS.md.
 *
 * The same payload as the C/Go/Java/Rust rows: every scalar kind, the nested
 * message, the two proto3 optionals, and the repeated fields scaled x0 / x1 /
 * x100 / x3000, which lands the tiers on 136 / 174 / 2469 / 74913 bytes. Both
 * sides must produce identical bytes before anything is timed, and both
 * allocate per call (pbwire encode()/decode() against toByteArray()/
 * parseFrom()), so the comparison is like for like.
 *
 *   ./bench.sh          generate, build, run
 */
import structtest.Matrix3
import structtest.inner3
import structtest.scalars3

private val cases = listOf("small", "medium", "large", "xlarge")
private val repeats = intArrayOf(0, 1, 100, 3000)
private val want = intArrayOf(136, 174, 2469, 74913)
/** Floor iterations per tier; doubled until one pass takes 0.25 s. */
private val iterations = intArrayOf(50_000, 50_000, 5_000, 500)
private const val MIN_PASS_SECONDS = 0.25
private const val PASSES = 3

private var failures = 0

private fun fillOfficial(n: Int): Matrix3.Scalars3 =
    scalars3 {
        i32 = Int.MIN_VALUE
        i64 = Long.MAX_VALUE
        u32 = -1
        u64 = -1L
        s32 = Int.MIN_VALUE
        s64 = Long.MIN_VALUE
        fx32 = 0xdeadbeef.toInt()
        fx64 = 72623859790382856L
        sfx32 = Int.MIN_VALUE
        sfx64 = Long.MIN_VALUE
        f32 = Float.POSITIVE_INFINITY
        f64 = -123456.789
        flag = true
        str = "hello"
        color = Matrix3.Color3.COLOR3_GREEN
        inner = inner3 {
            name = "kid"
            value = 42
        }
        optI32 = -7
        optStr = "opt"
        for (i in 0 until n) {
            nums += i - 3
            tags += "tag"
            inners += inner3 {
                name = "grandkid"
                value = i + 1
            }
            flags += i % 2 == 0
        }
    }

private fun fillPbwire(ty: MessageType, n: Int): Message = ty.newMessage().apply {
    set("i32", Int.MIN_VALUE.toLong())
    set("i64", Long.MAX_VALUE)
    set("u32", 0xffffffffL)
    set("u64", -1L)
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
    newMessage("inner").set("name", "kid").set("value", 42L)
    for (i in 0 until n) {
        set("nums", (i - 3).toLong())
        set("tags", "tag")
        set("flags", i % 2 == 0)
        newMessage("inners").set("name", "grandkid").set("value", (i + 1).toLong())
    }
}

private fun run(iterations: Int, op: () -> Unit): Double {
    val start = System.nanoTime()
    for (i in 0 until iterations) {
        op()
    }
    return (System.nanoTime() - start) / iterations.toDouble()
}

/** Double from the tier floor until one pass takes MIN_PASS_SECONDS. */
private fun calibrate(op: () -> Unit, floor: Int): Int {
    var count = maxOf(1, floor)
    while (true) {
        val start = System.nanoTime()
        for (i in 0 until count) {
            op()
        }
        if ((System.nanoTime() - start) / 1e9 >= MIN_PASS_SECONDS || count >= 100_000_000) {
            return count
        }
        count *= 2
    }
}

/** Every op calibrates independently from the floor, then one warm-up
 * pass and the best of PASSES timed passes. */
private fun measure(floor: Int, op: () -> Unit): Double {
    val count = calibrate(op, floor)
    run(count, op)
    var best = Double.MAX_VALUE
    for (pass in 0 until PASSES) {
        best = minOf(best, run(count, op))
    }
    return best
}

private fun row(impl: String, op: String, bytes: Int, ns: Double) {
    println("  %-12s %-7s %12.1f %10.1f".format(impl, op, ns, bytes / ns * 1000.0))
}

fun main(args: Array<String>) {
    if (args.size != 1) {
        System.err.println("usage: Bench <descriptor-set>")
        System.exit(2)
    }
    schema(args[0]).use { schema ->
        val ty = schema.message("structtest.Scalars3")

        for (tier in cases.indices) {
            val label = cases[tier]
            val n = repeats[tier]
            val count = iterations[tier]

            val reference = fillOfficial(n).toByteArray()
            if (reference.size != want[tier]) {
                println("FAIL $label: official encoder wrote ${reference.size} bytes, expected ${want[tier]}")
                failures++
                continue
            }
            fillPbwire(ty, n).use { message ->
                val got = message.encode()
                if (!got.contentEquals(reference)) {
                    println("FAIL $label: pbwire-kotlin wrote ${got.size} bytes, protobuf-kotlin ${reference.size}")
                    failures++
                    return@use
                }
                println("OK   $label encoding parity, x$n (${reference.size} bytes)")

                // Decode must round-trip byte-exactly and expose the same values.
                var backOk = false
                ty.decode(reference).use { back ->
                    val backInner = back.getMessage("inner")
                    backOk = back.encode().contentEquals(reference)
                        && back.getString("str") == "hello"
                        && backInner.getString("name") == "kid"
                        && back.getList("nums").size == n
                        && (n == 0 || back.getList("nums")[n - 1] as Long == (n - 4).toLong())
                }
                if (!backOk) {
                    println("FAIL $label: pbwire-kotlin decode differs")
                    failures++
                    return@use
                }

                val official = fillOfficial(n)
                // Tier line shows the reference op's count; slower ops keep the floor.
                val tierCount = calibrate({ message.encode() }, count)
                val encode = measure(count) { message.encode() }
                val decode = measure(count) { ty.decode(reference).close() }
                val officialEncode = measure(count) { official.toByteArray() }
                val officialDecode = measure(count) { Matrix3.Scalars3.parseFrom(reference) }

                println("$label: ${reference.size} bytes, $tierCount iterations (repeat field x$n)")
                println("  %-12s %-7s %12s %10s".format("impl", "op", "ns/op", "MB/s"))
                row("pbwire-kotlin", "encode", reference.size, encode)
                row("protobuf-kotlin", "encode", reference.size, officialEncode)
                row("pbwire-kotlin", "decode", reference.size, decode)
                row("protobuf-kotlin", "decode", reference.size, officialDecode)
                println()
            }
        }
    }
    if (failures != 0) {
        System.err.println("$failures check(s) failed")
        System.exit(1)
    }
}

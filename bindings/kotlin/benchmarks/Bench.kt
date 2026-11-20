/*
 * Kotlin binding benchmark against the official Kotlin generated code
 * (protoc --kotlin_out, which drives protobuf-java) on the same matrix3.proto
 * and the four standard tiers from BENCHMARKS.md.
 *
 * The same payload as the C/Go/Java/Rust rows: every scalar kind, the nested
 * message, the two proto3 optionals, and the repeated fields scaled x0 / x1 /
 * x100 / x3000, which lands the tiers on 136 / 174 / 2469 / 74913 bytes. Every
 * row allocates per call (pbwire encode()/decode() against toByteArray()/
 * parseFrom()), so the timer covers the user-visible task.
 *
 * Arms (bindings/README.md#pbench-three-arms-one-contract):
 *   tree  Message (GenericCodec/DMsg), prebuilt -- what the codec costs once the tree exists
 *   core  the committed generated model      -- native object in and out (Structtest_Scalars3)
 *   shim  ../java/pbench/jni                 -- ceiling: one JNI crossing, no tree translation
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

/** The committed generated Java model for the same payload (native object). */
private fun fillModel(n: Int): Structtest_Scalars3 {
    val b = Structtest_Scalars3.newBuilder()
    b.setI32(Int.MIN_VALUE)
    b.setI64(Long.MAX_VALUE)
    b.setU32(-1)
    b.setU64(-1L)
    b.setS32(Int.MIN_VALUE)
    b.setS64(Long.MIN_VALUE)
    b.setFx32(0xdeadbeef.toInt())
    b.setFx64(72623859790382856L)
    b.setSfx32(Int.MIN_VALUE)
    b.setSfx64(Long.MIN_VALUE)
    b.setF32(Float.POSITIVE_INFINITY)
    b.setF64(-123456.789)
    b.setFlag(true)
    b.setStr("hello")
    b.setColorValue(2)
    b.setInner(Structtest_Inner3.newBuilder().setName("kid").setValue(42))
    b.setOptI32(-7)
    b.setOptStr("opt")
    for (i in 0 until n) {
        b.addNums(i - 3)
        b.addTags("tag")
        b.addInners(Structtest_Inner3.newBuilder().setName("grandkid").setValue(i + 1))
        b.addFlags(i % 2 == 0)
    }
    return b.build()
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
 * pass and the best of PASSES timed passes, all ops back to back per pass. */
private fun measureAll(floor: Int, ops: List<() -> Unit>): DoubleArray {
    val counts = IntArray(ops.size) { calibrate(ops[it], floor) }
    ops.forEachIndexed { i, op -> run(minOf(counts[i], 1000), op) }
    val best = DoubleArray(ops.size) { Double.MAX_VALUE }
    for (pass in 0 until PASSES) {
        ops.forEachIndexed { i, op -> best[i] = minOf(best[i], run(counts[i], op)) }
    }
    return best
}

private fun row(impl: String, op: String, bytes: Int, ns: Double) {
    println("  %-18s %-7s %12.1f %10.1f".format(impl, op, ns, bytes / ns * 1000.0))
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

                // The native-object arm: the committed generated model must
                // round-trip byte-identically and expose the same values.
                val model = fillModel(n)
                if (!model.toByteArray().contentEquals(reference)) {
                    println("FAIL $label: pbwire-kotlin core encode differs")
                    failures++
                    return@use
                }
                val modelBack = Structtest_Scalars3.parseFrom(reference)
                val modelOk = modelBack.toByteArray().contentEquals(reference) &&
                    modelBack.getStr() == "hello" &&
                    modelBack.getNumsCount() == n &&
                    modelBack.getInnersCount() == n &&
                    modelBack.getInner().getName() == "kid" &&
                    (n == 0 || modelBack.getInners(n - 1).getValue() == n)
                if (!modelOk) {
                    println("FAIL $label: pbwire-kotlin core decode differs")
                    failures++
                    return@use
                }
                println("OK   $label generated-model parity, x$n (${reference.size} bytes)")

                val official = fillOfficial(n)
                // Tier line shows the reference op's count; slower ops keep the floor.
                val tierCount = calibrate({ message.encode() }, count)
                val res = measureAll(count, listOf(
                    { message.encode() },
                    { model.toByteArray() },
                    { official.toByteArray() },
                    { ty.decode(reference).close() },
                    { Structtest_Scalars3.parseFrom(reference) },
                    { Matrix3.Scalars3.parseFrom(reference) },
                ))

                println("$label: ${reference.size} bytes, $tierCount iterations (repeat field x$n)")
                println("  %-18s %-7s %12s %10s".format("impl", "op", "ns/op", "MB/s"))
                row("pbwire-kotlin-tree", "encode", reference.size, res[0])
                row("pbwire-kotlin-core", "encode", reference.size, res[1])
                row("protobuf-kotlin", "encode", reference.size, res[2])
                row("pbwire-kotlin-tree", "decode", reference.size, res[3])
                row("pbwire-kotlin-core", "decode", reference.size, res[4])
                row("protobuf-kotlin", "decode", reference.size, res[5])
                println()
            }
        }
    }
    if (failures != 0) {
        System.err.println("$failures check(s) failed")
        System.exit(1)
    }
}

/*
 * Generated-compat check for the Kotlin binding (TESTSPEC T1–T6, vectors.json as
 * the single source of truth). Kotlin reuses the Java facade classes: the
 * generated `matrix3.kt`/`oneof1.kt` layers are thin DSL constructors over
 * the Java Builders, and every assertion below calls the Java API from
 * idiomatic Kotlin (property syntax, apply, runCatching).
 *
 *   java --enable-native-access=ALL-UNNAMED -Dpbwire.library=... \
 *        -cp out/pbwire.jar:../java/out GeneratedTestKt matrix3.bin oneof1.bin
 */

import java.nio.file.Files
import java.nio.file.Path

private var checks = 0

private const val ORACLE_MAJOR = 4
private const val ORACLE_MINOR = 36
private const val ORACLE_PATCH = 2

private fun check(ok: Boolean, what: String) {
    if (!ok) {
        System.err.println("FAIL $what")
        System.exit(1)
    }
    checks++
}

private fun build(): Structtest_Scalars3 = scalars3 {
    setI32(Int.MIN_VALUE)
    setI64(Long.MAX_VALUE)
    setU32(-1) // 4294967295 as raw 32-bit bits
    setU64(-1L) // 18446744073709551615 as raw 64-bit bits
    setS32(Int.MIN_VALUE)
    setS64(Long.MIN_VALUE)
    setFx32(0xdeadbeef.toInt())
    setFx64(72623859790382856L)
    setSfx32(Int.MIN_VALUE)
    setSfx64(Long.MIN_VALUE)
    setF32(Float.POSITIVE_INFINITY)
    setF64(-123456.789)
    setFlag(true)
    setStr("hello")
    setColor(Structtest_Color3.COLOR3_GREEN)
    setInner(inner3 { setName("kid"); setValue(42) })
    addNums(1); addNums(-5); addNums(300)
    addTags("a"); addTags("bb")
    addInners(inner3 { setName("x"); setValue(1) })
    addInners(inner3 { setName("y"); setValue(-2) })
    setOptI32(-7)
    setOptStr("opt")
    addFlags(true); addFlags(false); addFlags(true)
}

private fun buildOracle(): structtest.Matrix3.Scalars3 =
    structtest.Matrix3.Scalars3.newBuilder()
        .setI32(Int.MIN_VALUE)
        .setI64(Long.MAX_VALUE)
        .setU32(-1)
        .setU64(-1L)
        .setS32(Int.MIN_VALUE)
        .setS64(Long.MIN_VALUE)
        .setFx32(0xdeadbeef.toInt())
        .setFx64(72623859790382856L)
        .setSfx32(Int.MIN_VALUE)
        .setSfx64(Long.MIN_VALUE)
        .setF32(Float.POSITIVE_INFINITY)
        .setF64(-123456.789)
        .setFlag(true)
        .setStr("hello")
        .setColor(structtest.Matrix3.Color3.COLOR3_GREEN)
        .setInner(structtest.Matrix3.Inner3.newBuilder().setName("kid").setValue(42))
        .addNums(1).addNums(-5).addNums(300)
        .addTags("a").addTags("bb")
        .addInners(structtest.Matrix3.Inner3.newBuilder().setName("x").setValue(1))
        .addInners(structtest.Matrix3.Inner3.newBuilder().setName("y").setValue(-2))
        .setOptI32(-7)
        .setOptStr("opt")
        .addFlags(true).addFlags(false).addFlags(true)
        .build()

private fun verify(m: Structtest_Scalars3) {
    check(m.i32 == Int.MIN_VALUE, "T2 i32")
    check(m.i64 == Long.MAX_VALUE, "T2 i64")
    check(m.u32 == -1, "T2 u32")
    check(m.u64 == -1L, "T2 u64")
    check(m.s32 == Int.MIN_VALUE, "T2 s32")
    check(m.s64 == Long.MIN_VALUE, "T2 s64")
    check(m.fx32 == 0xdeadbeef.toInt(), "T2 fx32")
    check(m.fx64 == 72623859790382856L, "T2 fx64")
    check(m.sfx32 == Int.MIN_VALUE, "T2 sfx32")
    check(m.sfx64 == Long.MIN_VALUE, "T2 sfx64")
    check(m.f32 == Float.POSITIVE_INFINITY, "T2 f32")
    check(m.f64 == -123456.789, "T2 f64")
    check(m.isFlag, "T2 flag")
    check(m.str == "hello", "T2 str")
    check(m.color == Structtest_Color3.COLOR3_GREEN, "T2 color")
    check(m.inner.name == "kid" && m.inner.value == 42, "T2 inner")
    check(m.numsList == listOf(1, -5, 300), "T2 nums")
    check(m.tagsList == listOf("a", "bb"), "T2 tags")
    check(m.innersCount == 2 && m.getInners(1).value == -2, "T2 inners")
    check(m.hasOptI32() && m.optI32 == -7, "T2 opt_i32")
    check(m.hasOptStr() && m.optStr == "opt", "T2 opt_str")
    check(m.flagsList == listOf(true, false, true), "T2 flags")
}

private fun verifyOneof(m: Spectest_Oneof1) {
    check(m.name == "a", "T2 oneof name")
    check(m.kindCase == Spectest_Oneof1.KindCase.TEXT, "T2 oneof case")
    check(m.text == "hi", "T2 oneof text")
    check(!m.hasNum(), "T2 oneof num absent")
    val nested = oneof1 { setName("n"); setInner(oneof1Inner { setV(7) }) }
    check(Spectest_Oneof1.parseFrom(nested.toByteArray()).inner.v == 7, "T2 nested oneof member")
}

/** The comparable business result for T5. */
private data class Summary(
    val wire: ByteArray,
    val str: String,
    val i32: Int,
    val nums: List<Int>,
    val inner: String,
    val hasOpt: Boolean,
) {
    override fun equals(other: Any?): Boolean {
        if (other !is Summary) return false
        return wire.contentEquals(other.wire) && str == other.str && i32 == other.i32 &&
            nums == other.nums && inner == other.inner && hasOpt == other.hasOpt
    }

    override fun hashCode(): Int = wire.contentHashCode()
}

private fun <B, M> workflow(
    nw: () -> B,
    set: B.() -> Unit,
    build: (B) -> M,
    enc: (M) -> ByteArray,
    dec: (ByteArray) -> M,
    get: (M) -> Summary,
): Summary {
    val wire = enc(build(nw().apply(set)))
    val s = get(dec(wire))
    return Summary(wire, s.str, s.i32, s.nums, s.inner, s.hasOpt)
}

fun main(args: Array<String>) {
    // The codegen Makefile still passes the .desc path; resolve the sibling
    // reference bytes (and the shared oneof1 fixture) from it.
    val first = args.getOrNull(0)
    val matrixBin = when {
        first == null -> Path.of("../c/tests/data/matrix3.bin")
        first.endsWith(".desc") -> Path.of(first).resolveSibling("matrix3.bin")
        else -> Path.of(first)
    }
    val oneofBin = args.getOrNull(1)?.let { Path.of(it) }
        ?: Path.of("../../codegen/testdata/oneof1.bin")
    val reference = Files.readAllBytes(matrixBin)
    val referenceOneof = Files.readAllBytes(oneofBin)

    check(
        com.google.protobuf.RuntimeVersion.MAJOR == ORACLE_MAJOR &&
            com.google.protobuf.RuntimeVersion.MINOR == ORACLE_MINOR &&
            com.google.protobuf.RuntimeVersion.PATCH == ORACLE_PATCH,
        "oracle pin drifted; see codegen/testdata/ORACLES.md",
    )

    // T1: encode equals the protoc reference bytes.
    check(build().toByteArray().contentEquals(reference), "T1 facade != reference")
    val one = oneof1 { setName("a"); setText("hi") }
    check(one.toByteArray().contentEquals(referenceOneof), "T1 oneof != reference")

    // T2: decode reads every fixture value back.
    verify(Structtest_Scalars3.parseFrom(reference))
    verifyOneof(Spectest_Oneof1.parseFrom(referenceOneof))

    // T3: empty encodes empty; explicit optional zero/empty round-trips.
    check(Structtest_Scalars3.newBuilder().build().toByteArray().isEmpty(), "T3 empty encodes empty")
    val zero = scalars3 { setOptI32(0) }
    val zeroBack = Structtest_Scalars3.parseFrom(zero.toByteArray())
    check(zeroBack.hasOptI32() && zeroBack.optI32 == 0 && !zeroBack.hasOptStr(), "T3 explicit zero keeps presence")
    val emptyOpt = scalars3 { setOptStr("") }
    val emptyOptBack = Structtest_Scalars3.parseFrom(emptyOpt.toByteArray())
    check(emptyOptBack.hasOptStr() && emptyOptBack.optStr.isEmpty(), "T3 explicit empty optional keeps presence")
    check(!Structtest_Scalars3.parseFrom(ByteArray(0)).hasOptI32(), "T3 empty wire has no presence")

    // T4: differential vs the pinned oracle.
    check(buildOracle().toByteArray().contentEquals(reference), "T4 oracle != reference")
    check(
        spectest.Oneof1OuterClass.Oneof1.newBuilder().setName("a").setText("hi").build()
            .toByteArray().contentEquals(referenceOneof),
        "T4 oracle oneof != reference",
    )
    val om = structtest.Matrix3.Scalars3.parseFrom(build().toByteArray())
    val fm = Structtest_Scalars3.parseFrom(buildOracle().toByteArray())
    check(om.i32 == fm.i32 && om.i64 == fm.i64, "T4 i32/i64")
    check(om.u32 == fm.u32 && om.u64 == fm.u64, "T4 u32/u64")
    check(om.s32 == fm.s32 && om.s64 == fm.s64, "T4 s32/s64")
    check(om.fx32 == fm.fx32 && om.fx64 == fm.fx64, "T4 fx32/fx64")
    check(om.sfx32 == fm.sfx32 && om.sfx64 == fm.sfx64, "T4 sfx32/sfx64")
    check(om.f32 == fm.f32 && om.f64 == fm.f64, "T4 f32/f64")
    check(om.flag == fm.isFlag && om.str == fm.str, "T4 flag/str")
    check(om.colorValue == fm.colorValue, "T4 color")
    check(om.inner.name == fm.inner.name && om.inner.value == fm.inner.value, "T4 inner")
    check(om.numsList == fm.numsList, "T4 nums")
    check(om.tagsList == fm.tagsList, "T4 tags")
    check(om.innersCount == fm.innersCount && om.getInners(1).value == fm.getInners(1).value, "T4 inners")
    check(om.hasOptI32() == fm.hasOptI32() && om.optI32 == fm.optI32 &&
        om.hasOptStr() == fm.hasOptStr() && om.optStr == fm.optStr, "T4 optional presence agrees")
    check(om.flagsList == fm.flagsList, "T4 flags")

    // T5: one generic workflow against both libraries.
    val a = workflow(
        { structtest.Matrix3.Scalars3.newBuilder() },
        { str = "kid"; i32 = -5; addNums(1); addNums(2); inner = structtest.Matrix3.Inner3.newBuilder().setName("x").setValue(1).build() },
        { it.build() },
        { it.toByteArray() },
        { structtest.Matrix3.Scalars3.parseFrom(it) },
        { Summary(ByteArray(0), it.str, it.i32, it.numsList, it.inner.name, it.hasOptI32()) },
    )
    val f = workflow(
        { Structtest_Scalars3.newBuilder() },
        { str = "kid"; i32 = -5; addNums(1); addNums(2); inner = inner3 { setName("x"); setValue(1) } },
        { it.build() },
        { it.toByteArray() },
        { Structtest_Scalars3.parseFrom(it) },
        { Summary(ByteArray(0), it.str, it.i32, it.numsList, it.inner.name, it.hasOptI32()) },
    )
    check(a == f, "T5 mismatch")

    // T6: errors are loud.
    check(runCatching { Structtest_Scalars3.parseFrom(byteArrayOf(0xff.toByte(), 0xff.toByte(), 0xff.toByte())) }.isFailure, "T6 bad wire fails")
    check(runCatching { structtest.Matrix3.Scalars3.parseFrom(byteArrayOf(0xff.toByte(), 0xff.toByte(), 0xff.toByte())) }.isFailure, "T6 oracle bad wire fails")
    val empty = Structtest_Scalars3.parseFrom(ByteArray(0))
    check(empty.toByteArray().isEmpty() && empty.i32 == 0 && !empty.hasOptI32() &&
        empty.inner == Structtest_Inner3.getDefaultInstance(), "T6 empty decodes to default")
    check(runCatching { Structtest_Scalars3.newBuilder().setStr(null) }.isFailure, "T6 null setter fails")
    check(runCatching { Tutorial_Person.newBuilder().setName("x").build() }.isFailure, "T6 missing required fails")

    // Unknown enum values survive as UNRECOGNIZED on both sides, raw value kept.
    val mystery = scalars3 { setColorValue(99) }
    val mysteryBack = Structtest_Scalars3.parseFrom(mystery.toByteArray())
    val oracleMystery = structtest.Matrix3.Scalars3.newBuilder().setColorValue(99).build()
    val oracleMysteryBack = structtest.Matrix3.Scalars3.parseFrom(oracleMystery.toByteArray())
    check(mysteryBack.color == Structtest_Color3.UNRECOGNIZED && mysteryBack.colorValue == 99, "T6 unknown enum kept")
    check(oracleMysteryBack.color == structtest.Matrix3.Color3.UNRECOGNIZED && oracleMysteryBack.colorValue == 99, "T6 oracle unknown enum kept")
    check(mystery.toByteArray().contentEquals(oracleMystery.toByteArray()), "T6 unknown enum bytes agree")

    // Several oneof members set: the last one wins, on both sides.
    val both = oneof1 { setName("n"); setNum(1); setText("hi") }
    check(both.kindCase == Spectest_Oneof1.KindCase.TEXT && both.text == "hi" && !both.hasNum(), "T6 oneof last wins")
    val nested = oneof1 { setName("n"); setInner(oneof1Inner { setV(7) }) }
    check(Spectest_Oneof1.parseFrom(nested.toByteArray()).inner.v == 7, "T6 nested oneof member round-trips")
    check(both.kindCase.name == spectest.Oneof1OuterClass.Oneof1.newBuilder().setName("n").setNum(1).setText("hi").build().kindCase.name, "T6 oneof case agrees with oracle")

    println("GeneratedTest: $checks checks passed")
}

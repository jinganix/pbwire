/*
 * Scala binding benchmark against the official Scala baseline (ScalaPB
 * generated code from the same matrix3.proto) on the four standard tiers
 * from BENCHMARKS.md.
 *
 * The same payload as the C/Go/Java/Kotlin/Rust rows: every scalar kind,
 * the nested message, the two proto3 optionals, and the repeated fields
 * scaled x0 / x1 / x100 / x3000, which lands the tiers on 136 / 174 / 2469 /
 * 74913 bytes. Both sides must produce identical bytes before anything is
 * timed, and both allocate per call (pbwire encode()/decode() against
 * toByteArray/parseFrom), so the timer covers the user-visible task.
 *
 * Arms (bindings/README.md#pbench-three-arms-one-contract):
 *   tree  Message (GenericCodec/DMsg) full task -- fresh build per field per
 *               iteration, encode; decode, every gate-checked field read back
 *               per iteration (mirrors bindings/java/benchmarks/Bench.java)
 *   core  the committed generated model      -- native object in and out (matrix3Gen case classes)
 *   proxy Message (GenericCodec/DMsg), prebuilt -- re-encode a held tree,
 *               decode-and-drop with no materialization. Published, never
 *               the headline.
 *   shim  ../java/pbench/jni                 -- ceiling: one JNI crossing, no tree translation
 *
 * The `structtest` classes are the ScalaPB baseline: bench.sh generates
 * them from bindings/c/tests/data/matrix3.proto with scalapbc, so this
 * file compiles only as part of that script (like the Kotlin harness,
 * which compiles against protoc --kotlin_out output).
 *
 *   ./bench.sh          generate, build, run
 */
import structtest.matrix3._

import Pbwire._

object Bench {
  private val cases = List("small", "medium", "large", "xlarge")
  private val repeats = Array(0, 1, 100, 3000)
  private val want = Array(136, 174, 2469, 74913)

  /** Floor iterations per tier; doubled until one pass takes 0.25 s. */
  private val iterations = Array(50000, 50000, 5000, 500)
  private val MinPassSeconds = 0.25
  private val Passes = 3

  private var failures = 0

  private var sink = 0L

  /**
   * The baseline decode half of the timed full task, mirroring
   * bindings/java/benchmarks/Bench.java consumeBaseline (Task 5) and the
   * Kotlin harness (Task 6): the pbwire core arm
   * (matrix3Gen.Structtest_Scalars3.decode) materializes every field
   * eagerly into the case class, so the ScalaPB arm folds the same
   * gate-checked fields instead of timing a bare parseFrom (fairness §4).
   * ScalaPB parses strictly (every field lands in the case class), so expect
   * the same nil effect Task 5 measured; this keeps the two harnesses'
   * headline cells directly comparable.
   */
  private def consumeBaseline(wire: Array[Byte], n: Int): Unit = {
    val back = Scalars3.parseFrom(wire)
    sink += back.str.length
    var i = 0
    while (i < back.tags.size) {
      sink += back.tags(i).length
      i += 1
    }
    sink += back.inners.size
    back.inner.foreach(in => {
      sink += in.name.length
      sink += in.value
    })
    var j = 0
    while (j < n) {
      val m = back.inners(j)
      sink += m.name.length + m.value
      j += 1
    }
  }

  /** Fresh-build tree encode: a new per-field Message per call (never the held tree). */
  private def freshEncode(ty: MessageType, n: Int): Array[Byte] =
    using(fillPbwire(ty, n)) { m => m.encode() }

  /**
   * The tree decode half of the timed full task: decode, then read back
   * every gate-checked field (the same field list the gate asserts).
   * Message.decode is lazy on message/string/bytes fields, so this is the
   * work the binding really does turning wire into values.
   */
  private def consumeTree(ty: MessageType, wire: Array[Byte], n: Int): Unit = {
    using(ty.decode(wire)) { back =>
      sink += back.getString("str").length
      sink += back.getList("tags").size()
      val inners = back.getList("inners")
      sink += inners.size()
      val inner = back.getMessage("inner")
      sink += inner.getString("name").length
      sink += inner.getLong("value")
      if (n > 0) {
        val last = inners.get(n - 1).asInstanceOf[Message]
        sink += last.getString("name").length
        sink += last.getLong("value")
      }
    }
  }

  /** The committed generated model for the same payload (native object). */
  private def fillGenerated(n: Int): matrix3Gen.Structtest_Scalars3 =
    matrix3Gen.Structtest_Scalars3(
      i32 = Int.MinValue,
      i64 = Long.MaxValue,
      u32 = 0xffffffffL,
      u64 = -1L,
      s32 = Int.MinValue,
      s64 = Long.MinValue,
      fx32 = 0xdeadbeefL,
      fx64 = 72623859790382856L,
      sfx32 = Int.MinValue,
      sfx64 = Long.MinValue,
      f32 = Float.PositiveInfinity,
      f64 = -123456.789,
      flag = true,
      str = "hello",
      color = 2,
      inner = Some(matrix3Gen.Structtest_Inner3(name = "kid", value = 42)),
      nums = (0 until n).map(i => i - 3).toList,
      tags = List.fill(n)("tag"),
      inners = (0 until n).map(i =>
        matrix3Gen.Structtest_Inner3(name = "grandkid", value = i + 1)).toList,
      optI32 = Some(-7),
      optStr = Some("opt"),
      flags = (0 until n).map(i => i % 2 == 0).toList
    )

  private def fillOfficial(n: Int): Scalars3 = {
    var m = Scalars3(
      i32 = Int.MinValue,
      i64 = Long.MaxValue,
      u32 = -1,
      u64 = -1L,
      s32 = Int.MinValue,
      s64 = Long.MinValue,
      fx32 = 0xdeadbeef,
      fx64 = 72623859790382856L,
      sfx32 = Int.MinValue,
      sfx64 = Long.MinValue,
      f32 = Float.PositiveInfinity,
      f64 = -123456.789,
      flag = true,
      str = "hello",
      color = Color3.COLOR3_GREEN,
      inner = Some(Inner3(name = "kid", value = 42)),
      optI32 = Some(-7),
      optStr = Some("opt")
    )
    for (i <- 0 until n) {
      m = m.addNums(i - 3)
        .addTags("tag")
        .addInners(Inner3(name = "grandkid", value = i + 1))
        .addFlags(i % 2 == 0)
    }
    m
  }

  private def fillPbwire(ty: MessageType, n: Int): Message = ty.build { m =>
    m.set("i32", Int.MinValue.toLong)
    m.set("i64", Long.MaxValue)
    m.set("u32", 0xffffffffL)
    m.set("u64", -1L)
    m.set("s32", Int.MinValue.toLong)
    m.set("s64", Long.MinValue)
    m.set("fx32", 0xdeadbeefL)
    m.set("fx64", 72623859790382856L)
    m.set("sfx32", Int.MinValue.toLong)
    m.set("sfx64", Long.MinValue)
    m.set("f32", Double.PositiveInfinity)
    m.set("f64", -123456.789)
    m.set("flag", true)
    m.set("str", "hello")
    m.set("color", 2L)
    m.set("opt_i32", -7L)
    m.set("opt_str", "opt")
    m.message("inner") { c => c.set("name", "kid"); c.set("value", 42L) }
    for (i <- 0 until n) {
      m.set("nums", (i - 3).toLong)
      m.set("tags", "tag")
      m.set("flags", i % 2 == 0)
      m.message("inners") { c => c.set("name", "grandkid"); c.set("value", (i + 1).toLong) }
    }
  }

  private def run(iterations: Int, op: () => Unit): Double = {
    val start = System.nanoTime()
    var i = 0
    while (i < iterations) { op(); i += 1 }
    (System.nanoTime() - start).toDouble / iterations
  }

  /** Double from the tier floor until one pass takes MinPassSeconds. */
  private def calibrate(op: () => Unit, floor: Int): Int = {
    var count = math.max(1, floor)
    while (true) {
      val start = System.nanoTime()
      var i = 0
      while (i < count) { op(); i += 1 }
      if ((System.nanoTime() - start).toDouble / 1e9 >= MinPassSeconds || count >= 100000000)
        return count
      count *= 2
    }
    count
  }

  /** Every op calibrates from the floor, each warms up once, then the best
   * of Passes timed passes interleaved per op (the unified measurement
   * standard in BENCHMARKS.md), so a load spike hits both sides equally. */
  private def measureAll(floor: Int, ops: Seq[() => Unit]): Seq[Double] = {
    val counts = ops.map(calibrate(_, floor))
    counts.zip(ops).foreach { case (c, op) => run(math.min(c, 1000), op) }
    var best = Seq.fill(ops.size)(Double.MaxValue)
    var pass = 0
    while (pass < Passes) {
      val timed = counts.zip(ops).map { case (c, op) => run(c, op) }
      best = best.zip(timed).map { case (b, t) => math.min(b, t) }
      pass += 1
    }
    best
  }

  private def row(impl: String, op: String, bytes: Int, ns: Double): Unit =
    println("  %-18s %-7s %12.1f %10.1f".format(impl, op, ns, bytes / ns * 1000.0))

  def main(args: Array[String]): Unit = {
    if (args.length != 1) {
      System.err.println("usage: Bench <descriptor-set>")
      System.exit(2)
    }
    using(schema(args(0))) { schema =>
      val ty = schema.message("structtest.Scalars3")

      for (tier <- cases.indices) {
        val label = cases(tier)
        val n = repeats(tier)
        val count = iterations(tier)

        val reference = fillOfficial(n).toByteArray
        if (reference.length != want(tier)) {
          println("FAIL " + label + ": official encoder wrote " + reference.length +
            " bytes, expected " + want(tier))
          failures += 1
        } else {
          using(fillPbwire(ty, n)) { message =>
            val got = message.encode()
            if (!java.util.Arrays.equals(got, reference)) {
              println("FAIL " + label + ": pbwire-scala wrote " + got.length +
                " bytes, ScalaPB " + reference.length)
              failures += 1
            } else {
              println("OK   " + label + " encoding parity, x" + n + " (" + reference.length + " bytes)")

              // Decode must round-trip byte-exactly and expose the same values.
              var backOk = false
              using(ty.decode(reference)) { back =>
                backOk = java.util.Arrays.equals(back.encode(), reference) &&
                  back.getString("str") == "hello" &&
                  back.getMessage("inner").getString("name") == "kid" &&
                  back.getList("nums").size() == n &&
                  (n == 0 || back.getList("nums").get(n - 1) == (n - 4).toLong)
              }
              if (!backOk) {
                println("FAIL " + label + ": pbwire-scala decode differs")
                failures += 1
              } else {
                // The native-object arm: the committed generated model must
                // round-trip byte-identically and expose the same values.
                val model = fillGenerated(n)
                val modelBytes = matrix3Gen.Structtest_Scalars3.encode(model)
                if (!java.util.Arrays.equals(modelBytes, reference)) {
                  println("FAIL " + label + ": pbwire-scala core encode differs")
                  failures += 1
                } else {
                  val modelBack = matrix3Gen.Structtest_Scalars3.decode(reference)
                  val modelOk = java.util.Arrays.equals(
                    matrix3Gen.Structtest_Scalars3.encode(modelBack), reference) &&
                    modelBack.str == "hello" &&
                    modelBack.nums.size == n &&
                    modelBack.inners.size == n &&
                    modelBack.inner.exists(_.name == "kid") &&
                    (n == 0 || modelBack.inners(n - 1).value == n)
                  if (!modelOk) {
                    println("FAIL " + label + ": pbwire-scala core decode differs")
                    failures += 1
                  } else {
                    println("OK   " + label + " generated-model parity, x" + n +
                      " (" + reference.length + " bytes)")
                    // Full-task tree gate: a fresh per-field build must match the
                    // reference too (not just the held tree above), and the decode
                    // below must expose the same values the timer materializes.
                    val fresh = freshEncode(ty, n)
                    if (!java.util.Arrays.equals(fresh, reference)) {
                      println("FAIL " + label + ": fresh-build tree bytes differ")
                      failures += 1
                    } else {
                      val official = fillOfficial(n)
                    // Tree ops are the Go-parity full task: fresh Message per
                      // iteration, deep materialization of every gate-checked field
                      // on decode (the same field list the gate asserts). Proxy ops
                      // are the old prebuilt task, relabelled, not re-measured as a
                      // new task. Calibration stays per-op (measureAll): the fast
                      // proxy ops keep their own count and never inflate the tree rows.
                      val tierCount = calibrate(() => freshEncode(ty, n), count)
                      val res = measureAll(count, Seq(
                        () => freshEncode(ty, n),
                        () => matrix3Gen.Structtest_Scalars3.encode(fillGenerated(n)),
                        () => message.encode(),
                        () => fillOfficial(n).toByteArray,
                        () => consumeTree(ty, reference, n),
                        () => matrix3Gen.Structtest_Scalars3.decode(reference),
                        () => ty.decode(reference).close(),
                        () => consumeBaseline(reference, n)))

                      println(label + ": " + reference.length + " bytes, " + tierCount +
                        " iterations (repeat field x" + n + ")")
                      println("  %-18s %-7s %12s %10s".format("impl", "op", "ns/op", "MB/s"))
                      row("pbwire-scala-tree", "encode", reference.length, res(0))
                      row("pbwire-scala-core", "encode", reference.length, res(1))
                      row("pbwire-scala-proxy", "encode", reference.length, res(2))
                      row("scalapb", "encode", reference.length, res(3))
                      row("pbwire-scala-tree", "decode", reference.length, res(4))
                      row("pbwire-scala-core", "decode", reference.length, res(5))
                      row("pbwire-scala-proxy", "decode", reference.length, res(6))
                      row("scalapb", "decode", reference.length, res(7))
                      println()
                    }
                  }
                }
              }
            }
          }
        }
      }
    }
    if (failures != 0) {
      System.err.println(failures + " check(s) failed")
      System.exit(1)
    }
  }
}

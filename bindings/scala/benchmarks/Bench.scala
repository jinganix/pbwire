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
 * toByteArray/parseFrom), so the comparison is like for like.
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

  /** Every op calibrates independently from the floor, then one warm-up
   * pass and the best of Passes timed passes. */
  private def measure(floor: Int, op: () => Unit): Double = {
    val count = calibrate(op, floor)
    run(count, () => op())
    var best = Double.MaxValue
    var pass = 0
    while (pass < Passes) {
      best = math.min(best, run(count, () => op()))
      pass += 1
    }
    best
  }

  private def row(impl: String, op: String, bytes: Int, ns: Double): Unit =
    println("  %-12s %-7s %12.1f %10.1f".format(impl, op, ns, bytes / ns * 1000.0))

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
                val official = fillOfficial(n)
                val tierCount = calibrate(() => message.encode(), count)
                val encode = measure(count, () => message.encode())
                val decode = measure(count, () => ty.decode(reference).close())
                val officialEncode = measure(count, () => official.toByteArray)
                val officialDecode = measure(count, () => Scalars3.parseFrom(reference))

                println(label + ": " + reference.length + " bytes, " + tierCount +
                  " iterations (repeat field x" + n + ")")
                println("  %-12s %-7s %12s %10s".format("impl", "op", "ns/op", "MB/s"))
                row("pbwire-scala", "encode", reference.length, encode)
                row("scalapb", "encode", reference.length, officialEncode)
                row("pbwire-scala", "decode", reference.length, decode)
                row("scalapb", "decode", reference.length, officialDecode)
                println()
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

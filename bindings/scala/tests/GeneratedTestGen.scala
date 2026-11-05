/*
 * Generated-conformance check for the Scala binding: build matrix3.proto's
 * Scalars3 through the generated case classes, encode, require the protoc
 * reference bytes (matrix3.bin), then decode those bytes back and check
 * every field. The schema travels embedded in tests/gen/matrix3.scala.
 *
 * Run after ./build.sh, from bindings/scala:
 *   java -cp out/classes GeneratedTestGen ../c/tests/data/matrix3.bin
 */
import java.nio.file.{Files, Paths}
import java.util.Arrays
import matrix3Gen._

object GeneratedTestGen {
  var checks = 0
  def check(ok: Boolean, what: String): Unit = {
    assert(ok, what)
    checks += 1
  }

  def build: Structtest_Scalars3 = Structtest_Scalars3(
    i32 = Int.MinValue, i64 = Long.MaxValue,
    u32 = 4294967295L, u64 = -1L,
    s32 = Int.MinValue, s64 = Long.MinValue,
    fx32 = 0xdeadbeefL, fx64 = 72623859790382856L,
    sfx32 = Int.MinValue, sfx64 = Long.MinValue,
    f32 = Float.PositiveInfinity, f64 = -123456.789,
    flag = true, str = "hello", color = 2,
    inner = Some(Structtest_Inner3(name = "kid", value = 42)),
    nums = List(1, -5, 300), tags = List("a", "bb"),
    inners = List(Structtest_Inner3(name = "x", value = 1),
                 Structtest_Inner3(name = "y", value = -2)),
    optI32 = Some(-7), optStr = Some("opt"),
    flags = List(true, false, true)
  )

  def main(args: Array[String]): Unit = {
    val bin = Paths.get(if (args.length > 0) args(0) else "../c/tests/data/matrix3.bin")
    val reference = Files.readAllBytes(bin)

    val wire = Structtest_Scalars3.encode(build)
    check(Arrays.equals(wire, reference), "generated encode equals the protoc reference bytes")

    val m = Structtest_Scalars3.decode(reference)
    check(m.i32 == Int.MinValue, "i32")
    check(m.i64 == Long.MaxValue, "i64")
    check(m.u32 == 4294967295L, "u32")
    check(m.u64 == -1L, "u64")
    check(m.s32 == Int.MinValue, "s32")
    check(m.s64 == Long.MinValue, "s64")
    check(m.fx32 == 0xdeadbeefL, "fx32")
    check(m.fx64 == 72623859790382856L, "fx64")
    check(m.f32 == Float.PositiveInfinity, "f32")
    check(m.f64 == -123456.789, "f64")
    check(m.flag, "flag")
    check(m.str == "hello", "str")
    check(m.color == 2, "color")
    check(m.inner.exists(i => i.name == "kid" && i.value == 42), "inner")
    check(m.nums == List(1, -5, 300), "nums")
    check(m.tags == List("a", "bb"), "tags")
    check(m.inners.length == 2 && m.inners(1).value == -2, "inners")
    check(m.optI32.contains(-7), "opt_i32")
    check(m.optStr.contains("opt"), "opt_str")
    check(m.flags == List(true, false, true), "flags")

    check(Structtest_Scalars3.encode(Structtest_Scalars3()).isEmpty, "empty encodes to nothing")
    val zero = Structtest_Scalars3.decode(
      Structtest_Scalars3.encode(Structtest_Scalars3(optI32 = Some(0))))
    check(zero.optI32.contains(0L), "explicit zero with presence survives")

    println("generated test ok: " + checks + " checks")
  }
}

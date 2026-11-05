/*
 * Self-check against the pinned protoc bytes: builds structtest.Scalars3
 * from bindings/c/tests/data/matrix3.txt, encodes, compares with
 * matrix3.bin (the same oracle the C binding uses), then decodes it back
 * and checks the values. A second payload covers the cases a binding can
 * get wrong even when the core is right (tests/data/edge.bin, protoc
 * --encode): embedded NULs, non-ASCII text, a negative enum, empty
 * repeated elements, proto3 optional presence and oneof switching.
 *
 * Run after ./build.sh, from bindings/scala:
 *   java -cp out/classes TestPbwire [matrix3.desc] [edge.desc]
 */
import java.nio.file.{Files, Path, Paths}

import scala.jdk.CollectionConverters._

import Pbwire._

object TestPbwire {
  private var checks = 0

  private def check(ok: Boolean, what: String): Unit = {
    checks += 1
    if (!ok) {
      System.err.println("FAIL " + what)
      System.exit(1)
    }
  }

  private def fill(ty: MessageType): Message = ty.build { m =>
    m.set("i32", Int.MinValue.toLong)
    m.set("i64", Long.MaxValue)
    m.set("u32", 0xffffffffL)
    m.set("u64", -1L) // u64::MAX bits
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
    List(1L, -5L, 300L).foreach(v => m.set("nums", v))
    List("a", "bb").foreach(v => m.set("tags", v))
    List("x" -> 1L, "y" -> -2L).foreach { case (name, value) =>
      m.message("inners") { c => c.set("name", name); c.set("value", value) }
    }
    List(true, false, true).foreach(v => m.set("flags", v))
    m.message("inner") { c => c.set("name", "kid"); c.set("value", 42L) }
  }

  def main(args: Array[String]): Unit = {
    val matrixDesc = if (args.length > 0) args(0) else "../c/tests/data/matrix3.desc"
    val edgeDesc = if (args.length > 1) args(1) else "../../tests/data/edge.desc"
    val oracle = Files.readAllBytes(Paths.get(matrixDesc).resolveSibling("matrix3.bin"))

    using(schema(matrixDesc)) { schema =>
      val ty = schema.message("structtest.Scalars3")
      val innerTy = schema.message("structtest.Inner3")

      using(fill(ty)) { msg =>
        val wire = msg.encode()
        check(
          java.util.Arrays.equals(wire, oracle),
          "encoded bytes differ from protoc (" + wire.length + " vs " + oracle.length + ")"
        )
        println("OK   encode matches protoc, " + oracle.length + " bytes")
      }

      using(ty.parse(oracle)) { decoded =>
        check(decoded.getLong("i32") == Int.MinValue.toLong, "i32")
        check(decoded.getBool("flag"), "flag")
        check(decoded.getString("str") == "hello", "str")
        check(decoded.getDouble("f64") == -123456.789, "f64")
        check(decoded.getLong("color") == 2L, "color")
        check(decoded.has("opt_str") && !ty.newMessage().has("i32"), "presence")
        check(decoded.getList("nums").asScala.toList == List(1L, -5L, 300L), "nums")
        check(decoded.getList("tags").asScala.toList == List("a", "bb"), "tags")
        check(decoded.getList("flags").asScala.toList == List(true, false, true), "flags")
        check(decoded.getMessage("inner").getString("name") == "kid", "inner.name")
        check(decoded.getList("inners").size() == 2, "inners")
        check(java.util.Arrays.equals(decoded.encode(), oracle), "decode -> encode is byte-exact")
        println("OK   decode round trip, repeated and nested fields")
      }

      // A field the schema does not declare (99, varint 7) survives re-encoding.
      val unknown = Array(0x98.toByte, 0x06.toByte, 0x07.toByte)
      using(innerTy.parse(unknown)) { decoded =>
        check(java.util.Arrays.equals(decoded.encode(), unknown), "unknown field preserved")
      }

      // Bad input is an error, not a crash.
      check(
        (try { ty.parse(Array(0x80.toByte)); false } catch { case _: RuntimeException => true }),
        "truncated tag accepted"
      )
      check(
        (try { schema.message("structtest.Nope"); false } catch { case _: RuntimeException => true }),
        "unknown message accepted"
      )
    }

    val edgeOracle = Files.readAllBytes(Paths.get(edgeDesc).resolveSibling("edge.bin"))
    using(schema(Paths.get(edgeDesc).toString)) { edgeSchema =>
      val edgeTy = edgeSchema.message("edge.Edge")

      val edge = edgeTy.build { m =>
        m.set("raw", Array(0x00.toByte, 0x01.toByte, 0xff.toByte))
        m.set("text", "h\u00e9llo \u2603")
        m.set("sign", -3L)
        m.set("maybe", "")
        m.set("pick_num", 5L) // oneof: replaced below
        m.set("pick_str", "one")
        List("a", "", "b").foreach(v => m.set("tags", v))
        m.set("blobs", Array(0x00.toByte))
        m.set("blobs", Array.emptyByteArray)
        m.message("inner") { c => c.set("tag", "kid"); c.set("n", -1L) }
      }
      using(edge) { e =>
        check(java.util.Arrays.equals(e.encode(), edgeOracle), "edge payload differs from protoc")
      }

      using(edgeTy.parse(edgeOracle)) { decoded =>
        check(
          java.util.Arrays.equals(
            decoded.getBytes("raw"),
            Array(0x00.toByte, 0x01.toByte, 0xff.toByte)
          ),
          "bytes with NUL and 0xff"
        )
        check(decoded.getString("text") == "h\u00e9llo \u2603", "non-ASCII string")
        check(decoded.getLong("sign") == -3L, "negative enum")
        check(decoded.getList("tags").asScala.toList == List("a", "", "b"), "empty repeated element")
        val blobs = decoded.getList("blobs").asScala.toList
        check(blobs(0).asInstanceOf[Array[Byte]].sameElements(Array(0x00.toByte)), "bytes element with NUL")
        check(blobs(1).asInstanceOf[Array[Byte]].isEmpty, "empty bytes element")
        check(decoded.getMessage("inner").getLong("n") == -1L, "nested negative")
        check(decoded.has("maybe") && decoded.getString("maybe") == "", "optional present and empty")
        check(decoded.has("pick_str") && !decoded.has("pick_num"), "oneof keeps only the last member")
        check(java.util.Arrays.equals(decoded.encode(), edgeOracle), "edge decode -> encode is byte-exact")
      }
      println("OK   edge payload: bytes, UTF-8, negative enum, empty elements, oneof")

      // A default message is zero bytes both ways.
      using(edgeTy.newMessage()) { e => check(e.encode().isEmpty, "empty message encodes to zero bytes") }
      using(edgeTy.decode(Array.emptyByteArray)) { empty =>
        check(empty.encode().isEmpty, "empty wire decodes and re-encodes to zero bytes")
        check(!empty.has("text"), "empty message has no fields")
      }

      // A wrong value kind is refused, not written.
      check(
        (try { edgeTy.newMessage().set("pick_num", "not a number"); false }
         catch { case e: PbwireException => e.getMessage.contains("needs a string") }),
        "wrong-typed value rejected"
      )
      check(
        (try { edgeTy.newMessage().newMessage("text"); false }
         catch { case e: PbwireException => e.getMessage.contains("not a message field") }),
        "newMessage on a scalar rejected"
      )
    }
    println("OK   " + checks + " checks passed")
  }
}

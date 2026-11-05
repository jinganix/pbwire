/*
 * The runnable round trip: builds a structtest.Scalars3 through the
 * dynamic API, encodes it, decodes it back and prints both. Run after
 * ./build.sh, from bindings/scala:
 *   java -cp out/classes Roundtrip [matrix3.desc]
 */
import Pbwire._

object Roundtrip {
  def main(args: Array[String]): Unit = {
    val desc = if (args.length > 0) args(0) else "../c/tests/data/matrix3.desc"
    using(schema(desc)) { schema =>
      val ty = schema.message("structtest.Scalars3")
      using(ty.build { m =>
        m.set("i32", -42L)
        m.set("str", "alice")
        List("a", "bb").foreach(v => m.set("tags", v))
        m.message("inner") { c => c.set("name", "kid"); c.set("value", 42L) }
      }) { msg =>
        val wire = msg.encode()
        println("encoded: " + wire.length + " bytes")
        using(ty.parse(wire)) { back =>
          assert(back.getLong("i32") == -42L)
          assert(back.getString("str") == "alice")
          assert(back.getMessage("inner").getString("name") == "kid")
          assert(java.util.Arrays.equals(back.encode(), wire))
          println("round trip ok: i32=" + back.getLong("i32") +
            " str=" + back.getString("str") +
            " inner=" + back.getMessage("inner").getString("name") +
            " tags=" + back.getList("tags"))
        }
      }
    }
  }
}

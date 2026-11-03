/*
 * A round trip over the pinned matrix3.desc: build, encode, decode, read back.
 *
 *   java --enable-native-access=ALL-UNNAMED -Dpbwire.library=... \
 *        -cp out/pbwire.jar:../../java/out RoundtripKt matrix3.desc
 */

fun main(args: Array<String>) {
    val desc = args.getOrNull(0) ?: "../c/tests/data/matrix3.desc"

    schema(desc).use { schema ->
        val ty = schema.message("structtest.Scalars3")

        ty.build {
            set("i32", -42L)
            set("str", "alice")
            set("flag", true)
            for (tag in listOf("a", "bb")) {
                set("tags", tag) // repeated: appends
            }
            message("inner") { // nested: created in place
                set("name", "kid")
                set("value", 42L)
            }
        }.use { msg ->
            val wire = msg.encode()
            println("${wire.size} bytes: ${wire.joinToString("") { "%02x".format(it) }}")

            ty.parse(wire).use { decoded ->
                println("i32   = ${decoded.getLong("i32")}")
                println("str   = ${decoded.getString("str")}")
                println("tags  = ${decoded.getList("tags")}")
                println("inner = ${decoded.getMessage("inner")?.getString("name")}")
                check(decoded.encode().contentEquals(wire)) { "round trip differs" }
            }
            println("OK   byte-exact round trip")
        }
    }
}

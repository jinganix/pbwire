/*
 * The C example (examples/addressbook.c), in Kotlin, through the generated
 * typed data classes: assign fields, encode, decode, compare. The encoded bytes
 * must equal the pinned reference examples/addressbook.bin.
 *
 * Run it after bindings/kotlin/build.sh:
 *   java --enable-native-access=ALL-UNNAMED -Dpbwire.library=... \
 *        -cp out/pbwire.jar:../java/out RoundtripKt ../../examples/addressbook.desc
 */

import java.nio.file.Files
import java.nio.file.Path

fun main(args: Array<String>) {
    val desc = args.firstOrNull() ?: "../../examples/addressbook.desc"
    val bin = Path.of(args.getOrNull(1) ?: "../../examples/addressbook.bin")

    Schema.fromFile(desc).use { schema ->
        val book = build()
        val wire = book.encode(schema)
        if (Files.exists(bin)) {
            check(wire.contentEquals(Files.readAllBytes(bin))) { "encode does not match $bin" }
        }

        val back = Tutorial_AddressBook.decode(schema, wire)
        println("addressbook: ${wire.size} bytes, ${back.people.size} people")
        for (p in back.people) {
            println("  ${p.name} (${p.id}) ${p.email ?: ""}")
            for (phone in p.phones) {
                println("    ${phone.number}")
            }
        }
    }
}

private fun build(): Tutorial_AddressBook = Tutorial_AddressBook(
    people = mutableListOf(
        Tutorial_Person(
            name = "Alice",
            id = 1,
            phones = mutableListOf(phone("555-0100", Tutorial_Person_PhoneType.MOBILE)),
        ),
        Tutorial_Person(
            name = "Bob",
            id = 2,
            hasEmail = true,
            email = "bob@example.com",
            phones = mutableListOf(phone("555-0200", Tutorial_Person_PhoneType.HOME)),
        ),
    ),
)

private fun phone(number: String, type: Int): Tutorial_Person_PhoneNumber =
    Tutorial_Person_PhoneNumber(number = number, hasType = true, type = type)

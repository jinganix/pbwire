/*
 * The C example (examples/addressbook.c), in Kotlin, through the generated typed
 * classes: build with the protobuf-java-shaped Builder (or the thin
 * `addressBook { }` DSL), encode, decode, compare. The encoded bytes must
 * equal the pinned reference examples/addressbook.bin. The schema travels
 * embedded in the generated Java code; no .desc file is read.
 *
 * Run it after bindings/kotlin/build.sh:
 *   java --enable-native-access=ALL-UNNAMED -Dpbwire.library=... \
 *        -cp out/pbwire.jar RoundtripKt ../../examples/addressbook.bin
 */

import java.nio.file.Files
import java.nio.file.Path

fun main(args: Array<String>) {
    val bin = Path.of(args.getOrNull(0) ?: "../../examples/addressbook.bin")

    val book = build()
    val wire = book.toByteArray()
    if (Files.exists(bin)) {
        check(wire.contentEquals(Files.readAllBytes(bin))) { "encode does not match $bin" }
    }

    val back = Tutorial_AddressBook.parseFrom(wire)
    println("addressbook: ${wire.size} bytes, ${back.peopleCount} people")
    for (p in back.peopleList) {
        println("  ${p.name} (${p.id}) ${if (p.hasEmail()) p.email else ""}")
        for (phone in p.phonesList) {
            println("    ${phone.number}")
        }
    }
}

private fun build(): Tutorial_AddressBook = addressBook {
    addPeople(person { setName("Alice"); setId(1); addPhones(phone("555-0100", Tutorial_Person_PhoneType.MOBILE)) })
    addPeople(person {
        setName("Bob")
        setId(2)
        setEmail("bob@example.com")
        addPhones(phone("555-0200", Tutorial_Person_PhoneType.HOME))
    })
}

private fun phone(number: String, type: Tutorial_Person_PhoneType): Tutorial_Person_PhoneNumber =
    personPhoneNumber {
        setNumber(number)
        setType(type)
    }

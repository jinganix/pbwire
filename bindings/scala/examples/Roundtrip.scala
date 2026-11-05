/*
 * The C example (examples/addressbook.c), in Scala, through the generated
 * case classes: assign fields, encode, decode, compare. The encoded bytes
 * must equal the pinned reference examples/addressbook.bin.
 *
 * Run after ./build.sh, from bindings/scala:
 *   java -cp out/classes RoundtripGen ../../examples/addressbook.bin
 */
import java.nio.file.{Files, Paths}
import java.util.Arrays
import addressbookGen._

object Roundtrip {
  def main(args: Array[String]): Unit = {
    val bin = Paths.get(if (args.length > 0) args(0) else "../../examples/addressbook.bin")

    def phone(number: String, kind: Int): Tutorial_Person_PhoneNumber =
      Tutorial_Person_PhoneNumber(number = number, `type` = Some(kind))

    val book = Tutorial_AddressBook(people = List(
      Tutorial_Person(name = "Alice", id = 1,
        phones = List(phone("555-0100", Tutorial_Person_PhoneType.MOBILE))),
      Tutorial_Person(name = "Bob", id = 2, email = Some("bob@example.com"),
        phones = List(phone("555-0200", Tutorial_Person_PhoneType.HOME)))
    ))

    val wire = Tutorial_AddressBook.encode(book)
    if (Files.exists(bin)) {
      val reference = Files.readAllBytes(bin)
      assert(Arrays.equals(wire, reference), "encode does not match " + bin)
    }

    val back = Tutorial_AddressBook.decode(wire)
    println("addressbook: " + wire.length + " bytes, " + back.people.length + " people")
    back.people.foreach { p =>
      println("  " + p.name + " (" + p.id + ") " + p.email.getOrElse(""))
      p.phones.foreach { ph => println("    " + ph.number) }
    }
  }
}

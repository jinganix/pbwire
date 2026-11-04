import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Arrays;

/**
 * The C example (examples/addressbook.c), in Java, through the generated typed
 * classes: assign typed fields, encode, decode, compare. The encoded bytes must
 * equal the pinned reference {@code examples/addressbook.bin}, so the example
 * doubles as a cross-language byte check.
 *
 * <p>Run it after bindings/java/build.sh:
 * {@code java ... -cp out Roundtrip ../../examples/addressbook.desc ../../examples/addressbook.bin}
 */
public final class Roundtrip {
    private Roundtrip() {
    }

    public static void main(String[] args) throws Exception {
        String desc = args.length > 0 ? args[0] : "../../examples/addressbook.desc";
        Path bin = Path.of(args.length > 1 ? args[1] : "../../examples/addressbook.bin");

        try (Schema schema = Schema.fromFile(desc)) {
            Tutorial_AddressBook book = build();
            byte[] wire = book.encode(schema);

            if (Files.exists(bin)) {
                byte[] reference = Files.readAllBytes(bin);
                if (!Arrays.equals(wire, reference)) {
                    throw new AssertionError("encode does not match " + bin);
                }
            }

            Tutorial_AddressBook back = Tutorial_AddressBook.decode(schema, wire);
            System.out.printf("addressbook: %d bytes, %d people%n", wire.length, back.people.size());
            for (Tutorial_Person p : back.people) {
                System.out.printf("  %s (%d) %s%n", p.name, p.id, p.hasEmail ? p.email : "");
                for (Tutorial_Person_PhoneNumber phone : p.phones) {
                    System.out.printf("    %s%n", phone.number);
                }
            }
        }
    }

    private static Tutorial_AddressBook build() {
        Tutorial_Person alice = new Tutorial_Person();
        alice.name = "Alice";
        alice.id = 1;
        alice.phones.add(phone("555-0100", Tutorial_Person_PhoneType.MOBILE));

        Tutorial_Person bob = new Tutorial_Person();
        bob.name = "Bob";
        bob.id = 2;
        bob.hasEmail = true;
        bob.email = "bob@example.com";
        bob.phones.add(phone("555-0200", Tutorial_Person_PhoneType.HOME));

        Tutorial_AddressBook book = new Tutorial_AddressBook();
        book.people.addAll(Arrays.asList(alice, bob));
        return book;
    }

    private static Tutorial_Person_PhoneNumber phone(String number, int type) {
        Tutorial_Person_PhoneNumber phone = new Tutorial_Person_PhoneNumber();
        phone.number = number;
        phone.hasType = true;
        phone.type = type;
        return phone;
    }
}

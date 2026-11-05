import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Arrays;

/**
 * The C example (examples/addressbook.c), in Java, through the generated typed
 * classes: build with the protobuf-java-shaped Builder, encode, decode,
 * compare. The encoded bytes must equal the pinned reference
 * {@code examples/addressbook.bin}, so the example doubles as a
 * cross-language byte check. The schema travels embedded in the generated
 * code; no .desc file is read.
 *
 * <p>Run it after bindings/java/build.sh:
 * {@code java ... -cp out Roundtrip ../../examples/addressbook.bin}
 */
public final class Roundtrip {
    private Roundtrip() {
    }

    public static void main(String[] args) throws Exception {
        Path bin = Path.of(args.length > 0 ? args[0] : "../../examples/addressbook.bin");

        Tutorial_AddressBook book = build();
        byte[] wire = book.toByteArray();

        if (Files.exists(bin)) {
            byte[] reference = Files.readAllBytes(bin);
            if (!Arrays.equals(wire, reference)) {
                throw new AssertionError("encode does not match " + bin);
            }
        }

        Tutorial_AddressBook back = Tutorial_AddressBook.parseFrom(wire);
        System.out.printf("addressbook: %d bytes, %d people%n", wire.length, back.getPeopleCount());
        for (Tutorial_Person p : back.getPeopleList()) {
            System.out.printf("  %s (%d) %s%n", p.getName(), p.getId(),
                    p.hasEmail() ? p.getEmail() : "");
            for (Tutorial_Person_PhoneNumber phone : p.getPhonesList()) {
                System.out.printf("    %s%n", phone.getNumber());
            }
        }
    }

    private static Tutorial_AddressBook build() {
        Tutorial_Person alice = Tutorial_Person.newBuilder()
                .setName("Alice")
                .setId(1)
                .addPhones(phone("555-0100", Tutorial_Person_PhoneType.MOBILE))
                .build();
        Tutorial_Person bob = Tutorial_Person.newBuilder()
                .setName("Bob")
                .setId(2)
                .setEmail("bob@example.com")
                .addPhones(phone("555-0200", Tutorial_Person_PhoneType.HOME))
                .build();
        return Tutorial_AddressBook.newBuilder().addPeople(alice).addPeople(bob).build();
    }

    private static Tutorial_Person_PhoneNumber phone(String number, Tutorial_Person_PhoneType type) {
        return Tutorial_Person_PhoneNumber.newBuilder().setNumber(number).setType(type).build();
    }
}

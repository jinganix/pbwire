import java.io.IOException;
import java.nio.file.Files;
import java.util.Arrays;

/** Build a message, encode it, decode it, and check that every field survived. */
public final class Roundtrip {
    private Roundtrip() {
    }

    public static void main(String[] args) throws IOException {
        if (args.length != 1) {
            System.err.println("usage: Roundtrip <descriptor-set>  (see tests/data/sample.desc)");
            System.exit(2);
        }
        try (Schema schema = Schema.fromFile(args[0])) {
            MessageType sample = schema.message("pbwire.test.Sample");
            byte[] wire;
            try (Message message = sample.newMessage()) {
                message.set("int32_field", -42)
                        .set("string_field", "alice")
                        .set("bool_field", true)
                        .set("color", 2)
                        .set("numbers", 1)
                        .set("numbers", 2);
                message.newMessage("inner").set("x", 7).set("tag", "nested");
                wire = message.encode();
            }
            System.out.println("encoded " + wire.length + " bytes: " + hex(wire));

            try (Message decoded = sample.decode(wire)) {
                if (decoded.getLong("int32_field") != -42
                        || !"alice".equals(decoded.getString("string_field"))
                        || !decoded.getBool("bool_field")
                        || decoded.getLong("color") != 2
                        || !decoded.getList("numbers").equals(java.util.List.of(1L, 2L))
                        || decoded.getMessage("inner").getLong("x") != 7) {
                    throw new AssertionError("round trip lost a field");
                }
                if (!Arrays.equals(wire, decoded.encode())) {
                    throw new AssertionError("re-encode of the decoded message differs");
                }
                System.out.println("decoded back: " + decoded.get("string_field") + ", inner.x = "
                        + decoded.getMessage("inner").getLong("x"));
            }
        }
        System.out.println("round trip ok");
    }

    private static String hex(byte[] bytes) {
        StringBuilder out = new StringBuilder(bytes.length * 2);
        for (byte b : bytes) {
            out.append(Character.forDigit((b >> 4) & 0xf, 16)).append(Character.forDigit(b & 0xf, 16));
        }
        return out.toString();
    }
}

import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Arrays;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.atomic.AtomicInteger;

/**
 * Self-check for the Java binding: plain assertions, no framework, no
 * dependencies. Exits 0 when every check passes.
 *
 * <p>Run it with the descriptor-set fixture:
 * {@code java --enable-native-access=ALL-UNNAMED -Dpbwire.library=... -cp out TestPbwire tests/data/sample.desc}
 */
public final class TestPbwire {
    private static int checks;

    private TestPbwire() {
    }

    public static void main(String[] args) throws IOException {
        String descriptorSet = args.length > 0 ? args[0] : "tests/data/sample.desc";

        try (Schema schema = Schema.fromFile(descriptorSet)) {
            check(schema.message("pbwire.test.Sample").name().equals("pbwire.test.Sample"), "descriptor set loads");
            roundTrip(schema);
            byteExactReEncode(schema);
            absentFields(schema);
            nested(schema);
            doubleClose(schema);
            unknownField(schema);
            malformedWire(schema);
            unknownPreserved(schema);
            unpackedRepeated(schema);
            singularMerge(schema);
            threads(schema);
        }

        try (Schema fromBytes = Schema.fromBytes(Files.readAllBytes(Path.of(descriptorSet)))) {
            check(fromBytes.message("pbwire.test.Inner").name().equals("pbwire.test.Inner"),
                    "descriptor set loads from bytes");
        }

        schemaError();
        System.out.println(checks + " checks passed");
    }

    private static void roundTrip(Schema schema) {
        MessageType sample = schema.message("pbwire.test.Sample");
        try (Message message = sample.newMessage()) {
            message.set("int32_field", -42)
                    .set("int64_field", 1L << 40)
                    .set("uint32_field", 4000000000L)
                    .set("uint64_field", Long.MAX_VALUE)
                    .set("sint32_field", -7)
                    .set("bool_field", true)
                    .set("float_field", 1.5)
                    .set("double_field", 2.25)
                    .set("string_field", "héllo")
                    .set("bytes_field", new byte[] {1, 2, 3})
                    .set("color", 2)
                    .set("numbers", 1)
                    .set("numbers", 2)
                    .set("numbers", 3);

            byte[] wire = message.encode();
            check(wire.length > 0, "encode writes bytes");

            try (Message decoded = sample.decode(wire)) {
                check(decoded.type().name().equals("pbwire.test.Sample"), "decoded message type");
                check(decoded.getInt("int32_field") == -42, "int32 round trip");
                check(decoded.getLong("int64_field") == 1L << 40, "int64 round trip");
                check(decoded.getLong("uint32_field") == 4000000000L, "uint32 round trip");
                check(decoded.getLong("uint64_field") == Long.MAX_VALUE, "uint64 round trip");
                check(decoded.getInt("sint32_field") == -7, "sint32 round trip");
                check(decoded.getBool("bool_field"), "bool round trip");
                check(decoded.getFloat("float_field") == 1.5f, "float round trip");
                check(decoded.getDouble("double_field") == 2.25, "double round trip");
                check("héllo".equals(decoded.getString("string_field")), "string round trip");
                check(Arrays.equals(new byte[] {1, 2, 3}, decoded.getBytes("bytes_field")), "bytes round trip");
                check(decoded.getInt("color") == 2, "enum round trip");
                check(decoded.getList("numbers").equals(List.of(1L, 2L, 3L)), "repeated round trip");
                check(decoded.get("int32_field").equals(-42L), "get by name");
                check(decoded.get(1).equals(-42L), "get by number");
                check(decoded.has("string_field"), "has() sees a set field");
            }
        }
    }

    private static void byteExactReEncode(Schema schema) {
        MessageType sample = schema.message("pbwire.test.Sample");
        Message message = sample.newMessage().set("string_field", "x").set("int32_field", 9).set("numbers", 4);
        byte[] wire = message.encode();
        message.close();
        try (Message decoded = sample.decode(wire)) {
            check(Arrays.equals(wire, decoded.encode()), "re-encode of decoded bytes is byte exact");
        }
    }

    private static void absentFields(Schema schema) {
        MessageType sample = schema.message("pbwire.test.Sample");
        try (Message empty = sample.newMessage()) {
            check(!empty.has("int32_field"), "absent field is not present");
            check(empty.get("int32_field") == null, "absent field reads null");
            check(empty.getLong("int32_field") == 0, "absent integer reads 0");
            check(empty.getString("string_field") == null, "absent string reads null");
            check(empty.getBytes("bytes_field") == null, "absent bytes reads null");
            check(empty.getMessage("inner") == null, "absent message reads null");
            check(empty.getList("numbers").isEmpty(), "absent repeated reads empty");
            check(empty.encode().length == 0, "empty message encodes to nothing");

            empty.set("int32_field", 5);
            check(empty.has("int32_field"), "set marks the field present");
            empty.clear("int32_field");
            check(!empty.has("int32_field"), "clear drops presence");
        }
    }

    private static void nested(Schema schema) {
        MessageType sample = schema.message("pbwire.test.Sample");
        try (Message message = sample.newMessage()) {
            message.newMessage("inner").set("x", 7).set("tag", "in");
            message.newMessage("inners").set("x", 1);
            message.newMessage("inners").set("x", 2);

            try (Message decoded = sample.decode(message.encode())) {
                check(decoded.getMessage("inner").getLong("x") == 7, "nested int round trip");
                check("in".equals(decoded.getMessage("inner").getString("tag")), "nested string round trip");
                check(decoded.getMessage("inner").type().name().equals("pbwire.test.Inner"), "nested type name");

                List<Object> inners = decoded.getList("inners");
                check(inners.size() == 2, "repeated message count");
                check(((Message) inners.get(0)).getLong("x") == 1, "repeated message item 0");
                check(((Message) inners.get(1)).getLong("x") == 2, "repeated message item 1");

                decoded.getMessage("inner").set("x", 8);
                check(decoded.getMessage("inner").getLong("x") == 8, "nested view writes into the tree");
            }
        }
    }

    private static void doubleClose(Schema schema) {
        MessageType sample = schema.message("pbwire.test.Sample");
        Message message = sample.newMessage().set("int32_field", 1);
        Message child = message.newMessage("inner");
        message.close();
        message.close();
        check(true, "double close is a no-op");

        try {
            message.getLong("int32_field");
            check(false, "a closed message must throw");
        } catch (IllegalStateException expected) {
            check(true, "a closed message throws");
        }
        try {
            child.getLong("x");
            check(false, "a child view of a closed tree must throw");
        } catch (IllegalStateException expected) {
            check(true, "a child view of a closed tree throws");
        }
    }

    private static void unknownField(Schema schema) {
        MessageType sample = schema.message("pbwire.test.Sample");
        try (Message message = sample.newMessage()) {
            try {
                message.set("no_such_field", 1);
                check(false, "an unknown field name must fail");
            } catch (PbwireException expected) {
                check(expected.getMessage().contains("no_such_field"), "the error names the field: " + expected);
            }
        }
    }

    private static void malformedWire(Schema schema) {
        MessageType sample = schema.message("pbwire.test.Sample");
        byte[] reference;
        try (Message message = sampleMessage(sample)) {
            reference = message.encode();
        }
        // A truncated payload, a lone tag, field 0 and a wire type above 5
        // must all fail (Python/Ruby already pin these; Java did not).
        byte[][] bad = {
            Arrays.copyOf(reference, reference.length - 1),
            new byte[] {0x08},
            new byte[] {0x00},
            new byte[] {0x0f},
        };
        for (byte[] wire : bad) {
            try (Message ignored = sample.decode(wire)) {
                check(false, "malformed wire must fail");
            } catch (PbwireException expected) {
                check(true, "malformed wire fails");
            }
        }
    }

    private static void unknownPreserved(Schema schema) {
        MessageType sample = schema.message("pbwire.test.Sample");
        byte[] reference;
        try (Message message = sampleMessage(sample)) {
            reference = message.encode();
        }
        // Field 111, varint 1: unknown to the schema, kept verbatim.
        byte[] withUnknown = Arrays.copyOf(reference, reference.length + 3);
        withUnknown[reference.length] = (byte) 0xF8;
        withUnknown[reference.length + 1] = 0x06;
        withUnknown[reference.length + 2] = 0x01;
        try (Message decoded = sample.decode(withUnknown)) {
            check(Arrays.equals(withUnknown, decoded.encode()),
                    "unknown fields survive a decode/encode round trip");
            check(decoded.getInt("int32_field") == -42, "known fields still read");
        }
    }

    private static void unpackedRepeated(Schema schema) {
        MessageType sample = schema.message("pbwire.test.Sample");
        // Field 13 (numbers) is packed in the schema, but every protobuf
        // parser also accepts the unpacked element form (Ruby pins this).
        byte[] unpacked = new byte[] {0x68, 0x01, 0x68, 0x02, 0x68, 0x03};
        try (Message decoded = sample.decode(unpacked)) {
            check(decoded.getList("numbers").equals(List.of(1L, 2L, 3L)),
                    "unpacked occurrences of a packed field are accepted");
        }
    }

    private static void singularMerge(Schema schema) {
        MessageType sample = schema.message("pbwire.test.Sample");
        // Field 12 (inner) seen twice merges instead of replacing
        // (Python pins this on its own fixture).
        byte[] wire = new byte[] {
            0x62, 0x02, 0x08, 0x07, // inner.x = 7
            0x62, 0x04, 0x12, 0x02, 0x69, 0x6e, // inner.tag = "in"
        };
        try (Message decoded = sample.decode(wire)) {
            check(decoded.getMessage("inner").getLong("x") == 7, "merged int survives");
            check("in".equals(decoded.getMessage("inner").getString("tag")),
                    "merged string survives");
        }
    }

    /**
     * A loaded schema and its message types are read-only, so threads can share
     * them; only a message (and the per-thread encode workspace) is confined.
     */    private static void threads(Schema schema) {
        MessageType sample = schema.message("pbwire.test.Sample");
        byte[] reference;
        try (Message message = sampleMessage(sample)) {
            reference = message.encode();
        }
        ExecutorService pool = Executors.newFixedThreadPool(4);
        AtomicInteger mismatches = new AtomicInteger();
        CountDownLatch done = new CountDownLatch(4);
        for (int thread = 0; thread < 4; thread++) {
            pool.execute(() -> {
                try {
                    for (int i = 0; i < 2_000; i++) {
                        try (Message built = sampleMessage(sample)) {
                            if (!Arrays.equals(built.encode(), reference)) {
                                mismatches.incrementAndGet();
                            }
                        }
                        try (Message decoded = sample.decode(reference)) {
                            if (decoded.getInt("int32_field") != -42
                                    || decoded.getList("numbers").size() != 3
                                    || decoded.getMessage("inner").getLong("x") != 7) {
                                mismatches.incrementAndGet();
                            }
                        }
                    }
                } finally {
                    done.countDown();
                }
            });
        }
        try {
            done.await();
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        } finally {
            pool.shutdown();
        }
        check(mismatches.get() == 0, "4 threads share one schema, each with its own messages");
    }

    /** A message with every field family the fixture has, for the thread check. */
    private static Message sampleMessage(MessageType sample) {
        Message message = sample.newMessage()
                .set("int32_field", -42)
                .set("int64_field", 1L << 40)
                .set("bool_field", true)
                .set("float_field", 1.5)
                .set("string_field", "héllo")
                .set("bytes_field", new byte[] {1, 2, 3})
                .set("color", 2)
                .set("numbers", 1)
                .set("numbers", 2)
                .set("numbers", 3);
        message.newMessage("inner").set("x", 7).set("tag", "in");
        return message;
    }

    private static void schemaError() {
        try {
            Schema.fromBytes(new byte[] {0x2a, (byte) 0xff, (byte) 0xff});
            check(false, "a malformed descriptor set must fail");
        } catch (PbwireException expected) {
            check(expected.status() > 0, "the failure carries a pbw_status");
            check(expected.getMessage().length() > "descriptor set: ".length(),
                    "the failure explains itself: " + expected.getMessage());
        }
    }

    private static void check(boolean condition, String what) {
        checks++;
        if (!condition) {
            throw new AssertionError("check failed: " + what);
        }
    }
}

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

/**
 * Self-check for the single-level wire scan ({@link Visit} and
 * {@code MessageType.visit}): plain assertions, no framework, no
 * dependencies. Exits 0 when every check passes.
 *
 * <p>Run it with the descriptor-set fixture:
 * {@code java -cp out/classes TestVisit tests/data/sample.desc}
 */
public final class TestVisit {
    private static int checks;

    private TestVisit() {
    }

    public static void main(String[] args) throws IOException {
        String descriptorSet = args.length > 0 ? args[0] : "tests/data/sample.desc";

        try (Schema schema = Schema.fromFile(descriptorSet)) {
            MessageType sample = schema.message("pbwire.test.Sample");
            fullScan(schema, sample);
            abort(sample);
            limits(sample);
            malformed(sample);
            hardened(sample);
            empty(sample);
        }

        System.out.println(checks + " checks passed");
    }

    /** One visit per field in wire order, with raw C-style values. */
    private static void fullScan(Schema schema, MessageType sample) {
        MessageType innerType = schema.message("pbwire.test.Inner");
        final byte[] wire;
        try (Message message = sample.newMessage()) {
            message.set("int32_field", -42)
                    .set("int64_field", 1L << 40)
                    .set("uint32_field", 4000000000L)
                    .set("uint64_field", Long.MAX_VALUE)
                    .set("sint32_field", -7)
                    .set("bool_field", true)
                    .set("float_field", 1.5)
                    .set("double_field", 2.25)
                    .set("string_field", "h\u00e9llo")
                    .set("bytes_field", new byte[] {1, 2, 3})
                    .set("color", 2)
                    .set("numbers", 1)
                    .set("numbers", 2)
                    .set("numbers", 3);
            message.newMessage("inner").set("x", 7).set("tag", "in");
            byte[] body = message.encode();
            // Unknown varint field 111 = 1, then an unknown group field 20
            // holding field 1 = 5 (sample.proto has no group type, so an
            // unknown group exercises the raw-frame path).
            byte[] unk = {(byte) 0xF8, 0x06, 0x01};
            byte[] grp = {(byte) 0xA3, 0x01, 0x08, 0x05, (byte) 0xA4, 0x01};
            byte[] both = new byte[body.length + unk.length + grp.length];
            System.arraycopy(body, 0, both, 0, body.length);
            System.arraycopy(unk, 0, both, body.length, unk.length);
            System.arraycopy(grp, 0, both, body.length + unk.length, grp.length);
            wire = both;
        }

        final List<Integer> numbers = new ArrayList<>();
        final List<Integer> wts = new ArrayList<>();
        final List<String> names = new ArrayList<>();
        final List<Long> u64s = new ArrayList<>();
        final List<byte[]> payloads = new ArrayList<>();
        int at = sample.visit(wire, (field, number, wireType, value) -> {
            numbers.add(number);
            wts.add(wireType);
            names.add(field == null ? null : field.fieldName);
            u64s.add(value.u64);
            if (value.data != null) {
                check(value.data == wire, "LEN/group value aliases the input");
                payloads.add(Arrays.copyOfRange(value.data, value.off,
                        value.off + value.len));
            } else {
                payloads.add(null);
            }
            return 0;
        });
        check(at == wire.length, "a full scan returns the wire length");

        // Schema order for the set fields, then the two appended unknowns.
        List<Integer> want = Arrays.asList(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11,
                12, 13, 111, 20);
        check(numbers.equals(want), "every field is visited once in wire order");

        // Raw wire bits: varints unfolded, no zigzag or sign extension.
        check(u64s.get(0) == -42L, "int32 keeps its raw sign-extended bits");
        check(u64s.get(1) == (1L << 40), "int64 raw bits");
        check(u64s.get(2) == 4000000000L, "uint32 raw bits");
        check(u64s.get(3) == Long.MAX_VALUE, "uint64 raw bits");
        check(u64s.get(4) == 13L, "sint32 stays zigzagged (no unzigzag)");
        check(u64s.get(5) == 1L, "bool raw bit");
        check(u64s.get(6) == (Float.floatToRawIntBits(1.5f) & 0xFFFFFFFFL),
                "float raw bits");
        check(u64s.get(7) == Double.doubleToRawLongBits(2.25), "double raw bits");
        check(u64s.get(10) == 2L, "enum raw number");

        // The packed repeated scalar arrives as one LEN field.
        check(wts.get(12) == Wire.WT_LEN, "packed numbers report wire type LEN");
        check(Arrays.equals(payloads.get(12), new byte[] {1, 2, 3}),
                "packed payload is the raw run");

        // The nested message arrives as an opaque LEN payload, not recursed.
        check(wts.get(11) == Wire.WT_LEN, "a nested message reports wire type LEN");
        try (Message inner = innerType.decode(payloads.get(11))) {
            check(inner.getLong("x") == 7, "LEN payload holds the nested message");
        }
        check("h\u00e9llo".equals(new String(payloads.get(8),
                StandardCharsets.UTF_8)), "string LEN payload");
        check(Arrays.equals(payloads.get(9), new byte[] {1, 2, 3}),
                "bytes LEN payload");

        // Unknown numbers resolve to a null field.
        check(names.get(13) == null, "unknown field 111 yields a null field");
        check(u64s.get(13) == 1L, "unknown varint raw bit");
        check(names.get(14) == null, "unknown group yields a null field");
        check(wts.get(14) == 3, "a group reports wire type START_GROUP");
        byte[] grp = {(byte) 0xA3, 0x01, 0x08, 0x05, (byte) 0xA4, 0x01};
        check(Arrays.equals(payloads.get(14), grp),
                "a group arrives as the exact raw wire slice");
    }

    /** A non-zero callback return aborts with that field's start offset. */
    private static void abort(MessageType sample) {
        final byte[] wire;
        try (Message message = sample.newMessage()) {
            message.set("int32_field", -42).set("int64_field", 1L << 40);
            wire = message.encode();
        }
        // Field 1 is tag 0x08 plus a 10-byte varint (negative int32 sign
        // extends), so field 2 starts at offset 11 with tag 0x10.
        final int[] calls = {0};
        int at = sample.visit(wire, (field, number, wireType, value) -> {
            calls[0]++;
            return calls[0] == 2 ? 7 : 0;
        });
        check(calls[0] == 2, "the scan stops at the aborting field");
        check(at == 11, "an abort returns the field's start offset");
        check(wire[at] == 0x10, "the offset points at the second field's tag");
    }

    /** A byte cap above the input fails, like the core failing at offset 0. */
    private static void limits(MessageType sample) {
        final byte[] wire;
        try (Message message = sample.newMessage()) {
            message.set("int32_field", 1).set("string_field", "way too long");
            wire = message.encode();
        }
        check(wire.length > 10, "the fixture exceeds the 10-byte cap");
        int at = sample.visit(wire, new Limits(10, 0), (field, number, wireType,
                value) -> 0);
        check(at == 0, "an over-cap input fails at offset 0");
    }

    /** Truncated payloads and field 0 fail at the field's start offset. */
    private static void malformed(MessageType sample) {
        final byte[] wire;
        try (Message message = sample.newMessage()) {
            message.set("int32_field", -42);
            wire = message.encode();
        }
        byte[][] bad = {
            Arrays.copyOf(wire, wire.length - 1),
            new byte[] {0x00},
            new byte[] {0x0f},
        };
        for (byte[] w : bad) {
            int at = sample.visit(w, (field, number, wireType, value) -> 0);
            check(at == 0, "malformed wire fails at the field's start");
        }
        // LEN length overrunning the input fails at the field's start too.
        final byte[] strWire;
        try (Message message = sample.newMessage()) {
            message.set("string_field", "hello");
            strWire = message.encode();
        }
        byte[] cut = Arrays.copyOf(strWire, strWire.length - 2);
        int at = sample.visit(cut, (field, number, wireType, value) -> 0);
        check(at == 0, "a truncated LEN payload fails at the field's start");
    }

    /** Fuzz-adjacent malformed wire: every case fails at the field's start. */
    private static void hardened(MessageType sample) {
        Visit.Visitor nop = (field, number, wireType, value) -> 0;

        // Field number 0, one-byte and two-byte tag forms.
        check(sample.visit(new byte[] {0x00}, nop) == 0,
                "field 0 fails at offset 0");
        check(sample.visit(new byte[] {(byte) 0x80, 0x00}, nop) == 0,
                "a two-byte field-0 tag fails at offset 0");

        // A top-level END_GROUP has no opener, even for a known field.
        check(sample.visit(new byte[] {0x0C}, nop) == 0,
                "a top-level end-group fails at offset 0");

        // Truncated fixed32 at the start and after a valid field.
        check(sample.visit(new byte[] {(byte) 0xD5, 0x05, 0x01, 0x02},
                nop) == 0, "a truncated fixed32 fails at offset 0");
        byte[] afterValid = {0x08, 0x01, (byte) 0xD5, 0x05, 0x01, 0x02};
        check(sample.visit(afterValid, nop) == 2,
                "a truncated fixed32 fails at its own start offset");

        // Truncated fixed64 (only 3 of 8 bytes present).
        check(sample.visit(new byte[] {(byte) 0xD1, 0x05, 0x01, 0x02, 0x03},
                nop) == 0, "a truncated fixed64 fails at offset 0");

        // Length prefix cut off mid-varint and before the body.
        check(sample.visit(new byte[] {0x42, (byte) 0x80}, nop) == 0,
                "a cut length prefix fails at offset 0");
        check(sample.visit(new byte[] {0x42}, nop) == 0,
                "a missing length fails at offset 0");

        // A varint past the 10-byte limit: ten continuations plus a closer.
        byte[] overlong = new byte[12];
        overlong[0] = 0x08;
        Arrays.fill(overlong, 1, 11, (byte) 0x80);
        overlong[11] = 0x01;
        check(sample.visit(overlong, nop) == 0,
                "an 11-byte varint fails at offset 0");

        // A group closed by another field's end tag, and one never closed.
        // Field 20 opens (0xA3); field 21 closes (0xAC) or nothing does.
        check(sample.visit(new byte[] {(byte) 0xA3, 0x08, 0x05, (byte) 0xAC},
                nop) == 0, "a mismatched group end fails at offset 0");
        check(sample.visit(new byte[] {(byte) 0xA3, 0x08, 0x05},
                nop) == 0, "an unclosed group fails at offset 0");

        // 300 nested groups exceed the 256 depth cap; the scan never recurses
        // into payloads, so the callback must not fire before the cap trips.
        byte[] deep = new byte[600];
        Arrays.fill(deep, 0, 300, (byte) 0x0B);
        Arrays.fill(deep, 300, 600, (byte) 0x0C);
        final int[] calls = {0};
        int at = sample.visit(deep, (field, number, wireType, value) -> {
            calls[0]++;
            return 0;
        });
        check(at == 0 && calls[0] == 0,
                "300-deep nesting fails at the depth cap with no callback");
    }

    /** Empty input visits nothing and reports length 0. */
    private static void empty(MessageType sample) {
        final int[] calls = {0};
        int at = sample.visit(new byte[0], (field, number, wireType, value) -> {
            calls[0]++;
            return 0;
        });
        check(at == 0 && calls[0] == 0, "empty input visits nothing");
    }

    private static void check(boolean condition, String what) {
        checks++;
        if (!condition) {
            throw new AssertionError("check failed: " + what);
        }
    }
}

import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Arrays;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * Coverage for the core-parity surface: delimited framing, deterministic
 * encode, decode limits, encoded sizes, JSON and text mappings, WKT helpers
 * and service descriptors. Plain assertions, no dependencies.
 *
 * <p>Run it with:
 * {@code java -cp out TestCoverage tests/data/sample.desc tests/data/svc.desc}
 */
public final class TestCoverage {
    private static int checks;

    private TestCoverage() {
    }

    public static void main(String[] args) throws Exception {
        String sampleDesc = args.length > 0 ? args[0] : "tests/data/sample.desc";
        String svcDesc = args.length > 1 ? args[1] : "tests/data/svc.desc";
        String mapDesc = args.length > 2 ? args[2] : "../../codegen/testdata/maptest.desc";

        try (Schema schema = Schema.fromFile(sampleDesc)) {
            sizeAndDeterministic(schema);
            delimited(schema);
            stream(schema);
            decodeLimits(schema, sampleDesc);
            json(schema);
            text(schema);
            textUnknownFlag(schema);
            invalidUtf8String(schema);
        }
        try (Schema schema = Schema.fromFile(svcDesc)) {
            wkt(schema);
            services(schema);
            nestedLimits(schema);
        }
        try (Schema schema = Schema.fromFile(mapDesc)) {
            deterministicMaps(schema.message("spectest.MapTest"));
        }
        String confDesc = args.length > 3 ? args[3] : "../../build/conformance.desc";
        if (java.nio.file.Files.exists(java.nio.file.Path.of(confDesc))) {
            try (Schema schema = Schema.fromFile(confDesc)) {
                wideVarintInt32(schema);
                nullMapValue(schema);
            }
        }
        typedJson();
        System.out.println(checks + " checks passed");
    }

    private static void check(boolean ok, String what) {
        checks++;
        if (!ok) {
            throw new AssertionError("check failed: " + what);
        }
    }

    private static void fails(Runnable r, String what) {
        checks++;
        try {
            r.run();
        } catch (PbwireException | IllegalArgumentException expected) {
            return;
        }
        throw new AssertionError("check failed (must fail): " + what);
    }

    private static Message sample(MessageType type) {
        Message m = type.newMessage()
                .set("int32_field", -42)
                .set("int64_field", 1L << 40)
                .set("bool_field", true)
                .set("float_field", 1.5)
                .set("double_field", 2.25)
                .set("string_field", "héllo")
                .set("bytes_field", new byte[] {1, 2, 3})
                .set("color", 2)
                .set("numbers", 1)
                .set("numbers", 2);
        m.newMessage("inner").set("x", 7).set("tag", "in");
        return m;
    }

    /* ---------------- sizes ---------------- */

    private static void sizeAndDeterministic(Schema schema) {
        MessageType type = schema.message("pbwire.test.Sample");
        try (Message m = sample(type)) {
            byte[] wire = m.encode();
            check(m.encodedSize() == wire.length, "encoded size matches");
            check(Arrays.equals(m.encodeDeterministic(), wire),
                    "deterministic encode is byte-identical");
        }
        try (Message empty = type.newMessage()) {
            check(empty.encodedSize() == 0, "empty message sizes to 0");
        }
    }

    /* ---------------- delimited ---------------- */

    private static void delimited(Schema schema) {
        MessageType type = schema.message("pbwire.test.Sample");
        byte[] wire;
        try (Message m = sample(type)) {
            wire = m.encode();
            byte[] frame = m.encodeDelimited();
            int llen = frame.length - wire.length;
            check(llen >= 1 && llen <= 5, "frame adds a length prefix");
            check(frame[0] == (byte) (wire.length & 0x7F) || llen > 1,
                    "frame length prefix is the body size");
            check(Arrays.equals(Arrays.copyOfRange(frame, llen, frame.length), wire),
                    "frame body is the encoding");
        }
        // Trailing bytes are left for the caller, with the consumed count.
        // Second frame: length 2, field 13 (numbers) varint 5.
        byte[] tail = new byte[] {0x02, 0x68, 0x05};
        byte[] two;
        try (Message m = sample(type)) {
            byte[] frame = m.encodeDelimited();
            two = new byte[frame.length + tail.length];
            System.arraycopy(frame, 0, two, 0, frame.length);
            System.arraycopy(tail, 0, two, frame.length, tail.length);
        }
        MessageType.Delimited d = type.decodeDelimited(two);
        try (Message first = d.message) {
            check(d.consumed == two.length - tail.length, "consumed is prefix + body");
            check(first.getInt("int32_field") == -42, "delimited body reads");
        }
        MessageType.Delimited second = type.decodeDelimited(two, d.consumed,
                Limits.defaults());
        try (Message msg = second.message) {
            check(second.consumed == tail.length, "second frame consumed");
            check(msg.getList("numbers").equals(List.of(5L)),
                    "second frame body reads");
        }
        fails(() -> type.decodeDelimited(new byte[] {0x05}), "truncated frame fails");
        fails(() -> type.decodeDelimited(wire), "bare message is no frame");
        // A declared frame past the byte cap fails before the body decodes.
        fails(() -> type.decodeDelimited(two, 0, new Limits(1, 0)),
                "delimited frame honors max_bytes");
    }

    private static void stream(Schema schema) {
        MessageType type = schema.message("pbwire.test.Sample");
        byte[] first;
        byte[] second;
        try (Message m = sample(type)) {
            first = m.encodeDelimited();
        }
        try (Message m = type.newMessage().set("int32_field", 3)) {
            second = m.encodeDelimited();
        }
        GenericCodec.Stream st = new GenericCodec.Stream();
        st.feed(first, 0, 2); // split the length varint and the body
        check(st.next(type.def) == null, "split frame waits for more");
        st.feed(first, 2, first.length - 2);
        GenericCodec.DMsg m = st.next(type.def);
        check(m != null && ((Long) m.get(1)).intValue() == -42, "stream decodes frame 1");
        check(st.next(type.def) == null, "drained stream returns null");
        // Split across feed calls, body first byte by byte.
        st.feed(second, 0, 1);
        check(st.next(type.def) == null, "split body waits for more");
        for (int i = 1; i < second.length; i++) {
            st.feed(second, i, 1);
        }
        GenericCodec.DMsg n = st.next(type.def);
        check(n != null && ((Long) n.get(1)).intValue() == 3, "stream decodes frame 2");
        check(st.buffered() == 0, "drained stream holds nothing");
        st.reset();
        // A corrupt frame leaves the buffer untouched for reset-and-carry-on.
        st.feed(new byte[] {(byte) 0xFF, (byte) 0xFF, (byte) 0xFF, (byte) 0xFF, 0x7F});
        final GenericCodec.Stream stuck = st;
        fails(() -> stuck.next(type.def), "5-byte unterminated length is corrupt");
        check(st.buffered() == 5, "failed frame stays buffered");
        st.reset();
        check(st.buffered() == 0, "reset drops the bytes");
    }

    /* ---------------- limits ---------------- */

    private static void decodeLimits(Schema schema, String sampleDesc) {
        MessageType type = schema.message("pbwire.test.Sample");
        byte[] wire;
        try (Message m = sample(type)) {
            wire = m.encode();
        }
        fails(() -> type.decode(wire, new Limits(wire.length - 1, 0)),
                "decode honors max_bytes");
        try (Message m = type.decode(wire, new Limits(wire.length, 0))) {
            check(m.getInt("int32_field") == -42, "exact byte cap passes");
        }
        final String desc = sampleDesc;
        fails(() -> {
            try {
                Schema.fromBytes(Files.readAllBytes(Path.of(desc)), new Limits(10, 0));
            } catch (java.io.IOException e) {
                throw new RuntimeException(e);
            }
        }, "descriptor load honors max_bytes");
    }

    /* A recursive message exercises the nesting cap on every format. */

    /**
     * An int32 field decoded from a varint wider than 32 bits re-encodes as
     * the sign-extended int32 (10 bytes), not as the raw wire value: the
     * reference truncates and canonicalizes the same way.
     */
    private static void wideVarintInt32(Schema schema) {
        MessageType type = schema.message("protobuf_test_messages.proto3.TestAllTypesProto3");
        byte[] wide = {0x08, (byte) 0xFF, (byte) 0xFF, (byte) 0xFF, (byte) 0xFF, 0x1F};
        try (Message m = type.decode(wide)) {
            check(m.getLong("optional_int32") == -1L, "wide varint truncates to int32");
            byte[] out = m.encode();
            check(out.length == 11 && out[0] == 0x08 && out[10] == 0x01,
                    "wide varint re-encodes as a 10-byte int32");
            for (int i = 1; i <= 9; i++) {
                check(out[i] == (byte) 0xFF, "sign-extended byte " + i);
            }
        }
        // A varint of 2^33 truncates to an int32 zero: implicit presence drops
        // the field, so the re-encode is empty (the reference does the same).
        byte[] zero = {0x08, (byte) 0x80, (byte) 0x80, (byte) 0x80, (byte) 0x80, 0x20};
        try (Message m = type.decode(zero)) {
            check(!m.has("optional_int32"), "truncated-to-zero int32 is absent");
            check(m.encode().length == 0, "absent zero field re-encodes to nothing");
        }
    }

    /** Text printing honours the unknown-field flag (the runner's
     *  print_unknown_fields, which the conformance testee forwards). */
    private static void textUnknownFlag(Schema schema) {
        MessageType type = schema.message("pbwire.test.Sample");
        // Field 111, varint 2: unknown to the schema, kept verbatim.
        byte[] wire = {0x08, 0x01, (byte) 0xF8, 0x06, 0x02};
        try (Message m = type.decode(wire)) {
            check(m.toText(true).contains("111: 2"), "unknown field prints");
            check(!m.toText(false).contains("111"), "unknown field drops");
        }
    }

    /** A proto3 string that is not valid UTF-8 fails the decode, like the
     *  reference parser, instead of turning into U+FFFD on access. */
    private static void invalidUtf8String(Schema schema) {
        MessageType type = schema.message("pbwire.test.Sample");
        byte[] wire = {0x4A, 0x02, 0x68, (byte) 0x92}; // field 9, "h" + 0x92
        checkThrows(() -> type.decode(wire), "decode rejects invalid UTF-8");
    }

    private static void checkThrows(Runnable op, String what) {
        checks++;
        try {
            op.run();
        } catch (RuntimeException expected) {
            return;
        }
        throw new AssertionError("check failed (must fail): " + what);
    }

    /** A null map value is invalid JSON, unlike a null singular field. */
    private static void nullMapValue(Schema schema) {
        MessageType type = schema.message("protobuf_test_messages.proto3.TestAllTypesProto3");
        fails(() -> type.parseJson("{\"mapInt32Int32\": {\"0\": null}}"),
                "a null map value is rejected");
        try (Message m = type.parseJson("{\"optional_int32\": null}")) {
            check(!m.has("optional_int32"), "a null singular field is unset");
        }
    }

    private static void nestedLimits(Schema schema) {
        MessageType node = schema.message("pbwire.test.Node");
        Message root = node.newMessage().set("v", 0);
        Message at = root;
        for (int i = 1; i < 5; i++) {
            at = at.newMessage("next").set("v", i);
        }
        byte[] wire = root.encode();
        root.close();
        fails(() -> node.decode(wire, new Limits(0, 3)), "binary honors max_depth");
        try (Message m = node.decode(wire, new Limits(0, 10))) {
            check(m.getMessage("next").getMessage("next").getInt("v") == 2,
                    "cap above nesting passes");
        }
        String json = "{\"next\":{\"next\":{\"next\":{}}}}";
        fails(() -> node.parseJson(json, false, new Limits(0, 2)),
                "JSON honors max_depth");
        try (Message m = node.parseJson(json, false, new Limits(0, 10))) {
            check(m.has("next"), "deep JSON passes a high cap");
        }
        String text = "next { next { next { } } }";
        fails(() -> node.parseText(text, new Limits(0, 1)), "text honors max_depth");
        try (Message m = node.parseText(text, new Limits(0, 10))) {
            check(m.has("next"), "deep text passes a high cap");
        }
    }

    /* ---------------- JSON ---------------- */

    private static void json(Schema schema) {
        MessageType type = schema.message("pbwire.test.Sample");
        String printed;
        try (Message m = sample(type)) {
            printed = m.toJson();
            check(printed.contains("\"int32Field\":-42"), "int32 prints camelCase");
            check(printed.contains("\"int64Field\":\"" + (1L << 40) + "\""),
                    "int64 prints quoted");
            check(printed.contains("\"stringField\":\"h\u00e9llo\""), "string prints");
            check(printed.contains("\"bytesField\":\"AQID\""), "bytes print base64");
            check(printed.contains("\"numbers\":[1,2]"), "repeated prints");
            check(printed.contains("\"inner\":{\"x\":7"), "nested prints");
        }
        try (Message m = type.parseJson(printed)) {
            try (Message ref = sample(type)) {
                check(Arrays.equals(m.encode(), ref.encode()), "JSON round trip");
            }
        }
        // The legacy spellings: snake_case, quoted 64-bit, base64, nulls,
        // single repeated values and unknown skipping.
        String legacy = "{'int32_field':\"-42\", \"int64Field\": 1099511627776, "
                + "\"bool_field\": \"true\", \"float_field\": 1.5, "
                + "\"double_field\": \"Infinity\", \"string_field\": null, "
                + "\"bytes_field\": \"AQID\", "
                + "\"inner\": {\"x\": 1, \"tag\": \"a\\u00e9\"}}";
        try (Message m = type.parseJson(legacy, true, Limits.defaults())) {
            check(m.getInt("int32_field") == -42, "quoted int32 parses");
            check(m.getLong("int64_field") == 1L << 40, "bare int64 parses");
            check(m.getBool("bool_field"), "quoted bool parses");
            check(Double.isInfinite(m.getDouble("double_field")), "Infinity parses");
            check(m.getString("string_field") == null, "null means unset");
            check(Arrays.equals(m.getBytes("bytes_field"), new byte[] {1, 2, 3}),
                    "base64 parses");
            fails(() -> type.parseJson("{\"numbers\": 9}"),
                    "a repeated field wants an array");
            check("a\u00e9".equals(m.getMessage("inner").getString("tag")),
                    "\\u escape parses");
        }
        fails(() -> type.parseJson("{\"nope\": 1}"), "unknown JSON field fails");
        try (Message m = type.parseJson("{\"nope\": 1}", true, Limits.defaults())) {
            check(m.encode().length == 0, "unknown JSON field skips");
        }
        fails(() -> type.parseJson("{\"int32_field\": 9999999999}"), "int32 range fails");
        fails(() -> type.parseJson("{\"int32_field\":}"), "malformed JSON fails");
        fails(() -> type.parseJson(legacy, false, new Limits(10, 0)),
                "JSON honors max_bytes");
    }

    /* ---------------- text ---------------- */

    private static void text(Schema schema) {
        MessageType type = schema.message("pbwire.test.Sample");
        String printed;
        try (Message m = sample(type)) {
            printed = m.toText();
            check(printed.contains("int32_field: -42\n"), "int32 prints");
            check(printed.contains("inner {\n"), "nested prints braced");
            check(printed.contains("  x: 7\n"), "nested indents");
            check(printed.contains("numbers: 1\n"), "repeated prints per line");
        }
        try (Message m = type.parseText(printed)) {
            try (Message ref = sample(type)) {
                check(Arrays.equals(m.encode(), ref.encode()), "text round trip");
            }
        }
        // Optional colons, <> delimiters, lists, hex/octal, escapes, comments.
        String loose = "# a comment\n"
                + "int32_field: 0x2A\n"
                + "sint32_field: 010\n"
                + "uint64_field: 18446744073709551615\n"
                + "inner <x: 1 tag: \"a\\n\\u00e9\"> \n"
                + "numbers: [1, 2]\n"
                + "color: 2\n";
        try (Message m = type.parseText(loose)) {
            check(m.getInt("int32_field") == 42, "hex parses");
            check(m.getInt("sint32_field") == 8, "octal parses");
            check(m.getLong("uint64_field") == -1L, "full-range uint64 parses");
            check(m.getMessage("inner").getInt("x") == 1, "<> delimits");
            check("a\n\u00e9".equals(m.getMessage("inner").getString("tag")),
                    "escapes parse");
            check(m.getList("numbers").equals(List.of(1L, 2L)), "list form parses");
        }
        fails(() -> type.parseText("nope: 1"), "unknown text field fails");
        fails(() -> type.parseText("int32_field 1"), "missing colon fails");
        fails(() -> type.parseText(loose, new Limits(10, 0)), "text honors max_bytes");
    }

    /* ---------------- WKT ---------------- */

    private static void wkt(Schema schema) {
        MessageType stamp = schema.message("pbwire.test.Stamp");
        MessageType any = schema.message("pbwire.test.AnyMsg");
        MessageType mask = schema.message("pbwire.test.Mask");

        try (Message ts = Wkt.timestampFromMillis(stamp, 1500)) {
            check(Wkt.timestampToMillis(ts) == 1500, "timestamp round trip");
            check(ts.getLong(1) == 1 && ts.getLong(2) == 500000000,
                    "timestamp shape is seconds + nanos");
        }
        try (Message ts = Wkt.timestampFromMillis(stamp, -1500)) {
            check(Wkt.timestampToMillis(ts) == -1500, "negative timestamp borrows");
            check(ts.getLong(2) >= 0, "timestamp nanos stay non-negative");
        }
        try (Message d = Wkt.durationFromMillis(stamp, -1500)) {
            check(Wkt.durationToMillis(d) == -1500, "duration keeps the sign");
            check(d.getLong(1) == -1 && d.getLong(2) == -500000000,
                    "duration shape keeps both signs");
        }
        try (Message bad = stamp.newMessage().set(1, 1).set(2, 2000000000L)) {
            fails(() -> Wkt.timestampToMillis(bad), "nanos range fails");
        }
        try (Message noSec = stamp.newMessage().set(2, 0)) {
            fails(() -> Wkt.timestampToMillis(noSec), "missing seconds fails");
        }

        // Seconds are nonzero so the proto3 field is present on the wire.
        try (Message payload = Wkt.timestampFromMillis(stamp, 42000);
                Message packed = Wkt.anyPackMessage(any,
                        "type.googleapis.com/pbwire.test.Stamp", payload)) {
            check("type.googleapis.com/pbwire.test.Stamp".equals(Wkt.anyTypeUrl(packed)),
                    "type URL reads");
            check(Wkt.anyPayload(packed) != null, "payload reads");
            try (Message out = Wkt.anyUnpack(packed, stamp)) {
                check(Wkt.timestampToMillis(out) == 42000, "Any unpacks");
            }
            fails(() -> Wkt.anyUnpack(packed, mask), "Any type mismatch fails");
        }

        try (Message m = Wkt.newFieldMask(mask, List.of("a.b", "c"))) {
            check(Wkt.fieldMaskPaths(m).equals(List.of("a.b", "c")), "mask paths read");
            try (Message parsed = mask.decode(m.encode())) {
                check(Wkt.fieldMaskPaths(parsed).equals(List.of("a.b", "c")),
                        "mask round trips");
            }
        }
    }

    /* ---------------- services ---------------- */

    /** Deterministic encoding sorts map entries by key: the same message
     *  built in a different insertion order encodes to the same bytes. */
    private static void deterministicMaps(MessageType type) {
        Map<Long, String> intsA = new LinkedHashMap<>();
        intsA.put(1L, "one");
        intsA.put(-5L, "neg");
        Map<String, Long> strsA = new LinkedHashMap<>();
        strsA.put("a", 10L);
        strsA.put("b", -3L);
        Map<Long, String> intsB = new LinkedHashMap<>();
        intsB.put(-5L, "neg");
        intsB.put(1L, "one");
        Map<String, Long> strsB = new LinkedHashMap<>();
        strsB.put("b", -3L);
        strsB.put("a", 10L);
        Message first = type.newMessage();
        fillMaps(first, intsA, strsA);
        Message second = type.newMessage();
        fillMaps(second, intsB, strsB);
        byte[] a = first.encodeDeterministic();
        byte[] b = second.encodeDeterministic();
        check(Arrays.equals(a, b), "deterministic maps sort by key");
        // Plain encode keeps insertion order, so the two differ.
        check(!Arrays.equals(first.encode(), second.encode()),
                "plain encode keeps insertion order");
        first.close();
        second.close();
    }

    private static void fillMaps(Message m, Map<Long, String> ints,
            Map<String, Long> strs) {
        for (Map.Entry<Long, String> e : ints.entrySet()) {
            m.newMessage("by_int").set("key", e.getKey())
                    .set("value", e.getValue());
        }
        for (Map.Entry<String, Long> e : strs.entrySet()) {
            m.newMessage("by_str").set("key", e.getKey())
                    .set("value", e.getValue().longValue());
        }
    }

    private static void services(Schema schema) {
        ServiceType search = schema.service("pbwire.test.Search");
        check(search.name().equals("pbwire.test.Search"), "service name");
        ServiceType.MethodType find = search.method("Find");
        check(find.inputType().name().equals("pbwire.test.SearchRequest"),
                "input type links");
        check(find.outputType().name().equals("pbwire.test.SearchResponse"),
                "output type links");
        check(!find.clientStreaming() && !find.serverStreaming(),
                "unary has no streaming flags");
        ServiceType.MethodType watch = search.method("Watch");
        check(watch.clientStreaming() && watch.serverStreaming(),
                "bidi keeps both streaming flags");
        fails(() -> schema.service("pbwire.test.Missing"), "unknown service fails");
        fails(() -> search.method("Missing"), "unknown method fails");

        // The linked types are usable message types.
        try (Message req = find.inputType().newMessage()
                .set("query", "x").set("limit", 3)) {
            try (Message parsed = find.inputType().decode(req.encode())) {
                check("x".equals(parsed.getString("query")), "linked type decodes");
            }
        }
    }

    /* ---------------- typed path ---------------- */

    private static void typedJson() {
        // The generated oneof model drives the typed JSON/delimited helpers.
        Meta<Spectest_Oneof1OrBuilder, Spectest_Oneof1.Builder> meta =
                Spectest_Oneof1Meta.META;
        Spectest_Oneof1 msg = Spectest_Oneof1.newBuilder().setNum(7).build();
        check(PbCodec.encodedSize(msg, meta) == PbCodec.encode(msg, meta).length,
                "typed size matches");
        check(Arrays.equals(PbCodec.encodeDeterministic(msg, meta),
                PbCodec.encode(msg, meta)), "typed deterministic matches");
        String json = PbCodec.toJson(msg, meta);
        check(json.contains("\"num\":7"), "typed message prints JSON: " + json);
        Spectest_Oneof1 back = PbCodec.fromJson(json, Spectest_Oneof1::newBuilder,
                meta, Spectest_Oneof1.Builder::build);
        check(Arrays.equals(PbCodec.encode(back, meta), PbCodec.encode(msg, meta)),
                "typed JSON round trips");
        byte[] frame = PbCodec.encodeDelimited(msg, meta);
        PbCodec.DelimitedFrame<Spectest_Oneof1> d = PbCodec.parseDelimitedFrom(frame,
                0, Spectest_Oneof1::newBuilder, meta, Spectest_Oneof1.Builder::build);
        check(d.consumed == frame.length, "typed delimited consumes the frame");
        check(d.message.getNum() == 7, "typed delimited body reads");
        String text = PbCodec.toText(msg, meta);
        Spectest_Oneof1 fromText = PbCodec.fromText(text, Spectest_Oneof1::newBuilder,
                meta, Spectest_Oneof1.Builder::build);
        check(Arrays.equals(PbCodec.encode(fromText, meta), PbCodec.encode(msg, meta)),
                "typed text round trips");
    }

}

package mvp;

import com.google.protobuf.DescriptorProtos;
import com.google.protobuf.Descriptors;
import com.google.protobuf.DynamicMessage;

import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Arrays;
import java.util.List;

import structtest.Matrix3;

/*
 * Full-version benchmark + correctness gate for GenericCodec.
 *
 * Timed (matrix3, 4 tiers): generic codec vs protobuf-java generated code
 * vs DynamicMessage, same payload, best of 3. Correctness (un-timed):
 * proto2 round trip against matrix2.bin, oneof against edge.bin, groups +
 * extensions cross-checked through DynamicMessage, unknown-field retention,
 * and the required-field gate.
 */
public final class GenericBench {
    private GenericBench() {
    }

    private static final String[] CASES = {"small", "medium", "large", "xlarge"};
    private static final int[] REPEATS = {0, 1, 100, 3000};
    private static final int[] WANT = {136, 174, 2469, 74913};
    private static final int[] ITERATIONS = {50_000, 50_000, 5_000, 500};
    private static final double MIN_PASS_SECONDS = 0.25;
    private static final int PASSES = 3;

    private static int failures;

    public static void main(String[] args) throws Exception {
        if (args.length != 6) {
            System.err.println("usage: GenericBench <matrix3.desc> <matrix2.desc>"
                    + " <matrix2.bin> <edge.desc> <edge.bin>"
                    + " <desc_proto2.desc>");
            System.exit(2);
        }
        GenericCodec.Schema schema = loadSet(args[0]);
        GenericCodec.MessageDef scalars3 =
                schema.message("structtest.Scalars3");
        System.out.println("schema ok: " + scalars3.fields.length
                + " fields for structtest.Scalars3");

        String only = System.getenv("TIER"); // fast loop: TIER=xlarge
        for (int tier = 0; tier < CASES.length; tier++) {
            if (only != null && !only.isEmpty()
                    && !CASES[tier].equals(only)) {
                continue;
            }
            runCase(schema, scalars3, tier);
        }
        proto2Gate(args[1], args[2]);
        oneofGate(args[3], args[4]);
        groupExtGate(args[5]);
        unknownGate(schema, scalars3);

        if (failures != 0) {
            System.err.println(failures + " check(s) failed");
            System.exit(1);
        }
        System.out.println("all gates passed");
    }

    private static GenericCodec.Schema loadSet(String path) throws Exception {
        byte[] desc = Files.readAllBytes(Path.of(path));
        return GenericCodec.Schema.load(
                DescriptorProtos.FileDescriptorSet.parseFrom(desc));
    }

    /* ---------------- timed: matrix3 ---------------- */

    private static void runCase(GenericCodec.Schema schema,
            GenericCodec.MessageDef scalars3, int tier) throws Exception {
        String label = CASES[tier];
        int n = REPEATS[tier];
        int floor = ITERATIONS[tier];

        byte[] reference = fillOfficial(n).toByteArray();
        if (reference.length != WANT[tier]) {
            fail(label, "official wrote " + reference.length);
            return;
        }
        GenericCodec.DMsg data = fillGeneric(scalars3, n);
        if (!Arrays.equals(GenericCodec.encode(data), reference)) {
            fail(label, "generic bytes differ");
            return;
        }
        GenericCodec.DMsg back = GenericCodec.decode(scalars3, reference);
        if (!Arrays.equals(GenericCodec.encode(back), reference)) {
            fail(label, "generic round trip differs");
            return;
        }
        checkDecoded(back, n, label);
        if (failures != 0) {
            return;
        }
        System.out.printf("OK   %s parity, x%d (%d bytes)%n", label, n,
                reference.length);

        Descriptors.Descriptor desc = scalars3.descriptor;
        DynamicMessage dynBuilt =
                DynamicMessage.parseFrom(desc, reference); // validates schema
        if (!Arrays.equals(dynBuilt.toByteArray(), reference)) {
            fail(label, "DynamicMessage bytes differ");
            return;
        }

        Matrix3.Scalars3 officialMessage = fillOfficial(n);
        // Interleaved passes: a busy machine hits every candidate equally,
        // so a load spike cannot flatter one op and punish the next.
        Runnable[] ops = {
            () -> GenericCodec.encode(data),
            () -> GenericCodec.decode(scalars3, reference),
            officialMessage::toByteArray,
            () -> parse(reference),
            dynBuilt::toByteArray,
            () -> parseDyn(desc, reference),
        };
        double[] best = measureAll(floor, ops);
        double genEnc = best[0];
        double genDec = best[1];
        double offEnc = best[2];
        double offDec = best[3];
        double dynEnc = best[4];
        double dynDec = best[5];

        System.out.printf("%s: %d bytes, floor %d iterations (repeat field x%d)%n",
                label, reference.length, floor, n);
        System.out.printf("  %-14s %-7s %12s %10s%n", "impl", "op", "ns/op", "MB/s");
        row("generic-java", "encode", reference.length, genEnc);
        row("protobuf-java", "encode", reference.length, offEnc);
        row("dynamicmessage", "encode", reference.length, dynEnc);
        row("generic-java", "decode", reference.length, genDec);
        row("protobuf-java", "decode", reference.length, offDec);
        row("dynamicmessage", "decode", reference.length, dynDec);
        System.out.println();
    }

    /* ---------------- correctness gates ---------------- */

    /** proto2: required/optional/defaults/unpacked-vs-packed/negative enum. */
    private static void proto2Gate(String descPath, String binPath)
            throws Exception {
        GenericCodec.Schema s = loadSet(descPath);
        GenericCodec.MessageDef def = s.message("structtest.Scalars2");
        byte[] ref = Files.readAllBytes(Path.of(binPath));
        GenericCodec.DMsg m = GenericCodec.decode(def, ref);
        check("proto2 id", ((Number) m.get("id")).intValue() == 99);
        check("proto2 explicit default", // i32: -5 present, == default -5
                m.has("i32") && ((Number) m.get("i32")).intValue() == -5);
        check("proto2 packed",
                ((List<?>) m.getList(num(def, "packed_nums"))).size() > 0);
        if (!Arrays.equals(GenericCodec.encode(m), ref)) {
            fail("proto2", "round trip differs");
            return;
        }
        // Required gate: dropping a required field must fail on encode.
        GenericCodec.DMsg bad = GenericCodec.decode(def, ref);
        bad.clear("id");
        try {
            GenericCodec.encode(bad);
            fail("proto2", "missing required encoded silently");
        } catch (IllegalStateException expected) {
            System.out.println("OK   proto2 required gate + round trip");
        }
    }

    /** oneof: last-wins decode, switching set, byte-exact re-encode. */
    private static void oneofGate(String descPath, String binPath)
            throws Exception {
        GenericCodec.Schema s = loadSet(descPath);
        GenericCodec.MessageDef def = s.message("edge.Edge");
        byte[] ref = Files.readAllBytes(Path.of(binPath));
        GenericCodec.DMsg m = GenericCodec.decode(def, ref);
        int which = m.oneofCase("choice");
        check("oneof case set", which != 0);
        if (!Arrays.equals(GenericCodec.encode(m), ref)) {
            fail("oneof", "round trip differs");
            return;
        }
        // Switching members clears the previous one.
        m.set("pick_num", 7);
        check("oneof switch", m.oneofCase("choice") == num(def, "pick_num")
                && !m.has("pick_str"));
        System.out.println("OK   oneof gate (case=" + which + ") + round trip");
    }

    /** groups + extensions: round trip + DynamicMessage cross-check. */
    private static void groupExtGate(String descPath) throws Exception {
        GenericCodec.Schema s = loadSet(descPath);
        GenericCodec.MessageDef grouped = s.message("pbwire.test.Grouped");
        GenericCodec.MessageDef old = s.message("pbwire.test.Old");
        Descriptors.Descriptor gd = grouped.descriptor;
        DynamicMessage.Builder b = DynamicMessage.newBuilder(gd);
        Descriptors.FieldDescriptor inner = gd.findFieldByName("inner");
        DynamicMessage.Builder ib = DynamicMessage.newBuilder(
                inner.getMessageType());
        ib.setField(inner.getMessageType().findFieldByName("x"), 42);
        b.setField(inner, ib.build());
        b.setField(gd.findFieldByName("after"), 7);
        byte[] ref = b.build().toByteArray();

        GenericCodec.DMsg m = GenericCodec.decode(grouped, ref);
        GenericCodec.DMsg gm = (GenericCodec.DMsg) m.get("inner");
        check("group value", gm != null
                && ((Number) gm.get("x")).intValue() == 42);
        if (!Arrays.equals(GenericCodec.encode(m), ref)) {
            fail("groupext", "group round trip differs");
            return;
        }

        // Extensions by number through the generic model (id: required).
        GenericCodec.DMsg e = new GenericCodec.DMsg(old);
        e.set("id", 1);
        e.set(100, 5);
        e.set(101, "hi");
        byte[] eb = GenericCodec.encode(e);
        GenericCodec.DMsg back = GenericCodec.decode(old, eb);
        check("extension values", ((Number) back.get(100)).intValue() == 5
                && "hi".equals(back.get(101)));
        // Cross-check: the official runtime parses our bytes identically.
        com.google.protobuf.ExtensionRegistry reg =
                com.google.protobuf.ExtensionRegistry.newInstance();
        // No generated extensions exist; parse as DynamicMessage instead:
        // unknown extension numbers must survive as unknown fields there.
        DynamicMessage dm = DynamicMessage.parseFrom(old.descriptor, eb);
        check("extension cross-read",
                dm.getUnknownFields().asMap().containsKey(100));
        System.out.println("OK   group + extension gate");
    }

    /** Unknown fields survive a decode -> encode round trip byte-exactly. */
    private static void unknownGate(GenericCodec.Schema schema,
            GenericCodec.MessageDef scalars3) {
        byte[] ref = fillOfficial(1).toByteArray();
        byte[] withUnknown = Arrays.copyOf(ref, ref.length + 3);
        withUnknown[ref.length] = (byte) 0xF8; // field 63, varint
        withUnknown[ref.length + 1] = 0x03;
        withUnknown[ref.length + 2] = 0x2A; // 42
        GenericCodec.DMsg m = GenericCodec.decode(scalars3, withUnknown);
        if (!Arrays.equals(GenericCodec.encode(m), withUnknown)) {
            fail("unknown", "unknown field not preserved");
            return;
        }
        // Wrong wire type on a known field is data too, not an error.
        byte[] wrongWt = Arrays.copyOf(ref, ref.length + 5);
        wrongWt[ref.length] = 0x0D; // field 1 (i32), wire type 5
        wrongWt[ref.length + 1] = 0x01;
        wrongWt[ref.length + 2] = 0x02;
        wrongWt[ref.length + 3] = 0x03;
        wrongWt[ref.length + 4] = 0x04;
        GenericCodec.DMsg m2 = GenericCodec.decode(scalars3, wrongWt);
        if (!Arrays.equals(GenericCodec.encode(m2), wrongWt)) {
            fail("unknown", "wrong-wire-type field not preserved");
            return;
        }
        System.out.println("OK   unknown-field retention gate");
    }

    /* ---------------- helpers ---------------- */

    private static int num(GenericCodec.MessageDef def, String name) {
        return def.fields[def.byName.get(name)].number;
    }

    private static void check(String what, boolean ok) {
        if (!ok) {
            fail("gate", what);
            throw new IllegalStateException("gate failed: " + what);
        }
    }

    private static void checkDecoded(GenericCodec.DMsg back, int n,
            String label) {
        int inners = num(back.def, "inners");
        int tags = num(back.def, "tags");
        boolean ok = "hello".equals(back.get("str"))
                && back.getList(tags).size() == n
                && back.getList(inners).size() == n;
        GenericCodec.DMsg inner = (GenericCodec.DMsg) back.get("inner");
        ok = ok && inner != null && "kid".equals(inner.get("name"))
                && ((Number) inner.get("value")).intValue() == 42;
        if (ok && n > 0) {
            GenericCodec.DMsg last = (GenericCodec.DMsg) back.getList(inners)
                    .get(n - 1);
            ok = "grandkid".equals(last.get("name"))
                    && ((Number) last.get("value")).intValue() == n;
        }
        if (!ok) {
            fail(label, "materialized values mismatch");
        }
    }

    private static Matrix3.Scalars3 parse(byte[] wire) {
        try {
            return Matrix3.Scalars3.parseFrom(wire);
        } catch (Exception e) {
            throw new RuntimeException(e);
        }
    }

    private static DynamicMessage parseDyn(Descriptors.Descriptor desc,
            byte[] wire) {
        try {
            return DynamicMessage.parseFrom(desc, wire);
        } catch (Exception e) {
            throw new RuntimeException(e);
        }
    }

    private static void fail(String label, String what) {
        System.out.printf("FAIL %s: %s%n", label, what);
        failures++;
    }

    private static void row(String impl, String op, int bytes, double ns) {
        System.out.printf("  %-14s %-7s %12.1f %10.1f%n", impl, op, ns,
                bytes / ns * 1000.0);
    }

    private static Matrix3.Scalars3 fillOfficial(int n) {
        Matrix3.Scalars3.Builder b = Matrix3.Scalars3.newBuilder()
                .setI32(Integer.MIN_VALUE)
                .setI64(Long.MAX_VALUE)
                .setU32(-1)
                .setU64(-1L)
                .setS32(Integer.MIN_VALUE)
                .setS64(Long.MIN_VALUE)
                .setFx32(0xdeadbeef)
                .setFx64(72623859790382856L)
                .setSfx32(Integer.MIN_VALUE)
                .setSfx64(Long.MIN_VALUE)
                .setF32(Float.POSITIVE_INFINITY)
                .setF64(-123456.789)
                .setFlag(true)
                .setStr("hello")
                .setColor(Matrix3.Color3.COLOR3_GREEN)
                .setInner(Matrix3.Inner3.newBuilder().setName("kid").setValue(42)
                        .build())
                .setOptI32(-7)
                .setOptStr("opt");
        for (int i = 0; i < n; i++) {
            b.addNums(i - 3);
            b.addTags("tag");
            b.addInners(Matrix3.Inner3.newBuilder().setName("grandkid")
                    .setValue(i + 1));
            b.addFlags(i % 2 == 0);
        }
        return b.build();
    }

    private static GenericCodec.DMsg fillGeneric(GenericCodec.MessageDef def,
            int n) {
        GenericCodec.DMsg m = new GenericCodec.DMsg(def);
        m.set("i32", Integer.MIN_VALUE).set("i64", Long.MAX_VALUE)
                .set("u32", -1).set("u64", -1L).set("s32", Integer.MIN_VALUE)
                .set("s64", Long.MIN_VALUE).set("fx32", 0xdeadbeef)
                .set("fx64", 72623859790382856L)
                .set("sfx32", Integer.MIN_VALUE).set("sfx64", Long.MIN_VALUE)
                .set("f32", Float.POSITIVE_INFINITY).set("f64", -123456.789)
                .set("flag", true).set("str", "hello").set("color", 2)
                .set("opt_i32", -7).set("opt_str", "opt");
        m.newMessage("inner").set("name", "kid").set("value", 42);
        for (int i = 0; i < n; i++) {
            m.add("nums", i - 3).add("tags", "tag").add("flags", i % 2 == 0);
            m.newMessage("inners").set("name", "grandkid")
                    .set("value", i + 1);
        }
        return m;
    }

    private static double run(Runnable op, int iterations) {
        long start = System.nanoTime();
        for (int i = 0; i < iterations; i++) {
            op.run();
        }
        return (System.nanoTime() - start) / (double) iterations;
    }

    /** Calibrates every op, warms each once, then times all ops in one
     *  loop per pass and keeps the best per op. */
    private static double[] measureAll(int floor, Runnable[] ops) {
        int[] counts = new int[ops.length];
        for (int i = 0; i < ops.length; i++) {
            counts[i] = calibrate(ops[i], floor);
        }
        for (int i = 0; i < ops.length; i++) {
            run(ops[i], Math.min(counts[i], 1000));
        }
        double[] best = new double[ops.length];
        Arrays.fill(best, Double.MAX_VALUE);
        for (int pass = 0; pass < PASSES; pass++) {
            for (int i = 0; i < ops.length; i++) {
                best[i] = Math.min(best[i], run(ops[i], counts[i]));
            }
        }
        return best;
    }

    private static int calibrate(Runnable op, int floor) {
        int count = Math.max(1, floor);
        for (;;) {
            long start = System.nanoTime();
            for (int i = 0; i < count; i++) {
                op.run();
            }
            if ((System.nanoTime() - start) / 1e9 >= MIN_PASS_SECONDS
                    || count >= 100_000_000) {
                return count;
            }
            count *= 2;
        }
    }
}

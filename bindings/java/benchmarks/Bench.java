import com.google.protobuf.DescriptorProtos;
import com.google.protobuf.Descriptors;
import com.google.protobuf.DynamicMessage;

import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Arrays;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

import structtest.Matrix3;

/*
 * Benchmark for the pure-Java binding: the generic codec against the
 * official protobuf-java generated code, same matrix3.proto, same four
 * tiers as BENCHMARKS.md. The codec loads the schema from matrix3.desc with
 * its own parser -- protobuf-java below is only the baseline being beaten.
 * DynamicMessage stays a correctness cross-check, not a timed column: the
 * timed rows are the two arms plus the baseline, six per tier.
 *
 * Correctness gates (un-timed): proto2 round trip against matrix2.bin,
 * oneof against edge.bin, groups + extensions cross-checked through
 * DynamicMessage, unknown-field retention, and the required-field gate.
 *
 * Arms (bindings/README.md#pbench-three-arms-one-contract), timed in one
 * interleaved round per tier. The first two time the native-in/native-out
 * task; the third is the prebuilt handle:
 *
 *   tree  GenericCodec/DMsg full task    -- fresh DMsg built per field per
 *                     iteration, encode; decode, every gate-checked field
 *                     read back per iteration (Java has no free; GC owns it)
 *   core  the committed generated model  -- native object in and out (Structtest_Scalars3)
 *   proxy GenericCodec/DMsg, prebuilt    -- re-encode a held tree,
 *                     decode-and-drop with no materialization. Published,
 *                     never the headline.
 *   shim  ../pbench/jni                  -- ceiling: one JNI crossing, no tree translation
 */
public final class Bench {
    private Bench() {
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
            System.err.println("usage: Bench <matrix3.desc> <matrix2.desc>"
                    + " <matrix2.bin> <edge.desc> <edge.bin>"
                    + " <desc_proto2.desc>");
            System.exit(2);
        }
        byte[] matrix3 = Files.readAllBytes(Path.of(args[0]));
        GenericCodec.Schema schema = GenericCodec.Schema.load(matrix3);
        GenericCodec.MessageDef scalars3 =
                schema.message("structtest.Scalars3");
        System.out.println("schema ok: " + scalars3.fields.length
                + " fields for structtest.Scalars3");
        // Baseline-only descriptor for the DynamicMessage column.
        Descriptors.Descriptor protoScalars3 =
                protoDescriptor(matrix3, "structtest.Scalars3");

        String only = System.getenv("TIER"); // fast iteration: TIER=xlarge
        for (int tier = 0; tier < CASES.length; tier++) {
            if (only != null && !only.isEmpty()
                    && !CASES[tier].equals(only)) {
                continue;
            }
            runCase(schema, scalars3, protoScalars3, tier);
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

    /* ---------------- timed: matrix3 ---------------- */

    private static void runCase(GenericCodec.Schema schema,
            GenericCodec.MessageDef scalars3, Descriptors.Descriptor protoScalars3,
            int tier) throws Exception {
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
        // The native-object row: the committed generated model must produce
        // the same bytes in both directions before it is timed.
        Structtest_Scalars3 model = fillModel(n);
        if (!Arrays.equals(model.toByteArray(), reference)) {
            fail(label, "generated model bytes differ");
            return;
        }
        Structtest_Scalars3 modelBack = Structtest_Scalars3.parseFrom(reference);
        boolean modelOk = "hello".equals(modelBack.getStr())
                && modelBack.getNumsCount() == n
                && modelBack.getInnersCount() == n
                && "kid".equals(modelBack.getInner().getName())
                && (n == 0 || modelBack.getInners(n - 1).getValue() == n);
        if (!Arrays.equals(modelBack.toByteArray(), reference)) {
            fail(label, "generated model round trip differs");
            return;
        }
        if (!modelOk) {
            fail(label, "generated model values differ");
            return;
        }
        System.out.printf("OK   %s parity, x%d (%d bytes)%n", label, n,
                reference.length);
        System.out.printf("OK   %s generated-model parity, x%d (%d bytes)%n",
                label, n, reference.length);

        Descriptors.Descriptor desc = protoScalars3;
        DynamicMessage dynBuilt = DynamicMessage.parseFrom(desc, reference);
        if (!Arrays.equals(dynBuilt.toByteArray(), reference)) {
            fail(label, "DynamicMessage bytes differ");
            return;
        }

        // Full-task tree gate: a fresh per-field build must match the
        // reference too (not just the held tree above), and the decode
        // below must expose the same values the timer materializes.
        if (!Arrays.equals(GenericCodec.encode(fillGeneric(scalars3, n)),
                reference)) {
            fail(label, "fresh-build tree bytes differ");
            return;
        }

        Matrix3.Scalars3 officialMessage = fillOfficial(n);
        // Tree ops are the Go-parity full task: fresh DMsg per iteration,
        // deep materialization of every gate-checked field on decode (the
        // same field list checkDecoded asserts). Proxy ops are the old
        // prebuilt task, relabelled, not re-measured as a new task.
        // Calibration stays per-op (measureAll): the fast proxy ops keep
        // their own count and never inflate the tree rows.
        Runnable[] ops = {
            () -> GenericCodec.encode(fillGeneric(scalars3, n)),
            () -> fillModel(n).toByteArray(),
            () -> GenericCodec.encode(data),
            () -> fillOfficial(n).toByteArray(),
            () -> consumeTree(scalars3, reference, n),
            () -> Structtest_Scalars3.parseFrom(reference),
            () -> GenericCodec.decode(scalars3, reference),
            () -> parse(reference),
        };
        double[] best = measureAll(floor, ops);

        System.out.printf("%s: %d bytes, floor %d iterations (repeat field x%d)%n",
                label, reference.length, floor, n);
        System.out.printf("  %-18s %-7s %12s %10s%n", "impl", "op", "ns/op", "MB/s");
        row("pbwire-java-tree", "encode", reference.length, best[0]);
        row("pbwire-java-core", "encode", reference.length, best[1]);
        row("pbwire-java-proxy", "encode", reference.length, best[2]);
        row("protobuf-java", "encode", reference.length, best[3]);
        row("pbwire-java-tree", "decode", reference.length, best[4]);
        row("pbwire-java-core", "decode", reference.length, best[5]);
        row("pbwire-java-proxy", "decode", reference.length, best[6]);
        row("protobuf-java", "decode", reference.length, best[7]);
        System.out.println();
    }

    /** Baseline-only helper: protobuf-java descriptors for DynamicMessage. */
    private static Descriptors.Descriptor protoDescriptor(byte[] descSet,
            String fullName) throws Exception {
        DescriptorProtos.FileDescriptorSet set =
                DescriptorProtos.FileDescriptorSet.parseFrom(descSet);
        Map<String, Descriptors.FileDescriptor> byFile = new HashMap<>();
        Map<String, Descriptors.Descriptor> byName = new HashMap<>();
        for (DescriptorProtos.FileDescriptorProto f : set.getFileList()) {
            Descriptors.FileDescriptor[] deps =
                    new Descriptors.FileDescriptor[f.getDependencyCount()];
            for (int j = 0; j < deps.length; j++) {
                deps[j] = byFile.get(f.getDependency(j));
            }
            Descriptors.FileDescriptor fd =
                    Descriptors.FileDescriptor.buildFrom(f, deps);
            byFile.put(f.getName(), fd);
            for (Descriptors.Descriptor m : fd.getMessageTypes()) {
                collect(m, byName);
            }
        }
        Descriptors.Descriptor d = byName.get(fullName);
        if (d == null) {
            throw new IllegalArgumentException("no such message: " + fullName);
        }
        return d;
    }

    private static void collect(Descriptors.Descriptor m,
            Map<String, Descriptors.Descriptor> byName) {
        byName.put(m.getFullName(), m);
        for (Descriptors.Descriptor n : m.getNestedTypes()) {
            collect(n, byName);
        }
    }

    /* ---------------- correctness gates ---------------- */

    /** proto2: required/optional/defaults/unpacked-vs-packed/negative enum. */
    private static void proto2Gate(String descPath, String binPath)
            throws Exception {
        GenericCodec.Schema s = GenericCodec.Schema.load(
                Files.readAllBytes(Path.of(descPath)));
        GenericCodec.MessageDef def = s.message("structtest.Scalars2");
        byte[] ref = Files.readAllBytes(Path.of(binPath));
        GenericCodec.DMsg m = GenericCodec.decode(def, ref);
        check("proto2 id", ((Number) m.get(num(def, "id"))).intValue() == 99);
        check("proto2 explicit default", // i32: -5 present, == default -5
                m.has(num(def, "i32"))
                        && ((Number) m.get(num(def, "i32"))).intValue() == -5);
        check("proto2 packed",
                m.getList(num(def, "packed_nums")).size() > 0);
        if (!Arrays.equals(GenericCodec.encode(m), ref)) {
            fail("proto2", "round trip differs");
            return;
        }
        // Required gate: dropping a required field must fail on encode.
        GenericCodec.DMsg bad = GenericCodec.decode(def, ref);
        bad.clear(num(def, "id"));
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
        GenericCodec.Schema s = GenericCodec.Schema.load(
                Files.readAllBytes(Path.of(descPath)));
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
        m.set(num(def, "pick_num"), 7);
        check("oneof switch", m.oneofCase("choice") == num(def, "pick_num")
                && !m.has(num(def, "pick_str")));
        System.out.println("OK   oneof gate (case=" + which + ") + round trip");
    }

    /** groups + extensions: round trip + DynamicMessage cross-check. */
    private static void groupExtGate(String descPath) throws Exception {
        byte[] descBytes = Files.readAllBytes(Path.of(descPath));
        GenericCodec.Schema s = GenericCodec.Schema.load(descBytes);
        GenericCodec.MessageDef grouped = s.message("pbwire.test.Grouped");
        GenericCodec.MessageDef old = s.message("pbwire.test.Old");
        Descriptors.Descriptor gd =
                protoDescriptor(descBytes, "pbwire.test.Grouped");
        DynamicMessage.Builder b = DynamicMessage.newBuilder(gd);
        Descriptors.FieldDescriptor inner = gd.findFieldByName("inner");
        DynamicMessage.Builder ib = DynamicMessage.newBuilder(
                inner.getMessageType());
        ib.setField(inner.getMessageType().findFieldByName("x"), 42);
        b.setField(inner, ib.build());
        b.setField(gd.findFieldByName("after"), 7);
        byte[] ref = b.build().toByteArray();

        GenericCodec.DMsg m = GenericCodec.decode(grouped, ref);
        GenericCodec.DMsg gm = (GenericCodec.DMsg) m.get(num(grouped, "inner"));
        check("group value", gm != null
                && ((Number) gm.get(num(gm.def, "x"))).intValue() == 42);
        if (!Arrays.equals(GenericCodec.encode(m), ref)) {
            fail("groupext", "group round trip differs");
            return;
        }

        // Extensions by number through the generic model (id: required).
        GenericCodec.DMsg e = new GenericCodec.DMsg(old);
        e.set(num(old, "id"), 1);
        e.set(100, 5);
        e.set(101, "hi");
        byte[] eb = GenericCodec.encode(e);
        GenericCodec.DMsg back = GenericCodec.decode(old, eb);
        check("extension values",
                ((Number) back.get(100)).intValue() == 5
                        && "hi".equals(back.get(101)));
        // Cross-check: the official runtime parses our bytes identically.
        Descriptors.Descriptor od = protoDescriptor(descBytes, "pbwire.test.Old");
        DynamicMessage dm = DynamicMessage.parseFrom(od, eb);
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

    private static long sink;

    /**
     * The tree decode half of the timed full task: decode, then read back
     * every gate-checked field (same field list as checkDecoded asserts).
     * GenericCodec.decode is lazy on message/string/bytes fields, so this
     * is the work the binding really does turning wire into values.
     */
    private static void consumeTree(GenericCodec.MessageDef def, byte[] wire,
            int n) {
        GenericCodec.DMsg back = GenericCodec.decode(def, wire);
        sink += ((String) back.get(num(back.def, "str"))).length();
        sink += back.getList(num(back.def, "tags")).size();
        java.util.List<Object> inners =
                back.getList(num(back.def, "inners"));
        sink += inners.size();
        GenericCodec.DMsg inner =
                (GenericCodec.DMsg) back.get(num(back.def, "inner"));
        sink += ((String) inner.get(num(inner.def, "name"))).length();
        sink += ((Number) inner.get(num(inner.def, "value"))).intValue();
        if (n > 0) {
            GenericCodec.DMsg last =
                    (GenericCodec.DMsg) inners.get(n - 1);
            sink += ((String) last.get(num(last.def, "name"))).length();
            sink += ((Number) last.get(num(last.def, "value"))).intValue();
        }
    }

    private static void checkDecoded(GenericCodec.DMsg back, int n,
            String label) {
        int inners = num(back.def, "inners");
        int tags = num(back.def, "tags");
        boolean ok = "hello".equals(back.get(num(back.def, "str")))
                && back.getList(tags).size() == n
                && back.getList(inners).size() == n;
        GenericCodec.DMsg inner =
                (GenericCodec.DMsg) back.get(num(back.def, "inner"));
        ok = ok && inner != null && "kid".equals(inner.get(num(inner.def, "name")))
                && ((Number) inner.get(num(inner.def, "value"))).intValue() == 42;
        if (ok && n > 0) {
            GenericCodec.DMsg last = (GenericCodec.DMsg) back.getList(inners).get(n - 1);
            ok = "grandkid".equals(last.get(num(last.def, "name")))
                    && ((Number) last.get(num(last.def, "value"))).intValue() == n;
        }
        if (!ok) {
            fail(label, "materialized values mismatch");
        }
    }

    private static void check(String what, boolean ok) {
        if (!ok) {
            fail("gate", what);
            throw new IllegalStateException("gate failed: " + what);
        }
    }

    private static Matrix3.Scalars3 parse(byte[] wire) {
        try {
            return Matrix3.Scalars3.parseFrom(wire);
        } catch (Exception e) {
            throw new RuntimeException(e);
        }
    }

    private static void fail(String label, String what) {
        System.out.printf("FAIL %s: %s%n", label, what);
        failures++;
    }

    private static void row(String impl, String op, int bytes, double ns) {
        System.out.printf("  %-18s %-7s %12.1f %10.1f%n", impl, op, ns,
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

    /** The committed generated model for the same payload (native object). */
    private static Structtest_Scalars3 fillModel(int n) {
        Structtest_Scalars3.Builder b = Structtest_Scalars3.newBuilder()
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
                .setColorValue(2)
                .setInner(Structtest_Inner3.newBuilder().setName("kid").setValue(42))
                .setOptI32(-7)
                .setOptStr("opt");
        for (int i = 0; i < n; i++) {
            b.addNums(i - 3)
                    .addTags("tag")
                    .addInners(Structtest_Inner3.newBuilder()
                            .setName("grandkid").setValue(i + 1))
                    .addFlags(i % 2 == 0);
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

    /** Calibrates every op, warms each once, then times all ops in one loop
     *  per pass and keeps the best per op (a busy machine hits all equally). */
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

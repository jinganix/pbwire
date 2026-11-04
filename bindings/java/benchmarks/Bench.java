import java.io.IOException;
import java.io.UncheckedIOException;
import java.util.Arrays;

import structtest.Matrix3.Color3;
import structtest.Matrix3.Inner3;
import structtest.Matrix3.Scalars3;

/**
 * Java binding benchmark against the official protobuf-java generated code on
 * the same matrix3.proto and the four standard tiers from BENCHMARKS.md.
 *
 * <p>The same payload as the C/Go/Python rows: every scalar kind, the nested
 * message, the two proto3 optionals, and the repeated fields scaled x0 / x1 /
 * x100 / x3000, which lands the tiers on 136 / 174 / 2469 / 74913 bytes. Both
 * sides must produce identical bytes before anything is timed.
 *
 * <pre>
 *   java --enable-native-access=ALL-UNNAMED -Dpbwire.library=... \
 *        -cp out:&lt;protobuf-java.jar&gt; Bench matrix3.desc
 * </pre>
 */
public final class Bench {
    private Bench() {
    }

    /** Tier, repeated-field count, wire size. */
    private static final String[] CASES = {"small", "medium", "large", "xlarge"};
    private static final int[] REPEATS = {0, 1, 100, 3000};
    private static final int[] WANT = {136, 174, 2469, 74913};
    /** Floor iterations per tier; doubled until one pass takes 0.25 s. */
    private static final int[] ITERATIONS = {50_000, 50_000, 5_000, 500};
    /** Minimum seconds for one calibrated pass (shared standard, BENCHMARKS.md). */
    private static final double MIN_PASS_SECONDS = 0.25;
    /** Interleaved passes; the best of them is reported (C2 needs a few). */
    private static final int PASSES = 3;

    private static int failures;

    public static void main(String[] args) throws IOException {
        if (args.length != 1) {
            System.err.println("usage: Bench <descriptor-set>");
            System.exit(2);
        }
        try (Schema schema = Schema.fromFile(args[0])) {
            MessageType type = schema.message("structtest.Scalars3");
            for (int tier = 0; tier < CASES.length; tier++) {
                runCase(type, tier);
            }
        }
        if (failures != 0) {
            System.err.println(failures + " check(s) failed");
            System.exit(1);
        }
    }

    private static void runCase(MessageType type, int tier) {
        String label = CASES[tier];
        int n = REPEATS[tier];
        int floor = ITERATIONS[tier];
        int count = floor; // replaced by the reference op's calibrated count
        byte[] reference = fillOfficial(n).toByteArray();
        if (reference.length != WANT[tier]) {
            fail(label, "official encoder wrote " + reference.length + " bytes, expected " + WANT[tier]);
            return;
        }
        try (Message message = fillPbwire(type, n)) {
            byte[] got = message.encode();
            if (!Arrays.equals(got, reference)) {
                fail(label, "pbwire-java wrote " + got.length + " bytes, protobuf-java "
                        + reference.length);
                return;
            }
        }
        System.out.printf("OK   %s encoding parity, x%d (%d bytes)%n",
                label, REPEATS[tier], reference.length);

        Scalars3 officialMessage = fillOfficial(n);
        double[] best = new double[4];
        Arrays.fill(best, Double.MAX_VALUE);
        try (Message message = fillPbwire(type, n)) {
            /* Every op calibrates independently from the floor (a slow op
             * already exceeds the minimum at the floor and keeps it), then
             * one warm-up pass and interleaved timed passes, best of each:
             * the JIT needs a few passes to settle and a busy machine then
             * hits every candidate equally. The tier line shows the reference
             * op's count; slower ops keep the floor. */
            count = calibrate(message::encode, floor);
            best[0] = measure(floor, message::encode);
            best[1] = measure(floor, () -> type.decode(reference).close());
            best[2] = measure(floor, officialMessage::toByteArray);
            best[3] = measure(floor, () -> parse(reference));
        }

        System.out.printf("%s: %d bytes, %d iterations (repeat field x%d)%n",
                label, reference.length, count, REPEATS[tier]);
        System.out.printf("  %-14s %-7s %12s %10s%n", "impl", "op", "ns/op", "MB/s");
        row("pbwire-java", "encode", reference.length, best[0]);
        row("protobuf-java", "encode", reference.length, best[2]);
        row("pbwire-java", "decode", reference.length, best[1]);
        row("protobuf-java", "decode", reference.length, best[3]);
        System.out.println();
    }

    private static Scalars3 parse(byte[] wire) {
        try {
            return Scalars3.parseFrom(wire);
        } catch (IOException e) {
            throw new UncheckedIOException(e);
        }
    }

    private static void fail(String label, String what) {
        System.out.printf("FAIL %s: %s%n", label, what);
        failures++;
    }

    private static void row(String impl, String op, int bytes, double ns) {
        System.out.printf("  %-14s %-7s %12.1f %10.1f%n", impl, op, ns, bytes / ns * 1000.0);
    }

    private static Scalars3 fillOfficial(int n) {
        Scalars3.Builder b = Scalars3.newBuilder()
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
                .setColor(Color3.COLOR3_GREEN)
                .setInner(Inner3.newBuilder().setName("kid").setValue(42).build())
                .setOptI32(-7)
                .setOptStr("opt");
        for (int i = 0; i < n; i++) {
            b.addNums(i - 3);
            b.addTags("tag");
            b.addInners(Inner3.newBuilder().setName("grandkid").setValue(i + 1));
            b.addFlags(i % 2 == 0);
        }
        return b.build();
    }

    private static Message fillPbwire(MessageType type, int n) {
        Message message = type.newMessage();
        message.set("i32", (long) Integer.MIN_VALUE)
                .set("i64", Long.MAX_VALUE)
                .set("u32", 0xffffffffL)
                .set("u64", -1L)
                .set("s32", (long) Integer.MIN_VALUE)
                .set("s64", Long.MIN_VALUE)
                .set("fx32", 0xdeadbeefL)
                .set("fx64", 72623859790382856L)
                .set("sfx32", (long) Integer.MIN_VALUE)
                .set("sfx64", Long.MIN_VALUE)
                .set("f32", Double.POSITIVE_INFINITY)
                .set("f64", -123456.789)
                .set("flag", true)
                .set("str", "hello")
                .set("color", 2L)
                .set("opt_i32", -7L)
                .set("opt_str", "opt");
        message.newMessage("inner").set("name", "kid").set("value", 42L);
        for (int i = 0; i < n; i++) {
            message.set("nums", (long) (i - 3))
                    .set("tags", "tag")
                    .set("flags", i % 2 == 0);
            message.newMessage("inners").set("name", "grandkid").set("value", (long) (i + 1));
        }
        return message;
    }

    private static double run(Runnable op, int iterations) {
        long start = System.nanoTime();
        for (int i = 0; i < iterations; i++) {
            op.run();
        }
        return (System.nanoTime() - start) / (double) iterations;
    }

    /** Every op calibrates independently from the floor, then one warm-up
     * pass and the best of PASSES timed passes. */
    private static double measure(int floor, Runnable op) {
        int count = calibrate(op, floor);
        run(op, count);
        double best = Double.MAX_VALUE;
        for (int pass = 0; pass < PASSES; pass++) {
            best = Math.min(best, run(op, count));
        }
        return best;
    }

    /** Double from the tier floor until one pass takes MIN_PASS_SECONDS. */
    private static int calibrate(Runnable op, int floor) {
        int count = Math.max(1, floor);
        for (;;) {
            long start = System.nanoTime();
            for (int i = 0; i < count; i++) {
                op.run();
            }
            if ((System.nanoTime() - start) / 1e9 >= MIN_PASS_SECONDS || count >= 100_000_000) {
                return count;
            }
            count *= 2;
        }
    }
}

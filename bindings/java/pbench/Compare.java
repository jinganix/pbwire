import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Arrays;

/*
 * pbench harness for the Java binding: one directory per candidate, the same
 * contract in every one, timed back to back in every round so machine drift
 * hits all sides equally.
 *
 *   pure/  PureCandidate -- the shipped pure-Java codec (runtime/)
 *   jni/   JniCandidate  -- the C core behind four JNI methods (jni/)
 *
 * Contract (both sides measure the same shape):
 *   prepare(tier, model, wire): put the candidate in the state a caller holds
 *           -- the fully built Java tree (pure) / a C tree decoded once from
 *           the same bytes (jni) -- outside timing.
 *   encode():            re-encode that already-built tree, one byte[] per
 *           call, one boundary crossing per call.
 *   decode(wire):        bytes -> the candidate's own materialized tree,
 *           every field touched so the work cannot be eliminated, then
 *           released (pbw_message_free / GC).
 *
 * Every tier is gated before timing: both candidates must reproduce the
 * reference bytes exactly and both decodes must report a live tree. Rows print
 * the BENCHMARKS.md "Output format" block so scripts/bench-record.py reads this
 * log like any other harness; the impl labels are stable.
 *
 * The jni row is a ceiling, not a binding: it never translates a Java tree into
 * a C tree, so a real JNI binding pays strictly more. See README.md.
 */
public final class Compare {
    /** What every candidate satisfies; the adapters keep the harness in one shape. */
    interface Candidate {
        String name();

        void prepare(int tier, GenericCodec.DMsg model, byte[] wire);

        byte[] encode();

        int decode(byte[] wire);
    }

    private static final String[] CASES = {"small", "medium", "large", "xlarge"};
    private static final int[] REPEATS = {0, 1, 100, 3000};
    private static final int[] ITERATIONS = {50_000, 50_000, 5_000, 500};
    private static final double MIN_PASS_SECONDS = 0.25;
    private static final int PASSES = 3;

    private static int sink;

    public static void main(String[] args) {
        if (args.length < 1) {
            System.err.println("usage: Compare <matrix3.desc>");
            System.exit(2);
        }
        try {
            run(Path.of(args[0]));
        } catch (Exception e) {
            System.err.println("FAIL " + e);
            System.exit(1);
        }
    }

    private static void run(Path descPath) throws Exception {
        byte[] desc = Files.readAllBytes(descPath);
        GenericCodec.Schema schema = GenericCodec.Schema.load(desc);
        GenericCodec.MessageDef def = schema.message("structtest.Scalars3");

        Candidate pure = new PureCandidate(def);
        Candidate jni = new JniCandidate(desc);
        Candidate[] candidates = {pure, jni};
        String[] names = {"pure-java", "jni-c"};

        String only = System.getenv("TIER");
        double[][] ratios = new double[CASES.length][2];
        System.out.printf("pbench java: %s vs %s (C core), %s%n%n", names[0], names[1],
                System.getProperty("os.name"));
        for (int tier = 0; tier < CASES.length; tier++) {
            if (only != null && !only.isEmpty() && !CASES[tier].equals(only)) continue;
            ratios[tier] = runTier(def, tier, candidates, names);
        }

        System.out.println("jni-c / pure-java (higher means the C core is faster)");
        System.out.printf("  %-8s %8s %8s%n", "tier", "encode", "decode");
        for (int tier = 0; tier < CASES.length; tier++) {
            if (only != null && !only.isEmpty() && !CASES[tier].equals(only)) continue;
            System.out.printf("  %-8s %8.2fx %8.2fx%n", CASES[tier],
                    ratios[tier][0], ratios[tier][1]);
        }
    }

    /** Gates every candidate, then times the four ops; returns {encode, decode} ratios. */
    private static double[] runTier(GenericCodec.MessageDef def, int tier,
                                    Candidate[] candidates, String[] names) {
        GenericCodec.DMsg model = fillGeneric(def, REPEATS[tier]);
        byte[] ref = GenericCodec.encode(model);
        for (int i = 0; i < candidates.length; i++) {
            candidates[i].prepare(tier, model, ref);
            if (!Arrays.equals(candidates[i].encode(), ref)) {
                throw new IllegalStateException(names[i] + " encode bytes differ at "
                        + CASES[tier]);
            }
            if (candidates[i].decode(ref) < 0) {
                throw new IllegalStateException(names[i] + " decode failed at " + CASES[tier]);
            }
        }
        System.out.printf("OK   %s parity, x%d (%d bytes)%n", CASES[tier], REPEATS[tier], ref.length);

        Runnable[] ops = new Runnable[candidates.length * 2];
        for (int i = 0; i < candidates.length; i++) {
            Candidate c = candidates[i];
            ops[i * 2] = () -> sink += c.encode().length;
            ops[i * 2 + 1] = () -> sink += c.decode(ref);
        }
        double[] best = measureAll(ITERATIONS[tier], ops);
        System.out.printf("%s: %d bytes, floor %d iterations (repeat field x%d)%n",
                CASES[tier], ref.length, ITERATIONS[tier], REPEATS[tier]);
        System.out.printf("  %-14s %-7s %12s %10s%n", "impl", "op", "ns/op", "MB/s");
        for (int i = 0; i < candidates.length; i++) {
            row(names[i], "encode", ref.length, best[i * 2]);
            row(names[i], "decode", ref.length, best[i * 2 + 1]);
        }
        return new double[] {best[0] / best[2], best[1] / best[3]};
    }

    /*
     * shortcut: the payload (fillGeneric) and the timing loop are copies of
     * benchmarks/Bench.java, so both Java harnesses time the same shape;
     * upgrade to a shared helper if a third harness needs them or if a change
     * lands in only one copy. Payload drift is visible in the printed byte
     * sizes (136/174/2469/74913), which must match BENCHMARKS.md.
     */
    private static GenericCodec.DMsg fillGeneric(GenericCodec.MessageDef def, int n) {
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
            m.newMessage("inners").set("name", "grandkid").set("value", i + 1);
        }
        return m;
    }

    private static void row(String impl, String op, int bytes, double ns) {
        System.out.printf("  %-14s %-7s %12.1f %10.1f%n", impl, op, ns, bytes / ns * 1000.0);
    }

    private static double run(Runnable op, int iterations) {
        long start = System.nanoTime();
        for (int i = 0; i < iterations; i++) op.run();
        return (System.nanoTime() - start) / (double) iterations;
    }

    private static double[] measureAll(int floor, Runnable[] ops) {
        int[] counts = new int[ops.length];
        for (int i = 0; i < ops.length; i++) counts[i] = calibrate(ops[i], floor);
        for (int i = 0; i < ops.length; i++) run(ops[i], Math.min(counts[i], 1000));
        double[] best = new double[ops.length];
        Arrays.fill(best, Double.MAX_VALUE);
        for (int pass = 0; pass < PASSES; pass++)
            for (int i = 0; i < ops.length; i++)
                best[i] = Math.min(best[i], run(ops[i], counts[i]));
        return best;
    }

    private static int calibrate(Runnable op, int floor) {
        int count = Math.max(1, floor);
        for (;;) {
            long start = System.nanoTime();
            for (int i = 0; i < count; i++) op.run();
            if ((System.nanoTime() - start) / 1e9 >= MIN_PASS_SECONDS || count >= 100_000_000) return count;
            count *= 2;
        }
    }
}

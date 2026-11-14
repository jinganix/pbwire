import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Arrays;

import structtest.Matrix3;

/*
 * The A/B probe behind docs/perf-attempts.md: one process measures every
 * codec path on the same payloads, so a candidate can be compared against
 * a base build by running the two class dirs alternately (benchmarks/ab.sh).
 *
 * Payload shapes: the standard benchmark message ("const", every repeated
 * string element is the same literal) and "varied" (each element a distinct
 * string), so a change can never be kept by winning on one shape alone.
 *
 * Output is one `label<TAB>ns` line per op; the labels are stable.
 */
public final class Ab {
    private Ab() {
    }

    private static final int ELEMS = 3000;
    private static final int ITERS = 1500;
    private static final int PASSES = 5;

    private static int sink;

    public static void main(String[] args) throws Exception {
        if (args.length != 2) {
            System.err.println("usage: Ab <matrix3.desc> <matrix3.bin>");
            System.exit(2);
        }
        GenericCodec.Schema s =
                GenericCodec.Schema.load(Files.readAllBytes(Path.of(args[0])));
        GenericCodec.MessageDef def = s.message("structtest.Scalars3");
        byte[] small = Files.readAllBytes(Path.of(args[1]));

        GenericCodec.DMsg dynSmall = GenericCodec.decode(def, small);
        Matrix3.Scalars3 refSmall = Matrix3.Scalars3.parseFrom(small);
        require(Arrays.equals(GenericCodec.encode(dynSmall), refSmall.toByteArray()),
                "dynamic bytes (small)");

        run("small", dynSmall, refSmall, def, small, false);

        GenericCodec.DMsg dynConst = fill(def, false);
        Matrix3.Scalars3 refConst = fillRef(false);
        byte[] constBytes = refConst.toByteArray();
        require(Arrays.equals(GenericCodec.encode(dynConst), constBytes),
                "dynamic bytes (const)");
        run("const", dynConst, refConst, def, constBytes, true);

        GenericCodec.DMsg dynVar = fill(def, true);
        Matrix3.Scalars3 refVar = fillRef(true);
        byte[] varBytes = refVar.toByteArray();
        require(Arrays.equals(GenericCodec.encode(dynVar), varBytes),
                "dynamic bytes (varied)");
        run("varied", dynVar, refVar, def, varBytes, true);

        System.err.println("sink " + sink);
    }

    private static void require(boolean ok, String what) {
        if (!ok) {
            System.out.println("PARITY-FAIL " + what);
            System.exit(1);
        }
    }

    private static void run(String shape, GenericCodec.DMsg dyn,
            Matrix3.Scalars3 ref,
            GenericCodec.MessageDef def, byte[] bytes, boolean big)
            throws Exception {
        int n = big ? ELEMS : ITERS;
        Runnable[] ops = {
            () -> sink += GenericCodec.encode(dyn).length,
            () -> sink += GenericCodec.decode(def, bytes).has.length,
            () -> sink += ref.toByteArray().length,
            () -> {
                try {
                    sink += Matrix3.Scalars3.parseFrom(bytes).getI32();
                } catch (Exception e) {
                    throw new IllegalStateException(e);
                }
            },
        };
        String[] names = {"dyn-encode", "dyn-decode", "pbuf-encode",
                "pbuf-decode"};
        double[] best = new double[ops.length];
        Arrays.fill(best, Double.MAX_VALUE);
        for (int i = 0; i < ops.length; i++) {
            for (int w = 0; w < 3; w++) {
                run(ops[i], n);
            }
        }
        for (int pass = 0; pass < PASSES; pass++) {
            for (int i = 0; i < ops.length; i++) {
                best[i] = Math.min(best[i], run(ops[i], n));
            }
        }
        for (int i = 0; i < ops.length; i++) {
            System.out.printf("%s/%s\t%.3f%n", shape, names[i], best[i]);
        }
    }

    private static double run(Runnable op, int iterations) {
        long t0 = System.nanoTime();
        for (int i = 0; i < iterations; i++) {
            op.run();
        }
        return (System.nanoTime() - t0) / (double) iterations;
    }

    private static String str(boolean varied, int i, String base) {
        return varied ? base + i : base;
    }

    private static GenericCodec.DMsg fill(GenericCodec.MessageDef def,
            boolean varied) {
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
        for (int i = 0; i < ELEMS; i++) {
            m.add("nums", i - 3);
            m.add("tags", str(varied, i, "tag"));
            m.add("flags", i % 2 == 0);
            m.newMessage("inners").set("name", str(varied, i, "grandkid"))
                    .set("value", i + 1);
        }
        return m;
    }

    private static Matrix3.Scalars3 fillRef(boolean varied) {
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
                .setInner(Matrix3.Inner3.newBuilder().setName("kid").setValue(42))
                .setOptI32(-7)
                .setOptStr("opt");
        for (int i = 0; i < ELEMS; i++) {
            b.addNums(i - 3);
            b.addTags(str(varied, i, "tag"));
            b.addInners(Matrix3.Inner3.newBuilder().setName(str(varied, i, "grandkid"))
                    .setValue(i + 1));
            b.addFlags(i % 2 == 0);
        }
        return b.build();
    }
}

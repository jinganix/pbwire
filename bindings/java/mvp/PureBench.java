package mvp;

import com.google.protobuf.DescriptorProtos;
import com.google.protobuf.Descriptors;
import java.io.UncheckedIOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Arrays;

import structtest.Matrix3.Color3;
import structtest.Matrix3.Inner3;
import structtest.Matrix3.Scalars3;

/**
 * MVP benchmark: pure-Java procedural codec (PureCodec) against the official
 * protobuf-java generated code, same matrix3.proto, same four tiers as
 * BENCHMARKS.md. The schema comes from matrix3.desc at runtime; nothing is
 * compiled from .proto into the codec.
 *
 * <p>Fairness notes, read before quoting: pure decode reuses thread-local
 * scratch (holder + arrays grow once, constant strings canonicalize), while
 * {@code parseFrom} allocates a new message tree per call. Pure encode
 * returns a fresh exact-size array per call, like the FFM binding's out-copy.
 */
public final class PureBench {
    private PureBench() {
    }

    private static final String[] CASES = {"small", "medium", "large", "xlarge"};
    private static final int[] REPEATS = {0, 1, 100, 3000};
    private static final int[] WANT = {136, 174, 2469, 74913};
    private static final int[] ITERATIONS = {50_000, 50_000, 5_000, 500};
    private static final double MIN_PASS_SECONDS = 0.25;
    private static final int PASSES = 3;

    private static int failures;

    public static void main(String[] args) throws Exception {
        if (args.length != 1) {
            System.err.println("usage: PureBench <descriptor-set>");
            System.exit(2);
        }
        byte[] desc = Files.readAllBytes(Path.of(args[0]));
        DescriptorProtos.FileDescriptorSet set =
                DescriptorProtos.FileDescriptorSet.parseFrom(desc);
        Descriptors.FileDescriptor[] deps = new Descriptors.FileDescriptor[set.getFileCount()];
        Descriptors.Descriptor scalars3 = null;
        for (int i = 0; i < set.getFileCount(); i++) {
            deps[i] = Descriptors.FileDescriptor.buildFrom(set.getFile(i),
                    new Descriptors.FileDescriptor[0]);
            Descriptors.Descriptor t =
                    deps[i].findMessageTypeByName("Scalars3");
            if (t != null && "structtest".equals(deps[i].getPackage())) {
                scalars3 = t;
            }
        }
        if (scalars3 == null) {
            // Files may depend on each other (descriptor.proto); resolve in order.
            Descriptors.FileDescriptor[] built =
                    new Descriptors.FileDescriptor[set.getFileCount()];
            java.util.Map<String, Descriptors.FileDescriptor> byName =
                    new java.util.HashMap<>();
            for (int i = 0; i < set.getFileCount(); i++) {
                DescriptorProtos.FileDescriptorProto f = set.getFile(i);
                Descriptors.FileDescriptor[] d = new Descriptors.FileDescriptor[f
                        .getDependencyCount()];
                for (int j = 0; j < d.length; j++) {
                    d[j] = byName.get(f.getDependency(j));
                }
                built[i] = Descriptors.FileDescriptor.buildFrom(f, d);
                byName.put(f.getName(), built[i]);
                Descriptors.Descriptor t = built[i].findMessageTypeByName("Scalars3");
                if (t != null && "structtest".equals(built[i].getPackage())) {
                    scalars3 = t;
                }
            }
        }
        if (scalars3 == null) {
            throw new IllegalArgumentException("structtest.Scalars3 not in " + args[0]);
        }
        PureCodec.load(scalars3);
        System.out.println("schema ok: " + scalars3.getFullName()
                + " (" + scalars3.getFields().size() + " fields verified)");

        for (int tier = 0; tier < CASES.length; tier++) {
            runCase(tier);
        }
        if (failures != 0) {
            System.err.println(failures + " check(s) failed");
            System.exit(1);
        }
    }

    private static void runCase(int tier) {
        String label = CASES[tier];
        int n = REPEATS[tier];
        int floor = ITERATIONS[tier];

        byte[] reference = fillOfficial(n).toByteArray();
        if (reference.length != WANT[tier]) {
            fail(label, "official wrote " + reference.length + " bytes, want "
                    + WANT[tier]);
            return;
        }
        PureCodec.Data data = PureCodec.fillBench(new PureCodec.Data(), n);
        byte[] got = PureCodec.encode(data);
        if (!Arrays.equals(got, reference)) {
            fail(label, "pure wrote " + got.length + " bytes, official "
                    + reference.length + " " + firstDiff(got, reference));
            return;
        }
        // Round trip: decode the official bytes, re-encode, compare again.
        PureCodec.Data back = PureCodec.decode(reference);
        if (!Arrays.equals(PureCodec.encode(back), reference)) {
            fail(label, "pure round trip mismatch");
            return;
        }
        checkDecoded(back, n, label);
        System.out.printf("OK   %s parity, x%d (%d bytes)%n", label, n,
                reference.length);

        Scalars3 officialMessage = fillOfficial(n);
        double pureEnc = measure(floor, () -> PureCodec.encode(data));
        double pureDec = measure(floor, () -> PureCodec.decode(reference));
        double offEnc = measure(floor, officialMessage::toByteArray);
        double offDec = measure(floor, () -> parse(reference));

        System.out.printf("%s: %d bytes, floor %d iterations (repeat field x%d)%n",
                label, reference.length, floor, n);
        System.out.printf("  %-14s %-7s %12s %10s%n", "impl", "op", "ns/op", "MB/s");
        row("pure-java", "encode", reference.length, pureEnc);
        row("protobuf-java", "encode", reference.length, offEnc);
        row("pure-java", "decode", reference.length, pureDec);
        row("protobuf-java", "decode", reference.length, offDec);
        System.out.println();
    }

    private static void checkDecoded(PureCodec.Data d, int n, String label) {
        boolean ok = d.i32 == Integer.MIN_VALUE && d.i64 == Long.MAX_VALUE
                && d.u32 == -1 && d.u64 == -1L && d.s32 == Integer.MIN_VALUE
                && d.s64 == Long.MIN_VALUE && d.fx32 == 0xdeadbeef
                && d.fx64 == 72623859790382856L && d.sfx32 == Integer.MIN_VALUE
                && d.sfx64 == Long.MIN_VALUE && d.f32 == Float.POSITIVE_INFINITY
                && d.f64 == -123456.789 && d.flag && PureCodec.C_HELLO.equals(d.str)
                && d.color == 2 && d.inner != null
                && PureCodec.C_KID.equals(d.inner.name) && d.inner.value == 42
                && d.numsLen == n && d.tagsLen == n && d.innersLen == n
                && d.flagsLen == n && d.hasOptI32 && d.optI32 == -7 && d.hasOptStr
                && PureCodec.C_OPT.equals(d.optStr);
        if (ok && n > 0) {
            ok = d.nums[0] == -3 && d.nums[n - 1] == n - 4
                    && PureCodec.C_TAG.equals(d.tagsArr[0])
                    && PureCodec.C_GRANDKID.equals(d.innersArr[n - 1].name)
                    && d.innersArr[n - 1].value == n && d.flagsArr[0]
                    && d.flagsArr[n - 1] == ((n - 1) % 2 == 0);
        }
        if (!ok) {
            fail(label, "decoded values mismatch");
        }
    }

    private static String firstDiff(byte[] a, byte[] b) {
        int n = Math.min(a.length, b.length);
        for (int i = 0; i < n; i++) {
            if (a[i] != b[i]) {
                return "first diff at " + i + " pure=" + (a[i] & 0xFF) + " ref="
                        + (b[i] & 0xFF);
            }
        }
        return "prefix equal";
    }

    private static Scalars3 parse(byte[] wire) {
        try {
            return Scalars3.parseFrom(wire);
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

    private static double run(Runnable op, int iterations) {
        long start = System.nanoTime();
        for (int i = 0; i < iterations; i++) {
            op.run();
        }
        return (System.nanoTime() - start) / (double) iterations;
    }

    private static double measure(int floor, Runnable op) {
        int count = calibrate(op, floor);
        run(op, count);
        double best = Double.MAX_VALUE;
        for (int pass = 0; pass < PASSES; pass++) {
            best = Math.min(best, run(op, count));
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

import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Arrays;
import java.util.List;
import java.util.Objects;
import java.util.function.Consumer;
import java.util.function.Function;
import java.util.function.Supplier;

/**
 * Generated-compat check for the Java binding (TESTSPEC T1–T6, vectors.json as
 * the single source of truth):
 *
 * <ul>
 * <li>T1 generated {@code toByteArray} == pinned protoc reference bytes
 * <li>T2 {@code parseFrom} of the reference reads every fixture value back
 * <li>T3 default builds encode empty; explicit optional zero/empty round-trips
 * <li>T4 differential vs the pinned protobuf-java oracle: same bytes, same
 * fields, cross-decode agrees
 * <li>T5 one generic workflow runs against both libraries' messages with
 * identical observable results (import path is the only difference)
 * <li>T6 errors are loud, not crashes: bad wire, null setters, unknown enum
 * values, missing required fields, several oneof members set
 * </ul>
 *
 * <p>Run it with:
 * {@code java -cp out:protobuf-java.jar TestGenerated matrix3.bin oneof1.bin}
 * (see bindings/java/build.sh; the oracle needs its jar on the classpath).
 */
public final class TestGenerated {
    /** Pinned oracle: codegen/testdata/ORACLES.md; drift fails here. */
    private static final int ORACLE_MAJOR = 4;
    private static final int ORACLE_MINOR = 36;
    private static final int ORACLE_PATCH = 2;

    private static int checks;

    private TestGenerated() {
    }

    private static void check(boolean ok, String what) {
        if (!ok) {
            throw new AssertionError(what);
        }
        checks++;
    }

    @FunctionalInterface
    private interface ThrowingFunction<A, B> {
        B apply(A value) throws Exception;
    }

    public static void main(String[] args) throws Exception {
        Path matrixBin = Path.of(args.length > 0 ? args[0] : "../c/tests/data/matrix3.bin");
        Path oneofBin = Path.of(args.length > 1 ? args[1]
                : "../../codegen/testdata/oneof1.bin");
        Path mapBin = Path.of(args.length > 2 ? args[2]
                : "../../codegen/testdata/maptest.bin");
        Path xfileBin = Path.of(args.length > 3 ? args[3]
                : "../../codegen/testdata/xfile.bin");
        byte[] reference = Files.readAllBytes(matrixBin);
        byte[] referenceOneof = Files.readAllBytes(oneofBin);
        byte[] referenceMap = Files.readAllBytes(mapBin);
        byte[] referenceXfile = Files.readAllBytes(xfileBin);

        if (com.google.protobuf.RuntimeVersion.MAJOR != ORACLE_MAJOR
                || com.google.protobuf.RuntimeVersion.MINOR != ORACLE_MINOR
                || com.google.protobuf.RuntimeVersion.PATCH != ORACLE_PATCH) {
            throw new AssertionError("oracle pin drifted (got "
                    + com.google.protobuf.RuntimeVersion.MAJOR + "."
                    + com.google.protobuf.RuntimeVersion.MINOR + "."
                    + com.google.protobuf.RuntimeVersion.PATCH
                    + "); see codegen/testdata/ORACLES.md");
        }
        checks++;

        // T1: encode equals the protoc reference bytes.
        check(Arrays.equals(build().toByteArray(), reference), "T1 facade != reference");
        check(Arrays.equals(buildOneof().toByteArray(), referenceOneof), "T1 oneof != reference");

        // T2: decode reads every fixture value back.
        verify(Structtest_Scalars3.parseFrom(reference));
        verifyOneof(Spectest_Oneof1.parseFrom(referenceOneof));

        // T3: empty encodes empty; explicit optional zero/empty round-trips.
        check(Structtest_Scalars3.newBuilder().build().toByteArray().length == 0,
                "T3 empty encodes empty");
        Structtest_Scalars3 zero = Structtest_Scalars3.newBuilder().setOptI32(0).build();
        Structtest_Scalars3 zeroBack = Structtest_Scalars3.parseFrom(zero.toByteArray());
        check(zeroBack.hasOptI32() && zeroBack.getOptI32() == 0 && !zeroBack.hasOptStr(),
                "T3 explicit zero keeps presence");
        Structtest_Scalars3 emptyOpt = Structtest_Scalars3.newBuilder().setOptStr("").build();
        Structtest_Scalars3 emptyOptBack = Structtest_Scalars3.parseFrom(emptyOpt.toByteArray());
        check(emptyOptBack.hasOptStr() && emptyOptBack.getOptStr().isEmpty(),
                "T3 explicit empty optional keeps presence");
        check(!Structtest_Scalars3.parseFrom(new byte[0]).hasOptI32(),
                "T3 empty wire has no presence");

        // T4: differential vs the pinned oracle.
        check(Arrays.equals(buildOracle().toByteArray(), reference), "T4 oracle != reference");
        check(Arrays.equals(buildOneofOracle().toByteArray(), referenceOneof),
                "T4 oracle oneof != reference");
        structtest.Matrix3.Scalars3 om =
                structtest.Matrix3.Scalars3.parseFrom(build().toByteArray());
        Structtest_Scalars3 fm = Structtest_Scalars3.parseFrom(buildOracle().toByteArray());
        check(om.getI32() == fm.getI32() && om.getI64() == fm.getI64(), "T4 i32/i64");
        check(om.getU32() == fm.getU32() && om.getU64() == fm.getU64(), "T4 u32/u64");
        check(om.getS32() == fm.getS32() && om.getS64() == fm.getS64(), "T4 s32/s64");
        check(om.getFx32() == fm.getFx32() && om.getFx64() == fm.getFx64(), "T4 fx32/fx64");
        check(om.getSfx32() == fm.getSfx32() && om.getSfx64() == fm.getSfx64(),
                "T4 sfx32/sfx64");
        check(om.getF32() == fm.getF32() && om.getF64() == fm.getF64(), "T4 f32/f64");
        check(om.getFlag() == fm.isFlag() && om.getStr().equals(fm.getStr()), "T4 flag/str");
        check(om.getColorValue() == fm.getColorValue(), "T4 color");
        check(om.getInner().getName().equals(fm.getInner().getName())
                && om.getInner().getValue() == fm.getInner().getValue(), "T4 inner");
        check(om.getNumsList().equals(fm.getNumsList()), "T4 nums");
        check(om.getTagsList().equals(fm.getTagsList()), "T4 tags");
        check(om.getInnersCount() == fm.getInnersCount()
                && om.getInners(1).getValue() == fm.getInners(1).getValue(), "T4 inners");
        check(om.hasOptI32() == fm.hasOptI32() && om.getOptI32() == fm.getOptI32()
                && om.hasOptStr() == fm.hasOptStr() && om.getOptStr().equals(fm.getOptStr()),
                "T4 optional presence agrees");
        check(om.getFlagsList().equals(fm.getFlagsList()), "T4 flags");

        // T5: one generic workflow against both libraries.
        Summary a = workflow(
                structtest.Matrix3.Scalars3::newBuilder,
                b -> b.setStr("kid").setI32(-5).addNums(1).addNums(2)
                        .setInner(structtest.Matrix3.Inner3.newBuilder()
                                .setName("x").setValue(1)),
                structtest.Matrix3.Scalars3.Builder::build,
                structtest.Matrix3.Scalars3::toByteArray,
                structtest.Matrix3.Scalars3::parseFrom,
                m -> new Summary(null, m.getStr(), m.getI32(), m.getNumsList(),
                        m.getInner().getName(), m.hasOptI32()));
        Summary f = workflow(
                Structtest_Scalars3::newBuilder,
                b -> b.setStr("kid").setI32(-5).addNums(1).addNums(2)
                        .setInner(Structtest_Inner3.newBuilder().setName("x").setValue(1)),
                Structtest_Scalars3.Builder::build,
                Structtest_Scalars3::toByteArray,
                Structtest_Scalars3::parseFrom,
                m -> new Summary(null, m.getStr(), m.getI32(), m.getNumsList(),
                        m.getInner().getName(), m.hasOptI32()));
        check(a.equals(f), "T5 mismatch: " + a + " vs " + f);

        // T6: errors are loud.
        checkThrows(() -> Structtest_Scalars3.parseFrom(new byte[]{(byte) 0xff, (byte) 0xff, (byte) 0xff}),
                "T6 bad wire fails");
        checkThrows(() -> structtest.Matrix3.Scalars3.parseFrom(
                        new byte[]{(byte) 0xff, (byte) 0xff, (byte) 0xff}),
                "T6 oracle bad wire fails");
        Structtest_Scalars3 empty = Structtest_Scalars3.parseFrom(new byte[0]);
        check(empty.toByteArray().length == 0 && empty.getI32() == 0 && !empty.hasOptI32()
                && empty.getInner() == Structtest_Inner3.getDefaultInstance(),
                "T6 empty decodes to default");
        check(structtest.Matrix3.Scalars3.parseFrom(new byte[0])
                        .equals(structtest.Matrix3.Scalars3.getDefaultInstance()),
                "T6 oracle empty decodes to default");
        checkThrows(() -> Structtest_Scalars3.newBuilder().setStr(null), "T6 null setter fails");
        checkThrows(() -> structtest.Matrix3.Scalars3.newBuilder().setStr(null),
                "T6 oracle null setter fails");
        checkThrows(() -> Tutorial_Person.newBuilder().setName("x").build(),
                "T6 missing required fails");

        // Unknown enum values survive as UNRECOGNIZED on both sides, raw value kept.
        Structtest_Scalars3 mystery = Structtest_Scalars3.newBuilder().setColorValue(99).build();
        Structtest_Scalars3 mysteryBack = Structtest_Scalars3.parseFrom(mystery.toByteArray());
        structtest.Matrix3.Scalars3 oracleMystery = structtest.Matrix3.Scalars3.newBuilder()
                .setColorValue(99).build();
        structtest.Matrix3.Scalars3 oracleMysteryBack =
                structtest.Matrix3.Scalars3.parseFrom(oracleMystery.toByteArray());
        check(mysteryBack.getColor() == Structtest_Color3.UNRECOGNIZED
                && mysteryBack.getColorValue() == 99, "T6 unknown enum kept");
        check(oracleMysteryBack.getColor() == structtest.Matrix3.Color3.UNRECOGNIZED
                && oracleMysteryBack.getColorValue() == 99, "T6 oracle unknown enum kept");
        check(Arrays.equals(mystery.toByteArray(), oracleMystery.toByteArray()),
                "T6 unknown enum bytes agree");

        // Several oneof members set: the last one wins, on both sides.
        Spectest_Oneof1 both = Spectest_Oneof1.newBuilder().setName("n").setNum(1).setText("hi").build();
        check(both.getKindCase() == Spectest_Oneof1.KindCase.TEXT && both.getText().equals("hi")
                && !both.hasNum(), "T6 oneof last wins");
        Spectest_Oneof1 nested = Spectest_Oneof1.newBuilder().setName("n")
                .setInner(Spectest_Oneof1_Inner.newBuilder().setV(7)).build();
        check(Spectest_Oneof1.parseFrom(nested.toByteArray()).getInner().getV() == 7,
                "T6 nested oneof member round-trips");
        check(both.getKindCase().name().equals(spectest.Oneof1OuterClass.Oneof1.newBuilder()
                        .setName("n").setNum(1).setText("hi").build().getKindCase().name()),
                "T6 oneof case agrees with oracle");
        // A oneof member that holds the default value still counts as set on
        // both sides (the case word is its presence bit).
        check(Spectest_Oneof1.newBuilder().setNum(0).build().toByteArray().length == 2
                && Spectest_Oneof1.parseFrom(new byte[] {0x10, 0x00}).getKindCase()
                        == Spectest_Oneof1.KindCase.NUM,
                "T6 zero-valued oneof member encodes");
        check(Spectest_Oneof1.newBuilder().setText("").build().toByteArray().length == 2,
                "T6 empty-string oneof member encodes");
        check(Arrays.equals(
                Spectest_Oneof1.newBuilder().setNum(0).build().toByteArray(),
                spectest.Oneof1OuterClass.Oneof1.newBuilder().setNum(0).build().toByteArray()),
                "T6 zero oneof bytes agree with oracle");
        // Wire order decides the surviving oneof member, not declaration order.
        Spectest_Oneof1 innerThenNum = Spectest_Oneof1.parseFrom(
                new byte[] {0x22, 0x02, 0x08, 0x07, 0x10, 0x07});
        check(innerThenNum.getKindCase() == Spectest_Oneof1.KindCase.NUM
                && innerThenNum.getNum() == 7, "T6 oneof follows wire order");
        check(spectest.Oneof1OuterClass.Oneof1
                        .parseFrom(new byte[] {0x22, 0x02, 0x08, 0x07, 0x10, 0x07})
                        .getKindCase()
                        == spectest.Oneof1OuterClass.Oneof1.KindCase.NUM,
                "T6 oneof wire order agrees with oracle");

        // Map entries ride as repeated entry structs, byte-exact with protoc.
        Spectest_MapTest maps = Spectest_MapTest.parseFrom(referenceMap);
        check(maps.getByIntCount() == 2 && maps.getByInt(0).getKey() == 1
                && maps.getByInt(0).getValue().equals("one")
                && maps.getByInt(1).getKey() == -5, "map int entries decode");
        check(maps.getByStrCount() == 2 && maps.getByStr(0).getKey().equals("a")
                && maps.getByStr(0).getValue() == 10, "map string entries decode");
        check(maps.getByMsgCount() == 1 && maps.getByMsg(0).getKey().equals("k")
                && maps.getByMsg(0).getValue().getName().equals("n")
                && maps.getByMsg(0).getValue().getValue() == 42,
                "map message entries decode");
        Spectest_MapTest builtMaps = Spectest_MapTest.newBuilder()
                .addByInt(Spectest_MapTest_ByIntEntry.newBuilder().setKey(1).setValue("one"))
                .addByInt(Spectest_MapTest_ByIntEntry.newBuilder().setKey(-5).setValue("neg"))
                .addByStr(Spectest_MapTest_ByStrEntry.newBuilder().setKey("a").setValue(10))
                .addByStr(Spectest_MapTest_ByStrEntry.newBuilder().setKey("b").setValue(-3))
                .addByMsg(Spectest_MapTest_ByMsgEntry.newBuilder().setKey("k")
                        .setValue(Spectest_MapValue.newBuilder().setName("n").setValue(42)))
                .build();
        check(Arrays.equals(builtMaps.toByteArray(), referenceMap), "map facade != reference");

        // Cross-file references import the sibling model.
        Xfile_UsesDep usesDep = Xfile_UsesDep.parseFrom(referenceXfile);
        check(usesDep.getSingle().getName().equals("one")
                && usesDep.getSingle().getValue() == 7, "xfile single decodes");
        check(usesDep.getManyCount() == 2 && usesDep.getMany(0).getName().equals("a")
                && usesDep.getMany(1).getValue() == -1, "xfile many decodes");
        check(usesDep.getNote().equals("hi"), "xfile note decodes");
        Xfile_UsesDep builtUsesDep = Xfile_UsesDep.newBuilder()
                .setSingle(Xdep_DepMsg.newBuilder().setName("one").setValue(7))
                .addMany(Xdep_DepMsg.newBuilder().setName("a"))
                .addMany(Xdep_DepMsg.newBuilder().setName("b").setValue(-1))
                .setNote("hi").build();
        check(Arrays.equals(builtUsesDep.toByteArray(), referenceXfile),
                "xfile facade != reference");

        System.out.println("TestGenerated: " + checks + " checks passed");
    }

    @FunctionalInterface
    private interface ThrowingRunnable {
        void run() throws Exception;
    }

    private static void checkThrows(ThrowingRunnable fn, String what) {
        try {
            fn.run();
        } catch (Exception e) {
            checks++;
            return;
        }
        throw new AssertionError(what);
    }

    private static Structtest_Scalars3 build() {
        return Structtest_Scalars3.newBuilder()
                .setI32(Integer.MIN_VALUE)
                .setI64(Long.MAX_VALUE)
                .setU32(-1) // 4294967295 as raw 32-bit bits
                .setU64(-1L) // 18446744073709551615 as raw 64-bit bits
                .setS32(Integer.MIN_VALUE)
                .setS64(Long.MIN_VALUE)
                .setFx32(0xDEADBEEF) // 3735928559 as raw bits
                .setFx64(0x0102030405060708L) // 72623859790382856
                .setSfx32(Integer.MIN_VALUE)
                .setSfx64(Long.MIN_VALUE)
                .setF32(Float.POSITIVE_INFINITY)
                .setF64(-123456.789)
                .setFlag(true)
                .setStr("hello")
                .setColor(Structtest_Color3.COLOR3_GREEN)
                .setInner(Structtest_Inner3.newBuilder().setName("kid").setValue(42))
                .addNums(1).addNums(-5).addNums(300)
                .addTags("a").addTags("bb")
                .addInners(Structtest_Inner3.newBuilder().setName("x").setValue(1))
                .addInners(Structtest_Inner3.newBuilder().setName("y").setValue(-2))
                .setOptI32(-7)
                .setOptStr("opt")
                .addFlags(true).addFlags(false).addFlags(true)
                .build();
    }

    private static structtest.Matrix3.Scalars3 buildOracle() {
        return structtest.Matrix3.Scalars3.newBuilder()
                .setI32(Integer.MIN_VALUE)
                .setI64(Long.MAX_VALUE)
                .setU32(-1)
                .setU64(-1L)
                .setS32(Integer.MIN_VALUE)
                .setS64(Long.MIN_VALUE)
                .setFx32(0xDEADBEEF)
                .setFx64(0x0102030405060708L)
                .setSfx32(Integer.MIN_VALUE)
                .setSfx64(Long.MIN_VALUE)
                .setF32(Float.POSITIVE_INFINITY)
                .setF64(-123456.789)
                .setFlag(true)
                .setStr("hello")
                .setColor(structtest.Matrix3.Color3.COLOR3_GREEN)
                .setInner(structtest.Matrix3.Inner3.newBuilder().setName("kid").setValue(42))
                .addNums(1).addNums(-5).addNums(300)
                .addTags("a").addTags("bb")
                .addInners(structtest.Matrix3.Inner3.newBuilder().setName("x").setValue(1))
                .addInners(structtest.Matrix3.Inner3.newBuilder().setName("y").setValue(-2))
                .setOptI32(-7)
                .setOptStr("opt")
                .addFlags(true).addFlags(false).addFlags(true)
                .build();
    }

    private static void verify(Structtest_Scalars3 m) {
        check(m.getI32() == Integer.MIN_VALUE, "T2 i32");
        check(m.getI64() == Long.MAX_VALUE, "T2 i64");
        check(m.getU32() == -1, "T2 u32");
        check(m.getU64() == -1L, "T2 u64");
        check(m.getS32() == Integer.MIN_VALUE, "T2 s32");
        check(m.getS64() == Long.MIN_VALUE, "T2 s64");
        check(m.getFx32() == 0xDEADBEEF, "T2 fx32");
        check(m.getFx64() == 0x0102030405060708L, "T2 fx64");
        check(m.getSfx32() == Integer.MIN_VALUE, "T2 sfx32");
        check(m.getSfx64() == Long.MIN_VALUE, "T2 sfx64");
        check(m.getF32() == Float.POSITIVE_INFINITY, "T2 f32");
        check(m.getF64() == -123456.789, "T2 f64");
        check(m.isFlag(), "T2 flag");
        check("hello".equals(m.getStr()), "T2 str");
        check(m.getColor() == Structtest_Color3.COLOR3_GREEN, "T2 color");
        check("kid".equals(m.getInner().getName()) && m.getInner().getValue() == 42, "T2 inner");
        check(m.getNumsList().equals(List.of(1, -5, 300)), "T2 nums");
        check(m.getTagsList().equals(List.of("a", "bb")), "T2 tags");
        check(m.getInnersCount() == 2 && m.getInners(1).getValue() == -2, "T2 inners");
        check(m.hasOptI32() && m.getOptI32() == -7, "T2 opt_i32");
        check(m.hasOptStr() && "opt".equals(m.getOptStr()), "T2 opt_str");
        check(m.getFlagsList().equals(List.of(true, false, true)), "T2 flags");
    }

    private static Spectest_Oneof1 buildOneof() {
        return Spectest_Oneof1.newBuilder().setName("a").setText("hi").build();
    }

    private static spectest.Oneof1OuterClass.Oneof1 buildOneofOracle() {
        return spectest.Oneof1OuterClass.Oneof1.newBuilder().setName("a").setText("hi").build();
    }

    private static void verifyOneof(Spectest_Oneof1 m) {
        check("a".equals(m.getName()), "T2 oneof name");
        check(m.getKindCase() == Spectest_Oneof1.KindCase.TEXT, "T2 oneof case");
        check("hi".equals(m.getText()), "T2 oneof text");
        check(!m.hasNum(), "T2 oneof num absent");
        Spectest_Oneof1 nested = Spectest_Oneof1.newBuilder().setName("n")
                .setInner(Spectest_Oneof1_Inner.newBuilder().setV(7)).build();
        check(Spectest_Oneof1.parseFrom(nested.toByteArray()).getInner().getV() == 7,
                "T2 nested oneof member");
    }

    /** The comparable business result for T5. */
    private static final class Summary {
        final byte[] wire;
        final String str;
        final int i32;
        final List<Integer> nums;
        final String inner;
        final boolean hasOpt;

        Summary(byte[] wire, String str, int i32, List<Integer> nums, String inner,
                boolean hasOpt) {
            this.wire = wire == null ? null : wire.clone();
            this.str = str;
            this.i32 = i32;
            this.nums = List.copyOf(nums);
            this.inner = inner;
            this.hasOpt = hasOpt;
        }

        @Override
        public boolean equals(Object o) {
            if (!(o instanceof Summary)) {
                return false;
            }
            Summary s = (Summary) o;
            return str.equals(s.str) && i32 == s.i32 && nums.equals(s.nums)
                    && inner.equals(s.inner) && hasOpt == s.hasOpt
                    && Arrays.equals(wire, s.wire);
        }

        @Override
        public int hashCode() {
            return Objects.hash(str, i32, nums, inner, hasOpt, Arrays.hashCode(wire));
        }

        @Override
        public String toString() {
            return "Summary[" + str + "," + i32 + "," + nums + "," + inner + "," + hasOpt + "]";
        }
    }

    private static <B, M> Summary workflow(Supplier<B> nw, Consumer<B> set,
            Function<B, M> build, ThrowingFunction<M, byte[]> enc,
            ThrowingFunction<byte[], M> dec, Function<M, Summary> get) throws Exception {
        B b = nw.get();
        set.accept(b);
        M m = build.apply(b);
        byte[] wire = enc.apply(m);
        Summary s = get.apply(dec.apply(wire));
        return new Summary(wire, s.str, s.i32, s.nums, s.inner, s.hasOpt);
    }
}

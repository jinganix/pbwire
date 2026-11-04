import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Arrays;

/**
 * Typed-conformance check for the Java binding: build matrix3.proto's
 * Scalars3 through the generated typed classes, encode, and require the bytes
 * to equal the protoc reference (matrix3.bin). Then decode the reference and
 * require every field back. This is the flow every binding's typed test runs:
 * plugin generates the model, the test assigns typed fields, encode/decode
 * must match the reference implementation.
 *
 * <p>Run it with:
 * {@code java ... -cp out TestTyped ../c/tests/data/matrix3.desc ../c/tests/data/matrix3.bin}
 */
public final class TestTyped {
    private static int checks;

    private TestTyped() {
    }

    private static void check(boolean ok, String what) {
        if (!ok) {
            throw new AssertionError(what);
        }
        checks++;
    }

    public static void main(String[] args) throws Exception {
        Path data = Path.of(args.length > 0 ? args[0] : "../c/tests/data/matrix3.desc").getParent();
        Path desc = Path.of(args.length > 0 ? args[0] : data.resolve("matrix3.desc").toString());
        Path bin = args.length > 1 ? Path.of(args[1]) : data.resolve("matrix3.bin");
        byte[] reference = Files.readAllBytes(bin);

        try (Schema schema = Schema.fromFile(desc.toString())) {
            Structtest_Scalars3 typed = build();
            byte[] wire = typed.encode(schema);
            check(Arrays.equals(wire, reference), "typed encode equals the protoc reference bytes");
            check(wire.length == reference.length, "typed encode length");
            verify(Structtest_Scalars3.decode(schema, reference));

            // Proto3 implicit presence: unset scalars stay off the wire.
            check(new Structtest_Scalars3().encode(schema).length == 0, "empty typed message is empty");

            // Presence flags are what keeps an explicit zero on the wire.
            Structtest_Scalars3 explicitZero = new Structtest_Scalars3();
            explicitZero.hasOptI32 = true;
            explicitZero.optI32 = 0;
            byte[] zeroWire = explicitZero.encode(schema);
            Structtest_Scalars3 zeroBack = Structtest_Scalars3.decode(schema, zeroWire);
            check(zeroBack.hasOptI32 && zeroBack.optI32 == 0, "explicit zero round-trips");

            // A nested wire view must keep its root alive: dropping the root
            // reference and forcing a collection must not free the tree.
            Message root = schema.message("structtest.Scalars3").decode(reference);
            Message child = (Message) root.getList("inners").get(1);
            root = null;
            System.gc();
            Thread.sleep(20);
            System.gc();
            check(child.getInt("value") == -2, "a nested view keeps its root alive");

            // A nested typed message set on its own encodes as the same field.
            Structtest_Inner3 inner = new Structtest_Inner3();
            inner.name = "kid";
            inner.value = 42;
            Structtest_Scalars3 nested = new Structtest_Scalars3();
            nested.inner = inner;
            check(Structtest_Scalars3.decode(schema, nested.encode(schema)).inner.value == 42, "nested typed message");
        }

        System.out.println("TestTyped: " + checks + " checks passed");
    }

    private static Structtest_Scalars3 build() {
        Structtest_Inner3 kid = new Structtest_Inner3();
        kid.name = "kid";
        kid.value = 42;

        Structtest_Inner3 first = new Structtest_Inner3();
        first.name = "x";
        first.value = 1;
        Structtest_Inner3 second = new Structtest_Inner3();
        second.name = "y";
        second.value = -2;

        Structtest_Scalars3 m = new Structtest_Scalars3();
        m.i32 = Integer.MIN_VALUE;
        m.i64 = Long.MAX_VALUE;
        m.u32 = -1;                    // 4294967295 as raw 32-bit bits
        m.u64 = -1L;                   // 18446744073709551615 as raw 64-bit bits
        m.s32 = Integer.MIN_VALUE;
        m.s64 = Long.MIN_VALUE;
        m.fx32 = 0xDEADBEEF;           // 3735928559 as raw bits
        m.fx64 = 0x0102030405060708L;  // 72623859790382856
        m.sfx32 = Integer.MIN_VALUE;
        m.sfx64 = Long.MIN_VALUE;
        m.f32 = Float.POSITIVE_INFINITY;
        m.f64 = -123456.789;
        m.flag = true;
        m.str = "hello";
        m.color = Structtest_Color3.COLOR3_GREEN;
        m.inner = kid;
        m.nums.addAll(Arrays.asList(1, -5, 300));
        m.tags.addAll(Arrays.asList("a", "bb"));
        m.inners.addAll(Arrays.asList(first, second));
        m.hasOptI32 = true;
        m.optI32 = -7;
        m.hasOptStr = true;
        m.optStr = "opt";
        m.flags.addAll(Arrays.asList(true, false, true));
        return m;
    }

    private static void verify(Structtest_Scalars3 m) {
        check(m.i32 == Integer.MIN_VALUE, "i32");
        check(m.i64 == Long.MAX_VALUE, "i64");
        check(m.u32 == -1, "u32");
        check(m.u64 == -1L, "u64");
        check(m.s32 == Integer.MIN_VALUE, "s32");
        check(m.s64 == Long.MIN_VALUE, "s64");
        check(m.fx32 == 0xDEADBEEF, "fx32");
        check(m.fx64 == 0x0102030405060708L, "fx64");
        check(m.sfx32 == Integer.MIN_VALUE, "sfx32");
        check(m.sfx64 == Long.MIN_VALUE, "sfx64");
        check(m.f32 == Float.POSITIVE_INFINITY, "f32");
        check(m.f64 == -123456.789, "f64");
        check(m.flag, "flag");
        check("hello".equals(m.str), "str");
        check(m.color == Structtest_Color3.COLOR3_GREEN, "color");
        check("kid".equals(m.inner.name) && m.inner.value == 42, "inner");
        check(m.nums.equals(Arrays.asList(1, -5, 300)), "nums");
        check(m.tags.equals(Arrays.asList("a", "bb")), "tags");
        check(m.inners.size() == 2 && m.inners.get(1).value == -2, "inners");
        check(m.hasOptI32 && m.optI32 == -7, "opt_i32");
        check(m.hasOptStr && "opt".equals(m.optStr), "opt_str");
        check(m.flags.equals(Arrays.asList(true, false, true)), "flags");
    }
}

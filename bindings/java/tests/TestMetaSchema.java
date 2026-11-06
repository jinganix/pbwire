import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Arrays;
import java.util.List;

/**
 * Schema-from-Meta check: the dynamic Message API runs on generated tables
 * with no descriptor set file. Decodes the same pinned reference bytes the
 * typed codec and the descriptor path agree on, so all three read the same
 * values and re-encode byte-exact.
 *
 * <p>Run it with:
 * {@code java -cp out TestMetaSchema matrix3.bin oneof1.bin}
 */
public final class TestMetaSchema {
    private static int checks;

    private TestMetaSchema() {
    }

    private static void check(boolean ok, String what) {
        if (!ok) {
            throw new AssertionError(what);
        }
        checks++;
    }

    public static void main(String[] args) throws Exception {
        byte[] matrix = Files.readAllBytes(Path.of(args[0]));
        byte[] oneof = Files.readAllBytes(Path.of(args[1]));

        // Roots only: Inner3 arrives through the Scalars3 table link.
        try (Schema schema = Schema.fromClasses(Structtest_Scalars3.class)) {
            MessageType scalars = schema.message("structtest.Scalars3");
            check(scalars.name().equals("structtest.Scalars3"), "type name");
            // The sub-table joined the same schema through the Meta link.
            check(schema.message("structtest.Inner3") != null, "sub type linked");

            try (Message m = scalars.decode(matrix)) {
                check(m.getInt("i32") == Integer.MIN_VALUE, "i32 by name");
                check(m.getLong("i64") == Long.MAX_VALUE, "i64 by name");
                check(m.getLong("u32") == 4294967295L, "u32 zero-extended");
                check(m.getLong("u64") == -1L, "u64 raw bits");
                check(m.getInt("s32") == Integer.MIN_VALUE, "s32");
                check(m.getLong("s64") == Long.MIN_VALUE, "s64");
                check(m.getLong("fx32") == 0xDEADBEEFL, "fx32");
                check(m.getLong("fx64") == 0x0102030405060708L, "fx64");
                check(m.getInt("sfx32") == Integer.MIN_VALUE, "sfx32");
                check(m.getLong("sfx64") == Long.MIN_VALUE, "sfx64");
                check(m.getFloat("f32") == Float.POSITIVE_INFINITY, "f32");
                check(m.getDouble("f64") == -123456.789, "f64");
                check(m.getBool("flag"), "flag");
                check("hello".equals(m.getString("str")), "str");
                check(m.getLong("color") == Structtest_Color3.COLOR3_GREEN.getNumber(),
                        "enum reads as the wire number");
                Message inner = m.getMessage("inner");
                check("kid".equals(inner.getString("name"))
                        && inner.getInt("value") == 42, "nested by name");
                check(m.getList("nums").equals(List.of(1L, -5L, 300L)), "packed by name");
                check(m.getList("tags").equals(List.of("a", "bb")), "strings by name");
                List<Object> inners = m.getList("inners");
                check(inners.size() == 2
                        && ((Message) inners.get(1)).getInt("value") == -2, "message list");
                // Proto names, not Java names: opt_i32 keeps its underscore.
                check(m.has("opt_i32") && m.getInt("opt_i32") == -7, "optional int");
                check(m.has("opt_str") && "opt".equals(m.getString("opt_str")),
                        "optional string");
                check(m.getList("flags").equals(List.of(true, false, true)), "bools");
                check(Arrays.equals(m.encode(), matrix), "re-encode is byte exact");
            }

            // Dynamic build, typed read: the wire is the shared contract.
            MessageType scalars2 = schema.message("structtest.Scalars3");
            byte[] wire;
            try (Message m = scalars2.newMessage()) {
                m.set("str", "hi").set("i32", 7);
                m.newMessage("inner").set("name", "n").set("value", 1);
                m.set("nums", 9);
                wire = m.encode();
            }
            Structtest_Scalars3 typed = Structtest_Scalars3.parseFrom(wire);
            check("hi".equals(typed.getStr()) && typed.getI32() == 7
                    && "n".equals(typed.getInner().getName())
                    && typed.getNumsList().equals(List.of(9)), "dynamic->typed");
            // Typed bytes decode back through the Meta schema.
            try (Message back = scalars2.decode(typed.toByteArray())) {
                check("hi".equals(back.getString("str"))
                        && back.getMessage("inner").getInt("value") == 1, "typed->dynamic");
            }
        }

        try (Schema schema = Schema.fromClasses(Spectest_Oneof1.class)) {
            MessageType oneof1 = schema.message("spectest.Oneof1");
            try (Message m = oneof1.decode(oneof)) {
                check("a".equals(m.getString("name")), "oneof name");
                check(m.has("text") && "hi".equals(m.getString("text")), "oneof member");
                check(!m.has("num"), "oneof sibling absent");
                check(m.getMessage("inner") == null, "oneof message absent");
                check(Arrays.equals(m.encode(), oneof), "oneof re-encode exact");
            }
        }

        try {
            Schema.fromClasses(String.class);
            check(false, "unregistered class must fail");
        } catch (PbwireException expected) {
            checks++;
        }
        try {
            Schema.fromClasses();
            check(false, "empty roots must fail");
        } catch (PbwireException expected) {
            checks++;
        }

        System.out.println("TestMetaSchema ok: " + checks + " checks");
    }
}

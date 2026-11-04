/*
 * The Java binding's FFI layer: one java.lang.invoke.MethodHandle per exported
 * pbwire function, resolved from the shared library with Linker.nativeLinker().
 * No JNI and no C glue: the C ABI in include/pbwire.h is the whole contract.
 *
 * Needs JDK 22+ (Foreign Function & Memory API, final since 22) and
 * --enable-native-access=ALL-UNNAMED. Never wraps a struct: the binding only
 * uses pointers and the scalar getters, so no MemoryLayout is hand-rolled.
 *
 * Every call goes through invokeExact on a static final handle. That is what
 * makes the boundary cheap: the JIT inlines the downcall into the caller
 * instead of resolving a signature per call, which is ~40x the cost of the
 * native call itself (invokeWithArguments boxes every argument).
 *
 * Per-thread workspace, one per thread that uses the binding:
 *   - a pbw_ctx, whose reusable encode buffer stops the encoder from
 *     mallocing a fresh buffer per encode() call;
 *   - the scratch slots C writes its out-parameters through;
 *   - a native buffer for the bytes crossing in: decode() input, and the
 *     payload of set(String)/set(byte[]) before C copies it into the value.
 * Nothing is shared between threads, and the whole lot is bounded by the
 * largest message the thread has seen.
 *
 * The library is found through -Dpbwire.library=<path> (or PBWIRE_LIB), else
 * build/libpbwire.dylib / build/libpbwire.so relative to the working directory.
 * `make shared` builds it.
 */
import java.lang.foreign.AddressLayout;
import java.lang.foreign.Arena;
import java.lang.foreign.FunctionDescriptor;
import java.lang.foreign.Linker;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.SymbolLookup;
import java.lang.foreign.ValueLayout;
import java.lang.invoke.MethodHandle;
import java.lang.ref.Cleaner;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.Consumer;

final class Pbwire {
    private Pbwire() {
    }

    /* pbw_status */
    static final int OK = 0;

    /* pbw_type */
    static final int TYPE_DOUBLE = 1, TYPE_FLOAT = 2, TYPE_INT64 = 3, TYPE_UINT64 = 4, TYPE_INT32 = 5,
            TYPE_FIXED64 = 6, TYPE_FIXED32 = 7, TYPE_BOOL = 8, TYPE_STRING = 9, TYPE_GROUP = 10,
            TYPE_MESSAGE = 11, TYPE_BYTES = 12, TYPE_UINT32 = 13, TYPE_ENUM = 14, TYPE_SFIXED32 = 15,
            TYPE_SFIXED64 = 16, TYPE_SINT32 = 17, TYPE_SINT64 = 18;

    /* pbw_label */
    static final int LABEL_REPEATED = 3;

    static final int ABI_VERSION = 1;

    static final Linker LINKER = Linker.nativeLinker();
    static final AddressLayout PTR = ValueLayout.ADDRESS;
    static final ValueLayout.OfInt I32 = ValueLayout.JAVA_INT; /* C int, int32_t, uint32_t */
    static final ValueLayout.OfLong I64 = ValueLayout.JAVA_LONG; /* int64_t, and size_t on LP64 */
    static final ValueLayout.OfFloat F32 = ValueLayout.JAVA_FLOAT;
    static final ValueLayout.OfDouble F64 = ValueLayout.JAVA_DOUBLE;
    static final Cleaner CLEANER = Cleaner.create();

    /* Holds the segments the per-thread workspace allocates: encode buffers and
     * decode input. A shared arena so any thread can grow its own workspace. */
    private static final Arena WORKSPACE = Arena.ofShared();

    private static final SymbolLookup LIB = loadLibrary();

    /* schema */
    private static final MethodHandle SCHEMA_FREE = down("pbw_schema_free", FunctionDescriptor.ofVoid(PTR));
    private static final MethodHandle SCHEMA_ERROR =
            down("pbw_schema_error", FunctionDescriptor.of(PTR, PTR));
    private static final MethodHandle SCHEMA_FIND = down("pbw_schema_find", FunctionDescriptor.of(PTR, PTR, PTR));
    private static final MethodHandle DESCRIPTOR_SET_LOAD =
            down("pbw_descriptor_set_load", FunctionDescriptor.of(I32, PTR, I64, PTR));
    private static final MethodHandle DESCRIPTOR_NAME =
            down("pbw_descriptor_name", FunctionDescriptor.of(PTR, PTR));
    private static final MethodHandle FIND_FIELD_NAME =
            down("pbw_descriptor_find_field_name", FunctionDescriptor.of(PTR, PTR, PTR));
    private static final MethodHandle FIND_FIELD =
            down("pbw_descriptor_find_field", FunctionDescriptor.of(PTR, PTR, I32));
    private static final MethodHandle FIELD_NUMBER = down("pbw_field_number", FunctionDescriptor.of(I32, PTR));
    private static final MethodHandle FIELD_NAME = down("pbw_field_name", FunctionDescriptor.of(PTR, PTR));
    private static final MethodHandle FIELD_TYPE = down("pbw_field_type", FunctionDescriptor.of(I32, PTR));
    private static final MethodHandle FIELD_LABEL = down("pbw_field_label", FunctionDescriptor.of(I32, PTR));
    private static final MethodHandle FIELD_MESSAGE_TYPE =
            down("pbw_field_message_type", FunctionDescriptor.of(PTR, PTR));

    /* message */
    private static final MethodHandle MESSAGE_NEW = down("pbw_message_new", FunctionDescriptor.of(PTR, PTR));
    private static final MethodHandle MESSAGE_FREE = down("pbw_message_free", FunctionDescriptor.ofVoid(PTR));
    private static final MethodHandle MESSAGE_GET_FIELD =
            down("pbw_message_get_field", FunctionDescriptor.of(PTR, PTR, PTR));
    private static final MethodHandle MESSAGE_SET_FIELD =
            down("pbw_message_set_field", FunctionDescriptor.of(I32, PTR, PTR, PTR));
    private static final MethodHandle MESSAGE_SET_MESSAGE =
            down("pbw_message_set_message", FunctionDescriptor.of(PTR, PTR, I32));
    private static final MethodHandle MESSAGE_CLEAR = down("pbw_message_clear", FunctionDescriptor.ofVoid(PTR, I32));

    /* value */
    private static final MethodHandle VALUE_NEW_INT = down("pbw_value_new_int", FunctionDescriptor.of(PTR, I64));
    private static final MethodHandle VALUE_NEW_BOOL = down("pbw_value_new_bool", FunctionDescriptor.of(PTR, I32));
    private static final MethodHandle VALUE_NEW_FLOAT = down("pbw_value_new_float", FunctionDescriptor.of(PTR, F32));
    private static final MethodHandle VALUE_NEW_DOUBLE =
            down("pbw_value_new_double", FunctionDescriptor.of(PTR, F64));
    private static final MethodHandle VALUE_NEW_BYTES =
            down("pbw_value_new_bytes", FunctionDescriptor.of(PTR, PTR, I64));
    private static final MethodHandle VALUE_FREE = down("pbw_value_free", FunctionDescriptor.ofVoid(PTR));
    private static final MethodHandle VALUE_GET_INT = down("pbw_value_get_int", FunctionDescriptor.of(I64, PTR));
    private static final MethodHandle VALUE_GET_BOOL = down("pbw_value_get_bool", FunctionDescriptor.of(I32, PTR));
    private static final MethodHandle VALUE_GET_FLOAT = down("pbw_value_get_float", FunctionDescriptor.of(F32, PTR));
    private static final MethodHandle VALUE_GET_DOUBLE =
            down("pbw_value_get_double", FunctionDescriptor.of(F64, PTR));
    private static final MethodHandle VALUE_GET_BYTES =
            down("pbw_value_get_bytes", FunctionDescriptor.of(PTR, PTR, PTR));
    private static final MethodHandle VALUE_GET_MESSAGE =
            down("pbw_value_get_message", FunctionDescriptor.of(PTR, PTR));
    private static final MethodHandle VALUE_COUNT = down("pbw_value_count", FunctionDescriptor.of(I64, PTR));
    private static final MethodHandle VALUE_AT = down("pbw_value_at", FunctionDescriptor.of(PTR, PTR, I64));

    /* wire */
    private static final MethodHandle ENCODE_CTX =
            down("pbw_encode_ctx", FunctionDescriptor.of(I32, PTR, PTR, PTR, PTR));
    private static final MethodHandle DECODE = down("pbw_decode", FunctionDescriptor.of(I32, PTR, PTR, I64, PTR));
    private static final MethodHandle STRERROR = down("pbw_strerror", FunctionDescriptor.of(PTR, I32));
    private static final MethodHandle ABI_VERSION_FN = down("pbw_abi_version", FunctionDescriptor.of(I32));

    /* context */
    private static final MethodHandle CTX_NEW = down("pbw_ctx_new", FunctionDescriptor.of(PTR));

    /** The per-thread workspace: one reusable encode context plus C out-parameters. */
    private static final ThreadLocal<Workspace> WORKSPACE_TLS =
            ThreadLocal.withInitial(Workspace::new);

    static final class Workspace {
        final MemorySegment ctx;
        /* Three slots the C layer writes through: a pointer, a size_t and a
         * second pointer (see outPtr/outLen/messageOut). */
        private final MemorySegment out;
        private final MemorySegment len;
        private final MemorySegment message;
        private MemorySegment input;

        Workspace() {
            ctx = notNull(callCtxNew(), "pbwire: cannot create an encode context");
            MemorySegment scratch = WORKSPACE.allocate(3 * Long.BYTES);
            out = scratch.asSlice(0, Long.BYTES);
            len = scratch.asSlice(Long.BYTES, Long.BYTES);
            message = scratch.asSlice(2 * Long.BYTES, Long.BYTES);
        }

        /** Where C writes an encoded byte pointer. */
        MemorySegment outPtr() {
            return out;
        }

        /** Where C writes a size_t (encoded length, byte payload length). */
        MemorySegment outLen() {
            return len;
        }

        /** Where C writes a decoded message pointer. */
        MemorySegment messageOut() {
            return message;
        }

        /**
         * A native buffer of at least {@code size} bytes for bytes crossing into
         * C. Grown on demand and kept for the thread's lifetime (bounded by the
         * largest message the thread has handled), so a steady decode loop stops
         * allocating.
         */
        MemorySegment input(int size) {
            if (input == null || input.byteSize() < size) {
                input = WORKSPACE.allocate(Math.max(64, size));
            }
            return input;
        }
    }

    static Workspace workspace() {
        return WORKSPACE_TLS.get();
    }

    static {
        int abi = abiVersion();
        if (abi != ABI_VERSION) {
            throw new UnsatisfiedLinkError("pbwire: library ABI " + abi + ", binding expects " + ABI_VERSION);
        }
    }

    private static SymbolLookup loadLibrary() {
        String given = System.getProperty("pbwire.library", System.getenv("PBWIRE_LIB"));
        List<String> candidates = given != null ? List.of(given)
                : List.of("build/libpbwire.dylib", "build/libpbwire.so");
        List<String> tried = new ArrayList<>();
        for (String candidate : candidates) {
            Path path = Path.of(candidate);
            if (Files.isRegularFile(path)) {
                return SymbolLookup.libraryLookup(path.toAbsolutePath(), Arena.ofAuto());
            }
            tried.add(candidate);
        }
        throw new UnsatisfiedLinkError("pbwire: shared library not found (tried " + String.join(", ", tried)
                + "); build it with `make shared` and pass -Dpbwire.library=<path>");
    }

    private static MethodHandle down(String name, FunctionDescriptor descriptor) {
        return LINKER.downcallHandle(
                LIB.find(name).orElseThrow(() -> new UnsatisfiedLinkError("pbwire: missing symbol " + name)),
                descriptor);
    }

    /* ------------------------------------------------------------ downcalls */

    /* One method per C function, with the exact signature of its handle so the
     * call site is invokeExact. A native call does not throw: the catch block is
     * unreachable in practice and costs nothing when it is not taken. */

    static void schemaFree(MemorySegment schema) {
        try {
            SCHEMA_FREE.invokeExact(schema);
        } catch (Throwable t) {
            throw rethrow(t);
        }
    }

    static MemorySegment schemaError(MemorySegment schema) {
        try {
            return (MemorySegment) SCHEMA_ERROR.invokeExact(schema);
        } catch (Throwable t) {
            throw rethrow(t);
        }
    }

    static MemorySegment schemaFind(MemorySegment schema, MemorySegment name) {
        try {
            return (MemorySegment) SCHEMA_FIND.invokeExact(schema, name);
        } catch (Throwable t) {
            throw rethrow(t);
        }
    }

    static int descriptorSetLoad(MemorySegment data, long len, MemorySegment out) {
        try {
            return (int) DESCRIPTOR_SET_LOAD.invokeExact(data, len, out);
        } catch (Throwable t) {
            throw rethrow(t);
        }
    }

    static MemorySegment descriptorName(MemorySegment desc) {
        try {
            return (MemorySegment) DESCRIPTOR_NAME.invokeExact(desc);
        } catch (Throwable t) {
            throw rethrow(t);
        }
    }

    static MemorySegment findFieldName(MemorySegment desc, MemorySegment name) {
        try {
            return (MemorySegment) FIND_FIELD_NAME.invokeExact(desc, name);
        } catch (Throwable t) {
            throw rethrow(t);
        }
    }

    static MemorySegment findField(MemorySegment desc, int number) {
        try {
            return (MemorySegment) FIND_FIELD.invokeExact(desc, number);
        } catch (Throwable t) {
            throw rethrow(t);
        }
    }

    static int fieldNumber(MemorySegment field) {
        try {
            return (int) FIELD_NUMBER.invokeExact(field);
        } catch (Throwable t) {
            throw rethrow(t);
        }
    }

    static MemorySegment fieldName(MemorySegment field) {
        try {
            return (MemorySegment) FIELD_NAME.invokeExact(field);
        } catch (Throwable t) {
            throw rethrow(t);
        }
    }

    static int fieldType(MemorySegment field) {
        try {
            return (int) FIELD_TYPE.invokeExact(field);
        } catch (Throwable t) {
            throw rethrow(t);
        }
    }

    static int fieldLabel(MemorySegment field) {
        try {
            return (int) FIELD_LABEL.invokeExact(field);
        } catch (Throwable t) {
            throw rethrow(t);
        }
    }

    static MemorySegment fieldMessageType(MemorySegment field) {
        try {
            return (MemorySegment) FIELD_MESSAGE_TYPE.invokeExact(field);
        } catch (Throwable t) {
            throw rethrow(t);
        }
    }

    static MemorySegment messageNew(MemorySegment desc) {
        try {
            return (MemorySegment) MESSAGE_NEW.invokeExact(desc);
        } catch (Throwable t) {
            throw rethrow(t);
        }
    }

    static void messageFree(MemorySegment message) {
        try {
            MESSAGE_FREE.invokeExact(message);
        } catch (Throwable t) {
            throw rethrow(t);
        }
    }

    static MemorySegment messageGetField(MemorySegment message, MemorySegment field) {
        try {
            return (MemorySegment) MESSAGE_GET_FIELD.invokeExact(message, field);
        } catch (Throwable t) {
            throw rethrow(t);
        }
    }

    static int messageSetField(MemorySegment message, MemorySegment field, MemorySegment value) {
        try {
            return (int) MESSAGE_SET_FIELD.invokeExact(message, field, value);
        } catch (Throwable t) {
            throw rethrow(t);
        }
    }

    static MemorySegment messageSetMessage(MemorySegment message, int number) {
        try {
            return (MemorySegment) MESSAGE_SET_MESSAGE.invokeExact(message, number);
        } catch (Throwable t) {
            throw rethrow(t);
        }
    }

    static void messageClear(MemorySegment message, int number) {
        try {
            MESSAGE_CLEAR.invokeExact(message, number);
        } catch (Throwable t) {
            throw rethrow(t);
        }
    }

    static MemorySegment valueNewInt(long value) {
        try {
            return (MemorySegment) VALUE_NEW_INT.invokeExact(value);
        } catch (Throwable t) {
            throw rethrow(t);
        }
    }

    static MemorySegment valueNewBool(int value) {
        try {
            return (MemorySegment) VALUE_NEW_BOOL.invokeExact(value);
        } catch (Throwable t) {
            throw rethrow(t);
        }
    }

    static MemorySegment valueNewFloat(float value) {
        try {
            return (MemorySegment) VALUE_NEW_FLOAT.invokeExact(value);
        } catch (Throwable t) {
            throw rethrow(t);
        }
    }

    static MemorySegment valueNewDouble(double value) {
        try {
            return (MemorySegment) VALUE_NEW_DOUBLE.invokeExact(value);
        } catch (Throwable t) {
            throw rethrow(t);
        }
    }

    static MemorySegment valueNewBytes(MemorySegment data, long len) {
        try {
            return (MemorySegment) VALUE_NEW_BYTES.invokeExact(data, len);
        } catch (Throwable t) {
            throw rethrow(t);
        }
    }

    static void valueFree(MemorySegment value) {
        try {
            VALUE_FREE.invokeExact(value);
        } catch (Throwable t) {
            throw rethrow(t);
        }
    }

    static long valueGetInt(MemorySegment value) {
        try {
            return (long) VALUE_GET_INT.invokeExact(value);
        } catch (Throwable t) {
            throw rethrow(t);
        }
    }

    static int valueGetBool(MemorySegment value) {
        try {
            return (int) VALUE_GET_BOOL.invokeExact(value);
        } catch (Throwable t) {
            throw rethrow(t);
        }
    }

    static float valueGetFloat(MemorySegment value) {
        try {
            return (float) VALUE_GET_FLOAT.invokeExact(value);
        } catch (Throwable t) {
            throw rethrow(t);
        }
    }

    static double valueGetDouble(MemorySegment value) {
        try {
            return (double) VALUE_GET_DOUBLE.invokeExact(value);
        } catch (Throwable t) {
            throw rethrow(t);
        }
    }

    static MemorySegment valueGetBytes(MemorySegment value, MemorySegment len) {
        try {
            return (MemorySegment) VALUE_GET_BYTES.invokeExact(value, len);
        } catch (Throwable t) {
            throw rethrow(t);
        }
    }

    static MemorySegment valueGetMessage(MemorySegment value) {
        try {
            return (MemorySegment) VALUE_GET_MESSAGE.invokeExact(value);
        } catch (Throwable t) {
            throw rethrow(t);
        }
    }

    static long valueCount(MemorySegment value) {
        try {
            return (long) VALUE_COUNT.invokeExact(value);
        } catch (Throwable t) {
            throw rethrow(t);
        }
    }

    static MemorySegment valueAt(MemorySegment value, long index) {
        try {
            return (MemorySegment) VALUE_AT.invokeExact(value, index);
        } catch (Throwable t) {
            throw rethrow(t);
        }
    }

    static int encodeCtx(MemorySegment ctx, MemorySegment message, MemorySegment out, MemorySegment len) {
        try {
            return (int) ENCODE_CTX.invokeExact(ctx, message, out, len);
        } catch (Throwable t) {
            throw rethrow(t);
        }
    }

    static int decode(MemorySegment desc, MemorySegment data, long len, MemorySegment out) {
        try {
            return (int) DECODE.invokeExact(desc, data, len, out);
        } catch (Throwable t) {
            throw rethrow(t);
        }
    }

    private static int abiVersion() {
        try {
            return (int) ABI_VERSION_FN.invokeExact();
        } catch (Throwable t) {
            throw rethrow(t);
        }
    }

    private static MemorySegment callCtxNew() {
        try {
            return (MemorySegment) CTX_NEW.invokeExact();
        } catch (Throwable t) {
            throw rethrow(t);
        }
    }

    private static RuntimeException rethrow(Throwable t) {
        if (t instanceof RuntimeException e) {
            return e;
        }
        if (t instanceof Error e) {
            throw e;
        }
        return new IllegalStateException("pbwire: native call failed", t);
    }

    /* ------------------------------------------------------------- helpers */

    static boolean isNull(MemorySegment p) {
        return p == null || p.address() == 0;
    }

    /** Reads a NUL-terminated C string from a bare (size-less) pointer. */
    static String cstr(MemorySegment p) {
        return isNull(p) ? "" : p.reinterpret(Long.MAX_VALUE).getString(0);
    }

    static String strerror(int status) {
        return cstr(strerrorPtr(status));
    }

    private static MemorySegment strerrorPtr(int status) {
        try {
            return (MemorySegment) STRERROR.invokeExact(status);
        } catch (Throwable t) {
            throw rethrow(t);
        }
    }

    /** The one copy the binding pays: native bytes into a fresh array. */
    static byte[] toByteArray(MemorySegment data, long length) {
        if (length < 0 || length > Integer.MAX_VALUE) {
            throw new PbwireException("pbwire: native length " + length + " does not fit a Java byte[]");
        }
        byte[] bytes = new byte[(int) length];
        if (length > 0) {
            MemorySegment.copy(data.reinterpret(length), ValueLayout.JAVA_BYTE, 0,
                    MemorySegment.ofArray(bytes), ValueLayout.JAVA_BYTE, 0, (int) length);
        }
        return bytes;
    }

    static PbwireException error(String what, int status) {
        return new PbwireException(what + ": " + strerror(status), status);
    }

    /* Ownership is checked at the boundary: a NULL from the C side never reaches
     * a caller who would dereference it. */
    static MemorySegment notNull(MemorySegment p, String what) {
        if (isNull(p)) {
            throw new PbwireException(what);
        }
        return p;
    }

    /**
     * Cleaner action for a native handle: takes the handle out of its
     * AtomicReference before freeing, so close() and the cleaner can never free
     * it twice.
     */
    static final class FreeOnce implements Runnable {
        private final AtomicReference<MemorySegment> handle;
        private final Consumer<MemorySegment> free;

        FreeOnce(AtomicReference<MemorySegment> handle, Consumer<MemorySegment> free) {
            this.handle = handle;
            this.free = free;
        }

        @Override
        public void run() {
            MemorySegment p = handle.getAndSet(null);
            if (!isNull(p)) {
                free.accept(p);
            }
        }
    }
}

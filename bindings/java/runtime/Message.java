import java.lang.foreign.MemorySegment;
import java.lang.foreign.ValueLayout;
import java.lang.ref.Cleaner;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.atomic.AtomicReference;

/**
 * One dynamic message: a mutable value tree over a pbw_message.
 *
 * <p>Fields are addressed by name or by number; both are resolved through the
 * type's field cache, so a hot {@code set}/{@code get} loop makes no native
 * lookup call. {@code set} replaces a singular field and appends to a repeated
 * one; a field that was never set is absent ({@code get} returns null, typed
 * getters return the Java default).
 *
 * <p>Ownership: a message from {@link MessageType#newMessage} or
 * {@link MessageType#decode} owns its native tree; {@link #close()} frees it and
 * is idempotent. {@link #newMessage} and {@link #getMessage} return child views
 * of an existing tree -- closing or using one after the root is closed throws
 * {@link IllegalStateException} (children are never freed on their own). A
 * cleaner frees a root that was not closed explicitly.
 *
 * <p>Not thread safe, like the C core: one message belongs to one thread.
 */
public final class Message implements AutoCloseable {
    private final MessageType type;
    private final MemorySegment handle;
    private final AtomicReference<MemorySegment> root; /* shared with child views, null once freed */
    private final Cleaner.Cleanable cleanable; /* roots only */

    /** A root owns the tree (and frees it, here or from the cleaner); a view only reads it. */
    private Message(MessageType type, MemorySegment handle, AtomicReference<MemorySegment> root, boolean own) {
        this.type = type;
        this.handle = handle;
        this.root = root;
        /* The action must not reference this message, or the cleaner could never
         * see it become unreachable; it holds the handle reference instead. */
        this.cleanable = own
                ? Pbwire.CLEANER.register(this, new Pbwire.FreeOnce(root, Pbwire::messageFree))
                : null;
    }

    /** A root: owns the tree and frees it. */
    static Message root(MessageType type, MemorySegment handle) {
        return new Message(type, handle, new AtomicReference<>(handle), true);
    }

    /** A child view: the root owns the allocation, so this holds no cleaner. */
    private Message view(MemorySegment child, MessageType childType) {
        return new Message(childType, child, root, false);
    }

    public MessageType type() {
        return type;
    }

    /* ------------------------------------------------------------ setting */

    public Message set(String name, long value) {
        return set(type.field(name), value);
    }

    /** Any integer or enum field; the value is stored as raw 64-bit bits. */
    public Message set(int number, long value) {
        return set(type.field(number), value);
    }

    private Message set(MessageType.FieldInfo field, long value) {
        store(field, Pbwire.valueNewInt(value));
        return this;
    }

    public Message set(String name, boolean value) {
        return set(type.field(name), value);
    }

    public Message set(int number, boolean value) {
        return set(type.field(number), value);
    }

    private Message set(MessageType.FieldInfo field, boolean value) {
        store(field, Pbwire.valueNewBool(value ? 1 : 0));
        return this;
    }

    public Message set(String name, double value) {
        return set(type.field(name), value);
    }

    /** A single overload for a float and a double field: the field type decides. */
    public Message set(int number, double value) {
        return set(type.field(number), value);
    }

    private Message set(MessageType.FieldInfo field, double value) {
        store(field, field.type == Pbwire.TYPE_FLOAT
                ? Pbwire.valueNewFloat((float) value)
                : Pbwire.valueNewDouble(value));
        return this;
    }

    public Message set(String name, String value) {
        return set(type.field(name), value);
    }

    /** A string field; the payload is UTF-8, like every protobuf string. */
    public Message set(int number, String value) {
        return set(type.field(number), value);
    }

    private Message set(MessageType.FieldInfo field, String value) {
        return set(field, value.getBytes(StandardCharsets.UTF_8));
    }

    public Message set(String name, byte[] value) {
        return set(type.field(name), value);
    }

    public Message set(int number, byte[] value) {
        return set(type.field(number), value);
    }

    private Message set(MessageType.FieldInfo field, byte[] value) {
        /* C copies the payload into the value, so the workspace buffer is free
         * again as soon as this returns. */
        Pbwire.Workspace workspace = Pbwire.workspace();
        MemorySegment data = workspace.input(value.length);
        MemorySegment.copy(value, 0, data, ValueLayout.JAVA_BYTE, 0, value.length);
        store(field, Pbwire.valueNewBytes(data, value.length));
        return this;
    }

    /**
     * The nested message of a message field, created in place: a singular field
     * replaces its previous value, a repeated one appends. Set fields on the
     * result; it writes straight into this message's tree.
     */
    public Message newMessage(String name) {
        return newMessage(type.field(name));
    }

    public Message newMessage(int number) {
        return newMessage(type.field(number));
    }

    private Message newMessage(MessageType.FieldInfo field) {
        if (!field.isMessage()) {
            throw new PbwireException(type.name() + ": field " + field.number + " is not a message field");
        }
        MemorySegment child = Pbwire.messageSetMessage(segment(), field.number);
        return view(Pbwire.notNull(child, type.name() + ": cannot create field " + field.number),
                field.subtype());
    }

    /** Drops a field value (and, for a oneof, the group case). */
    public void clear(String name) {
        clear(type.field(name));
    }

    public void clear(int number) {
        clear(type.field(number));
    }

    private void clear(MessageType.FieldInfo field) {
        Pbwire.messageClear(segment(), field.number);
    }

    /* -------------------------------------------------------------- presence */

    public boolean has(String name) {
        return has(type.field(name));
    }

    public boolean has(int number) {
        return has(type.field(number));
    }

    private boolean has(MessageType.FieldInfo field) {
        return !Pbwire.isNull(value(field));
    }

    /* -------------------------------------------------------------- reading */

    /** The field value, or null when absent: Long, Boolean, Float, Double, String, byte[], Message or List. */
    public Object get(String name) {
        return get(type.field(name));
    }

    public Object get(int number) {
        return get(type.field(number));
    }

    private Object get(MessageType.FieldInfo field) {
        MemorySegment value = value(field);
        if (field.repeated) {
            return Pbwire.isNull(value) ? List.of() : list(field, value);
        }
        return Pbwire.isNull(value) ? null : element(field, value);
    }

    public long getLong(String name) {
        return getLong(type.field(name));
    }

    /** An integer or enum field; absent reads as 0. */
    public long getLong(int number) {
        return getLong(type.field(number));
    }

    private long getLong(MessageType.FieldInfo field) {
        MemorySegment value = value(field);
        return Pbwire.isNull(value) ? 0L : Pbwire.valueGetInt(value);
    }

    public int getInt(String name) {
        return getInt(type.field(name));
    }

    public int getInt(int number) {
        return getInt(type.field(number));
    }

    private int getInt(MessageType.FieldInfo field) {
        return (int) getLong(field);
    }

    public boolean getBool(String name) {
        return getBool(type.field(name));
    }

    public boolean getBool(int number) {
        return getBool(type.field(number));
    }

    private boolean getBool(MessageType.FieldInfo field) {
        MemorySegment value = value(field);
        return !Pbwire.isNull(value) && Pbwire.valueGetBool(value) != 0;
    }

    public double getDouble(String name) {
        return getDouble(type.field(name));
    }

    /** A float or a double field; absent reads as 0. */
    public double getDouble(int number) {
        return getDouble(type.field(number));
    }

    private double getDouble(MessageType.FieldInfo field) {
        MemorySegment value = value(field);
        if (Pbwire.isNull(value)) {
            return 0;
        }
        return field.type == Pbwire.TYPE_FLOAT ? Pbwire.valueGetFloat(value) : Pbwire.valueGetDouble(value);
    }

    public float getFloat(String name) {
        return getFloat(type.field(name));
    }

    public float getFloat(int number) {
        return getFloat(type.field(number));
    }

    private float getFloat(MessageType.FieldInfo field) {
        return (float) getDouble(field);
    }

    public String getString(String name) {
        return getString(type.field(name));
    }

    /** A string field, or null when absent. */
    public String getString(int number) {
        return getString(type.field(number));
    }

    private String getString(MessageType.FieldInfo field) {
        byte[] bytes = payload(field);
        return bytes == null ? null : new String(bytes, StandardCharsets.UTF_8);
    }

    public byte[] getBytes(String name) {
        return getBytes(type.field(name));
    }

    /** A bytes (or string) field, or null when absent. */
    public byte[] getBytes(int number) {
        return getBytes(type.field(number));
    }

    private byte[] getBytes(MessageType.FieldInfo field) {
        return payload(field);
    }

    public Message getMessage(String name) {
        return getMessage(type.field(name));
    }

    /** A nested message view, or null when absent; never free it separately. */
    public Message getMessage(int number) {
        return getMessage(type.field(number));
    }

    private Message getMessage(MessageType.FieldInfo field) {
        MemorySegment value = value(field);
        if (Pbwire.isNull(value)) {
            return null;
        }
        MemorySegment child = Pbwire.valueGetMessage(value);
        return Pbwire.isNull(child) ? null : view(child, field.subtype());
    }

    public List<Object> getList(String name) {
        return getList(type.field(name));
    }

    /** The items of a repeated field, empty when absent. */
    public List<Object> getList(int number) {
        return getList(type.field(number));
    }

    private List<Object> getList(MessageType.FieldInfo field) {
        if (!field.repeated) {
            throw new PbwireException(type.name() + ": field " + field.number + " is not repeated");
        }
        MemorySegment value = value(field);
        return Pbwire.isNull(value) ? List.of() : list(field, value);
    }

    /* ---------------------------------------------------------------- wire */

    /**
     * Encodes the message. Field order is schema order and unknown fields from a
     * decode follow, so {@code decode(w).encode()} reproduces a canonical wire
     * image byte for byte.
     */
    public byte[] encode() {
        MemorySegment message = segment();
        Pbwire.Workspace workspace = Pbwire.workspace();
        MemorySegment out = workspace.outPtr();
        MemorySegment len = workspace.outLen();
        int status = Pbwire.encodeCtx(workspace.ctx, message, out, len);
        if (status != Pbwire.OK) {
            throw Pbwire.error(type.name() + ": encode failed", status);
        }
        return Pbwire.toByteArray(out.get(Pbwire.PTR, 0), len.get(Pbwire.I64, 0));
    }

    /** Frees the tree this message owns; callable more than once. */
    @Override
    public void close() {
        if (cleanable != null) {
            cleanable.clean();
        }
    }

    /* ------------------------------------------------------------- internals */

    private void store(MessageType.FieldInfo field, MemorySegment value) {
        Pbwire.notNull(value, type.name() + ": cannot allocate value");
        int status = Pbwire.messageSetField(segment(), field.handle, value);
        if (status != Pbwire.OK) {
            /* Ownership stays with the caller when the set fails. */
            Pbwire.valueFree(value);
            throw Pbwire.error(type.name() + ": field " + field.number, status);
        }
    }

    private MemorySegment value(MessageType.FieldInfo field) {
        return Pbwire.messageGetField(segment(), field.handle);
    }

    private MemorySegment segment() {
        if (root.get() == null) {
            throw new IllegalStateException(type.name() + ": message is closed");
        }
        type.checkOpen();
        return handle;
    }

    private List<Object> list(MessageType.FieldInfo field, MemorySegment value) {
        int n = (int) Pbwire.valueCount(value);
        List<Object> items = new ArrayList<>(n);
        for (int i = 0; i < n; i++) {
            items.add(element(field, Pbwire.valueAt(value, i)));
        }
        return List.copyOf(items);
    }

    /** One scalar field value, as the field type maps it. */
    private Object element(MessageType.FieldInfo field, MemorySegment value) {
        switch (field.type) {
            case Pbwire.TYPE_STRING:
                return new String(payload(value), StandardCharsets.UTF_8);
            case Pbwire.TYPE_BYTES:
                return payload(value);
            case Pbwire.TYPE_MESSAGE:
            case Pbwire.TYPE_GROUP:
                MemorySegment child = Pbwire.valueGetMessage(value);
                return Pbwire.isNull(child) ? null : view(child, field.subtype());
            case Pbwire.TYPE_FLOAT:
                return Pbwire.valueGetFloat(value);
            case Pbwire.TYPE_DOUBLE:
                return Pbwire.valueGetDouble(value);
            case Pbwire.TYPE_BOOL:
                return Pbwire.valueGetBool(value) != 0;
            default:
                return Pbwire.valueGetInt(value);
        }
    }

    /** A bytes or string field's payload, or null when the field is absent. */
    private byte[] payload(MessageType.FieldInfo field) {
        MemorySegment value = value(field);
        return Pbwire.isNull(value) ? null : payload(value);
    }

    private static byte[] payload(MemorySegment value) {
        Pbwire.Workspace workspace = Pbwire.workspace();
        MemorySegment len = workspace.outLen();
        MemorySegment data = Pbwire.valueGetBytes(value, len);
        if (Pbwire.isNull(data)) {
            return new byte[0];
        }
        return Pbwire.toByteArray(data, len.get(Pbwire.I64, 0));
    }
}

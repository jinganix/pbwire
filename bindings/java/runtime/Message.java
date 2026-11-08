import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * One dynamic message: a mutable value tree over a {@code DMsg}.
 *
 * <p>Fields are addressed by name or by number; both resolve through the
 * type's field table, so a hot {@code set}/{@code get} loop costs a map
 * lookup. {@code set} replaces a singular field and appends to a repeated
 * one; a field that was never set is absent ({@code get} returns null, typed
 * getters return the Java default).
 *
 * <p>Nested messages decode lazily and materialize on first access, so a
 * gateway that only re-encodes never pays for parsing. Ownership: a message
 * from {@link MessageType#newMessage} or {@link MessageType#decode} is a
 * root; {@link #close()} marks it closed and is idempotent.
 * {@link #newMessage} and {@link #getMessage} return child views of the
 * same tree -- using or closing one after the root is closed throws
 * {@link IllegalStateException}.
 *
 * <p>Not thread safe: one message belongs to one thread. Share the schema
 * and the types, not the messages.
 */
public final class Message implements AutoCloseable {
    private final MessageType type;
    private final GenericCodec.DMsg data;
    private final AtomicBoolean closed; /* shared with child views */
    private final Message parent; /* a view keeps its root reachable */
    private final boolean own;

    private Message(MessageType type, GenericCodec.DMsg data,
            AtomicBoolean closed, Message parent, boolean own) {
        this.type = type;
        this.data = data;
        this.closed = closed;
        this.parent = parent;
        this.own = own;
    }

    /** A root: closing it retires the whole tree. */
    static Message root(MessageType type, GenericCodec.DMsg data) {
        return new Message(type, data, new AtomicBoolean(false), null, true);
    }

    /** A child view: closing it is a no-op, closing the root retires it. */
    private Message view(GenericCodec.DMsg child, MessageType childType) {
        return new Message(childType, child, closed, parent != null ? parent : this, false);
    }

    public MessageType type() {
        return type;
    }

    /* ------------------------------------------------------------ setting */

    public Message set(String name, long value) {
        return setAt(type.field(name), value);
    }

    /** Any integer or enum field; the value is stored as raw 64-bit bits. */
    public Message set(int number, long value) {
        return setAt(type.field(number), value);
    }

    private Message setAt(int slot, long value) {
        GenericCodec.Field f = type.def.fields[slot];
        switch (f.kind) {
            case GenericCodec.K_INT32:
            case GenericCodec.K_UINT32:
            case GenericCodec.K_SINT32:
            case GenericCodec.K_INT64:
            case GenericCodec.K_UINT64:
            case GenericCodec.K_SINT64:
            case GenericCodec.K_FIXED32:
            case GenericCodec.K_SFIXED32:
            case GenericCodec.K_FIXED64:
            case GenericCodec.K_SFIXED64:
            case GenericCodec.K_ENUM:
                store(slot, f, Long.valueOf(value));
                return this;
            default:
                throw new PbwireException(type.name() + ": field " + f.number
                        + " needs an integer");
        }
    }

    public Message set(String name, boolean value) {
        return setAt(type.field(name), value);
    }

    public Message set(int number, boolean value) {
        return setAt(type.field(number), value);
    }

    private Message setAt(int slot, boolean value) {
        GenericCodec.Field f = type.def.fields[slot];
        if (f.kind != GenericCodec.K_BOOL) {
            throw new PbwireException(type.name() + ": field " + f.number
                    + " needs a boolean");
        }
        store(slot, f, Boolean.valueOf(value));
        return this;
    }

    public Message set(String name, double value) {
        return setAt(type.field(name), value);
    }

    /** A single overload for a float and a double field: the field type decides. */
    public Message set(int number, double value) {
        return setAt(type.field(number), value);
    }

    private Message setAt(int slot, double value) {
        GenericCodec.Field f = type.def.fields[slot];
        if (f.kind != GenericCodec.K_FLOAT && f.kind != GenericCodec.K_DOUBLE) {
            throw new PbwireException(type.name() + ": field " + f.number
                    + " needs a float or double");
        }
        store(slot, f, Double.valueOf(value));
        return this;
    }

    public Message set(String name, String value) {
        return setAt(type.field(name), value);
    }

    /** A string field; the payload is UTF-8, like every protobuf string. */
    public Message set(int number, String value) {
        return setAt(type.field(number), value);
    }

    private Message setAt(int slot, String value) {
        GenericCodec.Field f = type.def.fields[slot];
        if (f.kind != GenericCodec.K_STRING) {
            throw new PbwireException(type.name() + ": field " + f.number
                    + " needs a string");
        }
        store(slot, f, value);
        return this;
    }

    public Message set(String name, byte[] value) {
        return setAt(type.field(name), value);
    }

    public Message set(int number, byte[] value) {
        return setAt(type.field(number), value);
    }

    private Message setAt(int slot, byte[] value) {
        GenericCodec.Field f = type.def.fields[slot];
        if (f.kind != GenericCodec.K_BYTES) {
            // Only a bytes field takes a byte[]: a string field would store the
            // array and blow up later when the codec casts it to String.
            throw new PbwireException(type.name() + ": field " + f.number
                    + " needs bytes");
        }
        store(slot, f, value);
        return this;
    }

    private void store(int slot, GenericCodec.Field f, Object value) {
        segment();
        if (f.repeated) {
            data.add(f.number, value);
        } else {
            data.set(f.number, value);
        }
    }

    /**
     * The nested message of a message field, created in place: a singular field
     * replaces its previous value, a repeated one appends. Set fields on the
     * result; it writes straight into this message's tree.
     */
    public Message newMessage(String name) {
        return newMessageAt(type.field(name));
    }

    public Message newMessage(int number) {
        return newMessageAt(type.field(number));
    }

    private Message newMessageAt(int slot) {
        GenericCodec.Field f = type.def.fields[slot];
        if (f.kind != GenericCodec.K_MESSAGE && f.kind != GenericCodec.K_GROUP) {
            throw new PbwireException(type.name() + ": field " + f.number
                    + " is not a message field");
        }
        segment();
        return view(data.newMessage(f.number), type.subtype(slot));
    }

    /** Drops a field value (and, for a oneof, the group case). */
    public void clear(String name) {
        clearAt(type.field(name));
    }

    public void clear(int number) {
        clearAt(type.field(number));
    }

    private void clearAt(int slot) {
        segment();
        data.clear(type.def.fields[slot].number);
    }

    /* -------------------------------------------------------------- presence */

    public boolean has(String name) {
        return hasAt(type.field(name));
    }

    public boolean has(int number) {
        return hasAt(type.field(number));
    }

    private boolean hasAt(int slot) {
        segment();
        return data.has(type.def.fields[slot].number);
    }

    /* -------------------------------------------------------------- reading */

    /** The field value, or null when absent: Long, Boolean, Double, String, byte[], Message or List. */
    public Object get(String name) {
        return getAt(type.field(name));
    }

    public Object get(int number) {
        return getAt(type.field(number));
    }

    private Object getAt(int slot) {
        segment();
        GenericCodec.Field f = type.def.fields[slot];
        if (f.repeated) {
            return getList(slot);
        }
        Object v = data.get(f.number);
        if (v instanceof GenericCodec.DMsg) {
            return view((GenericCodec.DMsg) v, type.subtype(slot));
        }
        return v;
    }

    public long getLong(String name) {
        return getLongAt(type.field(name));
    }

    /** An integer or enum field; absent reads as 0. */
    public long getLong(int number) {
        return getLongAt(type.field(number));
    }

    private long getLongAt(int slot) {
        Object v = getAt(slot);
        if (v == null) {
            return 0L;
        }
        if (v instanceof Number) {
            return ((Number) v).longValue();
        }
        throw new PbwireException(type.name() + ": field "
                + type.def.fields[slot].number + " is not an integer");
    }

    public int getInt(String name) {
        return getIntAt(type.field(name));
    }

    public int getInt(int number) {
        return getIntAt(type.field(number));
    }

    private int getIntAt(int slot) {
        return (int) getLongAt(slot);
    }

    public boolean getBool(String name) {
        return getBoolAt(type.field(name));
    }

    public boolean getBool(int number) {
        return getBoolAt(type.field(number));
    }

    private boolean getBoolAt(int slot) {
        Object v = getAt(slot);
        if (v == null) {
            return false;
        }
        if (v instanceof Boolean) {
            return (Boolean) v;
        }
        if (v instanceof Number) {
            return ((Number) v).longValue() != 0;
        }
        throw new PbwireException(type.name() + ": field "
                + type.def.fields[slot].number + " is not a boolean");
    }

    public double getDouble(String name) {
        return getDoubleAt(type.field(name));
    }

    /** A float or a double field; absent reads as 0. */
    public double getDouble(int number) {
        return getDoubleAt(type.field(number));
    }

    private double getDoubleAt(int slot) {
        Object v = getAt(slot);
        if (v == null) {
            return 0;
        }
        if (v instanceof Number) {
            return ((Number) v).doubleValue();
        }
        throw new PbwireException(type.name() + ": field "
                + type.def.fields[slot].number + " is not a float");
    }

    public float getFloat(String name) {
        return getFloatAt(type.field(name));
    }

    public float getFloat(int number) {
        return getFloatAt(type.field(number));
    }

    private float getFloatAt(int slot) {
        return (float) getDoubleAt(slot);
    }

    public String getString(String name) {
        return getStringAt(type.field(name));
    }

    /** A string field, or null when absent. */
    public String getString(int number) {
        return getStringAt(type.field(number));
    }

    private String getStringAt(int slot) {
        Object v = getAt(slot);
        if (v == null) {
            return null;
        }
        if (v instanceof String) {
            return (String) v;
        }
        if (v instanceof byte[]) {
            return new String((byte[]) v, StandardCharsets.UTF_8);
        }
        throw new PbwireException(type.name() + ": field "
                + type.def.fields[slot].number + " is not a string");
    }

    public byte[] getBytes(String name) {
        return getBytesAt(type.field(name));
    }

    /** A bytes (or string) field, or null when absent. */
    public byte[] getBytes(int number) {
        return getBytesAt(type.field(number));
    }

    private byte[] getBytesAt(int slot) {
        Object v = getAt(slot);
        if (v == null) {
            return null;
        }
        if (v instanceof byte[]) {
            return Arrays.copyOf((byte[]) v, ((byte[]) v).length);
        }
        if (v instanceof String) {
            return ((String) v).getBytes(StandardCharsets.UTF_8);
        }
        throw new PbwireException(type.name() + ": field "
                + type.def.fields[slot].number + " is not bytes");
    }

    public Message getMessage(String name) {
        return getMessageAt(type.field(name));
    }

    /** A nested message view, or null when absent; never free it separately. */
    public Message getMessage(int number) {
        return getMessageAt(type.field(number));
    }

    private Message getMessageAt(int slot) {
        Object v = getAt(slot);
        if (v == null) {
            return null;
        }
        if (v instanceof Message) {
            return (Message) v;
        }
        throw new PbwireException(type.name() + ": field "
                + type.def.fields[slot].number + " is not a message");
    }

    public List<Object> getList(String name) {
        return getListAt(type.field(name));
    }

    /** The items of a repeated field, empty when absent. */
    public List<Object> getList(int number) {
        return getListAt(type.field(number));
    }

    private List<Object> getListAt(int slot) {
        segment();
        GenericCodec.Field f = type.def.fields[slot];
        if (!f.repeated) {
            throw new PbwireException(type.name() + ": field " + f.number
                    + " is not repeated");
        }
        List<Object> items = data.getList(f.number);
        List<Object> out = new ArrayList<>(items.size());
        for (Object v : items) {
            if (v instanceof GenericCodec.DMsg) {
                out.add(view((GenericCodec.DMsg) v, type.subtype(slot)));
            } else if (v instanceof byte[]) {
                byte[] b = (byte[]) v;
                out.add(Arrays.copyOf(b, b.length));
            } else {
                out.add(v);
            }
        }
        return List.copyOf(out);
    }

    /* ---------------------------------------------------------------- wire */

    /**
     * Encodes the message. Field order is schema order and unknown fields from
     * a decode follow, so {@code decode(w).encode()} reproduces a canonical
     * wire image byte for byte.
     */
    public byte[] encode() {
        segment();
        try {
            return GenericCodec.encode(data);
        } catch (IllegalStateException | IllegalArgumentException e) {
            throw new PbwireException(type.name() + ": encode failed", 1);
        }
    }

    /** Marks the tree closed; callable more than once. Views ignore this. */
    @Override
    public void close() {
        if (own) {
            closed.set(true);
        }
    }

    /* ------------------------------------------------------------- internals */

    private GenericCodec.DMsg segment() {
        if (closed.get()) {
            throw new IllegalStateException(type.name() + ": message is closed");
        }
        type.checkOpen();
        return data;
    }
}

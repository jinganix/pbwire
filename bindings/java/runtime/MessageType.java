import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.ValueLayout;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;

/**
 * A message descriptor: creates empty messages and decodes wire bytes into one.
 *
 * <p>Field facts (number, type, label, sub-message type) are resolved through
 * the C descriptor once and cached here, so {@code set}/{@code get} by name or
 * number cost a map lookup instead of a native call. A schema's descriptors are
 * read-only after load, which is what makes the cache safe to share between
 * threads.
 */
public final class MessageType {
    private final Schema schema;
    private final MemorySegment desc;
    private final String name;
    private final Map<String, FieldInfo> byName = new ConcurrentHashMap<>();
    private final Map<Integer, FieldInfo> byNumber = new ConcurrentHashMap<>();

    MessageType(Schema schema, MemorySegment desc) {
        this.schema = schema;
        this.desc = desc;
        this.name = Pbwire.cstr(Pbwire.descriptorName(desc));
    }

    /** Fully qualified name, e.g. {@code "pbwire.test.Sample"}. */
    public String name() {
        return name;
    }

    /** An empty, mutable message of this type. */
    public Message newMessage() {
        checkOpen();
        return Message.root(this, Pbwire.notNull(Pbwire.messageNew(desc), name + ": cannot create message"));
    }

    /**
     * Decodes wire bytes; unknown fields are kept and re-encoded. The bytes are
     * the authority on presence, so a field absent on the wire stays absent. The
     * returned tree is mutable.
     */
    public Message decode(byte[] wire) {
        checkOpen();
        Pbwire.Workspace workspace = Pbwire.workspace();
        MemorySegment data = workspace.input(wire.length);
        MemorySegment.copy(wire, 0, data, ValueLayout.JAVA_BYTE, 0, wire.length);
        MemorySegment out = workspace.messageOut();
        int status = Pbwire.decode(desc, data, wire.length, out);
        if (status != Pbwire.OK) {
            throw Pbwire.error(name + ": decode failed", status);
        }
        return Message.root(this, Pbwire.notNull(out.get(Pbwire.PTR, 0), name + ": no message decoded"));
    }

    /** The field with this name; throws when the schema has no such field. */
    FieldInfo field(String fieldName) {
        FieldInfo field = byName.get(fieldName);
        return field != null ? field : resolve(fieldName);
    }

    /** The field with this number; throws when the schema has no such field. */
    FieldInfo field(int number) {
        FieldInfo field = byNumber.get(number);
        return field != null ? field : resolve(number);
    }

    private FieldInfo resolve(String fieldName) {
        FieldInfo field;
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment handle = Pbwire.findFieldName(desc, arena.allocateFrom(fieldName));
            if (Pbwire.isNull(handle)) {
                throw new PbwireException(name + ": no such field: " + fieldName);
            }
            field = new FieldInfo(this, handle);
        }
        cache(field);
        return field;
    }

    private FieldInfo resolve(int number) {
        MemorySegment handle = Pbwire.findField(desc, number);
        if (Pbwire.isNull(handle)) {
            throw new PbwireException(name + ": no such field number: " + number);
        }
        FieldInfo field = new FieldInfo(this, handle);
        cache(field);
        return field;
    }

    private void cache(FieldInfo field) {
        /* A racing thread may resolve the same field twice; both entries are
         * equal in value, so putIfAbsent keeps one and the other is dropped. */
        byNumber.putIfAbsent(field.number, field);
        byName.putIfAbsent(field.name, field);
    }

    Schema schema() {
        return schema;
    }

    /** The native descriptor; throws once the schema is closed. */
    MemorySegment desc() {
        checkOpen();
        return desc;
    }

    void checkOpen() {
        schema.segment();
    }

    @Override
    public String toString() {
        return name;
    }

    /**
     * One field's stable facts, resolved once per type. The C descriptor pointer
     * stays valid for the schema's lifetime; using it always goes through a
     * message, which checks that the schema is still open first.
     */
    static final class FieldInfo {
        final MessageType owner;
        final String name;
        final MemorySegment handle;
        final int number;
        final int type;
        final boolean repeated;
        private MessageType subtype;

        FieldInfo(MessageType owner, MemorySegment handle) {
            this.owner = owner;
            this.handle = handle;
            this.name = Pbwire.cstr(Pbwire.fieldName(handle));
            this.number = Pbwire.fieldNumber(handle);
            this.type = Pbwire.fieldType(handle);
            this.repeated = Pbwire.fieldLabel(handle) == Pbwire.LABEL_REPEATED;
        }

        boolean isMessage() {
            return type == Pbwire.TYPE_MESSAGE || type == Pbwire.TYPE_GROUP;
        }

        /** The message type of a message/group field, resolved on first use. */
        synchronized MessageType subtype() {
            if (subtype == null) {
                MemorySegment sub = Pbwire.fieldMessageType(handle);
                subtype = Pbwire.isNull(sub) ? owner : new MessageType(owner.schema(), sub);
            }
            return subtype;
        }
    }
}

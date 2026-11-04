import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;

/**
 * A message descriptor: creates empty messages and decodes wire bytes into one.
 *
 * <p>Field facts (number, kind, label, sub-message type) are resolved once per
 * type and live in the shared definition, so {@code set}/{@code get} by name
 * or number cost a map lookup instead of a native call. A schema's
 * descriptors are read-only after load, which is what makes them safe to share
 * between threads.
 */
public final class MessageType {
    private final Schema schema;
    final GenericCodec.MessageDef def;
    private final Map<Integer, MessageType> subtypes = new ConcurrentHashMap<>();

    MessageType(Schema schema, GenericCodec.MessageDef def) {
        this.schema = schema;
        this.def = def;
    }

    /** Fully qualified name, e.g. {@code "pbwire.test.Sample"}. */
    public String name() {
        return def.fullName;
    }

    /** An empty, mutable message of this type. */
    public Message newMessage() {
        checkOpen();
        return Message.root(this, new GenericCodec.DMsg(def));
    }

    /**
     * Decodes wire bytes; unknown fields are kept and re-encoded. The bytes are
     * the authority on presence, so a field absent on the wire stays absent. The
     * returned tree materializes lazily: nested messages and strings parse on
     * first access, untouched slices re-encode verbatim.
     */
    public Message decode(byte[] wire) {
        checkOpen();
        try {
            return Message.root(this, GenericCodec.decode(def, wire));
        } catch (IllegalArgumentException e) {
            throw new PbwireException(name() + ": decode failed", 1);
        }
    }

    /** The field slot with this name; throws when the schema has no such field. */
    int field(String fieldName) {
        Integer slot = def.byName.get(fieldName);
        if (slot == null) {
            throw new PbwireException(name() + ": no such field: " + fieldName);
        }
        return slot;
    }

    /** The field slot with this number; throws when the schema has no such field. */
    int field(int number) {
        if (number < 0 || number >= def.slotByFnum.length
                || def.slotByFnum[number] < 0) {
            throw new PbwireException(name() + ": no such field number: " + number);
        }
        return def.slotByFnum[number];
    }

    /** The message type of a message/group field, resolved on first use. */
    MessageType subtype(int slot) {
        GenericCodec.Field f = def.fields[slot];
        MessageType sub = subtypes.get(slot);
        if (sub == null) {
            sub = new MessageType(schema, f.msgType);
            MessageType prev = subtypes.putIfAbsent(slot, sub);
            if (prev != null) {
                sub = prev;
            }
        }
        return sub;
    }

    void checkOpen() {
        schema.checkOpen();
    }

    @Override
    public String toString() {
        return name();
    }
}

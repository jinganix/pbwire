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
    // Flat name -> slot table, built once: the model fills address fields by
    // name, so this is the fill's per-field lookup. An open-addressed array
    // costs one hash and one probe instead of a HashMap node walk, and it is
    // the same cost as an ordinal without regenerating the model.
    private volatile String[] nameKeys;
    private volatile int[] nameSlots;
    private int nameMask;

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
        return decode(wire, Limits.defaults());
    }

    /** Bounded decode, like the core's {@code pbw_decode_limits}. */
    public Message decode(byte[] wire, Limits lim) {
        checkOpen();
        try {
            return Message.root(this, GenericCodec.decode(def, wire, lim));
        } catch (PbwireException e) {
            throw e;
        } catch (IllegalArgumentException e) {
            throw new PbwireException(name() + ": decode failed", 1);
        }
    }

    /** One delimited frame: the message plus its prefix + body length. */
    public static final class Delimited {
        public final Message message;
        public final int consumed;

        Delimited(Message message, int consumed) {
            this.message = message;
            this.consumed = consumed;
        }
    }

    /** Decodes one length-delimited frame; trailing bytes are left over. */
    public Delimited decodeDelimited(byte[] wire) {
        return decodeDelimited(wire, 0, Limits.defaults());
    }

    /** Same with an explicit offset and bounds. */
    public Delimited decodeDelimited(byte[] wire, int off, Limits lim) {
        checkOpen();
        try {
            GenericCodec.Frame f = GenericCodec.decodeDelimited(def, wire, off, lim);
            return new Delimited(Message.root(this, f.msg), f.consumed);
        } catch (PbwireException e) {
            throw e;
        } catch (IllegalArgumentException e) {
            throw new PbwireException(name() + ": decode failed", 1);
        }
    }

    /**
     * Single-level scan of wire bytes without building a message tree:
     * one callback per field, in wire order. Returns the wire length when
     * every field was visited, the failing field's start offset on malformed
     * input (or when the callback returns non-zero), or 0 when the input
     * exceeds the byte cap. Only a null callback throws; malformed input
     * and cap breaches are reported via the returned offset.
     */
    public int visit(byte[] wire, Visit.Visitor cb) {
        return visit(wire, Limits.defaults(), cb);
    }

    /** Bounded visit, like the core's {@code pbw_visit_limits}. */
    public int visit(byte[] wire, Limits lim, Visit.Visitor cb) {
        checkOpen();
        try {
            return GenericCodec.scan(def, wire, lim, cb);
        } catch (PbwireException e) {
            throw e;
        } catch (IllegalArgumentException e) {
            throw new PbwireException(name() + ": visit failed", 1);
        }
    }

    /** Parses JSON mapping into a new message; unknown fields fail. */
    public Message parseJson(String json) {
        return parseJson(json, false, Limits.defaults());
    }

    /** Parses JSON mapping, optionally skipping unknown field names. */
    public Message parseJson(String json, boolean ignoreUnknown, Limits lim) {
        checkOpen();
        try {
            return Message.root(this, Json.parse(def, json, ignoreUnknown, lim));
        } catch (PbwireException e) {
            throw new PbwireException(name() + ": " + e.getMessage(), e.status());
        } catch (IllegalArgumentException e) {
            throw new PbwireException(name() + ": parse failed", 1);
        }
    }

    /** Parses text format into a new message. */
    public Message parseText(String text) {
        return parseText(text, Limits.defaults());
    }

    /** Parses text format with explicit bounds. */
    public Message parseText(String text, Limits lim) {
        checkOpen();
        try {
            return Message.root(this, Text.parse(def, text, lim));
        } catch (PbwireException e) {
            throw new PbwireException(name() + ": " + e.getMessage(), e.status());
        } catch (IllegalArgumentException e) {
            throw new PbwireException(name() + ": parse failed", 1);
        }
    }

    /** The field slot with this name; throws when the schema has no such field. */
    int field(String fieldName) {
        String[] keys = nameKeys;
        if (keys == null) {
            keys = buildNames();
        }
        int h = fieldName.hashCode();
        h ^= h >>> 16;
        int i = h & nameMask;
        while (true) {
            String k = keys[i];
            if (k == null) {
                break;
            }
            if (k.equals(fieldName)) {
                return nameSlots[i];
            }
            i = (i + 1) & nameMask;
        }
        throw new PbwireException(name() + ": no such field: " + fieldName);
    }

    /** Builds the flat name table from the definition's name map. */
    private String[] buildNames() {
        int cap = 8;
        while (cap < def.byName.size() * 2) {
            cap <<= 1;
        }
        String[] keys = new String[cap];
        int[] slots = new int[cap];
        int mask = cap - 1;
        for (Map.Entry<String, Integer> e : def.byName.entrySet()) {
            int h = e.getKey().hashCode();
            h ^= h >>> 16;
            int i = h & mask;
            while (keys[i] != null) {
                i = (i + 1) & mask;
            }
            keys[i] = e.getKey();
            slots[i] = e.getValue();
        }
        nameSlots = slots;
        nameMask = mask;
        nameKeys = keys;
        return keys;
    }

    /** The field slot with this number; throws when the schema has no such field. */
    int field(int number) {
        int slot = def.lookup(number);
        if (slot < 0) {
            throw new PbwireException(name() + ": no such field number: " + number);
        }
        return slot;
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

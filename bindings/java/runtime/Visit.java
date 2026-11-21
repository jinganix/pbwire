/**
 * Single-level wire scan, like the core's {@code pbw_visit}: one validating
 * tag pass, one callback per field, no message tree. Nested messages and
 * packed runs arrive as LEN payloads for the caller to recurse into (via
 * another {@code visit} on the payload); groups arrive as raw bytes.
 *
 * <p>{@link VisitValue#data} aliases the scanned input: LEN payloads point at
 * the length body and group frames at the field's start tag, so a value is
 * only valid until the caller mutates or drops the input. Scalar shapes
 * (varint, fixed32, fixed64) carry their raw wire bits in {@link VisitValue#u64}
 * with no zigzag or sign-extension applied.
 */
public final class Visit {
    private Visit() {
    }

    /** One scanned field: raw wire content, no allocation, no value tree. */
    public static final class VisitValue {
        /** VARINT/32BIT/64BIT: raw wire bits. Unused for the shapes below. */
        public long u64;
        /**
         * LEN: the payload bytes (a nested message, a packed run, a string);
         * START_GROUP: the whole field (tag through the matching END_GROUP).
         * Aliases the scanned input; NULL for the scalar shapes above.
         */
        public byte[] data;
        /** Payload/raw start inside {@link #data}; 0 for scalar shapes. */
        public int off;
        /** Payload/raw length; 0 for the scalar shapes. */
        public int len;
    }

    /**
     * Scan callback: {@code field} is NULL when the number is unknown to the
     * schema. {@code number}/{@code wireType} are what the wire carries: a
     * known field may carry another wire type (a packed LEN for a repeated
     * scalar, or a mismatch the reference decoders keep as an unknown field).
     * Return 0 to continue, anything else aborts the scan with that field's
     * start offset.
     */
    @FunctionalInterface
    public interface Visitor {
        int visit(GenericCodec.Field field, int number, int wireType,
                VisitValue value);
    }
}

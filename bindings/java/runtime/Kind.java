/**
 * Wire kinds of the table engine (its own numbering, not protobuf's).
 * The generated tables name one of these per field; PbCodec owns the
 * single switch that reads and writes every kind.
 */
final class Kind {
    static final int INT32 = 0;
    static final int UINT32 = 1;
    static final int SINT32 = 2;
    static final int INT64 = 3;
    static final int UINT64 = 4;
    static final int SINT64 = 5;
    static final int BOOL = 6;
    static final int ENUM = 7;
    static final int FIXED32 = 8;
    static final int SFIXED32 = 9;
    static final int FLOAT = 10;
    static final int FIXED64 = 11;
    static final int SFIXED64 = 12;
    static final int DOUBLE = 13;
    static final int STRING = 14;
    static final int BYTES = 15;
    static final int MESSAGE = 16;
    static final int GROUP = 17;

    private Kind() {
    }

    /** The packable numeric kinds: packed runs cross every one of them. */
    static boolean isPackable(int kind) {
        return kind <= DOUBLE;
    }
}

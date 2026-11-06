import java.util.List;
import java.util.function.BiConsumer;
import java.util.function.Function;
import java.util.function.ObjDoubleConsumer;
import java.util.function.ObjIntConsumer;
import java.util.function.ObjLongConsumer;
import java.util.function.Predicate;
import java.util.function.Supplier;
import java.util.function.ToDoubleFunction;
import java.util.function.ToIntFunction;
import java.util.function.ToLongFunction;

/**
 * One field's compiled facts plus the interface lambdas that read and write
 * it. Generated per message; the engine only interprets this table.
 *
 * <p>{@code O} is the read side (the message interface), {@code B} the write
 * side (its builder). Encode uses {@code get}/{@code hasM}/{@code caseOf};
 * decode uses {@code set}/{@code hasB}/{@code factory}/{@code sub}.
 * Primitives ride specialized functional interfaces, so nothing boxes on the
 * singular path; repeated lists box like every Java protobuf API.
 */
final class FieldMeta<O, B> {
    final int number;
    final int wt; // singular wire type
    final int kind; // Kind.*
    final boolean explicit; // a has-bit decides, not the zero value
    final boolean required; // proto2 required: checked after parsing
    final boolean packed; // packed repeated numeric
    final boolean repeated;
    final int oneof; // -1 = none
    final byte[] tag; // singular tag bytes
    final byte[] ptag; // packed tag bytes (null unless packed)
    final byte[] etag; // group end tag (null unless a group)
    final Object get; // getter lambda on O (or list getter for repeated)
    final Object set; // setter/adder lambda on B
    final Predicate<O> hasM; // null unless explicit/message (encode gate)
    final Predicate<B> hasB; // null unless required (decode check)
    final ToIntFunction<O> caseOf; // null unless a oneof member (encode gate)
    final Supplier<?> factory; // message-typed only: fresh sub-builder
    final Meta<?, ?> sub; // message-typed only: the sub-message table
    final Function<?, ?> finish; // message-typed only: sub-builder to built

    private FieldMeta(int number, int wt, int kind, boolean explicit, boolean required,
            boolean packed, boolean repeated, int oneof, byte[] tag, byte[] ptag,
            byte[] etag, Object get, Object set, Predicate<O> hasM, Predicate<B> hasB,
            ToIntFunction<O> caseOf, Supplier<?> factory, Meta<?, ?> sub,
            Function<?, ?> finish) {
        this.number = number;
        this.wt = wt;
        this.kind = kind;
        this.explicit = explicit;
        this.required = required;
        this.packed = packed;
        this.repeated = repeated;
        this.oneof = oneof;
        this.tag = tag;
        this.ptag = ptag;
        this.etag = etag;
        this.get = get;
        this.set = set;
        this.hasM = hasM;
        this.hasB = hasB;
        this.caseOf = caseOf;
        this.factory = factory;
        this.sub = sub;
        this.finish = finish;
    }

    /** Singular wire type of one engine kind. */
    static int wtFor(int kind) {
        switch (kind) {
            case Kind.FIXED64:
            case Kind.SFIXED64:
            case Kind.DOUBLE:
                return 1;
            case Kind.STRING:
            case Kind.BYTES:
            case Kind.MESSAGE:
                return 2;
            case Kind.GROUP:
                return 3;
            case Kind.FIXED32:
            case Kind.SFIXED32:
            case Kind.FLOAT:
                return 5;
            default:
                return 0;
        }
    }

    // int-shaped singular: int32 uint32 sint32 enum
    static <O, B> FieldMeta<O, B> ofInt(int number, int kind, ToIntFunction<O> get,
            ObjIntConsumer<B> set, Predicate<O> hasM, Predicate<B> hasB, boolean required) {
        return new FieldMeta<>(number, wtFor(kind), kind, hasM != null, required, false, false, -1,
                Wire.tagBytes(number, wtFor(kind)), null, null, get, set, hasM, hasB, null, null, null, null);
    }

    // long-shaped singular: int64 uint64 sint64
    static <O, B> FieldMeta<O, B> ofLong(int number, int kind, ToLongFunction<O> get,
            ObjLongConsumer<B> set, Predicate<O> hasM, Predicate<B> hasB, boolean required) {
        return new FieldMeta<>(number, wtFor(kind), kind, hasM != null, required, false, false, -1,
                Wire.tagBytes(number, wtFor(kind)), null, null, get, set, hasM, hasB, null, null, null, null);
    }

    static <O, B> FieldMeta<O, B> ofFloat(int number, Function<O, Float> get,
            BiConsumer<B, Float> set, Predicate<O> hasM, Predicate<B> hasB, boolean required) {
        return new FieldMeta<>(number, 5, Kind.FLOAT, hasM != null, required, false, false, -1,
                Wire.tagBytes(number, 5), null, null, get, set, hasM, hasB, null, null, null, null);
    }

    static <O, B> FieldMeta<O, B> ofDouble(int number, ToDoubleFunction<O> get,
            ObjDoubleConsumer<B> set, Predicate<O> hasM, Predicate<B> hasB, boolean required) {
        return new FieldMeta<>(number, 1, Kind.DOUBLE, hasM != null, required, false, false, -1,
                Wire.tagBytes(number, 1), null, null, get, set, hasM, hasB, null, null, null, null);
    }

    static <O, B> FieldMeta<O, B> ofBool(int number, Predicate<O> get,
            BiConsumer<B, Boolean> set, Predicate<O> hasM, Predicate<B> hasB, boolean required) {
        return new FieldMeta<>(number, 0, Kind.BOOL, hasM != null, required, false, false, -1,
                Wire.tagBytes(number, 0), null, null, get, set, hasM, hasB, null, null, null, null);
    }

    static <O, B> FieldMeta<O, B> ofString(int number, Function<O, String> get,
            BiConsumer<B, String> set, Predicate<O> hasM, Predicate<B> hasB, boolean required) {
        return new FieldMeta<>(number, 2, Kind.STRING, hasM != null, required, false, false, -1,
                Wire.tagBytes(number, 2), null, null, get, set, hasM, hasB, null, null, null, null);
    }

    static <O, B> FieldMeta<O, B> ofBytes(int number, Function<O, byte[]> get,
            BiConsumer<B, byte[]> set, Predicate<O> hasM, Predicate<B> hasB, boolean required) {
        return new FieldMeta<>(number, 2, Kind.BYTES, hasM != null, required, false, false, -1,
                Wire.tagBytes(number, 2), null, null, get, set, hasM, hasB, null, null, null, null);
    }

    // fixed32/sfixed32/fixed64/sfixed64 ride the int/long factories by kind.
    static <O, B, S, SB> FieldMeta<O, B> ofMessage(int number, Function<O, S> get,
            BiConsumer<B, S> set, Supplier<SB> factory, Meta<?, ?> sub, Function<SB, S> finish,
            Predicate<O> hasM, Predicate<B> hasB, boolean required) {
        return new FieldMeta<>(number, 2, Kind.MESSAGE, true, required, false, false, -1,
                Wire.tagBytes(number, 2), null, null, get, set, hasM, hasB, null, factory, sub,
                finish);
    }

    static <O, B, S, SB> FieldMeta<O, B> ofGroup(int number, Function<O, S> get,
            BiConsumer<B, S> set, Supplier<SB> factory, Meta<?, ?> sub, Function<SB, S> finish,
            Predicate<O> hasM, Predicate<B> hasB) {
        return new FieldMeta<>(number, 3, Kind.GROUP, true, false, false, false, -1,
                Wire.tagBytes(number, 3), null, Wire.tagBytes(number, 4),
                get, set, hasM, hasB, null, factory, sub, finish);
    }

    // int-shaped repeated: int32 uint32 sint32 enum fixed32 sfixed32
    static <O, B> FieldMeta<O, B> ofRepInt(int number, int kind, boolean packed,
            Function<O, ? extends List<?>> get, ObjIntConsumer<B> add) {
        return new FieldMeta<>(number, wtFor(kind), kind, false, false, packed, true, -1,
                Wire.tagBytes(number, wtFor(kind)),
                packed ? Wire.tagBytes(number, 2) : null, null,
                get, add, null, null, null, null, null, null);
    }

    // long-shaped repeated: int64 uint64 sint64 fixed64 sfixed64
    static <O, B> FieldMeta<O, B> ofRepLong(int number, int kind, boolean packed,
            Function<O, ? extends List<?>> get, ObjLongConsumer<B> add) {
        return new FieldMeta<>(number, wtFor(kind), kind, false, false, packed, true, -1,
                Wire.tagBytes(number, wtFor(kind)),
                packed ? Wire.tagBytes(number, 2) : null, null,
                get, add, null, null, null, null, null, null);
    }

    static <O, B> FieldMeta<O, B> ofRepFloat(int number, int kind, boolean packed,
            Function<O, ? extends List<?>> get, BiConsumer<B, Float> add) {
        return new FieldMeta<>(number, 5, kind, false, false, packed, true, -1,
                Wire.tagBytes(number, 5),
                packed ? Wire.tagBytes(number, 2) : null, null,
                get, add, null, null, null, null, null, null);
    }

    static <O, B> FieldMeta<O, B> ofRepDouble(int number, int kind, boolean packed,
            Function<O, ? extends List<?>> get, ObjDoubleConsumer<B> add) {
        return new FieldMeta<>(number, 1, kind, false, false, packed, true, -1,
                Wire.tagBytes(number, 1),
                packed ? Wire.tagBytes(number, 2) : null, null,
                get, add, null, null, null, null, null, null);
    }

    static <O, B> FieldMeta<O, B> ofRepBool(int number, boolean packed,
            Function<O, ? extends List<?>> get, BiConsumer<B, Boolean> add) {
        return new FieldMeta<>(number, 0, Kind.BOOL, false, false, packed, true, -1,
                Wire.tagBytes(number, 0),
                packed ? Wire.tagBytes(number, 2) : null, null,
                get, add, null, null, null, null, null, null);
    }

    static <O, B> FieldMeta<O, B> ofRepString(int number, Function<O, ? extends List<?>> get,
            BiConsumer<B, String> add) {
        return new FieldMeta<>(number, 2, Kind.STRING, false, false, false, true, -1,
                Wire.tagBytes(number, 2), null, null, get, add, null, null, null, null, null, null);
    }

    static <O, B> FieldMeta<O, B> ofRepBytes(int number, Function<O, ? extends List<?>> get,
            BiConsumer<B, byte[]> add) {
        return new FieldMeta<>(number, 2, Kind.BYTES, false, false, false, true, -1,
                Wire.tagBytes(number, 2), null, null, get, add, null, null, null, null, null, null);
    }

    static <O, B, S, SB> FieldMeta<O, B> ofRepMessage(int number,
            Function<O, ? extends List<?>> get, BiConsumer<B, S> add,
            Supplier<SB> factory, Meta<?, ?> sub, Function<SB, S> finish) {
        return new FieldMeta<>(number, 2, Kind.MESSAGE, false, false, false, true, -1,
                Wire.tagBytes(number, 2), null, null, get, add, null, null, null, factory, sub,
                finish);
    }

    static <O, B, S, SB> FieldMeta<O, B> ofRepGroup(int number,
            Function<O, ? extends List<?>> get, BiConsumer<B, S> add,
            Supplier<SB> factory, Meta<?, ?> sub, Function<SB, S> finish) {
        return new FieldMeta<>(number, 3, Kind.GROUP, false, false, false, true, -1,
                Wire.tagBytes(number, 3), null, Wire.tagBytes(number, 4),
                get, add, null, null, null, factory, sub, finish);
    }

    // Oneof members: the encode gate is the case number, the Builder setter
    // records the case, so decode needs no extra state.
    static <O, B> FieldMeta<O, B> ofOneofInt(int number, int kind, ToIntFunction<O> get,
            ObjIntConsumer<B> set, ToIntFunction<O> caseOf) {
        return new FieldMeta<>(number, wtFor(kind), kind, false, false, false, false, 0,
                Wire.tagBytes(number, wtFor(kind)), null, null, get, set, null, null, caseOf, null, null, null);
    }

    static <O, B> FieldMeta<O, B> ofOneofLong(int number, int kind, ToLongFunction<O> get,
            ObjLongConsumer<B> set, ToIntFunction<O> caseOf) {
        return new FieldMeta<>(number, wtFor(kind), kind, false, false, false, false, 0,
                Wire.tagBytes(number, wtFor(kind)), null, null, get, set, null, null, caseOf, null, null, null);
    }

    static <O, B> FieldMeta<O, B> ofOneofFloat(int number, Function<O, Float> get,
            BiConsumer<B, Float> set, ToIntFunction<O> caseOf) {
        return new FieldMeta<>(number, 5, Kind.FLOAT, false, false, false, false, 0,
                Wire.tagBytes(number, 5), null, null, get, set, null, null, caseOf, null, null, null);
    }

    static <O, B> FieldMeta<O, B> ofOneofDouble(int number, ToDoubleFunction<O> get,
            ObjDoubleConsumer<B> set, ToIntFunction<O> caseOf) {
        return new FieldMeta<>(number, 1, Kind.DOUBLE, false, false, false, false, 0,
                Wire.tagBytes(number, 1), null, null, get, set, null, null, caseOf, null, null, null);
    }

    static <O, B> FieldMeta<O, B> ofOneofBool(int number, Predicate<O> get,
            BiConsumer<B, Boolean> set, ToIntFunction<O> caseOf) {
        return new FieldMeta<>(number, 0, Kind.BOOL, false, false, false, false, 0,
                Wire.tagBytes(number, 0), null, null, get, set, null, null, caseOf, null, null, null);
    }

    static <O, B> FieldMeta<O, B> ofOneofString(int number, Function<O, String> get,
            BiConsumer<B, String> set, ToIntFunction<O> caseOf) {
        return new FieldMeta<>(number, 2, Kind.STRING, false, false, false, false, 0,
                Wire.tagBytes(number, 2), null, null, get, set, null, null, caseOf, null, null, null);
    }

    static <O, B> FieldMeta<O, B> ofOneofBytes(int number, Function<O, byte[]> get,
            BiConsumer<B, byte[]> set, ToIntFunction<O> caseOf) {
        return new FieldMeta<>(number, 2, Kind.BYTES, false, false, false, false, 0,
                Wire.tagBytes(number, 2), null, null, get, set, null, null, caseOf, null, null, null);
    }

    static <O, B, S, SB> FieldMeta<O, B> ofOneofMessage(int number, Function<O, S> get,
            BiConsumer<B, S> set, Supplier<SB> factory, Meta<?, ?> sub, Function<SB, S> finish,
            ToIntFunction<O> caseOf) {
        return new FieldMeta<>(number, 2, Kind.MESSAGE, false, false, false, false, 0,
                Wire.tagBytes(number, 2), null, null, get, set, null, null, caseOf, factory, sub,
                finish);
    }

    static <O, B, S, SB> FieldMeta<O, B> ofOneofGroup(int number, Function<O, S> get,
            BiConsumer<B, S> set, Supplier<SB> factory, Meta<?, ?> sub, Function<SB, S> finish,
            ToIntFunction<O> caseOf) {
        return new FieldMeta<>(number, 3, Kind.GROUP, false, false, false, false, 0,
                Wire.tagBytes(number, 3), null, Wire.tagBytes(number, 4),
                get, set, null, null, caseOf, factory, sub, finish);
    }
}

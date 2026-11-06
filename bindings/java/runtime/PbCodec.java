import java.nio.charset.StandardCharsets;
import java.util.ArrayDeque;
import java.util.Arrays;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;
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
 * The single table engine: every generated message encodes and decodes
 * through these two loops, driven by its Meta plus the interface lambdas.
 * Wire framing (tags, zigzag, lengths, skips, groups) stays in Wire; field
 * facts stay in the generated tables; this file owns only the traversal.
 */
public final class PbCodec {
    private PbCodec() {
    }

    /** One registered message: its table plus how to build it. */
    private static final class Entry {
        final Meta<?, ?> meta;
        final Supplier<?> factory;
        final Function<?, ?> finish;

        Entry(Meta<?, ?> meta, Supplier<?> factory, Function<?, ?> finish) {
            this.meta = meta;
            this.factory = factory;
            this.finish = finish;
        }
    }

    // Message classes stay plain data: each generated <Xxx>Meta file registers
    // its table here from its static initializer, so no codec state lives in
    // the message. A hand-written class implementing the same OrBuilder interface
    // replaces the message with one register call and shares the same table.
    private static final Map<Class<?>, Entry> REGISTRY = new ConcurrentHashMap<>();

    // Interface registrations: one per OrBuilder, so any implementation class
    // encodes with no per-class call, and parseFrom through the interface
    // decodes into whatever Builder the latest registration names. A later
    // registerInterface call replaces the earlier one (custom types win).
    private static final Map<Class<?>, Entry> IFACES = new ConcurrentHashMap<>();

    public static <O, B> void register(Class<?> msgClass, Meta<O, B> meta,
            Supplier<B> factory, Function<B, ?> finish) {
        if (msgClass == null || meta == null || factory == null || finish == null) {
            throw new IllegalArgumentException("null codec registration");
        }
        REGISTRY.put(msgClass, new Entry(meta, factory, finish));
    }

    public static <O, B> void registerInterface(Class<O> iface, Meta<O, B> meta,
            Supplier<B> factory, Function<B, ?> finish) {
        if (iface == null || meta == null || factory == null || finish == null) {
            throw new IllegalArgumentException("null codec registration");
        }
        IFACES.put(iface, new Entry(meta, factory, finish));
    }

    /** Encodes any registered message through its table. */
    @SuppressWarnings({"unchecked", "rawtypes"})
    public static <M> byte[] encode(M msg) {
        return encode(msg, (Meta) lookup(msg.getClass()).meta);
    }

    /** Parses a registered message class. */
    @SuppressWarnings({"unchecked", "rawtypes"})
    public static <M> M parseFrom(byte[] data, Class<M> msgClass) {
        Entry e = lookup(msgClass);
        return (M) parseFrom(data, (Supplier) e.factory, (Meta) e.meta, (Function) e.finish);
    }

    /** The table behind a registered message class or interface. */
    static Meta<?, ?> metaOf(Class<?> c) {
        return lookup(c).meta;
    }

    private static Entry lookup(Class<?> c) {
        Entry e = REGISTRY.get(c);
        if (e != null) {
            return e;
        }
        e = findInterface(c);
        if (e != null) {
            return e;
        }
        // Convention: the generated table lives in <Message>Meta next to the
        // message, so first use loads and registers it with no init code in
        // the message class itself.
        if (!c.isInterface()) {
            try {
                Class.forName(c.getName() + "Meta", true, c.getClassLoader());
            } catch (ClassNotFoundException ex) {
                throw new IllegalArgumentException("no codec registered for " + c.getName());
            }
            e = REGISTRY.get(c);
            if (e == null) {
                e = findInterface(c);
            }
        }
        if (e == null) {
            throw new IllegalArgumentException("no codec registered for " + c.getName());
        }
        return e;
    }

    // Walks the superclass/interface lattice for a registered OrBuilder.
    // No caching: the walk is pointer-chasing cheap, and caching would go
    // stale when a custom type replaces an interface registration.
    private static Entry findInterface(Class<?> c) {
        if (c == null || c == Object.class) {
            return null;
        }
        if (c.isInterface()) {
            return IFACES.get(c);
        }
        ArrayDeque<Class<?>> q = new ArrayDeque<>();
        HashSet<Class<?>> seen = new HashSet<>();
        q.add(c);
        while (!q.isEmpty()) {
            Class<?> t = q.remove();
            if (!seen.add(t)) {
                continue;
            }
            Entry e = IFACES.get(t);
            if (e != null) {
                return e;
            }
            Class<?> s = t.getSuperclass();
            if (s != null) {
                q.add(s);
            }
            for (Class<?> i : t.getInterfaces()) {
                q.add(i);
            }
        }
        return null;
    }

    public static <O, B> byte[] encode(O msg, Meta<O, B> meta) {
        Wire.Buf b = new Wire.Buf();
        encodeInto(msg, meta, b, 0);
        byte[] out = new byte[b.pos];
        System.arraycopy(b.b, 0, out, 0, b.pos);
        return out;
    }

    @SuppressWarnings("unchecked")
    public static <O, B> void encodeInto(O msg, Meta<O, B> meta, Wire.Buf b, int depth) {
        if (depth > Wire.MAX_DEPTH) {
            throw new IllegalArgumentException("nesting exceeds " + Wire.MAX_DEPTH);
        }
        for (FieldMeta<O, B> f : meta.fields) {
            if (f.oneof >= 0 && ((ToIntFunction<O>) f.caseOf).applyAsInt(msg) != f.number) {
                continue;
            }
            if (f.repeated) {
                encodeRepeated(msg, f, b, depth);
                continue;
            }
            if (f.explicit && !((Predicate<O>) f.hasM).test(msg)) {
                continue;
            }
            switch (f.kind) {
                case Kind.INT32:
                case Kind.ENUM: {
                    int v = ((ToIntFunction<O>) f.get).applyAsInt(msg);
                    if (v == 0 && !f.explicit) {
                        break;
                    }
                    if (v >= 0) {
                        Wire.wTagVar32(b, f.tag, v);
                    } else {
                        Wire.wTagVar64(b, f.tag, v);
                    }
                    break;
                }
                case Kind.UINT32: {
                    int v = ((ToIntFunction<O>) f.get).applyAsInt(msg);
                    if (v == 0 && !f.explicit) {
                        break;
                    }
                    Wire.wTagVar32(b, f.tag, v);
                    break;
                }
                case Kind.SINT32: {
                    int v = ((ToIntFunction<O>) f.get).applyAsInt(msg);
                    if (v == 0 && !f.explicit) {
                        break;
                    }
                    Wire.wTagVar32(b, f.tag, Wire.zig32(v));
                    break;
                }
                case Kind.INT64:
                case Kind.UINT64: {
                    long v = ((ToLongFunction<O>) f.get).applyAsLong(msg);
                    if (v == 0 && !f.explicit) {
                        break;
                    }
                    Wire.wTagVar64(b, f.tag, v);
                    break;
                }
                case Kind.SINT64: {
                    long v = ((ToLongFunction<O>) f.get).applyAsLong(msg);
                    if (v == 0 && !f.explicit) {
                        break;
                    }
                    Wire.wTagVar64(b, f.tag, Wire.zig64(v));
                    break;
                }
                case Kind.BOOL: {
                    boolean v = ((Predicate<O>) f.get).test(msg);
                    if (!v && !f.explicit) {
                        break;
                    }
                    Wire.wTagVar32(b, f.tag, v ? 1 : 0);
                    break;
                }
                case Kind.FIXED32:
                case Kind.SFIXED32: {
                    int v = ((ToIntFunction<O>) f.get).applyAsInt(msg);
                    if (v == 0 && !f.explicit) {
                        break;
                    }
                    Wire.wTagFixed32(b, f.tag, v);
                    break;
                }
                case Kind.FLOAT: {
                    float v = ((Function<O, Float>) f.get).apply(msg);
                    if (v == 0f && !f.explicit) {
                        break;
                    }
                    Wire.wTagFixed32(b, f.tag, Float.floatToRawIntBits(v));
                    break;
                }
                case Kind.FIXED64:
                case Kind.SFIXED64: {
                    long v = ((ToLongFunction<O>) f.get).applyAsLong(msg);
                    if (v == 0 && !f.explicit) {
                        break;
                    }
                    Wire.wTagFixed64(b, f.tag, v);
                    break;
                }
                case Kind.DOUBLE: {
                    double v = ((ToDoubleFunction<O>) f.get).applyAsDouble(msg);
                    if (v == 0.0 && !f.explicit) {
                        break;
                    }
                    Wire.wTagFixed64(b, f.tag, Double.doubleToRawLongBits(v));
                    break;
                }
                case Kind.STRING: {
                    String s = ((Function<O, String>) f.get).apply(msg);
                    if (s.isEmpty() && !f.explicit) {
                        break;
                    }
                    Wire.wTag(b, f.tag);
                    Wire.wBytes(b, s.getBytes(StandardCharsets.UTF_8));
                    break;
                }
                case Kind.BYTES: {
                    byte[] v = ((Function<O, byte[]>) f.get).apply(msg);
                    if (v.length == 0 && !f.explicit) {
                        break;
                    }
                    Wire.wTag(b, f.tag);
                    Wire.wBytes(b, v);
                    break;
                }
                case Kind.MESSAGE: {
                    Object s = ((Function<O, Object>) f.get).apply(msg);
                    if (s == null) {
                        break;
                    }
                    Wire.wTag(b, f.tag);
                    int lp = b.pos;
                    Wire.ensure(b, 1);
                    b.b[b.pos++] = 0;
                    encodeInto(s, (Meta<Object, Object>) f.sub, b, depth + 1);
                    int len = b.pos - lp - 1;
                    if (len < 128) {
                        b.b[lp] = (byte) len;
                    } else {
                        Wire.patch(b, lp);
                    }
                    break;
                }
                case Kind.GROUP: {
                    Object s = ((Function<O, Object>) f.get).apply(msg);
                    if (s == null) {
                        break;
                    }
                    Wire.wTag(b, f.tag);
                    encodeInto(s, (Meta<Object, Object>) f.sub, b, depth + 1);
                    Wire.wTag(b, f.etag);
                    break;
                }
                default:
                    throw new IllegalArgumentException("no encoder for kind " + f.kind);
            }
        }
    }

    /** One repeated row: packed runs share one length prefix, unpacked
     *  elements repeat the tag, messages recurse per element. */
    @SuppressWarnings("unchecked")
    private static <O, B> void encodeRepeated(O msg, FieldMeta<O, B> f, Wire.Buf b, int depth) {
        switch (f.kind) {
            case Kind.INT32:
            case Kind.UINT32:
            case Kind.SINT32:
            case Kind.ENUM:
            case Kind.FIXED32:
            case Kind.SFIXED32: {
                List<Integer> vs = (List<Integer>) ((Function<O, ?>) f.get).apply(msg);
                if (vs.isEmpty()) {
                    return;
                }
                if (f.packed) {
                    int n = 0;
                    for (int v : vs) {
                        n += packedSize(f.kind, v);
                    }
                    Wire.wTag(b, f.ptag);
                    Wire.wVar32(b, n);
                    for (int v : vs) {
                        writePackedVar(b, f.kind, v);
                    }
                    return;
                }
                for (int v : vs) {
                    writeSingleVar(b, f, v);
                }
                return;
            }
            case Kind.INT64:
            case Kind.UINT64:
            case Kind.SINT64:
            case Kind.FIXED64:
            case Kind.SFIXED64: {
                List<Long> vs = (List<Long>) ((Function<O, ?>) f.get).apply(msg);
                if (vs.isEmpty()) {
                    return;
                }
                if (f.packed) {
                    int n = 0;
                    for (long v : vs) {
                        n += packedSizeLong(f.kind, v);
                    }
                    Wire.wTag(b, f.ptag);
                    Wire.wVar32(b, n);
                    for (long v : vs) {
                        writePackedVarLong(b, f.kind, v);
                    }
                    return;
                }
                for (long v : vs) {
                    writeSingleVarLong(b, f, v);
                }
                return;
            }
            case Kind.FLOAT: {
                List<Float> vs = (List<Float>) ((Function<O, ?>) f.get).apply(msg);
                if (vs.isEmpty()) {
                    return;
                }
                if (f.packed) {
                    Wire.wTag(b, f.ptag);
                    Wire.wVar32(b, vs.size() * 4);
                    Wire.ensure(b, vs.size() * 4);
                    for (float v : vs) {
                        int x = Float.floatToRawIntBits(v);
                        b.b[b.pos++] = (byte) x;
                        b.b[b.pos++] = (byte) (x >> 8);
                        b.b[b.pos++] = (byte) (x >> 16);
                        b.b[b.pos++] = (byte) (x >> 24);
                    }
                    return;
                }
                for (float v : vs) {
                    Wire.wTagFixed32(b, f.tag, Float.floatToRawIntBits(v));
                }
                return;
            }
            case Kind.DOUBLE: {
                List<Double> vs = (List<Double>) ((Function<O, ?>) f.get).apply(msg);
                if (vs.isEmpty()) {
                    return;
                }
                if (f.packed) {
                    Wire.wTag(b, f.ptag);
                    Wire.wVar32(b, vs.size() * 8);
                    Wire.ensure(b, vs.size() * 8);
                    for (double v : vs) {
                        long x = Double.doubleToRawLongBits(v);
                        b.b[b.pos++] = (byte) x;
                        b.b[b.pos++] = (byte) (x >> 8);
                        b.b[b.pos++] = (byte) (x >> 16);
                        b.b[b.pos++] = (byte) (x >> 24);
                        b.b[b.pos++] = (byte) (x >> 32);
                        b.b[b.pos++] = (byte) (x >> 40);
                        b.b[b.pos++] = (byte) (x >> 48);
                        b.b[b.pos++] = (byte) (x >> 56);
                    }
                    return;
                }
                for (double v : vs) {
                    Wire.wTagFixed64(b, f.tag, Double.doubleToRawLongBits(v));
                }
                return;
            }
            case Kind.BOOL: {
                List<Boolean> vs = (List<Boolean>) ((Function<O, ?>) f.get).apply(msg);
                if (vs.isEmpty()) {
                    return;
                }
                if (f.packed) {
                    Wire.wTag(b, f.ptag);
                    Wire.wVar32(b, vs.size());
                    Wire.ensure(b, vs.size());
                    for (boolean v : vs) {
                        b.b[b.pos++] = (byte) (v ? 1 : 0);
                    }
                    return;
                }
                for (boolean v : vs) {
                    Wire.wTagVar32(b, f.tag, v ? 1 : 0);
                }
                return;
            }
            case Kind.STRING: {
                List<String> vs = (List<String>) ((Function<O, ?>) f.get).apply(msg);
                for (String v : vs) {
                    Wire.wTag(b, f.tag);
                    Wire.wBytes(b, v.getBytes(StandardCharsets.UTF_8));
                }
                return;
            }
            case Kind.BYTES: {
                List<byte[]> vs = (List<byte[]>) ((Function<O, ?>) f.get).apply(msg);
                for (byte[] v : vs) {
                    Wire.wTag(b, f.tag);
                    Wire.wBytes(b, v);
                }
                return;
            }
            case Kind.MESSAGE: {
                List<Object> vs = (List<Object>) ((Function<O, ?>) f.get).apply(msg);
                for (Object v : vs) {
                    Wire.wTag(b, f.tag);
                    int lp = b.pos;
                    Wire.ensure(b, 1);
                    b.b[b.pos++] = 0;
                    encodeInto(v, (Meta<Object, Object>) f.sub, b, depth + 1);
                    int len = b.pos - lp - 1;
                    if (len < 128) {
                        b.b[lp] = (byte) len;
                    } else {
                        Wire.patch(b, lp);
                    }
                }
                return;
            }
            case Kind.GROUP: {
                List<Object> vs = (List<Object>) ((Function<O, ?>) f.get).apply(msg);
                for (Object v : vs) {
                    Wire.wTag(b, f.tag);
                    encodeInto(v, (Meta<Object, Object>) f.sub, b, depth + 1);
                    Wire.wTag(b, f.etag);
                }
                return;
            }
            default:
                throw new IllegalArgumentException("no encoder for kind " + f.kind);
        }
    }

    private static int packedSize(int kind, int v) {
        switch (kind) {
            case Kind.INT32:
            case Kind.ENUM:
                return Wire.sizeInt32(v);
            case Kind.UINT32:
                return Wire.sizeVar32(v);
            case Kind.SINT32:
                return Wire.sizeVar32(Wire.zig32(v));
            default:
                return 4; // fixed32/sfixed32
        }
    }

    private static int packedSizeLong(int kind, long v) {
        switch (kind) {
            case Kind.INT64:
            case Kind.UINT64:
                return Wire.sizeVar64(v);
            case Kind.SINT64:
                return Wire.sizeVar64(Wire.zig64(v));
            default:
                return 8; // fixed64/sfixed64
        }
    }

    private static void writePackedVar(Wire.Buf b, int kind, int v) {
        switch (kind) {
            case Kind.INT32:
            case Kind.ENUM:
                Wire.wInt32(b, v);
                return;
            case Kind.UINT32:
                Wire.wVar32(b, v);
                return;
            case Kind.SINT32:
                Wire.wVar32(b, Wire.zig32(v));
                return;
            default:
                Wire.wFixed32(b, v);
                return;
        }
    }

    private static void writePackedVarLong(Wire.Buf b, int kind, long v) {
        switch (kind) {
            case Kind.INT64:
            case Kind.UINT64:
                Wire.wVar64(b, v);
                return;
            case Kind.SINT64:
                Wire.wVar64(b, Wire.zig64(v));
                return;
            default:
                Wire.wFixed64(b, v);
                return;
        }
    }

    private static <O, B> void writeSingleVar(Wire.Buf b, FieldMeta<O, B> f, int v) {
        switch (f.kind) {
            case Kind.INT32:
            case Kind.ENUM:
                if (v >= 0) {
                    Wire.wTagVar32(b, f.tag, v);
                } else {
                    Wire.wTagVar64(b, f.tag, v);
                }
                return;
            case Kind.UINT32:
                Wire.wTagVar32(b, f.tag, v);
                return;
            case Kind.SINT32:
                Wire.wTagVar32(b, f.tag, Wire.zig32(v));
                return;
            default:
                Wire.wTagFixed32(b, f.tag, v);
                return;
        }
    }

    private static <O, B> void writeSingleVarLong(Wire.Buf b, FieldMeta<O, B> f, long v) {
        switch (f.kind) {
            case Kind.INT64:
            case Kind.UINT64:
                Wire.wTagVar64(b, f.tag, v);
                return;
            case Kind.SINT64:
                Wire.wTagVar64(b, f.tag, Wire.zig64(v));
                return;
            default:
                Wire.wTagFixed64(b, f.tag, v);
                return;
        }
    }

    public static <O, B, M> M parseFrom(byte[] data, Supplier<B> factory, Meta<O, B> meta,
            Function<B, M> finish) {
        B b = factory.get();
        Wire.Reader r = new Wire.Reader();
        r.b = data;
        r.pos = 0;
        r.end = data.length;
        parseInto(b, meta, r, data.length, -1, 0);
        if (r.pos != data.length) {
            throw new IllegalArgumentException("trailing bytes");
        }
        return finish.apply(b);
    }

    @SuppressWarnings({"unchecked", "rawtypes"})
    static <O, B> void parseInto(B b, Meta<O, B> meta, Wire.Reader r, int end,
            int groupEnd, int depth) {
        if (depth > Wire.MAX_DEPTH) {
            throw new IllegalArgumentException("nesting exceeds " + Wire.MAX_DEPTH);
        }
        // Singular message/group builders pending assignment: repeat
        // occurrences merge into the same builder, like the reference.
        Map<Integer, Object> pend = null;
        int outerEnd = r.end;
        r.end = end;
        boolean closed = groupEnd < 0;
        while (r.pos < r.end) {
            int tag = Wire.rTag(r);
            int fnum = tag >>> 3;
            int wt = tag & 7;
            if (fnum == 0 || wt > Wire.WT_FIXED32) {
                throw new IllegalArgumentException("bad tag " + tag);
            }
            if (wt == 4) {
                if (groupEnd < 0 || fnum != groupEnd) {
                    throw new IllegalArgumentException("stray end-group for " + fnum);
                }
                closed = true;
                break;
            }
            int slot = meta.slot(fnum);
            if (slot < 0) {
                Wire.skipUnknown(r, fnum, wt, depth);
                continue;
            }
            FieldMeta<O, B> f = meta.fields[slot];
            switch (f.kind) {
                case Kind.INT32:
                case Kind.UINT32:
                case Kind.SINT32:
                case Kind.ENUM:
                case Kind.FIXED32:
                case Kind.SFIXED32: {
                    if (f.repeated) {
                        if (wt == 2 && f.packed) {
                            int len = Wire.rLen32(r);
                            int stop = r.pos + len;
                            ObjIntConsumer<B> add = (ObjIntConsumer<B>) f.set;
                            while (r.pos < stop) {
                                add.accept(b, readInt(r, f.kind));
                            }
                            if (r.pos != stop) {
                                throw new IllegalArgumentException("packed overrun");
                            }
                        } else if (wt == singularWt(f)) {
                            ((ObjIntConsumer<B>) f.set).accept(b, readInt(r, f.kind));
                        } else {
                            Wire.skipUnknown(r, fnum, wt, depth);
                        }
                        break;
                    }
                    if (wt != singularWt(f)) {
                        Wire.skipUnknown(r, fnum, wt, depth);
                        break;
                    }
                    ((ObjIntConsumer<B>) f.set).accept(b, readInt(r, f.kind));
                    break;
                }
                case Kind.INT64:
                case Kind.UINT64:
                case Kind.SINT64:
                case Kind.FIXED64:
                case Kind.SFIXED64: {
                    if (f.repeated) {
                        if (wt == 2 && f.packed) {
                            int len = Wire.rLen32(r);
                            int stop = r.pos + len;
                            ObjLongConsumer<B> add = (ObjLongConsumer<B>) f.set;
                            while (r.pos < stop) {
                                add.accept(b, readLong(r, f.kind));
                            }
                            if (r.pos != stop) {
                                throw new IllegalArgumentException("packed overrun");
                            }
                        } else if (wt == singularWt(f)) {
                            ((ObjLongConsumer<B>) f.set).accept(b, readLong(r, f.kind));
                        } else {
                            Wire.skipUnknown(r, fnum, wt, depth);
                        }
                        break;
                    }
                    if (wt != singularWt(f)) {
                        Wire.skipUnknown(r, fnum, wt, depth);
                        break;
                    }
                    ((ObjLongConsumer<B>) f.set).accept(b, readLong(r, f.kind));
                    break;
                }
                case Kind.FLOAT: {
                    if (f.repeated) {
                        if (wt == 2 && f.packed) {
                            int len = Wire.rLen32(r);
                            int stop = r.pos + len;
                            BiConsumer<B, Float> add = (BiConsumer<B, Float>) f.set;
                            while (r.pos < stop) {
                                add.accept(b, Float.intBitsToFloat(Wire.rFixed32(r)));
                            }
                            if (r.pos != stop) {
                                throw new IllegalArgumentException("packed overrun");
                            }
                        } else if (wt == 5) {
                            ((BiConsumer<B, Float>) f.set).accept(b, Float.intBitsToFloat(Wire.rFixed32(r)));
                        } else {
                            Wire.skipUnknown(r, fnum, wt, depth);
                        }
                        break;
                    }
                    if (wt != 5) {
                        Wire.skipUnknown(r, fnum, wt, depth);
                        break;
                    }
                    ((BiConsumer<B, Float>) f.set).accept(b, Float.intBitsToFloat(Wire.rFixed32(r)));
                    break;
                }
                case Kind.DOUBLE: {
                    if (f.repeated) {
                        if (wt == 2 && f.packed) {
                            int len = Wire.rLen32(r);
                            int stop = r.pos + len;
                            ObjDoubleConsumer<B> add = (ObjDoubleConsumer<B>) f.set;
                            while (r.pos < stop) {
                                add.accept(b, Double.longBitsToDouble(Wire.rFixed64(r)));
                            }
                            if (r.pos != stop) {
                                throw new IllegalArgumentException("packed overrun");
                            }
                        } else if (wt == 1) {
                            ((ObjDoubleConsumer<B>) f.set).accept(b,
                                    Double.longBitsToDouble(Wire.rFixed64(r)));
                        } else {
                            Wire.skipUnknown(r, fnum, wt, depth);
                        }
                        break;
                    }
                    if (wt != 1) {
                        Wire.skipUnknown(r, fnum, wt, depth);
                        break;
                    }
                    ((ObjDoubleConsumer<B>) f.set).accept(b,
                            Double.longBitsToDouble(Wire.rFixed64(r)));
                    break;
                }
                case Kind.BOOL: {
                    if (f.repeated) {
                        if (wt == 2 && f.packed) {
                            int len = Wire.rLen32(r);
                            int stop = r.pos + len;
                            BiConsumer<B, Boolean> add = (BiConsumer<B, Boolean>) f.set;
                            while (r.pos < stop) {
                                add.accept(b, Wire.rVar(r) != 0);
                            }
                            if (r.pos != stop) {
                                throw new IllegalArgumentException("packed overrun");
                            }
                        } else if (wt == 0) {
                            ((BiConsumer<B, Boolean>) f.set).accept(b, Wire.rVar(r) != 0);
                        } else {
                            Wire.skipUnknown(r, fnum, wt, depth);
                        }
                        break;
                    }
                    if (wt != 0) {
                        Wire.skipUnknown(r, fnum, wt, depth);
                        break;
                    }
                    ((BiConsumer<B, Boolean>) f.set).accept(b, Wire.rVar(r) != 0);
                    break;
                }
                case Kind.STRING: {
                    if (wt != 2) {
                        Wire.skipUnknown(r, fnum, wt, depth);
                        break;
                    }
                    int n = Wire.rLen32(r);
                    String str = new String(r.b, r.pos, n, StandardCharsets.UTF_8);
                    r.pos += n;
                    ((BiConsumer<B, String>) f.set).accept(b, str);
                    break;
                }
                case Kind.BYTES: {
                    if (wt != 2) {
                        Wire.skipUnknown(r, fnum, wt, depth);
                        break;
                    }
                    int n = Wire.rLen32(r);
                    byte[] v = Arrays.copyOfRange(r.b, r.pos, r.pos + n);
                    r.pos += n;
                    ((BiConsumer<B, byte[]>) f.set).accept(b, v);
                    break;
                }
                case Kind.MESSAGE:
                case Kind.GROUP: {
                    boolean isGroup = f.kind == Kind.GROUP;
                    if (wt != (isGroup ? 3 : 2)) {
                        Wire.skipUnknown(r, fnum, wt, depth);
                        break;
                    }
                    if (f.repeated) {
                        Object sub = f.factory.get();
                        if (isGroup) {
                            parseInto(sub, (Meta<Object, Object>) f.sub, r, r.end, f.number,
                                    depth + 1);
                        } else {
                            int len = Wire.rLen32(r);
                            parseInto(sub, (Meta<Object, Object>) f.sub, r, r.pos + len, -1,
                                    depth + 1);
                        }
                        Object built = ((Function<Object, Object>) f.finish).apply(sub);
                        ((BiConsumer<B, Object>) f.set).accept(b, built);
                        break;
                    }
                    if (pend == null) {
                        pend = new HashMap<>();
                    }
                    Object sub = pend.get(slot);
                    if (sub == null) {
                        sub = f.factory.get();
                        pend.put(slot, sub);
                    }
                    if (isGroup) {
                        parseInto(sub, (Meta<Object, Object>) f.sub, r, r.end, f.number,
                                depth + 1);
                    } else {
                        int len = Wire.rLen32(r);
                        parseInto(sub, (Meta<Object, Object>) f.sub, r, r.pos + len, -1,
                                depth + 1);
                    }
                    break;
                }
                default:
                    Wire.skipUnknown(r, fnum, wt, depth);
                    break;
            }
        }
        r.end = outerEnd;
        if (!closed) {
            throw new IllegalArgumentException("missing end-group for " + groupEnd);
        }
        if (pend != null) {
            for (Map.Entry<Integer, Object> e : pend.entrySet()) {
                FieldMeta<O, B> f = meta.fields[e.getKey()];
                Object built = ((Function<Object, Object>) f.finish).apply(e.getValue());
                ((BiConsumer<B, Object>) f.set).accept(b, built);
            }
        }
        if (meta.hasRequired) {
            for (FieldMeta<O, B> f : meta.fields) {
                if (f.required && !((Predicate<B>) f.hasB).test(b)) {
                    throw new IllegalStateException(meta.fullName + ": missing required fields");
                }
            }
        }
    }

    private static int singularWt(FieldMeta<?, ?> f) {
        switch (f.kind) {
            case Kind.INT32:
            case Kind.UINT32:
            case Kind.SINT32:
            case Kind.INT64:
            case Kind.UINT64:
            case Kind.SINT64:
            case Kind.BOOL:
            case Kind.ENUM:
                return 0;
            case Kind.FIXED64:
            case Kind.SFIXED64:
            case Kind.DOUBLE:
                return 1;
            case Kind.FIXED32:
            case Kind.SFIXED32:
            case Kind.FLOAT:
                return 5;
            default:
                return 2;
        }
    }

    private static int readInt(Wire.Reader r, int kind) {
        switch (kind) {
            case Kind.SINT32:
                return Wire.unzig32(Wire.rVar(r) & 0xFFFFFFFFL);
            case Kind.FIXED32:
            case Kind.SFIXED32:
                return Wire.rFixed32(r);
            default:
                return (int) Wire.rVar(r);
        }
    }

    private static long readLong(Wire.Reader r, int kind) {
        switch (kind) {
            case Kind.SINT64:
                return Wire.unzig64(Wire.rVar(r));
            case Kind.FIXED64:
            case Kind.SFIXED64:
                return Wire.rFixed64(r);
            default:
                return Wire.rVar(r);
        }
    }
}

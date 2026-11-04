package mvp;

import com.google.protobuf.DescriptorProtos;
import com.google.protobuf.Descriptors;

import static mvp.Wire.*;

import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/*
 * Generic pure-Java protobuf codec, v3 (columnar + lazy).
 *
 * v2 removed boxing (numerics in long[]) but still parsed every nested
 * message and allocated every string on decode. v3 decodes MESSAGE/GROUP/
 * STRING/BYTES fields lazily: decode records a Slice (input ref + bounds)
 * per field and only scans tags, so sub-trees and strings cost nothing
 * until first access. Values materialize on get()/getList(); untouched
 * slices re-encode with a verbatim copy, which is also what keeps
 * byte-exactness (and invalid UTF-8, unknown enums, groups) for free.
 *
 * Numbers stay eager (cheap, and presence checks need them). The timed
 * decode path therefore allocates ~one Slice per lazy field plus the
 * numeric arrays, instead of a full object tree.
 *
 * shortcut: getList() boxes into a fresh ArrayList (read path, not timed);
 * lazy slices retain the input buffer while the message lives; a lazily
 * nested message restarts the MAX_DEPTH budget when materialized; JSON/
 * text mappings still binary-only.
 */
public final class GenericCodec {
    private GenericCodec() {
    }

    /** Presence kinds. IMPLICIT = proto3 plain scalar (no has-bit). */
    public static final int IMPLICIT = 0, EXPLICIT = 1, REQUIRED = 2, MESSAGE = 3;

    /* Storage kinds: one dispatch per field, tight loops per element. */
    private static final int K_INT32 = 0, K_UINT32 = 1, K_SINT32 = 2,
            K_INT64 = 3, K_UINT64 = 4, K_SINT64 = 5, K_BOOL = 6, K_ENUM = 7,
            K_FIXED32 = 8, K_SFIXED32 = 9, K_FLOAT = 10, K_FIXED64 = 11,
            K_SFIXED64 = 12, K_DOUBLE = 13, K_STRING = 14, K_BYTES = 15,
            K_MESSAGE = 16, K_GROUP = 17;

    private static final int MAX_DEPTH = 100;

    public static final class Field {
        public int number;
        public Descriptors.FieldDescriptor.Type type;
        public int kind;
        public int wt; // wire type for the singular/packed form
        public boolean repeated;
        public boolean packed;
        public int oneof = -1; // oneof index, -1 = none
        public int presence = IMPLICIT;
        public MessageDef msgType; // MESSAGE/GROUP element type
        public byte[] tag; // precomputed tag bytes (singular/packed form)
        public Descriptors.FieldDescriptor descriptor;
    }

    public static final class MessageDef {
        public Descriptors.Descriptor descriptor;
        public Field[] fields; // sorted by number
        public int[] slotByFnum; // dense
        public Map<String, Integer> byName = new HashMap<>();
        public Map<String, Integer> oneofByName = new HashMap<>();
        public int oneofCount;
    }

    public static final class Schema {
        public final Map<String, MessageDef> byName = new HashMap<>();

        public MessageDef message(String fullName) {
            MessageDef d = byName.get(fullName);
            if (d == null) {
                throw new IllegalArgumentException("no such message: " + fullName);
            }
            return d;
        }

        /** Loads every message (+ extensions) in the set, dependencies first. */
        public static Schema load(DescriptorProtos.FileDescriptorSet set) {
            List<Descriptors.FileDescriptor> built = new ArrayList<>();
            Map<String, Descriptors.FileDescriptor> byFile = new HashMap<>();
            for (int i = 0; i < set.getFileCount(); i++) {
                DescriptorProtos.FileDescriptorProto f = set.getFile(i);
                Descriptors.FileDescriptor[] deps =
                        new Descriptors.FileDescriptor[f.getDependencyCount()];
                for (int j = 0; j < deps.length; j++) {
                    deps[j] = byFile.get(f.getDependency(j));
                    if (deps[j] == null) {
                        throw new IllegalArgumentException("deps out of order: "
                                + f.getName() + " needs " + f.getDependency(j));
                    }
                }
                try {
                    Descriptors.FileDescriptor fd =
                            Descriptors.FileDescriptor.buildFrom(f, deps);
                    built.add(fd);
                    byFile.put(f.getName(), fd);
                } catch (Descriptors.DescriptorValidationException e) {
                    throw new IllegalArgumentException(e);
                }
            }
            Schema s = new Schema();
            for (Descriptors.FileDescriptor fd : built) {
                for (Descriptors.Descriptor m : fd.getMessageTypes()) {
                    s.buildDef(m);
                }
            }
            // Extensions attach to their extendee after all defs exist.
            for (Descriptors.FileDescriptor fd : built) {
                for (Descriptors.FieldDescriptor e : fd.getExtensions()) {
                    MessageDef d = s.byName.get(e.getContainingType().getFullName());
                    if (d == null) {
                        throw new IllegalArgumentException("extendee missing: "
                                + e.getContainingType().getFullName());
                    }
                    s.attachExtension(d, e);
                }
            }
            return s;
        }

        private MessageDef buildDef(Descriptors.Descriptor m) {
            MessageDef done = byName.get(m.getFullName());
            if (done != null) {
                return done;
            }
            MessageDef d = new MessageDef();
            d.descriptor = m;
            byName.put(m.getFullName(), d); // before fields: recursion-safe
            List<Field> fs = new ArrayList<>();
            Map<Descriptors.OneofDescriptor, Integer> oneofs = new HashMap<>();
            for (Descriptors.FieldDescriptor f : m.getFields()) {
                if (f.isExtension()) {
                    continue; // attached in the extension pass
                }
                fs.add(buildField(f, oneofs));
            }
            finishDef(d, fs, oneofs);
            for (Descriptors.Descriptor n : m.getNestedTypes()) {
                buildDef(n);
            }
            return d;
        }

        private Field buildField(Descriptors.FieldDescriptor f,
                Map<Descriptors.OneofDescriptor, Integer> oneofs) {
            Field s = new Field();
            s.number = f.getNumber();
            s.type = f.getType();
            s.kind = kindOf(f.getType());
            s.repeated = f.isRepeated();
            s.packed = f.isPacked();
            s.descriptor = f;
            if (f.getContainingOneof() != null) {
                Integer idx = oneofs.get(f.getContainingOneof());
                if (idx == null) {
                    idx = oneofs.size();
                    oneofs.put(f.getContainingOneof(), idx);
                }
                s.oneof = idx;
                s.presence = EXPLICIT; // reads/writes go through the oneof case
            } else if (f.isRequired()) {
                s.presence = REQUIRED;
            } else if (s.type == Descriptors.FieldDescriptor.Type.MESSAGE
                    || s.type == Descriptors.FieldDescriptor.Type.GROUP) {
                s.presence = MESSAGE;
            } else if (f.hasPresence()) {
                s.presence = EXPLICIT;
            } else {
                s.presence = IMPLICIT;
            }
            if (s.type == Descriptors.FieldDescriptor.Type.MESSAGE
                    || s.type == Descriptors.FieldDescriptor.Type.GROUP) {
                s.msgType = buildDef(f.getMessageType());
            }
            s.wt = wireType(s.type, s.repeated && s.packed);
            s.tag = Wire.tagBytes(s.number, s.wt);
            return s;
        }

        private void attachExtension(MessageDef d, Descriptors.FieldDescriptor e) {
            Field s = buildField(e, new HashMap<>());
            List<Field> fs = new ArrayList<>(Arrays.asList(d.fields));
            fs.add(s);
            Map<String, Integer> names = new HashMap<>(d.oneofByName);
            int count = d.oneofCount;
            finishDef(d, fs, new HashMap<>());
            d.oneofByName.putAll(names); // extensions are never in a oneof
            d.oneofCount = count;
        }

        private void finishDef(MessageDef d, List<Field> fs,
                Map<Descriptors.OneofDescriptor, Integer> oneofs) {
            fs.sort((a, b) -> Integer.compare(a.number, b.number));
            int max = 0;
            for (Field f : fs) {
                max = Math.max(max, f.number);
            }
            d.fields = fs.toArray(new Field[0]);
            d.slotByFnum = new int[max + 1];
            Arrays.fill(d.slotByFnum, -1);
            d.byName.clear();
            for (int i = 0; i < d.fields.length; i++) {
                d.slotByFnum[d.fields[i].number] = i;
                d.byName.put(d.fields[i].descriptor.getName(), i);
            }
            d.oneofByName.clear();
            for (Map.Entry<Descriptors.OneofDescriptor, Integer> oe : oneofs.entrySet()) {
                d.oneofByName.put(oe.getKey().getName(), oe.getValue());
            }
            d.oneofCount = Math.max(d.oneofCount, oneofs.size());
        }
    }

    private static int kindOf(Descriptors.FieldDescriptor.Type t) {
        switch (t) {
            case INT32:
                return K_INT32;
            case UINT32:
                return K_UINT32;
            case SINT32:
                return K_SINT32;
            case INT64:
                return K_INT64;
            case UINT64:
                return K_UINT64;
            case SINT64:
                return K_SINT64;
            case BOOL:
                return K_BOOL;
            case ENUM:
                return K_ENUM;
            case FIXED32:
                return K_FIXED32;
            case SFIXED32:
                return K_SFIXED32;
            case FLOAT:
                return K_FLOAT;
            case FIXED64:
                return K_FIXED64;
            case SFIXED64:
                return K_SFIXED64;
            case DOUBLE:
                return K_DOUBLE;
            case STRING:
                return K_STRING;
            case BYTES:
                return K_BYTES;
            case MESSAGE:
                return K_MESSAGE;
            case GROUP:
                return K_GROUP;
            default:
                throw new IllegalArgumentException("no kind for " + t);
        }
    }

    private static boolean isPackableKind(int kind) {
        return kind <= K_DOUBLE; // every numeric kind packs
    }

    private static int wireType(Descriptors.FieldDescriptor.Type t, boolean packed) {
        if (packed) {
            return WT_LEN;
        }
        switch (t) {
            case INT32:
            case INT64:
            case UINT32:
            case UINT64:
            case SINT32:
            case SINT64:
            case BOOL:
            case ENUM:
                return WT_VARINT;
            case FIXED64:
            case SFIXED64:
            case DOUBLE:
                return WT_FIXED64;
            case STRING:
            case BYTES:
            case MESSAGE:
                return WT_LEN;
            case GROUP:
                return 3; // start-group tag; end tag derived at use
            case FIXED32:
            case SFIXED32:
            case FLOAT:
                return WT_FIXED32;
            default:
                throw new IllegalArgumentException("no wire type for " + t);
        }
    }

    /** Unparsed field bytes: input ref + bounds, tag included. A verbatim
     *  copy re-encodes them, so untouched slices stay byte-exact for free. */
    static final class Slice {
        final byte[] b;
        final int off;
        final int len;

        Slice(byte[] b, int off, int len) {
            this.b = b;
            this.off = off;
            this.len = len;
        }
    }

    /**
     * Columnar dynamic message. Numerics live eager and decoded in long[]
     * (float/double as raw bits, bool as 0/1); MESSAGE/GROUP/STRING/BYTES
     * decode lazily into Slice and materialize on first access. No boxing
     * anywhere on the timed path.
     */
    public static final class DMsg {
        public final MessageDef def;
        public final long[] num;
        public final Object[] ref; // String/byte[]/DMsg/Slice, singular
        public final boolean[] has;
        public final long[][] repNum;
        public final int[] repNumLen;
        public final Object[][] repRef; // String/byte[]/DMsg/Slice elements
        public final int[] repRefLen;
        public final int[] oneofCase; // slot+1 per oneof, 0 = none set
        public List<byte[]> unknown; // lazy: null until the first one

        public DMsg(MessageDef def) {
            this.def = def;
            int n = def.fields.length;
            this.num = new long[n];
            this.ref = new Object[n];
            this.has = new boolean[n];
            this.repNum = new long[n][];
            this.repNumLen = new int[n];
            this.repRef = new Object[n][];
            this.repRefLen = new int[n];
            this.oneofCase = new int[def.oneofCount];
        }

        private int slot(int fnum) {
            if (fnum < 0 || fnum >= def.slotByFnum.length
                    || def.slotByFnum[fnum] < 0) {
                throw new IllegalArgumentException(def.descriptor.getFullName()
                        + " has no field " + fnum);
            }
            return def.slotByFnum[fnum];
        }

        private int slotByName(String name) {
            Integer i = def.byName.get(name);
            if (i == null) {
                throw new IllegalArgumentException(def.descriptor.getFullName()
                        + " has no field " + name);
            }
            return i;
        }

        public DMsg set(int fnum, Object v) {
            int s = slot(fnum);
            Field f = def.fields[s];
            if (f.repeated) {
                throw new IllegalArgumentException("repeated field " + fnum
                        + ": use add()");
            }
            if (v == null) {
                throw new IllegalArgumentException("null value");
            }
            setRef(s, f, v); // numeric kinds store into num[], refs into ref[]
            setPresent(s, f);
            return this;
        }

        private void setRef(int s, Field f, Object v) {
            switch (f.kind) {
                case K_INT32:
                case K_SINT32:
                case K_SFIXED32:
                case K_UINT32:
                case K_ENUM:
                case K_FIXED32:
                    num[s] = ((Number) v).intValue();
                    return;
                case K_INT64:
                case K_SINT64:
                case K_UINT64:
                case K_FIXED64:
                case K_SFIXED64:
                    num[s] = ((Number) v).longValue();
                    return;
                case K_BOOL:
                    num[s] = ((Boolean) v) ? 1 : 0;
                    return;
                case K_FLOAT:
                    num[s] = Float.floatToRawIntBits(((Number) v).floatValue());
                    return;
                case K_DOUBLE:
                    num[s] = Double.doubleToRawLongBits(((Number) v).doubleValue());
                    return;
                case K_STRING:
                    ref[s] = (String) v;
                    return;
                case K_BYTES:
                    if (!(v instanceof byte[])) {
                        throw new IllegalArgumentException("field " + f.number
                                + " needs byte[]");
                    }
                    ref[s] = v;
                    return;
                case K_MESSAGE:
                case K_GROUP:
                    if (!(v instanceof DMsg)) {
                        throw new IllegalArgumentException("field " + f.number
                                + " needs DMsg");
                    }
                    ref[s] = v;
                    return;
                default:
                    throw new IllegalArgumentException("bad kind");
            }
        }

        public DMsg set(String name, Object v) {
            return set(def.fields[slotByName(name)].number, v);
        }

        private void setPresent(int s, Field f) {
            if (f.oneof >= 0) {
                int prev = oneofCase[f.oneof];
                if (prev != 0 && prev != s + 1) {
                    has[prev - 1] = false;
                    if (def.fields[prev - 1].kind >= K_STRING) {
                        ref[prev - 1] = null;
                    }
                }
                oneofCase[f.oneof] = s + 1;
                has[s] = true;
            } else {
                has[s] = true;
            }
        }

        public DMsg add(int fnum, Object v) {
            int s = slot(fnum);
            Field f = def.fields[s];
            if (!f.repeated) {
                throw new IllegalArgumentException("singular field " + fnum
                        + ": use set()");
            }
            if (v == null) {
                throw new IllegalArgumentException("null value");
            }
            if (f.kind <= K_DOUBLE) {
                long[] a = repNum[s];
                int n = repNumLen[s];
                if (a == null || n == a.length) {
                    a = growLong(a, n + 1);
                    repNum[s] = a;
                }
                a[n] = toNum(f, v);
                repNumLen[s] = n + 1;
                return this;
            }
            Object[] a = repRef[s];
            int n = repRefLen[s];
            if (a == null || n == a.length) {
                a = growObj(a, n + 1);
                repRef[s] = a;
            }
            if (f.kind >= K_MESSAGE && !(v instanceof DMsg)) {
                throw new IllegalArgumentException("field " + fnum
                        + " needs DMsg");
            }
            a[n] = v;
            repRefLen[s] = n + 1;
            return this;
        }

        public DMsg add(String name, Object v) {
            return add(def.fields[slotByName(name)].number, v);
        }

        public DMsg newMessage(int fnum) {
            int s = slot(fnum);
            Field f = def.fields[s];
            if (f.kind != K_MESSAGE && f.kind != K_GROUP) {
                throw new IllegalArgumentException("field " + fnum
                        + " is not a message");
            }
            DMsg m = new DMsg(f.msgType);
            if (f.repeated) {
                add(fnum, m);
            } else {
                set(fnum, m);
            }
            return m;
        }

        public DMsg newMessage(String name) {
            int s = slotByName(name);
            return newMessage(def.fields[s].number);
        }

        public DMsg clear(int fnum) {
            int s = slot(fnum);
            Field f = def.fields[s];
            has[s] = false;
            if (f.oneof >= 0 && oneofCase[f.oneof] == s + 1) {
                oneofCase[f.oneof] = 0;
            }
            if (f.repeated) {
                repNumLen[s] = 0;
                repRefLen[s] = 0;
            } else if (f.kind <= K_DOUBLE) {
                num[s] = 0;
            } else {
                ref[s] = null;
            }
            return this;
        }

        public DMsg clear(String name) {
            return clear(def.fields[slotByName(name)].number);
        }

        /** Materializes a lazy slice on first access. */
        public Object get(int fnum) {
            int s = slot(fnum);
            if (!has[s]) {
                return null;
            }
            Object v = ref[s];
            if (v instanceof Slice) {
                v = materialize(def.fields[s], (Slice) v);
                ref[s] = v;
            }
            if (def.fields[s].kind <= K_DOUBLE) {
                return box(def.fields[s], num[s]);
            }
            return v;
        }

        public Object get(String name) {
            return get(def.fields[slotByName(name)].number);
        }

        public boolean has(int fnum) {
            return has[slot(fnum)];
        }

        public boolean has(String name) {
            return has[slotByName(name)];
        }

        /** Boxed copy, materializing slices (read path, not the timed path). */
        public List<Object> getList(int fnum) {
            int s = slot(fnum);
            Field f = def.fields[s];
            ArrayList<Object> l = new ArrayList<>(
                    f.kind <= K_DOUBLE ? repNumLen[s] : repRefLen[s]);
            if (f.kind <= K_DOUBLE) {
                for (int i = 0; i < repNumLen[s]; i++) {
                    l.add(box(f, repNum[s][i]));
                }
            } else {
                for (int i = 0; i < repRefLen[s]; i++) {
                    Object v = repRef[s][i];
                    if (v instanceof Slice) {
                        v = materialize(f, (Slice) v);
                        repRef[s][i] = v;
                    }
                    l.add(v);
                }
            }
            return l;
        }

        /** Which member of a oneof is set: field number, or 0 for none. */
        public int oneofCase(String oneofName) {
            Integer i = def.oneofByName.get(oneofName);
            if (i == null) {
                throw new IllegalArgumentException("no such oneof: " + oneofName);
            }
            int c = oneofCase[i];
            return c == 0 ? 0 : def.fields[c - 1].number;
        }

        static Object box(Field f, long n) {
            switch (f.kind) {
                case K_INT32:
                case K_SINT32:
                case K_SFIXED32:
                case K_UINT32:
                case K_FIXED32:
                case K_ENUM:
                    return Integer.valueOf((int) n);
                case K_INT64:
                case K_SINT64:
                case K_UINT64:
                case K_FIXED64:
                case K_SFIXED64:
                    return Long.valueOf(n);
                case K_BOOL:
                    return n != 0 ? Boolean.TRUE : Boolean.FALSE;
                case K_FLOAT:
                    return Float.valueOf(Float.intBitsToFloat((int) n));
                case K_DOUBLE:
                    return Double.valueOf(Double.longBitsToDouble(n));
                default:
                    throw new IllegalArgumentException("not numeric");
            }
        }
    }

    private static long toNum(Field f, Object v) {
        switch (f.kind) {
            case K_INT32:
            case K_SINT32:
            case K_SFIXED32:
            case K_UINT32:
            case K_ENUM:
            case K_FIXED32:
                return ((Number) v).intValue();
            case K_INT64:
            case K_SINT64:
            case K_UINT64:
            case K_FIXED64:
            case K_SFIXED64:
                return ((Number) v).longValue();
            case K_BOOL:
                return ((Boolean) v) ? 1 : 0;
            case K_FLOAT:
                return Float.floatToRawIntBits(((Number) v).floatValue());
            case K_DOUBLE:
                return Double.doubleToRawLongBits(((Number) v).doubleValue());
            default:
                throw new IllegalArgumentException("not numeric: " + f.kind);
        }
    }

    private static long[] growLong(long[] a, int need) {
        if (a == null) {
            return new long[Math.max(16, need)];
        }
        if (a.length >= need) {
            return a;
        }
        int n = a.length * 2;
        while (n < need) {
            n *= 2;
        }
        return Arrays.copyOf(a, n);
    }

    private static Object[] growObj(Object[] a, int need) {
        if (a == null) {
            return new Object[Math.max(16, need)];
        }
        if (a.length >= need) {
            return a;
        }
        int n = a.length * 2;
        while (n < need) {
            n *= 2;
        }
        Object[] r = new Object[n];
        System.arraycopy(a, 0, r, 0, a.length);
        return r;
    }

    /* ---------------- encode ---------------- */

    private static final ThreadLocal<Wire.Buf> BUF =
            ThreadLocal.withInitial(Wire.Buf::new);

    /** 1-entry reference-equality UTF-8 cache: repeated constants ("tag",
     *  "grandkid") never re-encode. Falls back to one String.getBytes. */
    private static final class StrCache {
        String s;
        byte[] b;
    }

    private static final ThreadLocal<StrCache> STRCACHE =
            ThreadLocal.withInitial(StrCache::new);

    private static void writeStr(Wire.Buf b, String s, StrCache c) {
        byte[] u;
        if (s == c.s) {
            u = c.b;
        } else {
            u = s.getBytes(StandardCharsets.UTF_8);
            c.s = s;
            c.b = u;
        }
        wBytes(b, u);
    }

    /** Single pass into a reusable buffer; nested payloads use a 1-byte
     *  length placeholder patched after (Wire.patch has the overflow).
     *  Untouched lazy slices copy verbatim: tag included, no re-encode. */
    public static byte[] encode(DMsg m) {
        Wire.Buf b = BUF.get();
        b.pos = 0;
        encodeInto(m, b, 0, STRCACHE.get()); // cache fetched once per encode
        return Arrays.copyOf(b.b, b.pos); // the one copy, like the FFM out-copy
    }

    private static void encodeInto(DMsg m, Wire.Buf b, int depth,
            StrCache sc) {
        if (depth > MAX_DEPTH) {
            throw new IllegalArgumentException("nesting exceeds " + MAX_DEPTH);
        }
        for (int s = 0; s < m.def.fields.length; s++) {
            Field f = m.def.fields[s];
            if (f.presence == REQUIRED && !m.has[s]) {
                throw new IllegalStateException("missing required field "
                        + m.def.descriptor.getFullName() + "." + f.number);
            }
            if (f.repeated) {
                int n = f.kind <= K_DOUBLE ? m.repNumLen[s] : m.repRefLen[s];
                if (n == 0) {
                    continue;
                }
                if (f.kind <= K_DOUBLE) {
                    encodeRepNum(b, f, m.repNum[s], n);
                } else {
                    Object[] a = m.repRef[s];
                    for (int i = 0; i < n; i++) {
                        writeRefVal(b, f, a[i], depth, sc);
                    }
                }
                continue;
            }
            if (!m.has[s]) {
                continue;
            }
            if (f.kind <= K_DOUBLE) {
                // Fused single write: one switch per field, no helper call
                // and no second dispatch. (-0.0 compares equal to 0.0.)
                long v = m.num[s];
                if (f.presence == IMPLICIT && isDefaultNum(f.kind, v)) {
                    continue; // implicit presence never emits defaults
                }
                switch (f.kind) {
                    case K_INT32:
                    case K_ENUM:
                        writeBytes(b, f.tag);
                        wInt32(b, (int) v);
                        break;
                    case K_UINT32:
                        writeBytes(b, f.tag);
                        wVar32(b, (int) v);
                        break;
                    case K_SINT32:
                        writeBytes(b, f.tag);
                        wVar32(b, zig32((int) v));
                        break;
                    case K_INT64:
                    case K_UINT64:
                        writeBytes(b, f.tag);
                        wVar64(b, v);
                        break;
                    case K_SINT64:
                        writeBytes(b, f.tag);
                        wVar64(b, zig64(v));
                        break;
                    case K_BOOL:
                        writeBytes(b, f.tag);
                        ensure(b, 1);
                        b.b[b.pos++] = (byte) (v == 0 ? 0 : 1);
                        break;
                    case K_FIXED32:
                    case K_SFIXED32:
                    case K_FLOAT:
                        writeBytes(b, f.tag);
                        wFixed32(b, (int) v);
                        break;
                    default:
                        writeBytes(b, f.tag);
                        wFixed64(b, v);
                        break;
                }
                continue;
            }
            if (f.kind == K_STRING && m.ref[s] instanceof String
                    && f.presence == IMPLICIT
                    && ((String) m.ref[s]).isEmpty()) {
                continue;
            }
            if (f.kind == K_BYTES && m.ref[s] instanceof byte[]
                    && f.presence == IMPLICIT
                    && ((byte[]) m.ref[s]).length == 0) {
                continue;
            }
            writeRefVal(b, f, m.ref[s], depth, sc);
        }
        if (m.unknown != null) { // unknown fields last, in input order
            for (byte[] u : m.unknown) {
                ensure(b, u.length);
                System.arraycopy(u, 0, b.b, b.pos, u.length);
                b.pos += u.length;
            }
        }
    }

    /** Singular or repeated-element ref value: Slice copies verbatim. */
    private static void writeRefVal(Wire.Buf b, Field f, Object v, int depth,
            StrCache sc) {
        if (v instanceof Slice) {
            Slice sl = (Slice) v; // tag included: pure copy, stays byte-exact
            ensure(b, sl.len);
            System.arraycopy(sl.b, sl.off, b.b, b.pos, sl.len);
            b.pos += sl.len;
            return;
        }
        if (f.kind == K_STRING) {
            writeBytes(b, f.tag);
            writeStr(b, (String) v, sc);
            return;
        }
        if (f.kind == K_BYTES) {
            writeBytes(b, f.tag);
            wBytes(b, (byte[]) v);
            return;
        }
        if (f.kind == K_MESSAGE) {
            writeBytes(b, f.tag);
            int lp = patchReserve(b);
            encodeInto((DMsg) v, b, depth + 1, sc);
            patch(b, lp);
            return;
        }
        writeGroup(b, f, (DMsg) v, depth, sc);
    }

    private static boolean isDefaultNum(int kind, long v) {
        switch (kind) {
            case K_FLOAT:
                return Float.intBitsToFloat((int) v) == 0f;
            case K_DOUBLE:
                return Double.longBitsToDouble(v) == 0d;
            default:
                return v == 0;
        }
    }

    /** One dispatch per field; the loops below it are branch-free writes. */
    private static void encodeRepNum(Wire.Buf b, Field f, long[] a, int n) {
        if (f.packed) {
            if (f.kind == K_BOOL) {
                writeBytes(b, f.tag);
                wVar32(b, n);
                ensure(b, n);
                for (int i = 0; i < n; i++) {
                    b.b[b.pos++] = (byte) (a[i] == 0 ? 0 : 1);
                }
                return;
            }
            if (f.kind == K_FIXED32 || f.kind == K_SFIXED32
                    || f.kind == K_FLOAT) {
                writeBytes(b, f.tag);
                wVar32(b, n * 4);
                ensure(b, n * 4);
                for (int i = 0; i < n; i++) {
                    int v = (int) a[i];
                    b.b[b.pos++] = (byte) v;
                    b.b[b.pos++] = (byte) (v >> 8);
                    b.b[b.pos++] = (byte) (v >> 16);
                    b.b[b.pos++] = (byte) (v >> 24);
                }
                return;
            }
            if (f.kind == K_FIXED64 || f.kind == K_SFIXED64
                    || f.kind == K_DOUBLE) {
                writeBytes(b, f.tag);
                wVar32(b, n * 8);
                ensure(b, n * 8);
                for (int i = 0; i < n; i++) {
                    long v = a[i];
                    b.b[b.pos++] = (byte) v;
                    b.b[b.pos++] = (byte) (v >> 8);
                    b.b[b.pos++] = (byte) (v >> 16);
                    b.b[b.pos++] = (byte) (v >> 24);
                    b.b[b.pos++] = (byte) (v >> 32);
                    b.b[b.pos++] = (byte) (v >> 40);
                    b.b[b.pos++] = (byte) (v >> 48);
                    b.b[b.pos++] = (byte) (v >> 56);
                }
                return;
            }
            // Varint kinds: payload first at a 5-byte placeholder, then
            // shift it down over the unused length bytes. One 6 KB memmove
            // (~1 us) beats a full size pre-pass over 3000 elements.
            writeBytes(b, f.tag);
            int lp = b.pos;
            ensure(b, n * 10 + 5);
            b.pos += 5;
            for (int i = 0; i < n; i++) {
                writePackedVar(b, f.kind, a[i]);
            }
            int len = b.pos - lp - 5;
            int llen = sizeVar32(len);
            System.arraycopy(b.b, lp + 5, b.b, lp + llen, len);
            int p = lp;
            int v = len;
            while ((v & ~0x7F) != 0) {
                b.b[p++] = (byte) ((v & 0x7F) | 0x80);
                v >>>= 7;
            }
            b.b[p++] = (byte) v;
            b.pos = lp + llen + len;
            return;
        }
        for (int i = 0; i < n; i++) { // unpacked: tag per element
            writeBytes(b, f.tag);
            writeNumSingle(b, f.kind, a[i]);
        }
    }

    private static void writePackedVar(Wire.Buf b, int kind, long v) {
        switch (kind) {
            case K_INT32:
            case K_ENUM:
                wInt32(b, (int) v);
                return;
            case K_UINT32:
                wVar32(b, (int) v);
                return;
            case K_SINT32:
                wVar32(b, zig32((int) v));
                return;
            case K_INT64:
            case K_UINT64:
                wVar64(b, v);
                return;
            case K_SINT64:
                wVar64(b, zig64(v));
                return;
            default:
                throw new IllegalArgumentException("not packable");
        }
    }

    private static void writeNumSingle(Wire.Buf b, int kind, long v) {
        switch (kind) {
            case K_INT32:
            case K_ENUM:
                wInt32(b, (int) v);
                return;
            case K_UINT32:
                wVar32(b, (int) v);
                return;
            case K_SINT32:
                wVar32(b, zig32((int) v));
                return;
            case K_INT64:
            case K_UINT64:
                wVar64(b, v);
                return;
            case K_SINT64:
                wVar64(b, zig64(v));
                return;
            case K_BOOL:
                ensure(b, 1);
                b.b[b.pos++] = (byte) (v == 0 ? 0 : 1);
                return;
            case K_FIXED32:
            case K_SFIXED32:
            case K_FLOAT:
                wFixed32(b, (int) v);
                return;
            case K_FIXED64:
            case K_SFIXED64:
            case K_DOUBLE:
                wFixed64(b, v);
                return;
            default:
                throw new IllegalArgumentException("not numeric");
        }
    }

    private static void writeBytes(Wire.Buf b, byte[] tag) {
        if (tag.length == 2) { // every multi-byte field number: no loop
            ensure(b, 2);
            b.b[b.pos++] = tag[0];
            b.b[b.pos++] = tag[1];
            return;
        }
        ensure(b, tag.length);
        for (byte x : tag) {
            b.b[b.pos++] = x;
        }
    }

    private static void writeGroup(Wire.Buf b, Field f, DMsg m, int depth,
            StrCache sc) {
        writeBytes(b, Wire.tagBytes(f.number, 3));
        encodeInto(m, b, depth + 1, sc);
        writeBytes(b, Wire.tagBytes(f.number, 4));
    }

    /* ---------------- decode ---------------- */

    private static final ThreadLocal<Wire.Reader> READER =
            ThreadLocal.withInitial(Wire.Reader::new);

    public static DMsg decode(MessageDef def, byte[] wire) {
        Wire.Reader r = READER.get();
        r.b = wire;
        r.pos = 0;
        r.end = wire.length;
        DMsg m = decodeInto(new DMsg(def), r, wire.length, -1, 0);
        if (r.pos != wire.length) {
            throw new IllegalArgumentException("trailing bytes");
        }
        return m;
    }

    /**
     * Decodes into m up to end (or the matching end-group when groupEnd
     * >= 0). One shared Reader threads through the recursion, so positions
     * never need saving: nested payloads consume exactly their slice.
     * Ref-kind fields (message/group/string/bytes) record a Slice and move
     * on; numerics parse eagerly.
     */
    private static DMsg decodeInto(DMsg m, Wire.Reader r, int end,
            int groupEnd, int depth) {
        if (depth > MAX_DEPTH) {
            throw new IllegalArgumentException("nesting exceeds " + MAX_DEPTH);
        }
        MessageDef def = m.def;
        int outerEnd = r.end;
        r.end = end; // nested slices share the Reader; restored on exit
        boolean closed = groupEnd < 0;
        while (r.pos < r.end) {
            int fieldStart = r.pos;
            int tag = (int) rVar(r);
            int fnum = tag >>> 3;
            int wt = tag & 7;
            if (groupEnd >= 0 && wt == 4 && fnum == groupEnd) {
                closed = true; // our end-group; tag already consumed
                break;
            }
            int s = fnum < def.slotByFnum.length ? def.slotByFnum[fnum] : -1;
            if (s < 0 || wtIncompatible(def.fields[s], wt)) {
                retainUnknown(m, r, fieldStart, wt);
                continue;
            }
            readKnown(m, r, def.fields[s], s, wt, fieldStart);
        }
        r.end = outerEnd;
        if (!closed) {
            throw new IllegalArgumentException("missing end-group for "
                    + groupEnd);
        }
        return m;
    }

    /** Wrong wire type (or unknown number) keeps the raw blob, like the core. */
    private static void retainUnknown(DMsg m, Wire.Reader r, int fieldStart,
            int wt) {
        if (wt == 3) { // group: scan to the matching end tag, keep it all
            int depth = 1;
            while (depth > 0) {
                if (r.pos >= r.end) {
                    throw new IllegalArgumentException("truncated group");
                }
                int t = (int) rVar(r);
                int w = t & 7;
                if (w == 3) {
                    depth++;
                } else if (w == 4) {
                    depth--;
                } else {
                    skip(r, w);
                }
            }
        } else {
            skip(r, wt);
        }
        if (m.unknown == null) {
            m.unknown = new ArrayList<>(4); // lazy: most messages have none
        }
        m.unknown.add(Arrays.copyOfRange(r.b, fieldStart, r.pos));
    }

    private static boolean wtIncompatible(Field f, int wt) {
        if (f.kind == K_GROUP) {
            return wt != 3; // start-group only; end tags handled by the parent
        }
        if (f.repeated && wt == WT_LEN && isPackableKind(f.kind)) {
            return false; // packed run (or unpacked-form tag, checked inside)
        }
        return wt != f.wt;
    }

    private static void readKnown(DMsg m, Wire.Reader r, Field f, int s,
            int wt, int fieldStart) {
        if (f.repeated) {
            if (f.kind <= K_DOUBLE) {
                long[] a = m.repNum[s];
                int n = m.repNumLen[s];
                if (wt == WT_LEN && isPackableKind(f.kind)) { // packed run
                    int len = rLen(r);
                    int end = r.pos + len;
                    if (f.kind == K_BOOL) {
                        a = needLong(m, s, n + len);
                        while (r.pos < end) {
                            a[n++] = rVar(r) != 0 ? 1 : 0;
                        }
                    } else if (f.kind == K_FIXED32 || f.kind == K_SFIXED32
                            || f.kind == K_FLOAT) {
                        int count = len / 4;
                        a = needLong(m, s, n + count);
                        for (int i = 0; i < count; i++) {
                            a[n++] = rFixed32(r) & 0xFFFFFFFFL;
                        }
                    } else if (f.kind == K_FIXED64 || f.kind == K_SFIXED64
                            || f.kind == K_DOUBLE) {
                        int count = len / 8;
                        a = needLong(m, s, n + count);
                        for (int i = 0; i < count; i++) {
                            a[n++] = rFixed64(r);
                        }
                    } else { // packed varints: at most len elements
                        a = needLong(m, s, n + Math.min(len, 4096));
                        while (r.pos < end) {
                            if (n == a.length) {
                                a = growLong(a, n + 1);
                                m.repNum[s] = a;
                            }
                            a[n++] = readPackedVar(r, f.kind);
                        }
                    }
                    if (r.pos != end) {
                        throw new IllegalArgumentException("packed overrun");
                    }
                    m.repNumLen[s] = n;
                    return;
                }
                a = needLong(m, s, n + 1); // unpacked element
                a[n] = readNumSingle(r, f.kind);
                m.repNumLen[s] = n + 1;
                return;
            }
            // Ref-kind repeated: record one slice per element, parse nothing.
            if (f.kind == K_MESSAGE) {
                int len = rLen(r);
                r.pos += len;
            } else if (f.kind == K_GROUP) {
                int depth = 1;
                while (depth > 0) {
                    if (r.pos >= r.end) {
                        throw new IllegalArgumentException("truncated group");
                    }
                    int t = (int) rVar(r);
                    int w = t & 7;
                    if (w == 3) {
                        depth++;
                    } else if (w == 4) {
                        depth--;
                    } else {
                        skip(r, w);
                    }
                }
            } else { // K_STRING / K_BYTES element
                int len = rLen(r);
                r.pos += len;
            }
            Object[] a = m.repRef[s];
            int n = m.repRefLen[s];
            if (a == null || n == a.length) {
                a = growObj(a, n + 1);
                m.repRef[s] = a;
            }
            a[n] = new Slice(r.b, fieldStart, r.pos - fieldStart);
            m.repRefLen[s] = n + 1;
            return;
        }
        if (f.kind <= K_DOUBLE) {
            long v = readNumSingle(r, f.kind);
            m.num[s] = v;
            if (f.presence != IMPLICIT || !isDefaultNum(f.kind, v)) {
                m.setPresent(s, f);
            }
            return;
        }
        // Ref-kind singular: slice it (empty implicit strings stay absent).
        if (f.kind == K_MESSAGE) {
            int len = rLen(r);
            if (len == 0 && f.presence == IMPLICIT) {
                return; // unreachable in practice, but costs nothing
            }
            r.pos += len;
        } else if (f.kind == K_GROUP) {
            int depth = 1;
            while (depth > 0) {
                if (r.pos >= r.end) {
                    throw new IllegalArgumentException("truncated group");
                }
                int t = (int) rVar(r);
                int w = t & 7;
                if (w == 3) {
                    depth++;
                } else if (w == 4) {
                    depth--;
                } else {
                    skip(r, w);
                }
            }
        } else { // K_STRING / K_BYTES
            int len = rLen(r);
            if (len == 0 && f.presence == IMPLICIT) {
                return; // proto3 "": absent, never emitted
            }
            r.pos += len;
        }
        m.ref[s] = new Slice(r.b, fieldStart, r.pos - fieldStart);
        m.setPresent(s, f);
    }

    private static long[] needLong(DMsg m, int s, int need) {
        long[] a = m.repNum[s];
        if (a == null || a.length < need) {
            a = growLong(a, need);
            m.repNum[s] = a;
        }
        return a;
    }

    /** Parses a slice on first access; caches the value in place. */
    static Object materialize(Field f, Slice sl) {
        Wire.Reader r = new Wire.Reader(); // rare path: no workspace needed
        r.b = sl.b;
        r.pos = sl.off;
        r.end = sl.off + sl.len;
        rVar(r); // the tag; already validated at slice time
        if (f.kind == K_MESSAGE) {
            int n = rLen(r);
            DMsg sub = new DMsg(f.msgType);
            decodeInto(sub, r, r.pos + n, -1, 0);
            return sub;
        }
        if (f.kind == K_GROUP) {
            DMsg sub = new DMsg(f.msgType);
            decodeInto(sub, r, r.end, f.number, 0);
            return sub;
        }
        int n = rLen(r);
        if (f.kind == K_STRING) {
            return new String(r.b, r.pos, n, StandardCharsets.UTF_8);
        }
        return Arrays.copyOfRange(r.b, r.pos, r.pos + n);
    }

    private static long readPackedVar(Wire.Reader r, int kind) {
        long v = rVar(r);
        if (kind == K_SINT32) {
            return unzig32(v);
        }
        if (kind == K_SINT64) {
            return unzig64(v);
        }
        return v; // INT32/UINT32/INT64/UINT64/ENUM stay raw (decode at use)
    }

    private static long readNumSingle(Wire.Reader r, int kind) {
        switch (kind) {
            case K_INT32:
            case K_UINT32:
            case K_INT64:
            case K_UINT64:
            case K_BOOL:
            case K_ENUM:
                return rVar(r);
            case K_SINT32:
                return unzig32(rVar(r));
            case K_SINT64:
                return unzig64(rVar(r));
            case K_FIXED32:
            case K_SFIXED32:
            case K_FLOAT:
                return rFixed32(r) & 0xFFFFFFFFL;
            case K_FIXED64:
            case K_SFIXED64:
            case K_DOUBLE:
                return rFixed64(r);
            default:
                throw new IllegalArgumentException("not numeric");
        }
    }
}

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
 *
 * Small-tier encode floor (136 B, ~0.7x vs generated): one presence gate +
 * one dispatch + tag load per field plus the result byte[] copy (~80 ns vs
 * ~60 ns straight-line). Fused writes and Unsafe stores already removed
 * what tuning can; closing it needs per-message codegen, which this
 * binding avoids on purpose. Do not retry micro-tuning here.
 */
public final class GenericCodec {
    private GenericCodec() {
    }

    /** Presence kinds. IMPLICIT = proto3 plain scalar (no has-bit). */
    public static final int IMPLICIT = 0, EXPLICIT = 1, REQUIRED = 2, MESSAGE = 3;

    /* Storage kinds: one dispatch per field, tight loops per element. */
    static final int K_INT32 = 0, K_UINT32 = 1, K_SINT32 = 2,
            K_INT64 = 3, K_UINT64 = 4, K_SINT64 = 5, K_BOOL = 6, K_ENUM = 7,
            K_FIXED32 = 8, K_SFIXED32 = 9, K_FLOAT = 10, K_FIXED64 = 11,
            K_SFIXED64 = 12, K_DOUBLE = 13, K_STRING = 14, K_BYTES = 15,
            K_MESSAGE = 16, K_GROUP = 17;

    public static final class Field {
        public int number;
        public Proto.Type type;
        public int kind;
        public int wt; // wire type for the singular/packed form
        public boolean repeated;
        public boolean packed;
        public int oneof = -1; // oneof index, -1 = none
        public int presence = IMPLICIT;
        public MessageDef msgType; // MESSAGE/GROUP element type
        public byte[] tag; // precomputed tag bytes (singular/packed form)
        public String fieldName;
    }

    public static final class MessageDef {
        public String fullName;
        public Field[] fields; // sorted by number
        public int[] slotByFnum; // dense
        public Map<String, Integer> byName = new HashMap<>();
        public Map<String, Integer> oneofByName = new HashMap<>();
        public int oneofCount;
        public boolean hasRequired; // any REQUIRED field: proto2 only
        // Transitive closure: some reachable message has a REQUIRED field.
        // decode() checks the tree only then, so other decodes pay one test.
        public boolean deepRequired;
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

        /**
         * Loads every message (+ extensions) in the set. The set is parsed
         * and linked here, so the runtime has no protobuf-java dependency:
         * a descriptor set plus this file is the whole schema story.
         */
        public static Schema load(byte[] fileDescriptorSet) {
            List<Proto.File> files;
            try {
                files = Proto.parseSet(fileDescriptorSet);
                Proto.link(files);
            } catch (IllegalArgumentException e) {
                throw new IllegalArgumentException("descriptor set: "
                        + e.getMessage(), e);
            }
            Schema s = new Schema();
            for (Proto.File f : files) {
                for (Proto.Message m : f.messages) {
                    s.buildDef(m);
                }
            }
            // Extensions attach to their extendee after all defs exist.
            for (Proto.File f : files) {
                for (Proto.Field e : f.extensions) {
                    s.attachExtension(e);
                }
                for (Proto.Message m : f.messages) {
                    for (Proto.Field e : m.extensions) {
                        s.attachExtension(e);
                    }
                }
            }
            // Transitive required closure, monotone fixpoint so recursive
            // messages terminate: a cycle with no REQUIRED field stays false.
            boolean changed = true;
            while (changed) {
                changed = false;
                for (MessageDef d : s.byName.values()) {
                    if (!d.deepRequired && needsRequired(d)) {
                        d.deepRequired = true;
                        changed = true;
                    }
                }
            }
            return s;
        }

        private static boolean needsRequired(MessageDef d) {
            for (Field f : d.fields) {
                if (f.presence == REQUIRED) {
                    return true;
                }
                if ((f.kind == K_MESSAGE || f.kind == K_GROUP)
                        && f.msgType != null && f.msgType.deepRequired) {
                    return true;
                }
            }
            return false;
        }

        private MessageDef buildDef(Proto.Message m) {
            MessageDef done = byName.get(m.fullName);
            if (done != null) {
                return done;
            }
            MessageDef d = new MessageDef();
            d.fullName = m.fullName;
            byName.put(m.fullName, d); // before fields: recursion-safe
            List<Field> fs = new ArrayList<>();
            Map<Proto.Oneof, Integer> oneofs = new HashMap<>();
            for (Proto.Field f : m.fields) {
                fs.add(buildField(f, oneofs));
            }
            finishDef(d, fs, oneofs);
            for (Proto.Message n : m.nested) {
                buildDef(n);
            }
            return d;
        }

        private Field buildField(Proto.Field f,
                Map<Proto.Oneof, Integer> oneofs) {
            Field s = new Field();
            s.number = f.number;
            s.type = f.type;
            s.kind = kindOf(f.type);
            s.repeated = f.label == Proto.Label.REPEATED;
            s.packed = f.packed;
            s.fieldName = f.name;
            if (f.oneof != null) {
                Integer idx = oneofs.get(f.oneof);
                if (idx == null) {
                    idx = oneofs.size();
                    oneofs.put(f.oneof, idx);
                }
                s.oneof = idx;
                s.presence = EXPLICIT; // reads/writes go through the oneof case
            } else if (f.label == Proto.Label.REQUIRED) {
                s.presence = REQUIRED;
            } else if (s.type == Proto.Type.MESSAGE
                    || s.type == Proto.Type.GROUP) {
                s.presence = MESSAGE;
            } else if (f.hasPresence) {
                s.presence = EXPLICIT;
            } else {
                s.presence = IMPLICIT;
            }
            if (s.type == Proto.Type.MESSAGE || s.type == Proto.Type.GROUP) {
                s.msgType = buildDef(f.messageType);
            }
            s.wt = wireType(s.type, s.repeated && s.packed);
            s.tag = Wire.tagBytes(s.number, s.wt);
            return s;
        }

        private void attachExtension(Proto.Field e) {
            String extendee = e.extendee;
            if (extendee.startsWith(".")) {
                extendee = extendee.substring(1);
            }
            MessageDef d = byName.get(extendee);
            if (d == null) {
                throw new IllegalArgumentException("extendee missing: "
                        + e.extendee);
            }
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
                Map<Proto.Oneof, Integer> oneofs) {
            fs.sort((a, b) -> Integer.compare(a.number, b.number));
            int max = 0;
            boolean req = false;
            for (Field f : fs) {
                max = Math.max(max, f.number);
                req = req || f.presence == REQUIRED;
            }
            d.hasRequired = req;
            d.fields = fs.toArray(new Field[0]);
            d.slotByFnum = new int[max + 1];
            Arrays.fill(d.slotByFnum, -1);
            d.byName.clear();
            for (int i = 0; i < d.fields.length; i++) {
                d.slotByFnum[d.fields[i].number] = i;
                d.byName.put(d.fields[i].fieldName, i);
            }
            d.oneofByName.clear();
            for (Map.Entry<Proto.Oneof, Integer> oe : oneofs.entrySet()) {
                d.oneofByName.put(oe.getKey().name, oe.getValue());
            }
            d.oneofCount = Math.max(d.oneofCount, oneofs.size());
        }
    }

    private static int kindOf(Proto.Type t) {
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

    private static int wireType(Proto.Type t, boolean packed) {
        if (packed) {
            return Wire.WT_LEN;
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
                return Wire.WT_VARINT;
            case FIXED64:
            case SFIXED64:
            case DOUBLE:
                return Wire.WT_FIXED64;
            case STRING:
            case BYTES:
            case MESSAGE:
                return Wire.WT_LEN;
            case GROUP:
                return 3; // start-group tag; end tag derived at use
            case FIXED32:
            case SFIXED32:
            case FLOAT:
                return Wire.WT_FIXED32;
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
                throw new IllegalArgumentException(def.fullName
                        + " has no field " + fnum);
            }
            return def.slotByFnum[fnum];
        }

        private int slotByName(String name) {
            Integer i = def.byName.get(name);
            if (i == null) {
                throw new IllegalArgumentException(def.fullName
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
            // Repeated presence lives in has[], like singular fields: the
            // encode loop gates every slot on it (see encodeInto).
            has[s] = true;
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
            // Every integer kind boxes as Long: the public get()/getList()
            // contract (and the old FFI binding) reads ints as long.
            switch (f.kind) {
                case K_INT32:
                case K_SINT32:
                case K_SFIXED32:
                case K_ENUM:
                    return Long.valueOf((int) n);
                case K_UINT32:
                case K_FIXED32:
                    // Unsigned 32-bit reads zero-extended, so 4000000000
                    // reads as 4000000000L (like the uint64 raw-bits rule).
                    return Long.valueOf(n & 0xFFFFFFFFL);
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

    /** One workspace per thread: the encode buffer plus the string
     *  cache, so an encode pays a single ThreadLocal lookup. */
    private static final class Workspace {
        final Wire.Buf buf = new Wire.Buf();
        final StrCache strings = new StrCache();
    }

    private static final ThreadLocal<Workspace> WORKSPACE =
            ThreadLocal.withInitial(Workspace::new);

    /** Whole field image (tag + length + UTF-8) by reference: a stable
     *  String on a stable field re-emits with one arraycopy. 8 FIFO
     *  slots cover the handful of distinct (field, string) pairs a
     *  message carries; beyond that each emit rebuilds its image. */
    private static final class StrCache {
        final byte[][] tags = new byte[8][];
        final String[] strs = new String[8];
        final byte[][] imgs = new byte[8][];
        int next;
    }

    private static void writeStrField(Wire.Buf b, byte[] tag, String s,
            StrCache c) {
        for (int i = 0; i < c.tags.length; i++) {
            if (c.tags[i] == tag && c.strs[i] == s) {
                byte[] img = c.imgs[i];
                Wire.ensure(b, img.length);
                System.arraycopy(img, 0, b.b, b.pos, img.length);
                b.pos += img.length;
                return;
            }
        }
        byte[] u = s.getBytes(StandardCharsets.UTF_8);
        int llen = Wire.sizeVar32(u.length);
        byte[] img = new byte[tag.length + llen + u.length];
        System.arraycopy(tag, 0, img, 0, tag.length);
        int p = tag.length;
        int n = u.length;
        while ((n & ~0x7F) != 0) {
            img[p++] = (byte) ((n & 0x7F) | 0x80);
            n >>>= 7;
        }
        img[p++] = (byte) n;
        System.arraycopy(u, 0, img, p, u.length);
        c.tags[c.next] = tag;
        c.strs[c.next] = s;
        c.imgs[c.next] = img;
        c.next = (c.next + 1) & (c.tags.length - 1);
        Wire.ensure(b, img.length);
        System.arraycopy(img, 0, b.b, b.pos, img.length);
        b.pos += img.length;
    }

    /** Single pass into a reusable buffer; nested payloads use a 1-byte
     *  length placeholder patched after (Wire.patch has the overflow).
     *  Untouched lazy slices copy verbatim: tag included, no re-encode.
     *  Singular numerics go through one fused tag+value write each. */
    public static byte[] encode(DMsg m) {
        Workspace w = WORKSPACE.get();
        Wire.Buf b = w.buf;
        b.pos = 0;
        encodeInto(m, b, 0, w.strings);
        return Arrays.copyOf(b.b, b.pos); // the one copy, like the FFM out-copy
    }

    private static void encodeInto(DMsg m, Wire.Buf b, int depth,
            StrCache sc) {
        if (depth > Wire.MAX_DEPTH) {
            throw new IllegalArgumentException("nesting exceeds " + Wire.MAX_DEPTH);
        }
        MessageDef def = m.def;
        boolean noRequired = !def.hasRequired; // proto3: skip the check
        for (int s = 0; s < def.fields.length; s++) {
            if (!m.has[s]) {
                // Absent singulars and empty repeats share one gate: every
                // mutation (set/add/decode) maintains has[] for both, so
                // the loop never consults lengths or defaults to skip.
                // Required fields throw below instead of skipping: a
                // missing required field is an error, never empty output.
                if (!noRequired && def.fields[s].presence == REQUIRED) {
                    throw new IllegalStateException("missing required field "
                            + def.fullName + "." + def.fields[s].number);
                }
                continue;
            }
            Field f = def.fields[s];
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
            int kind = f.kind;
            if (kind <= K_DOUBLE) {
                // has[] is set above, so only implicit defaults skip here;
                // explicit defaults (and required) emit once set.
                // Raw-bits zero is the default for every numeric kind
                // (float -0.0 has nonzero bits, like protobuf-java).
                long v = m.num[s];
                if (v == 0 && f.presence == IMPLICIT) {
                    continue;
                }
                byte[] tag = f.tag;
                switch (kind) {
                    case K_INT32:
                    case K_ENUM: {
                        int x = (int) v;
                        if (x >= 0) {
                            Wire.wTagVar32(b, tag, x);
                        } else {
                            Wire.wTagVar64(b, tag, v);
                        }
                        break;
                    }
                    case K_UINT32:
                        Wire.wTagVar32(b, tag, (int) v);
                        break;
                    case K_SINT32:
                        Wire.wTagVar32(b, tag, Wire.zig32((int) v));
                        break;
                    case K_INT64:
                    case K_UINT64:
                        Wire.wTagVar64(b, tag, v);
                        break;
                    case K_SINT64:
                        Wire.wTagVar64(b, tag, Wire.zig64(v));
                        break;
                    case K_BOOL:
                        Wire.wTagVar32(b, tag, v == 0 ? 0 : 1);
                        break;
                    case K_FIXED32:
                    case K_SFIXED32:
                    case K_FLOAT:
                        Wire.wTagFixed32(b, tag, (int) v);
                        break;
                    default:
                        Wire.wTagFixed64(b, tag, v);
                        break;
                }
                continue;
            }
            // has[] is set (see the loop gate): implicit empty strings
            // and bytes stay silent, everything else emits.
            if (kind == K_STRING && m.ref[s] instanceof String
                    && f.presence == IMPLICIT
                    && ((String) m.ref[s]).isEmpty()) {
                continue;
            }
            if (kind == K_BYTES && m.ref[s] instanceof byte[]
                    && f.presence == IMPLICIT
                    && ((byte[]) m.ref[s]).length == 0) {
                continue;
            }
            writeRefVal(b, f, m.ref[s], depth, sc);
        }
        if (m.unknown != null) { // unknown fields last, in input order
            for (byte[] u : m.unknown) {
                Wire.ensure(b, u.length);
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
            Wire.ensure(b, sl.len);
            System.arraycopy(sl.b, sl.off, b.b, b.pos, sl.len);
            b.pos += sl.len;
            return;
        }
        if (f.kind == K_STRING) {
            writeStrField(b, f.tag, (String) v, sc);
            return;
        }
        if (f.kind == K_BYTES) {
            writeBytes(b, f.tag);
            Wire.wBytes(b, (byte[]) v);
            return;
        }
        if (f.kind == K_MESSAGE) {
            // Inlined tag + length-placeholder + patch: the hottest
            // nesting shape (xlarge encodes 3000 of them) pays no calls.
            byte[] tag = f.tag;
            Wire.ensure(b, tag.length + 1);
            for (int i = 0; i < tag.length; i++) {
                b.b[b.pos++] = tag[i];
            }
            int lp = b.pos;
            b.b[b.pos++] = 0;
            encodeInto((DMsg) v, b, depth + 1, sc);
            int len = b.pos - lp - 1;
            if (len < 128) {
                b.b[lp] = (byte) len;
            } else {
                Wire.patch(b, lp);
            }
            return;
        }
        writeGroup(b, f, (DMsg) v, depth, sc);
    }

    /** One dispatch per field; the loops below it are branch-free writes. */
    private static void encodeRepNum(Wire.Buf b, Field f, long[] a, int n) {
        if (f.packed) {
            if (n == 1) {
                // Single element: the length is one byte, so a placeholder
                // shuffle (and its arraycopy call) would cost more than
                // the payload. Only the medium tier hits this.
                if (f.kind == K_BOOL) {
                    writeBytes(b, f.tag);
                    Wire.ensure(b, 2);
                    b.b[b.pos++] = 1;
                    b.b[b.pos++] = (byte) (a[0] == 0 ? 0 : 1);
                    return;
                }
                if (f.kind == K_FIXED32 || f.kind == K_SFIXED32
                        || f.kind == K_FLOAT) {
                    writeBytes(b, f.tag);
                    Wire.ensure(b, 5);
                    b.b[b.pos++] = 4;
                    Wire.wFixed32(b, (int) a[0]);
                    return;
                }
                if (f.kind == K_FIXED64 || f.kind == K_SFIXED64
                        || f.kind == K_DOUBLE) {
                    writeBytes(b, f.tag);
                    Wire.ensure(b, 9);
                    b.b[b.pos++] = 8;
                    Wire.wFixed64(b, a[0]);
                    return;
                }
                int sz = sizePackedVar(f.kind, a[0]);
                writeBytes(b, f.tag);
                Wire.ensure(b, 1 + sz);
                b.b[b.pos++] = (byte) sz;
                writePackedVar(b, f.kind, a[0]);
                return;
            }
            if (f.kind == K_BOOL) {
                writeBytes(b, f.tag);
                Wire.wVar32(b, n);
                Wire.ensure(b, n);
                for (int i = 0; i < n; i++) {
                    b.b[b.pos++] = (byte) (a[i] == 0 ? 0 : 1);
                }
                return;
            }
            if (f.kind == K_FIXED32 || f.kind == K_SFIXED32
                    || f.kind == K_FLOAT) {
                writeBytes(b, f.tag);
                Wire.wVar32(b, n * 4);
                Wire.ensure(b, n * 4);
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
                Wire.wVar32(b, n * 8);
                Wire.ensure(b, n * 8);
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
            Wire.ensure(b, n * 10 + 5);
            b.pos += 5;
            for (int i = 0; i < n; i++) {
                writePackedVar(b, f.kind, a[i]);
            }
            int len = b.pos - lp - 5;
            int llen = Wire.sizeVar32(len);
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

    private static int sizePackedVar(int kind, long v) {
        switch (kind) {
            case K_INT32:
            case K_ENUM:
                return Wire.sizeInt32((int) v);
            case K_UINT32:
                return Wire.sizeVar32((int) v);
            case K_SINT32:
                return Wire.sizeVar32(Wire.zig32((int) v));
            case K_INT64:
            case K_UINT64:
                return Wire.sizeVar64(v);
            case K_SINT64:
                return Wire.sizeVar64(Wire.zig64(v));
            default:
                throw new IllegalArgumentException("not packable");
        }
    }

    private static void writePackedVar(Wire.Buf b, int kind, long v) {
        switch (kind) {
            case K_INT32:
            case K_ENUM:
                Wire.wInt32(b, (int) v);
                return;
            case K_UINT32:
                Wire.wVar32(b, (int) v);
                return;
            case K_SINT32:
                Wire.wVar32(b, Wire.zig32((int) v));
                return;
            case K_INT64:
            case K_UINT64:
                Wire.wVar64(b, v);
                return;
            case K_SINT64:
                Wire.wVar64(b, Wire.zig64(v));
                return;
            default:
                throw new IllegalArgumentException("not packable");
        }
    }

    private static void writeNumSingle(Wire.Buf b, int kind, long v) {
        switch (kind) {
            case K_INT32:
            case K_ENUM:
                Wire.wInt32(b, (int) v);
                return;
            case K_UINT32:
                Wire.wVar32(b, (int) v);
                return;
            case K_SINT32:
                Wire.wVar32(b, Wire.zig32((int) v));
                return;
            case K_INT64:
            case K_UINT64:
                Wire.wVar64(b, v);
                return;
            case K_SINT64:
                Wire.wVar64(b, Wire.zig64(v));
                return;
            case K_BOOL:
                Wire.ensure(b, 1);
                b.b[b.pos++] = (byte) (v == 0 ? 0 : 1);
                return;
            case K_FIXED32:
            case K_SFIXED32:
            case K_FLOAT:
                Wire.wFixed32(b, (int) v);
                return;
            case K_FIXED64:
            case K_SFIXED64:
            case K_DOUBLE:
                Wire.wFixed64(b, v);
                return;
            default:
                throw new IllegalArgumentException("not numeric");
        }
    }

    private static void writeBytes(Wire.Buf b, byte[] tag) {
        Wire.ensure(b, tag.length);
        Wire.putTag(b, tag);
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
        // The core refuses a message missing a required field at decode
        // time; the flag above keeps this walk off trees that cannot fail it.
        if (def.deepRequired) {
            checkRequired(m, 0);
        }
        return m;
    }

    /**
     * Whole-tree required check. Present nested messages parse here (and are
     * cached in place), so a lazy slice cannot smuggle in a missing field.
     */
    private static void checkRequired(DMsg m, int depth) {
        if (depth > Wire.MAX_DEPTH) {
            throw new IllegalArgumentException("nesting exceeds " + Wire.MAX_DEPTH);
        }
        MessageDef def = m.def;
        if (def.hasRequired) {
            for (int s = 0; s < def.fields.length; s++) {
                if (def.fields[s].presence == REQUIRED && !m.has[s]) {
                    throw new IllegalArgumentException("missing required field "
                            + def.fullName + "." + def.fields[s].number);
                }
            }
        }
        for (int s = 0; s < def.fields.length; s++) {
            Field f = def.fields[s];
            if ((f.kind != K_MESSAGE && f.kind != K_GROUP) || !m.has[s]) {
                continue;
            }
            if (f.repeated) {
                for (int i = 0; i < m.repRefLen[s]; i++) {
                    Object v = m.repRef[s][i];
                    if (v instanceof Slice) {
                        v = materialize(f, (Slice) v);
                        m.repRef[s][i] = v;
                    }
                    if (v instanceof DMsg) {
                        checkRequired((DMsg) v, depth + 1);
                    }
                }
            } else {
                Object v = m.ref[s];
                if (v instanceof Slice) {
                    v = materialize(f, (Slice) v);
                    m.ref[s] = v;
                }
                if (v instanceof DMsg) {
                    checkRequired((DMsg) v, depth + 1);
                }
            }
        }
    }

    /**
     * Decodes into m up to end (or the matching end-group when groupEnd
     * >= 0). One shared Reader threads through the recursion, so positions
     * never need saving: nested payloads consume exactly their slice.
     * Ref-kind fields (message/group/string/bytes) record a Slice and move
     * on; numerics parse eagerly. Tags and lengths are strict (like the
     * core): field 0, wire types above 5 and 6-byte tags are rejected, and
     * a 5-byte tag above UINT32_MAX truncates exactly like the reference.
     */
    private static DMsg decodeInto(DMsg m, Wire.Reader r, int end,
            int groupEnd, int depth) {
        if (depth > Wire.MAX_DEPTH) {
            throw new IllegalArgumentException("nesting exceeds " + Wire.MAX_DEPTH);
        }
        MessageDef def = m.def;
        int outerEnd = r.end;
        r.end = end; // nested slices share the Reader; restored on exit
        boolean closed = groupEnd < 0;
        while (r.pos < r.end) {
            int fieldStart = r.pos;
            int tag = Wire.rTag(r);
            int fnum = tag >>> 3;
            int wt = tag & 7;
            if (fnum == 0 || wt > Wire.WT_FIXED32) {
                throw new IllegalArgumentException("bad tag " + tag);
            }
            if (wt == 4) { // end-group: only our own number closes us
                if (groupEnd < 0 || fnum != groupEnd) {
                    throw new IllegalArgumentException("stray end-group for "
                            + fnum);
                }
                closed = true; // our end-group; tag already consumed
                break;
            }
            int s = fnum < def.slotByFnum.length ? def.slotByFnum[fnum] : -1;
            if (s < 0 || wtIncompatible(def.fields[s], wt)) {
                retainUnknown(m, r, fieldStart, fnum, wt, depth);
                continue;
            }
            readKnown(m, r, def.fields[s], s, wt, fieldStart, depth);
        }
        r.end = outerEnd;
        if (!closed) {
            throw new IllegalArgumentException("missing end-group for "
                    + groupEnd);
        }
        return m;
    }

    /**
     * Wrong wire type (or unknown number) keeps the raw blob, like the core.
     * Groups validate to their matching end tag (numbers must pair); anything
     * else only checks value shape -- unknown LEN payloads stay opaque.
     */
    private static void retainUnknown(DMsg m, Wire.Reader r, int fieldStart,
            int fnum, int wt, int depth) {
        Wire.skipUnknown(r, fnum, wt, depth);
        if (m.unknown == null) {
            m.unknown = new ArrayList<>(4); // lazy: most messages have none
        }
        m.unknown.add(Arrays.copyOfRange(r.b, fieldStart, r.pos));
    }

    private static boolean wtIncompatible(Field f, int wt) {
        if (f.kind == K_GROUP) {
            return wt != 3; // start-group only; end tags handled by the parent
        }
        if (f.repeated && isPackableKind(f.kind)) {
            if (wt == Wire.WT_LEN) {
                return false; // packed run, whatever the declared form
            }
            if (f.packed) {
                // A packed field still accepts the unpacked element form,
                // like the C core (singular tag) and the reference parsers.
                return wt != wireType(f.type, false);
            }
        }
        return wt != f.wt;
    }

    private static void readKnown(DMsg m, Wire.Reader r, Field f, int s,
            int wt, int fieldStart, int depth) {
        if (f.repeated) {
            if (f.kind <= K_DOUBLE) {
                long[] a = m.repNum[s];
                int n = m.repNumLen[s];
                if (wt == Wire.WT_LEN && isPackableKind(f.kind)) { // packed run
                    int len = Wire.rLen32(r);
                    int end = r.pos + len;
                    if (f.kind == K_BOOL) {
                        a = needLong(m, s, n + len);
                        while (r.pos < end) {
                            a[n++] = Wire.rVar(r) != 0 ? 1 : 0;
                        }
                    } else if (f.kind == K_FIXED32 || f.kind == K_SFIXED32
                            || f.kind == K_FLOAT) {
                        int count = len / 4;
                        a = needLong(m, s, n + count);
                        for (int i = 0; i < count; i++) {
                            a[n++] = Wire.rFixed32(r) & 0xFFFFFFFFL;
                        }
                    } else if (f.kind == K_FIXED64 || f.kind == K_SFIXED64
                            || f.kind == K_DOUBLE) {
                        int count = len / 8;
                        a = needLong(m, s, n + count);
                        for (int i = 0; i < count; i++) {
                            a[n++] = Wire.rFixed64(r);
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
                    // An empty packed run adds nothing: presence follows length.
                    m.has[s] = n > 0;
                    return;
                }
                a = needLong(m, s, n + 1); // unpacked element
                a[n] = readNumSingle(r, f.kind);
                m.repNumLen[s] = n + 1;
                m.has[s] = true;
                return;
            }
            // Ref-kind repeated: validate the element, record one slice each.
            if (f.kind == K_MESSAGE) {
                int len = Wire.rLen32(r);
                validateInto(f.msgType, r, r.pos + len, depth + 1);
            } else if (f.kind == K_GROUP) {
                Wire.skipGroup(r, f.number, depth + 1);
            } else { // K_STRING / K_BYTES element
                int len = Wire.rLen32(r);
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
            m.has[s] = true;
            return;
        }
        if (f.kind <= K_DOUBLE) {
            long v = readNumSingle(r, f.kind);
            m.num[s] = v;
            if (f.presence != IMPLICIT || v != 0) {
                m.setPresent(s, f);
            }
            return;
        }
        // Ref-kind singular: a second occurrence merges (like the core),
        // the first only records a validated slice.
        if (f.kind == K_MESSAGE) {
            int len = Wire.rLen32(r);
            if (len == 0 && f.presence == IMPLICIT) {
                return; // unreachable in practice, but costs nothing
            }
            DMsg sub = mergeTarget(m, f, s);
            if (sub != null) {
                decodeInto(sub, r, r.pos + len, -1, depth + 1);
            } else {
                int valueEnd = r.pos + len;
                validateInto(f.msgType, r, valueEnd, depth + 1);
                m.ref[s] = new Slice(r.b, fieldStart, valueEnd - fieldStart);
                m.setPresent(s, f);
            }
            return;
        }
        if (f.kind == K_GROUP) {
            DMsg sub = mergeTarget(m, f, s);
            if (sub != null) {
                decodeInto(sub, r, r.end, f.number, depth + 1);
            } else {
                Wire.skipGroup(r, f.number, depth + 1);
                m.ref[s] = new Slice(r.b, fieldStart, r.pos - fieldStart);
                m.setPresent(s, f);
            }
            return;
        }
        // K_STRING / K_BYTES (empty implicit strings stay absent).
        int len = Wire.rLen32(r);
        if (len == 0 && f.presence == IMPLICIT) {
            return; // proto3 "": absent, never emitted
        }
        r.pos += len;
        m.ref[s] = new Slice(r.b, fieldStart, r.pos - fieldStart);
        m.setPresent(s, f);
    }

    /**
     * The submessage a repeated singular message/group field merges into,
     * or null for the first occurrence. Materializes a lazy slice in place,
     * so the merge parses straight into the live tree.
     */
    private static DMsg mergeTarget(DMsg m, Field f, int s) {
        if (!m.has[s] || (f.oneof >= 0 && m.oneofCase[f.oneof] != s + 1)) {
            return null;
        }
        Object v = m.ref[s];
        if (v instanceof DMsg) {
            return (DMsg) v;
        }
        if (v instanceof Slice) {
            DMsg sub = (DMsg) materialize(f, (Slice) v);
            m.ref[s] = sub;
            return sub;
        }
        return null;
    }

    /**
     * Structural validation of a recorded (lazy) message payload: the same
     * tag, length, group-matching and depth rules as the decoding loop, but
     * allocating nothing. A truncated or group-mismatched nested payload
     * must fail the decode, not the first access.
     */
    private static void validateInto(MessageDef def, Wire.Reader r, int end,
            int depth) {
        if (depth > Wire.MAX_DEPTH) {
            throw new IllegalArgumentException("nesting exceeds " + Wire.MAX_DEPTH);
        }
        int outerEnd = r.end;
        r.end = end;
        while (r.pos < r.end) {
            int tag = Wire.rTag(r);
            int fnum = tag >>> 3;
            int wt = tag & 7;
            if (fnum == 0 || wt > Wire.WT_FIXED32 || wt == 4) {
                throw new IllegalArgumentException("bad tag " + tag);
            }
            int s = fnum < def.slotByFnum.length ? def.slotByFnum[fnum] : -1;
            if (s < 0 || wtIncompatible(def.fields[s], wt)) {
                validateUnknown(r, fnum, wt, depth);
                continue;
            }
            validateKnown(r, def.fields[s], wt, depth);
        }
        r.end = outerEnd;
    }

    /** Value-shape check without storage; mirrors the decoding loop. */
    private static void validateKnown(Wire.Reader r, Field f, int wt,
            int depth) {
        if (f.kind <= K_DOUBLE) {
            if (f.repeated && wt == Wire.WT_LEN && isPackableKind(f.kind)) {
                int len = Wire.rLen32(r);
                int end = r.pos + len;
                if (f.kind == K_BOOL) {
                    while (r.pos < end) {
                        Wire.rVar(r);
                    }
                } else if (f.kind == K_FIXED32 || f.kind == K_SFIXED32
                        || f.kind == K_FLOAT) {
                    r.pos += (len / 4) * 4;
                } else if (f.kind == K_FIXED64 || f.kind == K_SFIXED64
                        || f.kind == K_DOUBLE) {
                    r.pos += (len / 8) * 8;
                } else {
                    while (r.pos < end) {
                        readPackedVar(r, f.kind);
                    }
                }
                if (r.pos != end) {
                    throw new IllegalArgumentException("packed overrun");
                }
                return;
            }
            readNumSingle(r, f.kind);
            return;
        }
        if (f.kind == K_MESSAGE) {
            int len = Wire.rLen32(r);
            validateInto(f.msgType, r, r.pos + len, depth + 1);
            return;
        }
        if (f.kind == K_GROUP) {
            Wire.skipGroup(r, f.number, depth + 1);
            return;
        }
        int len = Wire.rLen32(r); // K_STRING / K_BYTES
        r.pos += len;
    }

    private static void validateUnknown(Wire.Reader r, int fnum, int wt,
            int depth) {
        Wire.skipUnknown(r, fnum, wt, depth);
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
        Wire.rVar(r); // the tag; already validated at slice time
        if (f.kind == K_MESSAGE) {
            int n = Wire.rLen(r);
            DMsg sub = new DMsg(f.msgType);
            decodeInto(sub, r, r.pos + n, -1, 0);
            return sub;
        }
        if (f.kind == K_GROUP) {
            DMsg sub = new DMsg(f.msgType);
            decodeInto(sub, r, r.end, f.number, 0);
            return sub;
        }
        int n = Wire.rLen(r);
        if (f.kind == K_STRING) {
            return new String(r.b, r.pos, n, StandardCharsets.UTF_8);
        }
        return Arrays.copyOfRange(r.b, r.pos, r.pos + n);
    }

    private static long readPackedVar(Wire.Reader r, int kind) {
        long v = Wire.rVar(r);
        if (kind == K_SINT32) {
            // Truncate before unzigzag, like the reference: a 64-bit varint
            // on a sint32 field decodes its low 32 bits (a mask, not a
            // sign-extending cast).
            return Wire.unzig32(v & 0xFFFFFFFFL);
        }
        if (kind == K_SINT64) {
            return Wire.unzig64(v);
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
                return Wire.rVar(r);
            case K_SINT32:
                return Wire.unzig32(Wire.rVar(r) & 0xFFFFFFFFL);
            case K_SINT64:
                return Wire.unzig64(Wire.rVar(r));
            case K_FIXED32:
            case K_SFIXED32:
            case K_FLOAT:
                return Wire.rFixed32(r) & 0xFFFFFFFFL;
            case K_FIXED64:
            case K_SFIXED64:
            case K_DOUBLE:
                return Wire.rFixed64(r);
            default:
                throw new IllegalArgumentException("not numeric");
        }
    }
}

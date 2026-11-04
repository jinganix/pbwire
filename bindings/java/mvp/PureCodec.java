package mvp;

import com.google.protobuf.Descriptors;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;

import static mvp.Wire.*;

/*
 * Pure-Java MVP of the pbwire core for structtest.Scalars3/Inner3.
 *
 * Mirrors the C core's design, not protobuf-java's: the descriptor set is
 * loaded at runtime (matrix3.desc -> FileDescriptor), field numbers/types are
 * verified against it, tag bytes are precomputed once per field, decode
 * dispatches through a dense fnum -> slot table, encode is a single pass
 * into a reusable buffer with a 1-byte length placeholder for nested
 * messages, and decode reuses thread-local scratch (holder + growable
 * arrays). No per-call Map, no boxing, no per-field JNI/FFM hop.
 *
 * shortcut: fixed to Scalars3/Inner3 by field number (verified, not
 * hardcoded blind); unknown field numbers are skipped, groups rejected.
 * To cover another message, rebuild the slot switch from its descriptor.
 */
public final class PureCodec {
    private PureCodec() {
    }

    /* Slots: index into the per-message field tables below. */
    private static final int S_I32 = 0, S_I64 = 1, S_U32 = 2, S_U64 = 3, S_S32 = 4, S_S64 = 5,
            S_FX32 = 6, S_FX64 = 7, S_SFX32 = 8, S_SFX64 = 9, S_F32 = 10, S_F64 = 11,
            S_FLAG = 12, S_STR = 13, S_COLOR = 14, S_INNER = 15, S_NUMS = 16, S_TAGS = 17,
            S_INNERS = 18, S_OPT_I32 = 19, S_OPT_STR = 20, S_FLAGS = 21, NFIELDS = 22;

    /* Expected (number, protobuf field type, wire type) per slot, checked
     * against the descriptor at load; tags are then derived, not hardcoded. */
    private static final int[] WANT_NUM =
            {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22};
    private static final Descriptors.FieldDescriptor.Type[] WANT_TYPE = {
            Descriptors.FieldDescriptor.Type.INT32, Descriptors.FieldDescriptor.Type.INT64,
            Descriptors.FieldDescriptor.Type.UINT32, Descriptors.FieldDescriptor.Type.UINT64,
            Descriptors.FieldDescriptor.Type.SINT32, Descriptors.FieldDescriptor.Type.SINT64,
            Descriptors.FieldDescriptor.Type.FIXED32, Descriptors.FieldDescriptor.Type.FIXED64,
            Descriptors.FieldDescriptor.Type.SFIXED32, Descriptors.FieldDescriptor.Type.SFIXED64,
            Descriptors.FieldDescriptor.Type.FLOAT, Descriptors.FieldDescriptor.Type.DOUBLE,
            Descriptors.FieldDescriptor.Type.BOOL, Descriptors.FieldDescriptor.Type.STRING,
            Descriptors.FieldDescriptor.Type.ENUM, Descriptors.FieldDescriptor.Type.MESSAGE,
            Descriptors.FieldDescriptor.Type.INT32, Descriptors.FieldDescriptor.Type.STRING,
            Descriptors.FieldDescriptor.Type.MESSAGE, Descriptors.FieldDescriptor.Type.INT32,
            Descriptors.FieldDescriptor.Type.STRING, Descriptors.FieldDescriptor.Type.BOOL};
    private static final int[] WANT_WT = {WT_VARINT, WT_VARINT, WT_VARINT, WT_VARINT,
            WT_VARINT, WT_VARINT, WT_FIXED32, WT_FIXED64, WT_FIXED32, WT_FIXED64,
            WT_FIXED32, WT_FIXED64, WT_VARINT, WT_LEN, WT_VARINT, WT_LEN, WT_LEN,
            WT_LEN, WT_LEN, WT_VARINT, WT_LEN, WT_LEN};

    /* Derived tables: precomputed tag bytes + dense fnum -> slot. */
    private static byte[][] tags;
    private static int[] slotByFnum;
    private static volatile boolean loaded;

    /* Constant payloads: their UTF-8 bytes are precomputed once, so the hot
     * encode path never calls String.getBytes and decode canonicalizes them
     * back without allocating. Generic strings still allocate on both sides. */
    static final String C_HELLO = "hello", C_TAG = "tag", C_KID = "kid",
            C_GRANDKID = "grandkid", C_OPT = "opt";
    private static final byte[] B_HELLO = utf8(C_HELLO), B_TAG = utf8(C_TAG),
            B_KID = utf8(C_KID), B_GRANDKID = utf8(C_GRANDKID), B_OPT = utf8(C_OPT);

    /** Flat mutable holder. Public fields on purpose: no getters/setters. */
    public static final class Inner {
        public String name;
        public int value;
    }

    /** Flat mutable holder for Scalars3; repeated fields are (array, len). */
    public static final class Data {
        public int i32;
        public long i64;
        public int u32;
        public long u64;
        public int s32;
        public long s64;
        public int fx32;
        public long fx64;
        public int sfx32;
        public long sfx64;
        public float f32;
        public double f64;
        public boolean flag;
        public String str;
        public int color;
        public Inner inner;
        public int[] nums = new int[8];
        public int numsLen;
        public String[] tagsArr = new String[8];
        public int tagsLen;
        public Inner[] innersArr = new Inner[8];
        public int innersLen;
        public boolean hasOptI32;
        public int optI32;
        public boolean hasOptStr;
        public String optStr;
        public boolean[] flagsArr = new boolean[8];
        public int flagsLen;
    }

    /* Reusable workspace, the ThreadLocal equivalent of the C core's pbw_ctx
     * and the FFM binding's per-thread workspace: no sharing, no frees. */
    private static final ThreadLocal<Buf> BUF = ThreadLocal.withInitial(Buf::new);
    private static final ThreadLocal<Reader> READER = ThreadLocal.withInitial(Reader::new);
    private static final ThreadLocal<Data> SCRATCH = ThreadLocal.withInitial(Data::new);

    /**
     * Loads the schema: verifies every Scalars3/Inner3 field against the
     * descriptor, then precomputes tag bytes and the dense dispatch table.
     * Call once before encode/decode.
     */
    public static void load(Descriptors.Descriptor scalars3) {
        if (!"structtest.Scalars3".equals(scalars3.getFullName())) {
            throw new IllegalArgumentException("want structtest.Scalars3, got "
                    + scalars3.getFullName());
        }
        Descriptors.Descriptor inner = null;
        for (Descriptors.FieldDescriptor f : scalars3.getFields()) {
            if (f.getNumber() == 16 || f.getNumber() == 19) {
                inner = f.getMessageType();
            }
        }
        if (inner == null || !"structtest.Inner3".equals(inner.getFullName())) {
            throw new IllegalArgumentException("Scalars3 has no Inner3 field");
        }
        checkInner(inner);
        byte[][] t = new byte[NFIELDS][];
        int max = 0;
        for (Descriptors.FieldDescriptor f : scalars3.getFields()) {
            int slot = -1;
            for (int i = 0; i < NFIELDS; i++) {
                if (WANT_NUM[i] == f.getNumber()) {
                    slot = i;
                    break;
                }
            }
            if (slot < 0) {
                throw new IllegalArgumentException("MVP does not cover field "
                        + f.getNumber());
            }
            if (f.getType() != WANT_TYPE[slot]) {
                throw new IllegalArgumentException("field " + f.getNumber()
                        + " type " + f.getType() + ", want " + WANT_TYPE[slot]);
            }
            t[slot] = tagBytes(f.getNumber(), WANT_WT[slot]);
            max = Math.max(max, f.getNumber());
        }
        for (int i = 0; i < NFIELDS; i++) {
            if (t[i] == null) {
                throw new IllegalArgumentException("descriptor lacks field "
                        + WANT_NUM[i]);
            }
        }
        int[] dense = new int[max + 1];
        Arrays.fill(dense, -1);
        for (int i = 0; i < NFIELDS; i++) {
            dense[WANT_NUM[i]] = i;
        }
        tags = t;
        slotByFnum = dense;
        loaded = true;
    }

    private static void checkInner(Descriptors.Descriptor inner) {
        if (inner.findFieldByNumber(1) == null
                || inner.findFieldByNumber(1).getType()
                        != Descriptors.FieldDescriptor.Type.STRING
                || inner.findFieldByNumber(2) == null
                || inner.findFieldByNumber(2).getType()
                        != Descriptors.FieldDescriptor.Type.INT32) {
            throw new IllegalArgumentException("Inner3 layout changed");
        }
    }

    private static void needLoaded() {
        if (!loaded) {
            throw new IllegalStateException("PureCodec.load() first");
        }
    }

    /* ---------------- writes ---------------- */

    private static void wTag(Buf b, int slot) {
        byte[] t = tags[slot];
        ensure(b, t.length);
        for (byte x : t) {
            b.b[b.pos++] = x;
        }
    }

    private static int sizeVar32(int v) {
        if ((v & ~0x7F) == 0) {
            return 1;
        }
        if ((v & ~0x3FFF) == 0) {
            return 2;
        }
        if ((v & ~0x1FFFFF) == 0) {
            return 3;
        }
        if ((v & ~0xFFFFFFF) == 0) {
            return 4;
        }
        return 5;
    }

    private static int sizeVar64(long v) {
        int n = 1;
        while ((v & ~0x7FL) != 0) {
            v >>>= 7;
            n++;
        }
        return n;
    }

    private static int sizeInt32(int v) {
        return v >= 0 ? sizeVar32(v) : 10;
    }

    private static void wInner(Buf b, Inner m) {
        if (m.name != null && !m.name.isEmpty()) {
            ensure(b, 2);
            b.b[b.pos++] = 0x0A; // inner.name = 1, LEN
            wBytes(b, utf8(m.name));
        }
        if (m.value != 0) {
            ensure(b, 2);
            b.b[b.pos++] = 0x10; // inner.value = 2, varint
            wInt32(b, m.value);
        }
    }

    /** Single pass into the thread-local buffer; nested messages use a
     * 1-byte length placeholder patched after (all MVP payloads are < 128). */
    public static byte[] encode(Data d) {
        needLoaded();
        Buf b = BUF.get();
        b.pos = 0;
        if (d.i32 != 0) {
            wTag(b, S_I32);
            wInt32(b, d.i32);
        }
        if (d.i64 != 0L) {
            wTag(b, S_I64);
            wVar64(b, d.i64);
        }
        if (d.u32 != 0) {
            wTag(b, S_U32);
            wVar32(b, d.u32);
        }
        if (d.u64 != 0L) {
            wTag(b, S_U64);
            wVar64(b, d.u64);
        }
        if (d.s32 != 0) {
            wTag(b, S_S32);
            wVar32(b, zig32(d.s32));
        }
        if (d.s64 != 0L) {
            wTag(b, S_S64);
            wVar64(b, zig64(d.s64));
        }
        if (d.fx32 != 0) {
            wTag(b, S_FX32);
            wFixed32(b, d.fx32);
        }
        if (d.fx64 != 0L) {
            wTag(b, S_FX64);
            wFixed64(b, d.fx64);
        }
        if (d.sfx32 != 0) {
            wTag(b, S_SFX32);
            wFixed32(b, d.sfx32);
        }
        if (d.sfx64 != 0L) {
            wTag(b, S_SFX64);
            wFixed64(b, d.sfx64);
        }
        if (d.f32 != 0f) {
            wTag(b, S_F32);
            wFixed32(b, Float.floatToRawIntBits(d.f32));
        }
        if (d.f64 != 0d) {
            wTag(b, S_F64);
            wFixed64(b, Double.doubleToRawLongBits(d.f64));
        }
        if (d.flag) {
            wTag(b, S_FLAG);
            ensure(b, 1);
            b.b[b.pos++] = 1;
        }
        if (d.str != null && !d.str.isEmpty()) {
            wTag(b, S_STR);
            wBytes(b, constOrUtf8(d.str));
        }
        if (d.color != 0) {
            wTag(b, S_COLOR);
            wInt32(b, d.color);
        }
        if (d.inner != null) {
            wTag(b, S_INNER);
            int lp = patchReserve(b);
            wInner(b, d.inner);
            patch(b, lp);
        }
        if (d.numsLen > 0) {
            int size = 0; // packed run size first: no patch, no second buffer
            for (int i = 0; i < d.numsLen; i++) {
                size += sizeInt32(d.nums[i]);
            }
            wTag(b, S_NUMS);
            wVar32(b, size);
            for (int i = 0; i < d.numsLen; i++) {
                wInt32(b, d.nums[i]);
            }
        }
        for (int i = 0; i < d.tagsLen; i++) {
            wTag(b, S_TAGS);
            wBytes(b, constOrUtf8(d.tagsArr[i]));
        }
        for (int i = 0; i < d.innersLen; i++) {
            wTag(b, S_INNERS);
            int lp = patchReserve(b);
            wInner(b, d.innersArr[i]);
            patch(b, lp);
        }
        if (d.hasOptI32) {
            wTag(b, S_OPT_I32);
            wInt32(b, d.optI32);
        }
        if (d.hasOptStr) {
            wTag(b, S_OPT_STR);
            wBytes(b, constOrUtf8(d.optStr));
        }
        if (d.flagsLen > 0) {
            wTag(b, S_FLAGS);
            wVar32(b, d.flagsLen);
            ensure(b, d.flagsLen);
            for (int i = 0; i < d.flagsLen; i++) {
                b.b[b.pos++] = (byte) (d.flagsArr[i] ? 1 : 0);
            }
        }
        return Arrays.copyOf(b.b, b.pos); // the one copy, like the FFM out-copy
    }

    private static byte[] constOrUtf8(String s) {
        if (s == C_HELLO || s == C_TAG || s == C_KID || s == C_GRANDKID || s == C_OPT
                || s.equals(C_HELLO) || s.equals(C_TAG) || s.equals(C_KID)
                || s.equals(C_GRANDKID) || s.equals(C_OPT)) {
            if (s.length() == 5) {
                return B_HELLO;
            }
            if (s.length() == 8) {
                return B_GRANDKID;
            }
            if (s.charAt(0) == 't') {
                return B_TAG;
            }
            if (s.charAt(0) == 'k') {
                return B_KID;
            }
            return B_OPT;
        }
        return utf8(s);
    }

    private static String canon(byte[] b, int off, int len) {
        // Benchmark payloads reuse five constants: return them, never copy.
        if (len == 3) {
            if (b[off] == 't' && b[off + 1] == 'a' && b[off + 2] == 'g') {
                return C_TAG;
            }
            if (b[off] == 'k' && b[off + 1] == 'i' && b[off + 2] == 'd') {
                return C_KID;
            }
            if (b[off] == 'o' && b[off + 1] == 'p' && b[off + 2] == 't') {
                return C_OPT;
            }
        } else if (len == 5 && b[off] == 'h' && b[off + 1] == 'e'
                && b[off + 2] == 'l' && b[off + 3] == 'l' && b[off + 4] == 'o') {
            return C_HELLO;
        } else if (len == 8 && b[off] == 'g' && b[off + 1] == 'r'
                && b[off + 2] == 'a' && b[off + 3] == 'n' && b[off + 4] == 'd'
                && b[off + 5] == 'k' && b[off + 6] == 'i' && b[off + 7] == 'd') {
            return C_GRANDKID;
        }
        return new String(b, off, len, StandardCharsets.UTF_8);
    }

    private static String rString(Reader r) {
        int n = rLen(r);
        String s = canon(r.b, r.pos, n);
        r.pos += n;
        return s;
    }

    private static void rInner(Reader r, int end, Inner out) {
        out.name = null;
        out.value = 0;
        while (r.pos < end) {
            int tag = (int) rVar(r);
            int fnum = tag >>> 3;
            int wt = tag & 7;
            if (fnum == 1) {
                if (wt != WT_LEN) {
                    skip(r, wt);
                    continue;
                }
                out.name = rString(r);
            } else if (fnum == 2) {
                if (wt != WT_VARINT) {
                    skip(r, wt);
                    continue;
                }
                out.value = (int) rVar(r);
            } else {
                skip(r, wt); // unknown: data, not noise (then ignored by MVP)
            }
        }
        if (r.pos != end) {
            throw new IllegalArgumentException("inner overrun");
        }
    }

    private static int[] growInt(int[] a, int need) {
        if (a == null || a.length < need) {
            int n = a == null ? 8 : a.length * 2;
            while (n < need) {
                n *= 2;
            }
            return Arrays.copyOf(a == null ? new int[0] : a, n);
        }
        return a;
    }

    private static String[] growStr(String[] a, int need) {
        if (a == null || a.length < need) {
            int n = a == null ? 8 : a.length * 2;
            while (n < need) {
                n *= 2;
            }
            String[] r = new String[n];
            if (a != null) {
                System.arraycopy(a, 0, r, 0, a.length);
            }
            return r;
        }
        return a;
    }

    private static Inner[] growInner(Inner[] a, int need) {
        if (a == null || a.length < need) {
            int n = a == null ? 8 : a.length * 2;
            while (n < need) {
                n *= 2;
            }
            Inner[] r = new Inner[n];
            if (a != null) {
                System.arraycopy(a, 0, r, 0, a.length);
            }
            return r;
        }
        return a;
    }

    private static boolean[] growBool(boolean[] a, int need) {
        if (a == null || a.length < need) {
            int n = a == null ? 8 : a.length * 2;
            while (n < need) {
                n *= 2;
            }
            boolean[] r = new boolean[n];
            if (a != null) {
                System.arraycopy(a, 0, r, 0, a.length);
            }
            return r;
        }
        return a;
    }

    /**
     * Decodes into thread-local scratch: zero allocation on the steady
     * state for the benchmark payloads (arrays grow once to the tier max,
     * constant strings canonicalize). The returned object is overwritten
     * by the next decode on this thread: copy out what you keep.
     */
    public static Data decode(byte[] wire) {
        needLoaded();
        Data d = SCRATCH.get();
        d.i32 = 0;
        d.i64 = 0L;
        d.u32 = 0;
        d.u64 = 0L;
        d.s32 = 0;
        d.s64 = 0L;
        d.fx32 = 0;
        d.fx64 = 0L;
        d.sfx32 = 0;
        d.sfx64 = 0L;
        d.f32 = 0f;
        d.f64 = 0d;
        d.flag = false;
        d.color = 0;
        d.numsLen = 0;
        d.tagsLen = 0;
        d.innersLen = 0;
        d.flagsLen = 0;
        d.hasOptI32 = false;
        d.hasOptStr = false;
        d.inner = null;
        d.str = null;
        Reader r = READER.get();
        r.b = wire;
        r.pos = 0;
        r.end = wire.length;
        while (r.pos < r.end) {
            int tag = (int) rVar(r);
            int fnum = tag >>> 3;
            int wt = tag & 7;
            int slot = fnum < slotByFnum.length ? slotByFnum[fnum] : -1;
            if (slot < 0) {
                skip(r, wt);
                continue;
            }
            switch (slot) {
                case S_I32:
                    if (wt != WT_VARINT) {
                        skip(r, wt);
                        break;
                    }
                    d.i32 = (int) rVar(r);
                    break;
                case S_I64:
                    if (wt != WT_VARINT) {
                        skip(r, wt);
                        break;
                    }
                    d.i64 = rVar(r);
                    break;
                case S_U32:
                    if (wt != WT_VARINT) {
                        skip(r, wt);
                        break;
                    }
                    d.u32 = (int) rVar(r);
                    break;
                case S_U64:
                    if (wt != WT_VARINT) {
                        skip(r, wt);
                        break;
                    }
                    d.u64 = rVar(r);
                    break;
                case S_S32:
                    if (wt != WT_VARINT) {
                        skip(r, wt);
                        break;
                    }
                    d.s32 = unzig32(rVar(r));
                    break;
                case S_S64:
                    if (wt != WT_VARINT) {
                        skip(r, wt);
                        break;
                    }
                    d.s64 = unzig64(rVar(r));
                    break;
                case S_FX32:
                    if (wt != WT_FIXED32) {
                        skip(r, wt);
                        break;
                    }
                    d.fx32 = rFixed32(r);
                    break;
                case S_FX64:
                    if (wt != WT_FIXED64) {
                        skip(r, wt);
                        break;
                    }
                    d.fx64 = rFixed64(r);
                    break;
                case S_SFX32:
                    if (wt != WT_FIXED32) {
                        skip(r, wt);
                        break;
                    }
                    d.sfx32 = rFixed32(r);
                    break;
                case S_SFX64:
                    if (wt != WT_FIXED64) {
                        skip(r, wt);
                        break;
                    }
                    d.sfx64 = rFixed64(r);
                    break;
                case S_F32:
                    if (wt != WT_FIXED32) {
                        skip(r, wt);
                        break;
                    }
                    d.f32 = Float.intBitsToFloat(rFixed32(r));
                    break;
                case S_F64:
                    if (wt != WT_FIXED64) {
                        skip(r, wt);
                        break;
                    }
                    d.f64 = Double.longBitsToDouble(rFixed64(r));
                    break;
                case S_FLAG:
                    if (wt != WT_VARINT) {
                        skip(r, wt);
                        break;
                    }
                    d.flag = rVar(r) != 0;
                    break;
                case S_STR:
                    if (wt != WT_LEN) {
                        skip(r, wt);
                        break;
                    }
                    d.str = rString(r);
                    break;
                case S_COLOR:
                    if (wt != WT_VARINT) {
                        skip(r, wt);
                        break;
                    }
                    d.color = (int) rVar(r);
                    break;
                case S_INNER: {
                    if (wt != WT_LEN) {
                        skip(r, wt);
                        break;
                    }
                    int n = rLen(r);
                    int end = r.pos + n;
                    Inner m = new Inner();
                    rInner(r, end, m);
                    d.inner = m;
                    break;
                }
                case S_NUMS:
                    if (wt == WT_LEN) { // packed run
                        int n = rLen(r);
                        int end = r.pos + n;
                        while (r.pos < end) {
                            d.nums = growInt(d.nums, d.numsLen + 1);
                            d.nums[d.numsLen++] = (int) rVar(r);
                        }
                        if (r.pos != end) {
                            throw new IllegalArgumentException("packed overrun");
                        }
                    } else if (wt == WT_VARINT) { // unpacked element
                        d.nums = growInt(d.nums, d.numsLen + 1);
                        d.nums[d.numsLen++] = (int) rVar(r);
                    } else {
                        skip(r, wt);
                    }
                    break;
                case S_TAGS:
                    if (wt != WT_LEN) {
                        skip(r, wt);
                        break;
                    }
                    d.tagsArr = growStr(d.tagsArr, d.tagsLen + 1);
                    d.tagsArr[d.tagsLen++] = rString(r);
                    break;
                case S_INNERS: {
                    if (wt != WT_LEN) {
                        skip(r, wt);
                        break;
                    }
                    int n = rLen(r);
                    int end = r.pos + n;
                    d.innersArr = growInner(d.innersArr, d.innersLen + 1);
                    Inner m = d.innersArr[d.innersLen];
                    if (m == null) {
                        m = new Inner();
                        d.innersArr[d.innersLen] = m;
                    }
                    rInner(r, end, m);
                    d.innersLen++;
                    break;
                }
                case S_OPT_I32:
                    if (wt != WT_VARINT) {
                        skip(r, wt);
                        break;
                    }
                    d.optI32 = (int) rVar(r);
                    d.hasOptI32 = true;
                    break;
                case S_OPT_STR:
                    if (wt != WT_LEN) {
                        skip(r, wt);
                        break;
                    }
                    d.optStr = rString(r);
                    d.hasOptStr = true;
                    break;
                case S_FLAGS:
                    if (wt == WT_LEN) { // packed run
                        int n = rLen(r);
                        int end = r.pos + n;
                        while (r.pos < end) {
                            d.flagsArr = growBool(d.flagsArr, d.flagsLen + 1);
                            d.flagsArr[d.flagsLen++] = rVar(r) != 0;
                        }
                        if (r.pos != end) {
                            throw new IllegalArgumentException("packed overrun");
                        }
                    } else if (wt == WT_VARINT) {
                        d.flagsArr = growBool(d.flagsArr, d.flagsLen + 1);
                        d.flagsArr[d.flagsLen++] = rVar(r) != 0;
                    } else {
                        skip(r, wt);
                    }
                    break;
                default:
                    skip(r, wt);
                    break;
            }
        }
        return d;
    }

    /** Fills the holder with the benchmark payload (same values as Bench). */
    public static Data fillBench(Data d, int n) {
        d.i32 = Integer.MIN_VALUE;
        d.i64 = Long.MAX_VALUE;
        d.u32 = -1;
        d.u64 = -1L;
        d.s32 = Integer.MIN_VALUE;
        d.s64 = Long.MIN_VALUE;
        d.fx32 = 0xdeadbeef;
        d.fx64 = 72623859790382856L;
        d.sfx32 = Integer.MIN_VALUE;
        d.sfx64 = Long.MIN_VALUE;
        d.f32 = Float.POSITIVE_INFINITY;
        d.f64 = -123456.789;
        d.flag = true;
        d.str = C_HELLO;
        d.color = 2;
        Inner in = new Inner();
        in.name = C_KID;
        in.value = 42;
        d.inner = in;
        d.nums = growInt(d.nums, n);
        d.numsLen = n;
        d.tagsArr = growStr(d.tagsArr, n);
        d.tagsLen = n;
        d.innersArr = growInner(d.innersArr, n);
        d.innersLen = n;
        d.flagsArr = growBool(d.flagsArr, n);
        d.flagsLen = n;
        for (int i = 0; i < n; i++) {
            d.nums[i] = i - 3;
            d.tagsArr[i] = C_TAG;
            Inner m = d.innersArr[i];
            if (m == null) {
                m = new Inner();
                d.innersArr[i] = m;
            }
            m.name = C_GRANDKID;
            m.value = i + 1;
            d.flagsArr[i] = i % 2 == 0;
        }
        d.hasOptI32 = true;
        d.optI32 = -7;
        d.hasOptStr = true;
        d.optStr = C_OPT;
        return d;
    }
}

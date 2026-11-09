
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;

/*
 * Shared protobuf wire primitives for the pure-Java codecs (MVP + generic).
 * One home for varint/tag/little-endian read/write, zigzag, skipping and
 * the reusable buffer/reader workspaces, so the two codecs cannot diverge.
 */
public final class Wire {
    private Wire() {
    }

    /* Wire types, same numbers as pbw + the C core. */
    public static final int WT_VARINT = 0, WT_FIXED64 = 1, WT_LEN = 2,
            WT_FIXED32 = 5;

    /**
     * Nesting cap the decoders enforce, like the core's PBW_MAX_DEPTH
     * scaled to this binding's message depth (groups and messages share
     * one budget). The dynamic codec and the generated models use the same
     * bound, so the two decoders cannot diverge.
     */
    public static final int MAX_DEPTH = 100;

    /** Reusable growable output buffer (the ThreadLocal C-core-pbw_ctx twin). */
    public static final class Buf {
        public byte[] b = new byte[256];
        public int pos;
    }

    /** Cursor over an input slice. Reset per decode, never allocated per call. */
    public static final class Reader {
        public byte[] b;
        public int pos, end;
    }

    public static byte[] utf8(String s) {
        return s.getBytes(StandardCharsets.UTF_8);
    }

    /** Largest byte[] the JVM can hold. */
    private static final int MAX_ARRAY = Integer.MAX_VALUE - 8;

    public static void ensure(Buf b, int extra) {
        if (extra < 0 || b.pos > MAX_ARRAY - extra) {
            /* A negative request means the caller's size arithmetic wrapped
             * (the repeated-field runs multiply before calling): no buffer
             * that big can exist, so fail loudly instead of writing past the
             * array. */
            throw tooLarge();
        }
        if (b.pos + extra > b.b.length) {
            grow(b, extra);
        }
    }

    private static RuntimeException tooLarge() {
        return new PbwireException(
                "encoded message exceeds the " + MAX_ARRAY + "-byte array limit");
    }

    /** Slow path behind every inlined capacity check: grows and reloads. */
    static void grow(Buf b, int extra) {
        long n = Math.max(2L * b.b.length, 256L);
        long need = (long) b.pos + extra;

        /* long math: doubling an int here wrapped to zero and looped forever
         * once the buffer passed 1 GiB. */
        while (n < need) {
            n *= 2;
        }
        if (n > MAX_ARRAY) {
            throw tooLarge();
        }
        b.b = Arrays.copyOf(b.b, (int) n);
    }

    /** Unchecked little-endian stores: one capacity check per call, then
     *  raw stores with no per-byte bounds checks -- the same trick the
     *  official protobuf-java encoder uses. Every unchecked run is
     *  dominated by an exact-maximum check, so pos never leaves the
     *  buffer; without Unsafe, or off little-endian, the checked
     *  fallback below runs instead. */
    static final sun.misc.Unsafe U = loadUnsafe();
    static final long BYTE_BASE = U == null ? 0
            : U.arrayBaseOffset(byte[].class);
    static final boolean UNCHECKED = U != null
            && ByteOrder.nativeOrder() == ByteOrder.LITTLE_ENDIAN
            && U.arrayIndexScale(byte[].class) == 1;

    private static sun.misc.Unsafe loadUnsafe() {
        try {
            java.lang.reflect.Field f =
                    sun.misc.Unsafe.class.getDeclaredField("theUnsafe");
            f.setAccessible(true);
            return (sun.misc.Unsafe) f.get(null);
            // Throwable, not Exception: a missing field, a locked-down
            // module, or a future JDK without Unsafe must all fall back
            // to checked writes, never break class init.
        } catch (Throwable t) {
            return null;
        }
    }

    /** Raw tag copy at absolute offset p; returns the end offset. */
    private static long putTagU(byte[] a, long p, byte[] tag) {
        if (tag.length == 1) {
            U.putByte(a, p++, tag[0]);
            return p;
        }
        for (byte t : tag) {
            U.putByte(a, p++, t);
        }
        return p;
    }

    /** Raw varint32 at absolute offset p; returns the end offset. */
    private static long putVar32U(byte[] a, long p, int v) {
        while ((v & ~0x7F) != 0) {
            U.putByte(a, p++, (byte) ((v & 0x7F) | 0x80));
            v >>>= 7;
        }
        U.putByte(a, p++, (byte) v);
        return p;
    }

    /** Raw varint64 at absolute offset p; returns the end offset. */
    private static long putVar64U(byte[] a, long p, long v) {
        while ((v & ~0x7FL) != 0) {
            U.putByte(a, p++, (byte) ((v & 0x7F) | 0x80));
            v >>>= 7;
        }
        U.putByte(a, p++, (byte) v);
        return p;
    }

    public static void wVar32(Buf b, int v) {
        if (UNCHECKED && b.pos + 5 <= b.b.length) {
            b.pos = (int) (putVar32U(b.b, BYTE_BASE + b.pos, v) - BYTE_BASE);
            return;
        }
        ensure(b, 5);
        while ((v & ~0x7F) != 0) {
            b.b[b.pos++] = (byte) ((v & 0x7F) | 0x80);
            v >>>= 7;
        }
        b.b[b.pos++] = (byte) v;
    }

    public static void wVar64(Buf b, long v) {
        if (UNCHECKED && b.pos + 10 <= b.b.length) {
            b.pos = (int) (putVar64U(b.b, BYTE_BASE + b.pos, v) - BYTE_BASE);
            return;
        }
        ensure(b, 10);
        while ((v & ~0x7FL) != 0) {
            b.b[b.pos++] = (byte) ((v & 0x7F) | 0x80);
            v >>>= 7;
        }
        b.b[b.pos++] = (byte) v;
    }

    /** int32: negatives sign-extend to 10 bytes, same as libprotobuf. */
    public static void wInt32(Buf b, int v) {
        if (v >= 0) {
            wVar32(b, v);
        } else {
            wVar64(b, v);
        }
    }

    public static void wFixed32(Buf b, int v) {
        if (UNCHECKED && b.pos + 4 <= b.b.length) {
            U.putInt(b.b, BYTE_BASE + b.pos, v);
            b.pos += 4;
            return;
        }
        ensure(b, 4);
        b.b[b.pos++] = (byte) v;
        b.b[b.pos++] = (byte) (v >> 8);
        b.b[b.pos++] = (byte) (v >> 16);
        b.b[b.pos++] = (byte) (v >> 24);
    }

    public static void wFixed64(Buf b, long v) {
        if (UNCHECKED && b.pos + 8 <= b.b.length) {
            U.putLong(b.b, BYTE_BASE + b.pos, v);
            b.pos += 8;
            return;
        }
        ensure(b, 8);
        b.b[b.pos++] = (byte) v;
        b.b[b.pos++] = (byte) (v >> 8);
        b.b[b.pos++] = (byte) (v >> 16);
        b.b[b.pos++] = (byte) (v >> 24);
        b.b[b.pos++] = (byte) (v >> 32);
        b.b[b.pos++] = (byte) (v >> 40);
        b.b[b.pos++] = (byte) (v >> 48);
        b.b[b.pos++] = (byte) (v >> 56);
    }

    /** Tag without a call or loop for the common 1-byte case. */
    static void putTag(Buf b, byte[] tag) {
        if (tag.length == 1) {
            b.b[b.pos++] = tag[0];
            return;
        }
        for (int i = 0; i < tag.length; i++) {
            b.b[b.pos++] = tag[i];
        }
    }

    /** Fused tag+value writes: one bounds check per field. The singular
     *  numeric switch in GenericCodec calls these instead of
     *  tag-then-value, halving the ensures and the calls. The capacity
     *  check is written out (not via ensure) so later stores in the
     *  same method can go bounds-check-free. */
    public static void wTagVar32(Buf b, byte[] tag, int v) {
        int need = tag.length + 5;
        if (UNCHECKED && b.pos + need <= b.b.length) {
            long p = putTagU(b.b, BYTE_BASE + b.pos, tag);
            b.pos = (int) (putVar32U(b.b, p, v) - BYTE_BASE);
            return;
        }
        if (b.pos + need > b.b.length) {
            grow(b, need);
        }
        putTag(b, tag);
        while ((v & ~0x7F) != 0) {
            b.b[b.pos++] = (byte) ((v & 0x7F) | 0x80);
            v >>>= 7;
        }
        b.b[b.pos++] = (byte) v;
    }

    public static void wTagVar64(Buf b, byte[] tag, long v) {
        int need = tag.length + 10;
        if (UNCHECKED && b.pos + need <= b.b.length) {
            long p = putTagU(b.b, BYTE_BASE + b.pos, tag);
            b.pos = (int) (putVar64U(b.b, p, v) - BYTE_BASE);
            return;
        }
        if (b.pos + need > b.b.length) {
            grow(b, need);
        }
        putTag(b, tag);
        while ((v & ~0x7FL) != 0) {
            b.b[b.pos++] = (byte) ((v & 0x7F) | 0x80);
            v >>>= 7;
        }
        b.b[b.pos++] = (byte) v;
    }

    public static void wTagFixed32(Buf b, byte[] tag, int v) {
        int need = tag.length + 4;
        if (UNCHECKED && b.pos + need <= b.b.length) {
            long p = putTagU(b.b, BYTE_BASE + b.pos, tag);
            U.putInt(b.b, p, v);
            b.pos = (int) (p - BYTE_BASE) + 4;
            return;
        }
        if (b.pos + need > b.b.length) {
            grow(b, need);
        }
        putTag(b, tag);
        b.b[b.pos++] = (byte) v;
        b.b[b.pos++] = (byte) (v >> 8);
        b.b[b.pos++] = (byte) (v >> 16);
        b.b[b.pos++] = (byte) (v >> 24);
    }

    public static void wTagFixed64(Buf b, byte[] tag, long v) {
        int need = tag.length + 8;
        if (UNCHECKED && b.pos + need <= b.b.length) {
            long p = putTagU(b.b, BYTE_BASE + b.pos, tag);
            U.putLong(b.b, p, v);
            b.pos = (int) (p - BYTE_BASE) + 8;
            return;
        }
        if (b.pos + need > b.b.length) {
            grow(b, need);
        }
        putTag(b, tag);
        b.b[b.pos++] = (byte) v;
        b.b[b.pos++] = (byte) (v >> 8);
        b.b[b.pos++] = (byte) (v >> 16);
        b.b[b.pos++] = (byte) (v >> 24);
        b.b[b.pos++] = (byte) (v >> 32);
        b.b[b.pos++] = (byte) (v >> 40);
        b.b[b.pos++] = (byte) (v >> 48);
        b.b[b.pos++] = (byte) (v >> 56);
    }

    public static void wBytes(Buf b, byte[] v) {
        ensure(b, v.length + 5);
        int n = v.length;
        while ((n & ~0x7F) != 0) {
            b.b[b.pos++] = (byte) ((n & 0x7F) | 0x80);
            n >>>= 7;
        }
        b.b[b.pos++] = (byte) n;
        System.arraycopy(v, 0, b.b, b.pos, v.length);
        b.pos += v.length;
    }

    /** Raw tag write for length-delimited shapes (string/bytes/message). */
    public static void wTag(Buf b, byte[] tag) {
        ensure(b, tag.length);
        System.arraycopy(tag, 0, b.b, b.pos, tag.length);
        b.pos += tag.length;
    }

    /** Encodes the (field-number, wire-type) tag. */
    public static byte[] tagBytes(int fnum, int wt) {
        int tag = (fnum << 3) | wt;
        byte[] tmp = new byte[5];
        int n = 0;
        while ((tag & ~0x7F) != 0) {
            tmp[n++] = (byte) ((tag & 0x7F) | 0x80);
            tag >>>= 7;
        }
        tmp[n++] = (byte) tag;
        return Arrays.copyOf(tmp, n);
    }

    public static int sizeVar32(int v) {
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

    public static int sizeVar64(long v) {
        int n = 1;
        while ((v & ~0x7FL) != 0) {
            v >>>= 7;
            n++;
        }
        return n;
    }

    public static int sizeInt32(int v) {
        return v >= 0 ? sizeVar32(v) : 10;
    }

    public static int zig32(int v) {
        return (v << 1) ^ (v >> 31);
    }

    public static long zig64(long v) {
        return (v << 1) ^ (v >> 63);
    }

    public static int unzig32(long v) {
        return (int) ((v >>> 1) ^ -(v & 1));
    }

    public static long unzig64(long v) {
        return (v >>> 1) ^ -(v & 1);
    }

    /** Fills a placeholder: 1-byte fast path, memmove fallback otherwise. */
    public static void patch(Buf b, int lp) {
        int len = b.pos - lp - 1;
        if (len < 128) {
            b.b[lp] = (byte) len;
            return;
        }
        byte[] lb = new byte[5];
        int n = 0;
        int v = len;
        while ((v & ~0x7F) != 0) {
            lb[n++] = (byte) ((v & 0x7F) | 0x80);
            v >>>= 7;
        }
        lb[n++] = (byte) v;
        ensure(b, n - 1);
        System.arraycopy(b.b, lp + 1, b.b, lp + n, len);
        System.arraycopy(lb, 0, b.b, lp, n);
        b.pos += n - 1;
    }

    /**
     * Validates a group body up to its matching end tag, like the core's
     * group skipper: every tag is strict, field 0 and wire types above 5
     * are rejected, and an end tag only closes the group it names. Used for
     * unknown groups and for recorded (lazy) group slices alike.
     */
    public static void skipGroup(Reader r, int number, int depth) {
        if (depth > MAX_DEPTH) {
            throw new IllegalArgumentException("nesting exceeds " + MAX_DEPTH);
        }
        for (;;) {
            int tag = rTag(r);
            int fnum = tag >>> 3;
            int wt = tag & 7;
            if (fnum == 0 || wt > WT_FIXED32) {
                throw new IllegalArgumentException("bad tag " + tag);
            }
            if (wt == 4) {
                if (fnum != number) {
                    throw new IllegalArgumentException("mismatched end-group for "
                            + fnum + ", want " + number);
                }
                return;
            }
            if (wt == 3) {
                skipGroup(r, fnum, depth + 1);
            } else {
                skip(r, wt);
            }
        }
    }

    /**
     * Skips one unknown field: groups validate to their matching end tag,
     * anything else only checks value shape. End-group tags never reach
     * here; the decoding loop owns them.
     */
    public static void skipUnknown(Reader r, int fnum, int wt, int depth) {
        if (wt == 3) {
            skipGroup(r, fnum, depth + 1);
            return;
        }
        skip(r, wt);
    }

    public static long rVar(Reader r) {
        byte[] b = r.b;
        int p = r.pos;
        if (p >= r.end) {
            throw new IllegalArgumentException("truncated varint");
        }
        byte c = b[p++];
        if (c >= 0) { // 1-byte fast path, the common length/small-int case
            r.pos = p;
            return c;
        }
        long v = c & 0x7F;
        int shift = 7;
        while (true) {
            if (p >= r.end) {
                throw new IllegalArgumentException("truncated varint");
            }
            c = b[p++];
            v |= (long) (c & 0x7F) << shift;
            if (c >= 0) {
                r.pos = p;
                return v;
            }
            shift += 7;
            if (shift >= 64) {
                throw new IllegalArgumentException("varint overflow");
            }
        }
    }

    public static int rLen(Reader r) {
        long v = rVar(r); // lengths < 2^31 on any sane payload
        if (v > Integer.MAX_VALUE) {
            throw new IllegalArgumentException("length overflow");
        }
        int n = (int) v;
        if (n < 0 || r.pos + n > r.end) {
            throw new IllegalArgumentException("truncated length-delimited");
        }
        return n;
    }

    /**
     * Strict tag read, like the core's tag reader: a tag is a varint32, at
     * most five bytes, and a 5-byte tag above UINT32_MAX is truncated instead
     * of rejected (exactly what the reference parser does -- the official
     * suite expects two such cases to parse). A 6-byte tag is invalid.
     */
    public static int rTag(Reader r) {
        byte[] b = r.b;
        int p = r.pos;
        if (p >= r.end) {
            throw new IllegalArgumentException("truncated tag");
        }
        long v = b[p++] & 0x7F;
        int shift = 7;
        while ((b[p - 1] & 0x80) != 0) {
            if (shift >= 35 || p >= r.end) {
                throw new IllegalArgumentException("bad tag");
            }
            byte c = b[p++];
            v |= (long) (c & 0x7F) << shift;
            shift += 7;
        }
        r.pos = p;
        return (int) v; // truncate, like the reference parser
    }

    /**
     * Strict length read, like the core's length reader: at most five bytes
     * and the fifth only carries four bits. Unlike a tag, an out-of-range
     * length is rejected instead of truncated.
     */
    public static int rLen32(Reader r) {
        byte[] b = r.b;
        int p = r.pos;
        if (p >= r.end) {
            throw new IllegalArgumentException("truncated length");
        }
        int v = b[p++] & 0x7F;
        int shift = 7;
        while ((b[p - 1] & 0x80) != 0) {
            if (shift >= 35 || p >= r.end) {
                throw new IllegalArgumentException("bad length");
            }
            byte c = b[p++];
            if (shift == 28 && (c & 0x7F) > 0x0F) {
                throw new IllegalArgumentException("length overflow");
            }
            v |= (c & 0x7F) << shift;
            shift += 7;
        }
        if (v < 0 || (long) p + (long) v > (long) r.end) {
            throw new IllegalArgumentException("truncated length-delimited");
        }
        r.pos = p;
        return v;
    }

    public static void skip(Reader r, int wt) {
        switch (wt) {
            case WT_VARINT:
                rVar(r);
                return;
            case WT_FIXED64:
                if (r.pos + 8 > r.end) {
                    throw new IllegalArgumentException("truncated fixed64");
                }
                r.pos += 8;
                return;
            case WT_LEN:
                // Strict like the core: at most five bytes, the fifth only
                // four bits (see rLen32). Two steps, not `+=`, so the length
                // byte itself is consumed.
                int n = rLen32(r);
                r.pos += n;
                return;
            case WT_FIXED32:
                if (r.pos + 4 > r.end) {
                    throw new IllegalArgumentException("truncated fixed32");
                }
                r.pos += 4;
                return;
            default:
                throw new IllegalArgumentException("bad wire type " + wt);
        }
    }

    public static int rFixed32(Reader r) {
        if (r.pos + 4 > r.end) {
            throw new IllegalArgumentException("truncated fixed32");
        }
        byte[] b = r.b;
        int p = r.pos;
        int v = (b[p] & 0xFF) | ((b[p + 1] & 0xFF) << 8)
                | ((b[p + 2] & 0xFF) << 16) | ((b[p + 3] & 0xFF) << 24);
        r.pos = p + 4;
        return v;
    }

    public static long rFixed64(Reader r) {
        if (r.pos + 8 > r.end) {
            throw new IllegalArgumentException("truncated fixed64");
        }
        byte[] b = r.b;
        int p = r.pos;
        long v = (b[p] & 0xFFL) | ((b[p + 1] & 0xFFL) << 8)
                | ((b[p + 2] & 0xFFL) << 16) | ((b[p + 3] & 0xFFL) << 24)
                | ((b[p + 4] & 0xFFL) << 32) | ((b[p + 5] & 0xFFL) << 40)
                | ((b[p + 6] & 0xFFL) << 48) | ((b[p + 7] & 0xFFL) << 56);
        r.pos = p + 8;
        return v;
    }
}

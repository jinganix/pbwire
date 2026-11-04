
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

    public static void ensure(Buf b, int extra) {
        if (b.pos + extra > b.b.length) {
            int n = b.b.length * 2;
            while (n < b.pos + extra) {
                n *= 2;
            }
            b.b = Arrays.copyOf(b.b, n);
        }
    }

    public static void wVar32(Buf b, int v) {
        ensure(b, 5);
        while ((v & ~0x7F) != 0) {
            b.b[b.pos++] = (byte) ((v & 0x7F) | 0x80);
            v >>>= 7;
        }
        b.b[b.pos++] = (byte) v;
    }

    public static void wVar64(Buf b, long v) {
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
        ensure(b, 4);
        b.b[b.pos++] = (byte) v;
        b.b[b.pos++] = (byte) (v >> 8);
        b.b[b.pos++] = (byte) (v >> 16);
        b.b[b.pos++] = (byte) (v >> 24);
    }

    public static void wFixed64(Buf b, long v) {
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

    /** Reserves a 1-byte length placeholder; see patch(). */
    public static int patchReserve(Buf b) {
        ensure(b, 1);
        int lp = b.pos;
        b.b[b.pos++] = 0;
        return lp;
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

    public static long rVar(Reader r) {
        byte[] b = r.b;
        int p = r.pos;
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
                r.pos += rLen(r);
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

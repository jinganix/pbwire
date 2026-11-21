// Protobuf wire primitives for the pure-C# core: varint/tag/little-endian
// read and write, zigzag, skipping (with group frames) and the structural
// UTF-8 check. One home for everything the descriptor reader, the encoder and
// the decoder do to bytes, so they cannot diverge.
//
// The rules are the C core's (src/wire.c, src/decode.c): strict tags and
// lengths, unknown fields skippable by shape, groups validated to their
// matching end tag, one nesting cap for groups and messages alike.
//
// Nothing here calls the native library: this file is the whole point of the
// plan -- a build with no C toolchain still encodes and decodes the same bytes.

using System.Text;

namespace Pbwire.Core;

/// <summary>Protobuf wire types.</summary>
public static class Wt
{
    public const int Varint = 0;
    public const int Fixed64 = 1;
    public const int Len = 2;
    public const int StartGroup = 3;
    public const int EndGroup = 4;
    public const int Fixed32 = 5;
}

/// <summary>Anything the pure core refused: the C core's pbw_status where one exists, else -1.</summary>
public sealed class CoreException : Exception
{
    public CoreException(string message, int status = -1) : base(message) => Status = status;

    /// <summary>A <c>pbw_status</c> value, or -1 when there is no matching C status.</summary>
    public int Status { get; }
}

/// <summary>Growable output buffer, reset per encode (the C core's pbw_ctx twin).</summary>
public sealed class W
{
    public W(int capacity = 256) => B = new byte[capacity];

    public byte[] B;

    public int Pos;

    public void Reset() => Pos = 0;
}

/// <summary>Cursor over an input slice.</summary>
public sealed class R
{
    public R(byte[] b, int pos = 0, int? end = null)
    {
        B = b;
        Pos = pos;
        End = end ?? b.Length;
    }

    public byte[] B;

    public int Pos;

    public int End;

    /// <summary>Nesting cap for this decode; set from <see cref="Limits"/> at entry.</summary>
    public int Cap = Wire.MaxDepth;
}

/// <summary>The wire primitives. Every one is bounds-checked against the reader's slice.</summary>
public static class Wire
{
    /// <summary>Nesting cap, the same bound as the C core's PBW_MAX_DEPTH.</summary>
    public const int MaxDepth = 256;

    public static void Ensure(W w, int extra)
    {
        if (w.Pos + extra > w.B.Length)
        {
            Grow(w, w.Pos + extra);
        }
    }

    private static void Grow(W w, int need)
    {
        var n = w.B.Length * 2;
        while (n < need)
        {
            n *= 2;
        }
        var next = new byte[n];
        Array.Copy(w.B, next, w.Pos);
        w.B = next;
    }

    /// <summary>The (field-number, wire-type) tag as bytes.</summary>
    public static byte[] TagBytes(int fnum, int wt)
    {
        var tag = (uint)fnum * 8u + (uint)wt;
        var outb = new byte[5];
        var n = 0;
        while (tag > 0x7f)
        {
            outb[n++] = (byte)((tag & 0x7f) | 0x80);
            tag >>= 7;
        }
        outb[n++] = (byte)tag;
        var result = new byte[n];
        Array.Copy(outb, result, n);
        return result;
    }

    public static void WTag(W w, byte[] tag)
    {
        Ensure(w, tag.Length);
        Array.Copy(tag, 0, w.B, w.Pos, tag.Length);
        w.Pos += tag.Length;
    }

    /// <summary>varint32 write of an unsigned 32-bit value.</summary>
    public static void WVar32(W w, uint v)
    {
        Ensure(w, 5);
        var b = w.B;
        var p = w.Pos;
        var x = v;
        while (x > 0x7f)
        {
            b[p++] = (byte)((x & 0x7f) | 0x80);
            x >>= 7;
        }
        b[p++] = (byte)x;
        w.Pos = p;
    }

    /// <summary>varint64 write of an unsigned 64-bit value.</summary>
    public static void WVar64(W w, ulong v)
    {
        Ensure(w, 10);
        var b = w.B;
        var p = w.Pos;
        var x = v;
        while (x > 0x7f)
        {
            b[p++] = (byte)((x & 0x7f) | 0x80);
            x >>= 7;
        }
        b[p++] = (byte)x;
        w.Pos = p;
    }

    public static void WFixed32(W w, uint v)
    {
        Ensure(w, 4);
        var b = w.B;
        var p = w.Pos;
        b[p++] = (byte)v;
        b[p++] = (byte)(v >> 8);
        b[p++] = (byte)(v >> 16);
        b[p++] = (byte)(v >> 24);
        w.Pos = p;
    }

    public static void WFixed64(W w, ulong v)
    {
        Ensure(w, 8);
        var b = w.B;
        var p = w.Pos;
        for (var i = 0; i < 8; i++)
        {
            b[p++] = (byte)(v & 0xff);
            v >>= 8;
        }
        w.Pos = p;
    }

    /// <summary>Fused tag+varint32: one capacity check per field.</summary>
    public static void WTagVar32(W w, byte[] tag, uint v)
    {
        Ensure(w, tag.Length + 5);
        var b = w.B;
        var p = w.Pos;
        for (var i = 0; i < tag.Length; i++)
        {
            b[p++] = tag[i];
        }
        var x = v;
        while (x > 0x7f)
        {
            b[p++] = (byte)((x & 0x7f) | 0x80);
            x >>= 7;
        }
        b[p++] = (byte)x;
        w.Pos = p;
    }

    public static void WTagVar64(W w, byte[] tag, ulong v)
    {
        Ensure(w, tag.Length + 10);
        var b = w.B;
        var p = w.Pos;
        for (var i = 0; i < tag.Length; i++)
        {
            b[p++] = tag[i];
        }
        var x = v;
        while (x > 0x7f)
        {
            b[p++] = (byte)((x & 0x7f) | 0x80);
            x >>= 7;
        }
        b[p++] = (byte)x;
        w.Pos = p;
    }

    public static void WTagFixed32(W w, byte[] tag, uint v)
    {
        Ensure(w, tag.Length + 4);
        var b = w.B;
        var p = w.Pos;
        for (var i = 0; i < tag.Length; i++)
        {
            b[p++] = tag[i];
        }
        b[p++] = (byte)v;
        b[p++] = (byte)(v >> 8);
        b[p++] = (byte)(v >> 16);
        b[p++] = (byte)(v >> 24);
        w.Pos = p;
    }

    public static void WTagFixed64(W w, byte[] tag, ulong v)
    {
        Ensure(w, tag.Length + 8);
        var b = w.B;
        var p = w.Pos;
        for (var i = 0; i < tag.Length; i++)
        {
            b[p++] = tag[i];
        }
        for (var i = 0; i < 8; i++)
        {
            b[p++] = (byte)(v & 0xff);
            v >>= 8;
        }
        w.Pos = p;
    }

    /// <summary>Length-delimited payload under <paramref name="tag"/> (string/bytes/message).</summary>
    public static void WBytes(W w, byte[]? tag, byte[] v)
    {
        var tlen = tag?.Length ?? 0;
        Ensure(w, tlen + 5 + v.Length);
        var b = w.B;
        var p = w.Pos;
        if (tag is not null)
        {
            for (var i = 0; i < tag.Length; i++)
            {
                b[p++] = tag[i];
            }
        }
        var n = (uint)v.Length;
        while (n > 0x7f)
        {
            b[p++] = (byte)((n & 0x7f) | 0x80);
            n >>= 7;
        }
        b[p++] = (byte)n;
        Array.Copy(v, 0, b, p, v.Length);
        w.Pos = p + v.Length;
    }

    /// <summary>
    /// Fills a 1-byte length placeholder written at <paramref name="lp"/>: the
    /// single-byte fast path, or a memmove when the length needs more bytes.
    /// </summary>
    public static void PatchLen(W w, int lp)
    {
        var len = w.Pos - lp - 1;
        if (len < 128)
        {
            w.B[lp] = (byte)len;
            return;
        }
        var lb = new byte[5];
        var n = 0;
        var v = (uint)len;
        while (v > 0x7f)
        {
            lb[n++] = (byte)((v & 0x7f) | 0x80);
            v >>= 7;
        }
        lb[n++] = (byte)v;
        Ensure(w, n - 1);
        Array.Copy(w.B, lp + 1, w.B, lp + n, w.Pos - lp - 1);
        Array.Copy(lb, 0, w.B, lp, n);
        w.Pos += n - 1;
    }

    public static int SizeVar32(uint v)
    {
        var n = 1;
        while (v > 0x7f)
        {
            v >>= 7;
            n++;
        }
        return n;
    }

    /// <summary>varint64 of an unsigned value.</summary>
    public static int SizeVar64(ulong v)
    {
        var n = 1;
        while (v > 0x7f)
        {
            v >>= 7;
            n++;
        }
        return n;
    }

    /// <summary>int32: negatives sign-extend to 10 bytes, same as libprotobuf.</summary>
    public static int SizeInt32(int v) => v >= 0 ? SizeVar32((uint)v) : 10;

    public static uint Zig32(int v) => (uint)((v << 1) ^ (v >> 31));

    public static ulong Zig64(long v) => (ulong)((v << 1) ^ (v >> 63));

    public static int Unzig32(uint v) => (int)((v >> 1) ^ (uint)-(int)(v & 1));

    public static long Unzig64(ulong v) => (long)((v >> 1) ^ (ulong)-(long)(v & 1));

    /* ---------------- reading ---------------- */

    /// <summary>varint, low 32 bits (unsigned). Validates the encoding's length only.</summary>
    public static uint RVar(R r)
    {
        var b = r.B;
        var end = r.End;
        var p = r.Pos;
        uint lo = 0;
        var j = 0;
        for (;;)
        {
            if (p >= end)
            {
                throw new CoreException("truncated varint");
            }
            var c = b[p++];
            if (j < 5)
            {
                // The 32-bit shift discards the high bits on purpose: only the
                // low 32 are ever visible for the 32-bit kinds.
                lo |= (uint)(c & 0x7f) << (j * 7);
            }
            if ((c & 0x80) == 0)
            {
                r.Pos = p;
                return lo;
            }
            j++;
            if (j >= 10)
            {
                throw new CoreException("varint overflow");
            }
        }
    }

    /// <summary>varint, full 64 bits (unsigned).</summary>
    public static ulong RVar64(R r)
    {
        var b = r.B;
        var end = r.End;
        var p = r.Pos;
        ulong v = 0;
        var shift = 0;
        for (;;)
        {
            if (p >= end)
            {
                throw new CoreException("truncated varint");
            }
            var c = b[p++];
            v |= (ulong)(c & 0x7f) << shift;
            if ((c & 0x80) == 0)
            {
                r.Pos = p;
                return v;
            }
            shift += 7;
            if (shift >= 64)
            {
                throw new CoreException("varint overflow");
            }
        }
    }

    /// <summary>varint, reduced to its bool value: any set payload bit means true.</summary>
    public static int RVarBool(R r)
    {
        var b = r.B;
        var end = r.End;
        var p = r.Pos;
        var nz = 0;
        var j = 0;
        for (;;)
        {
            if (p >= end)
            {
                throw new CoreException("truncated varint");
            }
            var c = b[p++];
            nz |= c & 0x7f;
            if ((c & 0x80) == 0)
            {
                r.Pos = p;
                return nz != 0 ? 1 : 0;
            }
            j++;
            if (j >= 10)
            {
                throw new CoreException("varint overflow");
            }
        }
    }

    /// <summary>
    /// Strict length read, like the core's length reader: at most five bytes and
    /// the fifth only carries four bits; an out-of-range length is rejected, not
    /// truncated.
    /// </summary>
    public static int RLen32(R r)
    {
        var b = r.B;
        var end = r.End;
        var p = r.Pos;
        if (p >= end)
        {
            throw new CoreException("truncated length");
        }
        var c = b[p++];
        var v = c & 0x7f;
        var shift = 7;
        while ((c & 0x80) != 0)
        {
            if (shift >= 35 || p >= end)
            {
                throw new CoreException("bad length");
            }
            c = b[p++];
            if (shift == 28 && (c & 0x7f) > 0x0f)
            {
                throw new CoreException("length overflow");
            }
            v |= (c & 0x7f) << shift;
            shift += 7;
        }
        if (v < 0 || p + v > end)
        {
            throw new CoreException("truncated length-delimited");
        }
        r.Pos = p;
        return v;
    }

    /// <summary>Loose length read for descriptor parsing: any varint up to 2^31-1, bounds checked.</summary>
    public static int RLen(R r)
    {
        var v = RVar64(r);
        if (v > 0x7fff_ffffUL)
        {
            throw new CoreException("length overflow");
        }
        var n = (int)v;
        if (r.Pos + n > r.End)
        {
            throw new CoreException("truncated length-delimited");
        }
        return n;
    }

    /// <summary>
    /// Strict tag read, like the core's tag reader: a varint32 of at most five
    /// bytes. A 5-byte tag above UINT32_MAX truncates instead of failing, exactly
    /// what the reference parser does (the official suite expects two such cases
    /// to parse).
    /// </summary>
    public static uint RTag(R r)
    {
        var b = r.B;
        var end = r.End;
        var p = r.Pos;
        if (p >= end)
        {
            throw new CoreException("truncated tag");
        }
        var c = b[p++];
        var v = (uint)(c & 0x7f);
        var shift = 7;
        while ((c & 0x80) != 0)
        {
            if (shift >= 35 || p >= end)
            {
                throw new CoreException("bad tag");
            }
            c = b[p++];
            v |= (uint)(c & 0x7f) << shift; // 32-bit: truncates like the reference
            shift += 7;
        }
        r.Pos = p;
        return v;
    }

    public static uint RFixed32(R r)
    {
        if (r.Pos + 4 > r.End)
        {
            throw new CoreException("truncated fixed32");
        }
        var b = r.B;
        var p = r.Pos;
        r.Pos = p + 4;
        return (uint)(b[p] | (b[p + 1] << 8) | (b[p + 2] << 16) | (b[p + 3] << 24));
    }

    /// <summary>Raw 64 bits, unsigned.</summary>
    public static ulong RFixed64(R r)
    {
        if (r.Pos + 8 > r.End)
        {
            throw new CoreException("truncated fixed64");
        }
        var b = r.B;
        var p = r.Pos;
        r.Pos = p + 8;
        var lo = (uint)(b[p] | (b[p + 1] << 8) | (b[p + 2] << 16) | (b[p + 3] << 24));
        var hi = (uint)(b[p + 4] | (b[p + 5] << 8) | (b[p + 6] << 16) | (b[p + 7] << 24));
        return lo | ((ulong)hi << 32);
    }

    /// <summary>Skips one field value of a non-group wire type.</summary>
    public static void Skip(R r, int wt)
    {
        switch (wt)
        {
            case Wt.Varint:
                RVarBool(r);
                return;
            case Wt.Fixed64:
                if (r.Pos + 8 > r.End)
                {
                    throw new CoreException("truncated fixed64");
                }
                r.Pos += 8;
                return;
            case Wt.Len:
            {
                var n = RLen32(r);
                r.Pos += n;
                return;
            }
            case Wt.Fixed32:
                if (r.Pos + 4 > r.End)
                {
                    throw new CoreException("truncated fixed32");
                }
                r.Pos += 4;
                return;
            default:
                throw new CoreException("bad wire type " + wt);
        }
    }

    /// <summary>
    /// Validates a group body up to its matching end tag, like the core's group
    /// skipper: every tag is strict, field 0 and wire types above 5 are rejected,
    /// and an end tag only closes the group it names.
    /// </summary>
    public static void SkipGroup(R r, int number, int depth)
    {
        if (depth > r.Cap)
        {
            throw new CoreException("nesting exceeds " + r.Cap, 4);
        }
        for (;;)
        {
            var tag = RTag(r);
            var fnum = (int)(tag >> 3);
            var wt = (int)(tag & 7);
            if (fnum == 0 || wt > Wt.Fixed32)
            {
                throw new CoreException("bad tag " + tag);
            }
            if (wt == Wt.EndGroup)
            {
                if (fnum != number)
                {
                    throw new CoreException("mismatched end-group for " + fnum + ", want " + number);
                }
                return;
            }
            if (wt == Wt.StartGroup)
            {
                SkipGroup(r, fnum, depth + 1);
            }
            else
            {
                Skip(r, wt);
            }
        }
    }

    /// <summary>
    /// Skips one unknown field: groups validate to their matching end tag,
    /// anything else only checks value shape.
    /// </summary>
    public static void SkipUnknown(R r, int fnum, int wt, int depth)
    {
        if (wt == Wt.StartGroup)
        {
            SkipGroup(r, fnum, depth + 1);
            return;
        }
        Skip(r, wt);
    }

    /// <summary>
    /// Structural UTF-8 check: rejects overlong forms, surrogates and values past
    /// U+10FFFF, like the core's pbw_utf8_valid and the reference parsers.
    /// </summary>
    public static bool Utf8Valid(byte[] b, int off, int len)
    {
        var i = off;
        var end = off + len;
        while (i < end)
        {
            var c = b[i];
            if (c < 0x80)
            {
                i++;
            }
            else if ((c & 0xe0) == 0xc0)
            {
                if (c < 0xc2 || i + 1 >= end || (b[i + 1] & 0xc0) != 0x80)
                {
                    return false;
                }
                i += 2;
            }
            else if ((c & 0xf0) == 0xe0)
            {
                if (i + 2 >= end || (b[i + 1] & 0xc0) != 0x80 || (b[i + 2] & 0xc0) != 0x80)
                {
                    return false;
                }
                var v = ((c & 15) << 12) | ((b[i + 1] & 63) << 6) | (b[i + 2] & 63);
                if (v < 0x800 || (v >= 0xd800 && v <= 0xdfff))
                {
                    return false;
                }
                i += 3;
            }
            else if ((c & 0xf8) == 0xf0)
            {
                if (c > 0xf4
                    || i + 3 >= end
                    || (b[i + 1] & 0xc0) != 0x80
                    || (b[i + 2] & 0xc0) != 0x80
                    || (b[i + 3] & 0xc0) != 0x80)
                {
                    return false;
                }
                var v = ((c & 7) << 18) | ((b[i + 1] & 63) << 12) | ((b[i + 2] & 63) << 6) | (b[i + 3] & 63);
                if (v < 0x10000 || v > 0x10ffff)
                {
                    return false;
                }
                i += 4;
            }
            else
            {
                return false;
            }
        }
        return true;
    }

    /// <summary>UTF-8 byte count of a .NET string (a lone surrogate encodes as U+FFFD).</summary>
    public static int Utf8Length(string s) => Encoding.UTF8.GetByteCount(s);

    /// <summary>Writes <paramref name="s"/> as UTF-8 at w.Pos, growing first.</summary>
    public static void Utf8Write(W w, string s)
    {
        var n = Encoding.UTF8.GetByteCount(s);
        Ensure(w, n);
        w.Pos += Encoding.UTF8.GetBytes(s, 0, s.Length, w.B, w.Pos);
    }

    /// <summary>UTF-8 decodes a slice, replacing each malformed unit with U+FFFD.</summary>
    public static string Utf8Decode(byte[] b, int off, int len) => Encoding.UTF8.GetString(b, off, len);

    /* ---------------- float/double bit patterns ---------------- */

    /// <summary>The float32 bit pattern of a value (IEEE round-to-nearest).</summary>
    public static uint FloatBits(float v) => BitConverter.SingleToUInt32Bits(v);

    public static float BitsFloat(uint bits) => BitConverter.UInt32BitsToSingle(bits);

    /// <summary>The float64 bit pattern of a value, as raw 64 bits.</summary>
    public static ulong DoubleBits(double v) => BitConverter.DoubleToUInt64Bits(v);

    public static double BitsDouble(ulong bits) => BitConverter.UInt64BitsToDouble(bits);
}

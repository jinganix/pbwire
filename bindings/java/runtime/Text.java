import java.util.ArrayList;
import java.util.List;

/**
 * Protobuf text format over the dynamic tree: prints the multi-line form the
 * reference implementation produces and parses what its parser accepts.
 * Mirrors the C core's {@code src/text.c}, which is the authority here.
 *
 * <p>Scalars print as {@code name: value}, messages as {@code name { ... }}
 * (two-space indent), groups under their unqualified type name, extensions as
 * {@code [full.name]}, and every repeated element on its own line. Unknown
 * fields print as numeric lines unless the caller asks otherwise. The parser
 * accepts optional colons before {@code {}}, {@code <>} delimiters, the
 * {@code [a, b]} list form for repeated fields, group type names, extensions,
 * {@code Any} expansions, decimal/hex/octal integers, C escapes and
 * {@code ,}/{@code ;} separators. Unknown field names fail unless reserved,
 * which the parser skips silently like the reference.
 *
 * <p>Unlike the binary decoder, a singular field or oneof member may only be
 * named once: text format has no last-wins and no message merge.
 */
public final class Text {
    private Text() {
    }

    /** Prints a message in text format, unknown fields included. */
    public static String print(GenericCodec.DMsg m) {
        return print(m, true);
    }

    /** Prints a message in text format; unknown fields are opt-out. */
    public static String print(GenericCodec.DMsg m, boolean printUnknown) {
        Out out = new Out();
        out.printUnknown = printUnknown;
        printMessage(out, m, 0);
        return out.sb.toString();
    }

    /** Parses text format into a new tree. */
    public static GenericCodec.DMsg parse(GenericCodec.MessageDef def, String text) {
        return parse(def, text, Limits.defaults());
    }

    /** Same with explicit bounds. */
    public static GenericCodec.DMsg parse(GenericCodec.MessageDef def, String text,
            Limits lim) {
        if (lim == null) {
            lim = Limits.defaults();
        }
        lim.checkBytes(text.length(), "text");
        Parser p = new Parser(text, lim.effectiveDepth());
        GenericCodec.DMsg m = new GenericCodec.DMsg(def);
        p.messageBody(m, 0, (char) 0);
        if (def.deepRequired) {
            try {
                GenericCodec.checkRequired(m, lim.effectiveDepth(), 0);
            } catch (IllegalArgumentException e) {
                throw new PbwireException("text: " + e.getMessage(), 6);
            }
        }
        return m;
    }

    /* ---------------- print ---------------- */

    private static final class Out {
        final StringBuilder sb = new StringBuilder();
        boolean printUnknown;
    }

    private static void printMessage(Out out, GenericCodec.DMsg m, int depth) {
        if (depth > Wire.MAX_DEPTH) {
            throw new PbwireException("text: nesting exceeds " + Wire.MAX_DEPTH, 5);
        }
        for (int s = 0; s < m.def.fields.length; s++) {
            GenericCodec.Field f = m.def.fields[s];
            if (!m.has[s]) {
                continue;
            }
            if (f.repeated) {
                int n = repLen(m, s);
                for (int i = 0; i < n; i++) {
                    printField(out, m, f, s, i, depth);
                }
                continue;
            }
            if (f.presence == GenericCodec.IMPLICIT && isDefault(m, s, f)) {
                continue; // proto3 omits defaults, like the reference printer
            }
            printField(out, m, f, s, -1, depth);
        }
        if (out.printUnknown && m.unknown != null) {
            for (byte[] blob : m.unknown) {
                printUnknown(out, blob, 0, blob.length, depth);
            }
        }
    }

    private static int repLen(GenericCodec.DMsg m, int slot) {
        GenericCodec.Rep rp = m.rep;
        if (rp == null) {
            return 0;
        }
        return m.def.fields[slot].kind <= GenericCodec.K_DOUBLE
                ? rp.numLen[slot] : rp.refLen[slot];
    }

    private static boolean isDefault(GenericCodec.DMsg m, int slot,
            GenericCodec.Field f) {
        if (f.kind <= GenericCodec.K_DOUBLE) {
            return m.num[slot] == 0;
        }
        Object v = m.ref[slot];
        if (f.kind == GenericCodec.K_STRING) {
            return v instanceof String && ((String) v).isEmpty();
        }
        if (f.kind == GenericCodec.K_BYTES) {
            return v instanceof byte[] && ((byte[]) v).length == 0;
        }
        return false;
    }

    private static Object fieldValue(GenericCodec.DMsg m, int slot, int i) {
        GenericCodec.Field f = m.def.fields[slot];
        GenericCodec.Rep rp = m.rep;
        if (i >= 0) {
            if (f.kind <= GenericCodec.K_DOUBLE) {
                return GenericCodec.DMsg.box(f, rp.num[slot][i]);
            }
            Object v = rp.ref[slot][i];
            if (v instanceof GenericCodec.Slice) {
                v = GenericCodec.materialize(f, (GenericCodec.Slice) v);
                rp.ref[slot][i] = v;
            }
            return v;
        }
        if (f.kind <= GenericCodec.K_DOUBLE) {
            return GenericCodec.DMsg.box(f, m.num[slot]);
        }
        Object v = m.ref[slot];
        if (v instanceof GenericCodec.Slice) {
            v = GenericCodec.materialize(f, (GenericCodec.Slice) v);
            m.ref[slot] = v;
        }
        return v;
    }

    private static void printField(Out out, GenericCodec.DMsg m,
            GenericCodec.Field f, int slot, int i, int depth) {
        indent(out, depth);
        out.sb.append(displayName(f));
        Object v = fieldValue(m, slot, i);
        if (f.kind == GenericCodec.K_MESSAGE || f.kind == GenericCodec.K_GROUP) {
            out.sb.append(" {\n");
            printMessage(out, (GenericCodec.DMsg) v, depth + 1);
            indent(out, depth);
            out.sb.append("}\n");
            return;
        }
        out.sb.append(": ");
        printScalar(out, f, v);
        out.sb.append('\n');
    }

    /** Groups print under their type's short name; extensions bracket the name. */
    private static String displayName(GenericCodec.Field f) {
        if (f.extension) {
            return "[" + (f.fullName != null ? f.fullName : f.fieldName) + "]";
        }
        if (f.kind == GenericCodec.K_GROUP && f.msgType != null) {
            String name = f.msgType.fullName;
            return name.substring(name.lastIndexOf('.') + 1);
        }
        return f.fieldName;
    }

    private static void indent(Out out, int depth) {
        for (int i = 0; i < depth; i++) {
            out.sb.append("  ");
        }
    }

    private static void printScalar(Out out, GenericCodec.Field f, Object v) {
        switch (f.kind) {
            case GenericCodec.K_INT32:
            case GenericCodec.K_SINT32:
            case GenericCodec.K_SFIXED32:
                out.sb.append(((Number) v).intValue());
                return;
            case GenericCodec.K_INT64:
            case GenericCodec.K_SINT64:
            case GenericCodec.K_SFIXED64:
                out.sb.append(((Number) v).longValue());
                return;
            case GenericCodec.K_UINT32:
            case GenericCodec.K_FIXED32:
                out.sb.append(((Number) v).longValue() & 0xFFFFFFFFL);
                return;
            case GenericCodec.K_UINT64:
            case GenericCodec.K_FIXED64:
                out.sb.append(Long.toUnsignedString(((Number) v).longValue()));
                return;
            case GenericCodec.K_BOOL:
                out.sb.append(((Boolean) v) ? "true" : "false");
                return;
            case GenericCodec.K_ENUM: {
                int number = ((Number) v).intValue();
                String name = f.enumType == null ? null : f.enumType.nameOf(number);
                if (name != null) {
                    out.sb.append(name);
                } else {
                    out.sb.append(number);
                }
                return;
            }
            case GenericCodec.K_FLOAT:
            case GenericCodec.K_DOUBLE: {
                double d = ((Number) v).doubleValue();
                if (Double.isNaN(d)) {
                    out.sb.append("nan");
                } else if (d == Double.POSITIVE_INFINITY) {
                    out.sb.append("inf");
                } else if (d == Double.NEGATIVE_INFINITY) {
                    out.sb.append("-inf");
                } else {
                    out.sb.append(Double.toString(d));
                }
                return;
            }
            case GenericCodec.K_STRING:
                quote(out, (String) v);
                return;
            default:
                quoteBytes(out, (byte[]) v);
                return;
        }
    }

    /** absl::CEscape: short escapes for tab/lf/cr, octal for the rest. */
    private static void quoteBytes(Out out, byte[] b) {
        out.sb.append('"');
        for (byte x : b) {
            escapeByte(out, x & 0xFF);
        }
        out.sb.append('"');
    }

    private static void quote(Out out, String s) {
        if (!validUnicode(s)) {
            throw new PbwireException("text: string is not valid Unicode", 1);
        }
        out.sb.append('"');
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            if (c < 0x80) {
                escapeByte(out, c);
                continue;
            }
            out.sb.append(c);
        }
        out.sb.append('"');
    }

    private static boolean validUnicode(String s) {
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            if (Character.isHighSurrogate(c)) {
                if (i + 1 >= s.length()
                        || !Character.isLowSurrogate(s.charAt(i + 1))) {
                    return false;
                }
                i++;
            } else if (Character.isLowSurrogate(c)) {
                return false;
            }
        }
        return true;
    }

    private static void escapeByte(Out out, int c) {
        switch (c) {
            case '\t':
                out.sb.append("\\t");
                return;
            case '\n':
                out.sb.append("\\n");
                return;
            case '\r':
                out.sb.append("\\r");
                return;
            case '"':
                out.sb.append("\\\"");
                return;
            case '\\':
                out.sb.append("\\\\");
                return;
            case '\'':
                out.sb.append("\\'");
                return;
            default:
                break;
        }
        if (c >= 32 && c < 127) {
            out.sb.append((char) c);
        } else {
            out.sb.append('\\');
            out.sb.append((char) ('0' + ((c >> 6) & 7)));
            out.sb.append((char) ('0' + ((c >> 3) & 7)));
            out.sb.append((char) ('0' + (c & 7)));
        }
    }

    /* ---------------- unknown fields ---------------- */

    private static void printUnknown(Out out, byte[] b, int from, int to,
            int depth) {
        Wire.Reader r = new Wire.Reader();
        r.b = b;
        r.pos = from;
        r.end = to;
        while (r.pos < r.end) {
            int tag = Wire.rTag(r);
            int number = tag >>> 3;
            int wt = tag & 7;
            if (number == 0 || wt > Wire.WT_FIXED32) {
                throw new PbwireException("text: bad unknown field", 1);
            }
            printUnknownValue(out, number, wt, r, depth);
        }
    }

    private static void printUnknownValue(Out out, int number, int wt,
            Wire.Reader r, int depth) {
        if (depth >= Wire.MAX_DEPTH) {
            throw new PbwireException("text: unknown nesting exceeds "
                    + Wire.MAX_DEPTH, 5);
        }
        indent(out, depth);
        out.sb.append(number);
        switch (wt) {
            case Wire.WT_VARINT:
                out.sb.append(": ").append(Wire.rVar(r)).append('\n');
                return;
            case Wire.WT_FIXED64: {
                long v = Wire.rFixed64(r);
                out.sb.append(": 0x")
                        .append(String.format("%016x", v)).append('\n');
                return;
            }
            case Wire.WT_FIXED32: {
                int v = Wire.rFixed32(r);
                out.sb.append(": 0x").append(String.format("%08x", v))
                        .append('\n');
                return;
            }
            case Wire.WT_LEN: {
                int n = Wire.rLen(r);
                int start = r.pos;
                r.pos = start; // rLen already validated the bounds
                if (unknownIsMessage(r.b, start, start + n, depth)) {
                    out.sb.append(" {\n");
                    printUnknown(out, r.b, start, start + n, depth + 1);
                    indent(out, depth);
                    out.sb.append("}\n");
                } else {
                    out.sb.append(": ");
                    byte[] slice = new byte[n];
                    System.arraycopy(r.b, start, slice, 0, n);
                    quoteBytes(out, slice);
                    out.sb.append('\n');
                }
                r.pos = start + n;
                return;
            }
            default: { // start group: print the body, then the end tag
                int[] span = groupSpan(r, number);
                out.sb.append(" {\n");
                printUnknown(out, r.b, r.pos, span[0], depth + 1);
                indent(out, depth);
                out.sb.append("}\n");
                r.pos = span[1]; // past the matching end-group tag
                return;
            }
        }
    }

    /** [body end, position past the end tag] for an unknown group. */
    private static int[] groupSpan(Wire.Reader r, int number) {
        int nesting = 1;
        Wire.Reader sub = new Wire.Reader();
        sub.b = r.b;
        sub.pos = r.pos;
        sub.end = r.end;
        while (sub.pos < sub.end) {
            int at = sub.pos;
            int tag = Wire.rTag(sub);
            int wt = tag & 7;
            if ((tag >>> 3) == 0 || wt > Wire.WT_FIXED32) {
                throw new PbwireException("text: bad unknown group", 1);
            }
            if (wt == 3) {
                nesting++;
            } else if (wt == 4) {
                nesting--;
                if (nesting == 0) {
                    if ((tag >>> 3) != number) {
                        throw new PbwireException("text: mismatched group end", 1);
                    }
                    return new int[] {at, sub.pos};
                }
            } else {
                Wire.skipUnknown(sub, tag >>> 3, wt, 0);
            }
        }
        throw new PbwireException("text: unterminated unknown group", 1);
    }

    /** Whether a raw payload parses cleanly as fields, like the reference. */
    private static boolean unknownIsMessage(byte[] b, int from, int to, int depth) {
        if (from == to || depth >= Wire.MAX_DEPTH) {
            return false;
        }
        Wire.Reader r = new Wire.Reader();
        r.b = b;
        r.pos = from;
        r.end = to;
        try {
            while (r.pos < r.end) {
                int tag = Wire.rTag(r);
                int wt = tag & 7;
                if ((tag >>> 3) == 0 || wt > Wire.WT_FIXED32) {
                    return false;
                }
                Wire.skipUnknown(r, tag >>> 3, wt, depth);
            }
        } catch (RuntimeException e) {
            return false;
        }
        return true;
    }

    /* ---------------- parse ---------------- */

    private static final class Parser {
        final String s;
        int pos;
        final int cap;

        Parser(String s, int cap) {
            this.s = s;
            this.cap = cap;
        }

        boolean eof() {
            return pos >= s.length();
        }

        void fail(String what) {
            throw new PbwireException("text: " + what + " at offset " + pos, 1);
        }

        char peek() {
            if (pos >= s.length()) {
                fail("truncated input");
            }
            return s.charAt(pos);
        }

        void spaces() {
            while (pos < s.length()) {
                char c = s.charAt(pos);
                if (c == ' ' || c == '\t' || c == '\n' || c == '\u000b'
                        || c == '\f' || c == '\r') {
                    pos++;
                } else if (c == '#') { // line comments, like the reference
                    while (pos < s.length() && s.charAt(pos) != '\n') {
                        pos++;
                    }
                } else {
                    return;
                }
            }
        }

        /* ---- tokens ---- */

        String ident() {
            spaces();
            int start = pos;
            if (pos >= s.length() || !isIdentStart(s.charAt(pos))) {
                return null;
            }
            while (pos < s.length() && isIdentChar(s.charAt(pos))) {
                pos++;
            }
            return s.substring(start, pos);
        }

        /** One quoted string, quotes included in the span. */
        int[] stringSpan() {
            spaces();
            if (pos >= s.length()) {
                return null;
            }
            char quote = s.charAt(pos);
            if (quote != '"' && quote != '\'') {
                return null;
            }
            int start = pos;
            int p = pos + 1;
            while (true) {
                if (p >= s.length()) {
                    return null; // unterminated
                }
                char c = s.charAt(p);
                if (c == '\n') {
                    return null; // no multiline strings
                }
                if (c == '\\') {
                    p++;
                    if (p >= s.length() || s.charAt(p) == '\n'
                            || s.charAt(p) == '\r') {
                        return null;
                    }
                    p++;
                    continue;
                }
                if (c == quote) {
                    p++;
                    break;
                }
                p++;
            }
            pos = p;
            return new int[] {start, p};
        }

        /* ---- numbers ---- */

        private static final int N_INT = 0, N_FLOAT = 1, N_BAD = 2;

        private static final class Num {
            int kind;
            boolean hex;
            boolean oct;
            String text;
        }

        /** Lexes a number like the reference tokenizer; null when there is none. */
        Num lexNumber() {
            Num n = new Num();
            int p = pos;
            if (p < s.length() && s.charAt(p) == '0' && p + 1 < s.length()
                    && (s.charAt(p + 1) == 'x' || s.charAt(p + 1) == 'X')) {
                int q = p + 2;
                while (q < s.length() && isHex(s.charAt(q))) {
                    q++;
                }
                if (q == p + 2) {
                    return null; // "0x" without digits
                }
                n.kind = N_INT;
                n.hex = true;
                n.text = s.substring(p, q);
                pos = q;
                return n;
            }
            int q = p;
            boolean isFloat = false;
            while (q < s.length() && isDigit(s.charAt(q))) {
                q++;
            }
            if (p < s.length() && s.charAt(p) == '0' && q > p + 1) {
                n.oct = true;
            }
            if (q < s.length() && s.charAt(q) == '.') {
                isFloat = true;
                q++;
                while (q < s.length() && isDigit(s.charAt(q))) {
                    q++;
                }
            }
            if (q < s.length() && (s.charAt(q) == 'e' || s.charAt(q) == 'E')) {
                int r = q + 1;
                if (r < s.length() && (s.charAt(r) == '-' || s.charAt(r) == '+')) {
                    r++;
                }
                if (r >= s.length() || !isDigit(s.charAt(r))) {
                    // "1e" without exponent: float-shaped, never a value
                    n.kind = N_FLOAT;
                    n.text = s.substring(p, q);
                    pos = q;
                    return n;
                }
                while (r < s.length() && isDigit(s.charAt(r))) {
                    r++;
                }
                q = r;
                isFloat = true;
            }
            if (q < s.length() && (s.charAt(q) == 'f' || s.charAt(q) == 'F')) {
                q++;
                isFloat = true;
            }
            if (q == p) {
                return null;
            }
            if (n.oct && isFloat) {
                return null; // 03.5 and 01e5 are tokenizer errors
            }
            n.kind = isFloat ? N_FLOAT : N_INT;
            n.text = s.substring(p, q);
            pos = q;
            return n;
        }

        /** A letter right after a number is an error, like the reference. */
        private boolean numberEnd() {
            return pos >= s.length() || !isIdentStart(s.charAt(pos));
        }

        Num parseNumber() {
            spaces();
            if (pos >= s.length()) {
                return null;
            }
            if (isDigit(s.charAt(pos))) {
                Num n = lexNumber();
                if (n == null || !numberEnd()) {
                    return null;
                }
                return n;
            }
            if (s.charAt(pos) == '.' && pos + 1 < s.length()
                    && isDigit(s.charAt(pos + 1))) {
                int p = pos;
                int q = p + 1;
                while (q < s.length() && isDigit(s.charAt(q))) {
                    q++;
                }
                if (q < s.length() && (s.charAt(q) == 'e' || s.charAt(q) == 'E')) {
                    int r = q + 1;
                    if (r < s.length()
                            && (s.charAt(r) == '-' || s.charAt(r) == '+')) {
                        r++;
                    }
                    if (r >= s.length() || !isDigit(s.charAt(r))) {
                        Num n = new Num();
                        n.kind = N_FLOAT;
                        n.text = s.substring(p, q);
                        pos = q;
                        return n;
                    }
                    while (r < s.length() && isDigit(s.charAt(r))) {
                        r++;
                    }
                    q = r;
                }
                if (q < s.length() && (s.charAt(q) == 'f' || s.charAt(q) == 'F')) {
                    q++;
                }
                Num n = new Num();
                n.kind = N_FLOAT;
                n.text = s.substring(p, q);
                pos = q;
                return numberEnd() ? n : null;
            }
            return null;
        }

        /** Unsigned magnitude with hex/octal/decimal range checking. */
        long parseUint(Num n, long max) {
            if (n.kind != N_INT) {
                fail("expected an integer");
            }
            String p = n.text;
            long value = 0;
            if (n.hex) {
                for (int i = 2; i < p.length(); i++) {
                    long d = Character.digit(p.charAt(i), 16);
                    if (Long.compareUnsigned(value, Long.divideUnsigned(max, 16)) > 0
                            || (Long.compareUnsigned(value,
                                    Long.divideUnsigned(max, 16)) == 0
                                    && d > Long.remainderUnsigned(max, 16))) {
                        fail("integer out of range");
                    }
                    value = value * 16 + d;
                }
                return value;
            }
            int base = n.oct ? 8 : 10;
            int from = n.oct ? 1 : 0;
            for (int i = from; i < p.length(); i++) {
                int d = p.charAt(i) - '0';
                if (d >= base) {
                    fail("bad integer digit");
                }
                if (Long.compareUnsigned(value, Long.divideUnsigned(max, base)) > 0
                        || (Long.compareUnsigned(value,
                                Long.divideUnsigned(max, base)) == 0
                                && d > Long.remainderUnsigned(max, base))) {
                    fail("integer out of range");
                }
                value = value * base + d;
            }
            return value;
        }

        /**
         * A signed value of `bits` width: the magnitude parses unsigned, so
         * INT32_MIN/INT64_MIN stay in range exactly like the reference tokenizer.
         */
        long signed(Num n, boolean negative, int bits) {
            long umax = bits == 32 ? 0x7FFFFFFFL : 0x7FFFFFFFFFFFFFFFL; // signed max
            long limit = bits == 32 ? 0x80000000L : Long.MIN_VALUE; // |min|
            long mag = parseUint(n, negative ? limit : umax);
            if (!negative) {
                return bits == 32 ? (long) (int) mag : mag;
            }
            if (Long.compareUnsigned(mag, limit) == 0) {
                return bits == 32 ? Integer.MIN_VALUE : Long.MIN_VALUE;
            }
            return bits == 32 ? -(long) (int) mag : -mag;
        }

        /* ---- field values ---- */

        void messageBody(GenericCodec.DMsg m, int depth, char closer) {
            if (depth > cap) {
                throw new PbwireException("text: nesting exceeds " + cap, 5);
            }
            while (true) {
                spaces();
                if (eof()) {
                    if (closer != 0) {
                        fail("missing '" + (closer == '}' ? '}' : '>') + "'");
                    }
                    return;
                }
                if (closer != 0 && s.charAt(pos) == closer) {
                    pos++;
                    return;
                }
                if (s.charAt(pos) == '[') {
                    bracketField(m, depth);
                } else {
                    String name = ident();
                    if (name == null) {
                        fail("expected a field name");
                    }
                    int slot = lookup(m.def, name);
                    if (slot < 0) {
                        if (m.def.reservedNames != null
                                && hasReserved(m.def, name)) {
                            spaces();
                            if (pos < s.length() && s.charAt(pos) != '{'
                                    && s.charAt(pos) != '<') {
                                if (s.charAt(pos) != ':') {
                                    fail("expected ':'");
                                }
                                pos++;
                            }
                            skipValue(depth);
                        } else {
                            throw new PbwireException("text: unknown field \""
                                    + name + "\" in " + m.def.fullName, 3);
                        }
                        separator();
                        continue;
                    }
                    namedField(m, slot, depth);
                }
                separator();
            }
        }

        private static boolean hasReserved(GenericCodec.MessageDef def,
                String name) {
            for (String r : def.reservedNames) {
                if (r.equals(name)) {
                    return true;
                }
            }
            return false;
        }

        /** Optional ',' and ';' between fields, like the reference parser. */
        private void separator() {
            spaces();
            if (pos < s.length() && (s.charAt(pos) == ','
                    || s.charAt(pos) == ';')) {
                pos++;
            }
        }

        /** Parses one `name value` pair of a message body. */
        void namedField(GenericCodec.DMsg m, int slot, int depth) {
            GenericCodec.Field f = m.def.fields[slot];
            if (!f.repeated) {
                // Singular fields and oneof members may be named only once:
                // text format has no last-wins and no merge.
                if (m.has[slot]) {
                    fail("duplicate field " + f.fieldName);
                }
                if (f.oneof >= 0 && m.oneofCase[f.oneof] != 0) {
                    fail("duplicate oneof member " + f.fieldName);
                }
            }
            if (f.kind == GenericCodec.K_MESSAGE
                    || f.kind == GenericCodec.K_GROUP) {
                messageValue(m, f, slot, depth);
                return;
            }
            spaces();
            if (eof() || peek() != ':') {
                fail("expected ':'");
            }
            pos++;
            if (f.repeated) {
                spaces();
                if (pos < s.length() && s.charAt(pos) == '[') {
                    list(m, f, slot, depth);
                    return;
                }
            }
            storeScalar(m, f, slot);
        }

        /** A `{...}` or `<...>` message value; repeated ones take the list form. */
        void messageValue(GenericCodec.DMsg m, GenericCodec.Field f, int slot,
                int depth) {
            if (depth >= cap) {
                throw new PbwireException("text: nesting exceeds " + cap, 5);
            }
            spaces();
            if (pos < s.length() && s.charAt(pos) == ':') {
                pos++;
                spaces();
            }
            if (f.repeated && pos < s.length() && s.charAt(pos) == '[') {
                list(m, f, slot, depth);
                return;
            }
            if (eof() || (peek() != '{' && peek() != '<')) {
                fail("expected '{'");
            }
            char open = s.charAt(pos++);
            GenericCodec.DMsg sub = new GenericCodec.DMsg(f.msgType);
            messageBody(sub, depth + 1, open == '{' ? '}' : '>');
            if (f.repeated) {
                m.add(f.number, sub);
            } else {
                m.set(f.number, sub);
            }
        }

        /** The `[a, b]` list form for repeated fields; no trailing comma. */
        void list(GenericCodec.DMsg m, GenericCodec.Field f, int slot, int depth) {
            spaces();
            if (pos >= s.length() || s.charAt(pos) != '[') {
                fail("expected '['");
            }
            pos++;
            spaces();
            if (pos < s.length() && s.charAt(pos) == ']') {
                pos++;
                return;
            }
            while (true) {
                if (f.kind == GenericCodec.K_MESSAGE
                        || f.kind == GenericCodec.K_GROUP) {
                    spaces();
                    if (pos >= s.length() || (s.charAt(pos) != '{'
                            && s.charAt(pos) != '<')) {
                        fail("expected '{'");
                    }
                    char open = s.charAt(pos++);
                    GenericCodec.DMsg sub = new GenericCodec.DMsg(f.msgType);
                    messageBody(sub, depth + 1, open == '{' ? '}' : '>');
                    m.add(f.number, sub);
                } else {
                    storeScalar(m, f, slot);
                }
                spaces();
                if (pos < s.length() && s.charAt(pos) == ',') {
                    pos++;
                    spaces();
                    if (pos < s.length() && s.charAt(pos) == ']') {
                        fail("trailing comma in a list");
                    }
                    continue;
                }
                if (pos < s.length() && s.charAt(pos) == ']') {
                    pos++;
                    return;
                }
                fail("expected ',' or ']'");
            }
        }

        void storeScalar(GenericCodec.DMsg m, GenericCodec.Field f, int slot) {
            switch (f.kind) {
                case GenericCodec.K_STRING:
                    storeNumber(m, f, slot, stringValue());
                    return;
                case GenericCodec.K_BYTES:
                    storeNumber(m, f, slot, bytesValue());
                    return;
                case GenericCodec.K_ENUM:
                    storeEnum(m, f, slot);
                    return;
                case GenericCodec.K_BOOL:
                    storeNumber(m, f, slot, boolValue());
                    return;
                case GenericCodec.K_FLOAT:
                case GenericCodec.K_DOUBLE:
                    storeNumber(m, f, slot, floatValue(f.kind == GenericCodec.K_FLOAT));
                    return;
                default:
                    storeNumber(m, f, slot, intValue(f));
                    return;
            }
        }

        void storeNumber(GenericCodec.DMsg m, GenericCodec.Field f, int slot,
                Object v) {
            if (f.repeated) {
                m.add(f.number, v);
            } else {
                m.set(f.number, v);
            }
        }

        void storeEnum(GenericCodec.DMsg m, GenericCodec.Field f, int slot) {
            spaces();
            int number;
            if (pos < s.length() && (isDigit(s.charAt(pos))
                    || s.charAt(pos) == '-')) {
                boolean negative = s.charAt(pos) == '-';
                if (negative) {
                    pos++;
                }
                Num n = parseNumber();
                if (n == null) {
                    fail("bad enum number");
                }
                number = (int) signed(n, negative, 32);
            } else {
                String name = ident();
                if (name == null || f.enumType == null) {
                    fail("bad enum value");
                }
                Integer v = f.enumType.valueOf(name);
                if (v == null) {
                    fail("unknown enum name \"" + name + "\"");
                }
                number = v;
            }
            if (f.enumType != null && f.enumType.nameOf(number) != null) {
                setEnum(m, f, slot, number);
                return;
            }
            // Unknown numbers are kept only by open enums, like the wire.
            if (f.enumType == null || f.enumClosed) {
                fail("enum value out of range");
            }
            setEnum(m, f, slot, number);
        }

        private void setEnum(GenericCodec.DMsg m, GenericCodec.Field f, int slot,
                int number) {
            if (f.repeated) {
                m.add(f.number, Integer.valueOf(number));
            } else {
                m.set(f.number, Integer.valueOf(number));
            }
        }

        Object intValue(GenericCodec.Field f) {
            spaces();
            boolean negative = false;
            if (pos < s.length() && s.charAt(pos) == '-') {
                negative = true;
                pos++;
            }
            Num n = parseNumber();
            if (n == null) {
                fail("expected a number");
            }
            switch (f.kind) {
                case GenericCodec.K_BOOL: {
                    if (negative) {
                        fail("bad bool");
                    }
                    return Long.valueOf(parseUint(n, 1));
                }
                case GenericCodec.K_INT32:
                case GenericCodec.K_SINT32:
                case GenericCodec.K_SFIXED32:
                    return Long.valueOf(signed(n, negative, 32));
                case GenericCodec.K_INT64:
                case GenericCodec.K_SINT64:
                case GenericCodec.K_SFIXED64:
                    return Long.valueOf(signed(n, negative, 64));
                case GenericCodec.K_UINT32:
                case GenericCodec.K_FIXED32:
                    if (negative) {
                        fail("unsigned field cannot be negative");
                    }
                    return Long.valueOf(parseUint(n, 0xFFFFFFFFL));
                case GenericCodec.K_UINT64:
                case GenericCodec.K_FIXED64:
                    if (negative) {
                        fail("unsigned field cannot be negative");
                    }
                    return Long.valueOf(parseUint(n, -1L));
                default:
                    fail("bad integer field");
                    return null;
            }
        }

        Object boolValue() {
            spaces();
            if (pos < s.length() && (isDigit(s.charAt(pos))
                    || s.charAt(pos) == '-')) {
                boolean negative = s.charAt(pos) == '-';
                if (negative) {
                    fail("bad bool");
                }
                Num n = parseNumber();
                if (n == null) {
                    fail("expected a bool");
                }
                return Long.valueOf(parseUint(n, 1));
            }
            String word = ident();
            if (word == null) {
                fail("expected a bool");
            }
            if ("true".equals(word) || "True".equals(word) || "t".equals(word)) {
                return Boolean.TRUE;
            }
            if ("false".equals(word) || "False".equals(word) || "f".equals(word)) {
                return Boolean.FALSE;
            }
            fail("bad bool \"" + word + "\"");
            return null;
        }

        Object floatValue(boolean isFloat) {
            spaces();
            boolean negative = false;
            if (pos < s.length() && s.charAt(pos) == '-') {
                negative = true;
                pos++;
                spaces();
            }
            double value;
            if (pos < s.length() && (isDigit(s.charAt(pos))
                    || (s.charAt(pos) == '.' && pos + 1 < s.length()
                    && isDigit(s.charAt(pos + 1))))) {
                Num n = parseNumber();
                if (n == null) {
                    fail("bad number");
                }
                if (n.hex || n.oct) {
                    fail("hex and octal float literals are invalid");
                }
                String text = n.text;
                if (n.kind == N_FLOAT && text.endsWith("f")) {
                    text = text.substring(0, text.length() - 1);
                }
                Double d = atod(text);
                if (d == null) {
                    fail("bad number \"" + n.text + "\"");
                }
                value = d;
            } else {
                String word = ident();
                if (word == null) {
                    fail("expected a number");
                }
                String lower = word.toLowerCase();
                if (lower.equals("inf") || lower.equals("infinity")) {
                    value = Double.POSITIVE_INFINITY;
                } else if (lower.equals("nan")) {
                    value = Double.NaN;
                } else {
                    fail("bad float \"" + word + "\"");
                    return null;
                }
            }
            if (negative) {
                value = -value;
            }
            return isFloat ? (Object) Float.valueOf((float) value)
                    : (Object) Double.valueOf(value);
        }

        String stringValue() {
            java.io.ByteArrayOutputStream out = new java.io.ByteArrayOutputStream();
            while (true) {
                int[] span = stringSpan();
                if (span == null) {
                    fail("expected a string");
                }
                appendUtf8Bytes(out, span[0], span[1]);
                int save = pos;
                spaces();
                if (pos >= s.length() || (s.charAt(pos) != '"'
                        && s.charAt(pos) != '\'')) {
                    pos = save;
                    break;
                }
            }
            return utf8Strict(out.toByteArray());
        }

        /** Decodes the escaped bytes as UTF-8, rejecting malformed input the
         *  way the core's text parser does (an octal/hex escape may produce
         *  a byte with no valid UTF-8 continuation). */
        private String utf8Strict(byte[] bytes) {
            java.nio.charset.CharsetDecoder dec =
                    java.nio.charset.StandardCharsets.UTF_8.newDecoder()
                            .onMalformedInput(java.nio.charset.CodingErrorAction.REPORT)
                            .onUnmappableCharacter(
                                    java.nio.charset.CodingErrorAction.REPORT);
            try {
                return dec.decode(java.nio.ByteBuffer.wrap(bytes)).toString();
            } catch (java.nio.charset.CharacterCodingException e) {
                fail("bad UTF-8 in a string literal");
                return null;
            }
        }

        /** Like appendUnescaped, into bytes: a raw char contributes its UTF-8
         *  bytes, escapes contribute their byte value or code point. */
        private void appendUtf8Bytes(java.io.ByteArrayOutputStream out,
                int from, int to) {
            int i = from + 1;
            int end = to - 1;
            while (i < end) {
                char c = s.charAt(i++);
                if (c != '\\') {
                    if (c < 0x80) {
                        out.write(c);
                    } else {
                        byte[] u = String.valueOf(c)
                                .getBytes(java.nio.charset.StandardCharsets.UTF_8);
                        out.write(u, 0, u.length);
                    }
                    continue;
                }
                i--;
                int code = escapeAt(i, end);
                if (code >= 0) {
                    out.write(code);
                } else {
                    byte[] u = new String(Character.toChars(pendingCodePoint))
                            .getBytes(java.nio.charset.StandardCharsets.UTF_8);
                    out.write(u, 0, u.length);
                }
                i = escapeEnd;
            }
        }

        byte[] bytesValue() {
            java.io.ByteArrayOutputStream out = new java.io.ByteArrayOutputStream();
            while (true) {
                int[] span = stringSpan();
                if (span == null) {
                    fail("expected a string");
                }
                appendUnescapedBytes(out, span[0], span[1]);
                int save = pos;
                spaces();
                if (pos >= s.length() || (s.charAt(pos) != '"'
                        && s.charAt(pos) != '\'')) {
                    pos = save;
                    break;
                }
            }
            return out.toByteArray();
        }

        private void appendUnescapedBytes(java.io.ByteArrayOutputStream out,
                int from, int to) {
            int i = from + 1;
            int end = to - 1;
            while (i < end) {
                char c = s.charAt(i++);
                if (c != '\\') {
                    if (c > 0xFF) {
                        fail("bad byte in a byte string");
                    }
                    out.write(c);
                    continue;
                }
                i--;
                int code = escapeAt(i, end);
                if (code >= 0) {
                    out.write(code);
                } else {
                    byte[] u = new String(Character.toChars(pendingCodePoint))
                            .getBytes(java.nio.charset.StandardCharsets.UTF_8);
                    out.write(u, 0, u.length);
                }
                i = escapeEnd;
            }
        }

        int escapeEnd;
        int pendingCodePoint;

        /**
         * Decodes one escape at `i` (which points at the backslash); returns
         * the byte value, or -1 when the value is a code point in
         * {@link #pendingCodePoint}.
         */
        private int escapeAt(int i, int end) {
            i++; // past the backslash
            if (i >= end) {
                fail("truncated escape");
            }
            char c = s.charAt(i++);
            escapeEnd = i;
            if (c >= '0' && c <= '7') {
                int v = c - '0';
                for (int k = 0; k < 2 && i < end && s.charAt(i) >= '0'
                        && s.charAt(i) <= '7'; k++) {
                    v = v * 8 + (s.charAt(i++) - '0');
                }
                escapeEnd = i;
                return v & 0xFF;
            }
            if (c == 'x' || c == 'X') {
                int v = 0;
                int digits = 0;
                while (digits < 2 && i < end && isHex(s.charAt(i))) {
                    v = v * 16 + Character.digit(s.charAt(i++), 16);
                    digits++;
                }
                if (digits == 0) {
                    fail("bad hex escape");
                }
                escapeEnd = i;
                return v & 0xFF;
            }
            if (c == 'u' || c == 'U') {
                int want = c == 'u' ? 4 : 8;
                long v = 0;
                for (int k = 0; k < want; k++) {
                    if (i >= end || !isHex(s.charAt(i))) {
                        fail("bad unicode escape");
                    }
                    v = v * 16 + Character.digit(s.charAt(i++), 16);
                }
                if (c == 'u' && v >= 0xd800 && v <= 0xdbff && i + 5 < end
                        && s.charAt(i) == '\\' && s.charAt(i + 1) == 'u') {
                    long trail = 0;
                    int k = 0;
                    for (; k < 4; k++) {
                        if (!isHex(s.charAt(i + 2 + k))) {
                            break;
                        }
                        trail = trail * 16 + Character.digit(s.charAt(i + 2 + k), 16);
                    }
                    if (k == 4 && trail >= 0xdc00 && trail <= 0xdfff) {
                        v = 0x10000 + ((v - 0xd800) << 10) + (trail - 0xdc00);
                        i += 6;
                    }
                }
                if (v > 0x10ffff || (v >= 0xd800 && v <= 0xdfff)) {
                    fail("bad unicode escape");
                }
                escapeEnd = i;
                if (v < 0x80 && want == 4) {
                    return (int) v;
                }
                pendingCodePoint = (int) v;
                return -1;
            }
            switch (c) {
                case 'a':
                    return 7;
                case 'b':
                    return '\b';
                case 'f':
                    return '\f';
                case 'n':
                    return '\n';
                case 'r':
                    return '\r';
                case 't':
                    return '\t';
                case 'v':
                    return 11;
                case '\\':
                    return '\\';
                case '\'':
                    return '\'';
                case '"':
                    return '"';
                case '?':
                    return '?';
                default:
                    fail("bad escape \\" + c);
                    return 0;
            }
        }

        /* ---- brackets: extensions and Any expansions ---- */

        void bracketField(GenericCodec.DMsg m, int depth) {
            String raw = brackets();
            if (isAny(m.def)) {
                anyExpansion(m, raw, depth);
                return;
            }
            if (raw.indexOf('/') >= 0 || !validTypeName(raw)) {
                fail("bad extension name");
            }
            GenericCodec.Schema schema = m.def.schema;
            GenericCodec.Field ext = schema == null ? null : schema.extension(raw);
            if (ext == null || m.def.lookup(ext.number) < 0
                    || m.def.fields[m.def.lookup(ext.number)] != ext) {
                throw new PbwireException("text: unknown extension \""
                        + raw + "\" in " + m.def.fullName, 3);
            }
            int slot = m.def.lookup(ext.number);
            namedField(m, slot, depth);
        }

        private static boolean isAny(GenericCodec.MessageDef def) {
            return def != null && "google.protobuf.Any".equals(def.fullName);
        }

        /** {@code [url]{ fields }}: the message IS the Any, not a field. */
        void anyExpansion(GenericCodec.DMsg m, String url, int depth) {
            int urlSlot = m.def.lookup(1);
            if (urlSlot >= 0 && m.has[urlSlot]) {
                fail("second Any expansion");
            }
            int slash = url.lastIndexOf('/');
            if (slash <= 0) {
                fail("bad Any type URL");
            }
            for (int i = 0; i < slash; i++) {
                if (url.charAt(i) == '%') {
                    if (i + 2 >= slash || !isHex(url.charAt(i + 1))
                            || !isHex(url.charAt(i + 2))) {
                        fail("bad Any type URL escape");
                    }
                    i += 2;
                }
            }
            String typeName = url.substring(slash + 1);
            if (!validTypeName(typeName)) {
                fail("bad Any type name");
            }
            GenericCodec.Schema schema = m.def.schema;
            GenericCodec.MessageDef target = schema == null ? null
                    : schema.byName.get(typeName);
            if (target == null || schema.enums.containsKey(typeName)) {
                fail("Any type is not resolvable: " + url);
            }
            spaces();
            if (pos < s.length() && s.charAt(pos) == ':') {
                pos++;
            }
            spaces();
            if (eof() || (peek() != '{' && peek() != '<')) {
                fail("expected '{'");
            }
            char open = s.charAt(pos++);
            GenericCodec.DMsg sub = new GenericCodec.DMsg(target);
            messageBody(sub, depth + 1, open == '{' ? '}' : '>');
            byte[] payload = GenericCodec.encode(sub);
            m.set(1, url);
            if (payload.length > 0) {
                m.set(2, payload);
            }
        }

        /** Raw bracket contents: URL characters only, whitespace skipped. */
        String brackets() {
            spaces();
            if (pos >= s.length() || s.charAt(pos) != '[') {
                fail("expected '['");
            }
            pos++;
            StringBuilder sb = new StringBuilder();
            while (true) {
                spaces();
                if (pos >= s.length()) {
                    fail("truncated '['");
                }
                char c = s.charAt(pos);
                if (c == ']') {
                    pos++;
                    return sb.toString();
                }
                if (!isUrlChar(c)) {
                    fail("bad character in '['");
                }
                sb.append(c);
                pos++;
            }
        }

        /* ---- skipping ---- */

        /** Skips one value of unknown type (reserved field names). */
        void skipValue(int depth) {
            spaces();
            if (pos >= s.length()) {
                fail("expected a value");
            }
            if (s.charAt(pos) == '{' || s.charAt(pos) == '<') {
                char closer = s.charAt(pos) == '{' ? '}' : '>';
                if (depth >= cap) {
                    throw new PbwireException("text: nesting exceeds " + cap, 5);
                }
                pos++;
                while (true) {
                    spaces();
                    if (pos >= s.length()) {
                        fail("truncated message");
                    }
                    if (s.charAt(pos) == closer) {
                        pos++;
                        return;
                    }
                    if (s.charAt(pos) == '[') {
                        brackets();
                    } else if (ident() == null) {
                        fail("expected a field name");
                    }
                    spaces();
                    if (pos < s.length() && (s.charAt(pos) == '{'
                            || s.charAt(pos) == '<')) {
                        skipValue(depth + 1);
                    } else {
                        if (pos >= s.length() || s.charAt(pos) != ':') {
                            fail("expected ':'");
                        }
                        pos++;
                        skipValue(depth + 1);
                    }
                    spaces();
                    if (pos < s.length() && (s.charAt(pos) == ','
                            || s.charAt(pos) == ';')) {
                        pos++;
                    }
                }
            }
            if (s.charAt(pos) == '[') {
                if (depth >= cap) {
                    throw new PbwireException("text: nesting exceeds " + cap, 5);
                }
                pos++;
                spaces();
                if (pos < s.length() && s.charAt(pos) == ']') {
                    pos++;
                    return;
                }
                while (true) {
                    skipValue(depth + 1);
                    spaces();
                    if (pos < s.length() && s.charAt(pos) == ',') {
                        pos++;
                        continue;
                    }
                    if (pos < s.length() && s.charAt(pos) == ']') {
                        pos++;
                        return;
                    }
                    fail("expected ',' or ']'");
                }
            }
            if (stringSpan() != null) {
                while (true) {
                    int save = pos;
                    spaces();
                    if (pos >= s.length() || (s.charAt(pos) != '"'
                            && s.charAt(pos) != '\'')) {
                        pos = save;
                        return;
                    }
                    if (stringSpan() == null) {
                        fail("bad string");
                    }
                }
            }
            if (pos < s.length() && s.charAt(pos) == '-') {
                pos++;
                spaces();
            }
            if (parseNumber() != null) {
                return;
            }
            if (ident() != null) {
                return;
            }
            fail("expected a value");
        }
    }

    /* ---------------- scalar text helpers ---------------- */

    /** protoc field-name lookup: the name, then a group's lowercased type name. */
    private static int lookup(GenericCodec.MessageDef def, String name) {
        Integer exact = def.byName.get(name);
        if (exact != null && !def.fields[exact].extension) {
            return exact;
        }
        String lower = name.toLowerCase(java.util.Locale.ROOT);
        Integer lc = def.byName.get(lower);
        if (lc != null) {
            GenericCodec.Field g = def.fields[lc];
            if (!g.extension && g.kind == GenericCodec.K_GROUP && g.msgType != null
                    && shortName(g.msgType.fullName).equals(name)) {
                return lc;
            }
        }
        for (int s = 0; s < def.fields.length; s++) {
            GenericCodec.Field g = def.fields[s];
            if (g.extension || g.kind != GenericCodec.K_GROUP || g.msgType == null) {
                continue;
            }
            if (shortName(g.msgType.fullName).equals(name)) {
                return s;
            }
        }
        return -1;
    }

    private static String shortName(String fq) {
        int dot = fq.lastIndexOf('.');
        return dot < 0 ? fq : fq.substring(dot + 1);
    }

    private static boolean validTypeName(String s) {
        if (s.isEmpty()) {
            return false;
        }
        int i = 0;
        while (true) {
            if (i >= s.length() || !isIdentStart(s.charAt(i))) {
                return false;
            }
            while (i < s.length() && isIdentChar(s.charAt(i))) {
                i++;
            }
            if (i == s.length()) {
                return true;
            }
            if (s.charAt(i) != '.') {
                return false;
            }
            i++;
        }
    }

    private static boolean isIdentStart(char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
    }

    private static boolean isIdentChar(char c) {
        return isIdentStart(c) || isDigit(c);
    }

    private static boolean isDigit(char c) {
        return c >= '0' && c <= '9';
    }

    private static boolean isHex(char c) {
        return isDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    }

    private static boolean isUrlChar(char c) {
        return isIdentChar(c) || c == '-' || c == '.' || c == '~' || c == '!'
                || c == '$' || c == '&' || c == '(' || c == ')' || c == '*'
                || c == '+' || c == ',' || c == ';' || c == '=' || c == '%'
                || c == '/';
    }

    /** strtod with full consumption; null on failure. */
    private static Double atod(String text) {
        String u = text.trim();
        if (u.isEmpty()) {
            return null;
        }
        try {
            return Double.valueOf(Double.parseDouble(u));
        } catch (NumberFormatException e) {
            return null;
        }
    }
}

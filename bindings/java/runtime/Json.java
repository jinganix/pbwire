import java.util.ArrayList;
import java.util.Base64;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Map;
import java.util.Set;

/**
 * Protobuf JSON mapping over the dynamic tree: prints the canonical encoding
 * the reference implementation produces and parses what its parser accepts in
 * default (legacy) mode. Mirrors the C core's {@code src/json.c}, which is the
 * authority for every semantic here.
 *
 * <p>Field names print as their camelCase ({@code json_name}) spelling,
 * extensions as {@code [full.name]}, 64-bit integers quoted, bytes as standard
 * base64, enums by name, floats in the shortest round-tripping form
 * (non-finite values quoted), maps as objects with duplicate keys collapsed to
 * the last entry, and the well-known types in their JSON form ({@code @type}
 * expansions, RFC 3339 timestamps, {@code "3.2s"} durations, {@code Struct} as
 * a nested object, {@code FieldMask} as one camelCase string, wrappers as bare
 * values). Unknown fields are not printed.
 *
 * <p>The parser accepts proto and camelCase names, quoted or bare 64-bit
 * integers, numbers with exponential or fractional spelling, quoted bools,
 * single-quoted and bare keys, trailing commas, {@code null} as unset, the
 * object form of well-known types, base64 with either alphabet and optional
 * padding, unknown closed-enum numbers diverted to unknown fields (like the
 * binary decoder), and duplicate keys except inside oneofs and maps. Anything
 * else unknown fails unless told to ignore it.
 */
public final class Json {
    private Json() {
    }

    /** Prints a message in the JSON mapping. */
    public static String print(GenericCodec.DMsg m) {
        Out out = new Out();
        printMessage(out, m);
        return out.sb.toString();
    }

    /** Parses JSON into a new tree; unknown fields fail. */
    public static GenericCodec.DMsg parse(GenericCodec.MessageDef def, String json) {
        return parse(def, json, false, Limits.defaults());
    }

    /** Parses JSON, optionally skipping unknown field names. */
    public static GenericCodec.DMsg parse(GenericCodec.MessageDef def, String json,
            boolean ignoreUnknown) {
        return parse(def, json, ignoreUnknown, Limits.defaults());
    }

    /** Same with explicit bounds; arrays cost a level like objects do. */
    public static GenericCodec.DMsg parse(GenericCodec.MessageDef def, String json,
            boolean ignoreUnknown, Limits lim) {
        if (lim == null) {
            lim = Limits.defaults();
        }
        lim.checkBytes(json.length(), "JSON");
        Parser p = new Parser(json, lim.effectiveDepth(), ignoreUnknown);
        GenericCodec.DMsg m = p.newMessage(def);
        p.intoMessage(m);
        p.spaces();
        if (!p.eof()) {
            throw new PbwireException("JSON: trailing bytes", 1);
        }
        if (def.deepRequired) {
            try {
                GenericCodec.checkRequired(m, lim.effectiveDepth(), 0);
            } catch (IllegalArgumentException e) {
                throw new PbwireException("JSON: " + e.getMessage(), 6);
            }
        }
        return m;
    }

    /** protoc's ToJsonName: drops every '_', uppercases the next letter. */
    static String jsonName(String name) {
        StringBuilder sb = new StringBuilder(name.length());
        boolean upper = false;
        for (int i = 0; i < name.length(); i++) {
            char c = name.charAt(i);
            if (c == '_') {
                upper = true;
                continue;
            }
            sb.append(upper ? Character.toUpperCase(c) : c);
            upper = false;
        }
        return sb.toString();
    }

    /** protoc's ToCamelCase with a lowercased first letter: the parse spelling. */
    private static String camelName(String name) {
        StringBuilder sb = new StringBuilder(name.length());
        boolean upper = true;
        for (int i = 0; i < name.length(); i++) {
            char c = name.charAt(i);
            if (c == '_') {
                upper = true;
                continue;
            }
            c = upper ? Character.toUpperCase(c) : c;
            upper = false;
            if (sb.length() == 0) {
                c = Character.toLowerCase(c);
            }
            sb.append(c);
        }
        return sb.toString();
    }

    /* ---------------- well-known-type routing ---------------- */

    private static final int W_NONE = 0, W_ANY = 1, W_TIMESTAMP = 2,
            W_DURATION = 3, W_STRUCT = 4, W_VALUE = 5, W_LIST = 6,
            W_FIELDMASK = 7, W_WRAPPER = 8, W_EMPTY = 9;

    private static final String[] WKT_NAMES = {
        "google.protobuf.Any", "google.protobuf.Timestamp",
        "google.protobuf.Duration", "google.protobuf.Struct",
        "google.protobuf.Value", "google.protobuf.ListValue",
        "google.protobuf.FieldMask", "google.protobuf.Empty",
    };
    private static final int[] WKT_KINDS = {
        W_ANY, W_TIMESTAMP, W_DURATION, W_STRUCT, W_VALUE, W_LIST, W_FIELDMASK,
        W_EMPTY,
    };

    /** Wrappers print as their bare value, so they share one kind. */
    private static final String[] WRAPPER_NAMES = {
        "google.protobuf.DoubleValue", "google.protobuf.FloatValue",
        "google.protobuf.Int64Value", "google.protobuf.UInt64Value",
        "google.protobuf.Int32Value", "google.protobuf.UInt32Value",
        "google.protobuf.BoolValue", "google.protobuf.StringValue",
        "google.protobuf.BytesValue",
    };

    private static final String NULL_VALUE = "google.protobuf.NullValue";

    private static int classify(GenericCodec.MessageDef def) {
        if (def == null) {
            return W_NONE;
        }
        String name = def.fullName;
        for (int i = 0; i < WKT_NAMES.length; i++) {
            if (WKT_NAMES[i].equals(name)) {
                return WKT_KINDS[i];
            }
        }
        for (String w : WRAPPER_NAMES) {
            if (w.equals(name)) {
                return W_WRAPPER;
            }
        }
        return W_NONE;
    }

    private static boolean isNullValue(GenericCodec.Field f) {
        return f.enumType != null && NULL_VALUE.equals(f.enumType.fullName);
    }

    /* ---------------- tree helpers ---------------- */

    private static boolean has(GenericCodec.DMsg m, int slot) {
        return m.has[slot];
    }

    private static int repLen(GenericCodec.DMsg m, int slot) {
        GenericCodec.Rep rp = m.rep;
        if (rp == null) {
            return 0;
        }
        return m.def.fields[slot].kind <= GenericCodec.K_DOUBLE
                ? rp.numLen[slot] : rp.refLen[slot];
    }

    private static Object repItem(GenericCodec.DMsg m, int slot, int i) {
        GenericCodec.Field f = m.def.fields[slot];
        GenericCodec.Rep rp = m.rep;
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

    /** A singular field's value, materialized; null when absent. */
    private static Object value(GenericCodec.DMsg m, int slot) {
        GenericCodec.Field f = m.def.fields[slot];
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

    private static long numValue(GenericCodec.DMsg m, int slot) {
        return m.has[slot] ? m.num[slot] : 0;
    }

    private static String stringValue(GenericCodec.DMsg m, int slot) {
        Object v = has(m, slot) ? value(m, slot) : null;
        return v instanceof String ? (String) v : "";
    }

    private static byte[] bytesValue(GenericCodec.DMsg m, int slot) {
        Object v = has(m, slot) ? value(m, slot) : null;
        return v instanceof byte[] ? (byte[]) v : null;
    }

    private static GenericCodec.DMsg messageValue(GenericCodec.DMsg m, int slot) {
        Object v = has(m, slot) ? value(m, slot) : null;
        return v instanceof GenericCodec.DMsg ? (GenericCodec.DMsg) v : null;
    }

    /** Whether a message declares no field as present (an empty {@code Value}). */
    private static boolean isEmptyMessage(GenericCodec.DMsg m) {
        for (int s = 0; s < m.def.fields.length; s++) {
            if (has(m, s)) {
                return false;
            }
        }
        return true;
    }

    /* ---------------- print ---------------- */

    private static final class Out {
        final StringBuilder sb = new StringBuilder();
        int depth; // message nesting, capped at Wire.MAX_DEPTH
    }

    private static void printMessage(Out out, GenericCodec.DMsg m) {
        if (out.depth >= Wire.MAX_DEPTH) {
            throw new PbwireException("JSON: nesting exceeds " + Wire.MAX_DEPTH, 5);
        }
        out.depth++;
        printMessageBody(out, m);
        out.depth--;
    }

    private static void printMessageBody(Out out, GenericCodec.DMsg m) {
        switch (classify(m.def)) {
            case W_ANY:
                printAny(out, m);
                return;
            case W_TIMESTAMP:
                quoteRaw(out, timestamp(out, m));
                return;
            case W_DURATION:
                quoteRaw(out, duration(out, m));
                return;
            case W_FIELDMASK:
                printFieldMask(out, m);
                return;
            case W_STRUCT:
                printStruct(out, m);
                return;
            case W_LIST:
                printListValue(out, m);
                return;
            case W_VALUE:
                printValue(out, m);
                return;
            case W_WRAPPER: {
                int slot = slotOf(m.def, 1);
                GenericCodec.Field inner = m.def.fields[slot];
                Object v = m.has[slot] ? value(m, slot) : null;
                if (inner.kind == GenericCodec.K_STRING) {
                    quoteChecked(out, v instanceof String ? (String) v : "");
                    return;
                }
                if (inner.kind == GenericCodec.K_BYTES) {
                    quoteRaw(out, base64(v instanceof byte[] ? (byte[]) v : new byte[0]));
                    return;
                }
                printScalar(out, inner, m.has[slot] ? m.num[slot] : 0);
                return;
            }
            case W_EMPTY:
                out.sb.append("{}");
                return;
            default:
                break;
        }
        out.sb.append('{');
        boolean first = true;
        for (int s = 0; s < m.def.fields.length; s++) {
            GenericCodec.Field f = m.def.fields[s];
            if (!shouldPrint(m, s)) {
                continue;
            }
            if (!first) {
                out.sb.append(',');
            }
            first = false;
            quoteRaw(out, keyName(f));
            out.sb.append(':');
            if (f.repeated) {
                int n = repLen(m, s);
                if (f.kind == GenericCodec.K_MESSAGE && f.msgType.mapEntry) {
                    printMap(out, f, m, s);
                } else {
                    out.sb.append('[');
                    for (int i = 0; i < n; i++) {
                        if (i > 0) {
                            out.sb.append(',');
                        }
                        printElement(out, f, m, s, i);
                    }
                    out.sb.append(']');
                }
                continue;
            }
            printElement(out, f, m, s, -1);
        }
        out.sb.append('}');
    }

    private static String keyName(GenericCodec.Field f) {
        return f.extension && f.fullName != null
                ? "[" + f.fullName + "]" : jsonName(f.fieldName);
    }

    /** Whether a field prints: proto3 drops defaults, presence prints. */
    private static boolean shouldPrint(GenericCodec.DMsg m, int s) {
        if (!has(m, s)) {
            return false;
        }
        GenericCodec.Field f = m.def.fields[s];
        if (f.repeated) {
            return repLen(m, s) > 0;
        }
        if (f.kind == GenericCodec.K_MESSAGE || f.kind == GenericCodec.K_GROUP) {
            return true;
        }
        if (f.presence == GenericCodec.IMPLICIT) {
            return !isDefault(f, m.num[s], m.ref[s]);
        }
        return true;
    }

    private static boolean isDefault(GenericCodec.Field f, long num, Object ref) {
        if (f.kind <= GenericCodec.K_DOUBLE) {
            return num == 0;
        }
        if (f.kind == GenericCodec.K_STRING) {
            return ref instanceof String && ((String) ref).isEmpty();
        }
        if (f.kind == GenericCodec.K_BYTES) {
            return ref instanceof byte[] && ((byte[]) ref).length == 0;
        }
        return false;
    }

    /** Prints one field value: a repeated element when i >= 0, else the field. */
    private static void printElement(Out out, GenericCodec.Field f,
            GenericCodec.DMsg m, int s, int i) {
        Object ref = i >= 0 ? repItem(m, s, i)
                : (f.kind <= GenericCodec.K_DOUBLE ? null : value(m, s));
        long bits = f.kind <= GenericCodec.K_DOUBLE
                ? (i >= 0 ? m.rep.num[s][i] : m.num[s]) : 0;
        if (f.kind <= GenericCodec.K_DOUBLE) {
            printScalar(out, f, bits);
            return;
        }
        switch (f.kind) {
            case GenericCodec.K_STRING:
                quoteChecked(out, (String) ref);
                return;
            case GenericCodec.K_BYTES:
                quoteRaw(out, base64((byte[]) ref));
                return;
            default:
                printMessage(out, (GenericCodec.DMsg) ref);
                return;
        }
    }

    private static void printEnum(Out out, GenericCodec.Field f, long v) {
        int number = (int) v;
        if (isNullValue(f)) {
            out.sb.append("null"); // the canonical spelling of NullValue
            return;
        }
        String name = f.enumType == null ? null : f.enumType.nameOf(number);
        if (name != null) {
            quoteRaw(out, name);
            return;
        }
        out.sb.append(number); // an open enum keeps unknown numbers
    }

    private static void printNum(Out out, int kind, long bits) {
        switch (kind) {
            case GenericCodec.K_INT32:
            case GenericCodec.K_SINT32:
            case GenericCodec.K_SFIXED32:
                out.sb.append((int) bits);
                return;
            case GenericCodec.K_UINT32:
            case GenericCodec.K_FIXED32:
                out.sb.append(bits & 0xFFFFFFFFL);
                return;
            case GenericCodec.K_INT64:
            case GenericCodec.K_SINT64:
            case GenericCodec.K_SFIXED64:
                out.sb.append('"').append(bits).append('"');
                return;
            case GenericCodec.K_UINT64:
            case GenericCodec.K_FIXED64:
                out.sb.append('"').append(Long.toUnsignedString(bits))
                        .append('"');
                return;
            case GenericCodec.K_BOOL:
                out.sb.append(bits != 0 ? "true" : "false");
                return;
            case GenericCodec.K_FLOAT: {
                float f = Float.intBitsToFloat((int) bits);
                if (Float.isNaN(f)) {
                    out.sb.append("\"NaN\"");
                } else if (f == Float.POSITIVE_INFINITY) {
                    out.sb.append("\"Infinity\"");
                } else if (f == Float.NEGATIVE_INFINITY) {
                    out.sb.append("\"-Infinity\"");
                } else {
                    out.sb.append(Float.toString(f));
                }
                return;
            }
            default: {
                double d = Double.longBitsToDouble(bits);
                if (Double.isNaN(d)) {
                    out.sb.append("\"NaN\"");
                } else if (d == Double.POSITIVE_INFINITY) {
                    out.sb.append("\"Infinity\"");
                } else if (d == Double.NEGATIVE_INFINITY) {
                    out.sb.append("\"-Infinity\"");
                } else {
                    out.sb.append(Double.toString(d));
                }
                return;
            }
        }
    }

    private static void printScalar(Out out, GenericCodec.Field f, long bits) {
        if (f.kind == GenericCodec.K_ENUM) {
            printEnum(out, f, bits);
            return;
        }
        printNum(out, f.kind, bits);
    }

    private static void printMap(Out out, GenericCodec.Field f,
            GenericCodec.DMsg m, int s) {
        int entryKey = f.msgType.lookup(1);
        int entryValue = f.msgType.lookup(2);
        if (entryKey < 0 || entryValue < 0) {
            throw new PbwireException("JSON: invalid map entry "
                    + f.msgType.fullName, 1);
        }
        GenericCodec.Field keyField = f.msgType.fields[entryKey];
        GenericCodec.Field valueField = f.msgType.fields[entryValue];
        int n = repLen(m, s);
        // Keys normalize to their JSON spelling (a byte[] key would otherwise
        // compare by identity): a later entry with the same key wins, and a
        // message value merges every entry carrying it, like the wire.
        Map<String, Integer> last = new HashMap<>();
        for (int i = 0; i < n; i++) {
            GenericCodec.DMsg entry = (GenericCodec.DMsg) repItem(m, s, i);
            Object key = entry.has[entryKey] ? value(entry, entryKey) : null;
            last.put(mapKeyString(keyField, key), i);
        }
        out.sb.append('{');
        boolean first = true;
        for (int i = 0; i < n; i++) {
            GenericCodec.DMsg entry = (GenericCodec.DMsg) repItem(m, s, i);
            Object key = entry.has[entryKey] ? value(entry, entryKey) : null;
            String kstr = mapKeyString(keyField, key);
            Integer survivor = last.get(kstr);
            if (survivor != null && survivor != i) {
                continue; // a later entry with the same key wins
            }
            boolean valIsMsg = valueField.kind == GenericCodec.K_MESSAGE
                    || valueField.kind == GenericCodec.K_GROUP;
            GenericCodec.DMsg val = valIsMsg && entry.has[entryValue]
                    ? (GenericCodec.DMsg) value(entry, entryValue) : null;

            if (val != null && classify(val.def) == W_VALUE && isEmptyMessage(val)) {
                continue; // an empty Value entry prints nothing, like the reference
            }
            if (!first) {
                out.sb.append(',');
            }
            first = false;
            quoteRaw(out, mapKeyString(keyField, key));
            out.sb.append(':');
            printMapValue(out, valueField, entry, entryValue, val);
        }
        out.sb.append('}');
    }

    /** One map value: scalars from num[], strings/bytes from ref[], messages
     *  nested. An absent value prints its kind's default, like the reference. */
    private static void printMapValue(Out out, GenericCodec.Field f,
            GenericCodec.DMsg entry, int slot, GenericCodec.DMsg sub) {
        if (f.kind == GenericCodec.K_STRING) {
            Object v = entry.has[slot] ? value(entry, slot) : null;
            quoteChecked(out, v instanceof String ? (String) v : "");
            return;
        }
        if (f.kind == GenericCodec.K_BYTES) {
            Object v = entry.has[slot] ? value(entry, slot) : null;
            quoteRaw(out, base64(v instanceof byte[] ? (byte[]) v : new byte[0]));
            return;
        }
        if (f.kind == GenericCodec.K_MESSAGE || f.kind == GenericCodec.K_GROUP) {
            if (sub != null) {
                printMessage(out, sub);
            } else {
                out.sb.append("{}");
            }
            return;
        }
        printScalar(out, f, entry.has[slot] ? entry.num[slot] : 0);
    }

    /** Map keys print as JSON object keys: decimal, "true"/"false" or text. */
    private static String mapKeyString(GenericCodec.Field f, Object key) {
        if (key == null) {
            // A missing key defaults; proto3 stores no default value, so a
            // false/""/0 key arrives absent and needs the kind's zero here.
            switch (f.kind) {
                case GenericCodec.K_STRING:
                    key = "";
                    break;
                case GenericCodec.K_BOOL:
                    key = Boolean.FALSE;
                    break;
                case GenericCodec.K_BYTES:
                    key = new byte[0];
                    break;
                default:
                    key = 0L;
                    break;
            }
        }
        switch (f.kind) {
            case GenericCodec.K_INT32:
            case GenericCodec.K_SINT32:
            case GenericCodec.K_SFIXED32:
            case GenericCodec.K_INT64:
            case GenericCodec.K_SINT64:
            case GenericCodec.K_SFIXED64:
                return Long.toString(((Number) key).longValue());
            case GenericCodec.K_UINT32:
            case GenericCodec.K_FIXED32:
                return Long.toString(((Number) key).longValue() & 0xFFFFFFFFL);
            case GenericCodec.K_UINT64:
            case GenericCodec.K_FIXED64:
                return Long.toUnsignedString(((Number) key).longValue());
            case GenericCodec.K_BOOL:
                return ((Boolean) key) ? "true" : "false";
            case GenericCodec.K_STRING:
                return (String) key;
            case GenericCodec.K_ENUM: {
                int v = ((Number) key).intValue();
                String name = f.enumType == null ? null : f.enumType.nameOf(v);
                return name != null ? name : Integer.toString(v);
            }
            default:
                throw new PbwireException("JSON: invalid map key type", 1);
        }
    }

    /** An RFC 3339 timestamp: 0/3/6/9 fractional digits, no offsets. */
    private static String timestamp(Out out, GenericCodec.DMsg m) {
        int secSecs = slotOf(m.def, 1);
        int secNanos = slotOf(m.def, 2);
        long secs = numValue(m, secSecs);
        int nanos = (int) numValue(m, secNanos);
        if (secs < -62135596800L || secs > 253402300799L || nanos < 0
                || nanos > 999999999) {
            throw new PbwireException("JSON: Timestamp out of range", 1);
        }
        // Julian Day -> Y/M/D, the same algorithm as the reference printer.
        secs += 62135596800L;
        int l = (int) (secs / 86400) - 719162 + 68569 + 2440588;
        int n = 4 * l / 146097;
        l = l - (146097 * n + 3) / 4;
        int i = 4000 * (l + 1) / 1461001;
        l = l - 1461 * i / 4 + 31;
        int jj = 80 * l / 2447;
        int k = l - 2447 * jj / 80;
        l = jj / 11;
        jj = jj + 2 - 12 * l;
        i = 100 * (n - 49) + i + l;
        int hour = (int) ((secs / 3600) % 24);
        int minute = (int) ((secs / 60) % 60);
        int second = (int) (secs % 60);
        StringBuilder sb = new StringBuilder();
        sb.append(String.format("%04d-%02d-%02dT%02d:%02d:%02d",
                i, jj, k, hour, minute, second));
        if (nanos != 0) {
            int frac = nanos;
            int digits = 9;
            while (frac % 1000 == 0) {
                frac /= 1000;
                digits -= 3;
            }
            sb.append('.');
            String f = Integer.toString(frac);
            for (int pad = f.length(); pad < digits; pad++) {
                sb.append('0');
            }
            sb.append(f);
        }
        return sb.append('Z').toString();
    }

    private static String duration(Out out, GenericCodec.DMsg m) {
        long secs = numValue(m, slotOf(m.def, 1));
        int nanos = (int) numValue(m, slotOf(m.def, 2));
        final long max = 3652500L * 86400;
        if (secs > max || secs < -max || nanos > 999999999 || nanos < -999999999
                || (secs != 0 && nanos != 0 && (secs < 0) != (nanos < 0))) {
            throw new PbwireException("JSON: Duration out of range", 1);
        }
        if (nanos == 0) {
            return secs + "s";
        }
        int frac = Math.abs(nanos);
        int digits = 9;
        while (frac % 1000 == 0) {
            frac /= 1000;
            digits -= 3;
        }
        StringBuilder sb = new StringBuilder();
        if (secs < 0 || nanos < 0) {
            sb.append('-');
        }
        sb.append(Math.abs(secs)).append('.');
        String f = Integer.toString(frac);
        for (int pad = f.length(); pad < digits; pad++) {
            sb.append('0');
        }
        return sb.append(f).append('s').toString();
    }

    /** Paths print camelCase, joined with commas. */
    private static void printFieldMask(Out out, GenericCodec.DMsg m) {
        int paths = slotOf(m.def, 1);
        StringBuilder sb = new StringBuilder();
        int n = repLen(m, paths);
        for (int i = 0; i < n; i++) {
            if (i > 0) {
                sb.append(',');
            }
            Object v = repItem(m, paths, i);
            if (!(v instanceof String)) {
                throw new PbwireException("JSON: FieldMask path is not a string", 1);
            }
            String p = (String) v;
            boolean sawUnder = false;
            for (int k = 0; k < p.length(); k++) {
                char c = p.charAt(k);
                if (c >= 'a' && c <= 'z' && sawUnder) {
                    sb.append((char) (c - 'a' + 'A'));
                } else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')
                        || c == '.') {
                    sb.append(c);
                } else if (c == '_') {
                    sawUnder = true;
                    continue;
                } else {
                    if (sawUnder) {
                        sb.append('_');
                    }
                    sb.append(c);
                }
                sawUnder = false;
            }
        }
        quoteRaw(out, sb.toString());
    }

    private static void printStruct(Out out, GenericCodec.DMsg m) {
        int fields = slotOf(m.def, 1);
        GenericCodec.Field entryField = m.def.fields[fields];
        int entryKey = entryField.msgType.lookup(1);
        int entryValue = entryField.msgType.lookup(2);
        out.sb.append('{');
        boolean first = true;
        int n = repLen(m, fields);
        for (int i = 0; i < n; i++) {
            GenericCodec.DMsg entry = (GenericCodec.DMsg) repItem(m, fields, i);
            Object key = entry.has[entryKey] ? value(entry, entryKey) : null;
            GenericCodec.DMsg val = entry.has[entryValue]
                    ? (GenericCodec.DMsg) value(entry, entryValue) : null;
            if (!(key instanceof String) || val == null) {
                throw new PbwireException("JSON: Struct entry is malformed", 1);
            }
            if (classify(val.def) == W_VALUE && isEmptyMessage(val)) {
                continue;
            }
            if (!first) {
                out.sb.append(',');
            }
            first = false;
            quoteRaw(out, (String) key);
            out.sb.append(':');
            printValue(out, val);
        }
        out.sb.append('}');
    }

    private static void printListValue(Out out, GenericCodec.DMsg m) {
        int values = slotOf(m.def, 1);
        out.sb.append('[');
        boolean first = true;
        int n = repLen(m, values);
        for (int i = 0; i < n; i++) {
            GenericCodec.DMsg item = (GenericCodec.DMsg) repItem(m, values, i);
            if (classify(item.def) == W_VALUE && isEmptyMessage(item)) {
                continue;
            }
            if (!first) {
                out.sb.append(',');
            }
            first = false;
            printValue(out, item);
        }
        out.sb.append(']');
    }

    private static void printValue(Out out, GenericCodec.DMsg m) {
        if (out.depth >= Wire.MAX_DEPTH) {
            throw new PbwireException("JSON: nesting exceeds " + Wire.MAX_DEPTH, 5);
        }
        out.depth++;
        int[] slots = valueSlots(m.def);
        for (int i = 0; i < slots.length; i++) {
            if (!has(m, slots[i])) {
                continue;
            }
            switch (i) {
                case 0: // null_value
                    out.sb.append("null");
                    out.depth--;
                    return;
                case 1: { // number_value
                    double d = Double.longBitsToDouble(m.num[slots[1]]);
                    if (Double.isNaN(d) || Double.isInfinite(d)) {
                        throw new PbwireException("JSON: Value number is not finite", 1);
                    }
                    out.sb.append(Double.toString(d));
                    out.depth--;
                    return;
                }
                case 2: // string_value
                    quoteChecked(out, (String) value(m, slots[2]));
                    out.depth--;
                    return;
                case 3: // bool_value
                    out.sb.append(m.num[slots[3]] != 0 ? "true" : "false");
                    out.depth--;
                    return;
                case 4: // struct_value
                    printStruct(out, (GenericCodec.DMsg) value(m, slots[4]));
                    out.depth--;
                    return;
                default: // list_value
                    printListValue(out, (GenericCodec.DMsg) value(m, slots[5]));
                    out.depth--;
                    return;
            }
        }
        throw new PbwireException("JSON: google.protobuf.Value has no value", 1);
    }

    private static int[] valueSlots(GenericCodec.MessageDef def) {
        int[] slots = new int[6];
        for (int i = 0; i < 6; i++) {
            slots[i] = slotOf(def, i + 1);
        }
        return slots;
    }

    /**
     * {@code {"@type": url}} plus the expanded payload, or the type under a
     * "value" key when the payload has its own JSON form.
     */
    private static void printAny(Out out, GenericCodec.DMsg m) {
        int urlSlot = slotOf(m.def, 1);
        int bytesSlot = slotOf(m.def, 2);
        String url = has(m, urlSlot) ? stringValue(m, urlSlot) : null;
        byte[] payload = bytesValue(m, bytesSlot);
        if (url == null) {
            if (payload != null) {
                throw new PbwireException("JSON: Any has a value without a type", 1);
            }
            out.sb.append("{}");
            return;
        }
        GenericCodec.Schema schema = m.def.schema;
        String name = url.substring(url.lastIndexOf('/') + 1);
        GenericCodec.MessageDef target = schema == null ? null
                : schema.byName.get(name);
        if (target == null || schema.enums.containsKey(name)) {
            throw new PbwireException("JSON: Any type is not resolvable: " + url, 1);
        }
        GenericCodec.DMsg decoded;
        try {
            decoded = GenericCodec.decode(target,
                    payload == null ? new byte[0] : payload, Limits.defaults());
        } catch (RuntimeException e) {
            throw new PbwireException("JSON: Any payload does not decode as "
                    + name, 1);
        }
        out.sb.append("{\"@type\":");
        quoteRaw(out, url);
        int wkt = classify(target);
        if (wkt != W_NONE && wkt != W_EMPTY) {
            out.sb.append(",\"value\":");
            printMessage(out, decoded);
        } else if (!isEmptyMessage(decoded)) {
            // Regular payloads expand inline: {"a":1} becomes ,"a":1.
            int mark = out.sb.length();
            printMessage(out, decoded);
            if (out.sb.length() >= mark + 2 && out.sb.charAt(mark) == '{'
                    && out.sb.charAt(out.sb.length() - 1) == '}') {
                out.sb.setCharAt(mark, ',');
                out.sb.setLength(out.sb.length() - 1);
            }
        }
        out.sb.append('}');
    }

    private static int slotOf(GenericCodec.MessageDef def, int number) {
        int s = def.lookup(number);
        if (s < 0) {
            throw new PbwireException("JSON: " + def.fullName
                    + " has no field " + number, 1);
        }
        return s;
    }

    private static String base64(byte[] b) {
        return Base64.getEncoder().encodeToString(b == null ? new byte[0] : b);
    }

    /** A string field must be valid Unicode text, like the reference printer. */
    private static void quoteChecked(Out out, String s) {
        if (!validUnicode(s)) {
            throw new PbwireException("JSON: string is not valid Unicode", 1);
        }
        quoteRaw(out, s);
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

    private static void quoteRaw(Out out, String s) {
        StringBuilder sb = out.sb;
        sb.append('"');
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            switch (c) {
                case '"':
                    sb.append("\\\"");
                    continue;
                case '\\':
                    sb.append("\\\\");
                    continue;
                case '\b':
                    sb.append("\\b");
                    continue;
                case '\f':
                    sb.append("\\f");
                    continue;
                case '\n':
                    sb.append("\\n");
                    continue;
                case '\r':
                    sb.append("\\r");
                    continue;
                case '\t':
                    sb.append("\\t");
                    continue;
                default:
                    break;
            }
            if (c == '<') {
                sb.append("\\u003c");
            } else if (c == '>') {
                sb.append("\\u003e");
            } else if (c < 0x20 || c == 0x7f) {
                sb.append(String.format("\\u%04x", (int) c));
            } else {
                sb.append(c);
            }
        }
        sb.append('"');
    }

    /* ---------------- parse ---------------- */

    private static final int JK_END = 0, JK_OBJ = 1, JK_ARR = 2, JK_STR = 3,
            JK_NUM = 4, JK_TRUE = 5, JK_FALSE = 6, JK_NULL = 7, JK_BAD = 8;

    private static final class Parser {
        final String s;
        int pos;
        final int cap;
        final boolean ignoreUnknown;
        int depth;

        Parser(String s, int cap, boolean ignoreUnknown) {
            this.s = s;
            this.cap = cap;
            this.ignoreUnknown = ignoreUnknown;
        }

        boolean eof() {
            return pos >= s.length();
        }

        void fail(String what) {
            throw new PbwireException("JSON: " + what + " at offset " + pos, 1);
        }

        void spaces() {
            while (pos < s.length()) {
                char c = s.charAt(pos);
                if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                    pos++;
                } else {
                    return;
                }
            }
        }

        GenericCodec.DMsg newMessage(GenericCodec.MessageDef def) {
            return new GenericCodec.DMsg(def);
        }

        /** True when the next non-space character is exactly `c`. */
        boolean at(char c) {
            spaces();
            return pos < s.length() && s.charAt(pos) == c;
        }

        int peekKind() {
            spaces();
            if (pos >= s.length()) {
                return JK_END;
            }
            switch (s.charAt(pos)) {
                case '{':
                    return JK_OBJ;
                case '[':
                    return JK_ARR;
                case '"':
                case '\'':
                    return JK_STR;
                case 't':
                    return JK_TRUE;
                case 'f':
                    return JK_FALSE;
                case 'n':
                    return JK_NULL;
                case '-':
                case '0':
                case '1':
                case '2':
                case '3':
                case '4':
                case '5':
                case '6':
                case '7':
                case '8':
                case '9':
                    return JK_NUM;
                default:
                    return JK_BAD;
            }
        }

        void expectLit(String lit) {
            spaces();
            for (int i = 0; i < lit.length(); i++) {
                if (pos >= s.length() || s.charAt(pos) != lit.charAt(i)) {
                    fail("expected " + lit);
                }
                pos++;
            }
        }

        void expectCh(char c) {
            spaces();
            if (pos >= s.length() || s.charAt(pos) != c) {
                fail("expected '" + c + "'");
            }
            pos++;
        }

        /* ---- strings and keys ---- */

        String parseString() {
            spaces();
            if (pos >= s.length()) {
                fail("expected a string");
            }
            char quote = s.charAt(pos);
            if (quote != '"' && quote != '\'') {
                fail("expected a string");
            }
            pos++;
            StringBuilder sb = new StringBuilder();
            while (true) {
                if (pos >= s.length()) {
                    fail("truncated string");
                }
                char c = s.charAt(pos++);
                if (c == quote) {
                    return validUnicode(sb.toString()) ? sb.toString()
                            : invalidString();
                }
                if (c != '\\') {
                    sb.append(c);
                    continue;
                }
                if (pos >= s.length()) {
                    fail("truncated escape");
                }
                char e = s.charAt(pos++);
                switch (e) {
                    case '"':
                        sb.append('"');
                        break;
                    case '\\':
                        sb.append('\\');
                        break;
                    case '/':
                        sb.append('/');
                        break;
                    case 'b':
                        sb.append('\b');
                        break;
                    case 'f':
                        sb.append('\f');
                        break;
                    case 'n':
                        sb.append('\n');
                        break;
                    case 'r':
                        sb.append('\r');
                        break;
                    case 't':
                        sb.append('\t');
                        break;
                    case '\'':
                        sb.append('\'');
                        break;
                    case 'u':
                    case 'U': { // the capital form reads four hex digits too
                        int hi = hex4();
                        int rune = hi;
                        if (rune >= 0xd800 && rune <= 0xdbff) {
                            int lo = 0;
                            if (pos + 1 >= s.length() || s.charAt(pos) != '\\'
                                    || s.charAt(pos + 1) != 'u') {
                                fail("unpaired surrogate escape");
                            }
                            pos += 2;
                            lo = hex4();
                            if (lo < 0xdc00 || lo > 0xdfff) {
                                fail("unpaired surrogate escape");
                            }
                            rune = ((hi & 0x3ff) << 10) | (lo & 0x3ff);
                            rune += 0x10000;
                        } else if (rune >= 0xdc00 && rune <= 0xdfff) {
                            fail("unpaired surrogate escape");
                        }
                        sb.appendCodePoint(rune);
                        break;
                    }
                    default:
                        fail("bad escape \\" + e);
                        break;
                }
            }
        }

        private String invalidString() {
            throw new PbwireException("JSON: string is not valid Unicode", 1);
        }

        private int hex4() {
            int v = 0;
            for (int i = 0; i < 4; i++) {
                if (pos >= s.length()) {
                    fail("truncated \\u escape");
                }
                char h = s.charAt(pos++);
                v <<= 4;
                if (h >= '0' && h <= '9') {
                    v |= h - '0';
                } else if (h >= 'a' && h <= 'f') {
                    v |= h - 'a' + 10;
                } else if (h >= 'A' && h <= 'F') {
                    v |= h - 'A' + 10;
                } else {
                    fail("bad \\u escape");
                }
            }
            return v;
        }

        /** A key: quoted, or bare (legacy mode; never a JSON literal word). */
        String parseKey() {
            if (peekKind() == JK_STR) {
                return parseString();
            }
            spaces();
            int start = pos;
            while (pos < s.length()) {
                char c = s.charAt(pos);
                if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z')
                        || (c >= 'A' && c <= 'Z') || c == '_') {
                    pos++;
                } else {
                    break;
                }
            }
            int n = pos - start;
            if (n == 0 || (s.charAt(start) >= '0' && s.charAt(start) <= '9')) {
                fail("expected a field name");
            }
            String key = s.substring(start, pos);
            if ((n == 4 && ("true".equals(key) || "null".equals(key)))
                    || (n == 5 && "false".equals(key))) {
                fail("expected a field name");
            }
            return key;
        }

        /* ---- numbers ---- */

        /**
         * Scans a number token without consuming what follows it: strict
         * decimal syntax, no leading zeros, no trailing '.', and the token
         * must end where a value may end.
         */
        String parseRaw() {
            spaces();
            int start = pos;
            int end = s.length();
            int state = 0; // 0 int, 1 fraction, 2 exponent
            char prev = 0;
            int p = pos;
            for (; p < end; p++) {
                char c = s.charAt(p);
                boolean digit = c >= '0' && c <= '9';
                boolean lastWasInt = prev >= '0' && prev <= '9';
                if (digit) {
                    // keep going
                } else if (state == 0 && c == '-') {
                    if (lastWasInt) {
                        break;
                    }
                } else if (state == 0 && lastWasInt && c == '.') {
                    state = 1;
                } else if (state != 2 && lastWasInt && (c == 'e' || c == 'E')) {
                    state = 2;
                } else if ((prev == 'e' || prev == 'E') && (c == '-' || c == '+')) {
                    // keep going
                } else {
                    break;
                }
                prev = c;
            }
            String token = s.substring(start, p);
            if (token.isEmpty() || token.equals("-")) {
                fail("expected a number");
            }
            int i = token.charAt(0) == '-' ? 1 : 0;
            if (token.length() - i > 1 && token.charAt(i) == '0'
                    && token.charAt(i + 1) >= '0' && token.charAt(i + 1) <= '9') {
                fail("leading zero in \"" + token + "\"");
            }
            if (token.endsWith(".")) {
                fail("trailing '.' in \"" + token + "\"");
            }
            pos = p;
            spaces();
            if (pos < s.length()) {
                char c = s.charAt(pos);
                if (c != ',' && c != ']' && c != '}') {
                    fail("number must end at a value boundary");
                }
            }
            return token;
        }

        /** Parses one scalar value of `f` and returns the boxed tree value. */
        Object parseScalar(GenericCodec.Field f) {
            switch (f.kind) {
                case GenericCodec.K_INT32:
                case GenericCodec.K_SINT32:
                case GenericCodec.K_SFIXED32:
                case GenericCodec.K_INT64:
                case GenericCodec.K_SINT64:
                case GenericCodec.K_SFIXED64:
                    return Long.valueOf(parseInt(false,
                            f.kind == GenericCodec.K_INT32
                            || f.kind == GenericCodec.K_SINT32
                            || f.kind == GenericCodec.K_SFIXED32));
                case GenericCodec.K_UINT32:
                case GenericCodec.K_FIXED32:
                    return Long.valueOf(parseInt(true,
                            f.kind == GenericCodec.K_UINT32
                            || f.kind == GenericCodec.K_FIXED32));
                case GenericCodec.K_UINT64:
                case GenericCodec.K_FIXED64:
                    return Long.valueOf(parseInt(true, false));
                case GenericCodec.K_BOOL:
                    return Boolean.valueOf(parseBool());
                case GenericCodec.K_FLOAT:
                    return Float.valueOf((float) parseDouble(true));
                case GenericCodec.K_DOUBLE:
                    return Double.valueOf(parseDouble(false));
                case GenericCodec.K_STRING:
                    return parseString();
                case GenericCodec.K_BYTES:
                    return parseBase64();
                default:
                    fail("expected an object");
                    return null;
            }
        }

        /**
         * Integer rules shared by tokens and quoted strings: the token is
         * strict, a quoted string rejects surrounding whitespace, and an
         * integral float spelling (1e3, 100000.000) counts.
         */
        long parseInt(boolean unsigned, boolean bits32) {
            int kind = peekKind();
            String text;
            if (kind == JK_NUM) {
                text = parseRaw();
            } else if (kind == JK_STR) {
                text = parseString();
                if (hasSpace(text)) {
                    fail("whitespace in \"" + text + "\"");
                }
            } else {
                fail("expected a number");
                return 0;
            }
            return parseIntText(text, unsigned, bits32);
        }

        long parseIntText(String text, boolean unsigned, boolean bits32) {
            if (unsigned) {
                Long u = atou64(text);
                if (u == null) {
                    Double d = atod(text);
                    if (d == null || Double.isNaN(d) || Double.isInfinite(d)
                            || d < 0 || d > 18014398509481984.0) {
                        fail("bad unsigned integer \"" + text + "\"");
                    }
                    long v = (long) (double) d;
                    if ((double) v != (double) d) {
                        fail("bad unsigned integer \"" + text + "\"");
                    }
                    u = Long.valueOf(v);
                }
                if (bits32 && Long.compareUnsigned(u, 0xFFFFFFFFL) > 0) {
                    fail("uint32 out of range \"" + text + "\"");
                }
                return u;
            }
            Long v = atoi64(text);
            if (v == null) {
                Double d = atod(text);
                if (d == null || Double.isNaN(d) || Double.isInfinite(d)
                        || d < -9007199254740992.0 || d > 9007199254740992.0) {
                    fail("bad integer \"" + text + "\"");
                }
                long sv = (long) (double) d;
                if ((double) sv != (double) d) {
                    fail("bad integer \"" + text + "\"");
                }
                v = Long.valueOf(sv);
            }
            if (bits32 && (v < Integer.MIN_VALUE || v > Integer.MAX_VALUE)) {
                fail("int32 out of range \"" + text + "\"");
            }
            return v;
        }

        boolean parseBool() {
            int kind = peekKind();
            if (kind == JK_TRUE) {
                expectLit("true");
                return true;
            }
            if (kind == JK_FALSE) {
                expectLit("false");
                return false;
            }
            if (kind == JK_STR) {
                String t = parseString();
                String lower = t.toLowerCase();
                if (lower.equals("true") || lower.equals("1")
                        || lower.equals("t") || lower.equals("y")
                        || lower.equals("yes")) {
                    return true;
                }
                if (lower.equals("false") || lower.equals("0")
                        || lower.equals("f") || lower.equals("n")
                        || lower.equals("no")) {
                    return false;
                }
                fail("bad bool \"" + t + "\"");
            }
            fail("expected a bool");
            return false;
        }

        double parseDouble(boolean isFloat) {
            int kind = peekKind();
            String text;
            boolean quoted;
            if (kind == JK_NUM) {
                text = parseRaw();
                quoted = false;
            } else if (kind == JK_STR) {
                text = parseString();
                quoted = true;
            } else {
                fail("expected a number");
                return 0;
            }
            double v = atodSpecial(text);
            if (!quoted && (Double.isNaN(v) || Double.isInfinite(v)) ) {
                fail("bad double \"" + text + "\"");
            }
            // A raw number may not overflow a float field either.
            if (isFloat && Double.isFinite(v) && Float.isInfinite((float) v)) {
                fail("float out of range \"" + text + "\"");
            }
            return v;
        }

        /** strtod semantics: specials either spelling, then a full-consume parse. */
        private double atodSpecial(String text) {
            String u = text.trim();
            String lower = u.toLowerCase();
            String body = lower;
            boolean neg = false;
            if (body.startsWith("-")) {
                neg = true;
                body = body.substring(1);
            } else if (body.startsWith("+")) {
                body = body.substring(1);
            }
            if (body.equals("nan")) {
                return Double.NaN;
            }
            if (body.equals("inf") || body.equals("infinity")) {
                return neg ? Double.NEGATIVE_INFINITY : Double.POSITIVE_INFINITY;
            }
            Double d = atod(text);
            if (d == null) {
                fail("bad number \"" + text + "\"");
            }
            return d;
        }

        byte[] parseBase64() {
            int kind = peekKind();
            if (kind != JK_STR) {
                fail("expected a base64 string");
            }
            String t = parseString();
            return base64(t);
        }

        /* ---- structure ---- */

        /** Reads the next value as a message of `def`. */
        GenericCodec.DMsg parseMessage(GenericCodec.MessageDef def) {
            GenericCodec.DMsg m = newMessage(def);
            intoMessage(m);
            return m;
        }

        /** Fills `m` from the next value, merging like the binary decoder. */
        void intoMessage(GenericCodec.DMsg m) {
            int wkt = classify(m.def);
            switch (wkt) {
                case W_ANY:
                    parseAny(m);
                    return;
                case W_VALUE:
                    parseValueMsg(m);
                    return;
                case W_STRUCT:
                    parseStruct(m);
                    return;
                default:
                    break;
            }
            int kind = peekKind();
            if (kind != JK_OBJ) {
                switch (wkt) {
                    case W_LIST:
                        parseListValue(m);
                        return;
                    case W_WRAPPER: {
                        int slot = slotOf(m.def, 1);
                        parseFieldValue(m, m.def.fields[slot], slot, false);
                        return;
                    }
                    case W_TIMESTAMP:
                        parseTimestamp(m);
                        return;
                    case W_DURATION:
                        parseDuration(m);
                        return;
                    case W_FIELDMASK:
                        parseFieldMask(m);
                        return;
                    default:
                        fail("expected an object");
                        return;
                }
            }
            expectCh('{');
            objectBody(m);
        }

        void objectBody(GenericCodec.DMsg m) {
            if (depth >= cap) {
                throw new PbwireException("JSON: nesting exceeds " + cap, 5);
            }
            depth++;
            spaces();
            if (at('}')) {
                pos++;
                depth--;
                return;
            }
            while (true) {
                spaces();
                String key = parseKey();
                spaces();
                expectCh(':');
                spaces();
                namedField(m, key);
                spaces();
                char c = peek();
                if (c == ',') {
                    pos++;
                    spaces();
                    if (at('}')) {
                        pos++; // trailing commas are accepted in legacy mode
                        depth--;
                        return;
                    }
                    continue;
                }
                if (c == '}') {
                    pos++;
                    depth--;
                    return;
                }
                fail("expected ',' or '}'");
            }
        }

        char peek() {
            if (pos >= s.length()) {
                fail("truncated input");
            }
            return s.charAt(pos);
        }

        /** Resolves a key to a field and parses its value. */
        void namedField(GenericCodec.DMsg m, String key) {
            if (key.length() >= 2 && key.charAt(0) == '['
                    && key.charAt(key.length() - 1) == ']') {
                String name = key.substring(1, key.length() - 1);
                GenericCodec.Schema schema = m.def.schema;
                GenericCodec.Field ext = schema == null ? null
                        : schema.extension(name);
                if (ext == null || m.def.lookup(ext.number) < 0) {
                    if (ignoreUnknown) {
                        skipValue();
                        return;
                    }
                    fail("unknown extension \"" + name + "\"");
                }
                int slot = m.def.lookup(ext.number);
                parseFieldValue(m, m.def.fields[slot], slot, false);
                return;
            }
            int slot = fieldSlot(m.def, key);
            if (slot < 0) {
                if (ignoreUnknown) {
                    skipValue();
                    return;
                }
                throw new PbwireException("JSON: unknown field \"" + key
                        + "\" in " + m.def.fullName, 3);
            }
            parseFieldValue(m, m.def.fields[slot], slot, false);
        }

        /** camelCase first, then the proto name, then the json_name. */
        int fieldSlot(GenericCodec.MessageDef def, String key) {
            for (int s = 0; s < def.fields.length; s++) {
                GenericCodec.Field f = def.fields[s];
                if (f.extension) {
                    continue;
                }
                if (camelName(f.fieldName).equals(key)) {
                    return s;
                }
            }
            Integer byName = def.byName.get(key);
            if (byName != null && !def.fields[byName].extension) {
                return byName;
            }
            for (int s = 0; s < def.fields.length; s++) {
                GenericCodec.Field f = def.fields[s];
                if (!f.extension && jsonName(f.fieldName).equals(key)) {
                    return s;
                }
            }
            return -1;
        }

        void skipValue() {
            switch (peekKind()) {
                case JK_OBJ: {
                    expectCh('{');
                    if (depth >= cap) {
                        throw new PbwireException("JSON: nesting exceeds " + cap, 5);
                    }
                    depth++;
                    spaces();
                    if (at('}')) {
                        pos++;
                        depth--;
                        return;
                    }
                    while (true) {
                        parseKey();
                        expectCh(':');
                        skipValue();
                        spaces();
                        char c = peek();
                        if (c == ',') {
                            pos++;
                            continue;
                        }
                        if (c == '}') {
                            pos++;
                            depth--;
                            return;
                        }
                        fail("expected ',' or '}'");
                    }
                }
                case JK_ARR: {
                    expectCh('[');
                    if (depth >= cap) {
                        throw new PbwireException("JSON: nesting exceeds " + cap, 5);
                    }
                    depth++;
                    spaces();
                    if (at(']')) {
                        pos++;
                        depth--;
                        return;
                    }
                    while (true) {
                        skipValue();
                        spaces();
                        char c = peek();
                        if (c == ',') {
                            pos++;
                            continue;
                        }
                        if (c == ']') {
                            pos++;
                            depth--;
                            return;
                        }
                        fail("expected ',' or ']'");
                    }
                }
                case JK_STR:
                    parseString();
                    return;
                case JK_NUM:
                    parseRaw();
                    return;
                case JK_TRUE:
                    expectLit("true");
                    return;
                case JK_FALSE:
                    expectLit("false");
                    return;
                case JK_NULL:
                    expectLit("null");
                    return;
                default:
                    fail("expected a value");
            }
        }

        /* ---- field values ---- */

        /**
         * Parses the value of one field. `inArray` marks an element already
         * inside an array; a repeated field outside one requires an array.
         */
        void parseFieldValue(GenericCodec.DMsg m, GenericCodec.Field f,
                int slot, boolean inArray) {
            boolean isValue = f.kind == GenericCodec.K_MESSAGE && f.msgType != null
                    && classify(f.msgType) == W_VALUE;
            boolean isNullValue = isNullValue(f);
            if (peekKind() == JK_NULL && !isValue && !isNullValue) {
                expectLit("null"); // any other null is a no-op
                return;
            }
            if (!f.repeated) {
                if (f.oneof >= 0) {
                    if (m.oneofCase[f.oneof] != 0) {
                        fail("duplicate key for oneof member " + f.fieldName);
                    }
                }
                // A duplicate singular key takes the last value; a duplicate
                // message field merges, like the reference.
            }
            if (f.kind == GenericCodec.K_MESSAGE
                    || f.kind == GenericCodec.K_GROUP) {
                if (f.msgType.mapEntry) {
                    parseMap(m, f, slot);
                    return;
                }
                if (f.repeated && !inArray) {
                    // The reference parser wants an array for a repeated field.
                    if (peekKind() != JK_ARR) {
                        fail("repeated field " + f.fieldName + " wants an array");
                    }
                    parseArray(m, f, slot);
                    return;
                }
                if (!f.repeated && f.oneof < 0 && has(m, slot)) {
                    Object prev = value(m, slot);
                    if (prev instanceof GenericCodec.DMsg
                            && ((GenericCodec.DMsg) prev).def == f.msgType) {
                        intoMessage((GenericCodec.DMsg) prev);
                        return;
                    }
                }
                GenericCodec.DMsg sub = parseMessage(f.msgType);
                if (f.repeated) {
                    m.add(f.number, sub);
                } else {
                    m.set(f.number, sub);
                }
                return;
            }
            if (f.repeated) {
                if (!inArray) {
                    // The reference parser wants an array for a repeated field.
                    if (peekKind() != JK_ARR) {
                        fail("repeated field " + f.fieldName + " wants an array");
                    }
                    parseArray(m, f, slot);
                    return;
                }
                if (f.kind == GenericCodec.K_ENUM) {
                    parseEnumValue(m, f, slot);
                    return;
                }
                m.add(f.number, parseScalar(f));
                return;
            }
            if (f.kind == GenericCodec.K_ENUM) {
                parseEnumValue(m, f, slot);
                return;
            }
            m.set(f.number, parseScalar(f));
        }

        /** Parses the array of a repeated field; nested arrays flatten. */
        void parseArray(GenericCodec.DMsg m, GenericCodec.Field f, int slot) {
            expectCh('[');
            if (depth >= cap) {
                throw new PbwireException("JSON: nesting exceeds " + cap, 5);
            }
            depth++;
            spaces();
            if (at(']')) {
                pos++;
                depth--;
                return;
            }
            while (true) {
                parseElement(m, f, slot);
                spaces();
                char c = peek();
                if (c == ',') {
                    pos++;
                    spaces();
                    if (at(']')) {
                        pos++;
                        depth--;
                        return;
                    }
                    continue;
                }
                if (c == ']') {
                    pos++;
                    depth--;
                    return;
                }
                fail("expected ',' or ']'");
            }
        }

        void parseElement(GenericCodec.DMsg m, GenericCodec.Field f, int slot) {
            boolean isValue = f.kind == GenericCodec.K_MESSAGE && f.msgType != null
                    && classify(f.msgType) == W_VALUE;
            if (peekKind() == JK_NULL && !isValue && !isNullValue(f)) {
                expectLit("null");
                emitNull(m, f, slot);
                return;
            }
            // Nested arrays flatten, except inside Value and ListValue.
            int wkt = f.msgType == null ? W_NONE : classify(f.msgType);
            if (peekKind() == JK_ARR && wkt != W_VALUE && wkt != W_LIST) {
                parseNestedArray(m, f, slot);
                return;
            }
            parseFieldValue(m, f, slot, true);
        }

        void parseNestedArray(GenericCodec.DMsg m, GenericCodec.Field f, int slot) {
            expectCh('[');
            if (depth >= cap) {
                throw new PbwireException("JSON: nesting exceeds " + cap, 5);
            }
            depth++;
            spaces();
            if (at(']')) {
                pos++;
                depth--;
                return;
            }
            while (true) {
                parseElement(m, f, slot);
                spaces();
                char c = peek();
                if (c == ',') {
                    pos++;
                    spaces();
                    if (at(']')) {
                        pos++;
                        depth--;
                        return;
                    }
                    continue;
                }
                if (c == ']') {
                    pos++;
                    depth--;
                    return;
                }
                fail("expected ',' or ']'");
            }
        }

        /** The reference emits a default element for a null array element. */
        void emitNull(GenericCodec.DMsg m, GenericCodec.Field f, int slot) {
            if (f.kind <= GenericCodec.K_DOUBLE) {
                m.add(f.number, f.kind == GenericCodec.K_BOOL
                        ? Boolean.FALSE : Long.valueOf(0));
                return;
            }
            switch (f.kind) {
                case GenericCodec.K_STRING:
                    m.add(f.number, "");
                    return;
                case GenericCodec.K_BYTES:
                    m.add(f.number, new byte[0]);
                    return;
                default:
                    m.add(f.number, newMessage(f.msgType));
                    return;
            }
        }

        /** Parses an enum value; unknown names skip under ignore_unknown. */
        void parseEnumValue(GenericCodec.DMsg m, GenericCodec.Field f, int slot) {
            int kind = peekKind();
            int number;
            boolean skipped = false;
            if (kind == JK_STR) {
                String text = parseString();
                Integer known = f.enumType == null ? null
                        : f.enumType.valueOf(text);
                if (known != null) {
                    number = known;
                } else {
                    // A quoted number spells the value; anything else is
                    // an unknown name, which ignore_unknown drops.
                    String t = text.trim();
                    Long v = atoi64(t);
                    if (v == null || v < Integer.MIN_VALUE || v > Integer.MAX_VALUE) {
                        if (ignoreUnknown) {
                            skipped = true;
                            number = 0;
                        } else {
                            throw new PbwireException("JSON: unknown enum name \""
                                    + text + "\" in " + m.def.fullName, 3);
                        }
                    } else {
                        number = (int) (long) v;
                    }
                }
            } else if (kind == JK_NUM) {
                String text = parseRaw();
                Long v = atoi64(text);
                if (v == null) {
                    v = (long) (double) parseDouble(false);
                }
                number = v.intValue();
            } else if (kind == JK_NULL && isNullValue(f)) {
                expectLit("null"); // the canonical spelling of NULL_VALUE
                number = 0;
            } else {
                fail("expected an enum value");
                return;
            }
            if (skipped) {
                return;
            }
            if (f.enumType != null && f.enumType.nameOf(number) != null) {
                storeEnum(m, f, number);
                return;
            }
            // Unknown number: open enums keep it, closed ones divert it.
            if (f.enumType == null || !f.enumClosed) {
                storeEnum(m, f, number);
                return;
            }
            storeUnknownEnum(m, f, number);
        }

        void storeEnum(GenericCodec.DMsg m, GenericCodec.Field f, int number) {
            if (f.repeated) {
                m.add(f.number, Integer.valueOf(number));
            } else {
                m.set(f.number, Integer.valueOf(number));
            }
        }

        /** A closed enum's unknown number becomes an unknown wire field. */
        void storeUnknownEnum(GenericCodec.DMsg m, GenericCodec.Field f,
                int number) {
            Wire.Buf b = new Wire.Buf();
            Wire.wTagVar32(b, Wire.tagBytes(f.number, Wire.WT_VARINT),
                    (int) number);
            if (m.unknown == null) {
                m.unknown = new ArrayList<>();
            }
            m.unknown.add(java.util.Arrays.copyOf(b.b, b.pos));
        }

        /** Parses a map field's JSON object; duplicate keys fail. */
        void parseMap(GenericCodec.DMsg m, GenericCodec.Field f, int slot) {
            GenericCodec.MessageDef entryDef = f.msgType;
            int keySlot = entryDef.lookup(1);
            int valueSlot = entryDef.lookup(2);
            if (keySlot < 0 || valueSlot < 0) {
                fail("invalid map entry " + entryDef.fullName);
            }
            GenericCodec.Field keyField = entryDef.fields[keySlot];
            GenericCodec.Field valueField = entryDef.fields[valueSlot];
            expectCh('{');
            if (depth >= cap) {
                throw new PbwireException("JSON: nesting exceeds " + cap, 5);
            }
            depth++;
            spaces();
            if (at('}')) {
                pos++;
                depth--;
                return;
            }
            Set<Object> seen = new HashSet<>();
            while (true) {
                String key = parseKey();
                expectCh(':');
                spaces();
                GenericCodec.DMsg entry = newMessage(entryDef);
                Object stored = mapKey(keyField, key);
                if (stored == null) {
                    depth--;
                    throw new PbwireException("JSON: bad map key \"" + key
                            + "\" in " + entryDef.fullName, 1);
                }
                entry.set(keyField.number, stored);
                // A null map value is invalid JSON for the mapping (the
                // reference rejects it), except for a google.protobuf.Value
                // value, where null means NullValue.
                boolean valueFieldIsValue = valueField.kind == GenericCodec.K_MESSAGE
                        && valueField.msgType != null
                        && classify(valueField.msgType) == W_VALUE;
                if (peekKind() == JK_NULL && !valueFieldIsValue) {
                    depth--;
                    fail("null is not a valid map value");
                }
                parseFieldValue(entry, valueField, valueSlot, false);
                // An unknown enum name under ignore_unknown stores nothing,
                // and its entry is dropped.
                boolean dropped = valueField.kind == GenericCodec.K_ENUM
                        && !entry.has[valueSlot];
                if (!dropped) {
                    if (!seen.add(stored)) {
                        depth--;
                        throw new PbwireException("JSON: duplicate map key \""
                                + key + "\" in " + m.def.fullName, 1);
                    }
                    m.add(f.number, entry);
                }
                spaces();
                char c = peek();
                if (c == ',') {
                    pos++;
                    spaces();
                    if (at('}')) {
                        pos++;
                        depth--;
                        return;
                    }
                    continue;
                }
                if (c == '}') {
                    pos++;
                    depth--;
                    return;
                }
                fail("expected ',' or '}'");
            }
        }

        /** Map keys: strict integers, exact bools, raw strings. */
        Object mapKey(GenericCodec.Field f, String text) {
            switch (f.kind) {
                case GenericCodec.K_INT32:
                case GenericCodec.K_SINT32:
                case GenericCodec.K_SFIXED32: {
                    Long v = atoi64(text.trim());
                    if (v == null || v < Integer.MIN_VALUE || v > Integer.MAX_VALUE) {
                        return null;
                    }
                    return Long.valueOf(v);
                }
                case GenericCodec.K_INT64:
                case GenericCodec.K_SINT64:
                case GenericCodec.K_SFIXED64: {
                    Long v = atoi64(text.trim());
                    return v == null ? null : Long.valueOf(v);
                }
                case GenericCodec.K_UINT32:
                case GenericCodec.K_FIXED32: {
                    Long v = atou64(text.trim());
                    if (v == null || Long.compareUnsigned(v, 0xFFFFFFFFL) > 0) {
                        return null;
                    }
                    return Long.valueOf(v);
                }
                case GenericCodec.K_UINT64:
                case GenericCodec.K_FIXED64: {
                    Long v = atou64(text.trim());
                    return v == null ? null : Long.valueOf(v);
                }
                case GenericCodec.K_BOOL:
                    if ("true".equals(text)) {
                        return Boolean.TRUE;
                    }
                    if ("false".equals(text)) {
                        return Boolean.FALSE;
                    }
                    return null;
                case GenericCodec.K_STRING:
                    return text;
                case GenericCodec.K_ENUM: {
                    Integer v = f.enumType == null ? null : f.enumType.valueOf(text);
                    return v == null ? null : Long.valueOf(v);
                }
                default:
                    return null;
            }
        }

        /* ---- well-known types ---- */

        /**
         * google.protobuf.Any: buffer the object, find @type, then parse the
         * payload's fields against the resolved type.
         */
        void parseAny(GenericCodec.DMsg m) {
            int urlSlot = m.def.lookup(1);
            int valueSlot = m.def.lookup(2);
            if (urlSlot < 0 || valueSlot < 0
                    || m.def.fields[urlSlot].kind != GenericCodec.K_STRING
                    || m.def.fields[urlSlot].repeated
                    || m.def.fields[valueSlot].kind != GenericCodec.K_BYTES
                    || m.def.fields[valueSlot].repeated) {
                fail("invalid google.protobuf.Any shape");
            }
            if (peekKind() == JK_NULL) {
                expectLit("null");
                return;
            }
            expectCh('{');
            int start = pos;
            if (depth >= cap) {
                throw new PbwireException("JSON: nesting exceeds " + cap, 5);
            }
            depth++;
            spaces();
            if (at('}')) {
                pos++; // an empty object is an empty Any in legacy mode
                depth--;
                return;
            }
            while (true) {
                String key = parseKey();
                expectCh(':');
                if ("@type".equals(key)) {
                    if (m.has[urlSlot]) {
                        fail("second @type");
                    }
                    spaces();
                    m.set(m.def.fields[urlSlot].number, parseString());
                } else {
                    skipValue();
                }
                spaces();
                char c = peek();
                if (c == ',') {
                    pos++;
                    spaces();
                    if (at('}')) {
                        pos++;
                        break;
                    }
                    continue;
                }
                if (c == '}') {
                    pos++;
                    break;
                }
                fail("expected ',' or '}'");
            }
            int end = pos;
            depth--;
            String url = stringValue(m, urlSlot);
            if (url.isEmpty()) {
                fail("Any object without @type");
            }
            String name = url.substring(url.lastIndexOf('/') + 1);
            GenericCodec.Schema schema = m.def.schema;
            GenericCodec.MessageDef target = schema == null ? null
                    : schema.byName.get(name);
            if (target == null || schema.enums.containsKey(name)) {
                fail("Any type is not resolvable: " + url);
            }
            // Re-walk the buffered object against the payload type.
            int after = pos;
            pos = start;
            depth++;
            GenericCodec.DMsg sub = newMessage(target);
            int wkt = classify(target);
            spaces();
            while (pos < end) {
                if (s.charAt(pos) == '}') {
                    pos++;
                    break;
                }
                String key = parseKey();
                expectCh(':');
                spaces();
                if ("@type".equals(key)) {
                    skipValue();
                } else if (wkt != W_NONE) {
                    if (!"value".equals(key)) {
                        fail("well-known Any payload needs a \"value\" key");
                    }
                    intoMessage(sub);
                } else {
                    namedField(sub, key);
                }
                spaces();
                if (pos < end && s.charAt(pos) == ',') {
                    pos++;
                    spaces();
                    continue;
                }
                if (pos < end && s.charAt(pos) == '}') {
                    pos++;
                    break;
                }
                fail("expected ',' or '}'");
            }
            depth--;
            pos = after;
            byte[] payload = GenericCodec.encode(sub);
            if (payload.length > 0) {
                m.set(m.def.fields[valueSlot].number, payload);
            }
        }

        /** google.protobuf.Value from the upcoming JSON kind. */
        void parseValueMsg(GenericCodec.DMsg m) {
            int[] slots = valueSlots(m.def);
            int kind = peekKind();
            switch (kind) {
                case JK_NULL:
                    expectLit("null");
                    m.set(m.def.fields[slots[0]].number, Integer.valueOf(0));
                    return;
                case JK_NUM: {
                    String text = parseRaw();
                    Double d = atod(text);
                    if (d == null) {
                        fail("bad number \"" + text + "\"");
                    }
                    m.set(m.def.fields[slots[1]].number, d);
                    return;
                }
                case JK_STR:
                    m.set(m.def.fields[slots[2]].number, parseString());
                    return;
                case JK_TRUE:
                    expectLit("true");
                    m.set(m.def.fields[slots[3]].number, Boolean.TRUE);
                    return;
                case JK_FALSE:
                    expectLit("false");
                    m.set(m.def.fields[slots[3]].number, Boolean.FALSE);
                    return;
                case JK_OBJ: {
                    GenericCodec.Field f5 = m.def.fields[slots[4]];
                    m.set(f5.number, parseMessage(f5.msgType));
                    return;
                }
                case JK_ARR: {
                    GenericCodec.Field f6 = m.def.fields[slots[5]];
                    m.set(f6.number, parseMessage(f6.msgType));
                    return;
                }
                default:
                    fail("bad google.protobuf.Value");
            }
        }

        /** google.protobuf.Struct: a map<string, Value> in disguise. */
        void parseStruct(GenericCodec.DMsg m) {
            int fields = m.def.lookup(1);
            GenericCodec.Field entryField = m.def.fields[fields];
            if (entryField.msgType == null) {
                fail("invalid google.protobuf.Struct shape");
            }
            int keySlot = entryField.msgType.lookup(1);
            int valueSlot = entryField.msgType.lookup(2);
            if (keySlot < 0 || valueSlot < 0) {
                fail("invalid google.protobuf.Struct shape");
            }
            if (peekKind() == JK_NULL) {
                expectLit("null");
                return;
            }
            expectCh('{');
            if (depth >= cap) {
                throw new PbwireException("JSON: nesting exceeds " + cap, 5);
            }
            depth++;
            spaces();
            if (at('}')) {
                pos++;
                depth--;
                return;
            }
            Set<Object> seen = new HashSet<>();
            while (true) {
                String key = parseKey();
                expectCh(':');
                spaces();
                if (!seen.add(key)) {
                    throw new PbwireException("JSON: duplicate Struct key \""
                            + key + "\"", 1);
                }
                GenericCodec.DMsg entry = newMessage(entryField.msgType);
                entry.set(entryField.msgType.fields[keySlot].number, key);
                GenericCodec.Field valueField = entryField.msgType.fields[valueSlot];
                entry.set(valueField.number, parseMessage(valueField.msgType));
                m.add(entryField.number, entry);
                spaces();
                char c = peek();
                if (c == ',') {
                    pos++;
                    spaces();
                    if (at('}')) {
                        pos++;
                        depth--;
                        return;
                    }
                    continue;
                }
                if (c == '}') {
                    pos++;
                    depth--;
                    return;
                }
                fail("expected ',' or '}'");
            }
        }

        /** google.protobuf.ListValue: an array of Values, never flattened. */
        void parseListValue(GenericCodec.DMsg m) {
            int entrySlot = m.def.lookup(1);
            if (entrySlot < 0 || m.def.fields[entrySlot].msgType == null) {
                fail("invalid google.protobuf.ListValue shape");
            }
            if (peekKind() == JK_NULL) {
                expectLit("null");
                return;
            }
            GenericCodec.Field f = m.def.fields[entrySlot];
            expectCh('[');
            if (depth >= cap) {
                throw new PbwireException("JSON: nesting exceeds " + cap, 5);
            }
            depth++;
            spaces();
            if (at(']')) {
                pos++;
                depth--;
                return;
            }
            while (true) {
                GenericCodec.DMsg item = parseMessage(f.msgType);
                m.add(f.number, item);
                spaces();
                char c = peek();
                if (c == ',') {
                    pos++;
                    spaces();
                    if (at(']')) {
                        pos++;
                        depth--;
                        return;
                    }
                    continue;
                }
                if (c == ']') {
                    pos++;
                    depth--;
                    return;
                }
                fail("expected ',' or ']'");
            }
        }

        /** Timestamp/Duration are stored by number; a wrong shape must fail. */
        private int timeSlot(GenericCodec.MessageDef def, int number, int kind) {
            int s = def.lookup(number);
            if (s < 0) {
                return -1;
            }
            GenericCodec.Field f = def.fields[s];
            if (f.repeated || f.kind != kind) {
                return -1;
            }
            return s;
        }

        /** An RFC 3339 timestamp, no library: fixed offsets like the reference. */
        void parseTimestamp(GenericCodec.DMsg m) {
            int secSecs = timeSlot(m.def, 1, GenericCodec.K_INT64);
            int secNanos = timeSlot(m.def, 2, GenericCodec.K_INT32);
            if (secSecs < 0 || secNanos < 0) {
                fail("invalid google.protobuf.Timestamp shape");
            }
            if (peekKind() == JK_NULL) {
                expectLit("null");
                return;
            }
            String text = parseString();
            if (text.length() < 20) {
                fail("bad Timestamp \"" + text + "\"");
            }
            int[] parts = new int[6];
            int i = 0;
            for (int part = 0; part < 6; part++) {
                int max = part == 0 ? 4 : 2;
                int value = 0;
                int digits = 0;
                while (i < text.length() && digits <= max) {
                    char c = text.charAt(i);
                    if (c < '0' || c > '9') {
                        break;
                    }
                    value = value * 10 + (c - '0');
                    digits++;
                    i++;
                }
                if (digits > max) {
                    fail("bad Timestamp \"" + text + "\"");
                }
                if (part < 5) {
                    char want = part < 2 ? '-' : part == 2 ? 'T' : ':';
                    if (i >= text.length() || text.charAt(i) != want) {
                        fail("bad Timestamp \"" + text + "\"");
                    }
                    i++;
                }
                parts[part] = value;
            }
            int year = parts[0], mon = parts[1], day = parts[2];
            int hour = parts[3], min = parts[4], sec = parts[5];
            if (year == 0 || mon == 0 || day == 0 || mon > 12 || hour > 23
                    || min > 59 || sec > 59) {
                fail("bad Timestamp \"" + text + "\"");
            }
            int[] days = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
            int maxDay = days[mon];
            if (mon == 2 && year % 4 == 0
                    && (year % 100 != 0 || year % 400 == 0)) {
                maxDay = 29;
            }
            if (day > maxDay) {
                fail("bad Timestamp \"" + text + "\"");
            }
            int frac = 0;
            if (i < text.length() && text.charAt(i) == '.') {
                i++;
                int fracDigits = 0;
                while (i < text.length() && text.charAt(i) >= '0'
                        && text.charAt(i) <= '9') {
                    if (fracDigits >= 9) {
                        fail("bad Timestamp \"" + text + "\"");
                    }
                    frac = frac * 10 + (text.charAt(i) - '0');
                    fracDigits++;
                    i++;
                }
                if (fracDigits == 0) {
                    fail("bad Timestamp \"" + text + "\"");
                }
                for (int k = fracDigits; k < 9; k++) {
                    frac *= 10;
                }
            }
            if (i >= text.length()) {
                fail("bad Timestamp \"" + text + "\"");
            }
            int mAdj = mon >= 3 ? mon - 3 : mon + 9;
            int carry = mon >= 3 ? 0 : 1;
            int yAdj = year + 4800 - carry;
            int monthDays = (mAdj * 62719 + 769) / 2048;
            int leapDays = yAdj / 4 - yAdj / 100 + yAdj / 400;
            int epochDays = yAdj * 365 + leapDays + monthDays + (day - 1)
                    - 2472632;
            long secs = (long) epochDays * 86400 + (long) hour * 3600
                    + (long) min * 60 + sec;
            char c = text.charAt(i);
            if (c == 'Z') {
                if (text.length() - i != 1) {
                    fail("bad Timestamp \"" + text + "\"");
                }
            } else if (c == '+' || c == '-') {
                boolean neg = c == '-';
                if (text.length() - i != 6) {
                    fail("bad Timestamp \"" + text + "\"");
                }
                i++;
                int offHour = 0, offMin = 0;
                int limit = 0;
                int start = i;
                while (i < text.length() && limit <= 2) {
                    char d = text.charAt(i);
                    if (d < '0' || d > '9') {
                        break;
                    }
                    offHour = offHour * 10 + (d - '0');
                    i++;
                    limit++;
                }
                if (i == start || i >= text.length() || text.charAt(i) != ':') {
                    fail("bad Timestamp \"" + text + "\"");
                }
                if (limit > 2) {
                    fail("bad Timestamp \"" + text + "\"");
                }
                i++;
                limit = 0;
                while (i < text.length() && limit <= 2) {
                    char d = text.charAt(i);
                    if (d < '0' || d > '9') {
                        break;
                    }
                    offMin = offMin * 10 + (d - '0');
                    i++;
                    limit++;
                }
                if (limit > 2 || i != text.length()) {
                    fail("bad Timestamp \"" + text + "\"");
                }
                long offset = ((long) offHour * 60 + offMin) * 60;
                secs += neg ? offset : -offset;
            } else {
                fail("bad Timestamp \"" + text + "\"");
            }
            m.set(m.def.fields[secSecs].number, Long.valueOf(secs));
            m.set(m.def.fields[secNanos].number, Integer.valueOf(frac));
        }

        /** A duration: {@code [-]seconds[.frac]s}. */
        void parseDuration(GenericCodec.DMsg m) {
            int secSecs = timeSlot(m.def, 1, GenericCodec.K_INT64);
            int secNanos = timeSlot(m.def, 2, GenericCodec.K_INT32);
            if (secSecs < 0 || secNanos < 0) {
                fail("invalid google.protobuf.Duration shape");
            }
            if (peekKind() == JK_NULL) {
                expectLit("null");
                return;
            }
            String text = parseString();
            int intEnd = 0;
            while (intEnd < text.length()
                    && ((text.charAt(intEnd) >= '0' && text.charAt(intEnd) <= '9')
                            || text.charAt(intEnd) == '-')) {
                intEnd++;
            }
            Long secs = atoi64(text.substring(0, intEnd));
            if (intEnd == 0 || secs == null) {
                fail("bad Duration \"" + text + "\"");
            }
            final long max = 3652500L * 86400;
            if (secs > max || secs < -max) {
                fail("bad Duration \"" + text + "\"");
            }
            int nanos = 0;
            int i = intEnd;
            if (i < text.length() && text.charAt(i) == '.') {
                i++;
                int frac = 0;
                int fracDigits = 0;
                while (i < text.length() && text.charAt(i) >= '0'
                        && text.charAt(i) <= '9') {
                    if (fracDigits >= 9) {
                        fail("bad Duration \"" + text + "\"");
                    }
                    frac = frac * 10 + (text.charAt(i) - '0');
                    fracDigits++;
                    i++;
                }
                if (fracDigits == 0) {
                    fail("bad Duration \"" + text + "\"");
                }
                for (int k = fracDigits; k < 9; k++) {
                    frac *= 10;
                }
                nanos = frac;
            }
            boolean neg = secs < 0 || (intEnd > 0 && text.charAt(0) == '-');
            if (neg) {
                nanos = -nanos;
            }
            if (text.length() - i != 1 || text.charAt(i) != 's') {
                fail("bad Duration \"" + text + "\"");
            }
            m.set(m.def.fields[secSecs].number, secs);
            m.set(m.def.fields[secNanos].number, Integer.valueOf(nanos));
        }

        /** A FieldMask: one comma-separated camelCase string. */
        void parseFieldMask(GenericCodec.DMsg m) {
            if (peekKind() == JK_NULL) {
                expectLit("null");
                return;
            }
            String text = parseString();
            int paths = m.def.lookup(1);
            if (paths < 0) {
                fail("invalid google.protobuf.FieldMask shape");
            }
            GenericCodec.Field f = m.def.fields[paths];
            if (text.isEmpty()) {
                return; // an empty string is an empty mask, not one empty path
            }
            int from = 0;
            while (true) {
                int comma = text.indexOf(',', from);
                int to = comma < 0 ? text.length() : comma;
                StringBuilder snake = new StringBuilder();
                for (int i = from; i < to; i++) {
                    char c = text.charAt(i);
                    if (c >= 'A' && c <= 'Z') {
                        snake.append('_').append((char) (c - 'A' + 'a'));
                    } else {
                        snake.append(c);
                    }
                }
                m.add(f.number, snake.toString());
                if (comma < 0) {
                    break;
                }
                from = comma + 1;
            }
        }
    }

    /* ---------------- scalar text helpers ---------------- */

    private static boolean hasSpace(String s) {
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                return true;
            }
        }
        return false;
    }

    /** Strict [+-]?digits with 64-bit range checking; null on failure. */
    private static Long atoi64(String text) {
        if (text.isEmpty()) {
            return null;
        }
        try {
            return Long.valueOf(Long.parseLong(text));
        } catch (NumberFormatException e) {
            return null;
        }
    }

    /** Strict unsigned digits (an optional '+'); null on failure. */
    private static Long atou64(String text) {
        if (text.isEmpty() || text.charAt(0) == '-') {
            return null;
        }
        try {
            return Long.valueOf(Long.parseUnsignedLong(text));
        } catch (NumberFormatException e) {
            return null;
        }
    }

    /** strtod with full consumption; whitespace-tolerant, null on failure. */
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

    private static byte[] base64(String text) {
        int n = text.length();
        int pad = 0;
        while (n - pad > 0 && text.charAt(n - pad - 1) == '=') {
            pad++;
        }
        int len = n - pad;
        if (pad > 2) {
            throw new PbwireException("JSON: bad base64 padding", 1);
        }
        if (len == 0) {
            if (n == 0) {
                return new byte[0];
            }
            throw new PbwireException("JSON: base64 padding only", 1);
        }
        if (pad == 0 ? (len % 4 == 1) : (n % 4 != 0)) {
            throw new PbwireException("JSON: bad base64 length", 1);
        }
        byte[] out = new byte[(len / 4) * 3 + 3];
        int o = 0;
        int i = 0;
        while (i < (len & ~3)) {
            int a = b64val(text.charAt(i));
            int b = b64val(text.charAt(i + 1));
            int c = b64val(text.charAt(i + 2));
            int d = b64val(text.charAt(i + 3));
            if (a < 0 || b < 0 || c < 0 || d < 0) {
                throw new PbwireException("JSON: bad base64", 1);
            }
            out[o++] = (byte) ((a << 2) | (b >> 4));
            out[o++] = (byte) (((b & 15) << 4) | (c >> 2));
            out[o++] = (byte) (((c & 3) << 6) | d);
            i += 4;
        }
        if (len - i == 2) {
            int a = b64val(text.charAt(i));
            int b = b64val(text.charAt(i + 1));
            if (a < 0 || b < 0) {
                throw new PbwireException("JSON: bad base64", 1);
            }
            out[o++] = (byte) ((a << 2) | (b >> 4));
        } else if (len - i == 3) {
            int a = b64val(text.charAt(i));
            int b = b64val(text.charAt(i + 1));
            int c = b64val(text.charAt(i + 2));
            if (a < 0 || b < 0 || c < 0) {
                throw new PbwireException("JSON: bad base64", 1);
            }
            out[o++] = (byte) ((a << 2) | (b >> 4));
            out[o++] = (byte) (((b & 15) << 4) | (c >> 2));
        }
        byte[] trimmed = new byte[o];
        System.arraycopy(out, 0, trimmed, 0, o);
        return trimmed;
    }

    private static int b64val(char c) {
        if (c >= 'A' && c <= 'Z') {
            return c - 'A';
        }
        if (c >= 'a' && c <= 'z') {
            return c - 'a' + 26;
        }
        if (c >= '0' && c <= '9') {
            return c - '0' + 52;
        }
        if (c == '+' || c == '-') {
            return 62;
        }
        if (c == '/' || c == '_') {
            return 63;
        }
        return -1;
    }
}

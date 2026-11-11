import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

/**
 * Well-known-type helpers over the dynamic tree, like the core's
 * {@code wkt.c}: {@code google.protobuf.Any} packing plus
 * Timestamp/Duration millisecond conversion and FieldMask paths.
 *
 * <p>Field numbers only (Any 1/2, seconds/nanos 1/2, paths 1), so any
 * descriptor with the same shape works; the helpers never special-case
 * WKTs anywhere else.
 */
public final class Wkt {
    private Wkt() {
    }

    /** Packs a payload with its type URL into a new {@code Any} message. */
    public static Message anyPack(MessageType anyType, String typeUrl, byte[] payload) {
        if (anyType == null || typeUrl == null || payload == null) {
            throw new PbwireException("anyPack needs a type, a URL and a payload");
        }
        Message m = anyType.newMessage();
        m.set(1, typeUrl);
        m.set(2, payload);
        return m;
    }

    /** Packs an encoded message with its type URL into a new {@code Any}. */
    public static Message anyPackMessage(MessageType anyType, String typeUrl,
            Message payload) {
        if (payload == null) {
            throw new PbwireException("anyPackMessage needs a payload");
        }
        return anyPack(anyType, typeUrl, payload.encode());
    }

    /** The type URL of an {@code Any} message, or null when absent. */
    public static String anyTypeUrl(Message any) {
        if (any == null) {
            return null;
        }
        return any.getString(1);
    }

    /** A copy of the {@code Any} payload, or null when absent. */
    public static byte[] anyPayload(Message any) {
        if (any == null) {
            return null;
        }
        return any.getBytes(2);
    }

    /**
     * Decodes the {@code Any} payload as {@code type}. A present type URL
     * must name that type (matched by suffix, so any URL prefix works).
     */
    public static Message anyUnpack(Message any, MessageType type) {
        if (any == null || type == null) {
            throw new PbwireException("anyUnpack needs a value and a type");
        }
        String url = any.getString(1);
        // The type name is the segment after the last '/': a suffix match would
        // also accept "myapp/other.Msg" for the type "Msg".
        if (url != null && !url.isEmpty()) {
            String named = url.substring(url.lastIndexOf('/') + 1);
            if (!named.equals(type.name())) {
                throw new PbwireException("Any type " + url + " is not "
                        + type.name());
            }
        }
        byte[] payload = any.getBytes(2);
        if (payload == null) {
            throw new PbwireException("Any has no payload");
        }
        return type.decode(payload);
    }

    /** Seconds (field 1) + nanos (field 2) as milliseconds; shared by both shapes. */
    private static long toMillis(Message msg, String what) {
        if (msg == null) {
            throw new PbwireException(what + " needs a value");
        }
        Object sec = msg.get(1);
        if (!(sec instanceof Number)) {
            throw new PbwireException(what + " has no seconds");
        }
        long secs = ((Number) sec).longValue();
        Object nano = msg.get(2);
        if (nano != null && !(nano instanceof Number)) {
            throw new PbwireException(what + " nanos are not integers");
        }
        long nanos = nano == null ? 0 : ((Number) nano).longValue();
        if (nanos > 999999999L || nanos < -999999999L) {
            throw new PbwireException(what + " nanos out of range", 1);
        }
        // sec*1000 + nanos/1e6 must stay inside int64; both bounds are checked
        // against the actual remainder (mirrors the core's wkt_to_millis).
        long frac = nanos / 1000000;
        long hi = (Long.MAX_VALUE - (frac > 0 ? frac : 0)) / 1000;
        long lo = (Long.MIN_VALUE - (frac < 0 ? frac : 0)) / 1000;
        if (secs > hi || secs < lo) {
            throw new PbwireException(what + " seconds out of range", 5);
        }
        return secs * 1000 + frac;
    }

    /** A Timestamp message as milliseconds since the epoch. */
    public static long timestampToMillis(Message ts) {
        return toMillis(ts, "Timestamp");
    }

    /** A Duration message as milliseconds. */
    public static long durationToMillis(Message dur) {
        return toMillis(dur, "Duration");
    }

    private static Message fromMillis(MessageType type, long ms, boolean floorNanos,
            String what) {
        if (type == null) {
            throw new PbwireException(what + " needs a type");
        }
        long secs = ms / 1000;
        long nanos = (ms % 1000) * 1000000;
        if (floorNanos && nanos < 0) { // Timestamps keep nanos in [0, 1e9)
            secs -= 1;
            nanos += 1000000000;
        }
        // The pair must convert back: secs*1000 has to stay inside int64, and
        // the Timestamp borrow can step one second below the range at INT64_MIN.
        if (secs > Long.MAX_VALUE / 1000 || secs < Long.MIN_VALUE / 1000) {
            throw new PbwireException(what + " milliseconds out of range", 5);
        }
        Message m = type.newMessage();
        m.set(1, secs);
        m.set(2, nanos);
        return m;
    }

    /** Milliseconds since the epoch as a Timestamp message. */
    public static Message timestampFromMillis(MessageType type, long ms) {
        return fromMillis(type, ms, true, "Timestamp");
    }

    /** Milliseconds as a Duration message; both fields keep the sign. */
    public static Message durationFromMillis(MessageType type, long ms) {
        return fromMillis(type, ms, false, "Duration");
    }

    /** The paths of a FieldMask message (field 1). */
    public static List<String> fieldMaskPaths(Message mask) {
        if (mask == null) {
            return Collections.emptyList();
        }
        List<Object> items = mask.getList(1);
        List<String> out = new ArrayList<>(items.size());
        for (Object v : items) {
            if (!(v instanceof String)) {
                throw new PbwireException("FieldMask paths must be strings");
            }
            out.add((String) v);
        }
        return Collections.unmodifiableList(out);
    }

    /** A FieldMask message holding these paths. */
    public static Message newFieldMask(MessageType type, List<String> paths) {
        if (type == null || paths == null) {
            throw new PbwireException("newFieldMask needs a type and paths");
        }
        Message m = type.newMessage();
        for (String p : paths) {
            m.set(1, p);
        }
        return m;
    }
}

import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.HashMap;
import java.util.Map;

/**
 * Google conformance testee driven by the pbwire Java binding.
 *
 * <p>Part of the repo-wide conformance story: {@code conformance/testee.c}
 * proves the dynamic core, this program proves the Java binding on the same
 * requests.
 *
 * <p>It speaks the runner's wire protocol (4-byte little-endian length prefix,
 * then a ConformanceRequest / ConformanceResponse) and, because the binding is
 * schema-driven, it needs no per-message code: the request names the message
 * type, the descriptor set answers with the schema.
 *
 * <p>Supported: binary, JSON and text in -&gt; binary, JSON or text out. JSPB
 * answers {@code skipped}, like every other testee here.
 *
 * <p>{@code decode()} validates the whole payload including proto2
 * {@code required} fields, and a parsed message keeps the bytes it was parsed
 * from, so {@code encode()} hands them back: a round trip preserves unknown
 * fields and repeated fields in wire order.
 *
 * <pre>
 *   java -cp out Testee build/conformance.desc     # from bindings/java
 * </pre>
 */
public class Testee {
    // ConformanceRequest.requested_output_format values.
    static final int PROTOBUF = 1;
    static final int JSON = 2;
    static final int TEXT = 4;

    // ConformanceRequest.test_category values.
    static final int CATEGORY_JSON_IGNORE_UNKNOWN = 3;

    // ConformanceResponse field numbers: exactly one result field is set.
    static final int PARSE_ERROR = 1;
    static final int RUNTIME_ERROR = 2;
    static final int PROTOBUF_PAYLOAD = 3;
    static final int JSON_PAYLOAD = 4;
    static final int SKIPPED = 5;
    static final int SERIALIZE_ERROR = 6;
    static final int TEXT_PAYLOAD = 8;

    private final MessageType requestType;
    private final MessageType responseType;
    private final Schema schema;
    private final Map<String, MessageType> types = new HashMap<>();

    Testee(Schema schema) {
        this.schema = schema;
        this.requestType = schema.message("conformance.ConformanceRequest");
        this.responseType = schema.message("conformance.ConformanceResponse");
    }

    private MessageType messageOf(String name) {
        MessageType t = types.get(name);
        if (t == null) {
            t = schema.message(name);
            types.put(name, t);
        }
        return t;
    }

    private Message handle(byte[] body) {
        Message response = responseType.newMessage();
        Message request;
        try {
            request = requestType.decode(body);
        } catch (RuntimeException e) { // malformed framing input, never a crash
            return response.set(RUNTIME_ERROR,
                    "cannot parse ConformanceRequest: " + e.getMessage());
        }
        long wanted = request.getLong(3);
        long category = request.getLong(5);
        String name = request.getString(4);
        byte[] payload = request.getBytes(1);
        String json = request.getString(2);
        String text = request.getString(8);
        boolean printUnknown = request.getBool(9);
        if (wanted != PROTOBUF && wanted != JSON && wanted != TEXT) {
            return response.set(SKIPPED,
                    "output format " + wanted + " is not supported");
        }
        MessageType type;
        try {
            type = messageOf(name);
        } catch (RuntimeException e) {
            return response.set(RUNTIME_ERROR,
                    "unknown message type '" + name + "'");
        }
        Message parsed;
        try {
            if (json != null) {
                parsed = type.parseJson(json,
                        category == CATEGORY_JSON_IGNORE_UNKNOWN, null);
            } else if (text != null) {
                parsed = type.parseText(text);
            } else if (payload != null) {
                parsed = type.decode(payload);
            } else {
                return response.set(SKIPPED, "unsupported payload");
            }
        } catch (RuntimeException e) { // refused input: verdict, not a bug
            return response.set(PARSE_ERROR, String.valueOf(e.getMessage()));
        }
        try {
            if (wanted == JSON) {
                return response.set(JSON_PAYLOAD, parsed.toJson());
            }
            if (wanted == TEXT) {
                // The runner asks for unknown fields to be dropped in the
                // *_UnknownFields_Drop cases; print them only when asked.
                return response.set(TEXT_PAYLOAD, parsed.toText(printUnknown));
            }
            return response.set(PROTOBUF_PAYLOAD, parsed.encode());
        } catch (RuntimeException e) {
            return response.set(SERIALIZE_ERROR, String.valueOf(e.getMessage()));
        }
    }

    private static byte[] readRequest(InputStream in) throws IOException {
        byte[] header = in.readNBytes(4);
        if (header.length < 4) {
            return null; // clean end of input (or a truncated header)
        }
        int size = (header[0] & 0xFF) | ((header[1] & 0xFF) << 8)
                | ((header[2] & 0xFF) << 16) | ((header[3] & 0xFF) << 24);
        byte[] body = in.readNBytes(size);
        if (body.length < size) {
            return null;
        }
        return body;
    }

    private static void writeResponse(OutputStream out, byte[] payload)
            throws IOException {
        int len = payload.length;
        out.write(new byte[] {
            (byte) (len & 0xFF), (byte) ((len >> 8) & 0xFF),
            (byte) ((len >> 16) & 0xFF), (byte) ((len >> 24) & 0xFF)
        });
        out.write(payload);
        out.flush();
    }

    public static void main(String[] args) throws IOException {
        if (args.length < 1) {
            System.err.println("usage: Testee <conformance.desc>");
            System.exit(2);
        }
        Schema schema = Schema.fromFile(args[0]);
        Testee testee = new Testee(schema);
        InputStream in = System.in;
        OutputStream out = System.out;
        for (;;) {
            byte[] body = readRequest(in);
            if (body == null) {
                return;
            }
            Message response = testee.handle(body);
            byte[] payload;
            try {
                payload = response.encode();
            } catch (RuntimeException e) {
                payload = testee.responseType.newMessage()
                        .set(RUNTIME_ERROR,
                                "cannot encode response: " + e.getMessage())
                        .encode();
            }
            writeResponse(out, payload);
        }
    }
}

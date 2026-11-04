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
 * <p>Supported: binary in -&gt; binary out. JSON and text output need printers the
 * binding does not expose, so those requests answer {@code skipped} and the
 * runner reports them as skipped rather than failed.
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

    // ConformanceResponse field numbers: exactly one result field is set.
    static final int PARSE_ERROR = 1;
    static final int RUNTIME_ERROR = 2;
    static final int PROTOBUF_PAYLOAD = 3;
    static final int SKIPPED = 5;
    static final int SERIALIZE_ERROR = 6;

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
        String name = request.getString(4);
        byte[] payload = request.getBytes(1);
        if (wanted != PROTOBUF) {
            return response.set(SKIPPED,
                    "output format " + wanted + " is not supported");
        }
        if (payload == null) {
            return response.set(SKIPPED, "only binary input is supported");
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
            parsed = type.decode(payload);
        } catch (RuntimeException e) { // refused input: verdict, not a bug
            return response.set(PARSE_ERROR, String.valueOf(e.getMessage()));
        }
        try {
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

import java.io.IOException;
import java.io.UncheckedIOException;
import java.nio.file.Files;
import java.nio.file.Path;

/**
 * A loaded {@code FileDescriptorSet}: the runtime schema every MessageType comes
 * from. No generated code, no protoc plugin, no native library -- the parser
 * in {@code Proto} reads the bytes protoc emitted, and {@code GenericCodec}
 * owns the wire format.
 *
 * <p>Closing the schema marks it unusable; messages and types created from it
 * must not be used afterwards (they throw {@link IllegalStateException}).
 */
public final class Schema implements AutoCloseable {
    private final GenericCodec.Schema schema;
    private boolean closed;

    private Schema(GenericCodec.Schema schema) {
        this.schema = schema;
    }

    /** Loads a descriptor set written by {@code protoc --descriptor_set_out}. */
    public static Schema fromFile(String path) {
        try {
            return fromBytes(Files.readAllBytes(Path.of(path)));
        } catch (IOException e) {
            throw new UncheckedIOException(path, e);
        }
    }

    /** Loads descriptor-set bytes; use {@code --include_imports} so every type resolves. */
    public static Schema fromBytes(byte[] descriptorSet) {
        try {
            return new Schema(GenericCodec.Schema.load(descriptorSet));
        } catch (IllegalArgumentException e) {
            throw new PbwireException("descriptor set: " + e.getMessage(), 1);
        }
    }

    /** Looks up a message by its fully qualified name, e.g. {@code "pbwire.test.Sample"}. */
    public MessageType message(String fullName) {
        checkOpen();
        try {
            return new MessageType(this, schema.message(fullName));
        } catch (IllegalArgumentException e) {
            throw new PbwireException("no such message type: " + fullName);
        }
    }

    void checkOpen() {
        if (closed) {
            throw new IllegalStateException("schema is closed");
        }
    }

    @Override
    public void close() {
        closed = true;
    }
}

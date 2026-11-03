import java.io.IOException;
import java.io.UncheckedIOException;
import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.ValueLayout;
import java.lang.ref.Cleaner;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.concurrent.atomic.AtomicReference;

/**
 * A loaded {@code FileDescriptorSet}: the runtime schema every MessageType comes
 * from. No generated code, no protoc plugin -- load the bytes protoc emitted.
 *
 * <p>Closing the schema frees it; messages and types created from it must not be
 * used afterwards (they throw {@link IllegalStateException}).
 */
public final class Schema implements AutoCloseable {
    private final AtomicReference<MemorySegment> handle;
    private final Cleaner.Cleanable cleanable;

    private Schema(MemorySegment schema) {
        handle = new AtomicReference<>(schema);
        /* The action holds the handle reference, not this schema, so the cleaner
         * can see the schema become unreachable. */
        cleanable = Pbwire.CLEANER.register(this, new Pbwire.FreeOnce(handle, Pbwire::schemaFree));
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
        Pbwire.Workspace workspace = Pbwire.workspace();
        MemorySegment data = workspace.input(descriptorSet.length);
        MemorySegment.copy(descriptorSet, 0, data, ValueLayout.JAVA_BYTE, 0, descriptorSet.length);
        MemorySegment out = workspace.messageOut();
        int status = Pbwire.descriptorSetLoad(data, descriptorSet.length, out);
        MemorySegment schema = out.get(Pbwire.PTR, 0);
        if (status != Pbwire.OK) {
            /* The loader always hands back the partial schema so its error string is readable. */
            PbwireException failure = new PbwireException("descriptor set: " + errorText(schema, status), status);
            Pbwire.schemaFree(schema);
            throw failure;
        }
        return new Schema(Pbwire.notNull(schema, "descriptor set: no schema returned"));
    }

    private static String errorText(MemorySegment schema, int status) {
        String why = Pbwire.isNull(schema) ? "" : Pbwire.cstr(Pbwire.schemaError(schema));
        return why.isEmpty() ? Pbwire.strerror(status) : why;
    }

    /** Looks up a message by its fully qualified name, e.g. {@code "pbwire.test.Sample"}. */
    public MessageType message(String fullName) {
        MemorySegment schema = segment();
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment desc = Pbwire.schemaFind(schema, arena.allocateFrom(fullName));
            if (Pbwire.isNull(desc)) {
                throw new PbwireException("no such message type: " + fullName);
            }
            return new MessageType(this, desc);
        }
    }

    /** The native schema; throws once the schema is closed. */
    MemorySegment segment() {
        MemorySegment schema = handle.get();
        if (schema == null) {
            throw new IllegalStateException("schema is closed");
        }
        return schema;
    }

    @Override
    public void close() {
        cleanable.clean();
    }
}

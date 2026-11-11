/**
 * An RPC service: resolves its methods' input and output types, like the
 * core's {@code pbw_service_*}. Services load with the descriptor set and
 * are read-only after that, so they are safe to share between threads.
 */
public final class ServiceType {
    private final Schema schema;
    final GenericCodec.SvcDef def;

    ServiceType(Schema schema, GenericCodec.SvcDef def) {
        this.schema = schema;
        this.def = def;
    }

    /** Fully qualified name, e.g. {@code "pbwire.test.Search"}. */
    public String name() {
        return def.fullName;
    }

    /** The number of methods in declaration order. */
    public int methodCount() {
        return def.methods.length;
    }

    /** The method with this name; throws when the service has no such method. */
    public MethodType method(String name) {
        Integer i = def.byName.get(name);
        if (i == null) {
            throw new PbwireException(name() + ": no such method: " + name);
        }
        return new MethodType(this, def.methods[i]);
    }

    /** One RPC method: its name, its streaming flags and its linked types. */
    public static final class MethodType {
        private final ServiceType service;
        private final GenericCodec.MethodDef def;

        MethodType(ServiceType service, GenericCodec.MethodDef def) {
            this.service = service;
            this.def = def;
        }

        public String name() {
            return def.name;
        }

        /** The request message type. */
        public MessageType inputType() {
            return new MessageType(service.schema, def.inputType);
        }

        /** The response message type. */
        public MessageType outputType() {
            return new MessageType(service.schema, def.outputType);
        }

        public boolean clientStreaming() {
            return def.clientStreaming;
        }

        public boolean serverStreaming() {
            return def.serverStreaming;
        }

        @Override
        public String toString() {
            return service.name() + "." + def.name;
        }
    }

    @Override
    public String toString() {
        return name();
    }
}

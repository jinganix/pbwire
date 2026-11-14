import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/*
 * Minimal FileDescriptorSet model + parser: the only reader of
 * descriptor.proto the pure-Java binding needs. Parses just enough for a
 * binary codec (names, numbers, labels, types, type names, oneofs,
 * extensions, packedness, syntax) and skips everything else by wire type,
 * so unknown descriptor.proto additions never break loading.
 *
 * <p>Kept separate from GenericCodec: this file knows the descriptor wire
 * layout, GenericCodec knows the message wire layout. Neither knows
 * protobuf-java; the runtime has no dependencies at all.
 */
final class Proto {
    private Proto() {
    }

    enum Type {
        DOUBLE, FLOAT, INT64, UINT64, INT32, FIXED64, FIXED32, BOOL, STRING,
        GROUP, MESSAGE, BYTES, UINT32, ENUM, SFIXED32, SFIXED64, SINT32, SINT64;

        static Type of(int n) {
            switch (n) {
                case 1:
                    return DOUBLE;
                case 2:
                    return FLOAT;
                case 3:
                    return INT64;
                case 4:
                    return UINT64;
                case 5:
                    return INT32;
                case 6:
                    return FIXED64;
                case 7:
                    return FIXED32;
                case 8:
                    return BOOL;
                case 9:
                    return STRING;
                case 10:
                    return GROUP;
                case 11:
                    return MESSAGE;
                case 12:
                    return BYTES;
                case 13:
                    return UINT32;
                case 14:
                    return ENUM;
                case 15:
                    return SFIXED32;
                case 16:
                    return SFIXED64;
                case 17:
                    return SINT32;
                case 18:
                    return SINT64;
                default:
                    throw new IllegalArgumentException("bad field type " + n);
            }
        }
    }

    enum Label {
        OPTIONAL, REQUIRED, REPEATED;

        static Label of(int n) {
            switch (n) {
                case 1:
                    return OPTIONAL;
                case 2:
                    return REQUIRED;
                case 3:
                    return REPEATED;
                default:
                    throw new IllegalArgumentException("bad label " + n);
            }
        }
    }

    static final class Oneof {
        String name;
    }

    static final class Field {
        String name;
        String extendee; // extensions only: ".pkg.Message"
        String fullName; // extensions only: the global name [a.b] addresses
        int number;
        Label label = Label.OPTIONAL;
        Type type;
        String typeName; // ".pkg.Msg" / ".pkg.Enum", message + enum + group
        int oneofIndex = -1;
        boolean proto3Optional;
        boolean packed; // resolved: explicit option or the syntax default
        boolean hasPackedOption;
        boolean hasPresence; // resolved: explicit presence (not proto3 plain)
        int enumOpen; // editions FeatureSet.enum_type: 0 unset, 1 OPEN, 2 CLOSED
        int utf8; // editions FeatureSet.utf8_validation: 0 unset, 2 VERIFY, 3 NONE
        Message messageType; // linked: message + group fields
        Enum enumType; // linked: enum fields
        boolean enumClosed; // linked: unknown numbers go to the unknown fields
        Oneof oneof; // linked
        boolean isExtension;

        /** The global name of an extension: [scope.name] addresses it. */
        String scope(String enclosing) {
            return enclosing.isEmpty() ? name : enclosing + "." + name;
        }
    }

    /** One enum type: its values (aliases repeat a number) and its openness. */
    static final class Enum {
        String name;
        String fullName;
        final List<EnumValue> values = new ArrayList<>();
        int enumOpen; // editions FeatureSet.enum_type: 0 unset, 1 OPEN, 2 CLOSED
        boolean closed; // linked: proto2 (and CLOSED editions) reject unknown numbers
    }

    static final class EnumValue {
        String name;
        int number;
    }

    static final class Message {
        String name;
        String fullName;
        boolean mapEntry; // options.map_entry: the JSON/text maps entry shape
        final List<Field> fields = new ArrayList<>();
        final List<Message> nested = new ArrayList<>();
        final List<Field> extensions = new ArrayList<>();
        final List<Oneof> oneofs = new ArrayList<>();
        final List<Enum> enums = new ArrayList<>();
        final List<String> reservedNames = new ArrayList<>();
    }

    static final class Service {
        String name;
        String fullName;
        final List<Method> methods = new ArrayList<>();
    }

    static final class Method {
        String name;
        String inputType; // e.g. ".pkg.Request"
        String outputType;
        boolean clientStreaming;
        boolean serverStreaming;
        Message input; // linked
        Message output; // linked
    }

    static final class File {
        String name;
        String pkg = "";
        String syntax = "proto2"; // absent means proto2
        boolean edition; // field 14: editions files behave as proto3
        int enumOpen; // options.features.enum_type: 0 unset, 1 OPEN, 2 CLOSED
        int utf8; // options.features.utf8_validation: 0 unset, 2 VERIFY, 3 NONE
        final List<String> deps = new ArrayList<>();
        final List<Message> messages = new ArrayList<>();
        final List<Field> extensions = new ArrayList<>();
        final List<Enum> enums = new ArrayList<>();
        final List<Service> services = new ArrayList<>();
    }

    /** Parses a FileDescriptorSet into files; call link() before encoding. */
    static List<File> parseSet(byte[] bytes) {
        Wire.Reader r = new Wire.Reader();
        r.b = bytes;
        r.pos = 0;
        r.end = bytes.length;
        List<File> files = new ArrayList<>();
        while (r.pos < r.end) {
            int tag = (int) Wire.rVar(r);
            if ((tag >>> 3) == 1 && (tag & 7) == Wire.WT_LEN) {
                int n = Wire.rLen(r);
                files.add(parseFile(r.b, r.pos, n));
                r.pos += n;
            } else {
                Wire.skip(r, tag & 7);
            }
        }
        return files;
    }

    /**
     * Links type names and computes presence/packed. Must run after every
     * file is parsed: extensions can attach across files.
     */
    static void link(List<File> files) {
        Map<String, Message> byName = new HashMap<>();
        Map<String, Enum> enums = new HashMap<>();
        for (File f : files) {
            for (Message m : f.messages) {
                index(m, f.pkg, byName, enums);
            }
            for (Enum e : f.enums) {
                indexEnum(e, f.pkg, enums);
            }
        }
        for (File f : files) {
            // Editions files carry syntax "editions" plus field 14: the core
            // treats them as proto3, so packed defaults and presence match.
            boolean proto3 = "proto3".equals(f.syntax) || f.edition;
            for (Enum e : f.enums) {
                linkEnum(e, proto3, f.enumOpen);
            }
            // Enums close/open before fields link: a field copies its enum's
            // closed flag, so the enum must be linked first.
            for (Message m : f.messages) {
                linkEnums(m, proto3, f.enumOpen);
            }
            for (Message m : f.messages) {
                linkMessage(m, f.pkg, proto3, f.enumOpen, f.utf8, byName,
                            enums);
            }
            for (Field e : f.extensions) {
                linkField(e, f.pkg, proto3, f.utf8, byName, enums, null);
                e.fullName = e.scope(f.pkg);
            }
            for (Service s : f.services) {
                s.fullName = f.pkg.isEmpty() ? s.name : f.pkg + "." + s.name;
                for (Method m : s.methods) {
                    if (m.name == null || m.inputType == null
                            || m.outputType == null) {
                        throw new IllegalArgumentException("method without name/input/output");
                    }
                    m.input = resolve(byName, f.pkg, m.inputType);
                    m.output = resolve(byName, f.pkg, m.outputType);
                    if (m.input == null || m.output == null) {
                        throw new IllegalArgumentException("method " + s.fullName
                                + "." + m.name + ": no such type");
                    }
                }
            }
        }
    }

    private static void index(Message m, String scope, Map<String, Message> byName,
            Map<String, Enum> enums) {
        m.fullName = scope.isEmpty() ? m.name : scope + "." + m.name;
        if (byName.put(m.fullName, m) != null) {
            throw new IllegalArgumentException("duplicate message " + m.fullName);
        }
        for (Enum e : m.enums) {
            indexEnum(e, m.fullName, enums);
        }
        for (Message n : m.nested) {
            index(n, m.fullName, byName, enums);
        }
    }

    private static void indexEnum(Enum e, String scope, Map<String, Enum> enums) {
        e.fullName = scope.isEmpty() ? e.name : scope + "." + e.name;
        if (enums.put(e.fullName, e) != null) {
            throw new IllegalArgumentException("duplicate enum " + e.fullName);
        }
    }

    /** An enum is closed for proto2 files (and editions files asking for it). */
    private static void linkEnum(Enum e, boolean proto3, int fileEnumOpen) {
        if (e.name == null) {
            throw new IllegalArgumentException("enum without a name");
        }
        if (e.enumOpen != 0) {
            e.closed = e.enumOpen == 2;
        } else if (fileEnumOpen != 0) {
            e.closed = fileEnumOpen == 2;
        } else {
            e.closed = !proto3;
        }
    }

    private static void linkMessage(Message m, String scope, boolean proto3,
            int fileEnumOpen, int fileUtf8, Map<String, Message> byName,
            Map<String, Enum> enums) {
        for (Field f : m.fields) {
            linkField(f, scope, proto3, fileUtf8, byName, enums, m);
        }
        for (Message n : m.nested) {
            linkMessage(n, m.fullName, proto3, fileEnumOpen, fileUtf8, byName,
                        enums);
        }
        for (Field e : m.extensions) {
            linkField(e, scope, proto3, fileUtf8, byName, enums, null);
            e.fullName = e.scope(m.fullName);
        }
    }

    /** Links the enums of a message and its nested messages (before fields). */
    private static void linkEnums(Message m, boolean proto3, int fileEnumOpen) {
        for (Enum e : m.enums) {
            linkEnum(e, proto3, fileEnumOpen);
        }
        for (Message n : m.nested) {
            linkEnums(n, proto3, fileEnumOpen);
        }
    }

    private static void linkField(Field f, String scope, boolean proto3,
            Map<String, Message> byName, Map<String, Enum> enums, Message parent) {
        linkField(f, scope, proto3, 0, byName, enums, parent);
    }

    private static void linkField(Field f, String scope, boolean proto3,
            int fileUtf8, Map<String, Message> byName, Map<String, Enum> enums,
            Message parent) {
        if (f.oneofIndex >= 0 && parent != null) {
            if (f.oneofIndex >= parent.oneofs.size()) {
                throw new IllegalArgumentException("field " + f.name
                        + ": oneof_index out of range");
            }
            f.oneof = parent.oneofs.get(f.oneofIndex);
        }
        if ((f.type == Type.MESSAGE || f.type == Type.GROUP) && f.typeName != null) {
            f.messageType = resolve(byName, scope, f.typeName);
            if (f.messageType == null) {
                throw new IllegalArgumentException("field " + f.name
                        + ": no such type " + f.typeName);
            }
        }
        if (f.type == Type.ENUM && f.typeName != null) {
            f.enumType = resolveEnum(enums, scope, f.typeName);
            if (f.enumType == null) {
                throw new IllegalArgumentException("field " + f.name
                        + ": no such enum " + f.typeName);
            }
            // Editions may close one field in an open file; that wins over
            // the enum type, exactly like the core's per-field enum_open.
            f.enumClosed = f.enumOpen != 0 ? f.enumOpen == 2
                    : f.enumType.closed;
        }
        if (f.label == Label.REPEATED && isPackable(f.type) && !f.hasPackedOption) {
            f.packed = proto3; // the syntax default
        }
        /* utf8_validation: field FeatureSet, then the file's, then the dialect
         * default (VERIFY for proto3/editions, NONE for proto2). Only `string`
         * fields carry it, and only they validate on parse, like the
         * reference. */
        if (f.type == Type.STRING) {
            int utf8 = f.utf8 != 0 ? f.utf8 : (fileUtf8 != 0 ? fileUtf8
                    : (proto3 ? 2 : 3));
            f.utf8 = utf8 == 2 ? 2 : 3;
        } else {
            f.utf8 = 3;
        }
        f.hasPresence = f.label != Label.REPEATED
                && (f.type == Type.MESSAGE || f.type == Type.GROUP
                        || f.label == Label.REQUIRED || !proto3 || f.proto3Optional);
    }

    private static boolean isPackable(Type t) {
        switch (t) {
            case STRING:
            case GROUP:
            case MESSAGE:
            case BYTES:
                return false;
            default:
                return true;
        }
    }

    /** Leading-dot names resolve from the root; otherwise search the scope up. */
    private static Message resolve(Map<String, Message> byName, String scope,
            String name) {
        if (name.startsWith(".")) {
            return byName.get(name.substring(1));
        }
        String s = scope;
        while (true) {
            String cand = s.isEmpty() ? name : s + "." + name;
            Message m = byName.get(cand);
            if (m != null) {
                return m;
            }
            // Nested scopes qualify from the inside out: C++ scoping.
            int dot = name.indexOf('.');
            if (dot >= 0) {
                String head = name.substring(0, dot);
                String rest = name.substring(dot + 1);
                String prefix = s.isEmpty() ? head : s + "." + head;
                if (byName.containsKey(prefix)) {
                    return resolve(byName, prefix, rest);
                }
            }
            if (s.isEmpty()) {
                return null;
            }
            int cut = s.lastIndexOf('.');
            s = cut < 0 ? "" : s.substring(0, cut);
        }
    }

    /* ---------------- wire parsing ---------------- */
    private static File parseFile(byte[] b, int off, int len) {
        Wire.Reader r = sub(b, off, len);
        File f = new File();
        while (r.pos < r.end) {
            int tag = (int) Wire.rVar(r);
            int num = tag >>> 3;
            int wt = tag & 7;
            if (wt == Wire.WT_VARINT) {
                // Field 14 is the edition marker; its value is irrelevant.
                if (num == 14) {
                    Wire.rVar(r);
                    f.edition = true;
                } else {
                    Wire.skip(r, wt);
                }
                continue;
            }
            if (wt == Wire.WT_LEN) {
                int n = Wire.rLen(r);
                if (num == 4) {
                    f.messages.add(parseMessage(r.b, r.pos, n, 1));
                } else if (num == 5) {
                    f.enums.add(parseEnum(r.b, r.pos, n));
                } else if (num == 6) {
                    f.services.add(parseService(r.b, r.pos, n));
                } else if (num == 7) {
                    f.extensions.add(parseField(r.b, r.pos, n, true));
                } else if (num == 8) {
                    f.enumOpen = parseFeatures(r.b, r.pos, n, 50);
                    f.utf8 = parseFeaturesUtf8(r.b, r.pos, n);
                } else if (num == 1) {
                    f.name = str(r.b, r.pos, n);
                } else if (num == 2) {
                    f.pkg = str(r.b, r.pos, n);
                } else if (num == 3) {
                    f.deps.add(str(r.b, r.pos, n));
                } else if (num == 12) {
                    f.syntax = str(r.b, r.pos, n);
                }
                r.pos += n;
            } else {
                Wire.skip(r, wt);
            }
        }
        return f;
    }

    private static Message parseMessage(byte[] b, int off, int len, int depth) {
        // Descriptor nesting is peer data too: a FileDescriptorSet a few tens
        // of KB can nest thousands of DescriptorProtos, so cap it instead of
        // letting the recursion overflow the JVM stack.
        if (depth > Wire.MAX_DEPTH) {
            throw new IllegalArgumentException("descriptor nesting exceeds "
                    + Wire.MAX_DEPTH);
        }
        Wire.Reader r = sub(b, off, len);
        Message m = new Message();
        while (r.pos < r.end) {
            int tag = (int) Wire.rVar(r);
            int num = tag >>> 3;
            if ((tag & 7) != Wire.WT_LEN) {
                Wire.skip(r, tag & 7);
                continue;
            }
            int n = Wire.rLen(r);
            if (num == 1) {
                m.name = str(r.b, r.pos, n);
            } else if (num == 2) {
                m.fields.add(parseField(r.b, r.pos, n, false));
            } else if (num == 3) {
                m.nested.add(parseMessage(r.b, r.pos, n, depth + 1));
            } else if (num == 6) {
                m.extensions.add(parseField(r.b, r.pos, n, true));
            } else if (num == 4) {
                m.enums.add(parseEnum(r.b, r.pos, n));
            } else if (num == 7) {
                m.mapEntry = parseMessageOptions(r.b, r.pos, n);
            } else if (num == 8) {
                m.oneofs.add(parseOneof(r.b, r.pos, n));
            } else if (num == 10) {
                m.reservedNames.add(str(r.b, r.pos, n));
            }
            r.pos += n;
        }
        if (m.name == null) {
            throw new IllegalArgumentException("message without a name");
        }
        return m;
    }

    private static Oneof parseOneof(byte[] b, int off, int len) {
        Wire.Reader r = sub(b, off, len);
        Oneof o = new Oneof();
        while (r.pos < r.end) {
            int tag = (int) Wire.rVar(r);
            if ((tag >>> 3) == 1 && (tag & 7) == Wire.WT_LEN) {
                int n = Wire.rLen(r);
                o.name = str(r.b, r.pos, n);
                r.pos += n;
            } else {
                Wire.skip(r, tag & 7);
            }
        }
        return o;
    }

    private static Enum parseEnum(byte[] b, int off, int len) {
        Wire.Reader r = sub(b, off, len);
        Enum e = new Enum();
        while (r.pos < r.end) {
            int tag = (int) Wire.rVar(r);
            int num = tag >>> 3;
            if ((tag & 7) != Wire.WT_LEN) {
                Wire.skip(r, tag & 7);
                continue;
            }
            int n = Wire.rLen(r);
            if (num == 1) {
                e.name = str(r.b, r.pos, n);
            } else if (num == 2) {
                e.values.add(parseEnumValue(r.b, r.pos, n));
            } else if (num == 3) {
                e.enumOpen = parseFeatures(r.b, r.pos, n, 7);
            }
            r.pos += n;
        }
        return e;
    }

    private static EnumValue parseEnumValue(byte[] b, int off, int len) {
        Wire.Reader r = sub(b, off, len);
        EnumValue v = new EnumValue();
        boolean hasNumber = false;
        while (r.pos < r.end) {
            int tag = (int) Wire.rVar(r);
            int num = tag >>> 3;
            switch (tag & 7) {
                case Wire.WT_VARINT:
                    if (num == 2) {
                        v.number = (int) Wire.rVar(r);
                        hasNumber = true;
                    } else {
                        Wire.skip(r, Wire.WT_VARINT);
                    }
                    break;
                case Wire.WT_LEN:
                    int n = Wire.rLen(r);
                    if (num == 1) {
                        v.name = str(r.b, r.pos, n);
                    }
                    r.pos += n;
                    break;
                default:
                    Wire.skip(r, tag & 7);
                    break;
            }
        }
        if (v.name == null || !hasNumber) {
            throw new IllegalArgumentException("enum value without name/number");
        }
        return v;
    }

    /** MessageOptions.message_set_wire_format is field 1; map_entry is 7. */
    private static boolean parseMessageOptions(byte[] b, int off, int len) {
        Wire.Reader r = sub(b, off, len);
        boolean mapEntry = false;
        while (r.pos < r.end) {
            int tag = (int) Wire.rVar(r);
            int num = tag >>> 3;
            if ((tag & 7) != Wire.WT_VARINT) {
                Wire.skip(r, tag & 7);
                continue;
            }
            if (num == 7) {
                mapEntry = Wire.rVar(r) != 0;
            } else {
                Wire.skip(r, Wire.WT_VARINT);
            }
        }
        return mapEntry;
    }

    /**
     * Reads one Options message and returns the FeatureSet.enum_type
     * (1 OPEN, 2 CLOSED) when it carries one: 0 means unset. `optionNumber` is
     * the field that holds the FeatureSet (50 file, 21 field, ...).
     */
    private static int parseFeatures(byte[] b, int off, int len, int optionNumber) {
        Wire.Reader r = sub(b, off, len);
        int open = 0;
        while (r.pos < r.end) {
            int tag = (int) Wire.rVar(r);
            int num = tag >>> 3;
            if ((tag & 7) != Wire.WT_LEN) {
                Wire.skip(r, tag & 7);
                continue;
            }
            int n = Wire.rLen(r);
            byte[] fb = r.b;
            int fs = r.pos;
            int fe = r.pos + n;
            if (num == optionNumber) {
                open = parseFeatureSetEnumType(fb, fs, fe);
            }
            r.pos += n;
        }
        return open;
    }

    /** FeatureSet.enum_type = 2: EnumType, 1 OPEN or 2 CLOSED. */
    private static int parseFeatureSetEnumType(byte[] b, int off, int end) {
        Wire.Reader r = sub(b, off, end - off);
        int open = 0;
        while (r.pos < r.end) {
            int tag = (int) Wire.rVar(r);
            if ((tag >>> 3) == 2 && (tag & 7) == Wire.WT_VARINT) {
                int v = (int) Wire.rVar(r);
                if (v == 1 || v == 2) {
                    open = v;
                }
            } else {
                Wire.skip(r, tag & 7);
            }
        }
        return open;
    }

    /**
     * FeatureSet.utf8_validation = 4: Utf8Validation, 2 VERIFY or 3 NONE.
     * 0 (unset) leaves the dialect default in charge: VERIFY for proto3 and
     * editions, NONE for proto2, exactly like the reference.
     */
    private static int parseFeatureSetUtf8(byte[] b, int off, int end) {
        Wire.Reader r = sub(b, off, end - off);
        int utf8 = 0;
        while (r.pos < r.end) {
            int tag = (int) Wire.rVar(r);
            if ((tag >>> 3) == 4 && (tag & 7) == Wire.WT_VARINT) {
                int v = (int) Wire.rVar(r);
                if (v == 2 || v == 3) {
                    utf8 = v;
                }
            } else {
                Wire.skip(r, tag & 7);
            }
        }
        return utf8;
    }

    /** FileOptions.features (field 50) -> FeatureSet.utf8_validation. */
    private static int parseFeaturesUtf8(byte[] b, int off, int len) {
        Wire.Reader r = sub(b, off, len);
        int utf8 = 0;
        while (r.pos < r.end) {
            int tag = (int) Wire.rVar(r);
            int num = tag >>> 3;
            if ((tag & 7) != Wire.WT_LEN) {
                Wire.skip(r, tag & 7);
                continue;
            }
            int n = Wire.rLen(r);
            if (num == 50) {
                utf8 = parseFeatureSetUtf8(r.b, r.pos, r.pos + n);
            }
            r.pos += n;
        }
        return utf8;
    }

    private static Service parseService(byte[] b, int off, int len) {
        Wire.Reader r = sub(b, off, len);
        Service s = new Service();
        while (r.pos < r.end) {
            int tag = (int) Wire.rVar(r);
            int num = tag >>> 3;
            if ((tag & 7) != Wire.WT_LEN) {
                Wire.skip(r, tag & 7);
                continue;
            }
            int n = Wire.rLen(r);
            if (num == 1) {
                s.name = str(r.b, r.pos, n);
            } else if (num == 2) {
                s.methods.add(parseMethod(r.b, r.pos, n));
            }
            r.pos += n;
        }
        if (s.name == null) {
            throw new IllegalArgumentException("service without a name");
        }
        return s;
    }

    private static Method parseMethod(byte[] b, int off, int len) {
        Wire.Reader r = sub(b, off, len);
        Method m = new Method();
        while (r.pos < r.end) {
            int tag = (int) Wire.rVar(r);
            int num = tag >>> 3;
            if ((tag & 7) == Wire.WT_VARINT) {
                boolean v = Wire.rVar(r) != 0;
                if (num == 5) {
                    m.clientStreaming = v;
                } else if (num == 6) {
                    m.serverStreaming = v;
                }
                continue;
            }
            if ((tag & 7) != Wire.WT_LEN) {
                Wire.skip(r, tag & 7);
                continue;
            }
            int n = Wire.rLen(r);
            if (num == 1) {
                m.name = str(r.b, r.pos, n);
            } else if (num == 2) {
                m.inputType = str(r.b, r.pos, n);
            } else if (num == 3) {
                m.outputType = str(r.b, r.pos, n);
            }
            r.pos += n;
        }
        return m;
    }

    private static Field parseField(byte[] b, int off, int len, boolean extension) {
        Wire.Reader r = sub(b, off, len);
        Field f = new Field();
        f.isExtension = extension;
        while (r.pos < r.end) {
            int tag = (int) Wire.rVar(r);
            int num = tag >>> 3;
            switch (tag & 7) {
                case Wire.WT_VARINT:
                    long v = Wire.rVar(r);
                    if (num == 3) {
                        f.number = (int) v;
                    } else if (num == 4) {
                        f.label = Label.of((int) v);
                    } else if (num == 5) {
                        f.type = Type.of((int) v);
                    } else if (num == 9) {
                        f.oneofIndex = (int) v;
                    } else if (num == 17) {
                        f.proto3Optional = v != 0;
                    }
                    break;
                case Wire.WT_LEN:
                    int n = Wire.rLen(r);
                    if (num == 1) {
                        f.name = str(r.b, r.pos, n);
                    } else if (num == 2) {
                        f.extendee = str(r.b, r.pos, n);
                    } else if (num == 6) {
                        f.typeName = str(r.b, r.pos, n);
                    } else if (num == 8) {
                        parseFieldOptions(r.b, r.pos, n, f);
                    }
                    r.pos += n;
                    break;
                default:
                    Wire.skip(r, tag & 7);
                    break;
            }
        }
        if (f.name == null || f.type == null || f.number == 0) {
            throw new IllegalArgumentException("field without name/type/number");
        }
        return f;
    }

    /** The enum counterpart of {@link #resolve}: same C++ scoping rules. */
    private static Enum resolveEnum(Map<String, Enum> byName, String scope,
            String name) {
        if (name.startsWith(".")) {
            return byName.get(name.substring(1));
        }
        String s = scope;
        while (true) {
            String cand = s.isEmpty() ? name : s + "." + name;
            Enum e = byName.get(cand);
            if (e != null) {
                return e;
            }
            int dot = name.indexOf('.');
            if (dot >= 0) {
                String head = name.substring(0, dot);
                String rest = name.substring(dot + 1);
                String prefix = s.isEmpty() ? head : s + "." + head;
                if (byName.containsKey(prefix)) {
                    return resolveEnum(byName, prefix, rest);
                }
            }
            if (s.isEmpty()) {
                return null;
            }
            int cut = s.lastIndexOf('.');
            s = cut < 0 ? "" : s.substring(0, cut);
        }
    }

    /** FieldOptions: packed (2) and the editions FeatureSet (21). */
    private static void parseFieldOptions(byte[] b, int off, int len, Field f) {
        Wire.Reader r = sub(b, off, len);
        while (r.pos < r.end) {
            int tag = (int) Wire.rVar(r);
            int num = tag >>> 3;
            if (num == 2 && (tag & 7) == Wire.WT_VARINT) {
                f.packed = Wire.rVar(r) != 0;
                f.hasPackedOption = true;
            } else if (num == 21 && (tag & 7) == Wire.WT_LEN) {
                int n = Wire.rLen(r);
                f.enumOpen = parseFeatureSetEnumType(r.b, r.pos, r.pos + n);
                f.utf8 = parseFeatureSetUtf8(r.b, r.pos, r.pos + n);
                r.pos += n;
            } else {
                Wire.skip(r, tag & 7);
            }
        }
    }

    private static Wire.Reader sub(byte[] b, int off, int len) {
        if (len < 0 || off < 0 || off + len > b.length) {
            throw new IllegalArgumentException("descriptor overrun");
        }
        Wire.Reader r = new Wire.Reader();
        r.b = b;
        r.pos = off;
        r.end = off + len;
        return r;
    }

    private static String str(byte[] b, int off, int len) {
        return new String(b, off, len, java.nio.charset.StandardCharsets.UTF_8);
    }
}

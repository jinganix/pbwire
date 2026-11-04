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
        int number;
        Label label = Label.OPTIONAL;
        Type type;
        String typeName; // ".pkg.Msg" / ".pkg.Enum", message + enum + group
        int oneofIndex = -1;
        boolean proto3Optional;
        boolean packed; // resolved: explicit option or the syntax default
        boolean hasPackedOption;
        boolean hasPresence; // resolved: explicit presence (not proto3 plain)
        Message messageType; // linked: message + group fields
        Oneof oneof; // linked
        boolean isExtension;
    }

    static final class Message {
        String name;
        String fullName;
        final List<Field> fields = new ArrayList<>();
        final List<Message> nested = new ArrayList<>();
        final List<Field> extensions = new ArrayList<>();
        final List<Oneof> oneofs = new ArrayList<>();
    }

    static final class File {
        String name;
        String pkg = "";
        String syntax = "proto2"; // absent means proto2
        final List<String> deps = new ArrayList<>();
        final List<Message> messages = new ArrayList<>();
        final List<Field> extensions = new ArrayList<>();
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
        for (File f : files) {
            for (Message m : f.messages) {
                index(m, f.pkg, byName);
            }
        }
        for (File f : files) {
            boolean proto3 = "proto3".equals(f.syntax);
            for (Message m : f.messages) {
                linkMessage(m, f.pkg, proto3, byName);
            }
            for (Field e : f.extensions) {
                linkField(e, f.pkg, proto3, byName, null);
            }
        }
    }

    private static void index(Message m, String scope, Map<String, Message> byName) {
        m.fullName = scope.isEmpty() ? m.name : scope + "." + m.name;
        if (byName.put(m.fullName, m) != null) {
            throw new IllegalArgumentException("duplicate message " + m.fullName);
        }
        for (Message n : m.nested) {
            index(n, m.fullName, byName);
        }
    }

    private static void linkMessage(Message m, String scope, boolean proto3,
            Map<String, Message> byName) {
        for (Field f : m.fields) {
            linkField(f, scope, proto3, byName, m);
        }
        for (Message n : m.nested) {
            linkMessage(n, m.fullName, proto3, byName);
        }
        for (Field e : m.extensions) {
            linkField(e, scope, proto3, byName, null);
        }
    }

    private static void linkField(Field f, String scope, boolean proto3,
            Map<String, Message> byName, Message parent) {
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
        if (f.label == Label.REPEATED && isPackable(f.type) && !f.hasPackedOption) {
            f.packed = proto3; // the syntax default
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
            switch (tag & 7) {
                case Wire.WT_LEN:
                    int n = Wire.rLen(r);
                    if (num == 4) {
                        f.messages.add(parseMessage(r.b, r.pos, n));
                    } else if (num == 7) {
                        f.extensions.add(parseField(r.b, r.pos, n, true));
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
                    break;
                default:
                    Wire.skip(r, tag & 7);
                    break;
            }
        }
        return f;
    }

    private static Message parseMessage(byte[] b, int off, int len) {
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
                m.nested.add(parseMessage(r.b, r.pos, n));
            } else if (num == 6) {
                m.extensions.add(parseField(r.b, r.pos, n, true));
            } else if (num == 8) {
                m.oneofs.add(parseOneof(r.b, r.pos, n));
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

    /** FieldOptions: only packed (2) matters to a binary codec. */
    private static void parseFieldOptions(byte[] b, int off, int len, Field f) {
        Wire.Reader r = sub(b, off, len);
        while (r.pos < r.end) {
            int tag = (int) Wire.rVar(r);
            if ((tag >>> 3) == 2 && (tag & 7) == Wire.WT_VARINT) {
                f.packed = Wire.rVar(r) != 0;
                f.hasPackedOption = true;
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

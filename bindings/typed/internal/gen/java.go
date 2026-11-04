package gen

import (
	"fmt"
	"strings"

	"pbwire-typed/internal/model"

	"google.golang.org/protobuf/reflect/protoreflect"
)

// javaType is the Java type of one element of a field.
func javaType(field model.Field, prefix string) string {
	switch field.Kind {
	case protoreflect.Int32Kind, protoreflect.Sint32Kind, protoreflect.Sfixed32Kind,
		protoreflect.Uint32Kind, protoreflect.Fixed32Kind, protoreflect.EnumKind:
		return "int"
	case protoreflect.Int64Kind, protoreflect.Sint64Kind, protoreflect.Sfixed64Kind,
		protoreflect.Uint64Kind, protoreflect.Fixed64Kind:
		return "long"
	case protoreflect.FloatKind:
		return "float"
	case protoreflect.DoubleKind:
		return "double"
	case protoreflect.BoolKind:
		return "boolean"
	case protoreflect.StringKind:
		return "String"
	case protoreflect.BytesKind:
		return "byte[]"
	default:
		return prefix + "_" + field.Message
	}
}

func javaBoxed(kind protoreflect.Kind, field model.Field, prefix string) string {
	switch kind {
	case protoreflect.Int32Kind, protoreflect.Sint32Kind, protoreflect.Sfixed32Kind,
		protoreflect.Uint32Kind, protoreflect.Fixed32Kind, protoreflect.EnumKind:
		return "Integer"
	case protoreflect.Int64Kind, protoreflect.Sint64Kind, protoreflect.Sfixed64Kind,
		protoreflect.Uint64Kind, protoreflect.Fixed64Kind:
		return "Long"
	case protoreflect.FloatKind:
		return "Float"
	case protoreflect.DoubleKind:
		return "Double"
	case protoreflect.BoolKind:
		return "Boolean"
	case protoreflect.StringKind:
		return "String"
	case protoreflect.BytesKind:
		return "byte[]"
	default:
		return prefix + "_" + field.Message
	}
}

// javaValue is the expression that reads one message value into the Java type.
func javaValue(field model.Field, expr string) string {
	switch field.Kind {
	case protoreflect.Int32Kind, protoreflect.Sint32Kind, protoreflect.Sfixed32Kind,
		protoreflect.Uint32Kind, protoreflect.Fixed32Kind, protoreflect.EnumKind:
		return fmt.Sprintf("(int) %s", expr)
	case protoreflect.FloatKind:
		return fmt.Sprintf("(float) %s", expr)
	default:
		return expr
	}
}

// javaFiles emits one public class per message and one constants class per
// enum. Java classes live in the default package so they compile next to the
// binding's runtime classes without an import.
func javaFiles(f *model.File) []File {
	var files []File
	for _, e := range f.Enums {
		var b strings.Builder
		b.WriteString(header(f, "Enum values are int constants: an unknown value from the wire round-trips."))
		fmt.Fprintf(&b, "public final class %s {\n", className(f, e.Name))
		fmt.Fprintf(&b, "    private %s() {}\n\n", className(f, e.Name))
		for _, v := range e.Values {
			fmt.Fprintf(&b, "    public static final int %s = %d;\n", v.Name, v.Number)
		}
		b.WriteString("}\n")
		files = append(files, File{Name: className(f, e.Name) + ".java", Content: b.String()})
	}
	for _, m := range f.Messages {
		var b strings.Builder
		cls := className(f, m.Name)
		b.WriteString(header(f, "Typed view of "+m.Full+": assign fields, encode, decode."))
		b.WriteString("import java.util.ArrayList;\nimport java.util.List;\n\n")
		fmt.Fprintf(&b, "public final class %s {\n", cls)
		for _, field := range m.Fields {
			if field.Skip != "" {
				fmt.Fprintf(&b, "    // skipped %s: %s\n", field.Name, field.Skip)
				continue
			}
			if field.NeedsHas {
				fmt.Fprintf(&b, "    /** presence flag for %s */\n    public boolean has%s;\n", field.Name, field.Pascal)
			}
			fmt.Fprintf(&b, "    public %s %s", javaFieldType(f, field), field.Lower)
			if field.Repeated {
				fmt.Fprintf(&b, " = new ArrayList<>()")
			}
			fmt.Fprintf(&b, "; // %s = %d\n", field.Name, field.Number)
		}
		writeJavaCodec(&b, f, m, cls)
		b.WriteString("}\n")
		files = append(files, File{Name: cls + ".java", Content: b.String()})
	}
	return files
}

func javaFieldType(f *model.File, field model.Field) string {
	if field.Repeated {
		return "List<" + javaBoxed(field.Kind, field, f.Prefix) + ">"
	}
	return javaType(field, f.Prefix)
}

func writeJavaCodec(b *strings.Builder, f *model.File, m model.Message, cls string) {
	fmt.Fprintf(b, "\n    public byte[] encode(Schema schema) {\n")
	fmt.Fprintf(b, "        Message msg = schema.message(%q).newMessage();\n", m.Full)
	fmt.Fprintf(b, "        encodeInto(msg);\n        return msg.encode();\n    }\n\n")

	fmt.Fprintf(b, "    public void encodeInto(Message msg) {\n")
	for _, field := range m.Fields {
		if field.Skip != "" {
			continue
		}
		cond := javaPresent(field)
		fmt.Fprintf(b, "        if (%s) {\n", cond)
		if field.Repeated {
			elem := javaType(field, f.Prefix)
			if field.Message != "" {
				fmt.Fprintf(b, "            for (%s v : %s) { v.encodeInto(msg.newMessage(%q)); }\n", elem, field.Lower, field.Name)
			} else {
				fmt.Fprintf(b, "            for (%s v : %s) { msg.set(%q, %s); }\n", elem, field.Lower, field.Name, javaSetArg(field, "v"))
			}
		} else if field.Message != "" {
			fmt.Fprintf(b, "            %s.encodeInto(msg.newMessage(%q));\n", field.Lower, field.Name)
		} else {
			fmt.Fprintf(b, "            msg.set(%q, %s);\n", field.Name, javaSetArg(field, field.Lower))
		}
		b.WriteString("        }\n")
	}
	b.WriteString("    }\n\n")

	fmt.Fprintf(b, "    public static %s decode(Schema schema, byte[] wire) {\n", cls)
	fmt.Fprintf(b, "        return decode(schema.message(%q).decode(wire));\n    }\n\n", m.Full)

	fmt.Fprintf(b, "    public static %s decode(Message msg) {\n", cls)
	fmt.Fprintf(b, "        %s out = new %s();\n", cls, cls)
	for _, field := range m.Fields {
		if field.Skip != "" {
			continue
		}
		writeJavaRead(b, f, field)
	}
	b.WriteString("        return out;\n    }\n")
}

func javaPresent(field model.Field) string {
	if field.Required {
		return "true"
	}
	switch {
	case field.Repeated:
		return "!" + field.Lower + ".isEmpty()"
	case field.Message != "":
		return field.Lower + " != null"
	case field.NeedsHas:
		if field.Kind == protoreflect.StringKind || field.Kind == protoreflect.BytesKind {
			return "has" + field.Pascal + " && " + field.Lower + " != null"
		}
		return "has" + field.Pascal
	case field.Kind == protoreflect.BoolKind:
		return field.Lower
	case field.Kind == protoreflect.StringKind:
		return field.Lower + " != null && !" + field.Lower + ".isEmpty()"
	case field.Kind == protoreflect.BytesKind:
		return field.Lower + " != null && " + field.Lower + ".length != 0"
	default:
		return field.Lower + " != 0"
	}
}

func javaSetArg(field model.Field, expr string) string {
	switch field.Kind {
	case protoreflect.Int32Kind, protoreflect.Sint32Kind, protoreflect.Sfixed32Kind,
		protoreflect.Uint32Kind, protoreflect.Fixed32Kind, protoreflect.EnumKind:
		return "(long) " + expr
	case protoreflect.FloatKind, protoreflect.DoubleKind:
		return "(double) " + expr
	default:
		return expr
	}
}

func writeJavaRead(b *strings.Builder, f *model.File, field model.Field) {
	name := field.Lower
	if field.Repeated {
		var conv string
		switch field.Kind {
		case protoreflect.StringKind:
			conv = "(String) o"
		case protoreflect.BytesKind:
			conv = "(byte[]) o"
		case protoreflect.BoolKind:
			conv = "(Boolean) o"
		case protoreflect.FloatKind:
			conv = "((Number) o).floatValue()"
		case protoreflect.DoubleKind:
			conv = "((Number) o).doubleValue()"
		case protoreflect.Int64Kind, protoreflect.Sint64Kind, protoreflect.Sfixed64Kind,
			protoreflect.Uint64Kind, protoreflect.Fixed64Kind:
			conv = "((Number) o).longValue()"
		default:
			conv = "((Number) o).intValue()"
		}
		if field.Message != "" {
			fmt.Fprintf(b, "        for (Object o : msg.getList(%q)) { out.%s.add(%s_%s.decode((Message) o)); }\n", field.Name, name, f.Prefix, field.Message)
		} else {
			fmt.Fprintf(b, "        for (Object o : msg.getList(%q)) { out.%s.add(%s); }\n", field.Name, name, conv)
		}
		return
	}
	if field.Message != "" {
		fmt.Fprintf(b, "        { Message child = msg.getMessage(%q); out.%s = child == null ? null : %s_%s.decode(child); }\n", field.Name, name, f.Prefix, field.Message)
		return
	}
	read := fmt.Sprintf("msg.getInt(%q)", field.Name)
	switch field.Kind {
	case protoreflect.Int32Kind, protoreflect.Sint32Kind, protoreflect.Sfixed32Kind,
		protoreflect.Uint32Kind, protoreflect.Fixed32Kind, protoreflect.EnumKind:
		read = fmt.Sprintf("msg.getInt(%q)", field.Name)
	case protoreflect.Int64Kind, protoreflect.Sint64Kind, protoreflect.Sfixed64Kind,
		protoreflect.Uint64Kind, protoreflect.Fixed64Kind:
		read = fmt.Sprintf("msg.getLong(%q)", field.Name)
	case protoreflect.FloatKind:
		read = fmt.Sprintf("msg.getFloat(%q)", field.Name)
	case protoreflect.DoubleKind:
		read = fmt.Sprintf("msg.getDouble(%q)", field.Name)
	case protoreflect.BoolKind:
		read = fmt.Sprintf("msg.getBool(%q)", field.Name)
	case protoreflect.StringKind:
		read = fmt.Sprintf("msg.getString(%q)", field.Name)
	case protoreflect.BytesKind:
		read = fmt.Sprintf("msg.getBytes(%q)", field.Name)
	}
	if field.NeedsHas {
		fmt.Fprintf(b, "        out.has%s = msg.has(%q);\n", field.Pascal, field.Name)
		fmt.Fprintf(b, "        if (out.has%s) { out.%s = %s; }\n", field.Pascal, name, read)
		return
	}
	fmt.Fprintf(b, "        out.%s = %s;\n", name, read)
}

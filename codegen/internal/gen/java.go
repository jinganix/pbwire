package gen

import (
	"fmt"
	"strings"

	"pbwire-codegen/internal/model"

	"google.golang.org/protobuf/reflect/protoreflect"
)

// javaFiles emits protobuf-java-shaped immutable messages over the pbwire
// dynamic API: one public class per message with a nested Builder
// (newBuilder/setX/addX/clear/build), toByteArray/parseFrom,
// hasX/getX accessors, unmodifiable List getters for repeated fields, a
// KindCase enum for oneofs and Java enums for enum fields. Every codec call
// goes through the binding's existing dynamic API, so the wire format stays
// in the C core. The FileDescriptorSet travels embedded (no .desc file at
// use time) in a per-proto holder class. No protobuf-java dependency: the
// shape mirrors its method names, not its runtime.
func javaFiles(f *model.File, opt Options) []File {
	files := []File{javaHolder(f, opt)}
	for _, e := range f.Enums {
		files = append(files, javaEnumFile(f, e))
	}
	for _, m := range f.Messages {
		files = append(files, javaMessageFile(f, m))
	}
	return files
}

// javaHolder emits the per-proto schema holder: the embedded descriptor set
// plus a type lookup. One holder per .proto keeps the big descriptor bytes
// out of every message class.
func javaHolder(f *model.File, opt Options) File {
	var b strings.Builder
	b.WriteString(header(f, "Embedded FileDescriptorSet plus the type lookup the generated models decode through."))
	b.WriteString("import java.util.Base64;\n\n")
	fmt.Fprintf(&b, "public final class %s_Pbwire {\n", f.Prefix)
	fmt.Fprintf(&b, "    private %s_Pbwire() {}\n\n", f.Prefix)
	b.WriteString("    private static final String[] DESC_B64 = {\n")
	for _, chunk := range chunk64(opt.DescSetB64) {
		fmt.Fprintf(&b, "        %q,\n", chunk)
	}
	b.WriteString("    };\n\n")
	fmt.Fprintf(&b, "    public static final Schema SCHEMA = load();\n\n")
	b.WriteString("    private static Schema load() {\n")
	b.WriteString("        StringBuilder sb = new StringBuilder();\n")
	b.WriteString("        for (String chunk : DESC_B64) {\n")
	b.WriteString("            sb.append(chunk);\n")
	b.WriteString("        }\n")
	b.WriteString("        return Schema.fromBytes(Base64.getDecoder().decode(sb.toString()));\n")
	b.WriteString("    }\n\n")
	b.WriteString("    public static MessageType type(String fullName) {\n")
	b.WriteString("        return SCHEMA.message(fullName);\n")
	b.WriteString("    }\n")
	b.WriteString("}\n")
	return File{Name: fmt.Sprintf("%s_Pbwire.java", f.Prefix), Content: b.String()}
}

// javaEnumFile emits one Java enum per proto enum, protobuf-java naming:
// SCREAMING constants, UNRECOGNIZED for unknown wire values, getNumber,
// forNumber (null when unknown) and _VALUE int constants.
func javaEnumFile(f *model.File, e model.Enum) File {
	var b strings.Builder
	b.WriteString(header(f, "Generated enum "+e.Full+": unknown wire values read as UNRECOGNIZED."))
	fmt.Fprintf(&b, "public enum %s {\n", className(f, e.Name))
	for _, v := range e.Values {
		fmt.Fprintf(&b, "    %s(%d),\n", v.Name, v.Number)
	}
	b.WriteString("    UNRECOGNIZED(-1);\n\n")
	for _, v := range e.Values {
		fmt.Fprintf(&b, "    public static final int %s_VALUE = %d;\n", v.Name, v.Number)
	}
	b.WriteString("\n    private final int value;\n\n")
	fmt.Fprintf(&b, "    private %s(int value) {\n", className(f, e.Name))
	b.WriteString("        this.value = value;\n    }\n\n")
	b.WriteString("    public int getNumber() {\n")
	b.WriteString("        if (this == UNRECOGNIZED) {\n")
	b.WriteString("            throw new IllegalArgumentException(\"unknown enum value\");\n")
	b.WriteString("        }\n")
	b.WriteString("        return value;\n    }\n\n")
	fmt.Fprintf(&b, "    public static %s forNumber(int value) {\n", className(f, e.Name))
	b.WriteString("        switch (value) {\n")
	seen := map[int32]bool{}
	for _, v := range e.Values {
		if seen[v.Number] {
			continue // allow_alias: first name wins, like protoc
		}
		seen[v.Number] = true
		fmt.Fprintf(&b, "            case %d: return %s;\n", v.Number, v.Name)
	}
	b.WriteString("            default: return null;\n")
	b.WriteString("        }\n    }\n")
	b.WriteString("}\n")
	return File{Name: className(f, e.Name) + ".java", Content: b.String()}
}

// javaType is the Java type of one singular field value.
func javaType(f *model.File, field model.Field) string {
	switch field.Kind {
	case protoreflect.Int32Kind, protoreflect.Sint32Kind, protoreflect.Sfixed32Kind,
		protoreflect.Uint32Kind, protoreflect.Fixed32Kind:
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
	case protoreflect.EnumKind:
		if field.Enum != "" {
			return className(f, field.Enum)
		}
		return "int" // external enum: keep the wire number
	default:
		return className(f, field.Message)
	}
}

// javaBoxed is the element type of a repeated field's List.
func javaBoxed(f *model.File, field model.Field) string {
	switch field.Kind {
	case protoreflect.Int32Kind, protoreflect.Sint32Kind, protoreflect.Sfixed32Kind,
		protoreflect.Uint32Kind, protoreflect.Fixed32Kind:
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
	case protoreflect.EnumKind:
		if field.Enum != "" {
			return className(f, field.Enum)
		}
		return "Integer"
	default:
		return className(f, field.Message)
	}
}

// javaZero is the field default: protobuf-java returns these for absent fields.
func javaZero(f *model.File, field model.Field) string {
	switch field.Kind {
	case protoreflect.FloatKind:
		return "0f"
	case protoreflect.DoubleKind:
		return "0.0"
	case protoreflect.BoolKind:
		return "false"
	case protoreflect.StringKind:
		return `""`
	case protoreflect.BytesKind:
		return "new byte[0]"
	default:
		return "0"
	}
}

// javaOneofs returns the distinct oneof names of a message in field order.
func javaOneofs(m model.Message) []string {
	var out []string
	seen := map[string]bool{}
	for _, field := range m.Fields {
		if field.Skip != "" || field.Oneof == "" || seen[field.Oneof] {
			continue
		}
		seen[field.Oneof] = true
		out = append(out, field.Oneof)
	}
	return out
}

// javaMessageFile emits one immutable message class with its Builder.
func javaMessageFile(f *model.File, m model.Message) File {
	cls := className(f, m.Name)
	var b strings.Builder
	b.WriteString(header(f, "Generated message "+m.Full+" over the pbwire dynamic API."))
	b.WriteString("import java.util.ArrayList;\nimport java.util.Collections;\nimport java.util.List;\nimport java.util.Objects;\n\n")
	fmt.Fprintf(&b, "public final class %s {\n", cls)
	fmt.Fprintf(&b, "    public static final %s DEFAULT_INSTANCE = new %s(new Builder());\n\n", cls, cls)
	oneofs := javaOneofs(m)

	for _, field := range m.Fields {
		if field.Skip != "" {
			fmt.Fprintf(&b, "    // skipped %s: %s\n", field.Name, field.Skip)
			continue
		}
		if field.Oneof != "" {
			continue
		}
		writeJavaField(&b, f, field)
	}
	for _, o := range oneofs {
		fmt.Fprintf(&b, "    private final int %sCase_;\n", model.LowerCamel(o))
		fmt.Fprintf(&b, "    private final Object %s_;\n", model.LowerCamel(o))
	}

	fmt.Fprintf(&b, "\n    private %s(Builder b) {\n", cls)
	for _, field := range m.Fields {
		if field.Skip != "" || field.Oneof != "" {
			continue
		}
		writeJavaCtor(&b, f, field)
	}
	for _, o := range oneofs {
		lower := model.LowerCamel(o)
		fmt.Fprintf(&b, "        %sCase_ = b.%sCase_;\n", lower, lower)
		fmt.Fprintf(&b, "        %s_ = b.%s_;\n", lower, lower)
	}
	b.WriteString("    }\n\n")

	fmt.Fprintf(&b, "    public static Builder newBuilder() {\n        return new Builder();\n    }\n\n")
	fmt.Fprintf(&b, "    public static %s getDefaultInstance() {\n        return DEFAULT_INSTANCE;\n    }\n\n", cls)

	for _, field := range m.Fields {
		if field.Skip != "" || field.Oneof != "" {
			continue
		}
		writeJavaGetter(&b, f, field)
	}
	for _, o := range oneofs {
		writeJavaOneofGetter(&b, f, m, o)
	}

	writeJavaWire(&b, f, m, cls, oneofs)
	writeJavaBuilder(&b, f, m, cls, oneofs)
	for _, o := range oneofs {
		writeJavaKindCase(&b, m, o)
	}
	b.WriteString("}\n")
	return File{Name: cls + ".java", Content: b.String()}
}

// writeJavaField emits the immutable message's private storage.
func writeJavaField(b *strings.Builder, f *model.File, field model.Field) {
	priv := field.Lower + "_"
	if field.Repeated {
		if field.Kind == protoreflect.BytesKind {
			fmt.Fprintf(b, "    private final List<byte[]> %s; // %s = %d\n", priv, field.Name, field.Number)
		} else if field.Kind == protoreflect.EnumKind && field.Enum != "" {
			fmt.Fprintf(b, "    private final List<Integer> %s; // raw values; %s = %d\n", priv, field.Name, field.Number)
		} else {
			fmt.Fprintf(b, "    private final List<%s> %s; // %s = %d\n", javaBoxed(f, field), priv, field.Name, field.Number)
		}
		return
	}
	if field.Message != "" {
		fmt.Fprintf(b, "    private final %s %s; // null when absent; %s = %d\n", javaType(f, field), priv, field.Name, field.Number)
		return
	}
	if field.Kind == protoreflect.EnumKind && field.Enum != "" {
		if field.NeedsHas || field.Required {
			fmt.Fprintf(b, "    private final boolean has%s;\n", field.Pascal)
		}
		fmt.Fprintf(b, "    private final int %s; // raw value; %s = %d\n", priv, field.Name, field.Number)
		return
	}
	if field.NeedsHas || field.Required {
		fmt.Fprintf(b, "    private final boolean has%s;\n", field.Pascal)
	}
	fmt.Fprintf(b, "    private final %s %s; // %s = %d\n", javaType(f, field), priv, field.Name, field.Number)
}

// writeJavaCtor copies one Builder field into the immutable message.
func writeJavaCtor(b *strings.Builder, f *model.File, field model.Field) {
	priv := field.Lower + "_"
	if field.Repeated {
		if field.Kind == protoreflect.BytesKind {
			fmt.Fprintf(b, "        {\n            List<byte[]> tmp = new ArrayList<>(b.%s.size());\n", priv)
			fmt.Fprintf(b, "            for (byte[] e : b.%s) {\n                tmp.add(e.clone());\n            }\n", priv)
			fmt.Fprintf(b, "            %s = Collections.unmodifiableList(tmp);\n        }\n", priv)
		} else {
			fmt.Fprintf(b, "        %s = Collections.unmodifiableList(new ArrayList<>(b.%s));\n", priv, priv)
		}
		return
	}
	if field.Message != "" {
		fmt.Fprintf(b, "        %s = b.%s;\n", priv, priv)
		return
	}
	if field.Kind == protoreflect.StringKind {
		if field.NeedsHas || field.Required {
			fmt.Fprintf(b, "        has%s = b.has%s;\n", field.Pascal, field.Pascal)
			fmt.Fprintf(b, "        %s = b.%s == null ? \"\" : b.%s;\n", priv, priv, priv)
		} else {
			fmt.Fprintf(b, "        %s = b.%s == null ? \"\" : b.%s;\n", priv, priv, priv)
		}
		return
	}
	if field.Kind == protoreflect.BytesKind {
		if field.NeedsHas || field.Required {
			fmt.Fprintf(b, "        has%s = b.has%s;\n", field.Pascal, field.Pascal)
		}
		fmt.Fprintf(b, "        %s = b.%s == null ? new byte[0] : b.%s.clone();\n", priv, priv, priv)
		return
	}
	if field.Kind == protoreflect.EnumKind && field.Enum != "" {
		if field.NeedsHas || field.Required {
			fmt.Fprintf(b, "        has%s = b.has%s;\n", field.Pascal, field.Pascal)
		}
		fmt.Fprintf(b, "        %s = b.%s;\n", priv, priv)
		return
	}
	if field.NeedsHas || field.Required {
		fmt.Fprintf(b, "        has%s = b.has%s;\n", field.Pascal, field.Pascal)
	}
	fmt.Fprintf(b, "        %s = b.%s;\n", priv, priv)
}

// writeJavaGetter emits has/get (plus List/Count/indexed for repeated).
func writeJavaGetter(b *strings.Builder, f *model.File, field model.Field) {
	p, priv := field.Pascal, field.Lower+"_"
	if field.Repeated {
		writeJavaRepeatedGetter(b, f, field, p, priv)
		return
	}
	if field.Message != "" {
		t := javaType(f, field)
		fmt.Fprintf(b, "    public boolean has%s() {\n        return %s != null;\n    }\n\n", p, priv)
		fmt.Fprintf(b, "    public %s get%s() {\n        return %s == null ? %s.getDefaultInstance() : %s;\n    }\n\n",
			t, p, priv, t, priv)
		return
	}
	if field.Kind == protoreflect.EnumKind && field.Enum != "" {
		t := javaType(f, field)
		if field.NeedsHas || field.Required {
			fmt.Fprintf(b, "    public boolean has%s() {\n        return has%s;\n    }\n\n", p, p)
		}
		fmt.Fprintf(b, "    public int get%sValue() {\n        return %s;\n    }\n\n", p, priv)
		fmt.Fprintf(b, "    public %s get%s() {\n", t, p)
		fmt.Fprintf(b, "        %s v = %s.forNumber(%s);\n", t, t, priv)
		fmt.Fprintf(b, "        return v == null ? %s.UNRECOGNIZED : v;\n    }\n\n", t)
		return
	}
	t := javaType(f, field)
	if field.NeedsHas || field.Required {
		fmt.Fprintf(b, "    public boolean has%s() {\n        return has%s;\n    }\n\n", p, p)
	}
	switch field.Kind {
	case protoreflect.StringKind:
		fmt.Fprintf(b, "    public String get%s() {\n        return %s;\n    }\n\n", p, priv)
	case protoreflect.BytesKind:
		fmt.Fprintf(b, "    public byte[] get%s() {\n        return %s.clone();\n    }\n\n", p, priv)
	default:
		fmt.Fprintf(b, "    public %s get%s() {\n        return %s;\n    }\n\n", t, p, priv)
	}
}

// writeJavaRepeatedGetter emits getXList/getXCount/getX(i), plus the
// getXValueList/getXValue(i) pair for enums.
func writeJavaRepeatedGetter(b *strings.Builder, f *model.File, field model.Field, p, priv string) {
	if field.Kind == protoreflect.BytesKind {
		fmt.Fprintf(b, "    public List<byte[]> get%sList() {\n", p)
		fmt.Fprintf(b, "        List<byte[]> out = new ArrayList<>(%s.size());\n", priv)
		fmt.Fprintf(b, "        for (byte[] e : %s) {\n            out.add(e.clone());\n        }\n", priv)
		fmt.Fprintf(b, "        return Collections.unmodifiableList(out);\n    }\n\n")
		fmt.Fprintf(b, "    public int get%sCount() {\n        return %s.size();\n    }\n\n", p, priv)
		fmt.Fprintf(b, "    public byte[] get%s(int index) {\n        return %s.get(index).clone();\n    }\n\n", p, priv)
		return
	}
	if field.Kind == protoreflect.EnumKind && field.Enum != "" {
		t := javaType(f, field)
		fmt.Fprintf(b, "    public List<%s> get%sList() {\n", t, p)
		fmt.Fprintf(b, "        List<%s> out = new ArrayList<>(%s.size());\n", t, priv)
		fmt.Fprintf(b, "        for (int v : %s) {\n", priv)
		fmt.Fprintf(b, "            %s e = %s.forNumber(v);\n", t, t)
		fmt.Fprintf(b, "            out.add(e == null ? %s.UNRECOGNIZED : e);\n", t)
		fmt.Fprintf(b, "        }\n        return Collections.unmodifiableList(out);\n    }\n\n")
		fmt.Fprintf(b, "    public List<Integer> get%sValueList() {\n        return Collections.unmodifiableList(%s);\n    }\n\n", p, priv)
		fmt.Fprintf(b, "    public int get%sCount() {\n        return %s.size();\n    }\n\n", p, priv)
		fmt.Fprintf(b, "    public %s get%s(int index) {\n", t, p)
		fmt.Fprintf(b, "        %s e = %s.forNumber(%s.get(index));\n", t, t, priv)
		fmt.Fprintf(b, "        return e == null ? %s.UNRECOGNIZED : e;\n    }\n\n", t)
		fmt.Fprintf(b, "    public int get%sValue(int index) {\n        return %s.get(index);\n    }\n\n", p, priv)
		return
	}
	t := javaBoxed(f, field)
	fmt.Fprintf(b, "    public List<%s> get%sList() {\n        return Collections.unmodifiableList(%s);\n    }\n\n", t, p, priv)
	fmt.Fprintf(b, "    public int get%sCount() {\n        return %s.size();\n    }\n\n", p, priv)
	fmt.Fprintf(b, "    public %s get%s(int index) {\n        return %s.get(index);\n    }\n\n", unbox(t), p, priv)
}

// unbox renders the primitive for an indexed getter (Integer -> int).
func unbox(t string) string {
	switch t {
	case "Integer":
		return "int"
	case "Long":
		return "long"
	case "Float":
		return "float"
	case "Double":
		return "double"
	case "Boolean":
		return "boolean"
	default:
		return t
	}
}

// writeJavaOneofGetter emits getKindCase plus has/get per member.
func writeJavaOneofGetter(b *strings.Builder, f *model.File, m model.Message, o string) {
	pascal, lower := model.Pascal(o), model.LowerCamel(o)
	fmt.Fprintf(b, "    public %sCase get%sCase() {\n", pascal, pascal)
	fmt.Fprintf(b, "        return %sCase.forNumber(%sCase_);\n    }\n\n", pascal, lower)
	for _, field := range m.Fields {
		if field.Skip != "" || field.Oneof != o {
			continue
		}
		p := field.Pascal
		num := field.Number
		if field.Message != "" {
			t := javaType(f, field)
			fmt.Fprintf(b, "    public boolean has%s() {\n        return %sCase_ == %d;\n    }\n\n", p, lower, num)
			fmt.Fprintf(b, "    public %s get%s() {\n", t, p)
			fmt.Fprintf(b, "        return %sCase_ == %d ? (%s) %s_ : %s.getDefaultInstance();\n    }\n\n",
				lower, num, t, lower, t)
			continue
		}
		if field.Kind == protoreflect.EnumKind && field.Enum != "" {
			t := javaType(f, field)
			fmt.Fprintf(b, "    public boolean has%s() {\n        return %sCase_ == %d;\n    }\n\n", p, lower, num)
			fmt.Fprintf(b, "    public int get%sValue() {\n", p)
			fmt.Fprintf(b, "        return %sCase_ == %d ? (Integer) %s_ : 0;\n    }\n\n", lower, num, lower)
			fmt.Fprintf(b, "    public %s get%s() {\n", t, p)
			fmt.Fprintf(b, "        if (%sCase_ != %d) {\n            return %s.UNRECOGNIZED;\n        }\n", lower, num, t)
			fmt.Fprintf(b, "        %s e = %s.forNumber((Integer) %s_);\n", t, t, lower)
			fmt.Fprintf(b, "        return e == null ? %s.UNRECOGNIZED : e;\n    }\n\n", t)
			continue
		}
		t := javaType(f, field)
		fmt.Fprintf(b, "    public boolean has%s() {\n        return %sCase_ == %d;\n    }\n\n", p, lower, num)
		switch field.Kind {
		case protoreflect.StringKind:
			fmt.Fprintf(b, "    public String get%s() {\n", p)
			fmt.Fprintf(b, "        return %sCase_ == %d ? (String) %s_ : \"\";\n    }\n\n", lower, num, lower)
		case protoreflect.BytesKind:
			fmt.Fprintf(b, "    public byte[] get%s() {\n", p)
			fmt.Fprintf(b, "        return %sCase_ == %d ? ((byte[]) %s_).clone() : new byte[0];\n    }\n\n", lower, num, lower)
		case protoreflect.FloatKind:
			fmt.Fprintf(b, "    public float get%s() {\n", p)
			fmt.Fprintf(b, "        return %sCase_ == %d ? (Float) %s_ : 0f;\n    }\n\n", lower, num, lower)
		case protoreflect.DoubleKind:
			fmt.Fprintf(b, "    public double get%s() {\n", p)
			fmt.Fprintf(b, "        return %sCase_ == %d ? (Double) %s_ : 0.0;\n    }\n\n", lower, num, lower)
		case protoreflect.BoolKind:
			fmt.Fprintf(b, "    public boolean get%s() {\n", p)
			fmt.Fprintf(b, "        return %sCase_ == %d ? (Boolean) %s_ : false;\n    }\n\n", lower, num, lower)
		default:
			fmt.Fprintf(b, "    public %s get%s() {\n", t, p)
			if t == "long" {
				fmt.Fprintf(b, "        return %sCase_ == %d ? (Long) %s_ : 0L;\n    }\n\n", lower, num, lower)
			} else {
				fmt.Fprintf(b, "        return %sCase_ == %d ? (Integer) %s_ : 0;\n    }\n\n", lower, num, lower)
			}
		}
	}
}

// writeJavaWire emits toByteArray/parseFrom/encodeInto/decode/isInitialized.
func writeJavaWire(b *strings.Builder, f *model.File, m model.Message, cls string, oneofs []string) {
	fmt.Fprintf(b, "    public byte[] toByteArray() {\n")
	fmt.Fprintf(b, "        Message msg = %s_Pbwire.type(%q).newMessage();\n", f.Prefix, m.Full)
	b.WriteString("        try {\n")
	b.WriteString("            encodeInto(msg);\n")
	b.WriteString("            return msg.encode();\n")
	b.WriteString("        } finally {\n")
	b.WriteString("            msg.close();\n")
	b.WriteString("        }\n    }\n\n")

	fmt.Fprintf(b, "    public static %s parseFrom(byte[] data) {\n", cls)
	fmt.Fprintf(b, "        Message msg = %s_Pbwire.type(%q).decode(data);\n", f.Prefix, m.Full)
	b.WriteString("        try {\n")
	b.WriteString("            return decode(msg);\n")
	b.WriteString("        } finally {\n")
	b.WriteString("            msg.close();\n")
	b.WriteString("        }\n    }\n\n")

	fmt.Fprintf(b, "    public void encodeInto(Message msg) {\n")
	for _, field := range m.Fields {
		if field.Skip != "" || field.Oneof != "" {
			continue
		}
		writeJavaEncodeField(b, f, field)
	}
	for _, o := range oneofs {
		lower := model.LowerCamel(o)
		fmt.Fprintf(b, "        switch (%sCase_) {\n", lower)
		for _, field := range m.Fields {
			if field.Skip != "" || field.Oneof != o {
				continue
			}
			fmt.Fprintf(b, "            case %d:\n", field.Number)
			writeJavaEncodeOne(b, f, field, lower)
			b.WriteString("                break;\n")
		}
		b.WriteString("            default:\n                break;\n        }\n")
	}
	b.WriteString("    }\n\n")

	fmt.Fprintf(b, "    public static %s decode(Message msg) {\n", cls)
	b.WriteString("        Builder b = new Builder();\n")
	for _, field := range m.Fields {
		if field.Skip != "" {
			continue
		}
		writeJavaLoadField(b, f, field)
	}
	b.WriteString("        return b.build();\n    }\n\n")

	b.WriteString("    public boolean isInitialized() {\n")
	need := false
	for _, field := range m.Fields {
		if field.Skip != "" {
			continue
		}
		if field.Required && field.Message == "" {
			fmt.Fprintf(b, "        if (!has%s) {\n            return false;\n        }\n", field.Pascal)
			need = true
		} else if field.Required {
			fmt.Fprintf(b, "        if (%s_ == null) {\n            return false;\n        }\n", field.Lower)
			need = true
		}
		if field.Message != "" && !field.Repeated && field.Oneof == "" {
			fmt.Fprintf(b, "        if (%s_ != null && !%s_.isInitialized()) {\n            return false;\n        }\n", field.Lower, field.Lower)
			need = true
		}
		if field.Message != "" && field.Repeated {
			fmt.Fprintf(b, "        for (%s v : %s_) {\n", javaType(f, field), field.Lower)
			b.WriteString("            if (!v.isInitialized()) {\n                return false;\n            }\n        }\n")
			need = true
		}
	}
	for _, o := range oneofs {
		lower := model.LowerCamel(o)
		for _, field := range m.Fields {
			if field.Skip != "" || field.Oneof != o || field.Message == "" {
				continue
			}
			fmt.Fprintf(b, "        if (%sCase_ == %d && !((%s) %s_).isInitialized()) {\n", lower, field.Number, javaType(f, field), lower)
			b.WriteString("            return false;\n        }\n")
			need = true
		}
	}
	if !need {
		b.WriteString("        return true;\n")
	} else {
		b.WriteString("        return true;\n")
	}
	b.WriteString("    }\n\n")
}

// writeJavaEncodeField emits one non-oneof field of encodeInto.
func writeJavaEncodeField(b *strings.Builder, f *model.File, field model.Field) {
	cond := javaEncodeCond(f, field)
	if cond != "" {
		fmt.Fprintf(b, "        if (%s) {\n", cond)
		writeJavaEncodeOne(b, f, field, "")
		b.WriteString("        }\n")
		return
	}
	// Required: always written; presence was validated by build().
	fmt.Fprintf(b, "        // required %s\n", field.Name)
	writeJavaEncodeOneIndent(b, f, field, "", "        ")
}

// javaEncodeCond is the implicit/explicit presence gate; "" means always.
func javaEncodeCond(f *model.File, field model.Field) string {
	priv := field.Lower + "_"
	if field.Required {
		return ""
	}
	if field.Repeated {
		return "!" + priv + ".isEmpty()"
	}
	if field.Message != "" {
		return priv + " != null"
	}
	if field.NeedsHas {
		// Explicit presence (optional, required): the has-bit decides, even
		// for an empty string or bytes payload. Implicit fields below stay
		// off the wire at their zero value, like protoc.
		return "has" + field.Pascal
	}
	if field.Kind == protoreflect.StringKind {
		return "!" + priv + ".isEmpty()"
	}
	if field.Kind == protoreflect.BytesKind {
		return priv + ".length != 0"
	}
	if field.Kind == protoreflect.BoolKind {
		return priv
	}
	if field.Kind == protoreflect.FloatKind {
		return priv + " != 0f"
	}
	if field.Kind == protoreflect.DoubleKind {
		return priv + " != 0.0"
	}
	return priv + " != 0"
}

// writeJavaEncodeOne emits the set/newMessage call for a present field.
// oneofLower is the oneof payload holder for oneof members, "" otherwise.
func writeJavaEncodeOne(b *strings.Builder, f *model.File, field model.Field, oneofLower string) {
	writeJavaEncodeOneIndent(b, f, field, oneofLower, "            ")
}

// writeJavaEncodeOneIndent is writeJavaEncodeOne with a chosen indent.
func writeJavaEncodeOneIndent(b *strings.Builder, f *model.File, field model.Field, oneofLower, indent string) {
	key := fmt.Sprintf("%q", field.Name)
	src := field.Lower + "_"
	if oneofLower != "" {
		src = oneofLower + "_"
	}
	switch {
	case field.Message != "" && !field.Repeated:
		if oneofLower != "" {
			fmt.Fprintf(b, "%s((%s) %s).encodeInto(msg.newMessage(%s));\n", indent, javaType(f, field), src, key)
		} else {
			fmt.Fprintf(b, "%s%s.encodeInto(msg.newMessage(%s));\n", indent, src, key)
		}
	case field.Message != "" && field.Repeated:
		fmt.Fprintf(b, "%sfor (%s v : %s) {\n", indent, javaType(f, field), src)
		fmt.Fprintf(b, "%s    v.encodeInto(msg.newMessage(%s));\n", indent, key)
		fmt.Fprintf(b, "%s}\n", indent)
	case field.Repeated:
		elem := unbox(javaBoxed(f, field))
		if field.Kind == protoreflect.EnumKind && field.Enum != "" {
			elem = "int" // enums ride raw; getters map to the enum type
		}
		if field.Kind == protoreflect.BytesKind {
			fmt.Fprintf(b, "%sfor (byte[] v : %s) {\n", indent, src)
		} else {
			fmt.Fprintf(b, "%sfor (%s v : %s) {\n", indent, elem, src)
		}
		fmt.Fprintf(b, "%s    msg.set(%s, %s);\n", indent, key, javaEncodeArg(f, field, "v"))
		fmt.Fprintf(b, "%s}\n", indent)
	default:
		fmt.Fprintf(b, "%smsg.set(%s, %s);\n", indent, key, javaEncodeArg(f, field, src))
	}
}

// javaEncodeArg renders the dynamic-set value: integers ride as long, floats
// narrow through double, oneof payloads unbox from Object.
func javaEncodeArg(f *model.File, field model.Field, expr string) string {
	if field.Oneof != "" && field.Message == "" {
		switch field.Kind {
		case protoreflect.FloatKind:
			return "(double) (Float) " + expr
		case protoreflect.DoubleKind:
			return "(Double) " + expr
		case protoreflect.BoolKind:
			return "(Boolean) " + expr
		case protoreflect.StringKind:
			return "(String) " + expr
		case protoreflect.BytesKind:
			return "(byte[]) " + expr
		default:
			return "(long) (Integer) " + expr
		}
	}
	switch field.Kind {
	case protoreflect.Int32Kind, protoreflect.Sint32Kind, protoreflect.Sfixed32Kind,
		protoreflect.Uint32Kind, protoreflect.Fixed32Kind, protoreflect.EnumKind,
		protoreflect.Int64Kind, protoreflect.Sint64Kind, protoreflect.Sfixed64Kind,
		protoreflect.Uint64Kind, protoreflect.Fixed64Kind:
		return "(long) " + expr
	case protoreflect.FloatKind, protoreflect.DoubleKind:
		return "(double) " + expr
	default:
		return expr
	}
}

// writeJavaLoadField emits one field of decode(Message).
func writeJavaLoadField(b *strings.Builder, f *model.File, field model.Field) {
	priv := "b." + field.Lower + "_"
	key := fmt.Sprintf("%q", field.Name)
	if field.Repeated {
		if field.Message != "" {
			fmt.Fprintf(b, "        for (Object o : msg.getList(%s)) {\n", key)
			fmt.Fprintf(b, "            %s.add(%s.decode((Message) o));\n", priv, javaType(f, field))
			b.WriteString("        }\n")
			return
		}
		var conv string
		switch field.Kind {
		case protoreflect.StringKind:
			conv = "(String) o"
		case protoreflect.BytesKind:
			conv = "((byte[]) o).clone()"
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
		fmt.Fprintf(b, "        for (Object o : msg.getList(%s)) {\n", key)
		fmt.Fprintf(b, "            %s.add(%s);\n", priv, conv)
		b.WriteString("        }\n")
		return
	}
	if field.Message != "" {
		if field.Oneof != "" {
			fmt.Fprintf(b, "        if (msg.has(%s)) {\n", key)
			fmt.Fprintf(b, "            b.%sCase_ = %d;\n", model.LowerCamel(field.Oneof), field.Number)
			fmt.Fprintf(b, "            b.%s_ = %s.decode(msg.getMessage(%s));\n", model.LowerCamel(field.Oneof), javaType(f, field), key)
			b.WriteString("        }\n")
			return
		}
		fmt.Fprintf(b, "        { Message child = msg.getMessage(%s); if (child != null) { %s = %s.decode(child); } }\n",
			key, priv, javaType(f, field))
		return
	}
	if field.Oneof != "" {
		fmt.Fprintf(b, "        if (msg.has(%s)) {\n", key)
		fmt.Fprintf(b, "            b.%sCase_ = %d;\n", model.LowerCamel(field.Oneof), field.Number)
		fmt.Fprintf(b, "            b.%s_ = %s;\n", model.LowerCamel(field.Oneof), javaLoadBox(field, key))
		b.WriteString("        }\n")
		return
	}
	read := javaRead(f, field, key)
	if field.NeedsHas || field.Required {
		fmt.Fprintf(b, "        if (msg.has(%s)) {\n", key)
		fmt.Fprintf(b, "            b.has%s = true;\n", field.Pascal)
		fmt.Fprintf(b, "            %s = %s;\n", priv, read)
		b.WriteString("        }\n")
		return
	}
	fmt.Fprintf(b, "        %s = %s;\n", priv, javaReadDefault(f, field, key))
}

// javaRead renders the typed getter call for a present field.
func javaRead(f *model.File, field model.Field, key string) string {
	switch field.Kind {
	case protoreflect.Int32Kind, protoreflect.Sint32Kind, protoreflect.Sfixed32Kind,
		protoreflect.Uint32Kind, protoreflect.Fixed32Kind, protoreflect.EnumKind:
		return fmt.Sprintf("msg.getInt(%s)", key)
	case protoreflect.Int64Kind, protoreflect.Sint64Kind, protoreflect.Sfixed64Kind,
		protoreflect.Uint64Kind, protoreflect.Fixed64Kind:
		return fmt.Sprintf("msg.getLong(%s)", key)
	case protoreflect.FloatKind:
		return fmt.Sprintf("msg.getFloat(%s)", key)
	case protoreflect.DoubleKind:
		return fmt.Sprintf("msg.getDouble(%s)", key)
	case protoreflect.BoolKind:
		return fmt.Sprintf("msg.getBool(%s)", key)
	case protoreflect.StringKind:
		return fmt.Sprintf("msg.getString(%s)", key)
	case protoreflect.BytesKind:
		return fmt.Sprintf("msg.getBytes(%s)", key)
	default:
		return fmt.Sprintf("msg.getLong(%s)", key)
	}
}

// javaReadDefault reads an implicit field, keeping the default when absent
// (getString/getBytes return null then).
func javaReadDefault(f *model.File, field model.Field, key string) string {
	switch field.Kind {
	case protoreflect.StringKind:
		return fmt.Sprintf("orEmpty(msg.getString(%s))", key)
	case protoreflect.BytesKind:
		return fmt.Sprintf("orEmpty(msg.getBytes(%s))", key)
	default:
		return javaRead(f, field, key)
	}
}

// javaLoadBox boxes a present oneof member into the Object payload.
func javaLoadBox(field model.Field, key string) string {
	switch field.Kind {
	case protoreflect.Int32Kind, protoreflect.Sint32Kind, protoreflect.Sfixed32Kind,
		protoreflect.Uint32Kind, protoreflect.Fixed32Kind, protoreflect.EnumKind:
		return fmt.Sprintf("msg.getInt(%s)", key)
	case protoreflect.Int64Kind, protoreflect.Sint64Kind, protoreflect.Sfixed64Kind,
		protoreflect.Uint64Kind, protoreflect.Fixed64Kind:
		return fmt.Sprintf("msg.getLong(%s)", key)
	case protoreflect.FloatKind:
		return fmt.Sprintf("msg.getFloat(%s)", key)
	case protoreflect.DoubleKind:
		return fmt.Sprintf("msg.getDouble(%s)", key)
	case protoreflect.BoolKind:
		return fmt.Sprintf("msg.getBool(%s)", key)
	case protoreflect.StringKind:
		return fmt.Sprintf("msg.getString(%s)", key)
	case protoreflect.BytesKind:
		return fmt.Sprintf("msg.getBytes(%s)", key)
	default:
		return fmt.Sprintf("msg.getLong(%s)", key)
	}
}

// writeJavaBuilder emits the nested Builder class.
func writeJavaBuilder(b *strings.Builder, f *model.File, m model.Message, cls string, oneofs []string) {
	b.WriteString("\n    public static final class Builder {\n")
	for _, field := range m.Fields {
		if field.Skip != "" || field.Oneof != "" {
			continue
		}
		writeJavaBuilderField(b, f, field)
	}
	for _, o := range oneofs {
		fmt.Fprintf(b, "        private int %sCase_;\n", model.LowerCamel(o))
		fmt.Fprintf(b, "        private Object %s_;\n", model.LowerCamel(o))
	}
	b.WriteString("\n        public Builder clear() {\n")
	for _, field := range m.Fields {
		if field.Skip != "" || field.Oneof != "" {
			continue
		}
		writeJavaClearOne(b, f, field, "            ")
	}
	for _, o := range oneofs {
		fmt.Fprintf(b, "            %sCase_ = 0;\n", model.LowerCamel(o))
		fmt.Fprintf(b, "            %s_ = null;\n", model.LowerCamel(o))
	}
	b.WriteString("            return this;\n        }\n\n")
	for _, field := range m.Fields {
		if field.Skip != "" || field.Oneof != "" {
			continue
		}
		writeJavaSetter(b, f, field)
	}
	for _, o := range oneofs {
		writeJavaOneofBuilder(b, f, m, o)
	}
	fmt.Fprintf(b, "        public %s build() {\n", cls)
	fmt.Fprintf(b, "            %s result = buildPartial();\n", cls)
	fmt.Fprintf(b, "            if (!result.isInitialized()) {\n")
	fmt.Fprintf(b, "                throw new IllegalStateException(%q + \": missing required fields\");\n", m.Full)
	b.WriteString("            }\n")
	b.WriteString("            return result;\n        }\n\n")
	fmt.Fprintf(b, "        public %s buildPartial() {\n", cls)
	fmt.Fprintf(b, "            return new %s(this);\n        }\n", cls)
	b.WriteString("    }\n\n")
	b.WriteString("    private static String orEmpty(String s) {\n")
	b.WriteString("        return s == null ? \"\" : s;\n    }\n\n")
	b.WriteString("    private static byte[] orEmpty(byte[] b) {\n")
	b.WriteString("        return b == null ? new byte[0] : b;\n    }\n")
}

// writeJavaBuilderField emits one mutable Builder slot.
func writeJavaBuilderField(b *strings.Builder, f *model.File, field model.Field) {
	priv := field.Lower + "_"
	if field.Repeated {
		if field.Kind == protoreflect.BytesKind {
			fmt.Fprintf(b, "        private List<byte[]> %s = new ArrayList<>();\n", priv)
		} else if field.Kind == protoreflect.EnumKind && field.Enum != "" {
			fmt.Fprintf(b, "        private List<Integer> %s = new ArrayList<>();\n", priv)
		} else if field.Message != "" {
			fmt.Fprintf(b, "        private List<%s> %s = new ArrayList<>();\n", javaType(f, field), priv)
		} else {
			fmt.Fprintf(b, "        private List<%s> %s = new ArrayList<>();\n", javaBoxed(f, field), priv)
		}
		return
	}
	if field.Message != "" {
		fmt.Fprintf(b, "        private %s %s;\n", javaType(f, field), priv)
		return
	}
	if field.Kind == protoreflect.StringKind || field.Kind == protoreflect.BytesKind {
		if field.NeedsHas || field.Required {
			fmt.Fprintf(b, "        private boolean has%s;\n", field.Pascal)
		}
		fmt.Fprintf(b, "        private %s %s;\n", javaType(f, field), priv)
		return
	}
	if field.Kind == protoreflect.EnumKind && field.Enum != "" {
		if field.NeedsHas || field.Required {
			fmt.Fprintf(b, "        private boolean has%s;\n", field.Pascal)
		}
		fmt.Fprintf(b, "        private int %s;\n", priv)
		return
	}
	if field.NeedsHas || field.Required {
		fmt.Fprintf(b, "        private boolean has%s;\n", field.Pascal)
	}
	fmt.Fprintf(b, "        private %s %s = %s;\n", javaType(f, field), priv, javaZero(f, field))
}

// writeJavaClearOne resets one Builder slot (used by clear()).
func writeJavaClearOne(b *strings.Builder, f *model.File, field model.Field, indent string) {
	priv := field.Lower + "_"
	if field.Repeated {
		fmt.Fprintf(b, "%s%s.clear();\n", indent, priv)
		return
	}
	if field.Message != "" {
		fmt.Fprintf(b, "%s%s = null;\n", indent, priv)
		return
	}
	if field.Kind == protoreflect.StringKind {
		if field.NeedsHas || field.Required {
			fmt.Fprintf(b, "%shas%s = false;\n", indent, field.Pascal)
		}
		fmt.Fprintf(b, "%s%s = null;\n", indent, priv)
		return
	}
	if field.Kind == protoreflect.BytesKind {
		if field.NeedsHas || field.Required {
			fmt.Fprintf(b, "%shas%s = false;\n", indent, field.Pascal)
		}
		fmt.Fprintf(b, "%s%s = null;\n", indent, priv)
		return
	}
	if field.Kind == protoreflect.EnumKind && field.Enum != "" {
		if field.NeedsHas || field.Required {
			fmt.Fprintf(b, "%shas%s = false;\n", indent, field.Pascal)
		}
		fmt.Fprintf(b, "%s%s = 0;\n", indent, priv)
		return
	}
	if field.NeedsHas || field.Required {
		fmt.Fprintf(b, "%shas%s = false;\n", indent, field.Pascal)
	}
	fmt.Fprintf(b, "%s%s = %s;\n", indent, priv, javaZero(f, field))
}

// writeJavaSetter emits set/clear (plus has/get and repeated helpers).
func writeJavaSetter(b *strings.Builder, f *model.File, field model.Field) {
	p, priv := field.Pascal, field.Lower+"_"
	t := javaType(f, field)
	if field.Repeated {
		writeJavaRepeatedSetter(b, f, field, p, priv, t)
		return
	}
	if field.Message != "" {
		fmt.Fprintf(b, "        public boolean has%s() {\n            return %s != null;\n        }\n\n", p, priv)
		fmt.Fprintf(b, "        public %s get%s() {\n", t, p)
		fmt.Fprintf(b, "            return %s == null ? %s.getDefaultInstance() : %s;\n        }\n\n", priv, t, priv)
		fmt.Fprintf(b, "        public Builder set%s(%s value) {\n", p, t)
		fmt.Fprintf(b, "            %s = Objects.requireNonNull(value);\n", priv)
		b.WriteString("            return this;\n        }\n\n")
		fmt.Fprintf(b, "        public Builder set%s(%s.Builder builder) {\n", p, t)
		fmt.Fprintf(b, "            %s = Objects.requireNonNull(builder).build();\n", priv)
		b.WriteString("            return this;\n        }\n\n")
		fmt.Fprintf(b, "        public Builder clear%s() {\n", p)
		fmt.Fprintf(b, "            %s = null;\n", priv)
		b.WriteString("            return this;\n        }\n\n")
		return
	}
	if field.Kind == protoreflect.EnumKind && field.Enum != "" {
		if field.NeedsHas || field.Required {
			fmt.Fprintf(b, "        public boolean has%s() {\n            return has%s;\n        }\n\n", p, p)
		}
		fmt.Fprintf(b, "        public int get%sValue() {\n            return %s;\n        }\n\n", p, priv)
		fmt.Fprintf(b, "        public %s get%s() {\n", t, p)
		fmt.Fprintf(b, "            %s v = %s.forNumber(%s);\n", t, t, priv)
		fmt.Fprintf(b, "            return v == null ? %s.UNRECOGNIZED : v;\n        }\n\n", t)
		fmt.Fprintf(b, "        public Builder set%s(%s value) {\n", p, t)
		fmt.Fprintf(b, "            Objects.requireNonNull(value);\n")
		if field.NeedsHas || field.Required {
			fmt.Fprintf(b, "            has%s = true;\n", p)
		}
		fmt.Fprintf(b, "            %s = value.getNumber();\n", priv)
		b.WriteString("            return this;\n        }\n\n")
		fmt.Fprintf(b, "        public Builder set%sValue(int value) {\n", p)
		if field.NeedsHas || field.Required {
			fmt.Fprintf(b, "            has%s = true;\n", p)
		}
		fmt.Fprintf(b, "            %s = value;\n", priv)
		b.WriteString("            return this;\n        }\n\n")
		fmt.Fprintf(b, "        public Builder clear%s() {\n", p)
		writeJavaClearOne(b, f, field, "            ")
		b.WriteString("            return this;\n        }\n\n")
		return
	}
	if field.Kind == protoreflect.StringKind {
		if field.NeedsHas || field.Required {
			fmt.Fprintf(b, "        public boolean has%s() {\n            return has%s;\n        }\n\n", p, p)
		}
		fmt.Fprintf(b, "        public String get%s() {\n", p)
		fmt.Fprintf(b, "            return %s == null ? \"\" : %s;\n        }\n\n", priv, priv)
		fmt.Fprintf(b, "        public Builder set%s(String value) {\n", p)
		fmt.Fprintf(b, "            %s = Objects.requireNonNull(value);\n", priv)
		if field.NeedsHas || field.Required {
			fmt.Fprintf(b, "            has%s = true;\n", p)
		}
		b.WriteString("            return this;\n        }\n\n")
		fmt.Fprintf(b, "        public Builder clear%s() {\n", p)
		writeJavaClearOne(b, f, field, "            ")
		b.WriteString("            return this;\n        }\n\n")
		return
	}
	if field.Kind == protoreflect.BytesKind {
		if field.NeedsHas || field.Required {
			fmt.Fprintf(b, "        public boolean has%s() {\n            return has%s;\n        }\n\n", p, p)
		}
		fmt.Fprintf(b, "        public byte[] get%s() {\n", p)
		fmt.Fprintf(b, "            return %s == null ? new byte[0] : %s.clone();\n        }\n\n", priv, priv)
		fmt.Fprintf(b, "        public Builder set%s(byte[] value) {\n", p)
		fmt.Fprintf(b, "            %s = Objects.requireNonNull(value).clone();\n", priv)
		if field.NeedsHas || field.Required {
			fmt.Fprintf(b, "            has%s = true;\n", p)
		}
		b.WriteString("            return this;\n        }\n\n")
		fmt.Fprintf(b, "        public Builder clear%s() {\n", p)
		writeJavaClearOne(b, f, field, "            ")
		b.WriteString("            return this;\n        }\n\n")
		return
	}
	if field.NeedsHas || field.Required {
		fmt.Fprintf(b, "        public boolean has%s() {\n            return has%s;\n        }\n\n", p, p)
	}
	fmt.Fprintf(b, "        public %s get%s() {\n            return %s;\n        }\n\n", t, p, priv)
	fmt.Fprintf(b, "        public Builder set%s(%s value) {\n", p, t)
	fmt.Fprintf(b, "            %s = value;\n", priv)
	if field.NeedsHas || field.Required {
		fmt.Fprintf(b, "            has%s = true;\n", p)
	}
	b.WriteString("            return this;\n        }\n\n")
	fmt.Fprintf(b, "        public Builder clear%s() {\n", p)
	writeJavaClearOne(b, f, field, "            ")
	b.WriteString("            return this;\n        }\n\n")
}

// writeJavaRepeatedSetter emits the List/Count/indexed/set/add/addAll/clear set.
func writeJavaRepeatedSetter(b *strings.Builder, f *model.File, field model.Field, p, priv, t string) {
	if field.Kind == protoreflect.BytesKind {
		fmt.Fprintf(b, "        public List<byte[]> get%sList() {\n", p)
		fmt.Fprintf(b, "            List<byte[]> out = new ArrayList<>(%s.size());\n", priv)
		fmt.Fprintf(b, "            for (byte[] e : %s) {\n                out.add(e.clone());\n            }\n", priv)
		fmt.Fprintf(b, "            return Collections.unmodifiableList(out);\n        }\n\n")
		fmt.Fprintf(b, "        public int get%sCount() {\n            return %s.size();\n        }\n\n", p, priv)
		fmt.Fprintf(b, "        public byte[] get%s(int index) {\n            return %s.get(index).clone();\n        }\n\n", p, priv)
		fmt.Fprintf(b, "        public Builder set%s(int index, byte[] value) {\n", p)
		fmt.Fprintf(b, "            %s.set(index, Objects.requireNonNull(value).clone());\n", priv)
		b.WriteString("            return this;\n        }\n\n")
		fmt.Fprintf(b, "        public Builder add%s(byte[] value) {\n", p)
		fmt.Fprintf(b, "            %s.add(Objects.requireNonNull(value).clone());\n", priv)
		b.WriteString("            return this;\n        }\n\n")
		fmt.Fprintf(b, "        public Builder addAll%s(Iterable<byte[]> values) {\n", p)
		fmt.Fprintf(b, "            for (byte[] v : values) {\n                %s.add(Objects.requireNonNull(v).clone());\n            }\n", priv)
		b.WriteString("            return this;\n        }\n\n")
		fmt.Fprintf(b, "        public Builder clear%s() {\n            %s.clear();\n            return this;\n        }\n\n", p, priv)
		return
	}
	if field.Kind == protoreflect.EnumKind && field.Enum != "" {
		fmt.Fprintf(b, "        public List<%s> get%sList() {\n", t, p)
		fmt.Fprintf(b, "            List<%s> out = new ArrayList<>(%s.size());\n", t, priv)
		fmt.Fprintf(b, "            for (int v : %s) {\n", priv)
		fmt.Fprintf(b, "                %s e = %s.forNumber(v);\n", t, t)
		fmt.Fprintf(b, "                out.add(e == null ? %s.UNRECOGNIZED : e);\n", t)
		fmt.Fprintf(b, "            }\n            return Collections.unmodifiableList(out);\n        }\n\n")
		fmt.Fprintf(b, "        public List<Integer> get%sValueList() {\n            return Collections.unmodifiableList(%s);\n        }\n\n", p, priv)
		fmt.Fprintf(b, "        public int get%sCount() {\n            return %s.size();\n        }\n\n", p, priv)
		fmt.Fprintf(b, "        public %s get%s(int index) {\n", t, p)
		fmt.Fprintf(b, "            %s e = %s.forNumber(%s.get(index));\n", t, t, priv)
		fmt.Fprintf(b, "            return e == null ? %s.UNRECOGNIZED : e;\n        }\n\n", t)
		fmt.Fprintf(b, "        public int get%sValue(int index) {\n            return %s.get(index);\n        }\n\n", p, priv)
		fmt.Fprintf(b, "        public Builder set%s(int index, %s value) {\n", p, t)
		fmt.Fprintf(b, "            Objects.requireNonNull(value);\n")
		fmt.Fprintf(b, "            %s.set(index, value.getNumber());\n", priv)
		b.WriteString("            return this;\n        }\n\n")
		fmt.Fprintf(b, "        public Builder set%sValue(int index, int value) {\n", p)
		fmt.Fprintf(b, "            %s.set(index, value);\n", priv)
		b.WriteString("            return this;\n        }\n\n")
		fmt.Fprintf(b, "        public Builder add%s(%s value) {\n", p, t)
		fmt.Fprintf(b, "            Objects.requireNonNull(value);\n")
		fmt.Fprintf(b, "            %s.add(value.getNumber());\n", priv)
		b.WriteString("            return this;\n        }\n\n")
		fmt.Fprintf(b, "        public Builder add%sValue(int value) {\n", p)
		fmt.Fprintf(b, "            %s.add(value);\n", priv)
		b.WriteString("            return this;\n        }\n\n")
		fmt.Fprintf(b, "        public Builder addAll%s(Iterable<? extends %s> values) {\n", p, t)
		fmt.Fprintf(b, "            for (%s v : values) {\n                %s.add(Objects.requireNonNull(v).getNumber());\n            }\n", t, priv)
		b.WriteString("            return this;\n        }\n\n")
		fmt.Fprintf(b, "        public Builder clear%s() {\n            %s.clear();\n            return this;\n        }\n\n", p, priv)
		return
	}
	elem := unbox(javaBoxed(f, field))
	box := javaBoxed(f, field)
	fmt.Fprintf(b, "        public List<%s> get%sList() {\n            return Collections.unmodifiableList(%s);\n        }\n\n", box, p, priv)
	fmt.Fprintf(b, "        public int get%sCount() {\n            return %s.size();\n        }\n\n", p, priv)
	fmt.Fprintf(b, "        public %s get%s(int index) {\n            return %s.get(index);\n        }\n\n", elem, p, priv)
	if field.Message != "" {
		fmt.Fprintf(b, "        public Builder set%s(int index, %s value) {\n", p, t)
		fmt.Fprintf(b, "            %s.set(index, Objects.requireNonNull(value));\n", priv)
		b.WriteString("            return this;\n        }\n\n")
		fmt.Fprintf(b, "        public Builder set%s(int index, %s.Builder builder) {\n", p, t)
		fmt.Fprintf(b, "            %s.set(index, Objects.requireNonNull(builder).build());\n", priv)
		b.WriteString("            return this;\n        }\n\n")
		fmt.Fprintf(b, "        public Builder add%s(%s value) {\n", p, t)
		fmt.Fprintf(b, "            %s.add(Objects.requireNonNull(value));\n", priv)
		b.WriteString("            return this;\n        }\n\n")
		fmt.Fprintf(b, "        public Builder add%s(%s.Builder builder) {\n", p, t)
		fmt.Fprintf(b, "            %s.add(Objects.requireNonNull(builder).build());\n", priv)
		b.WriteString("            return this;\n        }\n\n")
		fmt.Fprintf(b, "        public Builder addAll%s(Iterable<? extends %s> values) {\n", p, t)
		fmt.Fprintf(b, "            for (%s v : values) {\n                %s.add(Objects.requireNonNull(v));\n            }\n", t, priv)
		b.WriteString("            return this;\n        }\n\n")
	} else {
		nullCheck := ""
		if elem == "String" {
			nullCheck = "            Objects.requireNonNull(value);\n"
		}
		fmt.Fprintf(b, "        public Builder set%s(int index, %s value) {\n", p, elem)
		b.WriteString(nullCheck)
		fmt.Fprintf(b, "            %s.set(index, value);\n", priv)
		b.WriteString("            return this;\n        }\n\n")
		fmt.Fprintf(b, "        public Builder add%s(%s value) {\n", p, elem)
		b.WriteString(nullCheck)
		fmt.Fprintf(b, "            %s.add(value);\n", priv)
		b.WriteString("            return this;\n        }\n\n")
		fmt.Fprintf(b, "        public Builder addAll%s(Iterable<? extends %s> values) {\n", p, box)
		if elem == "String" {
			fmt.Fprintf(b, "            for (%s v : values) {\n                %s.add(Objects.requireNonNull(v));\n            }\n", box, priv)
		} else {
			fmt.Fprintf(b, "            for (%s v : values) {\n                %s.add(v);\n            }\n", box, priv)
		}
		b.WriteString("            return this;\n        }\n\n")
	}
	fmt.Fprintf(b, "        public Builder clear%s() {\n            %s.clear();\n            return this;\n        }\n\n", p, priv)
}

// writeJavaOneofBuilder emits getKindCase/clearKind plus the member accessors.
func writeJavaOneofBuilder(b *strings.Builder, f *model.File, m model.Message, o string) {
	pascal, lower := model.Pascal(o), model.LowerCamel(o)
	fmt.Fprintf(b, "        public %sCase get%sCase() {\n", pascal, pascal)
	fmt.Fprintf(b, "            return %sCase.forNumber(%sCase_);\n        }\n\n", pascal, lower)
	fmt.Fprintf(b, "        public Builder clear%s() {\n", pascal)
	fmt.Fprintf(b, "            %sCase_ = 0;\n", lower)
	fmt.Fprintf(b, "            %s_ = null;\n", lower)
	b.WriteString("            return this;\n        }\n\n")
	for _, field := range m.Fields {
		if field.Skip != "" || field.Oneof != o {
			continue
		}
		p := field.Pascal
		num := field.Number
		t := javaType(f, field)
		fmt.Fprintf(b, "        public boolean has%s() {\n            return %sCase_ == %d;\n        }\n\n", p, lower, num)
		switch {
		case field.Message != "":
			fmt.Fprintf(b, "        public %s get%s() {\n", t, p)
			fmt.Fprintf(b, "            return %sCase_ == %d ? (%s) %s_ : %s.getDefaultInstance();\n        }\n\n",
				lower, num, t, lower, t)
			fmt.Fprintf(b, "        public Builder set%s(%s value) {\n", p, t)
			fmt.Fprintf(b, "            %s_ = Objects.requireNonNull(value);\n", lower)
			fmt.Fprintf(b, "            %sCase_ = %d;\n", lower, num)
			b.WriteString("            return this;\n        }\n\n")
			fmt.Fprintf(b, "        public Builder set%s(%s.Builder builder) {\n", p, t)
			fmt.Fprintf(b, "            %s_ = Objects.requireNonNull(builder).build();\n", lower)
			fmt.Fprintf(b, "            %sCase_ = %d;\n", lower, num)
			b.WriteString("            return this;\n        }\n\n")
			fmt.Fprintf(b, "        public Builder clear%s() {\n", p)
			fmt.Fprintf(b, "            if (%sCase_ == %d) {\n", lower, num)
			fmt.Fprintf(b, "                %sCase_ = 0;\n", lower)
			fmt.Fprintf(b, "                %s_ = null;\n", lower)
			b.WriteString("            }\n            return this;\n        }\n\n")
		case field.Kind == protoreflect.EnumKind && field.Enum != "":
			fmt.Fprintf(b, "        public int get%sValue() {\n", p)
			fmt.Fprintf(b, "            return %sCase_ == %d ? (Integer) %s_ : 0;\n        }\n\n", lower, num, lower)
			fmt.Fprintf(b, "        public %s get%s() {\n", t, p)
			fmt.Fprintf(b, "            if (%sCase_ != %d) {\n                return %s.UNRECOGNIZED;\n            }\n", lower, num, t)
			fmt.Fprintf(b, "            %s e = %s.forNumber((Integer) %s_);\n", t, t, lower)
			fmt.Fprintf(b, "            return e == null ? %s.UNRECOGNIZED : e;\n        }\n\n", t)
			fmt.Fprintf(b, "        public Builder set%s(%s value) {\n", p, t)
			fmt.Fprintf(b, "            Objects.requireNonNull(value);\n")
			fmt.Fprintf(b, "            %s_ = value.getNumber();\n", lower)
			fmt.Fprintf(b, "            %sCase_ = %d;\n", lower, num)
			b.WriteString("            return this;\n        }\n\n")
			fmt.Fprintf(b, "        public Builder set%sValue(int value) {\n", p)
			fmt.Fprintf(b, "            %s_ = value;\n", lower)
			fmt.Fprintf(b, "            %sCase_ = %d;\n", lower, num)
			b.WriteString("            return this;\n        }\n\n")
			fmt.Fprintf(b, "        public Builder clear%s() {\n", p)
			fmt.Fprintf(b, "            if (%sCase_ == %d) {\n", lower, num)
			fmt.Fprintf(b, "                %sCase_ = 0;\n", lower)
			fmt.Fprintf(b, "                %s_ = null;\n", lower)
			b.WriteString("            }\n            return this;\n        }\n\n")
		case field.Kind == protoreflect.StringKind:
			fmt.Fprintf(b, "        public String get%s() {\n", p)
			fmt.Fprintf(b, "            return %sCase_ == %d ? (String) %s_ : \"\";\n        }\n\n", lower, num, lower)
			fmt.Fprintf(b, "        public Builder set%s(String value) {\n", p)
			fmt.Fprintf(b, "            %s_ = Objects.requireNonNull(value);\n", lower)
			fmt.Fprintf(b, "            %sCase_ = %d;\n", lower, num)
			b.WriteString("            return this;\n        }\n\n")
			fmt.Fprintf(b, "        public Builder clear%s() {\n", p)
			fmt.Fprintf(b, "            if (%sCase_ == %d) {\n", lower, num)
			fmt.Fprintf(b, "                %sCase_ = 0;\n", lower)
			fmt.Fprintf(b, "                %s_ = null;\n", lower)
			b.WriteString("            }\n            return this;\n        }\n\n")
		case field.Kind == protoreflect.BytesKind:
			fmt.Fprintf(b, "        public byte[] get%s() {\n", p)
			fmt.Fprintf(b, "            return %sCase_ == %d ? ((byte[]) %s_).clone() : new byte[0];\n        }\n\n", lower, num, lower)
			fmt.Fprintf(b, "        public Builder set%s(byte[] value) {\n", p)
			fmt.Fprintf(b, "            %s_ = Objects.requireNonNull(value).clone();\n", lower)
			fmt.Fprintf(b, "            %sCase_ = %d;\n", lower, num)
			b.WriteString("            return this;\n        }\n\n")
			fmt.Fprintf(b, "        public Builder clear%s() {\n", p)
			fmt.Fprintf(b, "            if (%sCase_ == %d) {\n", lower, num)
			fmt.Fprintf(b, "                %sCase_ = 0;\n", lower)
			fmt.Fprintf(b, "                %s_ = null;\n", lower)
			b.WriteString("            }\n            return this;\n        }\n\n")
		default:
			fmt.Fprintf(b, "        public %s get%s() {\n", t, p)
			if t == "long" {
				fmt.Fprintf(b, "            return %sCase_ == %d ? (Long) %s_ : 0L;\n        }\n\n", lower, num, lower)
			} else if t == "float" {
				fmt.Fprintf(b, "            return %sCase_ == %d ? (Float) %s_ : 0f;\n        }\n\n", lower, num, lower)
			} else if t == "double" {
				fmt.Fprintf(b, "            return %sCase_ == %d ? (Double) %s_ : 0.0;\n        }\n\n", lower, num, lower)
			} else if t == "boolean" {
				fmt.Fprintf(b, "            return %sCase_ == %d ? (Boolean) %s_ : false;\n        }\n\n", lower, num, lower)
			} else {
				fmt.Fprintf(b, "            return %sCase_ == %d ? (Integer) %s_ : 0;\n        }\n\n", lower, num, lower)
			}
			fmt.Fprintf(b, "        public Builder set%s(%s value) {\n", p, t)
			fmt.Fprintf(b, "            %s_ = value;\n", lower)
			fmt.Fprintf(b, "            %sCase_ = %d;\n", lower, num)
			b.WriteString("            return this;\n        }\n\n")
			fmt.Fprintf(b, "        public Builder clear%s() {\n", p)
			fmt.Fprintf(b, "            if (%sCase_ == %d) {\n", lower, num)
			fmt.Fprintf(b, "                %sCase_ = 0;\n", lower)
			fmt.Fprintf(b, "                %s_ = null;\n", lower)
			b.WriteString("            }\n            return this;\n        }\n\n")
		}
	}
}

// writeJavaKindCase emits the nested oneof case enum.
func writeJavaKindCase(b *strings.Builder, m model.Message, o string) {
	pascal := model.Pascal(o)
	fmt.Fprintf(b, "\n    public enum %sCase {\n", pascal)
	for _, field := range m.Fields {
		if field.Skip != "" || field.Oneof != o {
			continue
		}
		fmt.Fprintf(b, "        %s(%d),\n", strings.ToUpper(field.Name), field.Number)
	}
	fmt.Fprintf(b, "        %s_NOT_SET(0);\n\n", strings.ToUpper(o))
	b.WriteString("        private final int value;\n\n")
	fmt.Fprintf(b, "        private %sCase(int value) {\n", pascal)
	b.WriteString("            this.value = value;\n        }\n\n")
	b.WriteString("        public int getNumber() {\n            return value;\n        }\n\n")
	fmt.Fprintf(b, "        public static %sCase forNumber(int value) {\n", pascal)
	b.WriteString("            switch (value) {\n")
	for _, field := range m.Fields {
		if field.Skip != "" || field.Oneof != o {
			continue
		}
		fmt.Fprintf(b, "                case %d: return %s;\n", field.Number, strings.ToUpper(field.Name))
	}
	b.WriteString("                default: return null;\n            }\n        }\n    }\n")
}

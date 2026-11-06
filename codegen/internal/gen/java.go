package gen

import (
	"fmt"
	"strings"

	"pbwire-codegen/internal/model"

	"google.golang.org/protobuf/reflect/protoreflect"
)

// javaFiles emits protobuf-java-shaped immutable messages with a direct
// Wire codec: one public class per message with a nested Builder
// (newBuilder/setX/addX/clear/build), toByteArray/parseFrom,
// hasX/getX accessors, unmodifiable List getters for repeated fields, a
// KindCase enum for oneofs and Java enums for enum fields. toByteArray writes
// straight into a Wire.Buf and parseFrom reads straight from a Wire.Reader:
// no Message tree, no per-field map lookup, no schema at use time. Wire
// bytes are the only shared contract with the dynamic binding. No
// protobuf-java dependency: the shape mirrors its method names, not its
// runtime.
func javaFiles(f *model.File, opt Options) []File {
	var files []File
	_ = opt
	for _, e := range f.Enums {
		files = append(files, javaEnumFile(f, e))
	}
	for _, m := range f.Messages {
		files = append(files, javaOrBuilderFile(f, m))
		files = append(files, javaMessageFile(f, m))
		files = append(files, javaMetaFile(f, m))
	}
	return files
}

// javaOrBuilderFile emits the top-level read interface of one message: the
// OrBuilder the message and its Builder share. A separate file because a
// class cannot implement its own nested interface.
func javaOrBuilderFile(f *model.File, m model.Message) File {
	cls := className(f, m.Name)
	var b strings.Builder
	b.WriteString(header(f, "Read interface of generated message "+m.Full+"."))
	b.WriteString("import java.util.List;\n\n")
	fmt.Fprintf(&b, "public interface %sOrBuilder {\n", cls)
	oneofs := javaOneofs(m)
	for _, field := range m.Fields {
		if field.Skip != "" || field.Oneof != "" {
			continue
		}
		writeJavaOrBuilderField(&b, f, field)
	}
	for _, o := range oneofs {
		pascal := model.Pascal(o)
		fmt.Fprintf(&b, "    %s.KindCase get%sCase();\n", cls, pascal)
		for _, field := range m.Fields {
			if field.Skip != "" || field.Oneof != o {
				continue
			}
			writeJavaOrBuilderMember(&b, f, field)
		}
	}
	b.WriteString("}\n")
	return File{Name: cls + "OrBuilder.java", Content: b.String()}
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
		return javaMsg(f, field)
	}
}

// javaMsg names the class for a message field: the owning file's prefix when
// foreign (same default package, so no import line), this file's own
// otherwise.
func javaMsg(f *model.File, field model.Field) string {
	if field.Import != "" {
		if imp := findImport(f, field.Import); imp != nil {
			return imp.Prefix + "_" + field.Message
		}
	}
	return className(f, field.Message)
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
		return javaMsg(f, field)
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
	b.WriteString(header(f, "Generated message "+m.Full+" with a direct Wire codec."))
	b.WriteString("import java.util.ArrayList;\nimport java.util.Collections;\nimport java.util.List;\nimport java.util.Objects;\n\n")
	fmt.Fprintf(&b, "public final class %s implements %sOrBuilder {\n", cls, cls)
	b.WriteString("    // Self-registration: one call registers the message, its Builder\n")
	b.WriteString("    // and the interface mapping (last registration wins).\n")
	fmt.Fprintf(&b, "    static {{\n        %sMeta.register(%s.class, %s.Builder::new, %s.Builder::build);\n    }}\n\n", cls, cls, cls, cls)
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

	writeJavaCodec(&b, f, m, cls, oneofs)
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
		fmt.Fprintf(b, "    public %s %s() {\n        return %s;\n    }\n\n", t, javaGetterName(field), priv)
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
			fmt.Fprintf(b, "    public boolean is%s() {\n", p)
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

// javaKindInt maps a proto kind to the table engine's Kind constant.
func javaKindInt(kind protoreflect.Kind) string {
	switch kind {
	case protoreflect.Int32Kind:
		return "Kind.INT32"
	case protoreflect.Uint32Kind:
		return "Kind.UINT32"
	case protoreflect.Sint32Kind:
		return "Kind.SINT32"
	case protoreflect.Int64Kind:
		return "Kind.INT64"
	case protoreflect.Uint64Kind:
		return "Kind.UINT64"
	case protoreflect.Sint64Kind:
		return "Kind.SINT64"
	case protoreflect.BoolKind:
		return "Kind.BOOL"
	case protoreflect.EnumKind:
		return "Kind.ENUM"
	case protoreflect.Fixed32Kind:
		return "Kind.FIXED32"
	case protoreflect.Sfixed32Kind:
		return "Kind.SFIXED32"
	case protoreflect.FloatKind:
		return "Kind.FLOAT"
	case protoreflect.Fixed64Kind:
		return "Kind.FIXED64"
	case protoreflect.Sfixed64Kind:
		return "Kind.SFIXED64"
	case protoreflect.DoubleKind:
		return "Kind.DOUBLE"
	case protoreflect.StringKind:
		return "Kind.STRING"
	case protoreflect.BytesKind:
		return "Kind.BYTES"
	case protoreflect.GroupKind:
		return "Kind.GROUP"
	default:
		return "Kind.MESSAGE"
	}
}

// javaGetterName is the read accessor: isX for singular booleans (Lombok
// style), getX otherwise. Repeated element access stays getX(i).
func javaGetterName(field model.Field) string {
	if field.Kind == protoreflect.BoolKind && !field.Repeated {
		return "is" + field.Pascal
	}
	return "get" + field.Pascal
}

// writeJavaCodec emits the delegating toByteArray/parseFrom: one line each
// through the PbCodec registry, so the message class carries no codec state.
// The table lives in the sibling <Cls>Meta file; all wire logic in PbCodec.
func writeJavaCodec(b *strings.Builder, f *model.File, m model.Message, cls string, oneofs []string) {
	b.WriteString("    public byte[] toByteArray() {\n")
	b.WriteString("        return PbCodec.encode(this);\n")
	b.WriteString("    }\n\n")
	b.WriteString("    public static " + cls + " parseFrom(byte[] data) {\n")
	b.WriteString("        return PbCodec.parseFrom(data, " + cls + ".class);\n")
	b.WriteString("    }\n\n")
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

// writeJavaOrBuilderField emits one non-oneof member of the interface.
func writeJavaOrBuilderField(b *strings.Builder, f *model.File, field model.Field) {
	p := field.Pascal
	if field.Repeated {
		writeJavaOrBuilderRepeated(b, f, field, p)
		return
	}
	if field.Message != "" {
		fmt.Fprintf(b, "    boolean has%s();\n", p)
		fmt.Fprintf(b, "    %s get%s();\n", javaType(f, field), p)
		return
	}
	if field.Kind == protoreflect.EnumKind && field.Enum != "" {
		if field.NeedsHas || field.Required {
			fmt.Fprintf(b, "    boolean has%s();\n", p)
		}
		fmt.Fprintf(b, "    int get%sValue();\n", p)
		fmt.Fprintf(b, "    %s get%s();\n", javaType(f, field), p)
		return
	}
	if field.NeedsHas || field.Required {
		fmt.Fprintf(b, "    boolean has%s();\n", p)
	}
	fmt.Fprintf(b, "    %s %s();\n", javaType(f, field), javaGetterName(field))
}

// writeJavaOrBuilderRepeated emits one repeated member of the interface.
func writeJavaOrBuilderRepeated(b *strings.Builder, f *model.File, field model.Field, p string) {
	if field.Kind == protoreflect.BytesKind {
		fmt.Fprintf(b, "    List<byte[]> get%sList();\n", p)
		fmt.Fprintf(b, "    int get%sCount();\n", p)
		fmt.Fprintf(b, "    byte[] get%s(int index);\n", p)
		return
	}
	if field.Kind == protoreflect.EnumKind && field.Enum != "" {
		t := javaType(f, field)
		fmt.Fprintf(b, "    List<%s> get%sList();\n", t, p)
		fmt.Fprintf(b, "    List<Integer> get%sValueList();\n", p)
		fmt.Fprintf(b, "    int get%sCount();\n", p)
		fmt.Fprintf(b, "    %s get%s(int index);\n", t, p)
		fmt.Fprintf(b, "    int get%sValue(int index);\n", p)
		return
	}
	t := javaBoxed(f, field)
	fmt.Fprintf(b, "    List<%s> get%sList();\n", t, p)
	fmt.Fprintf(b, "    int get%sCount();\n", p)
	fmt.Fprintf(b, "    %s get%s(int index);\n", unbox(t), p)
}

// writeJavaOrBuilderMember emits one oneof member of the interface.
func writeJavaOrBuilderMember(b *strings.Builder, f *model.File, field model.Field) {
	p := field.Pascal
	fmt.Fprintf(b, "    boolean has%s();\n", p)
	if field.Message != "" {
		fmt.Fprintf(b, "    %s get%s();\n", javaType(f, field), p)
		return
	}
	if field.Kind == protoreflect.EnumKind && field.Enum != "" {
		fmt.Fprintf(b, "    int get%sValue();\n", p)
		fmt.Fprintf(b, "    %s get%s();\n", javaType(f, field), p)
		return
	}
	fmt.Fprintf(b, "    %s %s();\n", javaType(f, field), javaGetterName(field))
}

// javaMetaFile emits the codec table of one message as a sibling file: the
// message class stays a plain OrBuilder implementation, and this file binds
// it to the PbCodec engine with one register call in its static initializer.
// The rows read through the OrBuilder interface and decode through the
// Builder, so a hand-written class implementing the same interface replaces
// the message with one PbCodec.register call. The interface registration
// lets unregistered implementations encode with no per-class call, and
// parseFrom through the OrBuilder decode into the registered Builder's
// product (a later registerInterface call swaps in the custom Builder).
func javaMetaFile(f *model.File, m model.Message) File {
	cls := className(f, m.Name)
	var b strings.Builder
	b.WriteString(header(f, "Codec table of generated message "+m.Full+"."))
	b.WriteString("import java.util.function.Function;\nimport java.util.function.Supplier;\n\n")
	fmt.Fprintf(&b, "final class %sMeta {\n", cls)
	writeJavaMeta(&b, f, m)
	writeJavaRegister(&b, f, m, cls)
	b.WriteString("}\n")
	return File{Name: cls + "Meta.java", Content: b.String()}
}

// javaOneofIndex returns the position of a oneof in the message's oneof
// list: the FieldMeta.oneof value the dynamic schema reads back.
func javaOneofIndex(oneofs []string, name string) int {
	for i, o := range oneofs {
		if o == name {
			return i
		}
	}
	return -1
}

// writeJavaMeta emits the compiled field table into the sibling <Cls>Meta
// file as a forBuilder factory: one row per field binding the OrBuilder
// getters to setter lookups on the passed Builder class, sub-message
// tables via their own <Sub>Meta files. The default META binds the
// generated Builder; a hand-written Builder with the canonical shape
// shares the exact same rows with no hand-written table.
func writeJavaMeta(b *strings.Builder, f *model.File, m model.Message) {
	cls := className(f, m.Name)
	b.WriteString("    @SuppressWarnings({\"unchecked\", \"rawtypes\"})\n")
	fmt.Fprintf(b, "    static final Meta<%sOrBuilder, %s.Builder> META = forBuilder(%s.Builder.class);\n\n", cls, cls, cls)
	b.WriteString("    @SuppressWarnings({\"unchecked\", \"rawtypes\"})\n")
	fmt.Fprintf(b, "    static <B> Meta<%sOrBuilder, B> forBuilder(Class<B> builderClass) {\n", cls)
	fmt.Fprintf(b, "        return new Meta<>(\n")
	fmt.Fprintf(b, "                %q,\n", m.Full)
	b.WriteString("                new FieldMeta[] {\n")
	ob := cls + "OrBuilder"
	oneofs := javaOneofs(m)
	for _, field := range m.Fields {
		if field.Skip != "" {
			continue
		}
		writeJavaMetaRow(b, f, field, ob, oneofs)
	}
	if len(oneofs) == 0 {
		b.WriteString("                },\n                new String[0]);\n")
	} else {
		b.WriteString("                },\n                new String[] {")
		for i, o := range oneofs {
			if i > 0 {
				b.WriteString(", ")
			}
			fmt.Fprintf(b, "%q", o)
		}
		b.WriteString("});\n")
	}
	b.WriteString("    }\n\n")
}

// writeJavaRegister emits the one-call registration: the generated Builder
// (or any hand-written one with the canonical shape) is bound through
// forBuilder above, then registered under the message class and the
// OrBuilder interface alike, so a later registration wins everywhere.
// The factory and finish are passed straight through; the product and
// presence checks fail fast on a misshapen Builder.
func writeJavaRegister(b *strings.Builder, f *model.File, m model.Message, cls string) {
	ob := cls + "OrBuilder"
	b.WriteString("    /**\n")
	b.WriteString("     * One call registers a hand-written Builder with the canonical shape:\n")
	b.WriteString("     * a nested {@code Builder} with a public no-arg constructor and\n")
	b.WriteString("     * {@code build()}, plus the same setters the generated Builder has\n")
	b.WriteString("     * (and {@code hasX} for required fields). Registers the message\n")
	b.WriteString("     * class and re-points the OrBuilder interface at the same pair,\n")
	b.WriteString("     * so a later registration wins everywhere: encode, decode by\n")
	b.WriteString("     * class and decode by interface. Fails fast.\n")
	b.WriteString("     */\n")
	b.WriteString("    @SuppressWarnings({\"unchecked\", \"rawtypes\"})\n")
	b.WriteString("    static <M, B> void register(Class<M> messageClass, Supplier<B> factory, Function<B, M> finish) {\n")
	b.WriteString("        Class<B> builderClass = Bind.nestedBuilder(messageClass);\n")
	fmt.Fprintf(b, "        Meta<%s, B> meta = forBuilder(builderClass);\n", ob)
	fmt.Fprintf(b, "        Bind.checkProduct(builderClass, messageClass, %s.class);\n", ob)
	b.WriteString("        PbCodec.register(messageClass, meta, factory, finish);\n")
	fmt.Fprintf(b, "        PbCodec.registerInterface(%s.class, meta, factory, finish);\n", ob)
	b.WriteString("    }\n")
}

// javaHasLambdas renders the presence gate of one row: the OrBuilder
// has-method, plus the Builder has-method name for required fields (only
// required rows are ever presence-checked, so anything else stays null
// and a custom Builder needs no has-methods except for required fields).
func javaHasLambdas(ob string, field model.Field) (string, string) {
	hasM := "null"
	if field.NeedsHas || field.Required || field.Message != "" {
		hasM = ob + "::has" + field.Pascal
	}
	hasB := ""
	if field.Required {
		hasB = "has" + field.Pascal
	}
	return hasM, hasB
}

// writeJavaMetaRow emits one table row: the setters resolve against the
// passed Builder class by canonical name, so the rows depend only on the
// field shape, never on hand-written code.
func writeJavaMetaRow(b *strings.Builder, f *model.File, field model.Field, ob string, oneofs []string) {
	num := field.Number
	hasM, hasBMethod := javaHasLambdas(ob, field)
	req := "false"
	if field.Required {
		req = "true"
	}
	hasB := "null"
	if hasBMethod != "" {
		hasB = "Bind.checker(builderClass, \"" + hasBMethod + "\")"
	}
	bindInt := func(name string) string {
		return "Bind.intSetter(builderClass, \"" + name + "\")"
	}
	bindLong := func(name string) string {
		return "Bind.longSetter(builderClass, \"" + name + "\")"
	}
	bindDouble := func(name string) string {
		return "Bind.doubleSetter(builderClass, \"" + name + "\")"
	}
	bindRef := func(name, vtype string) string {
		return "Bind.setter(builderClass, \"" + name + "\", " + vtype + ".class)"
	}
	setV := func(name, vtype string) string {
		switch vtype {
		case "int":
			return bindInt(name)
		case "long":
			return bindLong(name)
		case "double":
			return bindDouble(name)
		default:
			return bindRef(name, vtype)
		}
	}
	if field.Oneof != "" {
		caseOf := "(" + ob + " o) -> o.get" + model.Pascal(field.Oneof) + "Case().getNumber()"
		idx := javaOneofIndex(oneofs, field.Oneof)
		switch {
		case field.Message != "":
			emitOneofMsgRow(b, f, field, num, caseOf, ob, idx)
			return
		case field.Kind == protoreflect.StringKind:
			fmt.Fprintf(b, "                FieldMeta.ofOneofString(%d, %s::%s, %s, %s, %d, %q),\n",
				num, ob, javaGetterName(field), setV("set"+field.Pascal, "String"), caseOf, idx, field.Name)
			return
		case field.Kind == protoreflect.BytesKind:
			fmt.Fprintf(b, "                FieldMeta.ofOneofBytes(%d, %s::%s, %s, %s, %d, %q),\n",
				num, ob, javaGetterName(field), setV("set"+field.Pascal, "byte[]"), caseOf, idx, field.Name)
			return
		case field.Kind == protoreflect.BoolKind:
			fmt.Fprintf(b, "                FieldMeta.ofOneofBool(%d, %s::%s, %s, %s, %d, %q),\n",
				num, ob, javaGetterName(field), setV("set"+field.Pascal, "Boolean"), caseOf, idx, field.Name)
			return
		case field.Kind == protoreflect.FloatKind:
			fmt.Fprintf(b, "                FieldMeta.ofOneofFloat(%d, %s::%s, %s, %s, %d, %q),\n",
				num, ob, javaGetterName(field), setV("set"+field.Pascal, "Float"), caseOf, idx, field.Name)
			return
		case field.Kind == protoreflect.DoubleKind:
			fmt.Fprintf(b, "                FieldMeta.ofOneofDouble(%d, %s::%s, %s, %s, %d, %q),\n",
				num, ob, javaGetterName(field), setV("set"+field.Pascal, "double"), caseOf, idx, field.Name)
			return
		case field.Kind == protoreflect.Int64Kind, field.Kind == protoreflect.Uint64Kind,
			field.Kind == protoreflect.Sint64Kind, field.Kind == protoreflect.Fixed64Kind,
			field.Kind == protoreflect.Sfixed64Kind:
			fmt.Fprintf(b, "                FieldMeta.ofOneofLong(%d, %s, %s::%s, %s, %s, %d, %q),\n",
				num, javaKindInt(field.Kind), ob, javaGetterName(field), setV("set"+field.Pascal, "long"), caseOf, idx, field.Name)
			return
		default:
			fmt.Fprintf(b, "                FieldMeta.ofOneofInt(%d, %s, %s::%s, %s, %s, %d, %q),\n",
				num, javaKindInt(field.Kind), ob, javaGetterName(field), setV("set"+field.Pascal, "int"), caseOf, idx, field.Name)
			return
		}
	}
	switch {
	case field.Message != "" && !field.Repeated:
		t := javaType(f, field)
		fac := t + ".Builder::new"
		fin := t + ".Builder::buildPartial"
		if field.Kind == protoreflect.GroupKind {
			fmt.Fprintf(b, "                FieldMeta.ofGroup(%d, %s::get%s, %s, %s, %sMeta.META, %s, %s, %s, %q),\n",
				num, ob, field.Pascal, bindRef("set"+field.Pascal, t), fac, t, fin, hasM, hasB, field.Name)
			return
		}
		fmt.Fprintf(b, "                FieldMeta.ofMessage(%d, %s::get%s, %s, %s, %sMeta.META, %s, %s, %s, %s, %q),\n",
			num, ob, field.Pascal, bindRef("set"+field.Pascal, t), fac, t, fin, hasM, hasB, req, field.Name)
		return
	case field.Message != "" && field.Repeated:
		t := javaType(f, field)
		fac := t + ".Builder::new"
		fin := t + ".Builder::buildPartial"
		if field.Kind == protoreflect.GroupKind {
			fmt.Fprintf(b, "                FieldMeta.ofRepGroup(%d, %s::get%sList, %s, %s, %sMeta.META, %s, %q),\n",
				num, ob, field.Pascal, bindRef("add"+field.Pascal, t), fac, t, fin, field.Name)
			return
		}
		fmt.Fprintf(b, "                FieldMeta.ofRepMessage(%d, %s::get%sList, %s, %s, %sMeta.META, %s, %q),\n",
			num, ob, field.Pascal, bindRef("add"+field.Pascal, t), fac, t, fin, field.Name)
		return
	case field.Repeated:
		pack := "false"
		if field.Packed {
			pack = "true"
		}
		switch field.Kind {
		case protoreflect.StringKind:
			fmt.Fprintf(b, "                FieldMeta.ofRepString(%d, %s::get%sList, %s, %q),\n",
				num, ob, field.Pascal, setV("add"+field.Pascal, "String"), field.Name)
		case protoreflect.BytesKind:
			fmt.Fprintf(b, "                FieldMeta.ofRepBytes(%d, %s::get%sList, %s, %q),\n",
				num, ob, field.Pascal, setV("add"+field.Pascal, "byte[]"), field.Name)
		case protoreflect.BoolKind:
			fmt.Fprintf(b, "                FieldMeta.ofRepBool(%d, %s, %s::get%sList, %s, %q),\n",
				num, pack, ob, field.Pascal, setV("add"+field.Pascal, "Boolean"), field.Name)
		case protoreflect.FloatKind:
			fmt.Fprintf(b, "                FieldMeta.ofRepFloat(%d, %s, %s, %s::get%sList, %s, %q),\n",
				num, javaKindInt(field.Kind), pack, ob, field.Pascal, setV("add"+field.Pascal, "Float"), field.Name)
		case protoreflect.DoubleKind:
			fmt.Fprintf(b, "                FieldMeta.ofRepDouble(%d, %s, %s, %s::get%sList, %s, %q),\n",
				num, javaKindInt(field.Kind), pack, ob, field.Pascal, setV("add"+field.Pascal, "double"), field.Name)
		case protoreflect.Int64Kind, protoreflect.Uint64Kind, protoreflect.Sint64Kind,
			protoreflect.Fixed64Kind, protoreflect.Sfixed64Kind:
			fmt.Fprintf(b, "                FieldMeta.ofRepLong(%d, %s, %s, %s::get%sList, %s, %q),\n",
				num, javaKindInt(field.Kind), pack, ob, field.Pascal, setV("add"+field.Pascal, "long"), field.Name)
		default:
			fmt.Fprintf(b, "                FieldMeta.ofRepInt(%d, %s, %s, %s::get%sList, %s, %q),\n",
				num, javaKindInt(field.Kind), pack, ob, field.Pascal, setV("add"+field.Pascal, "int"), field.Name)
		}
		return
	}
	if field.Kind == protoreflect.EnumKind && field.Enum != "" {
		fmt.Fprintf(b, "                FieldMeta.ofInt(%d, Kind.ENUM, %s::get%sValue, %s, %s, %s, %s, %q),\n",
			num, ob, field.Pascal, setV("set"+field.Pascal+"Value", "int"), hasM, hasB, req, field.Name)
		return
	}
	switch field.Kind {
	case protoreflect.StringKind:
		fmt.Fprintf(b, "                FieldMeta.ofString(%d, %s::%s, %s, %s, %s, %s, %q),\n",
			num, ob, javaGetterName(field), setV("set"+field.Pascal, "String"), hasM, hasB, req, field.Name)
	case protoreflect.BytesKind:
		fmt.Fprintf(b, "                FieldMeta.ofBytes(%d, %s::%s, %s, %s, %s, %s, %q),\n",
			num, ob, javaGetterName(field), setV("set"+field.Pascal, "byte[]"), hasM, hasB, req, field.Name)
	case protoreflect.BoolKind:
		fmt.Fprintf(b, "                FieldMeta.ofBool(%d, %s::%s, %s, %s, %s, %s, %q),\n",
			num, ob, javaGetterName(field), setV("set"+field.Pascal, "Boolean"), hasM, hasB, req, field.Name)
	case protoreflect.FloatKind:
		fmt.Fprintf(b, "                FieldMeta.ofFloat(%d, %s::%s, %s, %s, %s, %s, %q),\n",
			num, ob, javaGetterName(field), setV("set"+field.Pascal, "Float"), hasM, hasB, req, field.Name)
	case protoreflect.DoubleKind:
		fmt.Fprintf(b, "                FieldMeta.ofDouble(%d, %s::%s, %s, %s, %s, %s, %q),\n",
			num, ob, javaGetterName(field), setV("set"+field.Pascal, "double"), hasM, hasB, req, field.Name)
	case protoreflect.Int64Kind, protoreflect.Uint64Kind, protoreflect.Sint64Kind,
		protoreflect.Fixed64Kind, protoreflect.Sfixed64Kind:
		fmt.Fprintf(b, "                FieldMeta.ofLong(%d, %s, %s::%s, %s, %s, %s, %s, %q),\n",
			num, javaKindInt(field.Kind), ob, javaGetterName(field), setV("set"+field.Pascal, "long"), hasM, hasB, req, field.Name)
	default:
		fmt.Fprintf(b, "                FieldMeta.ofInt(%d, %s, %s::%s, %s, %s, %s, %s, %q),\n",
			num, javaKindInt(field.Kind), ob, javaGetterName(field), setV("set"+field.Pascal, "int"), hasM, hasB, req, field.Name)
	}
}

// emitOneofMsgRow emits the table row of one oneof message/group member.
// The setter resolves against the passed Builder class like every row.
func emitOneofMsgRow(b *strings.Builder, f *model.File, field model.Field, num int32, caseOf string, ob string, idx int) {
	t := javaType(f, field)
	fac := t + ".Builder::new"
	fin := t + ".Builder::buildPartial"
	if field.Kind == protoreflect.GroupKind {
		fmt.Fprintf(b, "                FieldMeta.ofOneofGroup(%d, %s::get%s, %s, %s, %sMeta.META, %s, %s, %d, %q),\n",
			num, ob, field.Pascal, "Bind.setter(builderClass, \"set"+field.Pascal+"\", "+t+".class)", fac, t, fin, caseOf, idx, field.Name)
		return
	}
	fmt.Fprintf(b, "                FieldMeta.ofOneofMessage(%d, %s::get%s, %s, %s, %sMeta.META, %s, %s, %d, %q),\n",
		num, ob, field.Pascal, "Bind.setter(builderClass, \"set"+field.Pascal+"\", "+t+".class)", fac, t, fin, caseOf, idx, field.Name)
}

// writeJavaBuilder emits the nested Builder class.
func writeJavaBuilder(b *strings.Builder, f *model.File, m model.Message, cls string, oneofs []string) {
	fmt.Fprintf(b, "\n    public static final class Builder implements %sOrBuilder {\n", cls)
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
	b.WriteString("    }\n")
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
	fmt.Fprintf(b, "        public %s %s() {\n            return %s;\n        }\n\n", t, javaGetterName(field), priv)
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
			fmt.Fprintf(b, "        public %s %s() {\n", t, javaGetterName(field))
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

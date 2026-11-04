package gen

import (
	"fmt"
	"strings"

	"pbwire-typed/internal/model"

	"google.golang.org/protobuf/reflect/protoreflect"
)

func csType(field model.Field, prefix string) string {
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
		return "bool"
	case protoreflect.StringKind:
		return "string?"
	case protoreflect.BytesKind:
		return "byte[]?"
	default:
		return prefix + "_" + field.Message + "?"
	}
}

func csNeedsUnsafe(m model.Message) bool {
	for _, f := range m.Fields {
		if f.Kind == protoreflect.BytesKind {
			return true
		}
	}
	return false
}

func csharpFiles(f *model.File) []File {
	var b strings.Builder
	b.WriteString(header(f, "Typed view of every message in the file: assign fields, Encode, Decode."))
	b.WriteString("using System;\nusing System.Collections.Generic;\nusing Pbwire;\n\n")
	b.WriteString("namespace Pbwire.Generated\n{\n")
	for _, e := range f.Enums {
		fmt.Fprintf(&b, "    /// <summary>%s</summary>\n", e.Full)
		fmt.Fprintf(&b, "    public static class %s\n    {\n", className(f, e.Name))
		for _, v := range e.Values {
			fmt.Fprintf(&b, "        public const int %s = %d;\n", v.Name, v.Number)
		}
		b.WriteString("    }\n\n")
	}
	for _, m := range f.Messages {
		cls := className(f, m.Name)
		unsafe := ""
		if csNeedsUnsafe(m) {
			unsafe = "unsafe "
		}
		fmt.Fprintf(&b, "    /// <summary>%s</summary>\n", m.Full)
		fmt.Fprintf(&b, "    public sealed %sclass %s\n    {\n", unsafe, cls)
		for _, field := range m.Fields {
			if field.Skip != "" {
				fmt.Fprintf(&b, "        // skipped %s: %s\n", field.Name, field.Skip)
				continue
			}
			if field.NeedsHas {
				fmt.Fprintf(&b, "        public bool Has%s;\n", field.Pascal)
			}
			if field.Repeated {
				fmt.Fprintf(&b, "        public List<%s> %s = new(); // %s = %d\n", strings.TrimSuffix(csType(field, f.Prefix), "?"), field.Pascal, field.Name, field.Number)
			} else {
				fmt.Fprintf(&b, "        public %s %s; // %s = %d\n", csType(field, f.Prefix), field.Pascal, field.Name, field.Number)
			}
		}
		fmt.Fprintf(&b, "\n        public byte[] Encode(Schema schema)\n        {\n")
		fmt.Fprintf(&b, "            Message msg = schema.Message(%q).NewMessage();\n            EncodeInto(msg);\n            return msg.Encode();\n        }\n\n", m.Full)
		b.WriteString("        public void EncodeInto(Message msg)\n        {\n")
		for _, field := range m.Fields {
			if field.Skip != "" {
				continue
			}
			fmt.Fprintf(&b, "            if (%s)\n            {\n", csPresent(field))
			if field.Repeated {
				elem := csType(field, f.Prefix)
				if field.Message != "" {
					fmt.Fprintf(&b, "                foreach (%s v in %s) v.EncodeInto(msg.SetMessage(%q));\n", elem, field.Pascal, field.Name)
				} else {
					fmt.Fprintf(&b, "                foreach (%s v in %s) msg.Set(%q, %s);\n", elem, field.Pascal, field.Name, csSetArg(field, "v"))
				}
			} else if field.Message != "" {
				fmt.Fprintf(&b, "                %s.EncodeInto(msg.SetMessage(%q));\n", field.Pascal, field.Name)
			} else {
				fmt.Fprintf(&b, "                msg.Set(%q, %s);\n", field.Name, csSetArg(field, field.Pascal))
			}
			b.WriteString("            }\n")
		}
		b.WriteString("        }\n\n")
		fmt.Fprintf(&b, "        public static %s Decode(Schema schema, ReadOnlySpan<byte> wire)\n", cls)
		fmt.Fprintf(&b, "            => Decode(schema.Message(%q).Decode(wire));\n\n", m.Full)
		fmt.Fprintf(&b, "        public static %s Decode(Message msg)\n        {\n            var o = new %s();\n", cls, cls)
		for _, field := range m.Fields {
			if field.Skip != "" {
				continue
			}
			writeCsRead(&b, f, field)
		}
		b.WriteString("            return o;\n        }\n")
		b.WriteString("    }\n\n")
	}
	b.WriteString("}\n")
	return []File{{Name: base(f) + ".cs", Content: b.String()}}
}

func csPresent(field model.Field) string {
	if field.Required {
		return "true"
	}
	switch {
	case field.Repeated:
		return field.Pascal + ".Count != 0"
	case field.Message != "":
		return field.Pascal + " != null"
	case field.NeedsHas:
		return "Has" + field.Pascal
	case field.Kind == protoreflect.BoolKind:
		return field.Pascal
	case field.Kind == protoreflect.StringKind:
		return field.Pascal + " != null"
	case field.Kind == protoreflect.BytesKind:
		return field.Pascal + " != null && " + field.Pascal + ".Length != 0"
	default:
		return field.Pascal + " != 0"
	}
}

func csSetArg(field model.Field, expr string) string {
	switch field.Kind {
	case protoreflect.Int32Kind, protoreflect.Sint32Kind, protoreflect.Sfixed32Kind,
		protoreflect.Uint32Kind, protoreflect.Fixed32Kind, protoreflect.EnumKind:
		return "(long)" + expr
	case protoreflect.StringKind:
		return expr + "!"
	case protoreflect.BytesKind:
		return expr + "!"
	default:
		return expr
	}
}

func writeCsRead(b *strings.Builder, f *model.File, field model.Field) {
	if field.Repeated {
		var value string
		switch field.Kind {
		case protoreflect.StringKind:
			value = "v.AsString()!"
		case protoreflect.BytesKind:
			value = "v.AsBytes()!"
		case protoreflect.BoolKind:
			value = "v.AsBool()"
		case protoreflect.FloatKind:
			value = "v.AsFloat()"
		case protoreflect.DoubleKind:
			value = "v.AsDouble()"
		case protoreflect.Int64Kind, protoreflect.Sint64Kind, protoreflect.Sfixed64Kind,
			protoreflect.Uint64Kind, protoreflect.Fixed64Kind:
			value = "v.AsLong()"
		default:
			value = "v.AsInt()"
		}
		if field.Message != "" {
			fmt.Fprintf(b, "            for (int i = 0; i < msg.Count(%q); i++)\n", field.Name)
			fmt.Fprintf(b, "            {\n                var v = msg.At(%q, i);\n", field.Name)
			fmt.Fprintf(b, "                if (v != null && v.AsMessage() != null) o.%s.Add(%s_%s.Decode(v.AsMessage()!));\n            }\n", field.Pascal, f.Prefix, field.Message)
			return
		}
		fmt.Fprintf(b, "            for (int i = 0; i < msg.Count(%q); i++)\n", field.Name)
		fmt.Fprintf(b, "            {\n                var v = msg.At(%q, i);\n", field.Name)
		fmt.Fprintf(b, "                if (v != null) o.%s.Add(%s);\n            }\n", field.Pascal, value)
		return
	}
	if field.Message != "" {
		fmt.Fprintf(b, "            o.%s = msg.GetMessage(%q) is Message child%s ? %s_%s.Decode(child%s) : null;\n", field.Pascal, field.Name, field.Pascal, f.Prefix, field.Message, field.Pascal)
		return
	}
	var read string
	switch field.Kind {
	case protoreflect.Int32Kind, protoreflect.Sint32Kind, protoreflect.Sfixed32Kind,
		protoreflect.Uint32Kind, protoreflect.Fixed32Kind, protoreflect.EnumKind:
		read = fmt.Sprintf("msg.GetInt(%q)", field.Name)
	case protoreflect.Int64Kind, protoreflect.Sint64Kind, protoreflect.Sfixed64Kind,
		protoreflect.Uint64Kind, protoreflect.Fixed64Kind:
		read = fmt.Sprintf("msg.GetLong(%q)", field.Name)
	case protoreflect.FloatKind:
		read = fmt.Sprintf("msg.GetFloat(%q)", field.Name)
	case protoreflect.DoubleKind:
		read = fmt.Sprintf("msg.GetDouble(%q)", field.Name)
	case protoreflect.BoolKind:
		read = fmt.Sprintf("msg.GetBool(%q)", field.Name)
	case protoreflect.StringKind:
		read = fmt.Sprintf("msg.GetString(%q)", field.Name)
	case protoreflect.BytesKind:
		read = fmt.Sprintf("msg.GetBytes(%q)", field.Name)
	}
	if field.NeedsHas {
		fmt.Fprintf(b, "            o.Has%s = msg.Has(%q);\n", field.Pascal, field.Name)
		fmt.Fprintf(b, "            if (o.Has%s) o.%s = %s;\n", field.Pascal, field.Pascal, read)
		return
	}
	fmt.Fprintf(b, "            o.%s = %s;\n", field.Pascal, read)
}

package gen

import (
	"fmt"
	"strings"

	"pbwire-typed/internal/model"

	"google.golang.org/protobuf/reflect/protoreflect"
)

var dartKeywords = map[string]bool{
	"abstract": true, "as": true, "assert": true, "async": true, "await": true,
	"base": true, "break": true, "case": true, "catch": true, "class": true,
	"const": true, "continue": true, "covariant": true, "default": true,
	"deferred": true, "do": true, "dynamic": true, "else": true, "enum": true,
	"export": true, "extends": true, "extension": true, "external": true,
	"factory": true, "false": true, "final": true, "finally": true, "for": true,
	"function": true, "get": true, "hide": true, "if": true, "implements": true,
	"import": true, "in": true, "interface": true, "is": true, "late": true,
	"library": true, "mixin": true, "new": true, "null": true, "on": true,
	"operator": true, "part": true, "required": true, "rethrow": true,
	"return": true, "sealed": true, "set": true, "show": true, "static": true,
	"super": true, "switch": true, "sync": true, "this": true, "throw": true,
	"true": true, "try": true, "type": true, "typedef": true, "var": true,
	"void": true, "when": true, "while": true, "with": true, "yield": true,
}

func dartName(name string) string {
	if dartKeywords[name] {
		return name + "_"
	}
	return name
}

func dartType(field model.Field) string {
	switch field.Kind {
	case protoreflect.Int32Kind, protoreflect.Sint32Kind, protoreflect.Sfixed32Kind,
		protoreflect.Uint32Kind, protoreflect.Fixed32Kind,
		protoreflect.Int64Kind, protoreflect.Sint64Kind, protoreflect.Sfixed64Kind,
		protoreflect.Uint64Kind, protoreflect.Fixed64Kind, protoreflect.EnumKind:
		return "int"
	case protoreflect.FloatKind, protoreflect.DoubleKind:
		return "double"
	case protoreflect.BoolKind:
		return "bool"
	case protoreflect.StringKind:
		return "String"
	case protoreflect.BytesKind:
		return "Uint8List"
	default:
		return field.Message
	}
}

func dartFiles(f *model.File) []File {
	var b strings.Builder
	b.WriteString(header(f, "Typed classes: assign fields, encode, decode. Generated once, no runtime reflection."))
	b.WriteString("import 'dart:typed_data';\n\nimport 'package:pbwire/pbwire.dart';\n\n")
	for _, e := range f.Enums {
		fmt.Fprintf(&b, "/// %s\n", e.Full)
		fmt.Fprintf(&b, "abstract final class %s {\n", e.Name)
		for _, v := range e.Values {
			fmt.Fprintf(&b, "  static const int %s = %d;\n", v.Name, v.Number)
		}
		b.WriteString("}\n\n")
	}
	for _, m := range f.Messages {
		fmt.Fprintf(&b, "/// %s\nclass %s {\n", m.Full, m.Name)
		for _, field := range m.Fields {
			name := dartName(field.Lower)
			if field.Skip != "" {
				fmt.Fprintf(&b, "  // skipped %s: %s\n", field.Name, field.Skip)
				continue
			}
			if field.NeedsHas {
				fmt.Fprintf(&b, "  bool has%s = false;\n", field.Pascal)
			}
			if field.Repeated {
				fmt.Fprintf(&b, "  List<%s> %s = []; // %s = %d\n", dartType(field), name, field.Name, field.Number)
			} else if field.Message != "" {
				fmt.Fprintf(&b, "  %s? %s; // %s = %d\n", dartType(field), name, field.Name, field.Number)
			} else if field.Kind == protoreflect.BytesKind {
				fmt.Fprintf(&b, "  Uint8List? %s; // %s = %d\n", name, field.Name, field.Number)
			} else {
				fmt.Fprintf(&b, "  %s %s = %s; // %s = %d\n", dartType(field), name, dartZero(field), field.Name, field.Number)
			}
		}
		fmt.Fprintf(&b, "\n  Uint8List encode(Schema schema) {\n")
		fmt.Fprintf(&b, "    final msg = schema.message(%q).newMessage();\n    encodeInto(msg);\n    return msg.encode();\n  }\n\n", m.Full)
		b.WriteString("  void encodeInto(Message msg) {\n")
		for _, field := range m.Fields {
			if field.Skip != "" {
				continue
			}
			name := dartName(field.Lower)
			fmt.Fprintf(&b, "    if (%s) {\n", dartPresent(field, name))
			if field.Repeated {
				fmt.Fprintf(&b, "      for (final v in %s) {\n", name)
				if field.Message != "" {
					fmt.Fprintf(&b, "        v.encodeInto(msg.setMessage(%q));\n", field.Name)
				} else {
					fmt.Fprintf(&b, "        msg.%s(%q, v);\n", dartSetter(field), field.Name)
				}
				b.WriteString("      }\n")
			} else if field.Message != "" {
				fmt.Fprintf(&b, "      %s!.encodeInto(msg.setMessage(%q));\n", name, field.Name)
			} else {
				arg := name
				if field.Kind == protoreflect.BytesKind {
					arg = name + "!"
				}
				fmt.Fprintf(&b, "      msg.%s(%q, %s);\n", dartSetter(field), field.Name, arg)
			}
			b.WriteString("    }\n")
		}
		b.WriteString("  }\n\n")
		fmt.Fprintf(&b, "  static %s decode(Schema schema, List<int> wire) =>\n", m.Name)
		fmt.Fprintf(&b, "      fromMessage(schema.message(%q).decode(wire));\n\n", m.Full)
		fmt.Fprintf(&b, "  static %s fromMessage(Message msg) {\n    final o = %s();\n", m.Name, m.Name)
		for _, field := range m.Fields {
			if field.Skip != "" {
				continue
			}
			writeDartRead(&b, field)
		}
		b.WriteString("    return o;\n  }\n}\n\n")
	}
	return []File{{Name: base(f) + ".dart", Content: b.String()}}
}

func dartZero(field model.Field) string {
	switch field.Kind {
	case protoreflect.BoolKind:
		return "false"
	case protoreflect.StringKind:
		return "''"
	default:
		return "0"
	}
}

func dartPresent(field model.Field, name string) string {
	if field.Required {
		return "true"
	}
	switch {
	case field.Repeated:
		return name + ".isNotEmpty"
	case field.Message != "":
		return name + " != null"
	case field.NeedsHas:
		if field.Kind == protoreflect.BytesKind {
			return "has" + field.Pascal + " && " + name + " != null"
		}
		return "has" + field.Pascal
	case field.Kind == protoreflect.BoolKind:
		return name
	case field.Kind == protoreflect.StringKind:
		return name + ".isNotEmpty"
	case field.Kind == protoreflect.BytesKind:
		return name + " != null && " + name + "!.isNotEmpty"
	default:
		return name + " != 0"
	}
}

func dartSetter(field model.Field) string {
	switch field.Kind {
	case protoreflect.FloatKind:
		return "setFloat"
	case protoreflect.DoubleKind:
		return "setDouble"
	case protoreflect.BoolKind:
		return "setBool"
	case protoreflect.StringKind:
		return "setString"
	case protoreflect.BytesKind:
		return "setBytes"
	default:
		return "setInt"
	}
}

func writeDartRead(b *strings.Builder, field model.Field) {
	name := "o." + dartName(field.Lower)
	if field.Repeated {
		fmt.Fprintf(b, "    for (int i = 0; i < msg.count(%q); i++) {\n", field.Name)
		if field.Message != "" {
			fmt.Fprintf(b, "      final v = msg.at(%q, i);\n      if (v != null && v.asMessage() != null) %s.add(%s.fromMessage(v.asMessage()!));\n", field.Name, name, field.Message)
		} else {
			fmt.Fprintf(b, "      final v = msg.at(%q, i);\n      if (v != null) %s.add(%s);\n", field.Name, name, dartElem(field, "v"))
		}
		b.WriteString("    }\n")
		return
	}
	if field.Message != "" {
		fmt.Fprintf(b, "    final child%s = msg.getMessage(%q);\n    if (child%s != null) %s = %s.fromMessage(child%s);\n", field.Pascal, field.Name, field.Pascal, name, field.Message, field.Pascal)
		return
	}
	read := dartValue(field, "msg."+dartGetter(field)+"("+fmt.Sprintf("%q", field.Name)+")")
	if dartSigned32(field) {
		read += ".toSigned(32)"
	}
	if field.NeedsHas {
		fmt.Fprintf(b, "    o.has%s = msg.has(%q);\n    if (o.has%s) { %s = %s; }\n", field.Pascal, field.Name, field.Pascal, name, read)
		return
	}
	fmt.Fprintf(b, "    %s = %s;\n", name, read)
}

// dartElem converts one repeated Value to the Dart element type.
// dartSigned32 reports whether a decoded value arrives as an unsigned 64-bit
// int for a signed 32-bit field: Dart has no int32, so the typed layer clamps.
func dartSigned32(field model.Field) bool {
	switch field.Kind {
	case protoreflect.Int32Kind, protoreflect.Sint32Kind, protoreflect.Sfixed32Kind, protoreflect.EnumKind:
		return true
	default:
		return false
	}
}

func dartElem(field model.Field, expr string) string {
	if dartSigned32(field) {
		return expr + ".asInt().toSigned(32)"
	}
	switch field.Kind {
	case protoreflect.StringKind:
		return expr + ".asString()!"
	case protoreflect.BytesKind:
		return expr + ".asBytes()!"
	case protoreflect.BoolKind:
		return expr + ".asBool()"
	case protoreflect.FloatKind:
		return expr + ".asFloat()"
	case protoreflect.DoubleKind:
		return expr + ".asDouble()"
	default:
		return expr + ".asInt()"
	}
}

func dartGetter(field model.Field) string {
	switch field.Kind {
	case protoreflect.FloatKind:
		return "getFloat"
	case protoreflect.DoubleKind:
		return "getDouble"
	case protoreflect.BoolKind:
		return "getBool"
	case protoreflect.StringKind:
		return "getString"
	case protoreflect.BytesKind:
		return "getBytes"
	default:
		return "getInt"
	}
}

func dartValue(field model.Field, expr string) string {
	switch field.Kind {
	case protoreflect.StringKind:
		return expr + " ?? ''"
	case protoreflect.BytesKind:
		return expr
	default:
		return expr
	}
}

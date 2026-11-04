package gen

import (
	"fmt"
	"strings"

	"pbwire-typed/internal/model"

	"google.golang.org/protobuf/reflect/protoreflect"
)

var kotlinKeywords = map[string]bool{
	"as": true, "break": true, "class": true, "continue": true, "do": true,
	"else": true, "false": true, "for": true, "fun": true, "if": true, "in": true,
	"interface": true, "is": true, "null": true, "object": true, "package": true,
	"return": true, "super": true, "this": true, "throw": true, "true": true,
	"try": true, "typealias": true, "typeof": true, "val": true, "var": true,
	"when": true, "while": true,
}

func kotlinName(name string) string {
	if kotlinKeywords[name] {
		return name + "_"
	}
	return name
}

func kotlinType(field model.Field, prefix string) string {
	switch field.Kind {
	case protoreflect.Int32Kind, protoreflect.Sint32Kind, protoreflect.Sfixed32Kind,
		protoreflect.Uint32Kind, protoreflect.Fixed32Kind, protoreflect.EnumKind:
		return "Int"
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
		return "String?"
	case protoreflect.BytesKind:
		return "ByteArray?"
	default:
		return prefix + "_" + field.Message + "?"
	}
}

func kotlinFiles(f *model.File) []File {
	var b strings.Builder
	b.WriteString(header(f, "Typed classes over the Java runtime classes: assign fields, encode, decode."))
	b.WriteString("@file:Suppress(\"unused\", \"MemberVisibilityCanBePrivate\")\n\n")
	for _, e := range f.Enums {
		fmt.Fprintf(&b, "/** %s */\nobject %s {\n", e.Full, className(f, e.Name))
		for _, v := range e.Values {
			fmt.Fprintf(&b, "    const val %s: Int = %d\n", v.Name, v.Number)
		}
		b.WriteString("}\n\n")
	}
	for _, m := range f.Messages {
		cls := className(f, m.Name)
		fmt.Fprintf(&b, "/** %s */\n", m.Full)
		var params []string
		for _, field := range m.Fields {
			if field.Skip != "" {
				continue
			}
			name := kotlinName(field.Lower)
			if field.NeedsHas {
				params = append(params, fmt.Sprintf("var has%s: Boolean = false", field.Pascal))
			}
			if field.Repeated {
				params = append(params, fmt.Sprintf("var %s: MutableList<%s> = mutableListOf()", name, strings.TrimSuffix(kotlinType(field, f.Prefix), "?")))
			} else {
				params = append(params, fmt.Sprintf("var %s: %s = %s", name, kotlinType(field, f.Prefix), kotlinZero(field)))
			}
		}
		if len(params) == 0 {
			fmt.Fprintf(&b, "class %s {\n", cls)
		} else {
			fmt.Fprintf(&b, "data class %s(\n", cls)
			b.WriteString("    " + strings.Join(params, ",\n    ") + ",\n")
			b.WriteString(") {\n")
		}
		fmt.Fprintf(&b, "    fun encode(schema: Schema): ByteArray {\n")
		fmt.Fprintf(&b, "        val msg = schema.message(%q).newMessage()\n        encodeInto(msg)\n        return msg.encode()\n    }\n\n", m.Full)
		b.WriteString("    fun encodeInto(msg: Message) {\n")
		for _, field := range m.Fields {
			if field.Skip != "" {
				continue
			}
			name := kotlinName(field.Lower)
			fmt.Fprintf(&b, "        if (%s) {\n", kotlinPresent(field, name))
			if field.Repeated {
				fmt.Fprintf(&b, "            for (v in %s) {\n", name)
				if field.Message != "" {
					fmt.Fprintf(&b, "                v.encodeInto(msg.newMessage(%q))\n", field.Name)
				} else {
					fmt.Fprintf(&b, "                msg.set(%q, %s)\n", field.Name, kotlinSetArg(field, "v"))
				}
				b.WriteString("            }\n")
			} else if field.Message != "" {
				fmt.Fprintf(&b, "            %s?.encodeInto(msg.newMessage(%q))\n", name, field.Name)
			} else {
				fmt.Fprintf(&b, "            msg.set(%q, %s)\n", field.Name, kotlinSetArg(field, name))
			}
			b.WriteString("        }\n")
		}
		b.WriteString("    }\n\n")
		b.WriteString("    companion object {\n")
		fmt.Fprintf(&b, "        @JvmStatic\n        fun decode(schema: Schema, wire: ByteArray): %s = fromMessage(schema.message(%q).decode(wire))\n\n", cls, m.Full)
		fmt.Fprintf(&b, "        @JvmStatic\n        fun fromMessage(msg: Message): %s {\n            val o = %s()\n", cls, cls)
		for _, field := range m.Fields {
			if field.Skip != "" {
				continue
			}
			writeKotlinRead(&b, f, field)
		}
		b.WriteString("            return o\n        }\n    }\n}\n\n")
	}
	return []File{{Name: base(f) + ".kt", Content: b.String()}}
}

func kotlinZero(field model.Field) string {
	switch field.Kind {
	case protoreflect.StringKind, protoreflect.BytesKind:
		return "null"
	case protoreflect.BoolKind:
		return "false"
	case protoreflect.FloatKind:
		return "0f"
	case protoreflect.DoubleKind:
		return "0.0"
	default:
		if field.Message != "" {
			return "null"
		}
		return "0"
	}
}

func kotlinPresent(field model.Field, name string) string {
	if field.Required {
		return "true"
	}
	switch {
	case field.Repeated:
		return name + ".isNotEmpty()"
	case field.Message != "":
		return name + " != null"
	case field.NeedsHas:
		if field.Kind == protoreflect.StringKind || field.Kind == protoreflect.BytesKind {
			return "has" + field.Pascal + " && " + name + " != null"
		}
		return "has" + field.Pascal
	case field.Kind == protoreflect.BoolKind:
		return name
	case field.Kind == protoreflect.StringKind:
		return "!" + name + ".isNullOrEmpty()"
	case field.Kind == protoreflect.BytesKind:
		return name + "?.isNotEmpty() == true"
	default:
		return name + " != " + kotlinZeroLit(field)
	}
}

// kotlinZeroLit is the typed zero: Kotlin refuses to compare an Int with a
// Float/Long literal.
func kotlinZeroLit(field model.Field) string {
	switch field.Kind {
	case protoreflect.Int64Kind, protoreflect.Sint64Kind, protoreflect.Sfixed64Kind,
		protoreflect.Uint64Kind, protoreflect.Fixed64Kind:
		return "0L"
	case protoreflect.FloatKind:
		return "0f"
	case protoreflect.DoubleKind:
		return "0.0"
	default:
		return "0"
	}
}

func kotlinSetArg(field model.Field, expr string) string {
	switch field.Kind {
	case protoreflect.Int32Kind, protoreflect.Sint32Kind, protoreflect.Sfixed32Kind,
		protoreflect.Uint32Kind, protoreflect.Fixed32Kind, protoreflect.EnumKind:
		return expr + ".toLong()"
	case protoreflect.FloatKind:
		return expr + ".toDouble()"
	case protoreflect.StringKind, protoreflect.BytesKind:
		return expr + "!!"
	default:
		return expr
	}
}

func writeKotlinRead(b *strings.Builder, f *model.File, field model.Field) {
	name := "o." + kotlinName(field.Lower)
	if field.Repeated {
		list := fmt.Sprintf("msg.getList(%q)", field.Name)
		if field.Message != "" {
			fmt.Fprintf(b, "            %s = %s.mapTo(mutableListOf()) { %s.fromMessage(it as Message) }\n", name, list, className(f, field.Message))
			return
		}
		var conv string
		switch field.Kind {
		case protoreflect.StringKind:
			conv = "it as String"
		case protoreflect.BytesKind:
			conv = "it as ByteArray"
		case protoreflect.BoolKind:
			conv = "it as Boolean"
		case protoreflect.FloatKind:
			conv = "(it as Number).toFloat()"
		case protoreflect.DoubleKind:
			conv = "(it as Number).toDouble()"
		case protoreflect.Int64Kind, protoreflect.Sint64Kind, protoreflect.Sfixed64Kind,
			protoreflect.Uint64Kind, protoreflect.Fixed64Kind:
			conv = "(it as Number).toLong()"
		default:
			conv = "(it as Number).toInt()"
		}
		fmt.Fprintf(b, "            %s = %s.mapTo(mutableListOf()) { %s }\n", name, list, conv)
		return
	}
	if field.Message != "" {
		fmt.Fprintf(b, "            %s = msg.getMessage(%q)?.let { %s.fromMessage(it) }\n", name, field.Name, className(f, field.Message))
		return
	}
	var read string
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
		fmt.Fprintf(b, "            o.has%s = msg.has(%q)\n", field.Pascal, field.Name)
		fmt.Fprintf(b, "            if (o.has%s) o.%s = %s\n", field.Pascal, kotlinName(field.Lower), read)
		return
	}
	fmt.Fprintf(b, "            %s = %s\n", name, read)
}

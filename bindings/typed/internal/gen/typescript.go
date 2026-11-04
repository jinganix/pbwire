package gen

import (
	"fmt"
	"strings"

	"pbwire-typed/internal/model"

	"google.golang.org/protobuf/reflect/protoreflect"
)

func tsType(field model.Field) string {
	base := ""
	switch field.Kind {
	case protoreflect.Int32Kind, protoreflect.Sint32Kind, protoreflect.Sfixed32Kind,
		protoreflect.Uint32Kind, protoreflect.Fixed32Kind, protoreflect.EnumKind:
		base = "number"
	case protoreflect.Int64Kind, protoreflect.Sint64Kind, protoreflect.Sfixed64Kind,
		protoreflect.Uint64Kind, protoreflect.Fixed64Kind:
		base = "number | bigint"
	case protoreflect.FloatKind, protoreflect.DoubleKind:
		base = "number"
	case protoreflect.BoolKind:
		base = "boolean"
	case protoreflect.StringKind:
		base = "string"
	case protoreflect.BytesKind:
		base = "Uint8Array"
	default:
		base = field.Message
	}
	if field.Repeated {
		return base + "[]"
	}
	if field.Message != "" {
		return base + " | null"
	}
	return base
}

// typeScriptFiles emits a .d.ts: interfaces for the plain objects the Node
// binding accepts and returns, plus one type alias per enum.
func typeScriptFiles(f *model.File) []File {
	var b strings.Builder
	b.WriteString(header(f, "TypeScript declarations for the objects the Node binding accepts and returns."))
	for _, e := range f.Enums {
		fmt.Fprintf(&b, "/** %s */\nexport declare const %s: {\n", e.Full, className(f, e.Name))
		for _, v := range e.Values {
			fmt.Fprintf(&b, "  readonly %s: %d;\n", v.Name, v.Number)
		}
		b.WriteString("};\n\n")
	}
	for _, m := range f.Messages {
		fmt.Fprintf(&b, "/** %s */\nexport interface %s {\n", m.Full, m.Name)
		for _, field := range m.Fields {
			if field.Skip != "" {
				fmt.Fprintf(&b, "  // skipped %s: %s\n", field.Name, field.Skip)
				continue
			}
			optional := "?"
			fmt.Fprintf(&b, "  %s%s: %s;\n", field.Name, optional, tsType(field))
		}
		b.WriteString("}\n\n")
	}
	return []File{{Name: base(f) + ".d.ts", Content: b.String()}}
}

// javaScriptFiles emits a .js carrying the same shapes as JSDoc typedefs (for
// editors and // @ts-check) plus the enum constants as runtime objects.
func javaScriptFiles(f *model.File) []File {
	var b strings.Builder
	b.WriteString(header(f, "JSDoc types for the plain objects the Node binding accepts and returns."))
	b.WriteString("'use strict';\n\n")
	for _, e := range f.Enums {
		fmt.Fprintf(&b, "/**\n * @enum {number}\n */\nconst %s = Object.freeze({\n", className(f, e.Name))
		for _, v := range e.Values {
			fmt.Fprintf(&b, "  %s: %d,\n", v.Name, v.Number)
		}
		b.WriteString("});\n\n")
	}
	for _, m := range f.Messages {
		fmt.Fprintf(&b, "/**\n * @typedef {Object} %s %s\n", m.Name, m.Full)
		for _, field := range m.Fields {
			if field.Skip != "" {
				continue
			}
			fmt.Fprintf(&b, " * @property {%s} [%s]\n", jsType(field), field.Name)
		}
		b.WriteString(" */\n\n")
	}
	b.WriteString("module.exports = {\n")
	for _, e := range f.Enums {
		fmt.Fprintf(&b, "  %s,\n", className(f, e.Name))
	}
	b.WriteString("};\n")
	return []File{{Name: base(f) + ".js", Content: b.String()}}
}

func jsType(field model.Field) string {
	switch field.Kind {
	case protoreflect.Int64Kind, protoreflect.Sint64Kind, protoreflect.Sfixed64Kind,
		protoreflect.Uint64Kind, protoreflect.Fixed64Kind:
		if field.Repeated {
			return "Array<number|bigint>"
		}
		return "number|bigint"
	case protoreflect.StringKind:
		if field.Repeated {
			return "string[]"
		}
		return "string"
	case protoreflect.BytesKind:
		if field.Repeated {
			return "Uint8Array[]"
		}
		return "Uint8Array"
	case protoreflect.BoolKind:
		if field.Repeated {
			return "boolean[]"
		}
		return "boolean"
	case protoreflect.FloatKind, protoreflect.DoubleKind,
		protoreflect.Int32Kind, protoreflect.Sint32Kind, protoreflect.Sfixed32Kind,
		protoreflect.Uint32Kind, protoreflect.Fixed32Kind, protoreflect.EnumKind:
		if field.Repeated {
			return "number[]"
		}
		return "number"
	default:
		if field.Repeated {
			return field.Message + "[]"
		}
		return "Object"
	}
}

package gen

import (
	"fmt"
	"strings"

	"pbwire-typed/internal/model"

	"google.golang.org/protobuf/reflect/protoreflect"
)

var phpKeywords = map[string]bool{
	"abstract": true, "and": true, "array": true, "as": true, "break": true,
	"callable": true, "case": true, "catch": true, "class": true, "clone": true,
	"const": true, "continue": true, "declare": true, "default": true, "do": true,
	"echo": true, "else": true, "elseif": true, "empty": true, "enddeclare": true,
	"endfor": true, "endforeach": true, "endif": true, "endswitch": true,
	"endwhile": true, "enum": true, "eval": true, "exit": true, "extends": true,
	"final": true, "finally": true, "fn": true, "for": true, "foreach": true,
	"function": true, "global": true, "goto": true, "if": true, "implements": true,
	"include": true, "instanceof": true, "insteadof": true, "interface": true,
	"isset": true, "list": true, "match": true, "namespace": true, "new": true,
	"or": true, "print": true, "private": true, "protected": true, "public": true,
	"readonly": true, "require": true, "return": true, "static": true,
	"switch": true, "throw": true, "trait": true, "try": true, "unset": true,
	"use": true, "var": true, "while": true, "xor": true, "yield": true,
	"int": true, "float": true, "bool": true, "string": true, "void": true,
	"iterable": true, "object": true, "mixed": true, "never": true, "null": true,
	"false": true, "true": true, "self": true, "parent": true,
}

func phpName(name string) string {
	if phpKeywords[name] {
		return name + "_"
	}
	return name
}

func phpType(field model.Field, prefix string) string {
	switch field.Kind {
	case protoreflect.Int32Kind, protoreflect.Sint32Kind, protoreflect.Sfixed32Kind,
		protoreflect.Uint32Kind, protoreflect.Fixed32Kind,
		protoreflect.Int64Kind, protoreflect.Sint64Kind, protoreflect.Sfixed64Kind,
		protoreflect.Uint64Kind, protoreflect.Fixed64Kind, protoreflect.EnumKind:
		return "int"
	case protoreflect.FloatKind, protoreflect.DoubleKind:
		return "float"
	case protoreflect.BoolKind:
		return "bool"
	case protoreflect.StringKind, protoreflect.BytesKind:
		return "string"
	default:
		return "?" + prefix + "_" + field.Message
	}
}

func phpFiles(f *model.File) []File {
	var b strings.Builder
	b.WriteString("<?php\n\ndeclare(strict_types=1);\n\n")
	b.WriteString(header(f, "Typed classes over the dynamic Pbwire runtime: typed properties, one encode/decode."))
	b.WriteString("namespace Pbwire\\Gen;\n\nuse Pbwire\\Message;\nuse Pbwire\\Schema;\n\n")
	for _, e := range f.Enums {
		fmt.Fprintf(&b, "/** %s */\nfinal class %s\n{\n", e.Full, className(f, e.Name))
		for _, v := range e.Values {
			fmt.Fprintf(&b, "    public const %s = %d;\n", v.Name, v.Number)
		}
		b.WriteString("}\n\n")
	}
	for _, m := range f.Messages {
		cls := className(f, m.Name)
		fmt.Fprintf(&b, "/** %s */\nfinal class %s\n{\n", m.Full, cls)
		if phpNeedsI32(m) {
			b.WriteString("    /** A decoded 32-bit signed field arrives as unsigned 64-bit bits. */\n")
			b.WriteString("    private static function i32(int $v): int\n    {\n        return ($v & 0xFFFFFFFF) - (($v & 0x80000000) << 1);\n    }\n\n")
		}
		for _, field := range m.Fields {
			if field.Skip != "" {
				fmt.Fprintf(&b, "    // skipped %s: %s\n", field.Name, field.Skip)
				continue
			}
			name := phpName(field.Lower)
			if field.NeedsHas {
				fmt.Fprintf(&b, "    public bool $has%s = false;\n", field.Pascal)
			}
			if field.Repeated {
				fmt.Fprintf(&b, "    /** @var list<%s> */\n    public array $%s = []; // %s = %d\n", strings.TrimPrefix(phpType(field, f.Prefix), "?"), name, field.Name, field.Number)
			} else if field.Message != "" {
				fmt.Fprintf(&b, "    public %s $%s = null; // %s = %d\n", phpType(field, f.Prefix), name, field.Name, field.Number)
			} else if field.Kind == protoreflect.StringKind || field.Kind == protoreflect.BytesKind {
				fmt.Fprintf(&b, "    public string $%s = ''; // %s = %d\n", name, field.Name, field.Number)
			} else {
				fmt.Fprintf(&b, "    public %s $%s = %s; // %s = %d\n", phpType(field, f.Prefix), name, phpZero(field), field.Name, field.Number)
			}
		}
		fmt.Fprintf(&b, "\n    public function encode(Schema $schema): string\n    {\n")
		fmt.Fprintf(&b, "        $msg = $schema->message(%q)->newMessage();\n        $this->encodeInto($msg);\n        return $msg->encode();\n    }\n\n", m.Full)
		b.WriteString("    public function encodeInto(Message $msg): void\n    {\n")
		for _, field := range m.Fields {
			if field.Skip != "" {
				continue
			}
			name := "$this->" + phpName(field.Lower)
			fmt.Fprintf(&b, "        if (%s) {\n", phpPresent(field, name))
			if field.Repeated {
				fmt.Fprintf(&b, "            foreach (%s as $v) {\n", name)
				if field.Message != "" {
					fmt.Fprintf(&b, "                $v->encodeInto($msg->setMessage(%q));\n", field.Name)
				} else {
					fmt.Fprintf(&b, "                $msg->%s(%q, $v);\n", phpSetter(field), field.Name)
				}
				b.WriteString("            }\n")
			} else if field.Message != "" {
				fmt.Fprintf(&b, "            %s->encodeInto($msg->setMessage(%q));\n", name, field.Name)
			} else {
				fmt.Fprintf(&b, "            $msg->%s(%q, %s);\n", phpSetter(field), field.Name, name)
			}
			b.WriteString("        }\n")
		}
		b.WriteString("    }\n\n")
		fmt.Fprintf(&b, "    public static function decode(Schema $schema, string $wire): self\n    {\n")
		fmt.Fprintf(&b, "        return self::fromMessage($schema->message(%q)->decode($wire));\n    }\n\n", m.Full)
		fmt.Fprintf(&b, "    public static function fromMessage(Message $msg): self\n    {\n        $o = new self();\n")
		for _, field := range m.Fields {
			if field.Skip != "" {
				continue
			}
			writePhpRead(&b, f, field)
		}
		b.WriteString("        return $o;\n    }\n}\n\n")
	}
	return []File{{Name: base(f) + ".php", Content: b.String()}}
}

func phpSigned32(field model.Field) bool {
	switch field.Kind {
	case protoreflect.Int32Kind, protoreflect.Sint32Kind, protoreflect.Sfixed32Kind, protoreflect.EnumKind:
		return true
	default:
		return false
	}
}

func phpNeedsI32(m model.Message) bool {
	for _, field := range m.Fields {
		if field.Skip == "" && phpSigned32(field) {
			return true
		}
	}
	return false
}

func phpZero(field model.Field) string {
	switch field.Kind {
	case protoreflect.BoolKind:
		return "false"
	case protoreflect.StringKind, protoreflect.BytesKind:
		return "''"
	default:
		return "0"
	}
}

func phpPresent(field model.Field, name string) string {
	if field.Required {
		return "true"
	}
	switch {
	case field.Repeated:
		return name + " !== []"
	case field.Message != "":
		return name + " !== null"
	case field.NeedsHas:
		return "$this->has" + field.Pascal
	case field.Kind == protoreflect.BoolKind:
		return name
	case field.Kind == protoreflect.StringKind || field.Kind == protoreflect.BytesKind:
		return name + " !== ''"
	default:
		return name + " != 0"
	}
}

func phpSetter(field model.Field) string {
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

func writePhpRead(b *strings.Builder, f *model.File, field model.Field) {
	name := "$o->" + phpName(field.Lower)
	if field.Repeated {
		fmt.Fprintf(b, "        for ($i = 0; $i < $msg->count(%q); $i++) {\n", field.Name)
		fmt.Fprintf(b, "            $v = $msg->at(%q, $i);\n", field.Name)
		fmt.Fprintf(b, "            if ($v === null) { continue; }\n")
		if field.Message != "" {
			fmt.Fprintf(b, "            $child = $v->asMessage();\n            if ($child !== null) { %s[] = %s::fromMessage($child); }\n", name, className(f, field.Message))
		} else if phpSigned32(field) {
			fmt.Fprintf(b, "            %s[] = self::i32($v->asInt());\n", name)
		} else {
			fmt.Fprintf(b, "            %s[] = %s;\n", name, phpElem(field, "$v"))
		}
		b.WriteString("        }\n")
		return
	}
	if field.Message != "" {
		fmt.Fprintf(b, "        $child = $msg->getMessage(%q);\n", field.Name)
		fmt.Fprintf(b, "        if ($child !== null) { %s = %s::fromMessage($child); }\n", name, className(f, field.Message))
		return
	}
	read := fmt.Sprintf("$msg->%s(%q)", phpGetter(field), field.Name)
	if phpSigned32(field) {
		read = "self::i32(" + read + ")"
	}
	if field.NeedsHas {
		fmt.Fprintf(b, "        $o->has%s = $msg->has(%q);\n", field.Pascal, field.Name)
		fmt.Fprintf(b, "        if ($o->has%s) { %s = %s; }\n", field.Pascal, name, phpValue(field, read))
		return
	}
	fmt.Fprintf(b, "        %s = %s;\n", name, phpValue(field, read))
}

func phpGetter(field model.Field) string {
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

func phpValue(field model.Field, expr string) string {
	switch field.Kind {
	case protoreflect.StringKind, protoreflect.BytesKind:
		return expr + " ?? ''"
	default:
		return expr
	}
}

func phpElem(field model.Field, expr string) string {
	switch field.Kind {
	case protoreflect.StringKind:
		return expr + "->asString() ?? ''"
	case protoreflect.BytesKind:
		return expr + "->asBytes() ?? ''"
	case protoreflect.BoolKind:
		return expr + "->asBool()"
	case protoreflect.FloatKind:
		return expr + "->asFloat()"
	case protoreflect.DoubleKind:
		return expr + "->asDouble()"
	default:
		return expr + "->asInt()"
	}
}

package gen

import (
	"fmt"
	"strings"

	"pbwire-typed/internal/model"

	"google.golang.org/protobuf/reflect/protoreflect"
)

var rustKeywords = map[string]bool{
	"as": true, "break": true, "const": true, "continue": true, "crate": true,
	"dyn": true, "else": true, "enum": true, "extern": true, "false": true,
	"fn": true, "for": true, "if": true, "impl": true, "in": true, "let": true,
	"loop": true, "match": true, "mod": true, "move": true, "mut": true,
	"pub": true, "ref": true, "return": true, "self": true, "static": true,
	"struct": true, "super": true, "trait": true, "true": true, "type": true,
	"unsafe": true, "use": true, "where": true, "while": true, "async": true,
	"await": true, "try": true, "yield": true, "abstract": true, "become": true,
	"box": true, "do": true, "final": true, "macro": true, "override": true,
	"priv": true, "typeof": true, "union": true, "unsized": true, "virtual": true,
}

func rustName(name string) string {
	if rustKeywords[name] {
		return "r#" + name
	}
	return name
}

func rustType(field model.Field) string {
	switch field.Kind {
	case protoreflect.Int32Kind, protoreflect.Sint32Kind, protoreflect.Sfixed32Kind,
		protoreflect.Uint32Kind, protoreflect.Fixed32Kind, protoreflect.EnumKind:
		return "i32"
	case protoreflect.Int64Kind, protoreflect.Sint64Kind, protoreflect.Sfixed64Kind,
		protoreflect.Uint64Kind, protoreflect.Fixed64Kind:
		return "i64"
	case protoreflect.FloatKind:
		return "f32"
	case protoreflect.DoubleKind:
		return "f64"
	case protoreflect.BoolKind:
		return "bool"
	case protoreflect.StringKind:
		return "String"
	case protoreflect.BytesKind:
		return "Vec<u8>"
	default:
		return "Box<" + rustClass(field.Message) + ">"
	}
}

func rustFiles(f *model.File) []File {
	var b strings.Builder
	b.WriteString(header(f, "Typed structs over the pbwire runtime; every field number is a schema lookup."))
	b.WriteString("use pbwire::{Message, Schema};\n\n")
	for _, e := range f.Enums {
		fmt.Fprintf(&b, "/// %s\npub struct %s;\n\nimpl %s {\n", e.Full, rustClass(e.Name), rustClass(e.Name))
		for _, v := range e.Values {
			fmt.Fprintf(&b, "    pub const %s: i32 = %d;\n", v.Name, v.Number)
		}
		b.WriteString("}\n\n")
	}
	for _, m := range f.Messages {
		fmt.Fprintf(&b, "/// %s\n#[derive(Debug, Default, Clone, PartialEq)]\npub struct %s {\n", m.Full, rustClass(m.Name))
		for _, field := range m.Fields {
			if field.Skip != "" {
				fmt.Fprintf(&b, "    // skipped %s: %s\n", field.Name, field.Skip)
				continue
			}
			name := rustName(field.Name)
			if field.NeedsHas {
				fmt.Fprintf(&b, "    pub has_%s: bool,\n", field.Name)
			}
			switch {
			case field.Repeated:
				elem := rustType(field)
				if field.Message != "" {
					elem = rustClass(field.Message)
				}
				fmt.Fprintf(&b, "    pub %s: Vec<%s>, // %d\n", name, elem, field.Number)
			case field.Message != "":
				fmt.Fprintf(&b, "    pub %s: Option<%s>, // %d\n", name, rustType(field), field.Number)
			default:
				fmt.Fprintf(&b, "    pub %s: %s, // %d\n", name, rustType(field), field.Number)
			}
		}
		writeRustImpl(&b, f, m)
	}
	return []File{{Name: rustModuleName(f) + ".rs", Content: b.String()}}
}

// rustClass turns the neutral local name (Outer_Inner) into Rust's
// UpperCamelCase, which is also what the compiler asks for.
func rustClass(local string) string { return strings.ReplaceAll(local, "_", "") }

func rustModuleName(f *model.File) string {
	name := strings.ReplaceAll(base(f), "-", "_")
	return name + "_pbwire"
}

func writeRustImpl(b *strings.Builder, f *model.File, m model.Message) {
	fmt.Fprintf(b, "}\n\nimpl %s {\n    pub const FULL_NAME: &'static str = %q;\n\n", rustClass(m.Name), m.Full)
	fmt.Fprintf(b, "    /// Encodes through `schema` (the type is resolved from FULL_NAME).\n")
	fmt.Fprintf(b, "    pub fn encode(&self, schema: &Schema) -> pbwire::Result<Vec<u8>> {\n")
	fmt.Fprintf(b, "        let t = schema.message(Self::FULL_NAME)?;\n")
	fmt.Fprintf(b, "        let mut msg = t.new_message();\n")
	fmt.Fprintf(b, "        self.encode_into(schema, &mut msg)?;\n        msg.encode()\n    }\n\n")

	fmt.Fprintf(b, "    pub fn encode_into(&self, schema: &Schema, msg: &mut Message<'_>) -> pbwire::Result<()> {\n")
	fmt.Fprintf(b, "        let t = schema.message(Self::FULL_NAME)?;\n")
	for _, field := range m.Fields {
		if field.Skip != "" {
			continue
		}
		name := "self." + rustName(field.Name)
		cond := rustPresent(field, name)
		fmt.Fprintf(b, "        if %s {\n", cond)
		switch {
		case field.Repeated:
			fmt.Fprintf(b, "            for v in &%s {\n", name)
			if field.Message != "" {
				fmt.Fprintf(b, "                v.encode_into(schema, &mut msg.set_message(t.field(%q)?))?;\n", field.Name)
			} else {
				arg := "*v"
				if field.Kind == protoreflect.StringKind || field.Kind == protoreflect.BytesKind {
					arg = "v"
				}
				fmt.Fprintf(b, "                msg.%s(t.field(%q)?, %s);\n", rustSetter(field), field.Name, rustSetArg(field, arg))
			}
			b.WriteString("            }\n")
		case field.Message != "":
			fmt.Fprintf(b, "            if let Some(v) = &%s { v.encode_into(schema, &mut msg.set_message(t.field(%q)?))?; }\n", name, field.Name)
		default:
			fmt.Fprintf(b, "            msg.%s(t.field(%q)?, %s);\n", rustSetter(field), field.Name, rustSetArg(field, name))
		}
		b.WriteString("        }\n")
	}
	b.WriteString("        Ok(())\n    }\n\n")

	fmt.Fprintf(b, "    /// Decodes wire bytes; the typed struct is built from the schema.\n")
	fmt.Fprintf(b, "    pub fn decode(schema: &Schema, wire: &[u8]) -> pbwire::Result<Self> {\n")
	fmt.Fprintf(b, "        let t = schema.message(Self::FULL_NAME)?;\n")
	fmt.Fprintf(b, "        let msg = t.decode(wire)?;\n")
	fmt.Fprintf(b, "        Self::from_message(schema, &msg)\n    }\n\n")

	fmt.Fprintf(b, "    pub fn from_message(schema: &Schema, msg: &Message<'_>) -> pbwire::Result<Self> {\n")
	fmt.Fprintf(b, "        let t = schema.message(Self::FULL_NAME)?;\n        let mut o = Self::default();\n")
	for _, field := range m.Fields {
		if field.Skip != "" {
			continue
		}
		writeRustRead(b, f, field)
	}
	b.WriteString("        Ok(o)\n    }\n}\n\n")
}

func rustPresent(field model.Field, name string) string {
	if field.Required {
		return "true"
	}
	switch {
	case field.Repeated:
		return "!" + name + ".is_empty()"
	case field.Message != "":
		return name + ".is_some()"
	case field.NeedsHas:
		return "self.has_" + field.Name
	case field.Kind == protoreflect.BoolKind:
		return name
	case field.Kind == protoreflect.StringKind:
		return "!" + name + ".is_empty()"
	case field.Kind == protoreflect.BytesKind:
		return "!" + name + ".is_empty()"
	default:
		if field.Kind == protoreflect.FloatKind || field.Kind == protoreflect.DoubleKind {
			return name + " != 0.0"
		}
		return name + " != 0"
	}
}

func rustSetter(field model.Field) string {
	switch field.Kind {
	case protoreflect.FloatKind:
		return "set_f32"
	case protoreflect.DoubleKind:
		return "set_f64"
	case protoreflect.BoolKind:
		return "set_bool"
	case protoreflect.StringKind:
		return "set_str"
	case protoreflect.BytesKind:
		return "set_bytes"
	default:
		return "set_i64"
	}
}

func rustSetArg(field model.Field, expr string) string {
	switch field.Kind {
	case protoreflect.Int32Kind, protoreflect.Sint32Kind, protoreflect.Sfixed32Kind,
		protoreflect.Uint32Kind, protoreflect.Fixed32Kind, protoreflect.EnumKind:
		return expr + " as i64"
	case protoreflect.StringKind:
		return expr + ".as_str()"
	case protoreflect.BytesKind:
		return expr + ".as_slice()"
	default:
		return expr
	}
}

func writeRustRead(b *strings.Builder, f *model.File, field model.Field) {
	name := "o." + rustName(field.Name)
	if field.Repeated {
		fmt.Fprintf(b, "        if let Some(v) = msg.value(t.field(%q)?) {\n", field.Name)
		fmt.Fprintf(b, "            for i in 0..v.len() {\n                if let Some(item) = v.at(i) {\n")
		if field.Message != "" {
			fmt.Fprintf(b, "                    if let Some(m) = item.as_message() { %s.push(%s::from_message(schema, &m)?); }\n", name, rustClass(field.Message))
		} else {
			fmt.Fprintf(b, "                    %s.push(%s);\n", name, rustElem(field, "item"))
		}
		b.WriteString("                }\n            }\n        }\n")
		return
	}
	if field.Message != "" {
		fmt.Fprintf(b, "        if let Some(m) = msg.get_message(t.field(%q)?) {\n", field.Name)
		fmt.Fprintf(b, "            %s = Some(Box::new(%s::from_message(schema, &m)?));\n        }\n", name, rustClass(field.Message))
		return
	}
	read := fmt.Sprintf("msg.%s(t.field(%q)?)", rustGetter(field), field.Name)
	if field.NeedsHas {
		fmt.Fprintf(b, "        o.has_%s = msg.has(t.field(%q)?);\n", field.Name, field.Name)
		fmt.Fprintf(b, "        if o.has_%s { %s = %s; }\n", field.Name, name, rustValue(field, read))
		return
	}
	fmt.Fprintf(b, "        %s = %s;\n", name, rustValue(field, read))
}

func rustGetter(field model.Field) string {
	switch field.Kind {
	case protoreflect.FloatKind:
		return "get_f32"
	case protoreflect.DoubleKind:
		return "get_f64"
	case protoreflect.BoolKind:
		return "get_bool"
	case protoreflect.StringKind:
		return "get_str"
	case protoreflect.BytesKind:
		return "get_bytes"
	default:
		return "get_i64"
	}
}

func rustValue(field model.Field, expr string) string {
	switch field.Kind {
	case protoreflect.Int32Kind, protoreflect.Sint32Kind, protoreflect.Sfixed32Kind,
		protoreflect.Uint32Kind, protoreflect.Fixed32Kind, protoreflect.EnumKind:
		return expr + " as i32"
	case protoreflect.StringKind:
		return expr + ".unwrap_or(\"\").to_string()"
	case protoreflect.BytesKind:
		return expr + ".unwrap_or(&[]).to_vec()"
	default:
		return expr
	}
}

func rustElem(field model.Field, expr string) string {
	switch field.Kind {
	case protoreflect.Int32Kind, protoreflect.Sint32Kind, protoreflect.Sfixed32Kind,
		protoreflect.Uint32Kind, protoreflect.Fixed32Kind, protoreflect.EnumKind:
		return expr + ".as_i64() as i32"
	case protoreflect.StringKind:
		return expr + ".as_str().unwrap_or(\"\").to_string()"
	case protoreflect.BytesKind:
		return expr + ".as_bytes().unwrap_or(&[]).to_vec()"
	case protoreflect.BoolKind:
		return expr + ".as_bool()"
	case protoreflect.FloatKind:
		return expr + ".as_f32()"
	case protoreflect.DoubleKind:
		return expr + ".as_f64()"
	default:
		return expr + ".as_i64()"
	}
}

package gen

import (
	"fmt"
	"strings"

	"pbwire-typed/internal/model"

	"google.golang.org/protobuf/reflect/protoreflect"
)

var pythonKeywords = map[string]bool{
	"False": true, "None": true, "True": true, "and": true, "as": true,
	"assert": true, "async": true, "await": true, "break": true, "class": true,
	"continue": true, "def": true, "del": true, "elif": true, "else": true,
	"except": true, "finally": true, "for": true, "from": true, "global": true,
	"if": true, "import": true, "in": true, "is": true, "lambda": true,
	"nonlocal": true, "not": true, "or": true, "pass": true, "raise": true,
	"return": true, "try": true, "while": true, "with": true, "yield": true,
}

func pythonName(name string) string {
	if pythonKeywords[name] {
		return name + "_"
	}
	return name
}

func pythonType(field model.Field, prefix string) string {
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
	case protoreflect.StringKind:
		return "str"
	case protoreflect.BytesKind:
		return "bytes"
	default:
		return field.Message
	}
}

func pythonFiles(f *model.File) []File {
	var b strings.Builder
	b.WriteString(headerP(f, "#", "Typed dataclasses over the pbwire dict API: assign fields, encode, decode."))
	b.WriteString("from __future__ import annotations\n\n")
	b.WriteString("from dataclasses import dataclass, field as dc_field\n")
	b.WriteString("from typing import Any, Dict, List, Mapping, Optional\n\n")
	for _, e := range f.Enums {
		fmt.Fprintf(&b, "class %s:\n    \"\"\"%s\"\"\"\n", e.Name, e.Full)
		for _, v := range e.Values {
			fmt.Fprintf(&b, "    %s = %d\n", v.Name, v.Number)
		}
		b.WriteString("\n")
	}
	for _, m := range f.Messages {
		fmt.Fprintf(&b, "@dataclass\nclass %s:\n    \"\"\"%s\"\"\"\n\n", m.Name, m.Full)
		anyField := false
		for _, field := range m.Fields {
			if field.Skip != "" {
				fmt.Fprintf(&b, "    # skipped %s: %s\n", field.Name, field.Skip)
				continue
			}
			anyField = true
			fmt.Fprintf(&b, "    %s: %s = %s\n", pythonName(field.Name), pythonAnn(field), pythonDefault(field))
		}
		if !anyField {
			b.WriteString("    pass\n")
		}
		writePythonDict(&b, f, m)
		fmt.Fprintf(&b, "    def encode(self, msg_type: Any) -> bytes:\n")
		fmt.Fprintf(&b, "        \"\"\"Encode through a pbwire MessageType: msg_type.encode(self.to_dict()).\"\"\"\n")
		fmt.Fprintf(&b, "        return msg_type.encode(self.to_dict())\n\n")
		fmt.Fprintf(&b, "    @classmethod\n    def decode(cls, msg_type: Any, wire: bytes) -> %s:\n", m.Name)
		fmt.Fprintf(&b, "        \"\"\"Decode wire bytes: cls.from_dict(msg_type.decode(wire).to_dict()).\"\"\"\n")
		fmt.Fprintf(&b, "        return cls.from_dict(msg_type.decode(wire).to_dict())\n\n")
	}
	return []File{{Name: base(f) + "_pbwire.py", Content: b.String()}}
}

func pythonAnn(field model.Field) string {
	if field.NeedsHas {
		return "Optional[" + pythonType(field, "") + "]"
	}
	if field.Repeated {
		return "List[" + pythonType(field, "") + "]"
	}
	if field.Message != "" {
		return "Optional[" + field.Message + "]"
	}
	return pythonType(field, "")
}

func pythonDefault(field model.Field) string {
	if field.Repeated {
		return "dc_field(default_factory=list)"
	}
	if field.NeedsHas || field.Message != "" {
		return "None"
	}
	switch field.Kind {
	case protoreflect.BoolKind:
		return "False"
	case protoreflect.StringKind:
		return "''"
	case protoreflect.BytesKind:
		return "b''"
	case protoreflect.FloatKind, protoreflect.DoubleKind:
		return "0.0"
	default:
		return "0"
	}
}

func writePythonDict(b *strings.Builder, f *model.File, m model.Message) {
	b.WriteString("    def to_dict(self) -> Dict[str, Any]:\n")
	b.WriteString("        out: Dict[str, Any] = {}\n")
	for _, field := range m.Fields {
		if field.Skip != "" {
			continue
		}
		name := pythonName(field.Name)
		cond := pythonPresent(field, name)
		fmt.Fprintf(b, "        if %s:\n", cond)
		switch {
		case field.Repeated && field.Message != "":
			fmt.Fprintf(b, "            out[%q] = [v.to_dict() for v in self.%s]\n", field.Name, name)
		case field.Repeated:
			fmt.Fprintf(b, "            out[%q] = list(self.%s)\n", field.Name, name)
		case field.Message != "":
			fmt.Fprintf(b, "            out[%q] = self.%s.to_dict()\n", field.Name, name)
		default:
			fmt.Fprintf(b, "            out[%q] = self.%s\n", field.Name, name)
		}
		b.WriteString("\n")
	}
	b.WriteString("        return out\n\n")

	b.WriteString("    @classmethod\n")
	fmt.Fprintf(b, "    def from_dict(cls, data: Mapping[str, Any]) -> %s:\n", m.Name)
	fmt.Fprintf(b, "        return cls(\n")
	for _, field := range m.Fields {
		if field.Skip != "" {
			continue
		}
		name := pythonName(field.Name)
		fmt.Fprintf(b, "            %s=%s,\n", name, pythonFromDict(field))
	}
	b.WriteString("        )\n\n")
}

func pythonPresent(field model.Field, name string) string {
	if field.Required {
		return "True"
	}
	switch {
	case field.Repeated:
		return "self." + name
	case field.Message != "":
		return "self." + name + " is not None"
	case field.NeedsHas:
		return "self." + name + " is not None"
	case field.Kind == protoreflect.BoolKind:
		return "self." + name
	case field.Kind == protoreflect.StringKind, field.Kind == protoreflect.BytesKind:
		return "self." + name
	default:
		return "self." + name + " != 0"
	}
}

func pythonFromDict(field model.Field) string {
	key := fmt.Sprintf("%q", field.Name)
	switch {
	case field.Repeated && field.Message != "":
		return fmt.Sprintf("[%s.from_dict(v) for v in data.get(%s, [])]", field.Message, key)
	case field.Repeated && field.Kind == protoreflect.BytesKind:
		return fmt.Sprintf("[bytes(v) for v in data.get(%s, [])]", key)
	case field.Repeated && field.Kind == protoreflect.StringKind:
		return fmt.Sprintf("[str(v) for v in data.get(%s, [])]", key)
	case field.Repeated:
		return fmt.Sprintf("list(data.get(%s, []))", key)
	case field.Message != "":
		return fmt.Sprintf("%s.from_dict(data[%s]) if data.get(%s) else None", field.Message, key, key)
	case field.NeedsHas:
		return fmt.Sprintf("data[%s] if %s in data else None", key, key)
	case field.Kind == protoreflect.StringKind:
		return fmt.Sprintf("str(data.get(%s, ''))", key)
	case field.Kind == protoreflect.BytesKind:
		return fmt.Sprintf("bytes(data.get(%s, b''))", key)
	default:
		return fmt.Sprintf("data.get(%s, 0)", key)
	}
}

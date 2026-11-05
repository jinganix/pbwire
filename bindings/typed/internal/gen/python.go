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

const pythonRuntime = `import base64 as _base64

_SCHEMA = None
_TYPES = {}

def _schema():
    """The embedded FileDescriptorSet as a pbwire schema (built once)."""
    global _SCHEMA
    if _SCHEMA is None:
        import pbwire as _pbw
        _SCHEMA = _pbw.Schema.load(_base64.b64decode(_DESC_B64))
    return _SCHEMA


def _type(full):
    t = _TYPES.get(full)
    if t is None:
        t = _schema().message(full)
        _TYPES[full] = t
    return t

`

func pythonFiles(f *model.File, opt Options) []File {
	var b strings.Builder
	b.WriteString(headerP(f, "#", "Typed dataclasses over the pbwire dict API, official _pb2-shaped methods included."))
	b.WriteString("from __future__ import annotations\n\n")
	b.WriteString("from dataclasses import dataclass, field as dc_field\n")
	b.WriteString("from typing import Any, Dict, List, Mapping, Optional, Tuple\n\n")
	fmt.Fprintf(&b, "_DESC_B64 = \"%s\"\n\n", opt.DescSetB64)
	b.WriteString(pythonRuntime)
	for _, e := range f.Enums {
		fmt.Fprintf(&b, "class %s:\n    \"\"\"%s\"\"\"\n", e.Name, e.Full)
		for _, v := range e.Values {
			fmt.Fprintf(&b, "    %s = %d\n", v.Name, v.Number)
		}
		b.WriteString("\n")
	}
	for _, m := range f.Messages {
		writePythonClass(&b, f, m)
	}
	writePythonAliases(&b, f)
	b.WriteString("DESCRIPTOR = {\n")
	fmt.Fprintf(&b, "    \"name\": %q,\n", f.Path)
	fmt.Fprintf(&b, "    \"package\": %q,\n", f.Package)
	b.WriteString("    \"messages\": [\n")
	for _, m := range f.Messages {
		fmt.Fprintf(&b, "        %q,\n", m.Full)
	}
	b.WriteString("    ],\n}\n")
	return []File{{Name: base(f) + "_pbwire.py", Content: b.String()}}
}

func writePythonClass(b *strings.Builder, f *model.File, m model.Message) {
	fmt.Fprintf(b, "@dataclass\nclass %s:\n    \"\"\"%s\"\"\"\n\n", m.Name, m.Full)
	anyField := false
	for _, field := range m.Fields {
		if field.Skip != "" {
			fmt.Fprintf(b, "    # skipped %s: %s\n", field.Name, field.Skip)
			continue
		}
		anyField = true
		fmt.Fprintf(b, "    %s: %s = %s\n", pythonName(field.Name), pythonAnn(field), pythonDefault(field))
	}
	if !anyField {
		b.WriteString("    pass\n\n")
	}
	fmt.Fprintf(b, "    DESCRIPTOR = {\"fullName\": %q, \"fields\": {", m.Full)
	sep := ""
	for _, field := range m.Fields {
		if field.Skip != "" {
			continue
		}
		fmt.Fprintf(b, "%s%q: %d", sep, field.Name, field.Number)
		sep = ", "
	}
	b.WriteString("}}\n\n")
	oneofs := pythonOneofs(m)
	if len(oneofs) > 0 {
		b.WriteString("    _ONEOFS = {\n")
		for _, o := range oneofs {
			fmt.Fprintf(b, "        %q: [", o)
			sep := ""
			for _, field := range m.Fields {
				if field.Skip == "" && field.Oneof == o {
					fmt.Fprintf(b, "%s%q", sep, field.Name)
					sep = ", "
				}
			}
			b.WriteString("],\n")
		}
		b.WriteString("    }\n\n")
	}
	writePythonDict(b, f, m)
	writePythonOfficial(b, f, m, oneofs)
	fmt.Fprintf(b, "    def encode(self, msg_type: Any) -> bytes:\n")
	fmt.Fprintf(b, "        \"\"\"Encode through a pbwire MessageType: msg_type.encode(self.to_dict()).\"\"\"\n")
	fmt.Fprintf(b, "        return msg_type.encode(self.to_dict())\n\n")
	fmt.Fprintf(b, "    @classmethod\n    def decode(cls, msg_type: Any, wire: bytes) -> %s:\n", m.Name)
	fmt.Fprintf(b, "        \"\"\"Decode wire bytes: cls.from_dict(msg_type.decode(wire).to_dict()).\"\"\"\n")
	fmt.Fprintf(b, "        return cls.from_dict(msg_type.decode(wire).to_dict())\n\n")
}

// writePythonAliases re-attaches nested types to their parents, so both
// Oneof1_Inner and Oneof1.Inner resolve (the official _pb2 nests them).
func writePythonAliases(b *strings.Builder, f *model.File) {
	known := map[string]bool{}
	for _, m := range f.Messages {
		known[m.Name] = true
	}
	for _, e := range f.Enums {
		known[e.Name] = true
	}
	alias := func(flat string) {
		name := flat
		for {
			idx := strings.LastIndex(name, "_")
			if idx < 0 {
				return
			}
			parent, child := name[:idx], name[idx+1:]
			if known[parent] {
				fmt.Fprintf(b, "%s.%s = %s\n", parent, child, flat)
				return
			}
			name = parent
		}
	}
	for _, m := range f.Messages {
		alias(m.Name)
	}
	for _, e := range f.Enums {
		alias(e.Name)
	}
	b.WriteString("\n")
}

// pythonOneofs returns the distinct oneof names of a message in field order.
func pythonOneofs(m model.Message) []string {
	var out []string
	seen := map[string]bool{}
	for _, field := range m.Fields {
		if field.Skip == "" && field.Oneof != "" && !seen[field.Oneof] {
			seen[field.Oneof] = true
			out = append(out, field.Oneof)
		}
	}
	return out
}

// writePythonOfficial emits the official-runtime-shaped methods: zero-argument
// SerializeToString/ParseFromString/FromString plus CopyFrom/MergeFrom/Clear,
// HasField/WhichOneof/ListFields. The embedded schema feeds the C core.
func writePythonOfficial(b *strings.Builder, f *model.File, m model.Message, oneofs []string) {
	full := m.Full
	b.WriteString("    def SerializeToString(self) -> bytes:\n")
	b.WriteString("        \"\"\"Wire bytes; the embedded schema drives the C core.\"\"\"\n")
	fmt.Fprintf(b, "        return _type(%q).encode(self.to_dict())\n\n", full)
	b.WriteString("    def ParseFromString(self, wire: bytes) -> int:\n")
	b.WriteString("        \"\"\"Merge wire bytes into this message; returns bytes consumed.\"\"\"\n")
	fmt.Fprintf(b, "        other = %s.FromString(wire)\n", m.Name)
	b.WriteString("        self.CopyFrom(other)\n")
	b.WriteString("        return len(wire)\n\n")
	b.WriteString("    @classmethod\n")
	fmt.Fprintf(b, "    def FromString(cls, wire: bytes) -> %s:\n", m.Name)
	fmt.Fprintf(b, "        return cls.from_dict(_type(%q).decode(bytes(wire)).to_dict())\n\n", full)
	b.WriteString("    def CopyFrom(self, other) -> None:\n")
	fmt.Fprintf(b, "        if not isinstance(other, %s):\n", m.Name)
	fmt.Fprintf(b, "            raise TypeError(\"CopyFrom expects %s\")\n", m.Name)
	b.WriteString("        self.Clear()\n")
	b.WriteString("        self.MergeFrom(other)\n\n")
	b.WriteString("    def MergeFrom(self, other) -> None:\n")
	fmt.Fprintf(b, "        if not isinstance(other, %s):\n", m.Name)
	fmt.Fprintf(b, "            raise TypeError(\"MergeFrom expects %s\")\n", m.Name)
	hasMerge := false
	for _, field := range m.Fields {
		if field.Skip == "" {
			hasMerge = true
			break
		}
	}
	if !hasMerge {
		b.WriteString("        pass\n")
	}
	for _, field := range m.Fields {
		if field.Skip != "" {
			continue
		}
		name := pythonName(field.Name)
		switch {
		case field.Repeated && field.Message != "":
			fmt.Fprintf(b, "        for _v in other.%s:\n", name)
			fmt.Fprintf(b, "            _m = %s()\n", field.Message)
			b.WriteString("            _m.MergeFrom(_v)\n")
			fmt.Fprintf(b, "            self.%s.append(_m)\n", name)
		case field.Repeated:
			fmt.Fprintf(b, "        self.%s.extend(other.%s)\n", name, name)
		case field.Message != "":
			fmt.Fprintf(b, "        if other.%s is not None:\n", name)
			fmt.Fprintf(b, "            if self.%s is None:\n", name)
			fmt.Fprintf(b, "                self.%s = %s()\n", name, field.Message)
			fmt.Fprintf(b, "            self.%s.MergeFrom(other.%s)\n", name, name)
		case field.NeedsHas || field.Required:
			// Presence (or required): copy when the source has it. A
			// required field is always written, so it always lands here.
			if field.Required {
				fmt.Fprintf(b, "        self.%s = other.%s\n", name, name)
			} else {
				fmt.Fprintf(b, "        if other.%s is not None:\n", name)
				fmt.Fprintf(b, "            self.%s = other.%s\n", name, name)
			}
		default:
			// Implicit presence: proto3 MergeFrom copies unconditionally.
			fmt.Fprintf(b, "        self.%s = other.%s\n", name, name)
		}
	}
	b.WriteString("\n")
	b.WriteString("    def Clear(self) -> None:\n")
	hasClear := false
	for _, field := range m.Fields {
		if field.Skip == "" {
			hasClear = true
			break
		}
	}
	if !hasClear {
		b.WriteString("        pass\n")
	}
	for _, field := range m.Fields {
		if field.Skip != "" {
			continue
		}
		name := pythonName(field.Name)
		fmt.Fprintf(b, "        self.%s = %s\n", name, pythonFresh(field))
	}
	b.WriteString("\n")
	b.WriteString("    def HasField(self, name: str) -> bool:\n")
	b.WriteString("        \"\"\"Presence of a field; raises ValueError without explicit presence.\"\"\"\n")
	for _, field := range m.Fields {
		if field.Skip != "" {
			continue
		}
		if field.Message == "" && !field.NeedsHas && !field.Required {
			continue
		}
		name := pythonName(field.Name)
		if field.Required {
			fmt.Fprintf(b, "        if name == %q:\n            return True\n", field.Name)
		} else {
			fmt.Fprintf(b, "        if name == %q:\n            return self.%s is not None\n", field.Name, name)
		}
	}
	b.WriteString("        raise ValueError(\"no explicit presence for %r\" % (name,))\n\n")
	if len(oneofs) > 0 {
		b.WriteString("    def WhichOneof(self, name: str) -> Optional[str]:\n")
		b.WriteString("        for _member in self._ONEOFS.get(name, []):\n")
		b.WriteString("            if getattr(self, _member, None) is not None:\n")
		b.WriteString("                _v = getattr(self, _member)\n")
		b.WriteString("                if isinstance(_v, list) and not _v:\n")
		b.WriteString("                    continue\n")
		b.WriteString("                return _member\n")
		b.WriteString("        if name not in self._ONEOFS:\n")
		b.WriteString("            raise ValueError(\"unknown oneof %r\" % (name,))\n")
		b.WriteString("        return None\n\n")
	}
	b.WriteString("    def ListFields(self) -> List[Tuple[str, Any]]:\n")
	b.WriteString("        out: List[Tuple[str, Any]] = []\n")
	for _, field := range m.Fields {
		if field.Skip != "" {
			continue
		}
		name := pythonName(field.Name)
		cond := pythonPresent(field, "self."+name)
		fmt.Fprintf(b, "        if %s:\n            out.append((%q, self.%s))\n", cond, field.Name, name)
	}
	b.WriteString("        return out\n\n")
}

// pythonFresh is the Clear() value: a new mutable for lists, the default else.
func pythonFresh(field model.Field) string {
	if field.Repeated {
		return "[]"
	}
	if field.NeedsHas || field.Message != "" {
		return "None"
	}
	return pythonDefault(field)
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
		cond := pythonPresent(field, "self."+name)
		// Required fields are always present: emit a plain assignment so
		// the generated code reads like hand-written Python, not a
		// template leftover (`if True:`). Optional fields keep the guard.
		if cond == "True" {
			switch {
			case field.Repeated && field.Message != "":
				fmt.Fprintf(b, "        out[%q] = [v.to_dict() for v in self.%s]\n", field.Name, name)
			case field.Repeated:
				fmt.Fprintf(b, "        out[%q] = list(self.%s)\n", field.Name, name)
			case field.Message != "":
				fmt.Fprintf(b, "        out[%q] = self.%s.to_dict()\n", field.Name, name)
			default:
				fmt.Fprintf(b, "        out[%q] = self.%s\n", field.Name, name)
			}
			b.WriteString("\n")
			continue
		}
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
		return name
	case field.Message != "":
		return name + " is not None"
	case field.NeedsHas:
		return name + " is not None"
	case field.Kind == protoreflect.BoolKind:
		return name
	case field.Kind == protoreflect.StringKind, field.Kind == protoreflect.BytesKind:
		return name
	default:
		return name + " != 0"
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

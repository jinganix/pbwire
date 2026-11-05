package gen

import (
	"fmt"
	"strings"

	"pbwire-codegen/internal/model"

	"google.golang.org/protobuf/reflect/protoreflect"
)

// goPackage derives the Go package name from the proto base file name.
func goPackage(f *model.File) string {
	var b strings.Builder
	for _, r := range base(f) {
		if (r >= 'a' && r <= 'z') || (r >= 'A' && r <= 'Z') || (r >= '0' && r <= '9') {
			b.WriteRune(r)
		} else {
			b.WriteRune('_')
		}
	}
	s := b.String()
	if s == "" {
		return "gen"
	}
	return s
}

// goCamel converts a proto field name to a Go struct field name:
// opt_i32 -> OptI32, inner -> Inner. A leading uppercase letter is kept.
func goCamel(name string) string {
	var b strings.Builder
	upper := true
	for _, r := range name {
		if r == '_' {
			upper = true
			continue
		}
		if upper {
			if r >= 'a' && r <= 'z' {
				r -= 'a' - 'A'
			}
			upper = false
		}
		b.WriteRune(r)
	}
	return b.String()
}

// goTypeName is the Go type of a non-repeated, non-optional field.
func goTypeName(f *model.File, field model.Field) string {
	if field.Message != "" {
		return "*" + field.Message
	}
	switch field.Kind {
	case protoreflect.Int32Kind, protoreflect.Sint32Kind, protoreflect.Sfixed32Kind:
		return "int32"
	case protoreflect.Uint32Kind, protoreflect.Fixed32Kind:
		return "uint32"
	case protoreflect.EnumKind:
		if field.Enum != "" {
			return field.Enum
		}
		return "int32"
	case protoreflect.Int64Kind, protoreflect.Sint64Kind, protoreflect.Sfixed64Kind:
		return "int64"
	case protoreflect.Uint64Kind, protoreflect.Fixed64Kind:
		return "uint64"
	case protoreflect.FloatKind:
		return "float32"
	case protoreflect.DoubleKind:
		return "float64"
	case protoreflect.BoolKind:
		return "bool"
	case protoreflect.StringKind:
		return "string"
	case protoreflect.BytesKind:
		return "[]byte"
	default:
		return "*" + field.Message
	}
}

// goFieldType is the struct field type with repetition and presence.
func goFieldType(f *model.File, field model.Field) string {
	if field.Oneof != "" {
		return "" // oneof members live in wrapper structs
	}
	t := goTypeName(f, field)
	if field.Repeated {
		if field.Message != "" {
			return "[]*" + field.Message
		}
		if field.Kind == protoreflect.BytesKind {
			return "[][]byte"
		}
		if field.Kind == protoreflect.EnumKind && field.Enum != "" {
			return "[]" + field.Enum
		}
		return "[]" + t
	}
	if field.NeedsHas && !field.Required {
		if strings.HasPrefix(t, "*") || strings.HasPrefix(t, "[]") {
			return t
		}
		return "*" + t
	}
	return t
}

// goZero is the zero value literal for a struct field type.
func goZero(ft string) string {
	switch ft {
	case "string":
		return `""`
	case "bool":
		return "false"
	case "float32", "float64":
		return "0"
	case "[]byte":
		return "nil"
	default:
		if strings.HasPrefix(ft, "*") || strings.HasPrefix(ft, "[]") {
			return "nil"
		}
		return "0"
	}
}

const goRuntime = `var fileDesc, _ = base64.StdEncoding.DecodeString(_DESC_B64)

var schemaOnce sync.Once
var schemaCached *dynamic.Schema
var schemaErr error

func getSchema() (*dynamic.Schema, error) {
	schemaOnce.Do(func() {
		schemaCached, schemaErr = dynamic.Load(fileDesc)
	})
	return schemaCached, schemaErr
}

var typesMu sync.Mutex
var typesCache = map[string]*dynamic.MessageType{}

func getType(full string) (*dynamic.MessageType, error) {
	s, err := getSchema()
	if err != nil {
		return nil, err
	}
	typesMu.Lock()
	defer typesMu.Unlock()
	t, ok := typesCache[full]
	if !ok {
		t, err = s.Message(full)
		if err != nil {
			return nil, err
		}
		typesCache[full] = t
	}
	return t, nil
}
`

// goFiles emits one self-contained package: generated structs over the pbwire
// dynamic API in protoc-gen-go shape (CamelCase fields, optional pointers,
// oneof wrappers, Getters), with the FileDescriptorSet embedded.
// goImportAlias derives the package alias from a mapped import path
// (last segment, sanitized like goPackage): the dep file's own generation
// uses the same rule, so the alias agrees without further options.
func goImportAlias(importPath string) string {
	seg := importPath
	if i := strings.LastIndex(seg, "/"); i >= 0 {
		seg = seg[i+1:]
	}
	var sb strings.Builder
	for _, r := range seg {
		if (r >= 'a' && r <= 'z') || (r >= 'A' && r <= 'Z') || (r >= '0' && r <= '9') {
			sb.WriteRune(r)
		} else {
			sb.WriteRune('_')
		}
	}
	if sb.Len() == 0 {
		return "gen"
	}
	return sb.String()
}

func goFiles(f *model.File, opt Options) []File {
	var b strings.Builder
	// Foreign message refs qualify as alias.Type (M mapping) or stay bare
	// (same package); one rewrite up front, use sites untouched.
	qualifyFields(f, func(imp model.Import, local string) string {
		if path, ok := opt.Mappings[imp.Path]; ok {
			return goImportAlias(path) + "." + local
		}
		return local
	})
	b.WriteString(headerP(f, "//", "Generated structs over the pbwire dynamic API, protoc-gen-go shape."))
	fmt.Fprintf(&b, "package %s\n\n", goPackage(f))
	b.WriteString("import (\n\t\"encoding/base64\"\n\t\"fmt\"\n\t\"sync\"\n\n\t\"pbwire/dynamic\"\n")
	for _, imp := range f.Imports {
		if path, ok := opt.Mappings[imp.Path]; ok {
			fmt.Fprintf(&b, "\n\t%s %q\n", goImportAlias(path), path)
		}
	}
	b.WriteString(")\n\n")
	b.WriteString(goRuntime)
	fmt.Fprintf(&b, "\nconst _DESC_B64 = %q\n\n", opt.DescSetB64)
	for _, e := range f.Enums {
		writeGoEnum(&b, e)
	}
	for _, m := range f.Messages {
		writeGoMessage(&b, f, m)
	}
	return []File{{Name: base(f) + "_pbwire.go", Content: b.String()}}
}

func writeGoEnum(b *strings.Builder, e model.Enum) {
	fmt.Fprintf(b, "// %s\ntype %s int32\n\nconst (\n", e.Full, e.Name)
	for _, v := range e.Values {
		fmt.Fprintf(b, "\t%s_%s %s = %d\n", e.Name, v.Name, e.Name, v.Number)
	}
	b.WriteString(")\n\n")
	fmt.Fprintf(b, "var %s_name = map[int32]string{\n", e.Name)
	seen := map[int32]bool{}
	for _, v := range e.Values {
		if seen[v.Number] {
			continue // alias: first name wins, like protoc-gen-go
		}
		seen[v.Number] = true
		fmt.Fprintf(b, "\t%d: %q,\n", v.Number, v.Name)
	}
	b.WriteString("}\n\n")
	fmt.Fprintf(b, "func (c %s) String() string {\n", e.Name)
	fmt.Fprintf(b, "\tif s, ok := %s_name[int32(c)]; ok {\n\t\treturn s\n\t}\n", e.Name)
	b.WriteString("\treturn fmt.Sprintf(\"%d\", int32(c))\n}\n\n")
}

// goWrapper is the oneof wrapper struct name. Like protoc-gen-go, a member
// whose name collides with a real nested type gets a trailing underscore
// (oneof member `inner` of message `Oneof1_Inner` wraps as `Oneof1_Inner_`).
func goWrapper(f *model.File, m model.Message, field model.Field) string {
	base := fmt.Sprintf("%s_%s", m.Name, goCamel(field.Name))
	for _, mm := range f.Messages {
		if mm.Name == base {
			return base + "_"
		}
	}
	for _, e := range f.Enums {
		if e.Name == base {
			return base + "_"
		}
	}
	return base
}

func writeGoMessage(b *strings.Builder, f *model.File, m model.Message) {
	oneofs := goOneofs(m)
	fmt.Fprintf(b, "// %s\n", m.Full)
	if len(oneofs) > 0 {
		for _, o := range oneofs {
			fmt.Fprintf(b, "type is%s_%s interface{ is%s_%s() }\n", m.Name, o, m.Name, o)
		}
		b.WriteString("\n")
		for _, field := range m.Fields {
			if field.Skip != "" || field.Oneof == "" {
				continue
			}
			wrap := goWrapper(f, m, field)
			fmt.Fprintf(b, "type %s struct{ %s %s }\n", wrap, goCamel(field.Name), goWrapType(f, field))
			fmt.Fprintf(b, "func (*%s) is%s_%s() {}\n\n", wrap, m.Name, field.Oneof)
		}
	}
	fmt.Fprintf(b, "type %s struct {\n", m.Name)
	for _, o := range goOneofs(m) {
		fmt.Fprintf(b, "\t%s is%s_%s\n", goCamel(o), m.Name, o)
	}
	for _, field := range m.Fields {
		if field.Skip != "" || field.Oneof != "" {
			continue
		}
		fmt.Fprintf(b, "\t%s %s\n", goCamel(field.Name), goFieldType(f, field))
	}
	b.WriteString("}\n\n")
	writeGoGetters(b, f, m)
	writeGoCodec(b, f, m, oneofs)
}

// goWrapType is the payload type inside a oneof wrapper struct. Message
// members stay pointers, like protoc-gen-go.
func goWrapType(f *model.File, field model.Field) string {
	t := goTypeName(f, field)
	if field.Repeated {
		if field.Message != "" {
			return "[]*" + field.Message
		}
		return "[]" + t
	}
	return t
}

// goOneofs returns the distinct oneof names of a message in field order.
func goOneofs(m model.Message) []string {
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

// writeGoGetters emits nil-safe Getters, protoc-gen-go style: optional
// scalars dereference (nil reads zero), messages read through.
func writeGoGetters(b *strings.Builder, f *model.File, m model.Message) {
	for _, field := range m.Fields {
		if field.Skip != "" || field.Oneof != "" {
			continue
		}
		name, typ := goCamel(field.Name), goFieldType(f, field)
		if strings.HasPrefix(typ, "*") && field.Message == "" {
			base := strings.TrimPrefix(typ, "*")
			fmt.Fprintf(b, "func (m *%s) Get%s() %s {\n\tif m == nil || m.%s == nil {\n\t\treturn %s\n\t}\n\treturn *m.%s\n}\n\n",
				m.Name, name, base, name, goZero(base), name)
			continue
		}
		fmt.Fprintf(b, "func (m *%s) Get%s() %s {\n\tif m == nil {\n\t\treturn %s\n\t}\n\treturn m.%s\n}\n\n",
			m.Name, name, typ, goZero(typ), name)
	}
	for _, o := range goOneofs(m) {
		fmt.Fprintf(b, "func (m *%s) Get%s() is%s_%s {\n\tif m == nil {\n\t\treturn nil\n\t}\n\treturn m.%s\n}\n\n",
			m.Name, goCamel(o), m.Name, o, goCamel(o))
		for _, field := range m.Fields {
			if field.Skip != "" || field.Oneof != o {
				continue
			}
			wrap := goWrapper(f, m, field)
			fname, kind := goCamel(field.Name), goCamel(o)
			if field.Message != "" {
				fmt.Fprintf(b, "func (m *%s) Get%s() *%s {\n\tif x, ok := m.Get%s().(*%s); ok {\n\t\treturn x.%s\n\t}\n\treturn nil\n}\n\n",
					m.Name, fname, field.Message, kind, wrap, fname)
				continue
			}
			wt := goWrapType(f, field)
			fmt.Fprintf(b, "func (m *%s) Get%s() %s {\n\tif x, ok := m.Get%s().(*%s); ok {\n\t\treturn x.%s\n\t}\n\treturn %s\n}\n\n",
				m.Name, fname, wt, kind, wrap, fname, goZero(wt))
		}
	}
}

// writeGoCodec emits Reset/Marshal/Unmarshal over the embedded schema.
func writeGoCodec(b *strings.Builder, f *model.File, m model.Message, oneofs []string) {
	fmt.Fprintf(b, "func (m *%s) Reset() {\n\tif m == nil {\n\t\treturn\n\t}\n\t*m = %s{}\n}\n\n", m.Name, m.Name)
	fmt.Fprintf(b, "func (m *%s) Marshal() ([]byte, error) {\n", m.Name)
	b.WriteString("\tif m == nil {\n\t\treturn []byte{}, nil\n\t}\n")
	fmt.Fprintf(b, "\tt, err := getType(%q)\n\tif err != nil {\n\t\treturn nil, err\n\t}\n", m.Full)
	b.WriteString("\tmsg := t.New()\n\tdefer msg.Close()\n")
	b.WriteString("\tif err := m.marshalInto(msg); err != nil {\n\t\treturn nil, err\n\t}\n")
	b.WriteString("\treturn msg.Encode()\n}\n\n")
	fmt.Fprintf(b, "func (m *%s) marshalInto(msg *dynamic.Message) error {\n", m.Name)
	for _, o := range oneofs {
		fmt.Fprintf(b, "\tswitch x := m.%s.(type) {\n", goCamel(o))
		for _, field := range m.Fields {
			if field.Skip != "" || field.Oneof != o {
				continue
			}
			name := goCamel(field.Name)
			key := fmt.Sprintf("%q", field.Name)
			fmt.Fprintf(b, "\tcase *%s:\n", goWrapper(f, m, field))
			writeGoSetValue(b, f, field, "x."+name, key, "msg")
		}
		b.WriteString("\t}\n")
	}
	for _, field := range m.Fields {
		if field.Skip != "" || field.Oneof != "" {
			continue
		}
		writeGoMarshalField(b, f, m, field)
	}
	b.WriteString("\treturn nil\n}\n\n")
	fmt.Fprintf(b, "func (m *%s) Unmarshal(wire []byte) error {\n", m.Name)
	fmt.Fprintf(b, "\tif m == nil {\n\t\treturn fmt.Errorf(%q)\n\t}\n", "pbwire: unmarshal into nil "+m.Name)
	fmt.Fprintf(b, "\tt, err := getType(%q)\n\tif err != nil {\n\t\treturn err\n\t}\n", m.Full)
	b.WriteString("\tmsg, err := t.Decode(wire)\n\tif err != nil {\n\t\treturn err\n\t}\n\tdefer msg.Close()\n")
	b.WriteString("\treturn m.load(msg)\n}\n\n")
	fmt.Fprintf(b, "func (m *%s) load(msg *dynamic.Message) error {\n", m.Name)
	for _, field := range m.Fields {
		if field.Skip != "" {
			continue
		}
		writeGoLoadField(b, f, m, field)
	}
	b.WriteString("\treturn nil\n}\n\n")
	_ = oneofs
}

// writeGoMarshalField emits one non-oneof field (oneofs are grouped once in
// marshalInto, since a switch per member would repeat the whole group).
func writeGoMarshalField(b *strings.Builder, f *model.File, m model.Message, field model.Field) {
	name := goCamel(field.Name)
	key := fmt.Sprintf("%q", field.Name)
	cond, use := goMarshalCond(name, goFieldType(f, field), field)
	if !use {
		// Required: always written.
		writeGoSetValue(b, f, field, "m."+name, key, "msg")
		return
	}
	fmt.Fprintf(b, "\tif %s {\n", cond)
	writeGoSetValue(b, f, field, "m."+name, key, "msg")
	b.WriteString("\t}\n")
}

// goMarshalCond is the write guard for implicit and explicit presence.
func goMarshalCond(name, typ string, field model.Field) (string, bool) {
	if field.Required {
		return "", false
	}
	if field.NeedsHas {
		return "m." + name + " != nil", true
	}
	switch {
	case typ == "string" || typ == "[]byte":
		return "len(m." + name + ") > 0", true
	case typ == "bool":
		return "m." + name, true
	case strings.HasPrefix(typ, "[]"):
		return "len(m." + name + ") > 0", true
	case strings.HasPrefix(typ, "*"):
		return "m." + name + " != nil", true
	default:
		return "m." + name + " != 0", true
	}
}

// writeGoSetValue emits one field assignment into a dynamic message.
func writeGoSetValue(b *strings.Builder, f *model.File, field model.Field, src, key, msg string) {
	_ = f
	// Foreign values cross packages through bytes (SetMessageBytes): neither
	// side calls the other's unexported fill methods.
	if field.Message != "" && field.Import != "" {
		if !field.Repeated {
			fmt.Fprintf(b, "\t\tsubBytes, err := %s.Marshal()\n", src)
			b.WriteString("\t\tif err != nil {\n\t\t\treturn err\n\t\t}\n")
			fmt.Fprintf(b, "\t\tif err := %s.SetMessageBytes(%s, subBytes); err != nil {\n\t\t\treturn err\n\t\t}\n", msg, key)
			return
		}
		fmt.Fprintf(b, "\t\tfor _, item := range %s {\n", src)
		b.WriteString("\t\t\tsubBytes, err := item.Marshal()\n")
		b.WriteString("\t\t\tif err != nil {\n\t\t\t\treturn err\n\t\t\t}\n")
		fmt.Fprintf(b, "\t\t\tif err := %s.SetMessageBytes(%s, subBytes); err != nil {\n\t\t\t\treturn err\n\t\t\t}\n\t\t}\n", msg, key)
		return
	}
	switch {
	case field.Message != "" && !field.Repeated:
		fmt.Fprintf(b, "\t\tsub, err := %s.NewMessage(%s)\n", msg, key)
		b.WriteString("\t\tif err != nil {\n\t\t\treturn err\n\t\t}\n")
		fmt.Fprintf(b, "\t\tif err := %s.marshalInto(sub); err != nil {\n\t\t\treturn err\n\t\t\t}\n", src)
	case field.Message != "" && field.Repeated:
		fmt.Fprintf(b, "\t\tfor _, item := range %s {\n", src)
		fmt.Fprintf(b, "\t\t\tsub, err := %s.NewMessage(%s)\n", msg, key)
		b.WriteString("\t\t\tif err != nil {\n\t\t\t\treturn err\n\t\t\t}\n")
		b.WriteString("\t\t\tif err := item.marshalInto(sub); err != nil {\n\t\t\t\treturn err\n\t\t\t}\n\t\t}\n")
	case field.Repeated:
		fmt.Fprintf(b, "\t\tfor _, item := range %s {\n", src)
		writeGoSetScalar(b, field, "item", key, msg, "\t\t\t")
		b.WriteString("\t\t}\n")
	default:
		writeGoSetScalar(b, field, deref(src, field), key, msg, "\t\t")
	}
}

// deref unwraps an optional pointer for the setter call. Oneof members
// live in wrappers as plain values, so they are never dereferenced.
func deref(src string, field model.Field) string {
	if field.NeedsHas && !field.Required && field.Message == "" && !field.Repeated && field.Oneof == "" {
		return "*" + src
	}
	return src
}

// writeGoSetScalar emits a scalar setter keyed by field kind.
func writeGoSetScalar(b *strings.Builder, field model.Field, src, key, msg, indent string) {
	switch field.Kind {
	case protoreflect.Int32Kind, protoreflect.Sint32Kind, protoreflect.Sfixed32Kind,
		protoreflect.Int64Kind, protoreflect.Sint64Kind, protoreflect.Sfixed64Kind,
		protoreflect.EnumKind:
		fmt.Fprintf(b, "%sif err := %s.SetInt(%s, int64(%s)); err != nil {\n%s\treturn err\n%s}\n",
			indent, msg, key, src, indent, indent)
	case protoreflect.Uint32Kind, protoreflect.Fixed32Kind,
		protoreflect.Uint64Kind, protoreflect.Fixed64Kind:
		fmt.Fprintf(b, "%sif err := %s.SetUint(%s, uint64(%s)); err != nil {\n%s\treturn err\n%s}\n",
			indent, msg, key, src, indent, indent)
	case protoreflect.BoolKind:
		fmt.Fprintf(b, "%sif err := %s.SetBool(%s, %s); err != nil {\n%s\treturn err\n%s}\n",
			indent, msg, key, src, indent, indent)
	case protoreflect.FloatKind:
		fmt.Fprintf(b, "%sif err := %s.SetFloat(%s, %s); err != nil {\n%s\treturn err\n%s}\n",
			indent, msg, key, src, indent, indent)
	case protoreflect.DoubleKind:
		fmt.Fprintf(b, "%sif err := %s.SetDouble(%s, %s); err != nil {\n%s\treturn err\n%s}\n",
			indent, msg, key, src, indent, indent)
	case protoreflect.StringKind:
		fmt.Fprintf(b, "%sif err := %s.SetString(%s, %s); err != nil {\n%s\treturn err\n%s}\n",
			indent, msg, key, src, indent, indent)
	case protoreflect.BytesKind:
		fmt.Fprintf(b, "%sif err := %s.SetBytes(%s, %s); err != nil {\n%s\treturn err\n%s}\n",
			indent, msg, key, src, indent, indent)
	default:
		fmt.Fprintf(b, "%sif err := %s.SetString(%s, %s); err != nil {\n%s\treturn err\n%s}\n",
			indent, msg, key, src, indent, indent)
	}
}

func writeGoLoadField(b *strings.Builder, f *model.File, m model.Message, field model.Field) {
	name := goCamel(field.Name)
	key := fmt.Sprintf("%q", field.Name)
	if field.Oneof != "" {
		wrap := goWrapper(f, m, field)
		b.WriteString("\tif ok, err := msg.Has(" + key + "); err != nil {\n\t\treturn err\n\t} else if ok {\n")
		writeGoLoadOne(b, f, m, field, wrap)
		b.WriteString("\t}\n")
		return
	}
	// Foreign values arrive as bytes (sub.Encode) and parse through the
	// dep package's own Unmarshal: no unexported cross-package calls.
	if field.Message != "" && field.Import != "" {
		if !field.Repeated {
			fmt.Fprintf(b, "\tif sub, ok, err := msg.GetMessage(%s); err != nil {\n\t\treturn err\n\t} else if ok {\n", key)
			fmt.Fprintf(b, "\t\tsubBytes, err := sub.Encode()\n\t\tif err != nil {\n\t\t\treturn err\n\t\t}\n")
			fmt.Fprintf(b, "\t\tm.%s = &%s{}\n\t\tif err := m.%s.Unmarshal(subBytes); err != nil {\n\t\t\treturn err\n\t\t}\n\t}\n", name, field.Message, name)
			return
		}
		fmt.Fprintf(b, "\tif subs, err := msg.GetMessageSlice(%s); err != nil {\n\t\treturn err\n\t} else if subs != nil {\n", key)
		fmt.Fprintf(b, "\t\tm.%s = make([]*%s, len(subs))\n", name, field.Message)
		fmt.Fprintf(b, "\t\tfor i, sub := range subs {\n")
		fmt.Fprintf(b, "\t\t\tsubBytes, err := sub.Encode()\n\t\t\tif err != nil {\n\t\t\t\treturn err\n\t\t\t}\n")
		fmt.Fprintf(b, "\t\t\tm.%s[i] = &%s{}\n\t\t\tif err := m.%s[i].Unmarshal(subBytes); err != nil {\n\t\t\t\treturn err\n\t\t\t}\n\t\t}\n", name, field.Message, name)
		b.WriteString("\t}\n")
		return
	}
	if field.Message != "" && !field.Repeated {
		fmt.Fprintf(b, "\tif sub, ok, err := msg.GetMessage(%s); err != nil {\n\t\treturn err\n\t} else if ok {\n", key)
		fmt.Fprintf(b, "\t\tm.%s = &%s{}\n\t\tif err := m.%s.load(sub); err != nil {\n\t\t\treturn err\n\t\t}\n\t}\n", name, field.Message, name)
		return
	}
	if field.Message != "" && field.Repeated {
		fmt.Fprintf(b, "\tif subs, err := msg.GetMessageSlice(%s); err != nil {\n\t\treturn err\n\t} else if subs != nil {\n", key)
		fmt.Fprintf(b, "\t\tm.%s = make([]*%s, len(subs))\n", name, field.Message)
		fmt.Fprintf(b, "\t\tfor i, sub := range subs {\n")
		fmt.Fprintf(b, "\t\t\tm.%s[i] = &%s{}\n\t\t\tif err := m.%s[i].load(sub); err != nil {\n\t\t\t\treturn err\n\t\t\t}\n\t\t}\n", name, field.Message, name)
		b.WriteString("\t}\n")
		return
	}
	if field.Repeated {
		writeGoLoadSlice(b, f, field, name, key)
		return
	}
	if field.NeedsHas && !field.Required {
		writeGoLoadPresent(b, f, field, name, key)
		return
	}
	writeGoLoadScalar(b, f, field, "m."+name, key)
}

// writeGoLoadOne loads a oneof member guarded by presence. The wrapper is
// allocated only when the member is actually present.
func writeGoLoadOne(b *strings.Builder, f *model.File, m model.Message, field model.Field, wrap string) {
	name := goCamel(field.Name)
	key := fmt.Sprintf("%q", field.Name)
	kind := goCamel(field.Oneof)
	if field.Message != "" && field.Import != "" {
		fmt.Fprintf(b, "\t\tif sub, ok, err := msg.GetMessage(%s); err != nil {\n\t\t\treturn err\n\t\t} else if ok {\n", key)
		fmt.Fprintf(b, "\t\t\tsubBytes, err := sub.Encode()\n\t\t\tif err != nil {\n\t\t\t\treturn err\n\t\t\t}\n")
		fmt.Fprintf(b, "\t\t\tw := &%s{%s: &%s{}}\n\t\t\tif err := w.%s.Unmarshal(subBytes); err != nil {\n\t\t\t\treturn err\n\t\t\t}\n", wrap, name, field.Message, name)
		fmt.Fprintf(b, "\t\t\tm.%s = w\n\t\t}\n", kind)
		return
	}
	if field.Message != "" {
		fmt.Fprintf(b, "\t\tif sub, ok, err := msg.GetMessage(%s); err != nil {\n\t\t\treturn err\n\t\t} else if ok {\n", key)
		fmt.Fprintf(b, "\t\t\tw := &%s{%s: &%s{}}\n\t\t\tif err := w.%s.load(sub); err != nil {\n\t\t\t\treturn err\n\t\t\t}\n", wrap, name, field.Message, name)
		fmt.Fprintf(b, "\t\t\tm.%s = w\n\t\t}\n", kind)
		return
	}
	b.WriteString("\t\tif v, ok, err := ")
	writeGoGetCall(b, field, key)
	b.WriteString("; err != nil {\n\t\t\treturn err\n\t\t} else if ok {\n")
	if t := dstType(f, field, ""); t != "" {
		fmt.Fprintf(b, "\t\t\tw := &%s{%s: %s(v)}\n", wrap, name, t)
	} else {
		fmt.Fprintf(b, "\t\t\tw := &%s{%s: v}\n", wrap, name)
	}
	fmt.Fprintf(b, "\t\t\tm.%s = w\n\t\t}\n", kind)
}

// writeGoLoadPresent loads an optional field only when present. Pointers
// take the address of a converted copy; []byte assigns the copy directly.
func writeGoLoadPresent(b *strings.Builder, f *model.File, field model.Field, name, key string) {
	b.WriteString("\tif v, ok, err := ")
	writeGoGetCall(b, field, key)
	b.WriteString("; err != nil {\n\t\treturn err\n\t} else if ok {\n")
	t := goFieldType(f, field)
	if strings.HasPrefix(t, "*") {
		base := strings.TrimPrefix(t, "*")
		fmt.Fprintf(b, "\t\ttmp := %s(v)\n\t\tm.%s = &tmp\n\t}\n", base, name)
	} else if t == "[]byte" {
		fmt.Fprintf(b, "\t\tm.%s = v\n\t}\n", name)
	} else {
		fmt.Fprintf(b, "\t\tm.%s = v\n\t}\n", name)
	}
}

// writeGoLoadScalar loads an implicit scalar (zero when absent: same result).
func writeGoLoadScalar(b *strings.Builder, f *model.File, field model.Field, dst, key string) {
	b.WriteString("\tif v, ok, err := ")
	writeGoGetCall(b, field, key)
	b.WriteString("; err != nil {\n\t\treturn err\n\t} else if ok {\n")
	if t := dstType(f, field, dst); t != "" {
		fmt.Fprintf(b, "\t\t%s = %s(v)\n\t}\n", dst, t)
	} else {
		fmt.Fprintf(b, "\t\t%s = v\n\t}\n", dst)
	}
}

// dstType converts the getter's int64/uint64 to the destination type.
func dstType(f *model.File, field model.Field, dst string) string {
	_ = f
	_ = dst
	switch field.Kind {
	case protoreflect.Int32Kind, protoreflect.Sint32Kind, protoreflect.Sfixed32Kind:
		return "int32"
	case protoreflect.Uint32Kind, protoreflect.Fixed32Kind:
		return "uint32"
	case protoreflect.EnumKind:
		if field.Enum != "" {
			return field.Enum
		}
		return "int32"
	case protoreflect.FloatKind:
		return "float32"
	default:
		return ""
	}
}

// writeGoGetCall emits the getter invocation returning (v, ok, err).
func writeGoGetCall(b *strings.Builder, field model.Field, key string) {
	switch field.Kind {
	case protoreflect.Int32Kind, protoreflect.Sint32Kind, protoreflect.Sfixed32Kind,
		protoreflect.Int64Kind, protoreflect.Sint64Kind, protoreflect.Sfixed64Kind,
		protoreflect.EnumKind:
		fmt.Fprintf(b, "msg.GetInt(%s)", key)
	case protoreflect.Uint32Kind, protoreflect.Fixed32Kind,
		protoreflect.Uint64Kind, protoreflect.Fixed64Kind:
		fmt.Fprintf(b, "msg.GetUint(%s)", key)
	case protoreflect.BoolKind:
		fmt.Fprintf(b, "msg.GetBool(%s)", key)
	case protoreflect.FloatKind:
		fmt.Fprintf(b, "msg.GetFloat(%s)", key)
	case protoreflect.DoubleKind:
		fmt.Fprintf(b, "msg.GetDouble(%s)", key)
	case protoreflect.StringKind:
		fmt.Fprintf(b, "msg.GetString(%s)", key)
	case protoreflect.BytesKind:
		fmt.Fprintf(b, "msg.GetBytes(%s)", key)
	default:
		fmt.Fprintf(b, "msg.GetInt(%s)", key)
	}
}

// writeGoLoadSlice loads a repeated scalar field.
func writeGoLoadSlice(b *strings.Builder, f *model.File, field model.Field, name, key string) {
	b.WriteString("\tif vs, err := ")
	switch field.Kind {
	case protoreflect.Int32Kind, protoreflect.Sint32Kind, protoreflect.Sfixed32Kind,
		protoreflect.Int64Kind, protoreflect.Sint64Kind, protoreflect.Sfixed64Kind,
		protoreflect.EnumKind:
		fmt.Fprintf(b, "msg.GetIntSlice(%s)", key)
	case protoreflect.Uint32Kind, protoreflect.Fixed32Kind,
		protoreflect.Uint64Kind, protoreflect.Fixed64Kind:
		fmt.Fprintf(b, "msg.GetUintSlice(%s)", key)
	case protoreflect.BoolKind:
		fmt.Fprintf(b, "msg.GetBoolSlice(%s)", key)
	case protoreflect.FloatKind:
		fmt.Fprintf(b, "msg.GetFloatSlice(%s)", key)
	case protoreflect.DoubleKind:
		fmt.Fprintf(b, "msg.GetDoubleSlice(%s)", key)
	case protoreflect.StringKind:
		fmt.Fprintf(b, "msg.GetStringSlice(%s)", key)
	case protoreflect.BytesKind:
		fmt.Fprintf(b, "msg.GetBytesSlice(%s)", key)
	default:
		fmt.Fprintf(b, "msg.GetIntSlice(%s)", key)
	}
	b.WriteString("; err != nil {\n\t\treturn err\n\t} else if vs != nil {\n")
	t := goFieldType(f, field)
	elem := strings.TrimPrefix(t, "[]")
	if field.Kind == protoreflect.EnumKind && field.Enum != "" {
		elem = field.Enum
	}
	if elem == "[]byte" {
		fmt.Fprintf(b, "\t\tm.%s = vs\n\t}\n", name)
		return
	}
	fmt.Fprintf(b, "\t\tm.%s = make(%s, len(vs))\n\t\tfor i, v := range vs {\n\t\t\tm.%s[i] = %s(v)\n\t\t}\n\t}\n",
		name, t, name, elem)
}

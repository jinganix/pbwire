package gen

import (
	"fmt"
	"go/format"
	"sort"
	"strings"

	"pbwire-codegen/internal/model"

	"google.golang.org/protobuf/reflect/protoreflect"
)

// goKeywords are the identifiers a package clause cannot carry.
var goKeywords = map[string]bool{
	"break": true, "case": true, "chan": true, "const": true,
	"continue": true, "default": true, "defer": true, "else": true,
	"fallthrough": true, "for": true, "func": true, "go": true,
	"goto": true, "if": true, "import": true, "interface": true,
	"map": true, "package": true, "range": true, "return": true,
	"select": true, "struct": true, "switch": true, "type": true,
	"var": true,
}

// goIdent turns a derived name into a valid Go identifier: a dep file named
// struct.proto would otherwise emit "package struct" and fail gofmt.
func goIdent(name string) string {
	var b strings.Builder
	for _, r := range name {
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
	if goKeywords[s] {
		return s + "_pbwire"
	}
	return s
}

// goPackage derives the Go package name from the proto base file name.
func goPackage(f *model.File) string {
	return goIdent(base(f))
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
	return goIdent(seg)
}

func goFiles(f *model.File, opt Options) ([]File, error) {
	// Foreign message refs qualify as alias.Type (M mapping) or stay bare
	// (same package); one rewrite up front, use sites untouched.
	qualifyFields(f, func(imp model.Import, local string) string {
		if path, ok := opt.Mappings[imp.Path]; ok {
			return goImportAlias(path) + "." + local
		}
		return local
	})
	// Two files, like Java's message/metadata split: the model holds data
	// types and one-line forwarding only, so it stays replaceable; the meta
	// file holds the field tables. Every wire decision stays in codec.
	// The cshim wiring file joins them only when the whole file is
	// drivable: a skipped field fails cshim emission (loudly, in
	// writeGoCshimRow), and then the file stays on the table engine
	// instead of shipping a half-wired package.
	if !opt.Model {
		return nil, nil
	}
	// Probe first: writeGoCshim writes only its own builder, so a skip
	// failure discards nothing the model and meta already rendered.
	var cshim strings.Builder
	useCshim := writeGoCshim(&cshim, f, opt) == nil
	var models strings.Builder
	writeGoModelHeader(&models, f, opt, useCshim)
	for _, e := range f.Enums {
		writeGoEnum(&models, e)
	}
	for _, m := range f.Messages {
		writeGoMessage(&models, f, m, useCshim)
	}
	var meta strings.Builder
	writeGoMetaHeader(&meta, f, opt)
	for _, m := range f.Messages {
		writeGoTable(&meta, f, m)
	}
	// Canonical gofmt output: struct alignment and indentation stay
	// correct no matter how wide field names get.
	var files []File
	triples := []struct {
		suffix  string
		builder *strings.Builder
	}{{"_pbwire.go", &models}, {"_pbwire_meta.go", &meta}}
	if useCshim {
		triples = append(triples, struct {
			suffix  string
			builder *strings.Builder
		}{"_pbwire_cshim.go", &cshim})
	}
	for _, out := range triples {
		src, err := format.Source([]byte(out.builder.String()))
		if err != nil {
			return nil, err
		}
		files = append(files, File{Name: base(f) + out.suffix, Content: string(src)})
	}
	return files, nil
}

// writeGoModelHeader emits the package clause plus exactly the imports the
// models use: fmt for enum strings and the nil guards, mapped deps for
// foreign field types. Marshal/Unmarshal moved to the cshim wiring file,
// so the model no longer touches the table codec.
func writeGoModelHeader(b *strings.Builder, f *model.File, opt Options, useCshim bool) {
	b.WriteString(headerP(f, "//", "Generated structs over the pbwire table codec, protoc-gen-go shape."))
	fmt.Fprintf(b, "package %s\n\n", goPackage(f))
	if len(f.Messages) == 0 && len(f.Enums) == 0 {
		return // package clause only: nothing to import
	}
	// Build the import list first: an empty block is legal Go but not
	// gofmt-clean, and a cshim file needs none of them.
	var imports []string
	if len(f.Messages) > 0 && !useCshim {
		imports = append(imports, "\t\"pbwire/pbench/dyngo/codec\"\n")
	}
	if len(f.Enums) > 0 {
		imports = append(imports, "\t\"fmt\"\n")
	}
	for _, imp := range f.Imports {
		if path, ok := opt.Mappings[imp.Path]; ok {
			imports = append(imports, fmt.Sprintf("\n\t%s %q\n", goImportAlias(path), path))
		}
	}
	if len(imports) == 0 {
		return
	}
	b.WriteString("import (\n")
	for _, line := range imports {
		b.WriteString(line)
	}
	b.WriteString(")\n\n")
}

// writeGoMetaHeader emits the package clause plus exactly the imports the
// metadata file uses: codec for the tables, mapped deps for foreign rows.
func writeGoMetaHeader(b *strings.Builder, f *model.File, opt Options) {
	b.WriteString(headerP(f, "//", "Field tables for the generated model. Data only: the codec decides every tag, length and presence."))
	fmt.Fprintf(b, "package %s\n\n", goPackage(f))
	if len(f.Messages) == 0 {
		return
	}
	b.WriteString("import (\n\t\"pbwire/pbench/dyngo/codec\"\n")
	if goNeedsUnsafe(f) {
		b.WriteString("\t\"unsafe\"\n")
	}
	for _, imp := range f.Imports {
		if path, ok := opt.Mappings[imp.Path]; ok {
			fmt.Fprintf(b, "\n\t%s %q\n", goImportAlias(path), path)
		}
	}
	b.WriteString(")\n\n")
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

func writeGoMessage(b *strings.Builder, f *model.File, m model.Message, useCshim bool) {
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
	writeGoCodec(b, f, m, useCshim)
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

// writeGoCodec emits Reset. Marshal/Unmarshal live in the cshim wiring file
// next to the registry and fixup they forward to, so the model file stays
// data only. Field numbers and types come from the file's descriptor at
// generation time, so nothing is hand-written.
func writeGoCodec(b *strings.Builder, f *model.File, m model.Message, useCshim bool) {
	fmt.Fprintf(b, "func (m *%s) Reset() {\n\tif m == nil {\n\t\treturn\n\t}\n\t*m = %s{}\n}\n\n", m.Name, m.Name)
	if useCshim {
		// Marshal/Unmarshal live in the cshim wiring file next to the
		// registry and fixup they forward to.
		return
	}
	fmt.Fprintf(b, "func (m *%s) Marshal() ([]byte, error) {\n", m.Name)
	fmt.Fprintf(b, "\treturn codec.EncodeModel(m, %sMeta)\n}\n\n", m.Name)
	fmt.Fprintf(b, "func (m *%s) Unmarshal(wire []byte) error {\n", m.Name)
	fmt.Fprintf(b, "\treturn codec.DecodeModel(wire, m, %sMeta)\n}\n\n", m.Name)
}

// writeGoTable emits one message's data table: one row per proto field in
// ascending field-number order. Every row is field facts (number, wire
// type, kind, presence, repetition) plus trivial getter/setter closures on
// the struct; no wire logic is emitted here.
func writeGoTable(b *strings.Builder, f *model.File, m model.Message) {
	fmt.Fprintf(b, "// %sMeta is the generated DATA table for %s: one row per field.\n", m.Name, m.Full)
	fmt.Fprintf(b, "var %sMeta = codec.NewTable(%q, []*codec.Field{\n", m.Name, m.Full)
	for _, field := range goSorted(m) {
		if field.Skip != "" {
			continue
		}
		writeGoTableRow(b, f, m, field)
	}
	b.WriteString("})\n\n")
}

// writeGoTableRow emits one field's data row. Oneof members key on the
// wrapper type (a set member is present by construction); foreign messages
// cross the package boundary through the sibling's own Marshal/Unmarshal;
// everything else addresses the struct field directly.
func writeGoTableRow(b *strings.Builder, f *model.File, m model.Message, field model.Field) {
	name := goCamel(field.Name)
	fmt.Fprintf(b, "\t{Name: %q, Num: %d, Wire: %s, Kind: %s", field.Name, field.Number, goWireConst(field), goKindConst(field))
	if field.Required {
		b.WriteString(", Required: true")
	}
	switch {
	case field.Oneof != "":
		writeGoTableOneof(b, f, m, field, name)
	case field.Kind == protoreflect.GroupKind:
		writeGoTableMessage(b, f, m, field, name, true)
	case field.Message != "":
		writeGoTableMessage(b, f, m, field, name, false)
	default:
		writeGoTableScalar(b, f, m, field, name)
	}
	b.WriteString("},\n")
}

// goWireConst is the codec wire-type constant for a field's kind.
func goWireConst(field model.Field) string {
	switch field.Kind {
	case protoreflect.Fixed32Kind, protoreflect.Sfixed32Kind, protoreflect.FloatKind:
		return "codec.WireFixed32"
	case protoreflect.Fixed64Kind, protoreflect.Sfixed64Kind, protoreflect.DoubleKind:
		return "codec.WireFixed64"
	case protoreflect.StringKind, protoreflect.BytesKind, protoreflect.MessageKind:
		return "codec.WireLen"
	case protoreflect.GroupKind:
		return "codec.WireStartGroup"
	default:
		return "codec.WireVarint"
	}
}

// goKindConst is the codec table kind for a field's proto kind. Groups ride
// KindMessage with the row's Group flag; enums stay int32 (unknown values
// are preserved, never validated).
func goKindConst(field model.Field) string {
	switch field.Kind {
	case protoreflect.Int32Kind:
		return "codec.KindInt32"
	case protoreflect.Int64Kind:
		return "codec.KindInt64"
	case protoreflect.Uint32Kind:
		return "codec.KindUint32"
	case protoreflect.Uint64Kind:
		return "codec.KindUint64"
	case protoreflect.Sint32Kind:
		return "codec.KindSint32"
	case protoreflect.Sint64Kind:
		return "codec.KindSint64"
	case protoreflect.Fixed32Kind:
		return "codec.KindFixed32"
	case protoreflect.Fixed64Kind:
		return "codec.KindFixed64"
	case protoreflect.Sfixed32Kind:
		return "codec.KindSfixed32"
	case protoreflect.Sfixed64Kind:
		return "codec.KindSfixed64"
	case protoreflect.FloatKind:
		return "codec.KindFloat"
	case protoreflect.DoubleKind:
		return "codec.KindDouble"
	case protoreflect.BoolKind:
		return "codec.KindBool"
	case protoreflect.StringKind:
		return "codec.KindString"
	case protoreflect.BytesKind:
		return "codec.KindBytes"
	case protoreflect.EnumKind:
		return "codec.KindEnum"
	default:
		return "codec.KindMessage"
	}
}

// goScalarGoType is the struct field's Go type for a scalar/enum row.
func goScalarGoType(f *model.File, field model.Field) string {
	if field.Kind == protoreflect.EnumKind && field.Enum != "" {
		return field.Enum
	}
	return goTypeName(f, field)
}

// goFastRow reports whether a field row may carry offset layout facts.
// The engine's fast path excludes oneof members (presence lives in the
// wrapper), foreign rows (the sibling owns the bytes), groups and all
// message kinds (pointer framing stays on closures in v1). Everything
// else — scalars, enums, strings, bytes and repeated scalars — reads
// the struct field directly.
func goFastRow(field model.Field) bool {
	if field.Skip != "" || field.Oneof != "" || field.Import != "" {
		return false
	}
	return field.Kind != protoreflect.GroupKind && field.Kind != protoreflect.MessageKind
}

// goNeedsUnsafe reports whether any row of the file carries offsets.
func goNeedsUnsafe(f *model.File) bool {
	for _, m := range f.Messages {
		for _, field := range m.Fields {
			if goFastRow(field) {
				return true
			}
		}
	}
	return false
}

// writeGoTableFast appends the offset layout columns for rows the engine
// may drive directly: Fast validity, Off value offset, and Has presence
// offset for explicit rows. Message, oneof, foreign and group rows emit
// nothing. Only columns the engine reads are emitted.
func writeGoTableFast(b *strings.Builder, f *model.File, m model.Message, field model.Field, name string) {
	if !goFastRow(field) {
		return
	}
	off := fmt.Sprintf("uint32(unsafe.Offsetof(%s{}.%s))", m.Name, name)
	fmt.Fprintf(b, ", Fast: true, Off: %s", off)
	if field.NeedsHas && !field.Required {
		fmt.Fprintf(b, ", Has: %s", off)
	}
}

// writeGoTableOneof emits a oneof member row: presence lives in the wrapper
// type assertion (a set member is present by construction, zero and empty
// included), values pass through unwrapped, SetOneof rebuilds the wrapper,
// NewMsg allocates message members. Message members normalize typed nils
// to nil so a set-but-empty wrapper still encodes empty.
func writeGoTableOneof(b *strings.Builder, f *model.File, m model.Message, field model.Field, name string) {
	wrap := goWrapper(f, m, field)
	typ := goWrapType(f, field)
	b.WriteString(", Oneof: true,\n")
	if field.Message != "" {
		if field.Kind == protoreflect.GroupKind {
			b.WriteString(", Group: true")
		}
		if field.Import != "" {
			fmt.Fprintf(b, "\t\tForeign: &codec.ForeignOps{Encode: func(m any) ([]byte, error) { return m.(*%s).Marshal() }, Decode: func(m any, w []byte) error { return m.(*%s).Unmarshal(w) } },\n",
				field.Message, field.Message)
		} else {
			fmt.Fprintf(b, "\t\tSub: func() *codec.Table { return %sMeta },\n", field.Message)
		}
		fmt.Fprintf(b, "\t\tGetOneof: func(msg any) (any, bool) { if x, ok := msg.(*%s).%s.(*%s); ok { if x.%s == nil { return nil, true }; return x.%s, true }; return nil, false },\n",
			m.Name, goCamel(field.Oneof), wrap, name, name)
		fmt.Fprintf(b, "\t\tSetOneof: func(msg any, v any) { msg.(*%s).%s = &%s{%s: v.(%s)} },\n",
			m.Name, goCamel(field.Oneof), wrap, name, typ)
		fmt.Fprintf(b, "\t\tNewMsg: func() any { return &%s{} },\n", field.Message)
		return
	}
	if field.Kind == protoreflect.EnumKind {
		enum := goTableValType(f, field)
		fmt.Fprintf(b, "\t\tGetOneof: func(msg any) (any, bool) { if x, ok := msg.(*%s).%s.(*%s); ok { return int32(x.%s), true }; return nil, false },\n",
			m.Name, goCamel(field.Oneof), wrap, name)
		fmt.Fprintf(b, "\t\tSetOneof: func(msg any, v any) { msg.(*%s).%s = &%s{%s: %s(v.(int32))} },\n",
			m.Name, goCamel(field.Oneof), wrap, name, enum)
		return
	}
	fmt.Fprintf(b, "\t\tGetOneof: func(msg any) (any, bool) { if x, ok := msg.(*%s).%s.(*%s); ok { return x.%s, true }; return nil, false },\n",
		m.Name, goCamel(field.Oneof), wrap, name)
	fmt.Fprintf(b, "\t\tSetOneof: func(msg any, v any) { msg.(*%s).%s = &%s{%s: v.(%s)} },\n",
		m.Name, goCamel(field.Oneof), wrap, name, typ)
}

// writeGoTableMessage emits a singular or repeated message/group row. Local
// messages recurse through the sibling table; foreign ones cross through
// the sibling's own methods; groups add the framing flag. Message accessors
// normalize typed nils to nil so the engine's nil checks work.
func writeGoTableMessage(b *strings.Builder, f *model.File, m model.Message, field model.Field, name string, group bool) {
	if group {
		b.WriteString(", Group: true")
	}
	sub := field.Message
	if field.Import != "" {
		fmt.Fprintf(b, ",\n\t\tForeign: &codec.ForeignOps{Encode: func(m any) ([]byte, error) { return m.(*%s).Marshal() }, Decode: func(m any, w []byte) error { return m.(*%s).Unmarshal(w) } },\n",
			sub, sub)
	} else {
		fmt.Fprintf(b, ",\n\t\tSub: func() *codec.Table { return %sMeta },\n", sub)
	}
	if field.Repeated {
		b.WriteString("\t\tRepeated: true,\n")
		fmt.Fprintf(b, "\t\tRepMsgLen: func(msg any) int { return len(msg.(*%s).%s) },\n", m.Name, name)
		fmt.Fprintf(b, "\t\tRepMsgAt: func(msg any, i int) any { if v := msg.(*%s).%s[i]; v == nil { return nil }; return msg.(*%s).%s[i] },\n",
			m.Name, name, m.Name, name)
		fmt.Fprintf(b, "\t\tNewRepMsg: func(msg any) any { m := msg.(*%s); item := &%s{}; m.%s = append(m.%s, item); return item },\n",
			m.Name, sub, name, name)
		return
	}
	fmt.Fprintf(b, "\t\tGetMsg: func(msg any) any { if v := msg.(*%s).%s; v == nil { return nil }; return msg.(*%s).%s },\n",
		m.Name, name, m.Name, name)
	fmt.Fprintf(b, "\t\tGetOrCreateMsg: func(msg any) any { m := msg.(*%s); if m.%s == nil { m.%s = &%s{} }; return m.%s },\n",
		m.Name, name, name, sub, name)
}

// writeGoTableScalar emits a singular or repeated scalar/enum/string/bytes
// row. Pointer presence (proto3 optional, proto2 optional) rides the
// Explicit flag with optional accessors; required fields always emit;
// everything else skips the zero value in the engine.
func writeGoTableScalar(b *strings.Builder, f *model.File, m model.Message, field model.Field, name string) {
	if field.Repeated {
		b.WriteString(", Repeated: true")
		if field.Packed {
			b.WriteString(", Packed: true")
		}
		writeGoTableFast(b, f, m, field, name)
		b.WriteString(",\n")
		writeGoTableRepAccessors(b, f, m, field, name)
		return
	}
	if field.NeedsHas && !field.Required && field.Message == "" {
		b.WriteString(", Explicit: true")
		writeGoTableFast(b, f, m, field, name)
		b.WriteString(",\n")
		writeGoTableOptAccessors(b, f, m, field, name)
		return
	}
	writeGoTableFast(b, f, m, field, name)
	b.WriteString(",\n")
	writeGoTablePlainAccessors(b, f, m, field, name)
}

// goTableSuffix maps a proto kind to the accessor name suffix in the table
// engine (I32, U64, Sfx32, F32, Str, ...).
func goTableSuffix(field model.Field) string {
	switch field.Kind {
	case protoreflect.Int32Kind:
		return "I32"
	case protoreflect.Int64Kind:
		return "I64"
	case protoreflect.Uint32Kind:
		return "U32"
	case protoreflect.Uint64Kind:
		return "U64"
	case protoreflect.Sint32Kind:
		return "S32"
	case protoreflect.Sint64Kind:
		return "S64"
	case protoreflect.Fixed32Kind:
		return "Fx32"
	case protoreflect.Fixed64Kind:
		return "Fx64"
	case protoreflect.Sfixed32Kind:
		return "Sfx32"
	case protoreflect.Sfixed64Kind:
		return "Sfx64"
	case protoreflect.FloatKind:
		return "F32"
	case protoreflect.DoubleKind:
		return "F64"
	case protoreflect.BoolKind:
		return "Bool"
	case protoreflect.StringKind:
		return "Str"
	case protoreflect.BytesKind:
		return "Bytes"
	case protoreflect.EnumKind:
		return "Enum"
	default:
		return "I32"
	}
}

// goTableValType is the Go value type a row's accessors carry: the named
// enum for enums, the struct field type otherwise.
func goTableValType(f *model.File, field model.Field) string {
	if field.Kind == protoreflect.EnumKind && field.Enum != "" {
		return field.Enum
	}
	return goTypeName(f, field)
}

// writeGoTablePlainAccessors emits plain getter/setter closures for an
// implicitly present singular scalar: the engine skips the zero value.
func writeGoTablePlainAccessors(b *strings.Builder, f *model.File, m model.Message, field model.Field, name string) {
	sfx := goTableSuffix(field)
	typ := goTableValType(f, field)
	if field.Kind == protoreflect.EnumKind {
		fmt.Fprintf(b, "\t\tGetEnum: func(msg any) int32 { return int32(msg.(*%s).%s) },\n", m.Name, name)
		fmt.Fprintf(b, "\t\tSetEnum: func(msg any, v int32) { msg.(*%s).%s = %s(v) },\n", m.Name, name, typ)
		return
	}
	fmt.Fprintf(b, "\t\tGet%s: func(msg any) %s { return msg.(*%s).%s },\n", sfx, typ, m.Name, name)
	fmt.Fprintf(b, "\t\tSet%s: func(msg any, v %s) { msg.(*%s).%s = v },\n", sfx, typ, m.Name, name)
}

// writeGoTableOptAccessors emits pointer getter/setter closures for an
// explicitly present singular scalar: presence is the pointer, not the
// value, so zero and empty still encode.
func writeGoTableOptAccessors(b *strings.Builder, f *model.File, m model.Message, field model.Field, name string) {
	sfx := goTableSuffix(field)
	typ := goTableValType(f, field)
	if field.Kind == protoreflect.EnumKind {
		fmt.Fprintf(b, "\t\tGetOptEnum: func(msg any) *int32 { p := msg.(*%s).%s; if p == nil { return nil }; v := int32(*p); return &v },\n", m.Name, name)
		fmt.Fprintf(b, "\t\tSetOptEnum: func(msg any, v int32) { tmp := %s(v); msg.(*%s).%s = &tmp },\n", typ, m.Name, name)
		return
	}
	fmt.Fprintf(b, "\t\tGetOpt%s: func(msg any) *%s { return msg.(*%s).%s },\n", sfx, typ, m.Name, name)
	fmt.Fprintf(b, "\t\tSetOpt%s: func(msg any, v %s) { tmp := v; msg.(*%s).%s = &tmp },\n", sfx, typ, m.Name, name)
}

// writeGoTableRepAccessors emits slice getter/append closures for a repeated
// scalar row. Enum slices convert element-wise (the slice type is named);
// everything else addresses the slice directly.
func writeGoTableRepAccessors(b *strings.Builder, f *model.File, m model.Message, field model.Field, name string) {
	sfx := goTableSuffix(field)
	typ := goTableValType(f, field)
	if field.Kind == protoreflect.EnumKind {
		fmt.Fprintf(b, "\t\tGetRepEnum: func(msg any) []int32 { vs := msg.(*%s).%s; out := make([]int32, len(vs)); for i, v := range vs { out[i] = int32(v) }; return out },\n",
			m.Name, name)
		fmt.Fprintf(b, "\t\tAppendEnum: func(msg any, v int32) { m := msg.(*%s); m.%s = append(m.%s, %s(v)) },\n",
			m.Name, name, name, typ)
		return
	}
	fmt.Fprintf(b, "\t\tGetRep%s: func(msg any) []%s { return msg.(*%s).%s },\n", sfx, typ, m.Name, name)
	fmt.Fprintf(b, "\t\tAppend%s: func(msg any, v %s) { m := msg.(*%s); m.%s = append(m.%s, v) },\n", sfx, typ, m.Name, name, name)
}

// goSorted returns the message fields in wire order (ascending field
// number). Generated tables list rows in field-number order so the engine
// emits byte-identical output (oneof1.bin pins name=1 before text=3).
func goSorted(m model.Message) []model.Field {
	out := append([]model.Field(nil), m.Fields...)
	sort.Slice(out, func(i, j int) bool { return out[i].Number < out[j].Number })
	return out
}

// goCshimKind maps a proto kind onto the layout kind. Enums ride int32:
// unknown values are preserved, never validated.
func goCshimKind(field model.Field) (string, error) {
	switch field.Kind {
	case protoreflect.Int32Kind, protoreflect.EnumKind:
		return "layout.KindInt32", nil
	case protoreflect.Int64Kind:
		return "layout.KindInt64", nil
	case protoreflect.Uint32Kind:
		return "layout.KindUint32", nil
	case protoreflect.Uint64Kind:
		return "layout.KindUint64", nil
	case protoreflect.Sint32Kind:
		return "layout.KindSint32", nil
	case protoreflect.Sint64Kind:
		return "layout.KindSint64", nil
	case protoreflect.Fixed32Kind:
		return "layout.KindFixed32", nil
	case protoreflect.Fixed64Kind:
		return "layout.KindFixed64", nil
	case protoreflect.Sfixed32Kind:
		return "layout.KindSfixed32", nil
	case protoreflect.Sfixed64Kind:
		return "layout.KindSfixed64", nil
	case protoreflect.FloatKind:
		return "layout.KindFloat", nil
	case protoreflect.DoubleKind:
		return "layout.KindDouble", nil
	case protoreflect.BoolKind:
		return "layout.KindBool", nil
	case protoreflect.StringKind:
		return "layout.KindString", nil
	case protoreflect.BytesKind:
		return "layout.KindBytes", nil
	case protoreflect.MessageKind, protoreflect.GroupKind:
		return "layout.KindMessage", nil
	default:
		return "", fmt.Errorf("cshim: no layout kind for field %s of kind %s", field.Name, field.Kind)
	}
}

// goCshimWire is the layout wire-type constant for a field's kind.
func goCshimWire(field model.Field) string {
	switch field.Kind {
	case protoreflect.Fixed32Kind, protoreflect.Sfixed32Kind, protoreflect.FloatKind:
		return "layout.WireFixed32"
	case protoreflect.Fixed64Kind, protoreflect.Sfixed64Kind, protoreflect.DoubleKind:
		return "layout.WireFixed64"
	case protoreflect.StringKind, protoreflect.BytesKind, protoreflect.MessageKind:
		return "layout.WireLen"
	case protoreflect.GroupKind:
		return "layout.WireStartGroup"
	default:
		return "layout.WireVarint"
	}
}

// goCshimSub resolves the registry index of a message-typed field:
// by full proto name first, by local class name for map entries (which
// carry no Full).
func goCshimSub(f *model.File, field model.Field, regIdx map[string]int) (int32, error) {
	if field.Kind != protoreflect.MessageKind && field.Kind != protoreflect.GroupKind {
		return -1, nil
	}
	if i, ok := regIdx[field.Full]; ok && field.Full != "" {
		return int32(i), nil
	}
	if i, ok := regIdx[field.Message]; ok {
		return int32(i), nil
	}
	return -1, fmt.Errorf("cshim: message %s of field %s not in file %s", field.Message, field.Name, f.Path)
}

// writeGoCshimRow emits one layout.Row literal. Every shape has a mapping;
// model-level skips and unknown shapes fail loudly instead of emitting a
// silent subset, because the generated file names struct fields and a
// missing row would silently change the wire.
func writeGoCshimRow(b *strings.Builder, f *model.File, m model.Message, field model.Field, regIdx map[string]int, slots []string) error {
	if field.Skip != "" {
		return fmt.Errorf("cshim: field %s.%s skipped in model (%s)", m.Full, field.Name, field.Skip)
	}
	// A message from another file cannot be walked: this file's C codec root
	// only knows its own registry, so the whole file stays on the table
	// engine (the runtime rejects a foreign row at New time anyway).
	if field.Import != "" && (field.Kind == protoreflect.MessageKind || field.Kind == protoreflect.GroupKind) {
		return fmt.Errorf("cshim: field %s.%s comes from %s", m.Full, field.Name, field.Import)
	}
	kind, err := goCshimKind(field)
	if err != nil {
		return err
	}
	fmt.Fprintf(b, "\t\t{Fnum: %d, Kind: %s, Wire: %s", field.Number, kind, goCshimWire(field))
	if field.Repeated {
		b.WriteString(", Repeated: true")
		if field.Packed {
			b.WriteString(", Packed: true")
		}
	}
	explicit := field.NeedsHas && !field.Required && field.Oneof == ""
	if explicit {
		b.WriteString(", Explicit: true")
	}
	if field.Required {
		b.WriteString(", Required: true")
	}
	if field.Kind == protoreflect.GroupKind {
		b.WriteString(", Group: true")
	}
	name := goCamel(field.Name)
	off := fmt.Sprintf("uint32(unsafe.Offsetof(%s{}.%s))", m.Name, name)
	if field.Oneof != "" {
		wrap := goWrapper(f, m, field)
		fmt.Fprintf(b, ", Offset: uint32(unsafe.Offsetof(%s{}.%s))", m.Name, goCamel(field.Oneof))
		// The itab is read under the field's own interface type: a named
		// interface word differs from the any word for the same pointer.
		fmt.Fprintf(b, ", Itab: layout.ItabFor[is%s_%s]((*%s)(nil))", m.Name, field.Oneof, wrap)
		fmt.Fprintf(b, ", Ogrp: %d", goOneofSlotIndex(slots, field.Oneof))
	} else {
		fmt.Fprintf(b, ", Offset: %s", off)
		if explicit {
			fmt.Fprintf(b, ", Has: %s", off)
		}
	}
	sub, err := goCshimSub(f, field, regIdx)
	if err != nil {
		return err
	}
	fmt.Fprintf(b, ", Sub: %d", sub)
	b.WriteString("},\n")
	return nil
}

// goOneofSlots orders a message's oneofs first-seen over field-number
// order. Rows and fixup share it, so staging slots agree by construction.
func goOneofSlots(m model.Message) []string {
	var out []string
	seen := map[string]bool{}
	for _, field := range goSorted(m) {
		if field.Oneof == "" || seen[field.Oneof] {
			continue
		}
		seen[field.Oneof] = true
		out = append(out, field.Oneof)
	}
	return out
}

func goOneofSlotIndex(slots []string, oneof string) int {
	for i, s := range slots {
		if s == oneof {
			return i
		}
	}
	return -1
}

// writeGoCshimMsg emits one layout.Msg literal: full name, struct size
// for the C arena, and its rows in field-number order.
func writeGoCshimMsg(b *strings.Builder, f *model.File, m model.Message, regIdx map[string]int) error {
	slots := goOneofSlots(m)
	fmt.Fprintf(b, "\t{Full: %q, Size: uintptr(unsafe.Sizeof(%s{})), Rows: []layout.Row{\n", m.Full, m.Name)
	for _, field := range goSorted(m) {
		if err := writeGoCshimRow(b, f, m, field, regIdx, slots); err != nil {
			return err
		}
	}
	b.WriteString("\t}},\n")
	return nil
}

// writeGoCshimCopyField emits one field's materialization: raw C-filled
// struct memory in, fully owned Go values out. All stores are typed, so
// every pointer write keeps its write barrier; no unsafe appears here.
func writeGoCshimCopyField(b *strings.Builder, f *model.File, m model.Message, field model.Field) error {
	name := goCamel(field.Name)
	if field.Oneof != "" {
		return writeGoCshimCopyOneof(b, f, m, field, name)
	}
	if field.Kind == protoreflect.MessageKind || field.Kind == protoreflect.GroupKind {
		return writeGoCshimCopyMessage(b, f, m, field, name)
	}
	switch field.Kind {
	case protoreflect.StringKind:
		if field.Repeated {
			fmt.Fprintf(b, "\tif len(src.%s) != 0 {\n\t\tdst.%s = make([]string, 0, len(src.%s))\n\t\tfor _, s := range src.%s {\n\t\t\tdst.%s = append(dst.%s, a.AddString(s))\n\t\t}\n\t}\n", name, name, name, name, name, name)
		} else if field.NeedsHas && !field.Required {
			fmt.Fprintf(b, "\tif src.%s != nil {\n\t\tv := a.AddString(*src.%s)\n\t\tdst.%s = &v\n\t}\n", name, name, name)
		} else {
			fmt.Fprintf(b, "\tdst.%s = a.AddString(src.%s)\n", name, name)
		}
	case protoreflect.BytesKind:
		if field.Repeated {
			fmt.Fprintf(b, "\tif len(src.%s) != 0 {\n\t\tdst.%s = make([][]byte, 0, len(src.%s))\n\t\tfor _, p := range src.%s {\n\t\t\tdst.%s = append(dst.%s, a.AddBytes(p))\n\t\t}\n\t}\n", name, name, name, name, name, name)
		} else if field.NeedsHas && !field.Required {
			fmt.Fprintf(b, "\tif src.%s != nil {\n\t\tv := a.AddBytes(*src.%s)\n\t\tdst.%s = &v\n\t}\n", name, name, name)
		} else {
			fmt.Fprintf(b, "\tdst.%s = a.AddBytes(src.%s)\n", name, name)
		}
	default:
		if field.Repeated {
			fmt.Fprintf(b, "\tif len(src.%s) != 0 {\n\t\tdst.%s = append([]%s(nil), src.%s...)\n\t}\n", name, name, goTableValType(f, field), name)
		} else if field.NeedsHas && !field.Required {
			fmt.Fprintf(b, "\tif src.%s != nil {\n\t\tv := *src.%s\n\t\tdst.%s = &v\n\t}\n", name, name, name)
		} else {
			fmt.Fprintf(b, "\tdst.%s = src.%s\n", name, name)
		}
	}
	return nil
}

// writeGoCshimCopyOneof emits one oneof member: presence lives in the
// wrapper, so even a zero or empty member re-wraps on the way out.
func writeGoCshimCopyOneof(b *strings.Builder, f *model.File, m model.Message, field model.Field, name string) error {
	wrap := goWrapper(f, m, field)
	iname := goCamel(field.Oneof)
	payload := func(v string) string {
		switch field.Kind {
		case protoreflect.StringKind:
			return "a.AddString(" + v + ")"
		case protoreflect.BytesKind:
			return "a.AddBytes(" + v + ")"
		default:
			return v
		}
	}
	if field.Kind == protoreflect.MessageKind || field.Kind == protoreflect.GroupKind {
		fmt.Fprintf(b, "\tif x, ok := src.%s.(*%s); ok {\n", iname, wrap)
		fmt.Fprintf(b, "\t\tvar cp %s\n", goWrapType(f, field))
		fmt.Fprintf(b, "\t\tif x.%s != nil {\n", name)
		fmt.Fprintf(b, "\t\t\tcp = &%s{}\n", strings.TrimPrefix(goWrapType(f, field), "*"))
		fmt.Fprintf(b, "\t\t\t%sCshimCopy(cp, x.%s, a)\n", strings.TrimPrefix(goWrapType(f, field), "*"), name)
		fmt.Fprintf(b, "\t\t}\n\t\tdst.%s = &%s{%s: cp}\n\t}\n", iname, wrap, name)
		return nil
	}
	fmt.Fprintf(b, "\tif x, ok := src.%s.(*%s); ok {\n", iname, wrap)
	fmt.Fprintf(b, "\t\tdst.%s = &%s{%s: %s}\n\t}\n", iname, wrap, name, payload("x."+name))
	return nil
}

// goCshimPayload renders one staged oneof payload: scalar kinds convert
// the staged bits, strings/bytes alias the wire (the copy pass owns them
// after), messages cast the C-arena submessage. The bool reports whether
// the payload reads the staged scalar (else the pointer/length). Shapes
// without a mapping fail the emission instead of wrapping garbage.
func goCshimPayload(f *model.File, field model.Field) (string, bool, error) {
	switch field.Kind {
	case protoreflect.Int32Kind, protoreflect.Sint32Kind, protoreflect.Sfixed32Kind:
		return "int32(scalar)", true, nil
	case protoreflect.EnumKind:
		if field.Enum != "" {
			return field.Enum + "(scalar)", true, nil
		}
		return "int32(scalar)", true, nil
	case protoreflect.Int64Kind, protoreflect.Sint64Kind, protoreflect.Sfixed64Kind:
		return "int64(scalar)", true, nil
	case protoreflect.Uint32Kind, protoreflect.Fixed32Kind:
		return "uint32(scalar)", true, nil
	case protoreflect.Uint64Kind, protoreflect.Fixed64Kind:
		return "scalar", true, nil
	case protoreflect.FloatKind:
		return "*(*float32)(unsafe.Pointer(&scalar))", true, nil
	case protoreflect.DoubleKind:
		return "*(*float64)(unsafe.Pointer(&scalar))", true, nil
	case protoreflect.BoolKind:
		return "scalar != 0", true, nil
	case protoreflect.StringKind:
		return "unsafe.String((*byte)(ptr), ln)", false, nil
	case protoreflect.BytesKind:
		return "unsafe.Slice((*byte)(ptr), ln)", false, nil
	case protoreflect.MessageKind, protoreflect.GroupKind:
		return "(*" + strings.TrimPrefix(goTypeName(f, field), "*") + ")(ptr)", false, nil
	default:
		return "", false, fmt.Errorf("cshim: no staged payload for field %s of kind %s", field.Name, field.Kind)
	}
}

// writeGoCshimFixup emits one message's stage wrapper: staged oneof members
// become typed wrappers on the scratch struct; strings alias the wire and
// arena submessages stay C-owned until the copy pass. Slot order is the
// shared goOneofSlots order, so rows and fixup agree by construction.
// Messages without oneofs get an empty body so call sites stay uniform.
func writeGoCshimFixup(b *strings.Builder, f *model.File, m model.Message) error {
	fmt.Fprintf(b, "// %sCshimFixup wraps staged oneof members into typed wrappers.\n", m.Name)
	fmt.Fprintf(b, "func %sCshimFixup(raw *%s, c *pbwrt.Codec) {\n", m.Name, m.Name)
	for _, slot := range goOneofSlots(m) {
		var members []model.Field
		for _, field := range goSorted(m) {
			if field.Oneof == slot {
				members = append(members, field)
			}
		}
		useScalar, usePtr := false, false
		payloads := make([]string, len(members))
		for i, field := range members {
			payload, scalar, err := goCshimPayload(f, field)
			if err != nil {
				return err
			}
			payloads[i] = payload
			if scalar {
				useScalar = true
			} else {
				usePtr = true
			}
		}
		sv, pv, lv := "_", "_", "_"
		if useScalar {
			sv = "scalar"
		}
		if usePtr {
			pv, lv = "ptr", "ln"
		}
		idx := goOneofSlotIndex(goOneofSlots(m), slot)
		iname := goCamel(slot)
		fmt.Fprintf(b, "\tif fnum, _, %s, %s, %s, ok := c.Stage(%d); ok {\n", sv, pv, lv, idx)
		b.WriteString("\t\tswitch fnum {\n\t\tcase 0:\n")
		fmt.Fprintf(b, "\t\t\traw.%s = nil\n", iname)
		for i, field := range members {
			wrap := goWrapper(f, m, field)
			fname := goCamel(field.Name)
			if field.Kind == protoreflect.MessageKind || field.Kind == protoreflect.GroupKind {
				fmt.Fprintf(b, "\t\tcase %d:\n\t\t\tif ptr != nil {\n", field.Number)
				fmt.Fprintf(b, "\t\t\t\traw.%s = &%s{%s: %s}\n\t\t\t}\n", iname, wrap, fname, payloads[i])
				continue
			}
			fmt.Fprintf(b, "\t\tcase %d:\n\t\t\traw.%s = &%s{%s: %s}\n", field.Number, iname, wrap, fname, payloads[i])
		}
		b.WriteString("\t\t}\n\t}\n")
	}
	b.WriteString("}\n\n")
	return nil
}

// writeGoCshimCopyMessage emits singular and repeated message/group
// materialization, including map entries (repeated entry structs).
func writeGoCshimCopyMessage(b *strings.Builder, f *model.File, m model.Message, field model.Field, name string) error {
	sub := field.Message
	if field.Repeated {
		fmt.Fprintf(b, "\tif len(src.%s) != 0 {\n", name)
		fmt.Fprintf(b, "\t\tdst.%s = make([]*%s, len(src.%s))\n", name, sub, name)
		fmt.Fprintf(b, "\t\tfor i, s := range src.%s {\n", name)
		fmt.Fprintf(b, "\t\t\tif s == nil {\n\t\t\t\tcontinue\n\t\t\t}\n")
		fmt.Fprintf(b, "\t\t\tcp := &%s{}\n", sub)
		fmt.Fprintf(b, "\t\t\t%sCshimCopy(cp, s, a)\n", sub)
		fmt.Fprintf(b, "\t\t\tdst.%s[i] = cp\n\t\t}\n\t}\n", name)
		return nil
	}
	fmt.Fprintf(b, "\tif src.%s != nil {\n", name)
	fmt.Fprintf(b, "\t\tdst.%s = &%s{}\n", name, sub)
	fmt.Fprintf(b, "\t\t%sCshimCopy(dst.%s, src.%s, a)\n\t}\n", sub, name, name)
	return nil
}

// cshimCodecVar names the per-message cached runtime codec: unexported,
// one per message because the codec root is fixed at build time.
func cshimCodecVar(m model.Message) string {
	return strings.ToLower(m.Name[:1]) + m.Name[1:] + "CshimCodec"
}

// writeGoCshimCodec emits the cached codec plus the Marshal/Unmarshal
// forwarding: one cgo crossing per call, layout and fixup generated above.
// A bad generated row fails once at first use, loudly.
func writeGoCshimCodec(b *strings.Builder, f *model.File, m model.Message) error {
	cv := cshimCodecVar(m)
	fmt.Fprintf(b, "var %s = sync.OnceValue(func() *pbwrt.Codec {\n", cv)
	fmt.Fprintf(b, "\tc, err := pbwrt.NewCodecFromLayout(CshimRegistry(), %q)\n", m.Full)
	b.WriteString("\tif err != nil {\n\t\tpanic(err)\n\t}\n\treturn c\n})\n\n")
	fmt.Fprintf(b, "func (m *%s) Marshal() ([]byte, error) {\n", m.Name)
	fmt.Fprintf(b, "\tif m == nil {\n\t\treturn []byte{}, nil\n\t}\n")
	fmt.Fprintf(b, "\tout, err := %s().EncodeRaw(unsafe.Pointer(m))\n", cv)
	b.WriteString("\truntime.KeepAlive(m)\n\treturn out, err\n}\n\n")
	fmt.Fprintf(b, "func (m *%s) Unmarshal(wire []byte) error {\n", m.Name)
	fmt.Fprintf(b, "\tif m == nil {\n\t\treturn errors.New(%q)\n\t}\n", m.Full+": Unmarshal into nil message")
	fmt.Fprintf(b, "\tvar raw %s\n", m.Name)
	fmt.Fprintf(b, "\tc := %s()\n", cv)
	fmt.Fprintf(b, "\tvar out %s\n", m.Name)
	// DecodeInto holds the codec lock across decode, fixup and copy: the
	// staged oneof slots and the arena belong to this decode only.
	b.WriteString("\terr := c.DecodeInto(unsafe.Pointer(&raw), wire, func(strBytes int) {\n")
	b.WriteString("\t\tvar a layout.Arena\n\t\ta.Reset(strBytes)\n")
	fmt.Fprintf(b, "\t\t%sCshimFixup(&raw, c)\n", m.Name)
	fmt.Fprintf(b, "\t\t%sCshimCopy(&out, &raw, &a)\n", m.Name)
	b.WriteString("\t})\n\tif err != nil {\n\t\treturn err\n\t}\n")
	b.WriteString("\t*m = out\n\treturn nil\n}\n\n")
	return nil
}

// writeGoCshimCopy emits one message's typed copy function.
func writeGoCshimCopy(b *strings.Builder, f *model.File, m model.Message) error {
	fmt.Fprintf(b, "// %sCshimCopy materializes src into dst: src may alias C-filled\n// memory (raw string headers, C-arena submessages), dst ends fully\n// owned by Go. All stores are typed.\n", m.Name)
	fmt.Fprintf(b, "func %sCshimCopy(dst, src *%s, a *layout.Arena) {\n", m.Name, m.Name)
	for _, field := range goSorted(m) {
		if field.Skip != "" {
			return fmt.Errorf("cshim: field %s.%s skipped in model (%s)", m.Full, field.Name, field.Skip)
		}
		if err := writeGoCshimCopyField(b, f, m, field); err != nil {
			return err
		}
	}
	b.WriteString("}\n\n")
	return nil
}

// goCshimSubRepeated names the first message that is both named by a field
// of another message and carries repeated rows. The C decode publishes its
// repeat buffers for the root table only, so a message used as a sub table
// cannot have repeated fields: such a file stays on the table engine.
func goCshimSubRepeated(f *model.File) string {
	subs := map[string]bool{}
	for _, m := range f.Messages {
		for _, field := range m.Fields {
			if field.Import != "" {
				continue
			}
			if field.Kind == protoreflect.MessageKind || field.Kind == protoreflect.GroupKind {
				subs[field.Message] = true
			}
		}
	}
	for _, m := range f.Messages {
		if !subs[m.Name] {
			continue
		}
		for _, field := range m.Fields {
			if field.Repeated {
				return m.Full
			}
		}
	}
	return ""
}

// writeGoCshim emits the cshim layout file body: the per-file message
// registry, one typed copy per message, and the generic allocator used
// by decode paths that cannot name the type.
func writeGoCshim(b *strings.Builder, f *model.File, opt Options) error {
	if full := goCshimSubRepeated(f); full != "" {
		return fmt.Errorf("cshim: sub table %s has repeated rows", full)
	}
	regIdx := map[string]int{}
	for i, m := range f.Messages {
		regIdx[m.Full] = i
		regIdx[m.Name] = i
	}
	b.WriteString(headerP(f, "//", "Cshim layout over the generated structs: field facts plus offsets for the Erlang-style C codec."))
	fmt.Fprintf(b, "package %s\n\n", goPackage(f))
	// pbwrt aliases the engine package: its name collides with the standard
	// library's runtime, which the generated Marshal needs for KeepAlive.
	b.WriteString("import (\n\t\"errors\"\n\tpbwrt \"pbwire/runtime\"\n\t\"pbwire/runtime/layout\"\n\t\"runtime\"\n\t\"sync\"\n\t\"unsafe\"\n")
	for _, imp := range f.Imports {
		if path, ok := opt.Mappings[imp.Path]; ok {
			fmt.Fprintf(b, "\n\t%s %q\n", goImportAlias(path), path)
		}
	}
	b.WriteString(")\n\n")
	b.WriteString("// CshimRegistry lists every message in this file for the C codec:\n// field facts plus struct layout, indexes stable for Sub references.\n")
	b.WriteString("func CshimRegistry() []layout.Msg {\n\treturn []layout.Msg{\n")
	for _, m := range f.Messages {
		if err := writeGoCshimMsg(b, f, m, regIdx); err != nil {
			return err
		}
	}
	b.WriteString("\t}\n}\n\n")
	for _, m := range f.Messages {
		if err := writeGoCshimCopy(b, f, m); err != nil {
			return err
		}
	}
	for _, m := range f.Messages {
		if err := writeGoCshimFixup(b, f, m); err != nil {
			return err
		}
	}
	for _, m := range f.Messages {
		if err := writeGoCshimCodec(b, f, m); err != nil {
			return err
		}
	}
	b.WriteString("// CshimNew allocates a fresh message by full proto name for generic\n// decode paths that cannot name the type.\n")
	b.WriteString("func CshimNew(full string) any {\n\tswitch full {\n")
	for _, m := range f.Messages {
		fmt.Fprintf(b, "\tcase %q: return &%s{}\n", m.Full, m.Name)
	}
	b.WriteString("\t}\n\treturn nil\n}\n")
	return nil
}

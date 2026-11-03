// Package gogen generates Go structs plus straight-line size, encode and
// decode methods for the pbwire Go runtime. The generated code owns the
// message representation -- plain Go structs, pointers where a field has
// explicit presence, slices for repeated fields -- and writes wire bytes
// directly; nothing walks a descriptor table at run time, nothing crosses cgo
// and the runtime package has no dependencies.
//
// Coverage: every scalar kind, string, bytes, enums, messages, packed and
// unpacked repetition, proto2 required/optional, proto3 optional and editions
// presence. Map, oneof, group and extension fields have no generated
// representation and are rejected with an error, and so are message/enum types
// from another .proto file (the generator has no import path for them).
package gogen

import (
	"fmt"
	"go/format"
	"strconv"
	"strings"

	"google.golang.org/protobuf/reflect/protoreflect"
)

// DefaultRuntimeImport is where generated code imports the wire runtime from.
// It matches this repository's Go module; other projects pass
// runtime_import=<their import path>.
const DefaultRuntimeImport = "pbwire/runtime"

// Options configures one generator run.
type Options struct {
	RuntimeImport string
}

// Generate returns the output file name and content for one proto file. The
// output keeps the proto file's path and gets a .pbwire.go suffix.
func Generate(fd protoreflect.FileDescriptor, opts Options) (string, string, error) {
	if opts.RuntimeImport == "" {
		opts.RuntimeImport = DefaultRuntimeImport
	}
	g := &generator{
		fd:          fd,
		opts:        opts,
		runtime:     "pbwire",
		hasRequired: map[protoreflect.FullName]bool{},
	}
	if err := g.collect(); err != nil {
		return "", "", err
	}
	name := strings.TrimSuffix(fd.Path(), ".proto") + ".pbwire.go"
	var b strings.Builder
	g.writeHeader(&b)
	g.writeEnums(&b)
	for _, m := range g.messages {
		if err := g.writeMessage(&b, m); err != nil {
			return "", "", err
		}
	}
	// Generated files are checked in, so emit gofmt-clean output.
	src, err := format.Source([]byte(b.String()))
	if err != nil {
		return "", "", fmt.Errorf("%s: format generated source: %w", fd.Path(), err)
	}
	return name, string(src), nil
}

type generator struct {
	fd       protoreflect.FileDescriptor
	opts     Options
	pkg      string
	runtime  string // import alias of the runtime package in generated code
	messages []protoreflect.MessageDescriptor
	enums    []protoreflect.EnumDescriptor
	needMath bool
	// hasRequired marks messages that carry a required field or reach one
	// through a message field; only those get a checkRequired method.
	hasRequired map[protoreflect.FullName]bool
}

func (g *generator) collect() error {
	g.pkg = packageName(g.fd)
	for i := 0; i < g.fd.Enums().Len(); i++ {
		g.enums = append(g.enums, g.fd.Enums().Get(i))
	}
	var walk func(msgs protoreflect.MessageDescriptors) error
	walk = func(msgs protoreflect.MessageDescriptors) error {
		for i := 0; i < msgs.Len(); i++ {
			m := msgs.Get(i)
			if m.IsMapEntry() {
				continue
			}
			if err := g.checkMessage(m); err != nil {
				return err
			}
			for j := 0; j < m.Enums().Len(); j++ {
				g.enums = append(g.enums, m.Enums().Get(j))
			}
			g.messages = append(g.messages, m)
			if err := walk(m.Messages()); err != nil {
				return err
			}
		}
		return nil
	}
	if err := walk(g.fd.Messages()); err != nil {
		return err
	}
	g.needMath = g.usesFloats()
	g.markRequired()
	if g.collides(g.runtime) {
		g.runtime = "pbwire_"
	}
	return nil
}

// checkMessage rejects what the generated representation cannot express.
func (g *generator) checkMessage(m protoreflect.MessageDescriptor) error {
	fields := m.Fields()
	for i := 0; i < fields.Len(); i++ {
		f := fields.Get(i)
		if f.IsMap() {
			return fmt.Errorf("%s: map fields are not supported", f.FullName())
		}
		if od := f.ContainingOneof(); od != nil && !od.IsSynthetic() {
			return fmt.Errorf("%s: oneof fields are not supported", f.FullName())
		}
		switch f.Kind() {
		case protoreflect.GroupKind:
			return fmt.Errorf("%s: group fields are not supported", f.FullName())
		case protoreflect.MessageKind:
			if f.Message().ParentFile().Path() != g.fd.Path() {
				return fmt.Errorf("%s: message type %s comes from another file; cross-file references are not supported", f.FullName(), f.Message().FullName())
			}
		case protoreflect.EnumKind:
			if f.Enum().ParentFile().Path() != g.fd.Path() {
				return fmt.Errorf("%s: enum type %s comes from another file; cross-file references are not supported", f.FullName(), f.Enum().FullName())
			}
		}
	}
	return nil
}

func (g *generator) usesFloats() bool {
	for _, m := range g.messages {
		fields := m.Fields()
		for i := 0; i < fields.Len(); i++ {
			switch fields.Get(i).Kind() {
			case protoreflect.FloatKind, protoreflect.DoubleKind:
				return true
			}
		}
	}
	return false
}

// markRequired computes which messages need checkRequired: those with a
// required field, plus every message that reaches one through a message field.
func (g *generator) markRequired() {
	var reaches func(m protoreflect.MessageDescriptor, seen map[protoreflect.FullName]bool) bool
	reaches = func(m protoreflect.MessageDescriptor, seen map[protoreflect.FullName]bool) bool {
		if seen[m.FullName()] {
			return false
		}
		seen[m.FullName()] = true
		fields := m.Fields()
		for i := 0; i < fields.Len(); i++ {
			f := fields.Get(i)
			if f.Cardinality() == protoreflect.Required {
				return true
			}
			if f.Kind() == protoreflect.MessageKind && !f.IsMap() && reaches(f.Message(), seen) {
				return true
			}
		}
		return false
	}
	for _, m := range g.messages {
		if reaches(m, map[protoreflect.FullName]bool{}) {
			g.hasRequired[m.FullName()] = true
		}
	}
}

// collides reports whether name is already a generated top-level identifier.
func (g *generator) collides(name string) bool {
	if name == "math" && g.needMath {
		return true
	}
	for _, m := range g.messages {
		if goTypeName(m) == name {
			return true
		}
	}
	for _, e := range g.enums {
		if goTypeName(e) == name {
			return true
		}
	}
	return false
}

func (g *generator) writeHeader(b *strings.Builder) {
	fmt.Fprintf(b, "// Code generated by protoc-gen-pbwire-go from %s. DO NOT EDIT.\n\n", g.fd.Path())
	fmt.Fprintf(b, "package %s\n\n", g.pkg)
	fmt.Fprintf(b, "import (\n")
	if g.needMath {
		fmt.Fprintf(b, "\t\"math\"\n\n")
	}
	fmt.Fprintf(b, "\t%s %s\n", g.runtime, strconv.Quote(g.opts.RuntimeImport))
	fmt.Fprintf(b, ")\n\n")
}

func (g *generator) writeEnums(b *strings.Builder) {
	for _, e := range g.enums {
		name := goTypeName(e)
		fmt.Fprintf(b, "// %s mirrors %s.\n", name, e.FullName())
		fmt.Fprintf(b, "type %s int32\n\n", name)
		vals := e.Values()
		if vals.Len() == 0 {
			continue
		}
		fmt.Fprintf(b, "// %s values.\n", name)
		fmt.Fprintf(b, "const (\n")
		for i := 0; i < vals.Len(); i++ {
			v := vals.Get(i)
			fmt.Fprintf(b, "\t%s %s = %d\n", enumValueName(name, v), name, v.Number())
		}
		fmt.Fprintf(b, ")\n\n")
	}
}

func (g *generator) writeMessage(b *strings.Builder, m protoreflect.MessageDescriptor) error {
	name := goTypeName(m)
	fields, err := g.fields(m)
	if err != nil {
		return err
	}
	fmt.Fprintf(b, "// %s mirrors %s.\n", name, m.FullName())
	fmt.Fprintf(b, "type %s struct {\n", name)
	for _, f := range fields {
		fmt.Fprintf(b, "\t%s %s\n", f.name, f.typ)
	}
	fmt.Fprintf(b, "}\n\n")

	g.writeSize(b, name, fields)
	g.writeMarshal(b, name, fields)
	g.writeUnmarshal(b, name, m, fields)
	if g.hasRequired[m.FullName()] {
		g.writeCheckRequired(b, name, fields)
	}
	return nil
}

// ---------------------------------------------------------------- fields

type field struct {
	d      protoreflect.FieldDescriptor
	name   string // Go struct field name
	typ    string // full Go type, pointer/slice included
	elem   string // Go type of one value, without pointer/slice
	kind   protoreflect.Kind
	wire   uint8
	tagLit string // tag bytes as Go literals, ready to append
	// packTagLit is the wire-type-2 tag of a packed run; numeric repetition
	// carries a varint in the tag of each unpacked element instead.
	packTagLit string
	tagLen     int
	list       bool
	ptr        bool // explicit presence: the field is a pointer
	// nilPresence marks a []byte field whose presence is "non-nil" (proto2
	// optional/required, proto3 optional). Implicit-presence bytes use len > 0.
	nilPresence bool
	required    bool
	packed      bool
}

func (g *generator) fields(m protoreflect.MessageDescriptor) ([]*field, error) {
	used := map[string]bool{
		"Size": true, "Marshal": true, "MarshalAppend": true, "Unmarshal": true,
	}
	descs := m.Fields()
	out := make([]*field, 0, descs.Len())
	for i := 0; i < descs.Len(); i++ {
		d := descs.Get(i)
		f, err := g.field(d)
		if err != nil {
			return nil, err
		}
		name := camel(string(d.Name()))
		for used[name] {
			name += "_"
		}
		used[name] = true
		f.name = name
		out = append(out, f)
	}
	return out, nil
}

func (g *generator) field(d protoreflect.FieldDescriptor) (*field, error) {
	elem, err := g.elemType(d)
	if err != nil {
		return nil, err
	}
	f := &field{
		d:        d,
		kind:     d.Kind(),
		elem:     elem,
		list:     d.IsList(),
		required: d.Cardinality() == protoreflect.Required,
		packed:   d.IsPacked(),
	}
	f.wire = wireType(d.Kind())
	f.typ = elem
	switch {
	case f.list:
		f.typ = "[]" + elem
	case d.Kind() == protoreflect.BytesKind:
		f.nilPresence = d.HasPresence()
	case d.HasPresence():
		f.ptr = true
		f.typ = "*" + elem
	}
	// The tag width depends only on the field number (8 divides the varint
	// boundaries), so a packed run can reuse tagLen with its own wire type 2.
	tag := uint64(d.Number())<<3 | uint64(f.wire)
	f.tagLit, f.tagLen = tagLiteral(tag)
	f.packTagLit, _ = tagLiteral(uint64(d.Number())<<3 | 2)
	return f, nil
}

func (g *generator) elemType(d protoreflect.FieldDescriptor) (string, error) {
	switch d.Kind() {
	case protoreflect.DoubleKind:
		return "float64", nil
	case protoreflect.FloatKind:
		return "float32", nil
	case protoreflect.Int64Kind, protoreflect.Sint64Kind, protoreflect.Sfixed64Kind:
		return "int64", nil
	case protoreflect.Uint64Kind, protoreflect.Fixed64Kind:
		return "uint64", nil
	case protoreflect.Int32Kind, protoreflect.Sint32Kind, protoreflect.Sfixed32Kind:
		return "int32", nil
	case protoreflect.Uint32Kind, protoreflect.Fixed32Kind:
		return "uint32", nil
	case protoreflect.BoolKind:
		return "bool", nil
	case protoreflect.StringKind:
		return "string", nil
	case protoreflect.BytesKind:
		return "[]byte", nil
	case protoreflect.EnumKind:
		return goTypeName(d.Enum()), nil
	case protoreflect.MessageKind:
		return goTypeName(d.Message()), nil
	}
	return "", fmt.Errorf("%s: unsupported kind %s", d.FullName(), d.Kind())
}

// ---------------------------------------------------------------- size

func (g *generator) writeSize(b *strings.Builder, name string, fields []*field) {
	fmt.Fprintf(b, "// Size reports the length of the wire encoding of m.\n")
	fmt.Fprintf(b, "func (m *%s) Size() int {\n\treturn m.size(0)\n}\n\n", name)
	fmt.Fprintf(b, "func (m *%s) size(depth int) int {\n", name)
	fmt.Fprintf(b, "\tif depth >= %s.MaxDepth {\n\t\treturn 0\n\t}\n", g.runtime)
	fmt.Fprintf(b, "\tn := 0\n")
	for _, f := range fields {
		g.writeSizeField(b, f)
	}
	fmt.Fprintf(b, "\treturn n\n}\n\n")
}

func (g *generator) writeSizeField(b *strings.Builder, f *field) {
	expr := "m." + f.name
	switch {
	case f.list && f.kind == protoreflect.MessageKind:
		fmt.Fprintf(b, "\tfor i := range %s {\n", expr)
		fmt.Fprintf(b, "\t\tsz := %s[i].size(depth + 1)\n", expr)
		fmt.Fprintf(b, "\t\tn += %d + %s.VarintSize(uint64(sz)) + sz\n\t}\n", f.tagLen, g.runtime)
	case f.list && (f.kind == protoreflect.StringKind || f.kind == protoreflect.BytesKind):
		fmt.Fprintf(b, "\tfor _, v := range %s {\n", expr)
		fmt.Fprintf(b, "\t\tn += %d + %s.VarintSize(uint64(len(v))) + len(v)\n\t}\n", f.tagLen, g.runtime)
	case f.list && f.packed:
		if width, fixed := fixedWidth(f.kind); fixed {
			// Fixed-width elements (and bools) keep a constant packed size.
			fmt.Fprintf(b, "\tif len(%s) > 0 {\n", expr)
			fmt.Fprintf(b, "\t\tsz := %d * len(%s)\n", width, expr)
			fmt.Fprintf(b, "\t\tn += %d + %s.VarintSize(uint64(sz)) + sz\n\t}\n", f.tagLen, g.runtime)
		} else {
			fmt.Fprintf(b, "\tif len(%s) > 0 {\n\t\tsz := 0\n", expr)
			fmt.Fprintf(b, "\t\tfor _, v := range %s {\n\t\t\tsz += %s\n\t\t}\n", expr, g.sizeValue("v", f))
			fmt.Fprintf(b, "\t\tn += %d + %s.VarintSize(uint64(sz)) + sz\n\t}\n", f.tagLen, g.runtime)
		}
	case f.list:
		fmt.Fprintf(b, "\tfor _, v := range %s {\n", expr)
		fmt.Fprintf(b, "\t\tn += %d + %s\n\t}\n", f.tagLen, g.sizeValue("v", f))
	case f.kind == protoreflect.MessageKind:
		fmt.Fprintf(b, "\tif %s != nil {\n", expr)
		fmt.Fprintf(b, "\t\tsz := %s.size(depth + 1)\n", expr)
		fmt.Fprintf(b, "\t\tn += %d + %s.VarintSize(uint64(sz)) + sz\n\t}\n", f.tagLen, g.runtime)
	case f.ptr:
		fmt.Fprintf(b, "\tif %s != nil {\n", expr)
		fmt.Fprintf(b, "\t\tn += %d + %s\n\t}\n", f.tagLen, g.sizeValue("*"+expr, f))
	case f.nilPresence:
		fmt.Fprintf(b, "\tif %s != nil {\n", expr)
		fmt.Fprintf(b, "\t\tn += %d + %s.VarintSize(uint64(len(%s))) + len(%s)\n\t}\n",
			f.tagLen, g.runtime, expr, expr)
	case f.kind == protoreflect.BytesKind:
		fmt.Fprintf(b, "\tif len(%s) > 0 {\n", expr)
		fmt.Fprintf(b, "\t\tn += %d + %s.VarintSize(uint64(len(%s))) + len(%s)\n\t}\n",
			f.tagLen, g.runtime, expr, expr)
	case f.kind == protoreflect.StringKind:
		fmt.Fprintf(b, "\tif %s != \"\" {\n", expr)
		fmt.Fprintf(b, "\t\tn += %d + %s.VarintSize(uint64(len(%s))) + len(%s)\n\t}\n",
			f.tagLen, g.runtime, expr, expr)
	default:
		fmt.Fprintf(b, "\tif %s {\n", g.nonZero(expr, f))
		fmt.Fprintf(b, "\t\tn += %d + %s\n\t}\n", f.tagLen, g.sizeValue(expr, f))
	}
}

// fixedWidth reports the constant encoded width of a fixed-width kind (bool
// fits because a bool is always one byte), if any.
func fixedWidth(k protoreflect.Kind) (int, bool) {
	switch k {
	case protoreflect.DoubleKind, protoreflect.Fixed64Kind, protoreflect.Sfixed64Kind:
		return 8, true
	case protoreflect.FloatKind, protoreflect.Fixed32Kind, protoreflect.Sfixed32Kind:
		return 4, true
	case protoreflect.BoolKind:
		return 1, true
	}
	return 0, false
}

// sizeValue is the encoded value width of one scalar expression.
func (g *generator) sizeValue(expr string, f *field) string {
	if width, fixed := fixedWidth(f.kind); fixed {
		return strconv.Itoa(width)
	}
	switch f.kind {
	case protoreflect.StringKind, protoreflect.BytesKind:
		return fmt.Sprintf("%s.VarintSize(uint64(len(%s))) + len(%s)", g.runtime, expr, expr)
	}
	return fmt.Sprintf("%s.VarintSize(%s)", g.runtime, g.varintValue(expr, f))
}

// varintValue is the encoded varint of one scalar expression.
func (g *generator) varintValue(expr string, f *field) string {
	switch f.kind {
	case protoreflect.Int32Kind, protoreflect.EnumKind:
		return "uint64(int64(" + expr + "))"
	case protoreflect.Uint32Kind, protoreflect.Int64Kind:
		return "uint64(" + expr + ")"
	case protoreflect.Sint32Kind:
		return "uint64(uint32(" + expr + ")<<1 ^ uint32(" + expr + ">>31))"
	case protoreflect.Sint64Kind:
		return "uint64(" + expr + ")<<1 ^ uint64(" + expr + ">>63)"
	}
	return expr
}

// nonZero is the condition under which an implicit-presence field is emitted.
func (g *generator) nonZero(expr string, f *field) string {
	switch f.kind {
	case protoreflect.BoolKind:
		return expr
	case protoreflect.FloatKind, protoreflect.DoubleKind:
		return "(" + expr + " != 0 || math.Signbit(float64(" + expr + ")))"
	}
	return expr + " != 0"
}

// ---------------------------------------------------------------- marshal

func (g *generator) writeMarshal(b *strings.Builder, name string, fields []*field) {
	nested := false
	for _, f := range fields {
		if f.kind == protoreflect.MessageKind {
			nested = true
		}
	}
	fmt.Fprintf(b, "// Marshal encodes m into a new buffer.\n")
	fmt.Fprintf(b, "func (m *%s) Marshal() ([]byte, error) {\n", name)
	fmt.Fprintf(b, "\tb := make([]byte, 0, m.Size())\n\treturn m.marshal(b, 0)\n}\n\n")
	fmt.Fprintf(b, "// MarshalAppend appends the wire encoding of m to b.\n")
	fmt.Fprintf(b, "func (m *%s) MarshalAppend(b []byte) ([]byte, error) {\n\treturn m.marshal(b, 0)\n}\n\n", name)
	fmt.Fprintf(b, "func (m *%s) marshal(b []byte, depth int) ([]byte, error) {\n", name)
	fmt.Fprintf(b, "\tif depth >= %s.MaxDepth {\n\t\treturn b, %s.ErrDepth\n\t}\n", g.runtime, g.runtime)
	if nested {
		fmt.Fprintf(b, "\tvar err error\n")
	}
	for _, f := range fields {
		g.writeMarshalField(b, f)
	}
	fmt.Fprintf(b, "\treturn b, nil\n}\n\n")
}

func (g *generator) writeMarshalField(b *strings.Builder, f *field) {
	expr := "m." + f.name
	switch {
	case f.list && f.kind == protoreflect.MessageKind:
		fmt.Fprintf(b, "\tfor i := range %s {\n", expr)
		fmt.Fprintf(b, "\t\tb = append(b, %s)\n", f.tagLit)
		fmt.Fprintf(b, "\t\tmark := len(b)\n\t\tb = append(b, 0)\n")
		fmt.Fprintf(b, "\t\tb, err = %s[i].marshal(b, depth+1)\n", expr)
		fmt.Fprintf(b, "\t\tif err != nil {\n\t\t\treturn b, err\n\t\t}\n")
		fmt.Fprintf(b, "\t\tb = %s.FinishSlice(b, mark)\n\t}\n", g.runtime)
	case f.list && (f.kind == protoreflect.StringKind || f.kind == protoreflect.BytesKind):
		call := "AppendString"
		if f.kind == protoreflect.BytesKind {
			call = "AppendBytes"
		}
		fmt.Fprintf(b, "\tfor _, v := range %s {\n", expr)
		fmt.Fprintf(b, "\t\tb = append(b, %s)\n", f.tagLit)
		fmt.Fprintf(b, "\t\tb = %s.%s(b, v)\n\t}\n", g.runtime, call)
	case f.list && f.packed:
		fmt.Fprintf(b, "\tif len(%s) > 0 {\n", expr)
		fmt.Fprintf(b, "\t\tb = append(b, %s)\n", f.packTagLit)
		fmt.Fprintf(b, "\t\tmark := len(b)\n\t\tb = append(b, 0)\n")
		fmt.Fprintf(b, "\t\tfor _, v := range %s {\n\t\t\tb = %s\n\t\t}\n", expr, g.appendValue("v", f))
		fmt.Fprintf(b, "\t\tb = %s.FinishSlice(b, mark)\n\t}\n", g.runtime)
	case f.list:
		fmt.Fprintf(b, "\tfor _, v := range %s {\n", expr)
		fmt.Fprintf(b, "\t\tb = append(b, %s)\n", f.tagLit)
		fmt.Fprintf(b, "\t\tb = %s\n\t}\n", g.appendValue("v", f))
	case f.kind == protoreflect.MessageKind:
		if f.required {
			fmt.Fprintf(b, "\tif %s == nil {\n\t\treturn b, %s.ErrRequired\n\t}\n", expr, g.runtime)
			g.writeNested(b, f, expr, "\t")
		} else {
			fmt.Fprintf(b, "\tif %s != nil {\n", expr)
			g.writeNested(b, f, expr, "\t\t")
			fmt.Fprintf(b, "\t}\n")
		}
	case f.required:
		fmt.Fprintf(b, "\tif %s == nil {\n\t\treturn b, %s.ErrRequired\n\t}\n", expr, g.runtime)
		g.writeScalar(b, f, ptrExpr(f, expr), "\t")
	case f.ptr:
		fmt.Fprintf(b, "\tif %s != nil {\n", expr)
		g.writeScalar(b, f, "*"+expr, "\t\t")
		fmt.Fprintf(b, "\t}\n")
	case f.nilPresence:
		fmt.Fprintf(b, "\tif %s != nil {\n", expr)
		g.writeScalar(b, f, expr, "\t\t")
		fmt.Fprintf(b, "\t}\n")
	case f.kind == protoreflect.StringKind:
		fmt.Fprintf(b, "\tif %s != \"\" {\n", expr)
		g.writeScalar(b, f, expr, "\t\t")
		fmt.Fprintf(b, "\t}\n")
	case f.kind == protoreflect.BytesKind:
		fmt.Fprintf(b, "\tif len(%s) > 0 {\n", expr)
		g.writeScalar(b, f, expr, "\t\t")
		fmt.Fprintf(b, "\t}\n")
	case f.kind == protoreflect.BoolKind:
		fmt.Fprintf(b, "\tif %s {\n\t\tb = append(b, %s, 0x01)\n\t}\n", expr, f.tagLit)
	default:
		fmt.Fprintf(b, "\tif %s {\n", g.nonZero(expr, f))
		g.writeScalar(b, f, expr, "\t\t")
		fmt.Fprintf(b, "\t}\n")
	}
}

// writeNested emits one length-delimited message field at the given indent.
func (g *generator) writeNested(b *strings.Builder, f *field, expr, indent string) {
	fmt.Fprintf(b, "%sb = append(b, %s)\n", indent, f.tagLit)
	fmt.Fprintf(b, "%smark := len(b)\n%sb = append(b, 0)\n", indent, indent)
	fmt.Fprintf(b, "%sb, err = %s.marshal(b, depth+1)\n", indent, expr)
	fmt.Fprintf(b, "%sif err != nil {\n%s\treturn b, err\n%s}\n", indent, indent, indent)
	fmt.Fprintf(b, "%sb = %s.FinishSlice(b, mark)\n", indent, g.runtime)
}

// writeScalar emits the tag plus the encoded scalar.
func (g *generator) writeScalar(b *strings.Builder, f *field, expr, indent string) {
	if f.kind == protoreflect.BoolKind {
		fmt.Fprintf(b, "%sif %s {\n%s\tb = append(b, %s, 0x01)\n%s} else {\n%s\tb = append(b, %s, 0x00)\n%s}\n",
			indent, expr, indent, f.tagLit, indent, indent, f.tagLit, indent)
		return
	}
	fmt.Fprintf(b, "%sb = append(b, %s)\n", indent, f.tagLit)
	fmt.Fprintf(b, "%sb = %s\n", indent, g.appendValue(expr, f))
}

func ptrExpr(f *field, expr string) string {
	if f.ptr {
		return "*" + expr
	}
	return expr
}

// appendValue is the append call for one scalar expression.
func (g *generator) appendValue(expr string, f *field) string {
	switch f.kind {
	case protoreflect.DoubleKind:
		return fmt.Sprintf("%s.AppendFixed64(b, math.Float64bits(%s))", g.runtime, expr)
	case protoreflect.FloatKind:
		return fmt.Sprintf("%s.AppendFixed32(b, math.Float32bits(%s))", g.runtime, expr)
	case protoreflect.Fixed64Kind:
		return fmt.Sprintf("%s.AppendFixed64(b, %s)", g.runtime, expr)
	case protoreflect.Sfixed64Kind:
		return fmt.Sprintf("%s.AppendFixed64(b, uint64(%s))", g.runtime, expr)
	case protoreflect.Fixed32Kind:
		return fmt.Sprintf("%s.AppendFixed32(b, %s)", g.runtime, expr)
	case protoreflect.Sfixed32Kind:
		return fmt.Sprintf("%s.AppendFixed32(b, uint32(%s))", g.runtime, expr)
	case protoreflect.BoolKind:
		return fmt.Sprintf("%s.AppendBool(b, %s)", g.runtime, expr)
	case protoreflect.StringKind:
		return fmt.Sprintf("%s.AppendString(b, %s)", g.runtime, expr)
	case protoreflect.BytesKind:
		return fmt.Sprintf("%s.AppendBytes(b, %s)", g.runtime, expr)
	}
	return fmt.Sprintf("%s.AppendVarint(b, %s)", g.runtime, g.varintValue(expr, f))
}

// ---------------------------------------------------------------- unmarshal

func (g *generator) writeUnmarshal(b *strings.Builder, name string, m protoreflect.MessageDescriptor, fields []*field) {
	fmt.Fprintf(b, "// Unmarshal replaces m with the message decoded from b.\n")
	fmt.Fprintf(b, "func (m *%s) Unmarshal(b []byte) error {\n", name)
	fmt.Fprintf(b, "\t*m = %s{}\n", name)
	fmt.Fprintf(b, "\tif err := m.unmarshal(b, 0); err != nil {\n\t\treturn err\n\t}\n")
	if g.hasRequired[m.FullName()] {
		fmt.Fprintf(b, "\treturn m.checkRequired()\n")
	} else {
		fmt.Fprintf(b, "\treturn nil\n")
	}
	fmt.Fprintf(b, "}\n\n")
	fmt.Fprintf(b, "func (m *%s) unmarshal(b []byte, depth int) error {\n", name)
	fmt.Fprintf(b, "\tif depth >= %s.MaxDepth {\n\t\treturn %s.ErrDepth\n\t}\n", g.runtime, g.runtime)
	fmt.Fprintf(b, "\tfor len(b) > 0 {\n")
	fmt.Fprintf(b, "\t\tnum, wt, n := %s.ConsumeTag(b)\n", g.runtime)
	fmt.Fprintf(b, "\t\tif n < 0 {\n\t\t\treturn %s.ErrWire\n\t\t}\n", g.runtime)
	fmt.Fprintf(b, "\t\tb = b[n:]\n")
	fmt.Fprintf(b, "\t\tswitch num {\n")
	for _, f := range fields {
		g.writeUnmarshalField(b, f)
	}
	fmt.Fprintf(b, "\t\tdefault:\n")
	g.writeSkip(b, "\t\t\t")
	fmt.Fprintf(b, "\t\t}\n\t}\n\treturn nil\n}\n\n")
}

func (g *generator) writeUnmarshalField(b *strings.Builder, f *field) {
	expr := "m." + f.name
	fmt.Fprintf(b, "\t\tcase %d:\n", f.d.Number())
	switch {
	case f.list && f.kind == protoreflect.MessageKind:
		g.writeSkipUnless(b, 2)
		g.writeConsumeBytes(b, "\t\t\t")
		fmt.Fprintf(b, "\t\t\t%s = append(%s, %s{})\n", expr, expr, f.elem)
		fmt.Fprintf(b, "\t\t\tif err := %s[len(%s)-1].unmarshal(p, depth+1); err != nil {\n", expr, expr)
		fmt.Fprintf(b, "\t\t\t\treturn err\n\t\t\t}\n")
	case f.list && (f.kind == protoreflect.StringKind || f.kind == protoreflect.BytesKind):
		g.writeSkipUnless(b, 2)
		g.writeConsumeBytes(b, "\t\t\t")
		if f.kind == protoreflect.StringKind {
			fmt.Fprintf(b, "\t\t\t%s = append(%s, string(p))\n", expr, expr)
		} else {
			fmt.Fprintf(b, "\t\t\t%s = append(%s, append([]byte{}, p...))\n", expr, expr)
		}
	case f.list:
		// Numeric repetition accepts the element wire type and a packed run.
		fmt.Fprintf(b, "\t\t\tswitch wt {\n")
		fmt.Fprintf(b, "\t\t\tcase %d:\n", f.wire)
		g.writeConsumeScalar(b, f, "\t\t\t\t", "b")
		fmt.Fprintf(b, "\t\t\t\t%s = append(%s, x)\n", expr, expr)
		fmt.Fprintf(b, "\t\t\tcase 2:\n")
		g.writeConsumeBytes(b, "\t\t\t\t")
		fmt.Fprintf(b, "\t\t\t\tfor len(p) > 0 {\n")
		g.writeConsumeScalar(b, f, "\t\t\t\t\t", "p")
		fmt.Fprintf(b, "\t\t\t\t\t%s = append(%s, x)\n", expr, expr)
		fmt.Fprintf(b, "\t\t\t\t}\n")
		fmt.Fprintf(b, "\t\t\tdefault:\n")
		g.writeSkip(b, "\t\t\t\t")
		fmt.Fprintf(b, "\t\t\t}\n")
	case f.kind == protoreflect.MessageKind:
		g.writeSkipUnless(b, 2)
		g.writeConsumeBytes(b, "\t\t\t")
		fmt.Fprintf(b, "\t\t\tif %s == nil {\n\t\t\t\t%s = new(%s)\n\t\t\t}\n", expr, expr, f.elem)
		fmt.Fprintf(b, "\t\t\tif err := %s.unmarshal(p, depth+1); err != nil {\n", expr)
		fmt.Fprintf(b, "\t\t\t\treturn err\n\t\t\t}\n")
	case f.kind == protoreflect.StringKind, f.kind == protoreflect.BytesKind:
		g.writeSkipUnless(b, 2)
		g.writeConsumeBytes(b, "\t\t\t")
		if f.kind == protoreflect.StringKind {
			if f.ptr {
				fmt.Fprintf(b, "\t\t\tx := string(p)\n\t\t\t%s = &x\n", expr)
			} else {
				fmt.Fprintf(b, "\t\t\t%s = string(p)\n", expr)
			}
		} else {
			fmt.Fprintf(b, "\t\t\t%s = append([]byte{}, p...)\n", expr)
		}
	default:
		g.writeSkipUnless(b, f.wire)
		if f.ptr {
			g.writeConsumeScalar(b, f, "\t\t\t", "b")
			fmt.Fprintf(b, "\t\t\t%s = &x\n", expr)
		} else {
			g.writeConsumeValue(b, f, "\t\t\t", "b")
			fmt.Fprintf(b, "\t\t\t%s = %s\n", expr, g.decodeValue("v", f))
		}
	}
}

// writeConsumeScalar decodes one scalar into x and advances src past it.
func (g *generator) writeConsumeScalar(b *strings.Builder, f *field, indent, src string) {
	g.writeConsumeValue(b, f, indent, src)
	fmt.Fprintf(b, "%sx := %s\n", indent, g.decodeValue("v", f))
}

// writeConsumeValue decodes one scalar into v and advances src past it.
func (g *generator) writeConsumeValue(b *strings.Builder, f *field, indent, src string) {
	var call string
	switch f.wire {
	case 1:
		call = fmt.Sprintf("v, n := %s.ConsumeFixed64(%s)", g.runtime, src)
	case 5:
		call = fmt.Sprintf("v, n := %s.ConsumeFixed32(%s)", g.runtime, src)
	default:
		call = fmt.Sprintf("v, n := %s.ConsumeVarint(%s)", g.runtime, src)
	}
	fmt.Fprintf(b, "%s%s\n", indent, call)
	fmt.Fprintf(b, "%sif n < 0 {\n%s\treturn %s.ErrWire\n%s}\n", indent, indent, g.runtime, indent)
	fmt.Fprintf(b, "%s%s = %s[n:]\n", indent, src, src)
}

func (g *generator) writeConsumeBytes(b *strings.Builder, indent string) {
	fmt.Fprintf(b, "%sp, n := %s.ConsumeBytes(b)\n", indent, g.runtime)
	fmt.Fprintf(b, "%sif n < 0 {\n%s\treturn %s.ErrWire\n%s}\n", indent, indent, g.runtime, indent)
	fmt.Fprintf(b, "%sb = b[n:]\n", indent)
}

// writeSkipUnless skips the value when the wire type is not want, so a known
// field carrying the wrong wire type is dropped instead of misread.
func (g *generator) writeSkipUnless(b *strings.Builder, want uint8) {
	fmt.Fprintf(b, "\t\t\tif wt != %d {\n", want)
	g.writeSkip(b, "\t\t\t\t")
	fmt.Fprintf(b, "\t\t\t\tbreak\n\t\t\t}\n")
}

func (g *generator) writeSkip(b *strings.Builder, indent string) {
	fmt.Fprintf(b, "%sn := %s.SkipField(b, num, wt, depth)\n", indent, g.runtime)
	fmt.Fprintf(b, "%sif n < 0 {\n%s\treturn %s.ErrWire\n%s}\n", indent, indent, g.runtime, indent)
	fmt.Fprintf(b, "%sb = b[n:]\n", indent)
}

// decodeValue turns the consumed scalar v into the field's Go value.
func (g *generator) decodeValue(v string, f *field) string {
	switch f.kind {
	case protoreflect.DoubleKind:
		return "math.Float64frombits(v)"
	case protoreflect.FloatKind:
		return "math.Float32frombits(v)"
	case protoreflect.Int32Kind:
		return "int32(" + v + ")"
	case protoreflect.Int64Kind:
		return "int64(" + v + ")"
	case protoreflect.Uint32Kind, protoreflect.Fixed32Kind:
		return "uint32(" + v + ")"
	case protoreflect.Uint64Kind, protoreflect.Fixed64Kind:
		return v
	case protoreflect.Sint32Kind:
		return "int32(uint32(" + v + ")>>1) ^ -int32(" + v + "&1)"
	case protoreflect.Sint64Kind:
		return "int64(" + v + ">>1) ^ -int64(" + v + "&1)"
	case protoreflect.Sfixed32Kind:
		return "int32(" + v + ")"
	case protoreflect.Sfixed64Kind:
		return "int64(" + v + ")"
	case protoreflect.BoolKind:
		return v + " != 0"
	case protoreflect.EnumKind:
		return f.elem + "(" + v + ")"
	}
	return v
}

// ---------------------------------------------------------------- required

func (g *generator) writeCheckRequired(b *strings.Builder, name string, fields []*field) {
	fmt.Fprintf(b, "func (m *%s) checkRequired() error {\n", name)
	for _, f := range fields {
		expr := "m." + f.name
		switch {
		case f.required:
			fmt.Fprintf(b, "\tif %s == nil {\n\t\treturn %s.ErrRequired\n\t}\n", expr, g.runtime)
		case f.list && f.kind == protoreflect.MessageKind:
			if g.hasRequired[f.d.Message().FullName()] {
				fmt.Fprintf(b, "\tfor i := range %s {\n", expr)
				fmt.Fprintf(b, "\t\tif err := %s[i].checkRequired(); err != nil {\n\t\t\treturn err\n\t\t}\n\t}\n", expr)
			}
		case f.kind == protoreflect.MessageKind:
			if g.hasRequired[f.d.Message().FullName()] {
				fmt.Fprintf(b, "\tif %s != nil {\n", expr)
				fmt.Fprintf(b, "\t\tif err := %s.checkRequired(); err != nil {\n\t\t\treturn err\n\t\t}\n\t}\n", expr)
			}
		}
	}
	fmt.Fprintf(b, "\treturn nil\n}\n\n")
}

// ---------------------------------------------------------------- naming

func goTypeName(d protoreflect.Descriptor) string {
	name := string(d.Name())
	if parent, ok := d.Parent().(protoreflect.MessageDescriptor); ok {
		return goTypeName(parent) + "_" + name
	}
	return name
}

func enumValueName(enumType string, v protoreflect.EnumValueDescriptor) string {
	name := string(v.Name())
	prefix := upperSnake(string(v.Parent().Name())) + "_"
	if strings.HasPrefix(name, prefix) {
		name = name[len(prefix):]
	}
	return enumType + "_" + name
}

func upperSnake(s string) string {
	var b strings.Builder
	for _, r := range s {
		switch {
		case r >= 'a' && r <= 'z':
			b.WriteRune(r - 'a' + 'A')
		case (r >= 'A' && r <= 'Z') || (r >= '0' && r <= '9'):
			b.WriteRune(r)
		default:
			b.WriteByte('_')
		}
	}
	return b.String()
}

// camel turns a proto field name into an exported Go identifier.
func camel(s string) string {
	var b strings.Builder
	upper := true
	for i := 0; i < len(s); i++ {
		c := s[i]
		if c == '_' {
			upper = true
			continue
		}
		if upper && c >= 'a' && c <= 'z' {
			c -= 'a' - 'A'
		}
		upper = false
		b.WriteByte(c)
	}
	return b.String()
}

// packageName picks the generated package name: the last component of the proto
// package, or the file name when the file has no package.
func packageName(fd protoreflect.FileDescriptor) string {
	name := string(fd.Package())
	if i := strings.LastIndex(name, "."); i >= 0 {
		name = name[i+1:]
	}
	if name == "" {
		path := fd.Path()
		if i := strings.LastIndex(path, "/"); i >= 0 {
			path = path[i+1:]
		}
		name = strings.TrimSuffix(path, ".proto")
	}
	var b strings.Builder
	for _, r := range strings.ToLower(name) {
		if (r >= 'a' && r <= 'z') || (r >= '0' && r <= '9') || r == '_' {
			b.WriteRune(r)
		}
	}
	out := b.String()
	if out == "" || (out[0] >= '0' && out[0] <= '9') || goKeywords[out] {
		out = "pb" + out
	}
	return out
}

var goKeywords = map[string]bool{
	"break": true, "case": true, "chan": true, "const": true, "continue": true,
	"default": true, "defer": true, "else": true, "fallthrough": true, "for": true,
	"func": true, "go": true, "goto": true, "if": true, "import": true,
	"interface": true, "map": true, "package": true, "range": true, "return": true,
	"select": true, "struct": true, "switch": true, "type": true, "var": true,
}

func wireType(k protoreflect.Kind) uint8 {
	switch k {
	case protoreflect.DoubleKind, protoreflect.Fixed64Kind, protoreflect.Sfixed64Kind:
		return 1
	case protoreflect.FloatKind, protoreflect.Fixed32Kind, protoreflect.Sfixed32Kind:
		return 5
	case protoreflect.StringKind, protoreflect.BytesKind, protoreflect.MessageKind:
		return 2
	}
	return 0
}

// tagLiteral renders the encoded tag as Go byte literals plus its width.
func tagLiteral(tag uint64) (string, int) {
	var parts []string
	for {
		c := byte(tag & 0x7f)
		if tag >= 0x80 {
			c |= 0x80
		}
		parts = append(parts, fmt.Sprintf("0x%02x", c))
		tag >>= 7
		if tag == 0 {
			break
		}
	}
	return strings.Join(parts, ", "), len(parts)
}

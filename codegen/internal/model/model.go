// Package model turns a file descriptor into the neutral message model every
// language emitter consumes: names, kinds, repetition and presence, with maps
// and foreign message types left out (a generated model cannot express them, and
// the dynamic API still can).
package model

import (
	"strings"
	"unicode"

	"google.golang.org/protobuf/reflect/protoreflect"
)

// Field is one message field the generated model can hold.
type Field struct {
	Name     string // proto field name, the runtime lookup key
	Lower    string // lowerCamel
	Pascal   string // UpperCamel
	Number   int32
	Kind     protoreflect.Kind
	Repeated bool
	NeedsHas bool   // presence without a pointer: emit has<Field> next to it
	Required bool   // proto2 required (NeedsHas is true as well)
	Enum     string // local const class name, empty for an external enum
	Closed   bool   // the enum is closed (proto2): unknown values are invalid
	Message  string // local class name, empty for a foreign message
	Oneof    string // containing oneof, empty when not in one
	// Elixir-shape facts (other emitters ignore them).
	Proto3Optional bool   // proto3 `optional`: explicit presence without a oneof
	Packed         bool   // packed repeated numerics (proto3 default)
	Skip           string // non-empty: the generated model omits this field, reason
}

// Message is one message with its fields in declaration order.
type Message struct {
	Name   string // local: nested path joined with "_", e.g. Outer_Inner
	Full   string // fully qualified proto name
	Fields []Field
}

// EnumValue and Enum carry the enum as plain integer constants: no emitter
// narrows a field to a closed enum type, so an unknown wire value survives a
// round trip.
type EnumValue struct {
	Name   string
	Number int32
}

type Enum struct {
	Name   string // local const class name (no package prefix)
	Full   string
	Values []EnumValue
}

// File is one .proto file prepared for an emitter.
type File struct {
	Path     string
	Package  string
	Prefix   string // CamelCase of the proto package, used for class prefixes
	Syntax   string // "proto2" or "proto3" (editions report as proto2)
	Messages []Message
	Enums    []Enum
}

// Build walks a file descriptor in post-order (nested types first) and returns
// the generated model.
func Build(fd protoreflect.FileDescriptor) *File {
	f := &File{
		Path:    fd.Path(),
		Package: string(fd.Package()),
		Prefix:  Prefix(string(fd.Package())),
	}
	switch fd.Syntax() {
	case protoreflect.Proto3:
		f.Syntax = "proto3"
	default:
		f.Syntax = "proto2"
	}
	for i := 0; i < fd.Enums().Len(); i++ {
		f.Enums = append(f.Enums, buildEnum(fd.Enums().Get(i), nil))
	}
	for i := 0; i < fd.Messages().Len(); i++ {
		f.buildMessage(fd.Messages().Get(i), nil)
	}
	return f
}

// Prefix is the CamelCase form of a proto package: "protobuf_test_messages.proto3"
// becomes "ProtobufTestMessagesProto3". Used to keep generated top-level names
// unique in languages with a single global namespace (Java, C#, Ruby, PHP, ObjC).
func Prefix(pkg string) string {
	if pkg == "" {
		return "Pbw"
	}
	var b strings.Builder
	for _, part := range strings.Split(pkg, ".") {
		b.WriteString(Pascal(part))
	}
	return b.String()
}

// Pascal converts a proto identifier to UpperCamelCase: "opt_i32" -> "OptI32".
func Pascal(s string) string {
	var b strings.Builder
	upper := true
	for _, r := range s {
		if r == '_' {
			upper = true
			continue
		}
		if upper {
			b.WriteRune(unicode.ToUpper(r))
			upper = false
			continue
		}
		b.WriteRune(r)
	}
	return b.String()
}

// LowerCamel converts a proto identifier to lowerCamelCase: "opt_i32" -> "optI32".
func LowerCamel(s string) string {
	p := Pascal(s)
	if p == "" {
		return p
	}
	return strings.ToLower(p[:1]) + p[1:]
}

func (f *File) buildMessage(md protoreflect.MessageDescriptor, path []string) {
	if md.IsMapEntry() {
		return
	}
	path = append(path, string(md.Name()))
	local := strings.Join(path, "_")
	msg := Message{Name: local, Full: string(md.FullName())}
	fields := md.Fields()
	for i := 0; i < fields.Len(); i++ {
		msg.Fields = append(msg.Fields, f.buildField(fields.Get(i)))
	}
	// Nested messages and enums first: the emitter writes them before this one.
	for i := 0; i < md.Enums().Len(); i++ {
		f.Enums = append(f.Enums, buildEnum(md.Enums().Get(i), path))
	}
	for i := 0; i < md.Messages().Len(); i++ {
		f.buildMessage(md.Messages().Get(i), append([]string(nil), path...))
	}
	f.Messages = append(f.Messages, msg)
}

func (f *File) buildField(fd protoreflect.FieldDescriptor) Field {
	field := Field{
		Name:     string(fd.Name()),
		Lower:    LowerCamel(string(fd.Name())),
		Pascal:   Pascal(string(fd.Name())),
		Number:   int32(fd.Number()),
		Kind:     fd.Kind(),
		Repeated: fd.IsList(),
		Required: fd.Cardinality() == protoreflect.Required,
	}
	if fd.HasOptionalKeyword() {
		field.Proto3Optional = true
	}
	if fd.IsPacked() {
		field.Packed = true
	}
	if od := fd.ContainingOneof(); od != nil && !od.IsSynthetic() {
		field.Oneof = string(od.Name())
	}
	if fd.IsMap() {
		field.Skip = "map field: use the dynamic API"
		return field
	}
	switch fd.Kind() {
	case protoreflect.MessageKind, protoreflect.GroupKind:
		if local := f.messageLocal(fd); local != "" {
			field.Message = local
		} else {
			field.Skip = "message type from another file: use the dynamic API"
		}
		field.NeedsHas = false // a nil pointer is the absent state
	case protoreflect.EnumKind:
		field.Enum = f.enumLocal(fd)
		if ed := fd.Enum(); ed != nil {
			field.Closed = ed.IsClosed()
		}
		field.NeedsHas = fd.HasPresence() && !fd.IsList() && !field.Required
	default:
		field.NeedsHas = fd.HasPresence() && !fd.IsList() && !field.Required
	}
	return field
}

// messageLocal returns the generated class name of a message type defined in
// this file, or "" when it lives in an import (or the request left the type
// unresolved, which AllowUnresolvable permits).
func (f *File) messageLocal(fd protoreflect.FieldDescriptor) string {
	md := fd.Message()
	if md == nil || md.ParentFile() == nil || md.ParentFile().Path() != f.Path {
		return ""
	}
	var path []string
	for d := protoreflect.Descriptor(md); d != nil; d = d.Parent() {
		if m, ok := d.(protoreflect.MessageDescriptor); ok {
			path = append([]string{string(m.Name())}, path...)
			continue
		}
		break
	}
	return strings.Join(path, "_")
}

// enumLocal returns the generated name of an enum defined in this file, or
// "" when it lives in an import. It walks the descriptor directly (like
// messageLocal) instead of consulting f.Enums, whose nested entries are only
// appended after the enclosing message's fields are built.
func (f *File) enumLocal(fd protoreflect.FieldDescriptor) string {
	ed := fd.Enum()
	if ed == nil || ed.ParentFile() == nil || ed.ParentFile().Path() != f.Path {
		return ""
	}
	var path []string
	for d := protoreflect.Descriptor(ed); d != nil; d = d.Parent() {
		switch t := d.(type) {
		case protoreflect.EnumDescriptor:
			path = append([]string{string(t.Name())}, path...)
		case protoreflect.MessageDescriptor:
			path = append([]string{string(t.Name())}, path...)
		default:
			return strings.Join(path, "_")
		}
	}
	return strings.Join(path, "_")
}

func buildEnum(ed protoreflect.EnumDescriptor, path []string) Enum {
	path = append(path, string(ed.Name()))
	e := Enum{Name: strings.Join(path, "_"), Full: string(ed.FullName())}
	for i := 0; i < ed.Values().Len(); i++ {
		v := ed.Values().Get(i)
		e.Values = append(e.Values, EnumValue{Name: string(v.Name()), Number: int32(v.Number())})
	}
	return e
}

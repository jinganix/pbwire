// Package cgen generates C structs, layout tables and encode/decode
// wrappers for the pbwire C binding's generic struct engine.
package cgen

import (
	"fmt"
	"strings"

	"google.golang.org/protobuf/reflect/protoreflect"
	"google.golang.org/protobuf/types/descriptorpb"
)

// Generate returns the output file name and content for one proto file.
// The output name is "<base>.pbwire.h" where base is the proto file name
// without directory or extension.
func Generate(fd protoreflect.FileDescriptor) (string, string, error) {
	g := &generator{fd: fd}
	if err := g.collect(); err != nil {
		return "", "", err
	}
	base := strings.TrimSuffix(fd.Path()[strings.LastIndex(fd.Path(), "/")+1:], ".proto")
	name := base + ".h"
	g.output = name
	var b strings.Builder
	g.writeHeader(&b, fd.Path())
	g.writeEnums(&b)
	if err := g.writeMessages(&b); err != nil {
		return "", "", err
	}
	if err := g.writeTables(&b); err != nil {
		return "", "", err
	}
	g.writeWrappers(&b)
	g.writeSchema(&b)
	fmt.Fprintf(&b, "#endif /* %s */\n", g.guard())
	return name, b.String(), nil
}

type generator struct {
	fd       protoreflect.FileDescriptor
	output   string                           // output file name, guards derive from it
	messages []protoreflect.MessageDescriptor // post-order: nested first
	enums    []protoreflect.EnumDescriptor
	exts     map[string][]protoreflect.FieldDescriptor // extendee full name -> extensions, by number
}

func (g *generator) collect() error {
	var walk func(msgs protoreflect.MessageDescriptors) error
	walk = func(msgs protoreflect.MessageDescriptors) error {
		for i := 0; i < msgs.Len(); i++ {
			m := msgs.Get(i)
			if err := checkMessage(m); err != nil {
				return err
			}
			if err := walk(m.Messages()); err != nil {
				return err
			}
			g.messages = append(g.messages, m)
			for j := 0; j < m.Enums().Len(); j++ {
				g.enums = append(g.enums, m.Enums().Get(j))
			}
		}
		return nil
	}
	for i := 0; i < g.fd.Enums().Len(); i++ {
		g.enums = append(g.enums, g.fd.Enums().Get(i))
	}
	if err := walk(g.fd.Messages()); err != nil {
		return err
	}
	return g.collectExtensions()
}

// collectExtensions groups the file's extensions by extendee. Extension rows
// hang off the extended message's struct and layout table, in field-number
// order, so the runtime resolves them by number like any field (the schema
// already merges them into the extendee's field list).
// shortcut: extendee in another file is rejected, regenerating that file's
// header cannot gain members for this file's extensions; lift by emitting
// an extern table into the extending file when cross-file extend blocks
// are needed.
func (g *generator) collectExtensions() error {
	exts := g.fd.Extensions()
	if exts.Len() == 0 {
		return nil
	}
	local := map[string]protoreflect.MessageDescriptor{}
	var mark func(msgs protoreflect.MessageDescriptors)
	mark = func(msgs protoreflect.MessageDescriptors) {
		for i := 0; i < msgs.Len(); i++ {
			m := msgs.Get(i)
			local[string(m.FullName())] = m
			mark(m.Messages())
		}
	}
	mark(g.fd.Messages())
	grouped := map[string][]protoreflect.FieldDescriptor{}
	for i := 0; i < exts.Len(); i++ {
		ext := exts.Get(i)
		ee := ext.ContainingMessage()
		if ee.ParentFile() == nil || ee.ParentFile().Path() != g.fd.Path() {
			return fmt.Errorf("%s: extension of foreign message %s: regenerate that file instead", ext.FullName(), ee.FullName())
		}
		if _, ok := local[string(ee.FullName())]; !ok {
			return fmt.Errorf("%s: unknown extendee %s", ext.FullName(), ee.FullName())
		}
		key := string(ee.FullName())
		grouped[key] = append(grouped[key], ext)
	}
	for key, list := range grouped {
		m := local[key]
		taken := map[string]bool{}
		fields := m.Fields()
		for i := 0; i < fields.Len(); i++ {
			taken[snake(string(fields.Get(i).Name()))] = true
		}
		for i := 1; i < len(list); i++ {
			for j := i; j > 0 && list[j].Number() < list[j-1].Number(); j-- {
				list[j], list[j-1] = list[j-1], list[j]
			}
		}
		for _, ext := range list {
			if name := snake(string(ext.Name())); taken[name] {
				return fmt.Errorf("%s: extension member %q collides with a field of %s", ext.FullName(), name, m.FullName())
			}
			taken[snake(string(ext.Name()))] = true
		}
		grouped[key] = list
	}
	g.exts = grouped
	return nil
}

// extFields returns the extensions of m in field-number order, or nil.
func (g *generator) extFields(m protoreflect.MessageDescriptor) []protoreflect.FieldDescriptor {
	return g.exts[string(m.FullName())]
}

// tableLen is the layout row count: regular fields plus extensions. It must
// match the schema's field count (the descriptor merges extensions into the
// extendee), or the codec rejects the table.
func (g *generator) tableLen(m protoreflect.MessageDescriptor) int {
	return m.Fields().Len() + len(g.extFields(m))
}

// realOneof returns the oneof a field belongs to, or nil for plain fields
// and proto3-optional synthetics (those keep their has-flag spelling).
func realOneof(f protoreflect.FieldDescriptor) protoreflect.OneofDescriptor {
	if od := f.ContainingOneof(); od != nil && !od.IsSynthetic() {
		return od
	}
	return nil
}

// checkMessage rejects what the struct engine cannot represent.
func checkMessage(m protoreflect.MessageDescriptor) error {
	// A MessageSet extendee frames its extensions as 0b/0c items, which this
	// layout cannot express; rejecting beats emitting the wrong bytes.
	if mo, ok := m.Options().(*descriptorpb.MessageOptions); ok &&
		mo.GetMessageSetWireFormat() {
		return fmt.Errorf("%s: message_set_wire_format is not supported", m.FullName())
	}
	fields := m.Fields()
	for i := 0; i < fields.Len(); i++ {
		f := fields.Get(i)
		if f.IsMap() {
			if k := f.MapKey().Kind(); k != protoreflect.Int32Kind && k != protoreflect.Int64Kind &&
				k != protoreflect.Uint32Kind && k != protoreflect.Uint64Kind &&
				k != protoreflect.BoolKind && k != protoreflect.StringKind {
				return fmt.Errorf("%s: unsupported map key %s", f.FullName(), k)
			}
			if v := f.MapValue(); v.IsMap() || v.Kind() == protoreflect.GroupKind {
				return fmt.Errorf("%s: unsupported map value %s", f.FullName(), v.Kind())
			}
			continue
		}
		// Oneof members ride a union + case word; repeated members are
		// rejected by protoc itself, map members likewise.
		switch f.Kind() {
		case protoreflect.GroupKind:
			continue // groups encode as START_GROUP + message + END_GROUP
		}
	}
	return nil
}

// snake converts CamelCase to snake_case.
func snake(s string) string {
	var b strings.Builder
	runes := []rune(s)
	for i, r := range runes {
		upper := r >= 'A' && r <= 'Z'
		if upper && i > 0 {
			prev := runes[i-1]
			lowerPrev := (prev >= 'a' && prev <= 'z') || (prev >= '0' && prev <= '9')
			var nextLower bool
			if i+1 < len(runes) {
				n := runes[i+1]
				nextLower = n >= 'a' && n <= 'z'
			}
			prevUpper := prev >= 'A' && prev <= 'Z'
			if lowerPrev || (prevUpper && nextLower) {
				b.WriteByte('_')
			}
		}
		if upper {
			b.WriteByte(byte(r - 'A' + 'a'))
		} else if (r >= 'a' && r <= 'z') || (r >= '0' && r <= '9') {
			b.WriteByte(byte(r))
		} else {
			b.WriteByte('_')
		}
	}
	out := b.String()
	if cKeywords[out] {
		/* `switch`, `union`, `int` ... are legal proto identifiers but not C
		 * member names. snake() is the single identifier source, so the
		 * suffix applies to the struct member, the offsetof and the has_/n_
		 * peers alike. shortcut: a schema that also declares `switch_` would
		 * collide, protoc never pairs those two names in practice. */
		out += "_"
	}
	return out
}

var cKeywords = map[string]bool{
	"auto": true, "break": true, "case": true, "char": true, "const": true,
	"continue": true, "default": true, "do": true, "double": true,
	"else": true, "enum": true, "extern": true, "float": true, "for": true,
	"goto": true, "if": true, "inline": true, "int": true, "long": true,
	"register": true, "restrict": true, "return": true, "short": true,
	"signed": true, "sizeof": true, "static": true, "struct": true,
	"switch": true, "typedef": true, "union": true, "unsigned": true,
	"void": true, "volatile": true, "while": true, "_Bool": true,
	"_Complex": true, "_Imaginary": true,
	/* C++ keywords, for headers included from a .cc (bindings/cpp). */
	"bool": true, "catch": true, "class": true, "delete": true,
	"explicit": true, "false": true, "friend": true, "mutable": true,
	"namespace": true, "new": true, "operator": true, "private": true,
	"protected": true, "public": true, "template": true, "this": true,
	"throw": true, "true": true, "try": true, "typename": true,
	"virtual": true, "nullptr": true,
}

// cname maps a descriptor to its C base name: full proto name,
// dots to underscores, each part snake_cased.
func cname(d protoreflect.Descriptor) string {
	parts := strings.Split(string(d.FullName()), ".")
	for i, p := range parts {
		parts[i] = snake(p)
	}
	return strings.Join(parts, "_")
}

func upper(s string) string {
	var b strings.Builder
	for _, r := range s {
		switch {
		case r >= 'a' && r <= 'z':
			b.WriteByte(byte(r - 'a' + 'A'))
		case (r >= 'A' && r <= 'Z') || (r >= '0' && r <= '9'):
			b.WriteByte(byte(r))
		default:
			b.WriteByte('_')
		}
	}
	return b.String()
}

func (g *generator) guard() string {
	name := g.output
	if i := strings.LastIndex(name, "."); i >= 0 {
		name = name[:i]
	}
	return upper(name) + "_H"
}

func (g *generator) writeHeader(b *strings.Builder, path string) {
	fmt.Fprintf(b, "/* Generated by protoc-gen-pbwire-c from %s.\n", path)
	fmt.Fprintf(b, " * Do not edit; regenerate with `make -C bindings/c/plugin regen`.\n")
	fmt.Fprintf(b, " * Pinned here so `make test` stays protoc-free.\n")
	fmt.Fprintf(b, " */\n")
	fmt.Fprintf(b, "#ifndef %s\n", g.guard())
	fmt.Fprintf(b, "#define %s\n", g.guard())
	fmt.Fprintf(b, "\n")
	fmt.Fprintf(b, "#include <stdbool.h>\n")
	fmt.Fprintf(b, "#include <stddef.h>\n")
	fmt.Fprintf(b, "#include <stdint.h>\n")
	fmt.Fprintf(b, "#include <string.h>\n")
	fmt.Fprintf(b, "\n")
	fmt.Fprintf(b, "#include \"pbw_struct.h\"\n")
	fmt.Fprintf(b, "#include \"pbwire.h\"\n")
	fmt.Fprintf(b, "\n")
}

func (g *generator) writeEnums(b *strings.Builder) {
	for _, e := range g.enums {
		fmt.Fprintf(b, "/* %s */\n", e.FullName())
		fmt.Fprintf(b, "typedef enum {\n")
		vals := e.Values()
		for i := 0; i < vals.Len(); i++ {
			v := vals.Get(i)
			fmt.Fprintf(b, "    %s_%s = %d%s\n", upper(cname(e)), v.Name(), v.Number(), comma(i, vals.Len()))
		}
		fmt.Fprintf(b, "} %s_t;\n", cname(e))
		fmt.Fprintf(b, "\n")
	}
}

func comma(i, n int) string {
	if i+1 < n {
		return ","
	}
	return ""
}

// fieldCType maps a field to its C member type and engine kind.
// Maps are repeated entry structs: the wire shape is N length-delimited
// entry messages, so the engine needs no hash table (linear entries match
// the core byte for byte; insertion order is the wire order).
func fieldCType(f protoreflect.FieldDescriptor) (ctype, kind string, err error) {
	if f.IsMap() {
		elem := cname(f.Message()) + "_t"
		if f.MapValue().Kind() == protoreflect.MessageKind {
			return elem + " *", "PBW_SF_MESSAGE", nil
		}
		return elem + " *", "PBW_SF_MESSAGE", nil
	}
	if f.Kind() == protoreflect.GroupKind {
		return cname(f.Message()) + "_t *", "PBW_SF_MESSAGE", nil
	}
	if f.IsList() {
		elem, k, err := elemCType(f)
		if err != nil {
			return "", "", err
		}
		if strings.HasSuffix(elem, "*") {
			return elem + "*", k, nil
		}
		return elem + " *", k, nil
	}
	if f.Kind() == protoreflect.MessageKind {
		return cname(f.Message()) + "_t *", "PBW_SF_MESSAGE", nil
	}
	return elemCType(f)
}

func elemCType(f protoreflect.FieldDescriptor) (string, string, error) {
	switch f.Kind() {
	case protoreflect.Int32Kind, protoreflect.Sint32Kind, protoreflect.Sfixed32Kind:
		return "int32_t", "PBW_SF_INT32", nil
	case protoreflect.Uint32Kind, protoreflect.Fixed32Kind:
		return "uint32_t", "PBW_SF_UINT32", nil
	case protoreflect.Int64Kind, protoreflect.Sint64Kind, protoreflect.Sfixed64Kind:
		return "int64_t", "PBW_SF_INT64", nil
	case protoreflect.Uint64Kind, protoreflect.Fixed64Kind:
		return "uint64_t", "PBW_SF_UINT64", nil
	case protoreflect.BoolKind:
		return "bool", "PBW_SF_BOOL", nil
	case protoreflect.FloatKind:
		return "float", "PBW_SF_FLOAT", nil
	case protoreflect.DoubleKind:
		return "double", "PBW_SF_DOUBLE", nil
	case protoreflect.StringKind:
		return "const char *", "PBW_SF_STRING", nil
	case protoreflect.BytesKind:
		return "pbw_bytes_t", "PBW_SF_BYTES", nil
	case protoreflect.EnumKind:
		return "int32_t", "PBW_SF_ENUM", nil
	case protoreflect.MessageKind:
		return cname(f.Message()) + "_t", "PBW_SF_MESSAGE", nil
	case protoreflect.GroupKind:
		return cname(f.Message()) + "_t", "PBW_SF_MESSAGE", nil
	}
	return "", "", fmt.Errorf("%s: unsupported kind %s", f.FullName(), f.Kind())
}

// needsHas reports whether a singular scalar/enum field gets a has_ flag.
func (g *generator) needsHas(f protoreflect.FieldDescriptor) bool {
	if realOneof(f) != nil {
		return false /* the case word is the presence */
	}
	if f.IsList() || f.IsMap() || f.Kind() == protoreflect.MessageKind || f.Kind() == protoreflect.GroupKind || f.Kind() == protoreflect.StringKind || f.Kind() == protoreflect.BytesKind {
		return false
	}
	if f.Cardinality() == protoreflect.Required {
		return false
	}
	if f.Kind() != protoreflect.EnumKind {
		switch f.Kind() {
		case protoreflect.Int32Kind, protoreflect.Sint32Kind, protoreflect.Sfixed32Kind,
			protoreflect.Uint32Kind, protoreflect.Fixed32Kind,
			protoreflect.Int64Kind, protoreflect.Sint64Kind, protoreflect.Sfixed64Kind,
			protoreflect.Uint64Kind, protoreflect.Fixed64Kind,
			protoreflect.BoolKind, protoreflect.FloatKind, protoreflect.DoubleKind:
		default:
			return false
		}
	}
	if f.HasOptionalKeyword() {
		return true
	}
	return g.fd.Syntax() != protoreflect.Proto3
}

func (g *generator) writeMessages(b *strings.Builder) error {
	for _, m := range g.messages {
		/* Map entries are plain structs: key/value rows like any message. */
		fmt.Fprintf(b, "/* %s */\n", m.FullName())
		/* Oneof unions come before the struct: rows address members by
		 * offsetof(union) + offsetof(member), so the type needs a name. */
		for i := 0; i < m.Oneofs().Len(); i++ {
			od := m.Oneofs().Get(i)
			if od.IsSynthetic() {
				continue
			}
			oname := snake(string(od.Name()))
			uname := cname(m) + "_" + oname + "_u"
			fmt.Fprintf(b, "/* oneof %s */\n", od.FullName())
			fmt.Fprintf(b, "typedef union {\n")
			for j := 0; j < od.Fields().Len(); j++ {
				f := od.Fields().Get(j)
				ctype, _, err := fieldCType(f)
				if err != nil {
					return err
				}
				member := snake(string(f.Name()))
				if strings.HasSuffix(ctype, "*") {
					member = ctype + member
				} else {
					member = ctype + " " + member
				}
				fmt.Fprintf(b, "    %s;\n", member)
			}
			fmt.Fprintf(b, "} %s;\n", uname)
		}
		fmt.Fprintf(b, "typedef struct {\n")
		fields := m.Fields()
		if fields.Len() == 0 && len(g.extFields(m)) == 0 {
			/* ISO C forbids an empty struct too: one placeholder member. */
			fmt.Fprintf(b, "    uint8_t _pbwire_placeholder;\n")
		}
		emitted := map[protoreflect.OneofDescriptor]bool{}
		for i := 0; i < fields.Len(); i++ {
			f := fields.Get(i)
			if od := realOneof(f); od != nil {
				if emitted[od] {
					continue
				}
				emitted[od] = true
				oname := snake(string(od.Name()))
				uname := cname(m) + "_" + oname + "_u"
				fmt.Fprintf(b, "    uint32_t %s_case; /* oneof %s: selected field number, 0 = none */\n", oname, od.Name())
				fmt.Fprintf(b, "    %s %s;\n", uname, oname)
				continue
			}
			ctype, _, err := fieldCType(f)
			if err != nil {
				return err
			}
			comment := "optional"
			if f.IsList() || f.IsMap() {
				comment = "repeated"
			} else if f.Cardinality() == protoreflect.Required {
				comment = "required"
			}
			/* Name the declared type: the member is an int for an enum and a
			 * pointer for a message, so the comment is where the .proto type shows. */
			switch f.Kind() {
			case protoreflect.EnumKind:
				comment += ", " + string(f.Enum().FullName())
			case protoreflect.MessageKind, protoreflect.GroupKind:
				comment += ", " + string(f.Message().FullName())
			}
			member := snake(string(f.Name()))
			if strings.HasSuffix(ctype, "*") {
				member = ctype + member
			} else {
				member = ctype + " " + member
			}
			if f.IsList() || f.IsMap() {
				fmt.Fprintf(b, "    %s;\n", member)
				fmt.Fprintf(b, "    size_t n_%s;\n", snake(string(f.Name())))
			} else {
				fmt.Fprintf(b, "    %s; /* %s */\n", member, comment)
				if g.needsHas(f) {
					fmt.Fprintf(b, "    int has_%s;\n", snake(string(f.Name())))
				}
			}
		}
		/* Extensions ride the extendee struct as trailing members; the
		 * comment names the extension so same-named fields elsewhere
		 * stay distinguishable. */
		for _, x := range g.extFields(m) {
			ctype, _, err := fieldCType(x)
			if err != nil {
				return err
			}
			member := snake(string(x.Name()))
			if strings.HasSuffix(ctype, "*") {
				member = ctype + member
			} else {
				member = ctype + " " + member
			}
			if x.IsList() {
				fmt.Fprintf(b, "    %s; /* extension %s */\n", member, x.FullName())
				fmt.Fprintf(b, "    size_t n_%s;\n", snake(string(x.Name())))
			} else {
				fmt.Fprintf(b, "    %s; /* extension %s */\n", member, x.FullName())
				if g.needsHas(x) {
					fmt.Fprintf(b, "    int has_%s;\n", snake(string(x.Name())))
				}
			}
		}
		fmt.Fprintf(b, "} %s_t;\n", cname(m))
		fmt.Fprintf(b, "\n")
	}
	return nil
}

func (g *generator) writeTables(b *strings.Builder) error {
	for _, m := range g.messages {
		fmt.Fprintf(b, "static const pbw_struct_field_t %s_fields[] = {\n", cname(m))
		if g.tableLen(m) == 0 {
			/* ISO C forbids an empty initializer list: a message with no
			 * field still gets one (unused) row, and the wrappers pass 0. */
			fmt.Fprintf(b, "    {0, PBW_SF_INT32, 0, 0, 0, 0, 0, 0, NULL, 0, PBW_TYPE_INT32, 0, 0, 0, 0, 0},\n")
		}
		fields := m.Fields()
		for i := 0; i < fields.Len(); i++ {
			if err := g.writeRow(b, m, fields.Get(i)); err != nil {
				return err
			}
		}
		for _, x := range g.extFields(m) {
			if err := g.writeRow(b, m, x); err != nil {
				return err
			}
		}
		fmt.Fprintf(b, "};\n")
		fmt.Fprintf(b, "\n")
	}
	return nil
}

func (g *generator) writeRow(b *strings.Builder, m protoreflect.MessageDescriptor, f protoreflect.FieldDescriptor) error {
	_, kind, err := fieldCType(f)
	if err != nil {
		return err
	}
	rep := 0
	if f.IsList() || f.IsMap() {
		rep = 1
	}
	member := snake(string(f.Name()))
	tname := cname(m) + "_t"
	off := fmt.Sprintf("offsetof(%s, %s)", tname, member)
	oneof := 0
	cs := "0"
	if od := realOneof(f); od != nil {
		oname := snake(string(od.Name()))
		uname := cname(m) + "_" + oname + "_u"
		off = fmt.Sprintf("offsetof(%s, %s) + offsetof(%s, %s)", tname, oname, uname, member)
		oneof = 1
		cs = fmt.Sprintf("offsetof(%s, %s_case)", tname, oname)
	}
	has := "0"
	if !f.IsList() && !f.IsMap() && g.needsHas(f) {
		has = fmt.Sprintf("offsetof(%s, has_%s)", tname, member)
	}
	count, elem := "0", "0"
	if f.IsList() || f.IsMap() {
		count = fmt.Sprintf("offsetof(%s, n_%s)", tname, member)
		if kind == "PBW_SF_MESSAGE" {
			elem = fmt.Sprintf("sizeof(%s_t)", cname(f.Message()))
		}
	} else if kind == "PBW_SF_MESSAGE" {
		elem = fmt.Sprintf("sizeof(%s_t)", cname(f.Message()))
	}
	dflt := "0"
	if kind == "PBW_SF_ENUM" && !f.IsList() && realOneof(f) == nil {
		// Proto3 implicit-presence enums carry no default; the
		// zero value applies and DefaultEnumValue() is nil.
		if dv := f.DefaultEnumValue(); dv != nil {
			dflt = fmt.Sprintf("%d", dv.Number())
		}
	}
	sub, nsub := "NULL", "0"
	if kind == "PBW_SF_MESSAGE" {
		sub = cname(f.Message()) + "_fields"
		nsub = fmt.Sprintf("%d", g.tableLen(f.Message()))
	}
	ptype, wire, err := pbwType(f)
	if err != nil {
		return err
	}
	packed := 0
	if f.IsPacked() {
		packed = 1
	}
	required := 0
	if f.Cardinality() == protoreflect.Required {
		required = 1
	}
	// The trailing type/wire/packed/required/oneof/cs members let the runtime
	// codec work without looking anything up in the schema.
	fmt.Fprintf(b, "    {%d, %s, %d, %s, %s, %s, %s, %s, %s, %s, %s, %d, %d, %d, %d, %s},\n",
		f.Number(), kind, rep, off, has, count, elem, dflt, sub, nsub,
		ptype, wire, packed, required, oneof, cs)
	return nil
}

// pbwType maps a field to its pbw_type value and wire type. It mirrors
// src/encode.c (wire_value / expected_wire_type); keep the two in sync.
func pbwType(f protoreflect.FieldDescriptor) (string, int, error) {
	switch f.Kind() {
	case protoreflect.DoubleKind:
		return "PBW_TYPE_DOUBLE", 1, nil
	case protoreflect.FloatKind:
		return "PBW_TYPE_FLOAT", 5, nil
	case protoreflect.Int64Kind:
		return "PBW_TYPE_INT64", 0, nil
	case protoreflect.Uint64Kind:
		return "PBW_TYPE_UINT64", 0, nil
	case protoreflect.Int32Kind:
		return "PBW_TYPE_INT32", 0, nil
	case protoreflect.Fixed64Kind:
		return "PBW_TYPE_FIXED64", 1, nil
	case protoreflect.Fixed32Kind:
		return "PBW_TYPE_FIXED32", 5, nil
	case protoreflect.BoolKind:
		return "PBW_TYPE_BOOL", 0, nil
	case protoreflect.StringKind:
		return "PBW_TYPE_STRING", 2, nil
	case protoreflect.BytesKind:
		return "PBW_TYPE_BYTES", 2, nil
	case protoreflect.GroupKind:
		return "PBW_TYPE_GROUP", 3, nil
	case protoreflect.MessageKind:
		return "PBW_TYPE_MESSAGE", 2, nil
	case protoreflect.Uint32Kind:
		return "PBW_TYPE_UINT32", 0, nil
	case protoreflect.EnumKind:
		return "PBW_TYPE_ENUM", 0, nil
	case protoreflect.Sfixed32Kind:
		return "PBW_TYPE_SFIXED32", 5, nil
	case protoreflect.Sfixed64Kind:
		return "PBW_TYPE_SFIXED64", 1, nil
	case protoreflect.Sint32Kind:
		return "PBW_TYPE_SINT32", 0, nil
	case protoreflect.Sint64Kind:
		return "PBW_TYPE_SINT64", 0, nil
	}
	return "", 0, fmt.Errorf("%s: unsupported kind %s", f.FullName(), f.Kind())
}

// writeSchema emits the descriptor-set path: the FileDescriptorSet lives
// in its own file (protoc --descriptor_set_out, see `make -C bindings/c/plugin
// regen`) and is loaded at runtime with pbw_struct_schema_load, so the
// header pins no descriptor bytes.
func (g *generator) writeSchema(b *strings.Builder) {
	base := strings.TrimSuffix(g.output, ".h")
	desc := strings.TrimSuffix(g.fd.Path(), ".proto") + ".desc"
	fmt.Fprintf(b, "/* FileDescriptorSet for %s, kept in %s.\n", g.fd.Path(), desc)
	fmt.Fprintf(b, " * Produced by protoc --descriptor_set_out; regenerate with\n")
	fmt.Fprintf(b, " * `make -C bindings/c/plugin regen`. Load it at runtime:\n")
	fmt.Fprintf(b, " * pbw_struct_schema_load(%s_DESC_PATH, &schema).\n", upper(base))
	fmt.Fprintf(b, " */\n")
	fmt.Fprintf(b, "#define %s_DESC_PATH \"%s\"\n", upper(base), desc)
	fmt.Fprintf(b, "\n")
}

func (g *generator) writeWrappers(b *strings.Builder) {
	for _, m := range g.messages {
		if m.IsMapEntry() {
			continue
		}
		t := cname(m) + "_t"
		n := g.tableLen(m)
		fmt.Fprintf(b, "static inline pbw_status\n")
		fmt.Fprintf(b, "%s_encode(const pbw_schema *schema, const %s *in, uint8_t **wire, size_t *wire_len)\n", cname(m), t)
		fmt.Fprintf(b, "{\n")
		fmt.Fprintf(b, "    return pbw_struct_encode(schema, \"%s\", %s_fields, %d, in, wire, wire_len);\n", m.FullName(), cname(m), n)
		fmt.Fprintf(b, "}\n")
		fmt.Fprintf(b, "\n")
		fmt.Fprintf(b, "static inline pbw_status\n")
		fmt.Fprintf(b, "%s_decode(const pbw_schema *schema, const uint8_t *wire, size_t wire_len, %s *out)\n", cname(m), t)
		fmt.Fprintf(b, "{\n")
		fmt.Fprintf(b, "    memset(out, 0, sizeof(*out));\n")
		fmt.Fprintf(b, "    return pbw_struct_decode(schema, \"%s\", %s_fields, %d, wire, wire_len, out);\n", m.FullName(), cname(m), n)
		fmt.Fprintf(b, "}\n")
		fmt.Fprintf(b, "\n")
		fmt.Fprintf(b, "static inline void\n")
		fmt.Fprintf(b, "%s_free(%s *msg)\n", cname(m), t)
		fmt.Fprintf(b, "{\n")
		fmt.Fprintf(b, "    pbw_struct_free(%s_fields, %d, msg);\n", cname(m), n)
		fmt.Fprintf(b, "}\n")
		fmt.Fprintf(b, "\n")
	}
}

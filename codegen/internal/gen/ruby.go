package gen

import (
	"fmt"
	"strings"

	"pbwire-codegen/internal/model"

	"google.golang.org/protobuf/reflect/protoreflect"
)

func rubyFiles(f *model.File, opt Options) ([]File, error) {
	// Models and schema part ways: classes in one file, the embedded
	// descriptor set plus its loader in the other. model=false leaves the
	// schema file for dynamic-only users. Everything stays inside the shared
	// Pbwire::Gen module, so the models call the same qualified helpers.
	var files []File
	if opt.Model {
		var b strings.Builder
		b.WriteString("# frozen_string_literal: true\n\n")
		b.WriteString(headerP(f, "#", "Generated classes over the dynamic Pbwire runtime: accessors, one encode/decode."))
		b.WriteString("require_relative '../../lib/pbwire'\n")
		fmt.Fprintf(&b, "require_relative '%s_meta'\n", base(f))
		for _, imp := range f.Imports {
			fmt.Fprintf(&b, "require_relative '%s'\n", stem(imp.Path))
		}
		b.WriteString("\n")
		b.WriteString("module Pbwire\n  module Gen\n")
		for _, e := range f.Enums {
			fmt.Fprintf(&b, "    # %s\n    module %s\n", e.Full, className(f, e.Name))
			for _, v := range e.Values {
				fmt.Fprintf(&b, "      %s = %d\n", v.Name, v.Number)
			}
			b.WriteString("      REVMAP = {\n")
			for _, v := range e.Values {
				fmt.Fprintf(&b, "        %d => :%s,\n", v.Number, v.Name)
			}
			b.WriteString("      }.freeze\n")
			b.WriteString("    end\n\n")
		}
		for _, m := range f.Messages {
			writeRubyMessage(&b, f, m)
		}
		// Nested types are reachable through their parents (Oneof1::Inner),
		// the way protoc's ruby_out nests them.
		known := map[string]bool{}
		for _, m := range f.Messages {
			known[m.Name] = true
		}
		for _, e := range f.Enums {
			known[e.Name] = true
		}
		for _, m := range f.Messages {
			aliasRuby(&b, f, known, m.Name)
		}
		for _, e := range f.Enums {
			aliasRuby(&b, f, known, e.Name)
		}
		b.WriteString("  end\nend\n")
		files = append(files, File{Name: base(f) + ".rb", Content: b.String()})
	}
	var meta strings.Builder
	writeRubyMeta(&meta, f, opt)
	files = append(files, File{Name: base(f) + "_meta.rb", Content: meta.String()})
	return files, nil
}

// writeRubyMeta emits the schema file: the embedded descriptor set plus an
// explicit loader, inside the shared Pbwire::Gen module. Schemas and types
// stay cached per descriptor set, so any number of generated files share
// this module without collisions. register=true loads lazily on first use
// (as before); otherwise init_schema (embedded bytes) or use_schema (an
// external schema) must run first, loudly otherwise. registry adds the
// whole-file message list plus a preload that warms every type.
func writeRubyMeta(b *strings.Builder, f *model.File, opt Options) {
	descConst := "DESC_B64_" + strings.ToUpper(base(f))
	b.WriteString("# frozen_string_literal: true\n\n")
	b.WriteString(headerP(f, "#", "Embedded descriptor set plus its loader: the schema side of the generated model."))
	b.WriteString("require_relative '../../lib/pbwire'\n")
	b.WriteString("require 'base64'\n\n")
	b.WriteString("module Pbwire\n  module Gen\n")
	b.WriteString("    # A decoded 32-bit signed field arrives as unsigned 64-bit bits.\n")
	b.WriteString("    def self.i32(value)\n      value = value.to_i\n      value >= 2**31 ? value - 2**32 : value\n    end\n\n")
	fmt.Fprintf(b, "    # The embedded FileDescriptorSet: no .desc file at use time.\n")
	fmt.Fprintf(b, "    %s = %q\n", descConst, opt.DescSetB64)
	b.WriteString("    @_schemas = {}\n")
	b.WriteString("    @_types = {}\n")
	b.WriteString("    def self.init_schema(desc_b64 = " + descConst + ")\n")
	b.WriteString("      @_schemas[desc_b64] ||= Pbwire::Schema.from_bytes(Base64.strict_decode64(desc_b64))\n")
	b.WriteString("    end\n")
	b.WriteString("    def self.use_schema(desc_b64, schema)\n")
	b.WriteString("      @_schemas[desc_b64] = schema\n")
	b.WriteString("      @_types.delete(desc_b64)\n")
	b.WriteString("    end\n")
	if opt.Register {
		b.WriteString("    def self._schema(desc_b64)\n")
		b.WriteString("      @_schemas[desc_b64] ||= Pbwire::Schema.from_bytes(Base64.strict_decode64(desc_b64))\n")
		b.WriteString("    end\n")
	} else {
		b.WriteString("    def self._schema(desc_b64)\n")
		b.WriteString("      @_schemas[desc_b64] or raise \"call init_schema first (generated with register=false)\"\n")
		b.WriteString("    end\n")
	}
	b.WriteString("    def self._type(desc_b64, full)\n")
	b.WriteString("      @_types[desc_b64] ||= {}\n")
	b.WriteString("      @_types[desc_b64][full] ||= _schema(desc_b64).message(full)\n")
	b.WriteString("    end\n")
	if opt.Registry {
		fmt.Fprintf(b, "\n    # Every message in this file, for whole-schema pre-registration.\n")
		fmt.Fprintf(b, "    MESSAGE_NAMES_%s = [\n", strings.ToUpper(base(f)))
		for _, m := range f.Messages {
			fmt.Fprintf(b, "      %q,\n", m.Full)
		}
		b.WriteString("    ].freeze\n\n")
		fmt.Fprintf(b, "    def self.preload(desc_b64 = %s)\n", descConst)
		fmt.Fprintf(b, "      MESSAGE_NAMES_%s.each { |n| _type(desc_b64, n) }\n", strings.ToUpper(base(f)))
		b.WriteString("    end\n")
	}
	b.WriteString("  end\nend\n")
}

// rubyMsg names the class for a message field: the owning file's prefix
// when foreign (the sibling file is required above), this file's own
// otherwise. Shared Pbwire::Gen modules make the reference resolve.
func rubyMsg(f *model.File, field model.Field) string {
	if field.Import != "" {
		if imp := findImport(f, field.Import); imp != nil {
			return imp.Prefix + "_" + field.Message
		}
	}
	return className(f, field.Message)
}

// aliasRuby re-attaches nested types to their parents. Both sides carry
// the package prefix (className), since that is the declared variable.
func aliasRuby(b *strings.Builder, f *model.File, known map[string]bool, flat string) {
	varOf := func(local string) string {
		for _, m := range f.Messages {
			if m.Name == local {
				return className(f, local)
			}
		}
		for _, e := range f.Enums {
			if e.Name == local {
				return className(f, local)
			}
		}
		return local
	}
	name := flat
	for {
		idx := strings.LastIndex(name, "_")
		if idx < 0 {
			return
		}
		parent, child := name[:idx], name[idx+1:]
		if known[parent] {
			fmt.Fprintf(b, "    %s::%s = %s\n", varOf(parent), child, varOf(flat))
			return
		}
		name = parent
	}
}

func writeRubyMessage(b *strings.Builder, f *model.File, m model.Message) {
	cls := className(f, m.Name)
	fmt.Fprintf(b, "    # %s\n    class %s\n", m.Full, cls)
	var names []string
	var params []string
	var assigns []string
	for _, field := range m.Fields {
		if field.Skip != "" {
			fmt.Fprintf(b, "      # skipped %s: %s\n", field.Name, field.Skip)
			continue
		}
		name := rubyName(field.Name)
		names = append(names, ":"+name)
		if field.NeedsHas {
			names = append(names, ":has_"+name)
			params = append(params, fmt.Sprintf("has_%s: false", name))
			// A non-nil value implies presence (the gem's rule); the flag
			// only matters to spell it out. Oneof members work the same way.
			assigns = append(assigns, fmt.Sprintf("@has_%s = has_%s || !%s.nil?", name, name, name))
		}
		switch {
		case field.Repeated:
			/* nil default, not []: a literal [] would be one array shared by
			 * every instance constructed without the argument. */
			params = append(params, fmt.Sprintf("%s: nil", name))
			assigns = append(assigns, fmt.Sprintf("@%s = %s || []", name, name))
			continue
		case field.Message != "":
			params = append(params, fmt.Sprintf("%s: nil", name))
		case field.NeedsHas:
			/* Optional value: nil default, presence follows the value. */
			params = append(params, fmt.Sprintf("%s: nil", name))
		case field.Kind == protoreflect.BoolKind:
			params = append(params, fmt.Sprintf("%s: false", name))
		case field.Kind == protoreflect.StringKind, field.Kind == protoreflect.BytesKind:
			params = append(params, fmt.Sprintf("%s: nil", name))
		case field.Kind == protoreflect.FloatKind, field.Kind == protoreflect.DoubleKind:
			params = append(params, fmt.Sprintf("%s: 0.0", name))
		default:
			params = append(params, fmt.Sprintf("%s: 0", name))
		}
		assigns = append(assigns, fmt.Sprintf("@%s = %s", name, name))
	}
	if len(names) > 0 {
		fmt.Fprintf(b, "      attr_accessor %s\n\n", strings.Join(names, ", "))
	}
	b.WriteString("      def initialize(")
	if len(params) > 0 {
		b.WriteString(strings.Join(params, ", "))
	}
	b.WriteString(")\n")
	for _, line := range assigns {
		fmt.Fprintf(b, "        %s\n", line)
	}
	// Oneof: at most one member is set; the last one in declaration order wins,
	// matching the wire merge and the gem's assignment semantics.
	oneofs := map[string][]model.Field{}
	var order []string
	for _, field := range m.Fields {
		if field.Skip != "" || field.Oneof == "" {
			continue
		}
		if _, ok := oneofs[field.Oneof]; !ok {
			order = append(order, field.Oneof)
		}
		oneofs[field.Oneof] = append(oneofs[field.Oneof], field)
	}
	for _, o := range order {
		members := oneofs[o]
		if len(members) < 2 {
			continue
		}
		fmt.Fprintf(b, "        _last_%s = nil\n", o)
		for _, field := range members {
			name := rubyName(field.Name)
			fmt.Fprintf(b, "        _last_%s = :%s unless @%s.nil?\n", o, name, name)
		}
		for _, field := range members {
			name := rubyName(field.Name)
			fmt.Fprintf(b, "        if _last_%s && _last_%s != :%s\n", o, o, name)
			fmt.Fprintf(b, "          @%s = nil\n", name)
			if field.NeedsHas {
				fmt.Fprintf(b, "          @has_%s = false\n", name)
			}
			b.WriteString("        end\n")
		}
	}
	b.WriteString("      end\n\n")

	fmt.Fprintf(b, "      def to_message(schema)\n")
	fmt.Fprintf(b, "        msg = schema.message(%q)\n", m.Full)
	for _, field := range m.Fields {
		if field.Skip != "" {
			continue
		}
		name := rubyName(field.Name)
		cond := rubyPresent(field, name)
		switch {
		case field.Repeated && field.Message != "":
			fmt.Fprintf(b, "        msg[%q] = @%s.map { |v| v.to_message(schema) } if %s\n", field.Name, name, cond)
		case field.Repeated:
			fmt.Fprintf(b, "        msg[%q] = @%s if %s\n", field.Name, name, cond)
		case field.Message != "":
			fmt.Fprintf(b, "        msg[%q] = @%s.to_message(schema) if %s\n", field.Name, name, cond)
		default:
			fmt.Fprintf(b, "        msg[%q] = @%s if %s\n", field.Name, name, cond)
		}
	}
	b.WriteString("        msg\n      end\n\n")
	b.WriteString("      def encode(schema)\n        to_message(schema).encode\n      end\n\n")
	b.WriteString("      # Embedded-schema facade (no schema argument), mirroring the\n")
	b.WriteString("      # google-protobuf gem's Message.encode/decode. One schema object\n")
	b.WriteString("      # per file, so nested messages never cross schemas.\n")
	descConst := "DESC_B64_" + strings.ToUpper(base(f))
	b.WriteString("      def self.encode(msg)\n        msg.to_message(Pbwire::Gen._schema(" + descConst + ")).encode\n      rescue NoMethodError => e\n")
	b.WriteString("        # A mistyped field trips a presence predicate (.empty?/.zero?)\n")
	b.WriteString("        # before the extension sees it; report it as a type error.\n")
	b.WriteString("        raise TypeError, e.message\n      end\n\n")
	b.WriteString("      # One argument decodes with the embedded schema; two keep\n")
	b.WriteString("      # the old (schema, wire) form.\n")
	fmt.Fprintf(b, "      def self.decode(a, b = nil)\n")
	fmt.Fprintf(b, "        return from_message(Pbwire::Gen._type(%s, %q).decode(a)) if b.nil?\n", descConst, m.Full)
	fmt.Fprintf(b, "        from_message(a.message(%q).decode(b))\n      end\n\n", m.Full)
	b.WriteString("      def to_proto\n        self.class.encode(self)\n      end\n\n")
	writeRubyToH(b, f, m)
	writeRubyHas(b, m)

	fmt.Fprintf(b, "      def self.from_message(msg)\n")
	fmt.Fprintf(b, "        new(\n")
	var args []string
	for _, field := range m.Fields {
		if field.Skip != "" {
			continue
		}
		name := rubyName(field.Name)
		if field.NeedsHas {
			args = append(args, fmt.Sprintf("has_%s: !msg[%q].nil?", name, field.Name))
		}
		args = append(args, fmt.Sprintf("%s: %s", name, rubyFromMessage(f, field)))
	}
	if len(args) > 0 {
		fmt.Fprintf(b, "          %s,\n", strings.Join(args, ",\n          "))
	}
	b.WriteString("        )\n      end\n\n")
	b.WriteString("    end\n\n")
}

// writeRubyToH emits #to_h: present fields only, proto-name symbol keys,
// enums as name Symbols, nested messages as hashes. Mirrors the gem's to_h.
func writeRubyToH(b *strings.Builder, f *model.File, m model.Message) {
	b.WriteString("      def to_h\n        h = {}\n")
	for _, field := range m.Fields {
		if field.Skip != "" {
			continue
		}
		name := rubyName(field.Name)
		cond := rubyPresent(field, name)
		key := fmt.Sprintf(":%s", field.Name)
		switch {
		case field.Repeated && field.Message != "":
			fmt.Fprintf(b, "        h[%s] = @%s.map(&:to_h) if %s\n", key, name, cond)
		case field.Repeated:
			fmt.Fprintf(b, "        h[%s] = @%s.dup if %s\n", key, name, cond)
		case field.Message != "":
			fmt.Fprintf(b, "        h[%s] = @%s.to_h if %s\n", key, name, cond)
		case field.Kind == protoreflect.EnumKind && field.Enum != "":
			fmt.Fprintf(b, "        h[%s] = (%s::REVMAP[@%s] || @%s) if %s\n",
				key, className(f, field.Enum), name, name, cond)
		case field.Kind == protoreflect.EnumKind:
			fmt.Fprintf(b, "        h[%s] = @%s if %s\n", key, name, cond)
		default:
			fmt.Fprintf(b, "        h[%s] = @%s if %s\n", key, name, cond)
		}
	}
	b.WriteString("        h\n      end\n\n")
}

// writeRubyHas emits has_<field>? for presence-carrying fields only, like
// the gem (implicit-presence fields have no predicate). Presence answers the
// write semantic: the has_ flag the constructor took.
func writeRubyHas(b *strings.Builder, m model.Message) {
	for _, field := range m.Fields {
		if field.Skip != "" {
			continue
		}
		if field.Message == "" && !field.NeedsHas && !field.Required {
			continue
		}
		name := rubyName(field.Name)
		if field.Required {
			fmt.Fprintf(b, "      def has_%s?\n        true\n      end\n\n", name)
		} else if field.NeedsHas {
			fmt.Fprintf(b, "      def has_%s?\n        @has_%s\n      end\n\n", name, name)
		} else {
			fmt.Fprintf(b, "      def has_%s?\n        !@%s.nil?\n      end\n\n", name, name)
		}
	}
}

// rubyName keeps a proto field name usable as a Ruby method and keyword
// argument: a name that starts with an uppercase letter would parse as a
// constant, so it gets an underscore prefix.
func rubyName(name string) string {
	if name == "" {
		return name
	}
	if name[0] >= 'A' && name[0] <= 'Z' {
		return "_" + name
	}
	return name
}

func rubyPresent(field model.Field, name string) string {
	if field.Required {
		return "true"
	}
	switch {
	case field.Repeated:
		return "!" + name + ".empty?"
	case field.Message != "":
		return "!" + name + ".nil?"
	case field.NeedsHas:
		return "@has_" + name
	case field.Kind == protoreflect.BoolKind:
		return "@" + name
	case field.Kind == protoreflect.StringKind, field.Kind == protoreflect.BytesKind:
		return "!" + name + ".nil? && !" + name + ".empty?"
	default:
		return "!" + name + ".zero?"
	}
}

func rubyFromMessage(f *model.File, field model.Field) string {
	key := fmt.Sprintf("msg[%q]", field.Name)
	switch {
	case field.NeedsHas:
		conv := key
		if rubySigned32(field) {
			conv = fmt.Sprintf("Pbwire::Gen.i32(%s)", key)
		}
		return fmt.Sprintf("(%s.nil? ? nil : %s)", key, conv)
	case field.Repeated && field.Message != "":
		return fmt.Sprintf("(%s || []).map { |v| %s.from_message(v) }", key, rubyMsg(f, field))
	case field.Repeated && rubySigned32(field):
		return fmt.Sprintf("(%s || []).map { |v| Pbwire::Gen.i32(v) }", key)
	case field.Repeated:
		return fmt.Sprintf("%s || []", key)
	case field.Message != "":
		return fmt.Sprintf("(v = %s) ? %s.from_message(v) : nil", key, rubyMsg(f, field))
	case field.Kind == protoreflect.StringKind, field.Kind == protoreflect.BytesKind:
		return key
	case field.Kind == protoreflect.BoolKind:
		return fmt.Sprintf("%s || false", key)
	case field.Kind == protoreflect.FloatKind, field.Kind == protoreflect.DoubleKind:
		return fmt.Sprintf("%s || 0.0", key)
	case rubySigned32(field):
		return fmt.Sprintf("Pbwire::Gen.i32(%s)", key)
	default:
		return fmt.Sprintf("%s || 0", key)
	}
}

func rubySigned32(field model.Field) bool {
	switch field.Kind {
	case protoreflect.Int32Kind, protoreflect.Sint32Kind, protoreflect.Sfixed32Kind, protoreflect.EnumKind:
		return true
	default:
		return false
	}
}

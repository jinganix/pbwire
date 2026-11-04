package gen

import (
	"fmt"
	"strings"

	"pbwire-typed/internal/model"

	"google.golang.org/protobuf/reflect/protoreflect"
)

func rubyFiles(f *model.File) []File {
	var b strings.Builder
	b.WriteString("# frozen_string_literal: true\n\n")
	b.WriteString(headerP(f, "#", "Typed classes over the dynamic Pbwire runtime: accessors, one encode/decode."))
	b.WriteString("module Pbwire\n  module Gen\n")
	b.WriteString("    # A decoded 32-bit signed field arrives as unsigned 64-bit bits.\n")
	b.WriteString("    def self.i32(value)\n      value = value.to_i\n      value >= 2**31 ? value - 2**32 : value\n    end\n\n")
	for _, e := range f.Enums {
		fmt.Fprintf(&b, "    # %s\n    module %s\n", e.Full, className(f, e.Name))
		for _, v := range e.Values {
			fmt.Fprintf(&b, "      %s = %d\n", v.Name, v.Number)
		}
		b.WriteString("    end\n\n")
	}
	for _, m := range f.Messages {
		writeRubyMessage(&b, f, m)
	}
	b.WriteString("  end\nend\n")
	return []File{{Name: base(f) + ".rb", Content: b.String()}}
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
			assigns = append(assigns, fmt.Sprintf("@has_%s = has_%s", name, name))
		}
		switch {
		case field.Repeated:
			params = append(params, fmt.Sprintf("%s: []", name))
		case field.Message != "":
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
			fmt.Fprintf(b, "        msg[%q] = @%s.map { |v| v.to_message(schema) } if %s\n", name, name, cond)
		case field.Repeated:
			fmt.Fprintf(b, "        msg[%q] = @%s if %s\n", name, name, cond)
		case field.Message != "":
			fmt.Fprintf(b, "        msg[%q] = @%s.to_message(schema) if %s\n", name, name, cond)
		default:
			fmt.Fprintf(b, "        msg[%q] = @%s if %s\n", name, name, cond)
		}
	}
	b.WriteString("        msg\n      end\n\n")
	b.WriteString("      def encode(schema)\n        to_message(schema).encode\n      end\n\n")

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
	fmt.Fprintf(b, "      def self.decode(schema, wire)\n")
	fmt.Fprintf(b, "        from_message(schema.message(%q).decode(wire))\n      end\n", m.Full)
	b.WriteString("    end\n\n")
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
	case field.Repeated && field.Message != "":
		return fmt.Sprintf("(%s || []).map { |v| %s.from_message(v) }", key, className(f, field.Message))
	case field.Repeated && rubySigned32(field):
		return fmt.Sprintf("(%s || []).map { |v| Pbwire::Gen.i32(v) }", key)
	case field.Repeated:
		return fmt.Sprintf("%s || []", key)
	case field.Message != "":
		return fmt.Sprintf("(v = %s) ? %s.from_message(v) : nil", key, className(f, field.Message))
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

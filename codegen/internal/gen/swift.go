package gen

import (
	"fmt"
	"strings"

	"pbwire-codegen/internal/model"

	"google.golang.org/protobuf/reflect/protoreflect"
)

// swiftFiles emits one Swift file: generated structs over the Swift FFI runtime
// (Sources/Pbwire: Schema/Message/PbwireValue, FileDescriptorSet at run
// time). The descriptor set travels embedded as base64, so no .desc file is
// needed at use time. Oneof members are plain optionals (no enum wrapper).
func swiftFiles(f *model.File, opt Options) []File {
	var b strings.Builder
	b.WriteString(header(f, "Generated structs over the Swift FFI runtime (Schema/Message/PbwireValue)."))
	b.WriteString("import Foundation\nimport Pbwire\n\n")
	b.WriteString("private let _descData = Data(base64Encoded: \"\"\"\n")
	for i := 0; i < len(opt.DescSetB64); i += 76 {
		end := i + 76
		if end > len(opt.DescSetB64) {
			end = len(opt.DescSetB64)
		}
		b.WriteString(opt.DescSetB64[i:end] + "\n")
	}
	b.WriteString("\"\"\".filter { !$0.isWhitespace })!\n\n")
	b.WriteString("private let _schema: Schema = {\n")
	b.WriteString("    do { return try Schema(bytes: _descData) } catch {\n")
	b.WriteString("        fatalError(\"pbwire: embedded descriptor set: \\(error)\")\n")
	b.WriteString("    }\n}()\n\n")
	for _, e := range f.Enums {
		fmt.Fprintf(&b, "/// %s\n", e.Full)
		fmt.Fprintf(&b, "public enum %s: Int64 {\n", className(f, e.Name))
		for _, v := range e.Values {
			fmt.Fprintf(&b, "    case %s = %d\n", swiftEnumCase(v.Name), v.Number)
		}
		b.WriteString("}\n\n")
	}
	for _, m := range f.Messages {
		writeSwiftMessage(&b, f, m)
	}
	return []File{{Name: base(f) + ".swift", Content: b.String()}}
}

func swiftEnumCase(name string) string {
	lower := strings.ToLower(name)
	parts := strings.Split(lower, "_")
	if len(parts) == 1 {
		return parts[0]
	}
	out := parts[0]
	for _, p := range parts[1:] {
		if p == "" {
			continue
		}
		out += strings.ToUpper(p[:1]) + p[1:]
	}
	return out
}

// swiftMsg names the struct for a message field: the owning file's prefix
// when foreign (one Swift module, so no import line), this file's own
// otherwise.
func swiftMsg(f *model.File, field model.Field) string {
	if field.Import != "" {
		if imp := findImport(f, field.Import); imp != nil {
			return imp.Prefix + "_" + field.Message
		}
	}
	return className(f, field.Message)
}

func swiftType(f *model.File, field model.Field) string {
	if field.Message != "" {
		return swiftMsg(f, field)
	}
	// Enums stay integers (never narrowed): an unknown wire value
	// round-trips unchanged. The generated enum holds the names.
	switch field.Kind {
	case protoreflect.Int32Kind, protoreflect.Sint32Kind, protoreflect.Sfixed32Kind,
		protoreflect.Int64Kind, protoreflect.Sint64Kind, protoreflect.Sfixed64Kind:
		return "Int64"
	case protoreflect.Uint32Kind, protoreflect.Fixed32Kind,
		protoreflect.Uint64Kind, protoreflect.Fixed64Kind:
		return "UInt64"
	case protoreflect.FloatKind:
		return "Float"
	case protoreflect.DoubleKind:
		return "Double"
	case protoreflect.BoolKind:
		return "Bool"
	case protoreflect.StringKind:
		return "String"
	case protoreflect.BytesKind:
		return "Data"
	case protoreflect.EnumKind:
		return "Int64"
	default:
		return "Int64"
	}
}

func swiftFieldType(f *model.File, field model.Field) string {
	t := swiftType(f, field)
	if field.Repeated {
		return "[" + t + "]"
	}
	if field.Message != "" || field.NeedsHas {
		return t + "?"
	}
	return t
}

func writeSwiftMessage(b *strings.Builder, f *model.File, m model.Message) {
	cls := className(f, m.Name)
	fmt.Fprintf(b, "/// %s\n", m.Full)
	fmt.Fprintf(b, "public struct %s {\n", cls)
	for _, field := range m.Fields {
		if field.Skip != "" {
			fmt.Fprintf(b, "    // skipped %s: %s\n", field.Name, field.Skip)
			continue
		}
		ft := swiftFieldType(f, field)
		if field.Repeated {
			fmt.Fprintf(b, "    public var %s: %s = []\n", field.Lower, ft)
		} else if field.Message != "" || field.NeedsHas {
			fmt.Fprintf(b, "    public var %s: %s = nil\n", field.Lower, ft)
		} else {
			fmt.Fprintf(b, "    public var %s: %s = %s\n", field.Lower, ft, swiftZero(ft))
		}
	}
	fmt.Fprintf(b, "    public init() {}\n\n")
	// fill writes fields into an existing Message (root or nested view).
	// Internal (not fileprivate): sibling-file structs fill nested values
	// of imported types.
	b.WriteString("    func fill(_ msg: Message) throws {\n")
	for _, field := range m.Fields {
		if field.Skip != "" {
			continue
		}
		writeSwiftFill(b, f, field)
	}
	b.WriteString("    }\n\n")
	fmt.Fprintf(b, "    public func encode() throws -> Data {\n")
	fmt.Fprintf(b, "        guard let msg = _schema.message(%q) else {\n", m.Full)
	fmt.Fprintf(b, "            throw NSError(domain: \"pbwire\", code: -1, userInfo: [NSLocalizedDescriptionKey: \"no such type: %s\"])\n", m.Full)
	fmt.Fprintf(b, "        }\n")
	b.WriteString("        try fill(msg)\n")
	b.WriteString("        return try msg.encode()\n    }\n\n")
	fmt.Fprintf(b, "    public static func decode(_ data: Data) throws -> %s {\n", cls)
	fmt.Fprintf(b, "        guard let ty = _schema.message(%q) else {\n", m.Full)
	fmt.Fprintf(b, "            throw NSError(domain: \"pbwire\", code: -1, userInfo: [NSLocalizedDescriptionKey: \"no such type: %s\"])\n", m.Full)
	fmt.Fprintf(b, "        }\n")
	b.WriteString("        return try fromMessage(ty.decode(data))\n    }\n\n")
	fmt.Fprintf(b, "    static func fromMessage(_ msg: Message) throws -> %s {\n", cls)
	b.WriteString("        var out = " + cls + "()\n")
	for _, field := range m.Fields {
		if field.Skip != "" {
			continue
		}
		writeSwiftLoad(b, f, field)
	}
	b.WriteString("        return out\n    }\n}\n\n")
}

func swiftZero(t string) string {
	switch t {
	case "Int64", "UInt64":
		return "0"
	case "Float", "Double":
		return "0"
	case "Bool":
		return "false"
	case "String":
		return "\"\""
	case "Data":
		return "Data()"
	default:
		return "0"
	}
}

// swiftWrap builds the PbwireValue expression for a scalar field.
func swiftWrap(field model.Field, src string) string {
	switch field.Kind {
	case protoreflect.Int32Kind, protoreflect.Sint32Kind, protoreflect.Sfixed32Kind,
		protoreflect.Int64Kind, protoreflect.Sint64Kind, protoreflect.Sfixed64Kind:
		return ".int(" + src + ")"
	case protoreflect.Uint32Kind, protoreflect.Fixed32Kind,
		protoreflect.Uint64Kind, protoreflect.Fixed64Kind:
		return ".uint(" + src + ")"
	case protoreflect.EnumKind:
		return ".int(" + src + ")"
	case protoreflect.BoolKind:
		return ".bool(" + src + ")"
	case protoreflect.FloatKind:
		return ".float(" + src + ")"
	case protoreflect.DoubleKind:
		return ".double(" + src + ")"
	case protoreflect.StringKind:
		return ".string(" + src + ")"
	case protoreflect.BytesKind:
		return ".bytes(" + src + ")"
	default:
		return ".int(" + src + ")"
	}
}

func writeSwiftFill(b *strings.Builder, f *model.File, field model.Field) {
	name := field.Lower
	proto := field.Name
	switch {
	case field.Message != "" && !field.Repeated:
		fmt.Fprintf(b, "        if let v = %s { try v.fill(msg.makeNested(%q)) }\n", name, proto)
	case field.Message != "" && field.Repeated:
		fmt.Fprintf(b, "        for v in %s { try v.fill(msg.makeNested(%q)) }\n", name, proto)
	case field.Repeated:
		fmt.Fprintf(b, "        for v in %s { try msg.add(%s, %q) }\n", name, swiftWrap(field, "v"), proto)
	case field.NeedsHas:
		fmt.Fprintf(b, "        if let v = %s { try msg.set(%s, %q) }\n", name, swiftWrap(field, "v"), proto)
	case field.Required:
		fmt.Fprintf(b, "        try msg.set(%s, %q)\n", swiftWrap(field, name), proto)
	default:
		// Proto3 implicit: zero stays off the wire, like the zero value
		// the decoder produces for an absent field.
		fmt.Fprintf(b, "        %s\n", swiftGuard(field, name, proto))
	}
}

// swiftGuard renders the write guard for an implicit-presence scalar.
func swiftGuard(field model.Field, name, proto string) string {
	set := fmt.Sprintf("try msg.set(%s, %q)", swiftWrap(field, name), proto)
	switch field.Kind {
	case protoreflect.BoolKind:
		return fmt.Sprintf("if %s { %s }", name, set)
	case protoreflect.StringKind, protoreflect.BytesKind:
		return fmt.Sprintf("if !%s.isEmpty { %s }", name, set)
	default:
		return fmt.Sprintf("if %s != 0 { %s }", name, set)
	}
}

// swiftCase is the PbwireValue case and binding for a scalar field.
func swiftCase(field model.Field) (string, string) {
	switch field.Kind {
	case protoreflect.Uint32Kind, protoreflect.Fixed32Kind,
		protoreflect.Uint64Kind, protoreflect.Fixed64Kind:
		return "uint", "v"
	case protoreflect.BoolKind:
		return "bool", "v"
	case protoreflect.FloatKind:
		return "float", "v"
	case protoreflect.DoubleKind:
		return "double", "v"
	case protoreflect.StringKind:
		return "string", "v"
	case protoreflect.BytesKind:
		return "bytes", "v"
	default:
		return "int", "v"
	}
}

func writeSwiftLoad(b *strings.Builder, f *model.File, field model.Field) {
	name := field.Lower
	proto := field.Name
	t := swiftType(f, field)
	switch {
	case field.Message != "" && !field.Repeated:
		fmt.Fprintf(b, "        if let n = msg.nested(%q) { out.%s = try %s.fromMessage(n) }\n", proto, name, t)
	case field.Message != "" && field.Repeated:
		fmt.Fprintf(b, "        for i in 0..<msg.count(%q) {\n", proto)
		fmt.Fprintf(b, "            if case .message(let n)? = msg.at(%q, i) { out.%s.append(try %s.fromMessage(n)) }\n", proto, name, t)
		b.WriteString("        }\n")
	case field.Repeated:
		c, _ := swiftCase(field)
		fmt.Fprintf(b, "        for i in 0..<msg.count(%q) {\n", proto)
		fmt.Fprintf(b, "            if case .%s(let v)? = msg.at(%q, i) { out.%s.append(v) }\n", c, proto, name)
		b.WriteString("        }\n")
	default:
		// Optional or implicit: absent stays nil/zero, same as the
		// dynamic API. `if case ...?` matches the optional Value.
		c, _ := swiftCase(field)
		fmt.Fprintf(b, "        if case .%s(let v)? = msg.get(%q) { out.%s = v }\n", c, proto, name)
	}
}

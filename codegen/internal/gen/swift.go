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
func swiftFiles(f *model.File, opt Options) ([]File, error) {
	// Models and schema part ways: structs in one file, the embedded
	// descriptor set plus its loader in the other. model=false leaves the
	// schema file for dynamic-only users. The schema lets are stem-prefixed:
	// same-module files share globals, so one flat _schema would collide.
	stem := base(f)
	var files []File
	if opt.Model {
		var b strings.Builder
		b.WriteString(header(f, "Generated structs over the Swift FFI runtime (Schema/Message/PbwireValue)."))
		b.WriteString("import Foundation\nimport Pbwire\n\n")
		for _, e := range f.Enums {
			fmt.Fprintf(&b, "/// %s\n", e.Full)
			fmt.Fprintf(&b, "public enum %s: Int64 {\n", className(f, e.Name))
			for _, v := range e.Values {
				fmt.Fprintf(&b, "    case %s = %d\n", swiftEnumCase(v.Name), v.Number)
			}
			b.WriteString("}\n\n")
		}
		for _, m := range f.Messages {
			writeSwiftMessage(&b, f, m, stem)
			writeSwiftLayout(&b, f, m)
		}
		files = append(files, File{Name: stem + ".swift", Content: b.String()})
	}
	var meta strings.Builder
	writeSwiftMeta(&meta, f, opt, stem)
	files = append(files, File{Name: stem + "_meta.swift", Content: meta.String()})
	return files, nil
}

// writeSwiftMeta emits the schema file: the embedded descriptor set plus an
// explicit loader. register=true keeps the lazy schema (as before) with an
// override slot; otherwise initSchema (embedded) or useSchema (external) must
// run first, loudly otherwise. registry adds the whole-file message list
// plus a preload that warms every type.
func writeSwiftMeta(b *strings.Builder, f *model.File, opt Options, stem string) {
	b.WriteString(header(f, "Embedded descriptor set plus its loader: the schema side of the generated model."))
	b.WriteString("import Foundation\nimport Pbwire\n\n")
	fmt.Fprintf(b, "private let %s_descData = Data(base64Encoded: \"\"\"\n", stem)
	for i := 0; i < len(opt.DescSetB64); i += 76 {
		end := i + 76
		if end > len(opt.DescSetB64) {
			end = len(opt.DescSetB64)
		}
		b.WriteString(opt.DescSetB64[i:end] + "\n")
	}
	b.WriteString("\"\"\".filter { !$0.isWhitespace })!\n\n")
	if opt.Register {
		fmt.Fprintf(b, "private let _%s_embedded: Schema = {\n", stem)
		b.WriteString("    do { return try Schema(bytes: " + stem + "_descData) } catch {\n")
		b.WriteString("        fatalError(\"pbwire: embedded descriptor set: \\(error)\")\n")
		b.WriteString("    }\n}()\n\n")
	}
	fmt.Fprintf(b, "private let %s_lock = NSLock()\n", stem)
	fmt.Fprintf(b, "private var _%s_override: Schema? = nil\n\n", stem)
	fmt.Fprintf(b, "internal func %s_initSchema(_ bytes: Data = %s_descData) {\n", stem, stem)
	fmt.Fprintf(b, "    %s_lock.lock(); defer { %s_lock.unlock() }\n", stem, stem)
	fmt.Fprintf(b, "    _%s_override = try! Schema(bytes: bytes)\n", stem)
	b.WriteString("}\n\n")
	fmt.Fprintf(b, "/// Registers an externally loaded schema: models encode/decode through it instead of the embedded set. A nil call clears the override.\n")
	fmt.Fprintf(b, "internal func %s_useSchema(_ s: Schema?) {\n", stem)
	fmt.Fprintf(b, "    %s_lock.lock(); defer { %s_lock.unlock() }\n", stem, stem)
	fmt.Fprintf(b, "    _%s_override = s\n", stem)
	b.WriteString("}\n\n")
	fmt.Fprintf(b, "internal var %s_schema: Schema {\n", stem)
	fmt.Fprintf(b, "    %s_lock.lock(); defer { %s_lock.unlock() }\n", stem, stem)
	if opt.Register {
		fmt.Fprintf(b, "    return _%s_override ?? _%s_embedded\n", stem, stem)
	} else {
		b.WriteString("    guard let s = _" + stem + "_override else {\n")
		b.WriteString("        fatalError(\"call " + stem + "_initSchema() first (generated with register=false)\")\n")
		b.WriteString("    }\n")
		b.WriteString("    return s\n")
	}
	b.WriteString("}\n")
	if opt.Registry {
		fmt.Fprintf(b, "\n/// Every message in this file, for whole-schema pre-registration.\n")
		fmt.Fprintf(b, "internal let %s_messageNames: [String] = [\n", stem)
		for _, m := range f.Messages {
			fmt.Fprintf(b, "    %q,\n", m.Full)
		}
		b.WriteString("]\n\n")
		fmt.Fprintf(b, "/// Warms every message type, so later calls pay no lookup.\n")
		fmt.Fprintf(b, "internal func %s_preload() {\n", stem)
		fmt.Fprintf(b, "    let s = %s_schema\n", stem)
		fmt.Fprintf(b, "    for n in %s_messageNames {\n", stem)
		b.WriteString("        guard s.message(n) != nil else {\n")
		b.WriteString("            fatalError(\"pbwire: embedded type missing: \\(n)\")\n")
		b.WriteString("        }\n")
		b.WriteString("    }\n")
		b.WriteString("}\n")
	}
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

func writeSwiftMessage(b *strings.Builder, f *model.File, m model.Message, stem string) {
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
	fmt.Fprintf(b, "        guard let msg = %s_schema.message(%q) else {\n", stem, m.Full)
	fmt.Fprintf(b, "            throw NSError(domain: \"pbwire\", code: -1, userInfo: [NSLocalizedDescriptionKey: \"no such type: %s\"])\n", m.Full)
	fmt.Fprintf(b, "        }\n")
	b.WriteString("        try fill(msg)\n")
	b.WriteString("        return try msg.encode()\n    }\n\n")
	fmt.Fprintf(b, "    public static func decode(_ data: Data) throws -> %s {\n", cls)
	fmt.Fprintf(b, "        guard let ty = %s_schema.message(%q) else {\n", stem, m.Full)
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

// swiftLayoutKindInfo maps one field kind to the mirror's slot type and the
// codec's precomputed wire facts: the pbw_struct kind, the pbw type and the
// wire type of one element. A slot type of "" means a message pointer, whose
// element type the child mirror names.
func swiftLayoutKindInfo(field model.Field) (slot string, cKind, typ, wire int, ok bool) {
	switch field.Kind {
	case protoreflect.Int32Kind:
		return "Int32", 0, 5, 0, true
	case protoreflect.Sint32Kind:
		return "Int32", 0, 17, 0, true
	case protoreflect.Sfixed32Kind:
		return "Int32", 0, 15, 5, true
	case protoreflect.Int64Kind:
		return "Int64", 1, 3, 0, true
	case protoreflect.Sint64Kind:
		return "Int64", 1, 18, 0, true
	case protoreflect.Sfixed64Kind:
		return "Int64", 1, 16, 1, true
	case protoreflect.Uint32Kind:
		return "UInt32", 2, 13, 0, true
	case protoreflect.Fixed32Kind:
		return "UInt32", 2, 7, 5, true
	case protoreflect.Uint64Kind:
		return "UInt64", 3, 4, 0, true
	case protoreflect.Fixed64Kind:
		return "UInt64", 3, 6, 1, true
	case protoreflect.BoolKind:
		return "Bool", 4, 8, 0, true
	case protoreflect.FloatKind:
		return "Float", 5, 2, 5, true
	case protoreflect.DoubleKind:
		return "Double", 6, 1, 1, true
	case protoreflect.StringKind:
		// NUL-terminated UTF-8, the C codec's string contract: a string
		// cannot carry an embedded NUL here (bytes can).
		return "UnsafePointer<CChar>?", 7, 9, 2, true
	case protoreflect.BytesKind:
		// The core's inline bytes value (pointer + length), not a copy.
		return "Pbwire.PbwireBytes", 8, 12, 2, true
	case protoreflect.EnumKind:
		return "Int32", 9, 14, 0, true
	case protoreflect.MessageKind:
		return "", 10, 11, 2, true
	case protoreflect.GroupKind:
		return "", 10, 10, 3, true
	}
	return "", 0, 0, 0, false
}

// swiftLayoutChild names the mirror of a message field's child type: the same
// module (one Swift module), so no import line.
func swiftLayoutChild(f *model.File, field model.Field) string {
	return swiftMsg(f, field) + "Repr"
}

// swiftLayoutSlot is one scalar/message slot type; a message slot points at
// the child mirror, so the image is a tree of caller-owned memory.
func swiftLayoutSlot(f *model.File, field model.Field) string {
	slot, cKind, _, _, _ := swiftLayoutKindInfo(field)
	if cKind == 10 {
		return "UnsafeMutablePointer<" + swiftLayoutChild(f, field) + ">?"
	}
	return slot
}

// swiftLayoutElem is the element type of a repeated slot's array: the scalar
// slot without its optionality, or the child mirror.
func swiftLayoutElem(f *model.File, field model.Field) string {
	if field.Message != "" {
		return swiftLayoutChild(f, field)
	}
	slot, _, _, _, _ := swiftLayoutKindInfo(field)
	return strings.TrimSuffix(slot, "?")
}

// swiftLayoutZero is the zero value of a mirror slot (a mirror is a value
// type with no references, so every slot has a compile-time default). An empty
// result means the slot has none: a C struct's initializer cannot appear in a
// @frozen type's property initializer (the defining module is not imported
// here), so `init()` assigns those slots instead.
func swiftLayoutZero(slot string) string {
	switch {
	case strings.HasSuffix(slot, "?"):
		return "nil"
	case slot == "Bool":
		return "false"
	case slot == "Pbwire.PbwireBytes":
		return ""
	}
	return "0"
}

// swiftLayoutOK reports whether a message's mirror is expressible: every field
// has a slot, no message recurses into itself (a pointer cycle would need an
// unbounded image), and every local child is expressible too.
func swiftLayoutOK(f *model.File, m model.Message) bool {
	return swiftLayoutOKMsg(f, m, map[string]bool{})
}

func swiftLayoutOKMsg(f *model.File, m model.Message, stack map[string]bool) bool {
	if stack[m.Name] {
		return false
	}
	stack[m.Name] = true
	defer delete(stack, m.Name)
	for _, field := range m.Fields {
		if field.Skip != "" {
			return false
		}
		if _, _, _, _, ok := swiftLayoutKindInfo(field); !ok {
			return false
		}
		if field.Kind != protoreflect.MessageKind && field.Kind != protoreflect.GroupKind {
			continue
		}
		if field.Import != "" {
			continue // the sibling file emits its own mirror
		}
		var child *model.Message
		for i := range f.Messages {
			if f.Messages[i].Name == field.Message {
				child = &f.Messages[i]
			}
		}
		if child == nil || !swiftLayoutOKMsg(f, *child, stack) {
			return false
		}
	}
	return true
}

// swiftCamel camelcases a proto identifier (a oneof's case word name).
func swiftCamel(name string) string {
	parts := strings.Split(name, "_")
	out := ""
	for _, p := range parts {
		if p == "" {
			continue
		}
		out += strings.ToUpper(p[:1]) + p[1:]
	}
	if out == "" {
		return "case"
	}
	return out
}

// swiftLayoutOffset is the row offset of one mirror slot. MemoryLayout knows
// the layout Swift actually chose, so the generated table can never disagree
// with the memory the C codec walks (the Rust mirror asserts offset_of!
// instead; Swift has no compile-time offset).
func swiftLayoutOffset(cls, slot string) string {
	return "MemoryLayout<" + cls + ">.offset(of: \\." + slot + ")!"
}

// writeSwiftLayout emits the flat @frozen mirror of one message plus the
// layout-table rows the C codec is handed. Slots hold no references, so the
// in-place walk pays no ARC; a repeated field carries its own count and a
// oneof its own case word. Slot order: values (with counts) first, then has
// flags, then case words, so neither a has flag nor a case word can sit at
// offset 0 (the codec reads 0 as "no such slot").
func writeSwiftLayout(b *strings.Builder, f *model.File, m model.Message) {
	if !swiftLayoutOK(f, m) {
		return
	}
	cls := className(f, m.Name) + "Repr"
	taken := map[string]bool{}
	unique := func(name string) string {
		for taken[name] {
			name += "0"
		}
		taken[name] = true
		return name
	}
	countOf := make([]string, len(m.Fields))
	hasOf := make([]string, len(m.Fields))
	caseOf := map[string]string{}
	var caseOrder []string
	for i, field := range m.Fields {
		taken[field.Lower] = true
		if field.Repeated {
			countOf[i] = unique("count" + field.Pascal)
		}
	}
	for i, field := range m.Fields {
		if field.Repeated || field.Oneof != "" || !(field.NeedsHas || field.Required) {
			continue
		}
		if _, cKind, _, _, _ := swiftLayoutKindInfo(field); cKind == 7 || cKind == 8 || cKind == 10 {
			continue // a pointer-shaped slot is absent when nil
		}
		hasOf[i] = unique("has" + field.Pascal)
	}
	for _, field := range m.Fields {
		if field.Repeated || field.Oneof == "" {
			continue
		}
		if _, ok := caseOf[field.Oneof]; !ok {
			caseOf[field.Oneof] = unique(swiftCamel(field.Oneof) + "Case")
			caseOrder = append(caseOrder, field.Oneof)
		}
	}

	var decls, rows, inits []string
	for i, field := range m.Fields {
		slot := swiftLayoutSlot(f, field)
		if field.Repeated {
			slot = "UnsafeMutablePointer<" + swiftLayoutElem(f, field) + ">?"
		}
		zero := swiftLayoutZero(slot)
		if zero == "" {
			decls = append(decls, "    public var "+field.Lower+": "+slot)
			inits = append(inits, "        "+field.Lower+" = "+slot+"()")
		} else {
			decls = append(decls, "    public var "+field.Lower+": "+slot+" = "+zero)
		}
		if field.Repeated {
			decls = append(decls, "    public var "+countOf[i]+": Int = 0")
		}
	}
	for i := range m.Fields {
		if hasOf[i] != "" {
			decls = append(decls, "    public var "+hasOf[i]+": Int32 = 0")
		}
	}
	for _, oneof := range caseOrder {
		decls = append(decls, "    public var "+caseOf[oneof]+": UInt32 = 0")
	}
	for i, field := range m.Fields {
		_, cKind, typ, wire, _ := swiftLayoutKindInfo(field)
		kind := [...]string{"int32", "int64", "uint32", "uint64", "bool", "float",
			"double", "string", "bytes", "enum", "message"}[cKind]
		if kind == "enum" {
			kind = "`enum`"
		}
		row := fmt.Sprintf("        Pbwire.FieldPlan(number: %d, kind: .%s, offset: %s, type: %d, wire: %d",
			field.Number, kind, swiftLayoutOffset(cls, field.Lower), typ, wire)
		elem := ""
		if field.Message != "" {
			// Arrays of child mirrors are strided by Swift's stride, which
			// can be wider than the child's size.
			elem = ", elem: MemoryLayout<" + swiftLayoutChild(f, field) + ">.stride"
		}
		if field.Repeated {
			row += ", repeated: true, count: " + swiftLayoutOffset(cls, countOf[i]) + elem
			if field.Packed {
				row += ", packed: true"
			}
		} else {
			if hasOf[i] != "" {
				row += ", has: " + swiftLayoutOffset(cls, hasOf[i])
			}
			row += elem
		}
		if !field.Repeated && field.Required {
			row += ", required: true"
		}
		if !field.Repeated && field.Oneof != "" {
			row += ", oneof: true, cs: " + swiftLayoutOffset(cls, caseOf[field.Oneof])
		}
		rows = append(rows, row+"),")
	}

	var children []string
	seen := map[string]bool{}
	for _, field := range m.Fields {
		if field.Message == "" {
			continue
		}
		child := swiftLayoutChild(f, field)
		if seen[child] {
			continue
		}
		seen[child] = true
		children = append(children, child+".self")
	}

	fmt.Fprintf(b, "\n/// %s as a flat native mirror: one C call reads and writes it in place.\n", m.Full)
	if len(children) > 0 {
		fmt.Fprintf(b, "/// The child mirrors must be registered before this one.\n")
	}
	b.WriteString("@frozen\n")
	fmt.Fprintf(b, "public struct %s: Pbwire.ReprMessage {\n", cls)
	for _, d := range decls {
		b.WriteString(d + "\n")
	}
	if len(inits) == 0 {
		b.WriteString("    public init() {}\n")
	} else {
		b.WriteString("    public init() {\n")
		for _, line := range inits {
			b.WriteString(line + "\n")
		}
		b.WriteString("    }\n")
	}
	b.WriteString("\n")
	fmt.Fprintf(b, "    public static let protoName = %q\n", m.Full)
	if len(children) > 0 {
		fmt.Fprintf(b, "    public static let children: [any Pbwire.ReprMessage.Type] = [%s]\n", strings.Join(children, ", "))
	}
	b.WriteString("    public static let fields: [Pbwire.FieldPlan] = [\n")
	for _, r := range rows {
		b.WriteString(r + "\n")
	}
	b.WriteString("    ]\n}\n\n")
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

package gen

// C# emitter: Google.Protobuf-shaped classes over the dynamic API.
//
// The surface mirrors protoc --csharp_out for the same .proto (PascalCase
// properties, FieldNumber consts, Has/Clear for optional, KindOneofCase for
// oneofs, nested Types classes, RepeatedField/MapField, Parser.ParseFrom,
// ToByteArray/WriteTo/CalculateSize, Clone/Equals), but every codec call goes
// through the binding's existing dynamic API, so the wire format stays in the
// C core. No IMessage: MergeFrom(CodedInputStream) would need a second wire
// reader, so Parser is this file's own small class and there is no
// Descriptor; reflection users keep the dynamic Schema.

import (
	"encoding/base64"
	"fmt"
	"strings"

	"pbwire-codegen/internal/model"

	"google.golang.org/protobuf/reflect/protoreflect"
)

func csharpFiles(f *model.File, opt Options) ([]File, error) {
	// Models and schema part ways: classes over the dynamic API in one file,
	// the embedded descriptor set plus its loader in the other (named after
	// the holder, already file-stem-scoped). model=false leaves the schema
	// file for dynamic-only users.
	holder := "PbwireFile_" + csPascal(base(f))
	// Same .proto package can span files (spectest: oneof1 + maptest share
	// namespace Spectest), so the holder and mini-parser carry the file stem.
	parser := "Parser_" + csPascal(base(f))
	byLocal := map[string]*csNode{}
	var walk func(n *csNode)
	walk = func(n *csNode) {
		byLocal[n.msg.Name] = n
		for _, c := range n.children {
			walk(c)
		}
	}
	for _, r := range csTree(f) {
		walk(r)
	}
	var files []File
	if opt.Model {
		var b strings.Builder
		b.WriteString(header(f, "Google.Protobuf-shaped classes over the pbwire runtime; the wire format stays in the C core."))
		b.WriteString("using System;\nusing System.IO;\nusing Google.Protobuf;\nusing Google.Protobuf.Collections;\nusing Pbwire;\n\n")
		fmt.Fprintf(&b, "namespace %s {\n\n", f.Prefix)
		b.WriteString("    /// <summary>Minimal parser: ParseFrom over the C core. Not pb::MessageParser.</summary>\n")
		b.WriteString("    public sealed class " + parser + "<T> where T : new() {\n")
		b.WriteString("        private readonly Action<T, byte[]> _merge;\n")
		b.WriteString("        public " + parser + "(Action<T, byte[]> merge) { _merge = merge; }\n")
		b.WriteString("        public T ParseFrom(byte[] data) { var m = new T(); _merge(m, data); return m; }\n")
		b.WriteString("        public T ParseFrom(ByteString data) => ParseFrom(data.ToByteArray());\n")
		b.WriteString("        public T ParseFrom(Stream input) { using var ms = new MemoryStream(); input.CopyTo(ms); return ParseFrom(ms.ToArray()); }\n")
		b.WriteString("    }\n\n")
		for i := range f.Enums {
			e := &f.Enums[i]
			if csEnumRoot(f, e) {
				writeCsharpEnum(&b, e, "    ")
			}
		}
		for _, r := range csTree(f) {
			writeCsharpMessage(&b, f, r, "    ", byLocal, holder, parser, !opt.NoLayout)
		}
		b.WriteString("}\n")
		files = append(files, File{Name: base(f) + ".cs", Content: b.String()})
	}
	var meta strings.Builder
	meta.WriteString(header(f, "Embedded descriptor set plus its loader: the schema side of the generated model."))
	meta.WriteString("using System;\nusing Pbwire;\n\n")
	fmt.Fprintf(&meta, "namespace %s {\n\n", f.Prefix)
	writeCsharpHolder(&meta, f, opt, holder)
	meta.WriteString("}\n")
	files = append(files, File{Name: holder + ".cs", Content: meta.String()})
	return files, nil
}

// writeCsharpHolder emits the schema holder: the embedded descriptor set plus
// an explicit loader. register=true keeps the eager static constructor (as
// before); otherwise Init (embedded) or UseSchema (external) must run first,
// loudly otherwise. registry adds the whole-file message list plus a preload
// that warms every type.
func writeCsharpHolder(b *strings.Builder, f *model.File, opt Options, holder string) {
	raw, err := base64.StdEncoding.DecodeString(opt.DescSetB64)
	if err != nil {
		raw = nil
	}
	// Chunked string.Concat like protoc's C# output: plain literals cannot
	// span lines.
	enc := base64.StdEncoding.EncodeToString(raw)
	var chunks []string
	for len(enc) > 0 {
		n := 60
		if n > len(enc) {
			n = len(enc)
		}
		chunks = append(chunks, fmt.Sprintf("%q", enc[:n]))
		enc = enc[n:]
	}
	b.WriteString("    public static class " + holder + " {\n")
	b.WriteString("        private static volatile Schema? _schema;\n")
	b.WriteString("        private static readonly object _lock = new object();\n\n")
	if opt.Register {
		fmt.Fprintf(b, "        static %s() { Init(); }\n\n", holder)
	}
	b.WriteString("        /// <summary>Loads the embedded set. With register=true the static constructor already ran it.</summary>\n")
	b.WriteString("        public static void Init() {\n")
	b.WriteString("            if (_schema == null) {\n")
	b.WriteString("                lock (_lock) {\n")
	b.WriteString("                    if (_schema == null) {\n")
	b.WriteString("                        _schema = Schema.FromBytes(Convert.FromBase64String(DescBase64()));\n")
	b.WriteString("                    }\n")
	b.WriteString("                }\n")
	b.WriteString("            }\n")
	b.WriteString("        }\n\n")
	b.WriteString("        /// <summary>Registers an externally loaded schema: models encode/decode through it instead of the embedded set. The caller owns it; null restores the embedded set (call Init to reload).</summary>\n")
	b.WriteString("        public static void UseSchema(Schema? s) {\n")
	b.WriteString("            lock (_lock) { _schema = s; }\n")
	b.WriteString("        }\n\n")
	b.WriteString("        internal static Schema Schema => _schema ?? throw new InvalidOperationException(\"call Init or UseSchema first (generated with register=false)\");\n\n")
	b.WriteString("        private static string DescBase64() {\n")
	b.WriteString("            return string.Concat(\n            " + strings.Join(chunks, ",\n            ") + ");\n")
	b.WriteString("        }\n")
	if !opt.NoLayout {
		var specs []string
		var walk func(n *csNode)
		walk = func(n *csNode) {
			for _, c := range n.children {
				walk(c)
			}
			if csLayoutOK(n.msg) {
				specs = append(specs, strings.Join(csCur(n), ".")+"."+n.self+"Layout.Spec")
			}
		}
		for _, r := range csTree(f) {
			walk(r)
		}
		if len(specs) > 0 {
			b.WriteString("\n        /// <summary>The embedded descriptor set, for the native flat-image reader.</summary>\n")
			b.WriteString("        public static byte[] DescriptorSetBytes() { return Convert.FromBase64String(DescBase64()); }\n")
			b.WriteString("\n        private static global::Pbwire.LayoutSource? _layouts;\n")
			b.WriteString("        /// <summary>Every flat layout in this file: the native reader resolves sub-messages here.</summary>\n")
			b.WriteString("        /// <remarks>Built on first use: a static field would reach a layout's Type (and so this holder) while this type is still initializing.</remarks>\n")
			b.WriteString("        public static global::Pbwire.LayoutSource Layouts => _layouts ??= new global::Pbwire.LayoutSource(\n")
			fmt.Fprintf(b, "            %q, DescriptorSetBytes,\n            new global::Pbwire.LayoutType[] {\n", holder)
			for _, spec := range specs {
				fmt.Fprintf(b, "                %s,\n", spec)
			}
			b.WriteString("            });\n")
		}
	}
	if opt.Registry {
		b.WriteString("\n        /// <summary>Every message in this file, for whole-schema pre-registration.</summary>\n")
		b.WriteString("        public static readonly string[] MessageNames = {\n")
		for _, m := range f.Messages {
			fmt.Fprintf(b, "            %q,\n", m.Full)
		}
		b.WriteString("        };\n\n")
		b.WriteString("        /// <summary>Warms every message type, so later calls pay no lookup.</summary>\n")
		b.WriteString("        public static void Preload() {\n")
		b.WriteString("            var s = Schema;\n")
		b.WriteString("            foreach (var n in MessageNames) {\n")
		b.WriteString("                s.Message(n);\n")
		b.WriteString("            }\n")
		b.WriteString("        }\n")
	}
	b.WriteString("    }\n\n")
}

// csNode is a message with its nested types attached (same tree shape as the
// Rust emitter; nesting recovered from qualified names).
type csNode struct {
	msg      *model.Message
	self     string
	parent   *csNode
	children []*csNode
	enums    []*model.Enum
}

func csTree(f *model.File) []*csNode {
	byFull := map[string]*csNode{}
	var all []*csNode
	for i := range f.Messages {
		m := &f.Messages[i]
		segs := strings.Split(m.Name, "_")
		all = append(all, &csNode{msg: m, self: segs[len(segs)-1]})
		byFull[m.Full] = all[len(all)-1]
	}
	var roots []*csNode
	for _, n := range all {
		var best *csNode
		bestLen := -1
		for pf, c := range byFull {
			rest, ok := strings.CutPrefix(n.msg.Full, pf+".")
			if ok && !strings.Contains(rest, ".") && len(pf) > bestLen {
				best, bestLen = c, len(pf)
			}
		}
		n.parent = best
		if best != nil {
			best.children = append(best.children, n)
		} else {
			roots = append(roots, n)
		}
	}
	for i := range f.Enums {
		e := &f.Enums[i]
		var best *csNode
		bestLen := -1
		for pf, c := range byFull {
			rest, ok := strings.CutPrefix(e.Full, pf+".")
			if ok && !strings.Contains(rest, ".") && len(pf) > bestLen {
				best, bestLen = c, len(pf)
			}
		}
		if best != nil {
			best.enums = append(best.enums, e)
		}
	}
	return roots
}

func csEnumRoot(f *model.File, e *model.Enum) bool {
	for _, m := range f.Messages {
		if rest, ok := strings.CutPrefix(e.Full, m.Full+"."); ok && !strings.Contains(rest, ".") {
			return false
		}
	}
	return true
}

// csForeign names a type from another file: its namespace plus the nesting
// path the imported file's own generation renders (top-level classes carry
// no prefix; nested ones hang under Types, like csPath with empty cur).
func csForeign(imp model.Import, local string) string {
	segs := strings.Split(local, "_")
	var parts []string
	for i, s := range segs {
		if i > 0 {
			parts = append(parts, "Types")
		}
		parts = append(parts, s)
	}
	return imp.Prefix + "." + strings.Join(parts, ".")
}

// csFieldPath renders a field's message type: the foreign name when the
// type lives in an import, the nesting-relative path otherwise.
func csFieldPath(f *model.File, field model.Field, cur []string, byLocal map[string]*csNode) string {
	if field.Import != "" {
		if imp := findImport(f, field.Import); imp != nil {
			return csForeign(*imp, field.Message)
		}
	}
	return csPath(field.Message, cur, byLocal)
}

// csPath renders a local message name relative to the current nesting level.
// cur is the chain of enclosing classes; nested types live under Types.
func csPath(local string, cur []string, byLocal map[string]*csNode) string {
	n, ok := byLocal[local]
	if !ok {
		return strings.ReplaceAll(local, "_", "")
	}
	var segs []string
	for p := n; p != nil; p = p.parent {
		segs = append([]string{p.self}, segs...)
	}
	var parts []string
	for i, s := range segs {
		if i > 0 {
			parts = append(parts, "Types")
		}
		parts = append(parts, s)
	}
	i := 0
	for i < len(parts) && i < len(cur) && parts[i] == cur[i] {
		i++
	}
	rest := parts[i:]
	if len(rest) == 0 {
		// Self-reference (a recursive field names its own class): C#
		// resolves the bare name inside the class body.
		rest = parts[len(parts)-1:]
	}
	return strings.Join(rest, ".")
}

func csScalar(field model.Field, cur []string, byLocal map[string]*csNode) string {
	switch field.Kind {
	case protoreflect.Int32Kind, protoreflect.Sint32Kind, protoreflect.Sfixed32Kind:
		return "int"
	case protoreflect.Uint32Kind, protoreflect.Fixed32Kind:
		return "uint"
	case protoreflect.Int64Kind, protoreflect.Sint64Kind, protoreflect.Sfixed64Kind:
		return "long"
	case protoreflect.Uint64Kind, protoreflect.Fixed64Kind:
		return "ulong"
	case protoreflect.FloatKind:
		return "float"
	case protoreflect.DoubleKind:
		return "double"
	case protoreflect.BoolKind:
		return "bool"
	case protoreflect.StringKind:
		return "string"
	case protoreflect.BytesKind:
		return "ByteString"
	case protoreflect.EnumKind:
		if field.Enum == "" {
			// External enum (another file): keep the wire number.
			return "int"
		}
		return csEnumPath(field.Enum, cur, byLocal)
	default:
		return "long"
	}
}

// csEnumPath resolves a local enum name the way nested classes do:
// TopLevel or Parent.Types.Name, relative to cur.
func csEnumParent(byLocal map[string]*csNode, local string) *csNode {
	for _, n := range byLocal {
		for _, e := range n.enums {
			if e.Name == local {
				return n
			}
		}
	}
	return nil
}

func csEnumPath(local string, cur []string, byLocal map[string]*csNode) string {
	p := csEnumParent(byLocal, local)
	var parts []string
	if p == nil {
		parts = []string{csEnumName(local)}
	} else {
		for q := p; q != nil; q = q.parent {
			parts = append([]string{q.self}, parts...)
		}
		var dotted []string
		for i, s := range parts {
			if i > 0 {
				dotted = append(dotted, "Types")
			}
			dotted = append(dotted, s)
		}
		dotted = append(dotted, "Types", csEnumName(local))
		parts = dotted
	}
	i := 0
	for i < len(parts) && i < len(cur) && parts[i] == cur[i] {
		i++
	}
	rest := parts[i:]
	if len(rest) == 0 {
		// Self-reference (a recursive field names its own class): C#
		// resolves the bare name inside the class body.
		rest = parts[len(parts)-1:]
	}
	return strings.Join(rest, ".")
}

func csEnumName(local string) string {
	segs := strings.Split(local, "_")
	return csPascal(segs[len(segs)-1])
}

// csVariant is protoc's C# enum naming: strip the ENUM_NAME_ prefix, PascalCase.
func csVariant(enumLocal, value string) string {
	segs := strings.Split(enumLocal, "_")
	prefix := ""
	for i, s := range segs {
		if i > 0 {
			prefix += "_"
		}
		prefix += strings.ToUpper(s)
	}
	rest := strings.TrimPrefix(value, prefix+"_")
	parts := strings.Split(strings.ToLower(rest), "_")
	for i, p := range parts {
		if p != "" {
			parts[i] = strings.ToUpper(p[:1]) + p[1:]
		}
	}
	return strings.Join(parts, "")
}

// csProp is the PascalCase property name (model.Pascal already is, except it
// keeps leading underscores which C# drops).
func csProp(field model.Field) string {
	return strings.TrimPrefix(field.Pascal, "_")
}

// csPascal camelizes a proto name the way protoc's C# generator does:
// snake_case to UpperCamelCase.
func csPascal(s string) string {
	parts := strings.Split(s, "_")
	for i, p := range parts {
		if p != "" {
			parts[i] = strings.ToUpper(p[:1]) + p[1:]
		}
	}
	return strings.Join(parts, "")
}

// csSlot is one row of the emitted flat image: a field's storage offset, its
// width, the presence/count/oneof words the C layout codec reads, and the wire
// facts that let the codec skip its schema lookup. Reference-shaped fields
// (string, bytes, submessage, repeated) hold a pointer into the call arena, so
// the image is contiguous while the values it points at are not.
type csSlot struct {
	name     string
	offset   int
	size     int
	presence int // presence bit index for the gated Layout table (-1 = none)
	ref      bool
	kind     int // pbw_struct_kind_t
	typ      int // pbw_type
	wire     int
	repeated bool
	hasOff   int // 4-byte presence word (0 = none)
	countOff int // 8-byte repeated count (0 = singular)
	caseOff  int // 4-byte oneof case word (0 = none)
	oneof    bool
	packed   bool
	required bool
	child    string // MESSAGE: the child's proto full name
}

// csLayoutKindInfo maps one field kind to the C codec's kind, its pbw_type,
// wire type and slot width. The values are pbw_struct_kind_t / pbw_type from
// include/pbwire.h, the same ones bindings/c/runtime/pbw_struct.c expects.
func csLayoutKindInfo(field model.Field) (cKind, typ, wire, size int, ok bool) {
	switch field.Kind {
	case protoreflect.Int32Kind:
		return 0, 5, 0, 4, true
	case protoreflect.Sint32Kind:
		return 0, 17, 0, 4, true
	case protoreflect.Sfixed32Kind:
		return 0, 15, 5, 4, true
	case protoreflect.Int64Kind:
		return 1, 3, 0, 8, true
	case protoreflect.Sint64Kind:
		return 1, 18, 0, 8, true
	case protoreflect.Sfixed64Kind:
		return 1, 16, 1, 8, true
	case protoreflect.Uint32Kind:
		return 2, 13, 0, 4, true
	case protoreflect.Fixed32Kind:
		return 2, 7, 5, 4, true
	case protoreflect.Uint64Kind:
		return 3, 4, 0, 8, true
	case protoreflect.Fixed64Kind:
		return 3, 6, 1, 8, true
	case protoreflect.BoolKind:
		return 4, 8, 0, 1, true
	case protoreflect.FloatKind:
		return 5, 2, 5, 4, true
	case protoreflect.DoubleKind:
		return 6, 1, 1, 8, true
	case protoreflect.StringKind:
		return 7, 9, 2, 8, true
	case protoreflect.BytesKind:
		return 8, 12, 2, 16, true
	case protoreflect.EnumKind:
		return 9, 14, 0, 4, true
	case protoreflect.MessageKind:
		return 10, 11, 2, 8, true
	case protoreflect.GroupKind:
		return 10, 10, 3, 8, true
	}
	return 0, 0, 0, 0, false
}

// csSlotAlign is the natural alignment of a slot.
func csSlotAlign(size int) int {
	if size >= 8 {
		return 8
	}
	return size
}

// csLayoutOK reports whether a message's image is expressible: every field has
// a known kind and a slot. A message that is not (a skipped map-entry or an
// unresolved type) gets no layout and no image reader instead of a wrong one.
func csLayoutOK(m *model.Message) bool {
	for _, field := range m.Fields {
		if field.Skip != "" {
			return false
		}
		if _, _, _, _, ok := csLayoutKindInfo(field); !ok {
			return false
		}
	}
	return true
}

// csSlots packs a message into the flat image: scalar slots at natural
// alignment in declaration order, then one pointer slot per reference field
// (16 bytes for a `bytes` -> pbw_bytes_t), then the auxiliary words in
// declaration order: a 4-byte presence word per field the codec needs one for,
// an 8-byte count per repeated field and one 4-byte case word per oneof.
func csSlots(m *model.Message) (slots []csSlot, total, presenceBits int) {
	// Presence bits follow declaration order among the proto3 optionals, the
	// same index csOptBit gives the model's has-bits, wherever the slot lands.
	hasBit := map[string]int{}
	bit := 0
	for _, field := range m.Fields {
		if field.Skip != "" || !field.NeedsHas || field.Required || field.Oneof != "" {
			continue
		}
		hasBit[field.Name] = bit
		bit++
	}
	pos := 0
	alloc := func(size int) int {
		if align := csSlotAlign(size); pos%align != 0 {
			pos += align - pos%align
		}
		at := pos
		pos += size
		return at
	}
	byName := map[string]int{}
	// Scalars first (natural alignment, declaration order), then pointer slots.
	for pass := 0; pass < 2; pass++ {
		for _, field := range m.Fields {
			if field.Skip != "" {
				continue
			}
			cKind, typ, wire, size, ok := csLayoutKindInfo(field)
			if !ok {
				continue
			}
			ref := field.Repeated || field.Message != "" ||
				field.Kind == protoreflect.StringKind || field.Kind == protoreflect.BytesKind
			if (pass == 0) == ref {
				continue
			}
			if field.Repeated {
				size = 8 // pointer to the element array; the count is a separate word
			}
			presence := -1
			if b, ok := hasBit[field.Name]; ok {
				presence = b
			}
			slot := csSlot{
				name: field.Name, offset: alloc(size), size: size, presence: presence, ref: ref,
				kind: cKind, typ: typ, wire: wire, repeated: field.Repeated,
				packed: field.Packed && field.Repeated, required: field.Required,
			}
			if field.Message != "" {
				slot.child = csChildName(field)
			}
			byName[field.Name] = len(slots)
			slots = append(slots, slot)
		}
	}
	// Auxiliary words. A pointer-shaped value is absent when NULL, a repeated
	// field uses its count and a oneof uses its case word, so only the other
	// singular scalars need a presence word of their own.
	cases := map[string]int{}
	for _, field := range m.Fields {
		i, ok := byName[field.Name]
		if !ok {
			continue
		}
		slot := &slots[i]
		if field.Oneof != "" {
			at, seen := cases[field.Oneof]
			if !seen {
				at = alloc(4)
				cases[field.Oneof] = at
			}
			slot.oneof = true
			slot.caseOff = at
		}
		if field.Repeated {
			slot.countOff = alloc(8)
			continue
		}
		if field.Oneof != "" {
			continue
		}
		if (field.NeedsHas || field.Required) &&
			field.Kind != protoreflect.StringKind &&
			field.Kind != protoreflect.BytesKind &&
			field.Message == "" {
			slot.hasOff = alloc(4)
		}
	}
	if rem := pos % 8; rem != 0 {
		pos += 8 - rem
	}
	return slots, pos, bit
}

// csChildName is the registry key of a message-typed field's layout: the proto
// full name, so the C side resolves the child's rows and image size by name.
func csChildName(field model.Field) string {
	if field.Full != "" {
		return field.Full
	}
	return field.Message
}

// writeCsharpLayout emits the flat image the native reader walks: the gated
// offset table, the row facts the C layout codec needs, and the two image
// converters (`WriteImage` / `ReadImage`) that copy the native object into and
// out of one contiguous unmanaged block. A managed object with `string` /
// `List<T>` members cannot be pinned, so the image is the pinned
// representation and filling it is this arm's honest cost.
func writeCsharpLayout(b *strings.Builder, f *model.File, n *csNode, indent, holder string) {
	m := n.msg
	if !csLayoutOK(m) {
		return
	}
	slots, total, bits := csSlots(m)
	fmt.Fprintf(b, "%s/// <summary>Bytes of one flat image: the native object's pinned representation.</summary>\n", indent)
	fmt.Fprintf(b, "%spublic const int ImageBytes = %d;\n", indent, total)
	writeCsharpImage(b, f, n, indent, slots)
	fmt.Fprintf(b, "%s/// <summary>Flat slot layout: scalar offsets, pointer slots for the reference-typed fields, one presence bit per proto3 optional. Read by the native-object entry, gated by Pbwire.Layout.Verify.</summary>\n", indent)
	fmt.Fprintf(b, "%spublic static class %sLayout {\n", indent, n.self)
	in := indent + "    "
	fmt.Fprintf(b, "%s/// <summary>The message type the table describes.</summary>\n", in)
	fmt.Fprintf(b, "%spublic static readonly global::Pbwire.MessageType Type = %s.Schema.Message(%q);\n", in, holder, m.Full)
	fmt.Fprintf(b, "%s/// <summary>One row per field, in slot order.</summary>\n", in)
	if len(slots) == 0 {
		fmt.Fprintf(b, "%spublic static readonly global::Pbwire.Layout Table = new global::Pbwire.Layout(%q, %d, %d);\n", in, m.Full, total, bits)
	} else {
		fmt.Fprintf(b, "%spublic static readonly global::Pbwire.Layout Table = new global::Pbwire.Layout(%q, %d, %d)\n", in, m.Full, total, bits)
		for i, slot := range slots {
			call := fmt.Sprintf("FieldAt(%q, %d, %d)", slot.name, slot.offset, slot.size)
			switch {
			case slot.presence >= 0:
				call = fmt.Sprintf("Has(%q, %d, %d)", slot.name, slot.presence, slot.offset)
			case slot.ref:
				call = fmt.Sprintf("Ref(%q, %d, %d)", slot.name, slot.offset, slot.size)
			}
			end := ""
			if i == len(slots)-1 {
				end = ";"
			}
			fmt.Fprintf(b, "%s    .%s%s\n", in, call, end)
		}
	}
	fmt.Fprintf(b, "%s/// <summary>One row per field, with the presence/count/oneof words and wire facts the C layout codec reads.</summary>\n", in)
	fmt.Fprintf(b, "%spublic static readonly global::Pbwire.LayoutRow[] Rows = {\n", in)
	for _, slot := range slots {
		fmt.Fprintf(b, "%s    new(%d, global::Pbwire.FlatKind.%s, %d, %d", in, fieldNumber(m, slot.name), csKindName(slot.kind), slot.offset, slot.typ)
		if slot.wire != 0 {
			fmt.Fprintf(b, ", Wire: %d", slot.wire)
		}
		if slot.repeated {
			b.WriteString(", Repeated: true")
		}
		if slot.hasOff != 0 {
			fmt.Fprintf(b, ", Has: %d", slot.hasOff)
		}
		if slot.countOff != 0 {
			fmt.Fprintf(b, ", Count: %d", slot.countOff)
		}
		if slot.packed {
			b.WriteString(", Packed: true")
		}
		if slot.required {
			b.WriteString(", Required: true")
		}
		if slot.oneof {
			fmt.Fprintf(b, ", Oneof: true, Case: %d", slot.caseOff)
		}
		if slot.child != "" {
			fmt.Fprintf(b, ", Child: %q", slot.child)
		}
		b.WriteString("),\n")
	}
	fmt.Fprintf(b, "%s};\n", in)
	fmt.Fprintf(b, "%s/// <summary>This message's image, as the file's layout source sees it.</summary>\n", in)
	fmt.Fprintf(b, "%spublic static readonly global::Pbwire.LayoutType Spec = new(%q, ImageBytes, Rows);\n", in, m.Full)
	fmt.Fprintf(b, "%sprivate static global::Pbwire.LayoutPlan? _plan;\n", in)
	fmt.Fprintf(b, "%s/// <summary>The compiled reader, cached after the first call (no per-op lookup).</summary>\n", in)
	fmt.Fprintf(b, "%sprivate static global::Pbwire.LayoutPlan Plan => _plan ??= global::Pbwire.LayoutPlan.Compile(%s.Layouts, Spec);\n", in, holder)
	fmt.Fprintf(b, "%s/// <summary>One call per op: native object in, wire bytes out (one native call over the flat image).</summary>\n", in)
	fmt.Fprintf(b, "%spublic static unsafe byte[] Marshal(%s native)\n%s{\n", in, n.self, in)
	fmt.Fprintf(b, "%s    using var call = global::Pbwire.ImageCall.Rent(ImageBytes);\n", in)
	fmt.Fprintf(b, "%s    native.WriteImage(call.Image, call.Arena);\n", in)
	fmt.Fprintf(b, "%s    return Plan.Encode(call.Image);\n%s}\n", in, in)
	fmt.Fprintf(b, "%s/// <summary>One call per op: wire bytes in, native object out. The block carries the decoded image's strings, bytes and sub-images.</summary>\n", in)
	fmt.Fprintf(b, "%spublic static unsafe %s Unmarshal(byte[] wire)\n%s{\n", in, n.self, in)
	fmt.Fprintf(b, "%s    using var call = global::Pbwire.ImageCall.Rent(ImageBytes, wire.Length * 4 + 65536);\n", in)
	fmt.Fprintf(b, "%s    Plan.Decode(wire, call.Image, call.DecodeArena, call.DecodeArenaBytes);\n", in)
	fmt.Fprintf(b, "%s    var native = new %s();\n", in, n.self)
	fmt.Fprintf(b, "%s    native.ReadImage(call.Image);\n", in)
	fmt.Fprintf(b, "%s    return native;\n%s}\n", in, in)
	fmt.Fprintf(b, "%s}\n\n", indent)
}

// fieldNumber finds a field's proto number by its lower-case name.
func fieldNumber(m *model.Message, name string) int32 {
	for _, field := range m.Fields {
		if field.Name == name {
			return field.Number
		}
	}
	return 0
}

// csKindName is the FlatKind member for a pbw_struct_kind_t value.
func csKindName(kind int) string {
	switch kind {
	case 0:
		return "Int32"
	case 1:
		return "Int64"
	case 2:
		return "Uint32"
	case 3:
		return "Uint64"
	case 4:
		return "Bool"
	case 5:
		return "Float"
	case 6:
		return "Double"
	case 7:
		return "String"
	case 8:
		return "Bytes"
	case 9:
		return "Enum"
	default:
		return "Message"
	}
}

// csImageScalar is the C# storage type of a scalar slot (the type the image
// stores and the model property holds, modulo enums).
func csImageScalar(field model.Field) string {
	switch field.Kind {
	case protoreflect.BoolKind:
		return "bool"
	case protoreflect.Uint32Kind, protoreflect.Fixed32Kind:
		return "uint"
	case protoreflect.Int64Kind, protoreflect.Sint64Kind, protoreflect.Sfixed64Kind:
		return "long"
	case protoreflect.Uint64Kind, protoreflect.Fixed64Kind:
		return "ulong"
	case protoreflect.FloatKind:
		return "float"
	case protoreflect.DoubleKind:
		return "double"
	default:
		return "int"
	}
}

// csImageWidth is the byte width of one element in a repeated array.
func csImageWidth(field model.Field) int {
	switch field.Kind {
	case protoreflect.BoolKind:
		return 1
	case protoreflect.Uint64Kind, protoreflect.Fixed64Kind,
		protoreflect.Int64Kind, protoreflect.Sint64Kind, protoreflect.Sfixed64Kind,
		protoreflect.DoubleKind, protoreflect.StringKind:
		return 8
	case protoreflect.BytesKind:
		return 16
	case protoreflect.MessageKind, protoreflect.GroupKind:
		return 0 // the child's ImageBytes
	default:
		return 4
	}
}

// csWriteScalar stores one scalar expression at img+off in the image's width.
func csWriteScalar(b *strings.Builder, indent, base, expr string, field model.Field) {
	switch field.Kind {
	case protoreflect.BoolKind:
		fmt.Fprintf(b, "%s*(byte*)(%s) = (byte)(%s ? 1 : 0);\n", indent, base, expr)
	case protoreflect.Uint32Kind, protoreflect.Fixed32Kind:
		fmt.Fprintf(b, "%s*(uint*)(%s) = (uint)(%s);\n", indent, base, expr)
	case protoreflect.Int64Kind, protoreflect.Sint64Kind, protoreflect.Sfixed64Kind:
		fmt.Fprintf(b, "%s*(long*)(%s) = (long)(%s);\n", indent, base, expr)
	case protoreflect.Uint64Kind, protoreflect.Fixed64Kind:
		fmt.Fprintf(b, "%s*(ulong*)(%s) = (ulong)(%s);\n", indent, base, expr)
	case protoreflect.FloatKind:
		fmt.Fprintf(b, "%s*(float*)(%s) = (float)(%s);\n", indent, base, expr)
	case protoreflect.DoubleKind:
		fmt.Fprintf(b, "%s*(double*)(%s) = (double)(%s);\n", indent, base, expr)
	default: // int32 family and enums
		fmt.Fprintf(b, "%s*(int*)(%s) = (int)(%s);\n", indent, base, expr)
	}
}

// csReadScalar is the C# assignment that copies one scalar slot back.
func csReadScalar(field model.Field, base string) string {
	switch field.Kind {
	case protoreflect.BoolKind:
		return fmt.Sprintf("*(byte*)(%s) != 0", base)
	case protoreflect.Uint32Kind, protoreflect.Fixed32Kind:
		return fmt.Sprintf("*(uint*)(%s)", base)
	case protoreflect.Int64Kind, protoreflect.Sint64Kind, protoreflect.Sfixed64Kind:
		return fmt.Sprintf("*(long*)(%s)", base)
	case protoreflect.Uint64Kind, protoreflect.Fixed64Kind:
		return fmt.Sprintf("*(ulong*)(%s)", base)
	case protoreflect.FloatKind:
		return fmt.Sprintf("*(float*)(%s)", base)
	case protoreflect.DoubleKind:
		return fmt.Sprintf("*(double*)(%s)", base)
	default: // int32 family and enums (the property cast is applied by the caller)
		return fmt.Sprintf("*(int*)(%s)", base)
	}
}

// writeCsharpImage emits the native object's image converters: `WriteImage`
// copies every field into the caller's block (strings/bytes/sub-images into
// the call arena), `ReadImage` copies a decoded image back into the object and
// `NewImage` allocates a zeroed sub-image for a parent.
func writeCsharpImage(b *strings.Builder, f *model.File, n *csNode, indent string, slots []csSlot) {
	m := n.msg
	cur := csCur(n)
	byName := map[string]*csNode{}
	for _, r := range csTree(f) {
		var walk func(c *csNode)
		walk = func(c *csNode) {
			byName[c.msg.Name] = c
			for _, k := range c.children {
				walk(k)
			}
		}
		walk(r)
	}
	slotOf := map[string]csSlot{}
	for _, slot := range slots {
		slotOf[slot.name] = slot
	}
	in := indent + "    "
	fmt.Fprintf(b, "%s/// <summary>Copies this native object into a zeroed flat image; strings, bytes and sub-images come from <paramref name=\"arena\"/>.</summary>\n", indent)
	fmt.Fprintf(b, "%spublic unsafe void WriteImage(byte* img, global::Pbwire.ImageArena arena) {\n", indent)
	for _, field := range m.Fields {
		slot := slotOf[field.Name]
		prop := csProp(field)
		if slot.oneof {
			// Written unconditionally: the codec reads only the member the case
			// word selects, and a stale value beyond the case is never wire content.
		} else if slot.hasOff != 0 {
			cond := "1"
			if field.NeedsHas && !field.Required {
				cond = "Has" + csProp(field) + " ? 1 : 0"
			}
			fmt.Fprintf(b, "%s    *(int*)(img + %d) = %s;\n", indent, slot.hasOff, cond)
		}
		off := fmt.Sprintf("%d", slot.offset)
		switch {
		case field.Repeated:
			width := fmt.Sprintf("%d", csImageWidth(field))
			if field.Message != "" {
				width = csFieldPath(f, field, cur, byName) + ".ImageBytes"
			}
			fmt.Fprintf(b, "%s    {\n%s        var n = %s.Count;\n", indent, indent, prop)
			fmt.Fprintf(b, "%s        if (n != 0) {\n%s            var p = (byte*)arena.Alloc(n * %s);\n", indent, indent, width)
			fmt.Fprintf(b, "%s            for (var i = 0; i < n; i++) {\n", indent)
			writeCsharpElemWrite(b, indent+"                ", field, f, cur, byName)
			fmt.Fprintf(b, "%s            }\n", indent)
			fmt.Fprintf(b, "%s            *(IntPtr*)(img + %s) = (IntPtr)p;\n%s        }\n", indent, off, indent)
			fmt.Fprintf(b, "%s        *(nuint*)(img + %d) = (nuint)n;\n%s    }\n", indent, slot.countOff, indent)
		case field.Message != "":
			fmt.Fprintf(b, "%s    *(IntPtr*)(img + %s) = %s == null ? IntPtr.Zero : (IntPtr)%s.NewImage(arena);\n", indent, off, prop, prop)
		case field.Kind == protoreflect.StringKind:
			fmt.Fprintf(b, "%s    *(IntPtr*)(img + %s) = %s.Length == 0 ? IntPtr.Zero : arena.Utf8(%s);\n", indent, off, prop, prop)
		case field.Kind == protoreflect.BytesKind:
			fmt.Fprintf(b, "%s    {\n%s        var b = %s.Span;\n", indent, indent, prop)
			fmt.Fprintf(b, "%s        var p = b.Length == 0 ? IntPtr.Zero : arena.Copy(b);\n", indent)
			fmt.Fprintf(b, "%s        *(IntPtr*)(img + %s) = p;\n%s        *(nuint*)(img + %d) = (nuint)b.Length;\n%s    }\n", indent, off, indent, slot.offset+8, indent)
		default:
			csWriteScalar(b, indent+"    ", "img + "+off, prop, field)
		}
		if slot.oneof {
			fmt.Fprintf(b, "%s    *(uint*)(img + %d) = (uint)%sCase;\n", indent, slot.caseOff, csPascal(field.Oneof))
		}
	}
	fmt.Fprintf(b, "%s}\n\n", indent)

	fmt.Fprintf(b, "%s/// <summary>Allocates a zeroed sub-image from the call arena and fills it.</summary>\n", indent)
	fmt.Fprintf(b, "%spublic unsafe byte* NewImage(global::Pbwire.ImageArena arena) {\n", indent)
	fmt.Fprintf(b, "%svar p = (byte*)arena.Alloc(ImageBytes);\n", in)
	fmt.Fprintf(b, "%sglobal::Pbwire.ImageArena.Clear(p, ImageBytes);\n", in)
	fmt.Fprintf(b, "%sWriteImage(p, arena);\n", in)
	fmt.Fprintf(b, "%sreturn p;\n%s}\n\n", in, in)

	fmt.Fprintf(b, "%s/// <summary>Copies a decoded flat image into this native object; the image's pointers must still be alive.</summary>\n", indent)
	fmt.Fprintf(b, "%spublic unsafe void ReadImage(byte* img) {\n", indent)
	oneofDone := map[string]bool{}
	for _, field := range m.Fields {
		slot := slotOf[field.Name]
		prop := csProp(field)
		if slot.oneof {
			name := field.Oneof
			if !oneofDone[name] {
				oneofDone[name] = true
				writeCsharpOneofRead(b, indent+"    ", m, name, slotOf, f, cur, byName)
			}
			continue
		}
		inner := indent + "    "
		guard := ""
		if slot.hasOff != 0 && field.NeedsHas && !field.Required {
			guard = fmt.Sprintf("if (*(int*)(img + %d) != 0) ", slot.hasOff)
		}
		off := fmt.Sprintf("%d", slot.offset)
		switch {
		case field.Repeated:
			fmt.Fprintf(b, "%s%s.Clear();\n", inner, prop)
			fmt.Fprintf(b, "%svar n%s = (int)(*(nuint*)(img + %d));\n", inner, csProp(field), slot.countOff)
			fmt.Fprintf(b, "%sif (n%s != 0) {\n%s    var p = *(byte**)(img + %s);\n", inner, csProp(field), inner, off)
			writeCsharpElemRead(b, inner+"    ", field, f, cur, byName)
			fmt.Fprintf(b, "%s}\n", inner)
		case field.Message != "":
			child := csFieldPath(f, field, cur, byName)
			fmt.Fprintf(b, "%s%s{\n%s    var p = *(byte**)(img + %s);\n", inner, guard, inner, off)
			fmt.Fprintf(b, "%s    if (p != null) { var c = new %s(); c.ReadImage(p); %s = c; }\n%s}\n", inner, child, prop, inner)
		case field.Kind == protoreflect.StringKind:
			fmt.Fprintf(b, "%s%s{\n%s    var p = *(IntPtr*)(img + %s);\n", inner, guard, inner, off)
			empty := "\"\""
			if field.NeedsHas || field.Required {
				empty = "null!"
			}
			fmt.Fprintf(b, "%s    %s = p == IntPtr.Zero ? %s : (global::System.Runtime.InteropServices.Marshal.PtrToStringUTF8(p) ?? \"\");\n%s}\n", inner, prop, empty, inner)
		case field.Kind == protoreflect.BytesKind:
			fmt.Fprintf(b, "%s%s{\n%s    var p = *(byte**)(img + %s);\n", inner, guard, inner, off)
			fmt.Fprintf(b, "%s    var len = (int)(*(nuint*)(img + %d));\n", inner, slot.offset+8)
			fmt.Fprintf(b, "%s    if (p != null) { %s = global::Google.Protobuf.ByteString.CopyFrom(new ReadOnlySpan<byte>(p, len)); }\n%s}\n", inner, prop, inner)
		default:
			expr := csReadScalar(field, "img + "+off)
			if field.Kind == protoreflect.EnumKind && field.Enum != "" {
				expr = "(" + csScalar(field, cur, byName) + ")(" + expr + ")"
			}
			fmt.Fprintf(b, "%s%s%s = %s;\n", inner, guard, prop, expr)
		}
	}
	fmt.Fprintf(b, "%s    GC.KeepAlive(this);\n", indent)
	fmt.Fprintf(b, "%s}\n\n", indent)
}

// writeCsharpElemWrite fills one repeated element of the array at p.
func writeCsharpElemWrite(b *strings.Builder, indent string, field model.Field, f *model.File, cur []string, byLocal map[string]*csNode) {
	prop := csProp(field)
	at := "p + i * " + fmt.Sprintf("%d", csImageWidth(field))
	switch {
	case field.Message != "":
		child := csFieldPath(f, field, cur, byLocal)
		at = "p + i * " + child + ".ImageBytes"
		fmt.Fprintf(b, "%s%s[i].WriteImage(%s, arena);\n", indent, prop, at)
	case field.Kind == protoreflect.StringKind:
		fmt.Fprintf(b, "%s((IntPtr*)p)[i] = arena.Utf8(%s[i]);\n", indent, prop)
	case field.Kind == protoreflect.BytesKind:
		fmt.Fprintf(b, "%s{\n%s    var b = %s[i].Span;\n", indent, indent, prop)
		fmt.Fprintf(b, "%s    var bp = (byte*)(p + i * 16);\n", indent)
		fmt.Fprintf(b, "%s    *(IntPtr*)bp = b.Length == 0 ? IntPtr.Zero : arena.Copy(b);\n", indent)
		fmt.Fprintf(b, "%s    *(nuint*)(bp + 8) = (nuint)b.Length;\n%s}\n", indent, indent)
	default:
		csWriteScalar(b, indent, at, fmt.Sprintf("%s[i]", prop), field)
	}
}

// writeCsharpElemRead reads one repeated element of the array at p.
func writeCsharpElemRead(b *strings.Builder, indent string, field model.Field, f *model.File, cur []string, byLocal map[string]*csNode) {
	prop := csProp(field)
	at := "p + i * " + fmt.Sprintf("%d", csImageWidth(field))
	switch {
	case field.Message != "":
		child := csFieldPath(f, field, cur, byLocal)
		fmt.Fprintf(b, "%sfor (var i = 0; i < n%s; i++) { var c = new %s(); c.ReadImage(p + i * %s.ImageBytes); %s.Add(c); }\n",
			indent, csProp(field), child, child, prop)
	case field.Kind == protoreflect.StringKind:
		fmt.Fprintf(b, "%sfor (var i = 0; i < n%s; i++) { %s.Add(global::System.Runtime.InteropServices.Marshal.PtrToStringUTF8(((IntPtr*)p)[i]) ?? \"\"); }\n",
			indent, csProp(field), prop)
	case field.Kind == protoreflect.BytesKind:
		fmt.Fprintf(b, "%sfor (var i = 0; i < n%s; i++) {\n", indent, csProp(field))
		fmt.Fprintf(b, "%s    var bp = (byte*)(p + i * 16);\n", indent)
		fmt.Fprintf(b, "%s    var d = *(byte**)bp;\n", indent)
		fmt.Fprintf(b, "%s    var blen = (int)(*(nuint*)(bp + 8));\n", indent)
		fmt.Fprintf(b, "%s    %s.Add(d == null ? global::Google.Protobuf.ByteString.Empty : global::Google.Protobuf.ByteString.CopyFrom(new ReadOnlySpan<byte>(d, blen)));\n", indent, prop)
		fmt.Fprintf(b, "%s}\n", indent)
	default:
		expr := csReadScalar(field, at)
		if field.Kind == protoreflect.EnumKind && field.Enum != "" {
			expr = "(" + csScalar(field, cur, byLocal) + ")(" + expr + ")"
		}
		fmt.Fprintf(b, "%sfor (var i = 0; i < n%s; i++) { %s.Add(%s); }\n", indent, csProp(field), prop, expr)
	}
}

// writeCsharpOneofRead turns the image's case word back into the oneof.
func writeCsharpOneofRead(b *strings.Builder, indent string, m *model.Message, oneof string, slotOf map[string]csSlot,
	f *model.File, cur []string, byLocal map[string]*csNode) {
	fmt.Fprintf(b, "%sswitch (*(uint*)(img + %d)) {\n", indent, slotOf[oneofMember(m, oneof)].caseOff)
	for _, field := range m.Fields {
		if field.Skip != "" || field.Oneof != oneof {
			continue
		}
		slot := slotOf[field.Name]
		off := fmt.Sprintf("%d", slot.offset)
		prop := csProp(field)
		fmt.Fprintf(b, "%s    case %d:\n%s        ", indent, field.Number, indent)
		switch {
		case field.Message != "":
			fmt.Fprintf(b, "{ var p = *(byte**)(img + %s); if (p != null) { var c = new %s(); c.ReadImage(p); %s = c; } }\n",
				off, csFieldPath(f, field, cur, byLocal), prop)
		case field.Kind == protoreflect.StringKind:
			fmt.Fprintf(b, "{ var p = *(IntPtr*)(img + %s); %s = p == IntPtr.Zero ? \"\" : (global::System.Runtime.InteropServices.Marshal.PtrToStringUTF8(p) ?? \"\"); }\n", off, prop)
		case field.Kind == protoreflect.BytesKind:
			fmt.Fprintf(b, "{ var p = *(byte**)(img + %s); var len = (int)(*(nuint*)(img + %d)); %s = p == null ? global::Google.Protobuf.ByteString.Empty : global::Google.Protobuf.ByteString.CopyFrom(new ReadOnlySpan<byte>(p, len)); }\n", off, slot.offset+8, prop)
		default:
			expr := csReadScalar(field, "img + "+off)
			fmt.Fprintf(b, "%s = %s;\n", prop, expr)
		}
		fmt.Fprintf(b, "%s        break;\n", indent)
	}
	// A fresh image has no stale case, so the read path must not clear anything
	// (and has no `dst` in scope: this switch runs inside ReadImage).
	fmt.Fprintf(b, "%s    default: break;\n%s}\n", indent, indent)
}

// oneofMember is any member of a oneof (they share one case word).
func oneofMember(m *model.Message, oneof string) string {
	for _, field := range m.Fields {
		if field.Skip == "" && field.Oneof == oneof {
			return field.Name
		}
	}
	return ""
}

func writeCsharpEnum(b *strings.Builder, e *model.Enum, indent string) {
	fmt.Fprintf(b, "%s/// <summary>%s</summary>\n", indent, e.Full)
	fmt.Fprintf(b, "%spublic enum %s {\n", indent, csEnumName(e.Name))
	seen := map[int32]bool{}
	for _, v := range e.Values {
		if seen[v.Number] {
			continue // allow_alias: first name wins, like protoc
		}
		seen[v.Number] = true
		fmt.Fprintf(b, "%s    [global::Google.Protobuf.Reflection.OriginalName(%q)] %s = %d,\n", indent, v.Name, csVariant(e.Name, v.Name), v.Number)
	}
	fmt.Fprintf(b, "%s}\n\n", indent)
}

// csOneofs collects distinct oneof names in field order.
func csOneofs(m *model.Message) []string {
	var out []string
	seen := map[string]bool{}
	for _, field := range m.Fields {
		if field.Skip != "" || field.Oneof == "" || seen[field.Oneof] {
			continue
		}
		seen[field.Oneof] = true
		out = append(out, field.Oneof)
	}
	return out
}

// csCur is the lexically enclosing class chain with Types separators:
// [Person] at root, [Person, Types, PhoneNumber] inside PhoneNumber.
func csCur(n *csNode) []string {
	var chain []string
	for p := n; p != nil; p = p.parent {
		chain = append([]string{p.self}, chain...)
	}
	var cur []string
	for i, c := range chain {
		if i > 0 {
			cur = append(cur, "Types")
		}
		cur = append(cur, c)
	}
	return cur
}

func writeCsharpMessage(b *strings.Builder, f *model.File, n *csNode, indent string, byLocal map[string]*csNode, holder, parser string, layout bool) {
	m := n.msg
	oneofs := csOneofs(m)
	curobj := csCur(n)
	fmt.Fprintf(b, "%s/// <summary>%s</summary>\n", indent, m.Full)
	fmt.Fprintf(b, "%spublic sealed partial class %s : Pbwire.INativeModel {\n", indent, n.self)
	sub := indent + "    "
	for _, field := range m.Fields {
		if field.Skip != "" {
			fmt.Fprintf(b, "%s// skipped %s: %s\n", sub, field.Name, field.Skip)
			continue
		}
		if field.Oneof != "" {
			continue
		}
		writeCsharpField(b, f, n, field, sub, curobj, byLocal)
	}
	for _, o := range oneofs {
		writeCsharpOneof(b, f, n, o, sub, curobj, byLocal)
	}
	writeCsharpCodec(b, f, n, sub, curobj, byLocal, holder, parser)
	if layout {
		writeCsharpLayout(b, f, n, sub, holder)
	}
	if len(n.children) == 0 && len(n.enums) == 0 {
		fmt.Fprintf(b, "%s}\n\n", indent)
		return
	}
	fmt.Fprintf(b, "\n%s    public static partial class Types {\n", indent)
	tindent := indent + "        "
	for _, e := range n.enums {
		writeCsharpEnum(b, e, tindent)
	}
	for _, c := range n.children {
		writeCsharpMessage(b, f, c, tindent, byLocal, holder, parser, layout)
	}
	fmt.Fprintf(b, "%s    }\n%s}\n\n", indent, indent)
}

func writeCsharpField(b *strings.Builder, f *model.File, n *csNode, field model.Field, indent string, cur []string, byLocal map[string]*csNode) {
	prop, num := csProp(field), field.Number
	fmt.Fprintf(b, "%s/// <summary>Field number for the %q field.</summary>\n", indent, field.Name)
	fmt.Fprintf(b, "%spublic const int %sFieldNumber = %d;\n", indent, prop, num)
	switch {
	case field.Repeated:
		elem := csScalar(field, cur, byLocal)
		if field.Message != "" {
			elem = csFieldPath(f, field, cur, byLocal)
		}
		fmt.Fprintf(b, "%sprivate readonly global::Google.Protobuf.Collections.RepeatedField<%s> %s_ = new global::Google.Protobuf.Collections.RepeatedField<%s>();\n", indent, elem, strings.ToLower(prop[:1])+prop[1:], elem)
		fmt.Fprintf(b, "%spublic global::Google.Protobuf.Collections.RepeatedField<%s> %s { get { return %s_; } }\n", indent, elem, prop, strings.ToLower(prop[:1])+prop[1:])
	case field.Message != "":
		fmt.Fprintf(b, "%spublic %s? %s { get; set; }\n", indent, csFieldPath(f, field, cur, byLocal), prop)
	case field.NeedsHas && !field.Required:
		word, bit := csOptBit(n.msg, field)
		bits := fmt.Sprintf("_hasBits%d", word)
		// int32, not untyped Go int: bit 31 is a legal has-bit position, and
		// `1 << 31` would print 2147483648, which no int in C# can hold.
		mask := int32(1) << uint(bit)
		// A string/bytes field needs a non-null default: GetHashCode and
		// Equals run on a message where it was never set.
		init := ""
		switch field.Kind {
		case protoreflect.StringKind:
			init = " = \"\""
		case protoreflect.BytesKind:
			init = " = global::Google.Protobuf.ByteString.Empty"
		}
		fmt.Fprintf(b, "%sprivate %s %s_%s;\n", indent, csScalar(field, cur, byLocal), strings.ToLower(prop[:1])+prop[1:], init)
		fmt.Fprintf(b, "%spublic %s %s { get { return %s_; } set { %s_ = value; %s |= %d; } }\n", indent, csScalar(field, cur, byLocal), prop, strings.ToLower(prop[:1])+prop[1:], strings.ToLower(prop[:1])+prop[1:], bits, mask)
		fmt.Fprintf(b, "%s/// <summary>Gets whether the %q field is set</summary>\n", indent, field.Name)
		fmt.Fprintf(b, "%spublic bool Has%s { get { return (%s & %d) != 0; } }\n", indent, prop, bits, mask)
		fmt.Fprintf(b, "%s/// <summary>Clears the value of the %q field</summary>\n", indent, field.Name)
		fmt.Fprintf(b, "%spublic void Clear%s() { %s &= ~%d; }\n", indent, prop, bits, mask)
	default:
		def := csDefault(field)
		tail := ""
		if def != "" {
			tail = " " + def + ";"
		}
		fmt.Fprintf(b, "%spublic %s %s { get; set; }%s\n", indent, csScalar(field, cur, byLocal), prop, tail)
	}
}

// csOptBit is the has-bit index of an optional field in declaration order.
func csOptBit(m *model.Message, want model.Field) (word, bit int) {
	n := 0
	for _, field := range m.Fields {
		if field.Skip != "" || field.Oneof != "" || !field.NeedsHas || field.Required {
			continue
		}
		if field.Number == want.Number {
			return n / 32, n % 32
		}
		n++
	}
	return 0, 0
}

func csHasBits(m *model.Message) int {
	n := 0
	for _, field := range m.Fields {
		if field.Skip == "" && field.Oneof == "" && field.NeedsHas && !field.Required {
			n++
		}
	}
	return (n + 31) / 32
}

func csDefault(field model.Field) string {
	if field.Kind == protoreflect.StringKind {
		return " = \"\""
	}
	if field.Kind == protoreflect.BytesKind {
		return " = ByteString.Empty"
	}
	return ""
}

func writeCsharpOneof(b *strings.Builder, f *model.File, n *csNode, o string, indent string, cur []string, byLocal map[string]*csNode) {
	m := n.msg
	// null! so a never-set oneof satisfies the non-nullable storage type.
	fmt.Fprintf(b, "%sprivate object %s_ = null!;\n", indent, o)
	fmt.Fprintf(b, "%s/// <summary>Enum of possible cases for the %q oneof.</summary>\n", indent, o)
	fmt.Fprintf(b, "%spublic enum %sOneofCase {\n", indent, csPascal(o))
	fmt.Fprintf(b, "%s    None = 0,\n", indent)
	for _, field := range m.Fields {
		if field.Skip != "" || field.Oneof != o {
			continue
		}
		fmt.Fprintf(b, "%s    %s = %d,\n", indent, csProp(field), field.Number)
	}
	fmt.Fprintf(b, "%s}\n", indent)
	fmt.Fprintf(b, "%sprivate %sOneofCase %sCase_ = %sOneofCase.None;\n", indent, csPascal(o), o, csPascal(o))
	fmt.Fprintf(b, "%spublic %sOneofCase %sCase { get { return %sCase_; } }\n", indent, csPascal(o), csPascal(o), o)
	fmt.Fprintf(b, "%spublic void Clear%s() { %sCase_ = %sOneofCase.None; %s_ = null; }\n", indent, csPascal(o), o, csPascal(o), o)
	caseName := csPascal(o) + "OneofCase"
	for _, field := range m.Fields {
		if field.Skip != "" || field.Oneof != o {
			continue
		}
		p := csProp(field)
		fmt.Fprintf(b, "%s/// <summary>Field number for the %q field.</summary>\n", indent, field.Name)
		fmt.Fprintf(b, "%spublic const int %sFieldNumber = %d;\n", indent, p, field.Number)
		payload := csScalar(field, cur, byLocal)
		if field.Message != "" {
			payload = csFieldPath(f, field, cur, byLocal)
		}
		get := fmt.Sprintf("(%s) %s_", payload, o)
		def := csOneofDefault(field)
		if field.Message != "" {
			fmt.Fprintf(b, "%spublic %s? %s {\n", indent, payload, p)
			fmt.Fprintf(b, "%s    get { return %sCase_ == %s.%s ? (%s) %s_ : null; }\n", indent, o, caseName, p, payload, o)
		} else {
			fmt.Fprintf(b, "%spublic %s %s {\n", indent, payload, p)
			fmt.Fprintf(b, "%s    get { return %sCase_ == %s.%s ? %s : %s; }\n", indent, o, caseName, p, get, def)
		}
		if field.Kind == protoreflect.StringKind {
			fmt.Fprintf(b, "%s    set { %s_ = global::Google.Protobuf.ProtoPreconditions.CheckNotNull(value, \"value\"); %sCase_ = %s.%s; }\n", indent, o, o, caseName, p)
		} else if field.Message != "" {
			fmt.Fprintf(b, "%s    set { %s_ = value!; %sCase_ = value == null ? %s.None : %s.%s; }\n", indent, o, o, caseName, caseName, p)
		} else if field.Kind == protoreflect.BytesKind {
			fmt.Fprintf(b, "%s    set { %s_ = global::Google.Protobuf.ProtoPreconditions.CheckNotNull(value, \"value\"); %sCase_ = %s.%s; }\n", indent, o, o, caseName, p)
		} else {
			fmt.Fprintf(b, "%s    set { %s_ = value; %sCase_ = %s.%s; }\n", indent, o, o, caseName, p)
		}
		fmt.Fprintf(b, "%s}\n", indent)
		fmt.Fprintf(b, "%s/// <summary>Gets whether the %q field is set</summary>\n", indent, field.Name)
		fmt.Fprintf(b, "%spublic bool Has%s { get { return %sCase_ == %s.%s; } }\n", indent, p, o, caseName, p)
		fmt.Fprintf(b, "%s/// <summary> Clears the value of the oneof if it's currently set to %q </summary>\n", indent, field.Name)
		fmt.Fprintf(b, "%spublic void Clear%s() { if (Has%s) { Clear%s(); } }\n", indent, p, p, csPascal(o))
	}
}

func csOneofDefault(field model.Field) string {
	switch field.Kind {
	case protoreflect.StringKind:
		return "\"\""
	case protoreflect.BytesKind:
		return "ByteString.Empty"
	case protoreflect.BoolKind:
		return "false"
	case protoreflect.FloatKind, protoreflect.DoubleKind:
		return "0"
	default:
		return "0"
	}
}

func writeCsharpCodec(b *strings.Builder, f *model.File, n *csNode, indent string, cur []string, byLocal map[string]*csNode, holder, parser string) {
	m := n.msg
	full := m.Full
	for i := 0; i < csHasBits(m); i++ {
		fmt.Fprintf(b, "%sprivate int _hasBits%d;\n", indent, i)
	}
	fmt.Fprintf(b, "%spublic static "+parser+"<%s> Parser { get; } = new "+parser+"<%s>((m, b) => m.MergeFromBytes(b));\n", indent, n.self, n.self)
	fmt.Fprintf(b, "%spublic %s() { }\n", indent, n.self)
	fmt.Fprintf(b, "%spublic %s(%s other) : this() {\n", indent, n.self, n.self)
	for _, field := range m.Fields {
		if field.Skip != "" {
			continue
		}
		writeCsharpCopy(b, f, field, indent+"    ", cur, byLocal)
	}
	fmt.Fprintf(b, "%s}\n", indent)
	fmt.Fprintf(b, "%spublic %s Clone() { return new %s(this); }\n", indent, n.self, n.self)
	fmt.Fprintf(b, "%spublic override bool Equals(object? other) { return Equals(other as %s); }\n", indent, n.self)
	fmt.Fprintf(b, "%spublic bool Equals(%s? other) {\n", indent, n.self)
	fmt.Fprintf(b, "%s    if (ReferenceEquals(other, null)) { return false; }\n", indent)
	fmt.Fprintf(b, "%s    if (ReferenceEquals(other, this)) { return true; }\n", indent)
	for _, field := range m.Fields {
		if field.Skip != "" {
			continue
		}
		writeCsharpEquals(b, n, field, indent+"    ", cur)
	}
	for _, o := range csOneofs(m) {
		fmt.Fprintf(b, "%s    if (%sCase != other.%sCase) { return false; }\n", indent, csPascal(o), csPascal(o))
	}
	fmt.Fprintf(b, "%s    return true;\n%s}\n", indent, indent)
	fmt.Fprintf(b, "%spublic override int GetHashCode() {\n", indent)
	fmt.Fprintf(b, "%s    int hash = 1;\n", indent)
	for _, field := range m.Fields {
		if field.Skip != "" || field.Oneof != "" {
			continue
		}
		p := csProp(field)
		switch {
		case field.Message != "" && !field.Repeated:
			// An absent singular message field is null; protoc hashes 0.
			fmt.Fprintf(b, "%s    hash ^= (%s == null ? 0 : %s.GetHashCode());\n", indent, p, p)
		/* Bitwise so that two NaN payloads hash alike, which is what the
		 * reference does for a singular float/double. A repeated one is a
		 * RepeatedField and hashes as the collection. */
		case field.Kind == protoreflect.FloatKind && !field.Repeated:
			fmt.Fprintf(b, "%s    hash ^= global::Google.Protobuf.Collections.ProtobufEqualityComparers.BitwiseSingleEqualityComparer.GetHashCode(%s);\n", indent, p)
		case field.Kind == protoreflect.DoubleKind && !field.Repeated:
			fmt.Fprintf(b, "%s    hash ^= global::Google.Protobuf.Collections.ProtobufEqualityComparers.BitwiseDoubleEqualityComparer.GetHashCode(%s);\n", indent, p)
		default:
			fmt.Fprintf(b, "%s    hash ^= %s.GetHashCode();\n", indent, p)
		}
	}
	for _, o := range csOneofs(m) {
		fmt.Fprintf(b, "%s    hash ^= %sCase.GetHashCode();\n", indent, csPascal(o))
		fmt.Fprintf(b, "%s    hash ^= (%s_ == null ? 0 : %s_.GetHashCode());\n", indent, o, o)
	}
	fmt.Fprintf(b, "%s    return hash;\n%s}\n", indent, indent)
	fmt.Fprintf(b, "%spublic override string ToString() {\n", indent)
	var parts []string
	for _, field := range m.Fields {
		if field.Skip != "" || field.Oneof != "" || field.Repeated || field.Message != "" {
			continue
		}
		p := csProp(field)
		parts = append(parts, fmt.Sprintf("\"%s = \" + %s", p, p))
	}
	for _, o := range csOneofs(m) {
		parts = append(parts, fmt.Sprintf("\"%s = \" + %sCase", o, csPascal(o)))
	}
	if len(parts) == 0 {
		fmt.Fprintf(b, "%s    return \"%s {{ }}\";\n", indent, n.self)
	} else {
		fmt.Fprintf(b, "%s    return \"%s {{ \" + %s + \" }}\";\n", indent, n.self, strings.Join(parts, " + \", \" + "))
	}
	fmt.Fprintf(b, "%s}\n", indent)
	fmt.Fprintf(b, "%spublic byte[] ToByteArray() {\n", indent)
	fmt.Fprintf(b, "%s    using Pbwire.Message msg = "+holder+".Schema.Message(%q).NewMessage();\n", indent, full)
	fmt.Fprintf(b, "%s    EncodeInto(msg);\n", indent)
	fmt.Fprintf(b, "%s    return msg.Encode();\n%s}\n", indent, indent)
	fmt.Fprintf(b, "%spublic ByteString ToByteString() => ByteString.CopyFrom(ToByteArray());\n", indent)
	fmt.Fprintf(b, "%spublic void WriteTo(CodedOutputStream output) {\n", indent)
	fmt.Fprintf(b, "%s    // No raw-bytes sink in 3.36: chunk through WriteRawTag.\n", indent)
	fmt.Fprintf(b, "%s    var bytes = ToByteArray();\n", indent)
	fmt.Fprintf(b, "%s    int i = 0;\n", indent)
	fmt.Fprintf(b, "%s    for (; i + 5 <= bytes.Length; i += 5) { output.WriteRawTag(bytes[i], bytes[i + 1], bytes[i + 2], bytes[i + 3], bytes[i + 4]); }\n", indent)
	fmt.Fprintf(b, "%s    for (; i < bytes.Length; i++) { output.WriteRawTag(bytes[i]); }\n", indent)
	fmt.Fprintf(b, "%s}\n", indent)
	fmt.Fprintf(b, "%spublic int CalculateSize() => ToByteArray().Length;\n", indent)
	fmt.Fprintf(b, "%s internal void MergeFromBytes(byte[] wire) {\n", indent)
	fmt.Fprintf(b, "%s    using Pbwire.Message msg = "+holder+".Schema.Message(%q).Decode(wire);\n", indent, full)
	fmt.Fprintf(b, "%s    LoadFrom(msg);\n%s}\n", indent, indent)
	fmt.Fprintf(b, "%s/// <summary>The native-object entry: one call per op over the shipped converter.</summary>\n", indent)
	fmt.Fprintf(b, "%svoid Pbwire.INativeModel.WriteTo(Pbwire.Message msg) { EncodeInto(msg); }\n", indent)
	fmt.Fprintf(b, "%svoid Pbwire.INativeModel.ReadFrom(Pbwire.Message msg) { LoadFrom(msg); }\n", indent)
	fmt.Fprintf(b, "%spublic void MergeFrom(%s other) {\n", indent, n.self)
	fmt.Fprintf(b, "%s    if (other == null) { return; }\n", indent)
	for _, field := range m.Fields {
		if field.Skip != "" {
			continue
		}
		writeCsharpCopy(b, f, field, indent+"    ", cur, byLocal)
	}
	fmt.Fprintf(b, "%s}\n", indent)
	fmt.Fprintf(b, "%sinternal void EncodeInto(Pbwire.Message msg) {\n", indent)
	for _, field := range m.Fields {
		if field.Skip != "" || field.Oneof != "" {
			continue
		}
		writeCsharpEncode(b, field, indent+"    ")
	}
	for _, o := range csOneofs(m) {
		caseName := csPascal(o) + "OneofCase"
		fmt.Fprintf(b, "%s    switch (%sCase) {\n", indent, csPascal(o))
		for _, field := range m.Fields {
			if field.Skip != "" || field.Oneof != o {
				continue
			}
			p := csProp(field)
			fmt.Fprintf(b, "%s        case %s.%s:\n", indent, caseName, p)
			writeCsharpEncodeOne(b, field, p, indent+"            ")
			fmt.Fprintf(b, "%s            break;\n", indent)
		}
		fmt.Fprintf(b, "%s        default: break;\n%s    }\n", indent, indent)
	}
	fmt.Fprintf(b, "%s}\n", indent)
	fmt.Fprintf(b, "%sinternal void LoadFrom(Pbwire.Message msg) {\n", indent)
	for _, field := range m.Fields {
		if field.Skip != "" {
			continue
		}
		if field.Oneof != "" {
			writeCsharpLoadOne(b, f, field, indent+"    ", cur, byLocal)
			continue
		}
		writeCsharpLoad(b, f, field, indent+"    ", cur, byLocal)
	}
	fmt.Fprintf(b, "%s}\n", indent)
}

func writeCsharpCopy(b *strings.Builder, f *model.File, field model.Field, indent string, cur []string, byLocal map[string]*csNode) {
	p := csProp(field)
	switch {
	case field.Repeated:
		fmt.Fprintf(b, "%s%s.Add(other.%s);\n", indent, p, p)
	case field.Message != "" && field.Oneof == "":
		fmt.Fprintf(b, "%sif (other.%s != null) { if (%s == null) { %s = new %s(); } %s.MergeFrom(other.%s); }\n",
			indent, p, p, p, csFieldPath(f, field, cur, byLocal), p, p)
	case field.Oneof != "":
		/* Copy the member the source actually set: assigning every member
		 * would reset the case word to None (each setter clears it). */
		{
			caseName := csPascal(field.Oneof) + "OneofCase"
			if field.Message != "" {
				fmt.Fprintf(b, "%sif (other.%sCase == %s.%s) { %s = other.%s?.Clone(); }\n",
					indent, csPascal(field.Oneof), caseName, p, p, p)
			} else {
				fmt.Fprintf(b, "%sif (other.%sCase == %s.%s) { %s = other.%s; }\n",
					indent, csPascal(field.Oneof), caseName, p, p, p)
			}
		}
	default:
		if field.NeedsHas && !field.Required {
			fmt.Fprintf(b, "%sif (other.Has%s) { %s = other.%s; }\n", indent, p, p, p)
		} else {
			fmt.Fprintf(b, "%s%s = other.%s;\n", indent, p, p)
		}
	}
}

func writeCsharpEquals(b *strings.Builder, n *csNode, field model.Field, indent string, cur []string) {
	p := csProp(field)
	switch {
	case field.Oneof != "":
		fmt.Fprintf(b, "%sif (!object.Equals(%s, other.%s)) { return false; }\n", indent, p, p)
	case field.Message != "":
		fmt.Fprintf(b, "%sif (!object.Equals(%s, other.%s)) { return false; }\n", indent, p, p)
	case field.Repeated:
		fmt.Fprintf(b, "%sif (!%s.Equals(other.%s)) { return false; }\n", indent, p, p)
	case field.Kind == protoreflect.FloatKind:
		if field.NeedsHas && !field.Required {
			fmt.Fprintf(b, "%sif (Has%s != other.Has%s || (Has%s && !global::Google.Protobuf.Collections.ProtobufEqualityComparers.BitwiseSingleEqualityComparer.Equals(%s, other.%s))) { return false; }\n", indent, p, p, p, p, p)
		} else {
			fmt.Fprintf(b, "%sif (!global::Google.Protobuf.Collections.ProtobufEqualityComparers.BitwiseSingleEqualityComparer.Equals(%s, other.%s)) { return false; }\n", indent, p, p)
		}
	case field.Kind == protoreflect.DoubleKind:
		if field.NeedsHas && !field.Required {
			fmt.Fprintf(b, "%sif (Has%s != other.Has%s || (Has%s && !global::Google.Protobuf.Collections.ProtobufEqualityComparers.BitwiseDoubleEqualityComparer.Equals(%s, other.%s))) { return false; }\n", indent, p, p, p, p, p)
		} else {
			fmt.Fprintf(b, "%sif (!global::Google.Protobuf.Collections.ProtobufEqualityComparers.BitwiseDoubleEqualityComparer.Equals(%s, other.%s)) { return false; }\n", indent, p, p)
		}
	default:
		if field.NeedsHas && !field.Required {
			fmt.Fprintf(b, "%sif (Has%s != other.Has%s || (Has%s && %s != other.%s)) { return false; }\n", indent, p, p, p, p, p)
		} else {
			fmt.Fprintf(b, "%sif (%s != other.%s) { return false; }\n", indent, p, p)
		}
	}
}

func writeCsharpEncode(b *strings.Builder, field model.Field, indent string) {
	p := csProp(field)
	if field.Required && field.Message != "" {
		fmt.Fprintf(b, "%sif (%s == null) { throw new global::System.InvalidOperationException(\"required field %s is not set\"); }\n",
			indent, p, field.Name)
	}
	cond := csEncodeCond(field, p)
	if cond != "" {
		fmt.Fprintf(b, "%sif (%s) {\n", indent, cond)
		writeCsharpEncodeOne(b, field, p, indent+"    ")
		fmt.Fprintf(b, "%s}\n", indent)
		return
	}
	writeCsharpEncodeOne(b, field, p, indent)
}

func csEncodeCond(field model.Field, p string) string {
	if field.Required {
		return ""
	}
	switch {
	case field.Repeated:
		return p + ".Count != 0"
	case field.Message != "":
		return p + " != null"
	case field.NeedsHas:
		return "Has" + p
	case field.Kind == protoreflect.BoolKind:
		return p
	case field.Kind == protoreflect.StringKind, field.Kind == protoreflect.BytesKind:
		return p + ".Length != 0"
	case field.Kind == protoreflect.FloatKind, field.Kind == protoreflect.DoubleKind:
		return p + " != 0"
	default:
		return p + " != 0"
	}
}

func writeCsharpEncodeOne(b *strings.Builder, field model.Field, p string, indent string) {
	key := fmt.Sprintf("%q", field.Name)
	call := "msg.Set(" + key + ", " + csSetArg(field, p) + ");"
	switch {
	case field.Repeated && field.Message != "":
		fmt.Fprintf(b, "%sforeach (var v in %s) { v.EncodeInto(msg.SetMessage(%s)); }\n", indent, p, key)
	case field.Repeated:
		fmt.Fprintf(b, "%sforeach (var v in %s) { msg.Set(%s, %s); }\n", indent, p,
			key, csSetArg(field, "v"))
	case field.Message != "":
		fmt.Fprintf(b, "%s%s!.EncodeInto(msg.SetMessage(%s));\n", indent, p, key)
	default:
		fmt.Fprintf(b, "%s%s\n", indent, call)
	}
}

// csSetArg renders the setter value: Set takes long/bool/float/double/string/span.
func csSetArg(field model.Field, expr string) string {
	switch field.Kind {
	case protoreflect.Int32Kind, protoreflect.Sint32Kind, protoreflect.Sfixed32Kind,
		protoreflect.EnumKind:
		return "(long)" + expr
	case protoreflect.Uint32Kind, protoreflect.Fixed32Kind:
		return "(long)" + expr
	case protoreflect.Uint64Kind, protoreflect.Fixed64Kind, protoreflect.Int64Kind,
		protoreflect.Sint64Kind, protoreflect.Sfixed64Kind:
		return "unchecked((long)" + expr + ")"
	case protoreflect.StringKind:
		return expr
	case protoreflect.BytesKind:
		return expr + ".Span"
	default:
		return expr
	}
}

func writeCsharpLoad(b *strings.Builder, f *model.File, field model.Field, indent string, cur []string, byLocal map[string]*csNode) {
	p := csProp(field)
	key := fmt.Sprintf("%q", field.Name)
	switch {
	case field.Repeated && field.Message != "":
		fmt.Fprintf(b, "%sfor (int i = 0; i < msg.Count(%s); i++) {\n", indent, key)
		fmt.Fprintf(b, "%s    var item = msg.At(%s, i);\n", indent, key)
		fmt.Fprintf(b, "%s    var sub = item?.AsMessage();\n", indent)
		fmt.Fprintf(b, "%s    if (sub != null) { var m = new %s(); m.LoadFrom(sub); %s.Add(m); }\n", indent, csFieldPath(f, field, cur, byLocal), p)
		fmt.Fprintf(b, "%s}\n", indent)
	case field.Repeated:
		fmt.Fprintf(b, "%sfor (int i = 0; i < msg.Count(%s); i++) {\n", indent, key)
		fmt.Fprintf(b, "%s    var item%s = msg.At(%s, i);\n", indent, p, key)
		fmt.Fprintf(b, "%s    if (item%s != null) { %s.Add(%s); }\n", indent, p, p, strings.Replace(csGetElem(field, cur, byLocal), "item.", "item"+p+".", -1))
		fmt.Fprintf(b, "%s}\n", indent)
	case field.Message != "":
		fmt.Fprintf(b, "%svar sub%s = msg.GetMessage(%s);\n", indent, p, key)
		fmt.Fprintf(b, "%sif (sub%s != null) { %s = new %s(); %s.LoadFrom(sub%s); }\n", indent, p, p, csFieldPath(f, field, cur, byLocal), p, p)
	case field.NeedsHas && !field.Required:
		fmt.Fprintf(b, "%sif (msg.Has(%s)) { %s = %s; }\n", indent, key, p, csGetScalar(field, key, cur, byLocal))
	default:
		if field.Kind == protoreflect.BytesKind {
			fmt.Fprintf(b, "%s{ var tmp%s = msg.GetBytes(%s); %s = tmp%s == null ? ByteString.Empty : ByteString.CopyFrom(tmp%s); }\n", indent, p, key, p, p, p)
		} else {
			fmt.Fprintf(b, "%s%s = %s;\n", indent, p, csGetScalar(field, key, cur, byLocal))
		}
	}
}

func writeCsharpLoadOne(b *strings.Builder, f *model.File, field model.Field, indent string, cur []string, byLocal map[string]*csNode) {
	p := csProp(field)
	key := fmt.Sprintf("%q", field.Name)
	if field.Message != "" {
		fmt.Fprintf(b, "%svar sub%s = msg.GetMessage(%s);\n", indent, p, key)
		fmt.Fprintf(b, "%sif (sub%s != null) { var m = new %s(); m.LoadFrom(sub%s); %s = m; }\n", indent, p, csFieldPath(f, field, cur, byLocal), p, p)
		return
	}
	fmt.Fprintf(b, "%sif (msg.Has(%s)) { %s = %s; }\n", indent, key, p, csGetScalar(field, key, cur, byLocal))
}

func csGetScalar(field model.Field, key string, cur []string, byLocal map[string]*csNode) string {
	switch field.Kind {
	case protoreflect.Int32Kind, protoreflect.Sint32Kind, protoreflect.Sfixed32Kind,
		protoreflect.EnumKind:
		return fmt.Sprintf("(%s)msg.GetInt(%s)", csScalar(field, cur, byLocal), key)
	case protoreflect.Uint32Kind, protoreflect.Fixed32Kind:
		return fmt.Sprintf("(uint)msg.GetInt(%s)", key)
	case protoreflect.Uint64Kind, protoreflect.Fixed64Kind:
		return fmt.Sprintf("unchecked((ulong)msg.GetLong(%s))", key)
	case protoreflect.FloatKind:
		return fmt.Sprintf("msg.GetFloat(%s)", key)
	case protoreflect.DoubleKind:
		return fmt.Sprintf("msg.GetDouble(%s)", key)
	case protoreflect.BoolKind:
		return fmt.Sprintf("msg.GetBool(%s)", key)
	case protoreflect.StringKind:
		return fmt.Sprintf("msg.GetString(%s) ?? \"\"", key)
	case protoreflect.BytesKind:
		return "ByteString.Empty" // handled with a tmp in writeCsharpLoad
	default:
		return fmt.Sprintf("msg.GetLong(%s)", key)
	}
}

func csGetElem(field model.Field, cur []string, byLocal map[string]*csNode) string {
	switch field.Kind {
	case protoreflect.Int32Kind, protoreflect.Sint32Kind, protoreflect.Sfixed32Kind,
		protoreflect.EnumKind:
		return fmt.Sprintf("(%s)item.AsInt()", csScalar(field, cur, byLocal))
	case protoreflect.Uint32Kind, protoreflect.Fixed32Kind:
		return "unchecked((uint)item.AsInt())"
	case protoreflect.Uint64Kind, protoreflect.Fixed64Kind:
		return "unchecked((ulong)item.AsLong())"
	case protoreflect.Int64Kind, protoreflect.Sint64Kind, protoreflect.Sfixed64Kind:
		return "item.AsLong()"
	case protoreflect.FloatKind:
		return "item.AsFloat()"
	case protoreflect.DoubleKind:
		return "item.AsDouble()"
	case protoreflect.BoolKind:
		return "item.AsBool()"
	case protoreflect.StringKind:
		return "item.AsString() ?? \"\""
	case protoreflect.BytesKind:
		return "ByteString.CopyFrom(item.AsBytes() ?? Array.Empty<byte>())"
	default:
		return "item.AsLong()"
	}
}

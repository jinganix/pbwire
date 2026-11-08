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
			writeCsharpMessage(&b, f, r, "    ", byLocal, holder, parser)
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
	b.WriteString("        private static volatile Schema _schema;\n")
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

func writeCsharpMessage(b *strings.Builder, f *model.File, n *csNode, indent string, byLocal map[string]*csNode, holder, parser string) {
	m := n.msg
	oneofs := csOneofs(m)
	curobj := csCur(n)
	fmt.Fprintf(b, "%s/// <summary>%s</summary>\n", indent, m.Full)
	fmt.Fprintf(b, "%spublic sealed partial class %s {\n", indent, n.self)
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
		writeCsharpMessage(b, f, c, tindent, byLocal, holder, parser)
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
	fmt.Fprintf(b, "%sprivate object %s_;\n", indent, o)
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
			fmt.Fprintf(b, "%s    set { %s_ = value; %sCase_ = value == null ? %s.None : %s.%s; }\n", indent, o, o, caseName, caseName, p)
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
		case field.Kind == protoreflect.FloatKind:
			fmt.Fprintf(b, "%s    hash ^= global::Google.Protobuf.Collections.ProtobufEqualityComparers.BitwiseSingleEqualityComparer.GetHashCode(%s);\n", indent, p)
		case field.Kind == protoreflect.DoubleKind:
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
		fmt.Fprintf(b, "%s%s.EncodeInto(msg.SetMessage(%s));\n", indent, p, key)
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

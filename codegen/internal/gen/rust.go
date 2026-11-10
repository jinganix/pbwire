package gen

// Rust emitter: prost-shaped structs over the dynamic API.
//
// The surface mirrors what prost-build would generate for the same .proto
// (field names, Option for optional/message/oneof, i32 for enums, nested
// modules, encode_to_vec/decode), but every codec call goes through the
// binding's existing dynamic API, so the wire format stays in the C core.
// No prost dependency: the structs are plain, with Debug/Default derived
// where prost's Message derive would provide them.

import (
	"encoding/base64"
	"fmt"
	"strings"

	"pbwire-codegen/internal/model"

	"google.golang.org/protobuf/reflect/protoreflect"
)

var rustKeywords = map[string]bool{
	"as": true, "break": true, "const": true, "continue": true, "crate": true,
	"dyn": true, "else": true, "enum": true, "extern": true, "false": true,
	"fn": true, "for": true, "if": true, "impl": true, "in": true, "let": true,
	"loop": true, "match": true, "mod": true, "move": true, "mut": true,
	"pub": true, "ref": true, "return": true, "self": true, "static": true,
	"struct": true, "super": true, "trait": true, "true": true, "type": true,
	"unsafe": true, "use": true, "where": true, "while": true, "async": true,
	"await": true, "try": true, "yield": true, "abstract": true, "become": true,
	"box": true, "do": true, "final": true, "macro": true, "override": true,
	"priv": true, "typeof": true, "union": true, "unsized": true, "virtual": true,
}

func rustName(name string) string {
	if rustKeywords[name] {
		return "r#" + name
	}
	return name
}

// rustRawKeywords are the keywords a raw identifier can still name. A message
// named Struct/Type/Match yields a module segment that is one of them; the
// crate/self/super trio is not a legal raw identifier and gets a suffix.
var rustRawKeywords = map[string]bool{
	"abstract": true, "as": true, "async": true, "await": true, "become": true,
	"box": true, "break": true, "const": true, "continue": true, "do": true,
	"dyn": true, "else": true, "enum": true, "extern": true, "false": true,
	"final": true, "fn": true, "for": true, "if": true, "impl": true,
	"in": true, "let": true, "loop": true, "macro": true, "match": true,
	"mod": true, "move": true, "mut": true, "override": true, "priv": true,
	"pub": true, "ref": true, "return": true, "static": true, "struct": true,
	"trait": true, "true": true, "try": true, "type": true, "typeof": true,
	"unsafe": true, "unsized": true, "use": true, "virtual": true,
	"where": true, "while": true, "yield": true,
}

// rustModSegment keeps a snake_case module name usable: a keyword needs a raw
// identifier, the crate/self/super trio a suffix (they cannot be raw).
func rustModSegment(name string) string {
	switch name {
	case "crate", "self", "super":
		return name + "_pbwire"
	}
	if rustRawKeywords[name] {
		return "r#" + name
	}
	return name
}

// rustSnake is prost's module naming: CamelCase message name to snake_case.
func rustSnake(s string) string {
	var b strings.Builder
	for i, r := range s {
		if i > 0 && r >= 'A' && r <= 'Z' {
			b.WriteByte('_')
		}
		b.WriteRune(r)
	}
	return rustModSegment(strings.ToLower(b.String()))
}

// rustCamel is prost's oneof/variant naming: SCREAMING or snake_case to
// UpperCamelCase (COLOR3_GREEN of Color3 becomes Green, my_kind becomes
// MyKind). The tail lowercases because enum values arrive screaming.
func rustCamel(s string) string {
	parts := strings.Split(s, "_")
	for i, p := range parts {
		if p == "" {
			continue
		}
		parts[i] = strings.ToUpper(p[:1]) + strings.ToLower(p[1:])
	}
	return strings.Join(parts, "")
}

// rustScreaming is the SCREAMING_SNAKE form used for enum prefix stripping.
func rustScreaming(s string) string {
	var b strings.Builder
	for i, r := range s {
		if i > 0 && r >= 'A' && r <= 'Z' {
			b.WriteByte('_')
		}
		b.WriteRune(r)
	}
	return strings.ToUpper(b.String())
}

// rustVariant is prost's enum variant name: strip the ENUM_NAME_ prefix, then
// CamelCase what remains (COLOR3_GREEN of Color3 becomes Green).
func rustVariant(enumLocal, value string) string {
	segs := strings.Split(enumLocal, "_")
	prefix := rustScreaming(segs[len(segs)-1]) + "_"
	return rustCamel(strings.TrimPrefix(value, prefix))
}

// rustNode is a message with its nested types attached. Nesting is recovered
// from the qualified names: a message is nested when its Full extends another
// message's Full by one dotted segment.
type rustNode struct {
	msg      *model.Message
	self     string // struct name: last local segment
	segs     []string
	parent   *rustNode
	children []*rustNode
	enums    []*model.Enum
}

func rustTree(f *model.File) []*rustNode {
	byFull := map[string]*rustNode{}
	var all []*rustNode
	for i := range f.Messages {
		m := &f.Messages[i]
		segs := strings.Split(m.Name, "_")
		all = append(all, &rustNode{msg: m, self: segs[len(segs)-1]})
		byFull[m.Full] = all[len(all)-1]
	}
	parentOf := func(full string) *rustNode {
		var best *rustNode
		bestLen := -1
		for pf, n := range byFull {
			rest, ok := strings.CutPrefix(full, pf+".")
			if ok && !strings.Contains(rest, ".") && len(pf) > bestLen {
				best, bestLen = n, len(pf)
			}
		}
		return best
	}
	var roots []*rustNode
	for _, n := range all {
		n.parent = parentOf(n.msg.Full)
		if n.parent != nil {
			n.parent.children = append(n.parent.children, n)
		} else {
			roots = append(roots, n)
		}
	}
	var setSegs func(n *rustNode)
	setSegs = func(n *rustNode) {
		if n.parent == nil {
			n.segs = nil
			return
		}
		setSegs(n.parent)
		n.segs = append(append([]string{}, n.parent.segs...), rustSnake(n.parent.self))
	}
	for _, n := range all {
		setSegs(n)
	}
	for i := range f.Enums {
		e := &f.Enums[i]
		if p := parentOf(e.Full); p != nil {
			p.enums = append(p.enums, e)
		}
	}
	return roots
}

// rustRel renders target (module segs + type name) relative to cur, the
// module the reference is written in, using super:: to climb out.
func rustRel(target []string, cur []string) string {
	i := 0
	for i < len(target)-1 && i < len(cur) && target[i] == cur[i] {
		i++
	}
	var parts []string
	for range cur[i:] {
		parts = append(parts, "super")
	}
	rest := target[i:]
	if len(rest) == 0 {
		// Self-reference (a recursive field names its own struct).
		rest = target[len(target)-1:]
	}
	parts = append(parts, rest...)
	return strings.Join(parts, "::")
}

// rustRefs is the message-reference graph: message local name to the locals
// it names in field types. Recursive messages need Box (like prost-build).
func rustRefs(f *model.File) map[string]map[string]bool {
	g := map[string]map[string]bool{}
	for i := range f.Messages {
		m := &f.Messages[i]
		set := map[string]bool{}
		for _, field := range m.Fields {
			if field.Skip == "" && field.Message != "" {
				set[field.Message] = true
			}
		}
		g[m.Name] = set
	}
	return g
}

// rustClosure returns every message reachable from local (inclusive).
func rustClosure(g map[string]map[string]bool, local string) map[string]bool {
	out := map[string]bool{}
	var visit func(s string)
	visit = func(s string) {
		if out[s] {
			return
		}
		out[s] = true
		for t := range g[s] {
			visit(t)
		}
	}
	visit(local)
	return out
}

// rustBoxed reports whether a field to target from owner must be boxed:
// the edge closes a reference cycle (prost-build boxes those edges).
func rustBoxed(g map[string]map[string]bool, owner, target string) bool {
	if owner == target {
		return true
	}
	return rustClosure(g, target)[owner]
}

func rustFiles(f *model.File, opt Options) []File {
	// Models and schema part ways: structs in one file, the embedded
	// descriptor set plus its loader in the other (included by path, like
	// sibling files). model=false leaves the schema file for dynamic users.
	metaName := rustModuleName(f) + "_meta"
	var files []File
	if opt.Model {
		var b strings.Builder
		b.WriteString(header(f, "Prost-shaped structs over the pbwire runtime; the wire format stays in the C core."))
		// A generated module exposes every field, codec and enum helper; a
		// consumer that uses a subset would otherwise compile it with clippy
		// dead_code / field_reassign_with_default warnings.
		b.WriteString("#![allow(dead_code, clippy::field_reassign_with_default)]\n")
		b.WriteString("use pbwire::{DecodeError, Message, Schema};\n\n")
		fmt.Fprintf(&b, "#[path = \"%s.rs\"]\npub mod meta;\n\n", metaName)
		// Sibling imports ride inline modules (path-relative, so the include
		// works however this file was pulled in); foreign refs point through
		// them (see rustForeign).
		for _, imp := range f.Imports {
			fmt.Fprintf(&b, "\n#[path = \"%s_pbwire.rs\"]\npub mod %s;", stem(imp.Path), rustImpMod(imp.Path))
		}
		b.WriteString("\n")
		byLocal := map[string]*rustNode{}
		var walk func(n *rustNode)
		walk = func(n *rustNode) {
			byLocal[n.msg.Name] = n
			for _, c := range n.children {
				walk(c)
			}
		}
		roots := rustTree(f)
		for _, r := range roots {
			walk(r)
		}
		g := rustRefs(f)
		for _, r := range roots {
			writeRustMessage(&b, f, r, nil, byLocal, g, opt)
		}
		for i := range f.Enums {
			e := &f.Enums[i]
			if rustEnumRoot(f, e) {
				writeRustEnum(&b, e)
			}
		}
		files = append(files, File{Name: rustModuleName(f) + ".rs", Content: b.String()})
	}
	var meta strings.Builder
	writeRustMeta(&meta, f, opt)
	files = append(files, File{Name: metaName + ".rs", Content: meta.String()})
	return files
}

// writeRustMeta emits the schema file: the embedded descriptor set plus its
// loader. register=true keeps the lazy per-thread schema (as before);
// otherwise init() must run first on the thread, loudly otherwise. registry
// adds the whole-file message list plus a preload that warms every type.
func writeRustMeta(b *strings.Builder, f *model.File, opt Options) {
	b.WriteString(header(f, "Embedded descriptor set plus its loader: the schema side of the generated model."))
	b.WriteString("#![allow(dead_code, clippy::field_reassign_with_default)]\n")
	b.WriteString("use pbwire::Schema;\n\n")
	raw, err := base64.StdEncoding.DecodeString(opt.DescSetB64)
	if err != nil {
		raw = nil
	}
	var nums strings.Builder
	for i, v := range raw {
		if i > 0 {
			nums.WriteString(", ")
		}
		fmt.Fprintf(&nums, "%d", v)
	}
	fmt.Fprintf(b, "static DESC: &[u8] = &[%s];\n\n", nums.String())
	if opt.Register {
		b.WriteString("std::thread_local! {\n    pub(crate) static SCHEMA: Schema = Schema::from_bytes(DESC).expect(\"embedded descriptor set\");\n}\n")
	} else {
		b.WriteString("std::thread_local! {\n    pub(crate) static SCHEMA: std::cell::RefCell<Option<Schema>> = std::cell::RefCell::new(None);\n}\n\n")
		b.WriteString("/// Loads the embedded set on this thread. With register=false every\n")
		b.WriteString("/// thread calls this before first use (encode panics loudly otherwise).\n")
		b.WriteString("pub fn init() {\n")
		b.WriteString("    SCHEMA.with(|s| {\n")
		b.WriteString("        *s.borrow_mut() = Some(Schema::from_bytes(DESC).expect(\"embedded descriptor set\"));\n")
		b.WriteString("    })\n")
		b.WriteString("}\n\n")
		b.WriteString("pub(crate) fn schema<R>(f: impl FnOnce(&Schema) -> R) -> R {\n")
		b.WriteString("    SCHEMA.with(|s| {\n")
		b.WriteString("        let s = s.borrow();\n")
		b.WriteString("        let s = s.as_ref().expect(\"call init() first (generated with register=false)\");\n")
		b.WriteString("        f(s)\n")
		b.WriteString("    })\n")
		b.WriteString("}\n")
	}
	if opt.Registry {
		b.WriteString("\n/// Every message in this file, for whole-schema pre-registration.\n")
		b.WriteString("pub const MESSAGE_NAMES: &[&str] = &[\n")
		for _, m := range f.Messages {
			fmt.Fprintf(b, "    %q,\n", m.Full)
		}
		b.WriteString("];\n\n")
		b.WriteString("/// Warms every message type on this thread, so later calls pay no lookup.\n")
		b.WriteString("pub fn preload() {\n")
		if opt.Register {
			b.WriteString("    SCHEMA.with(|s| {\n")
		} else {
			b.WriteString("    schema(|s| {\n")
		}
		b.WriteString("        for n in MESSAGE_NAMES {\n")
		b.WriteString("            s.message(n).expect(\"embedded type missing\");\n")
		b.WriteString("        }\n")
		b.WriteString("    })\n")
		b.WriteString("}\n")
	}
}

func rustEnumRoot(f *model.File, e *model.Enum) bool {
	for _, m := range f.Messages {
		if strings.HasPrefix(e.Full, m.Full+".") && !strings.Contains(strings.TrimPrefix(e.Full, m.Full+"."), ".") {
			return false
		}
	}
	return true
}

func rustModuleName(f *model.File) string {
	name := strings.ReplaceAll(base(f), "-", "_")
	return name + "_pbwire"
}

// rustMsgPath resolves a local message name to a path relative to cur.
// rustImpMod is the inline module name for a sibling import: the dep file
// lives next to this one, so #[path] resolves regardless of how this file
// itself was included. Sanitized like rustModuleName.
func rustImpMod(path string) string {
	name := strings.ReplaceAll(stem(path), "-", "_")
	return "_pbwire_imp_" + name
}

// rustForeign names a type from another file through its inline module.
func rustForeign(imp model.Import, local string) string {
	return rustImpMod(imp.Path) + "::" + strings.Join(strings.Split(local, "_"), "::")
}

// rustFieldPath renders a field's message type: the foreign path when the
// type lives in an import, the nesting-relative path otherwise.
func rustFieldPath(f *model.File, field model.Field, cur []string, byLocal map[string]*rustNode) string {
	if field.Message != "" && field.Import != "" {
		if imp := findImport(f, field.Import); imp != nil {
			return rustForeign(*imp, field.Message)
		}
	}
	return rustMsgPath(field.Message, cur, byLocal)
}

func rustMsgPath(local string, cur []string, byLocal map[string]*rustNode) string {
	if n, ok := byLocal[local]; ok {
		return rustRel(append(append([]string{}, n.segs...), n.self), cur)
	}
	return strings.ReplaceAll(local, "_", "")
}

// rustOneofs collects distinct oneof names in field order.
func rustOneofs(m *model.Message) []string {
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

func rustScalar(field model.Field) string {
	switch field.Kind {
	case protoreflect.Int32Kind, protoreflect.Sint32Kind, protoreflect.Sfixed32Kind,
		protoreflect.EnumKind:
		return "i32"
	case protoreflect.Uint32Kind, protoreflect.Fixed32Kind:
		return "u32"
	case protoreflect.Int64Kind, protoreflect.Sint64Kind, protoreflect.Sfixed64Kind:
		return "i64"
	case protoreflect.Uint64Kind, protoreflect.Fixed64Kind:
		return "u64"
	case protoreflect.FloatKind:
		return "f32"
	case protoreflect.DoubleKind:
		return "f64"
	case protoreflect.BoolKind:
		return "bool"
	case protoreflect.StringKind:
		return "String"
	case protoreflect.BytesKind:
		return "Vec<u8>"
	default:
		return "i64"
	}
}

// rustHashable mirrors prost-build: a struct gets Eq/Hash when no direct
// field is float or message-typed (prost never derives those through), and
// every oneof payload is itself hashable. Kind enums are hashable when all
// their members are. stack guards reference cycles (a cycle is never Eq).
func rustHashable(byLocal map[string]*rustNode, m *model.Message, stack map[string]bool) bool {
	for _, field := range m.Fields {
		if field.Skip != "" {
			continue
		}
		if field.Oneof == "" {
			if field.Message != "" {
				return false
			}
			if field.Kind == protoreflect.FloatKind || field.Kind == protoreflect.DoubleKind {
				return false
			}
			continue
		}
		switch field.Kind {
		case protoreflect.FloatKind, protoreflect.DoubleKind:
			return false
		}
		if field.Message == "" {
			continue
		}
		if stack[field.Message] {
			return false
		}
		n, ok := byLocal[field.Message]
		if !ok {
			continue
		}
		stack[field.Message] = true
		ok = rustHashable(byLocal, n.msg, stack)
		delete(stack, field.Message)
		if !ok {
			return false
		}
	}
	return true
}

// rustDerives mirrors prost: every struct gets Clone/PartialEq plus the Debug
// and Default that prost's Message derive provides; Eq/Hash via rustHashable.
func rustDerives(m *model.Message, byLocal map[string]*rustNode) string {
	if rustHashable(byLocal, m, map[string]bool{}) {
		return "Clone, PartialEq, Eq, Hash, Debug, Default"
	}
	return "Clone, PartialEq, Debug, Default"
}

func writeRustMessage(b *strings.Builder, f *model.File, n *rustNode, cur []string, byLocal map[string]*rustNode, g map[string]map[string]bool, opt Options) {
	m := n.msg
	oneofs := rustOneofs(m)
	fmt.Fprintf(b, "/// %s\n#[derive(%s)]\npub struct %s {\n", m.Full, rustDerives(m, byLocal), n.self)
	for _, field := range m.Fields {
		if field.Skip != "" {
			fmt.Fprintf(b, "    // skipped %s: %s\n", field.Name, field.Skip)
			continue
		}
		if field.Oneof != "" {
			continue
		}
		fmt.Fprintf(b, "    pub %s: %s, // %d\n", rustName(field.Name), rustFieldType(f, *m, field, cur, byLocal, g), field.Number)
	}
	for _, o := range oneofs {
		kind := rustCamel(o)
		mod := rustSnake(n.self)
		qkind := rustRel(append(append([]string{}, cur...), mod, kind), cur)
		fmt.Fprintf(b, "    pub %s: ::core::option::Option<%s>,\n", rustName(o), qkind)
	}
	b.WriteString("}\n\n")
	writeRustImpl(b, f, n, cur, byLocal, g, opt)
	if len(n.children) == 0 && len(n.enums) == 0 && len(oneofs) == 0 {
		return
	}
	mod := rustSnake(n.self)
	fmt.Fprintf(b, "/// Nested message and enum types in `%s`.\npub mod %s {\n", n.self, mod)
	sub := append(append([]string{}, cur...), mod)
	var inner strings.Builder
	inner.WriteString("#[allow(unused_imports)]\nuse super::*;\n\n")
	for _, c := range n.children {
		writeRustMessage(&inner, f, c, sub, byLocal, g, opt)
	}
	for _, e := range n.enums {
		writeRustEnum(&inner, e)
	}
	for _, o := range oneofs {
		writeRustOneof(&inner, f, m, o, sub, byLocal, g)
	}
	for _, line := range strings.Split(strings.TrimSuffix(inner.String(), "\n"), "\n") {
		if line == "" {
			b.WriteString("\n")
		} else {
			b.WriteString("    " + line + "\n")
		}
	}
	b.WriteString("}\n\n")
}

func rustFieldType(f *model.File, owner model.Message, field model.Field, cur []string, byLocal map[string]*rustNode, g map[string]map[string]bool) string {
	elem := rustScalar(field)
	if field.Message != "" {
		elem = rustFieldPath(f, field, cur, byLocal)
		if rustBoxed(g, owner.Name, field.Message) {
			elem = "Box<" + elem + ">"
		}
	}
	if field.Repeated {
		return "Vec<" + elem + ">"
	}
	if field.Message != "" || field.NeedsHas {
		return "Option<" + elem + ">"
	}
	return elem
}

// rustOneofDerives mirrors prost-build: Eq/Hash when every member payload is
// hashable (a message payload counts when its struct is hashable).
func rustOneofDerives(m *model.Message, o string, byLocal map[string]*rustNode) string {
	for _, field := range m.Fields {
		if field.Skip != "" || field.Oneof != o {
			continue
		}
		switch field.Kind {
		case protoreflect.FloatKind, protoreflect.DoubleKind:
			return "Clone, PartialEq, Debug"
		}
		if field.Message == "" {
			continue
		}
		n, ok := byLocal[field.Message]
		if !ok {
			continue
		}
		stack := map[string]bool{field.Message: true}
		if !rustHashable(byLocal, n.msg, stack) {
			return "Clone, PartialEq, Debug"
		}
	}
	return "Clone, PartialEq, Eq, Hash, Debug"
}

func writeRustOneof(b *strings.Builder, f *model.File, m *model.Message, o string, cur []string, byLocal map[string]*rustNode, g map[string]map[string]bool) {
	kind := rustCamel(o)
	fmt.Fprintf(b, "    #[derive(%s)]\n    pub enum %s {\n", rustOneofDerives(m, o, byLocal), kind)
	for _, field := range m.Fields {
		if field.Skip != "" || field.Oneof != o {
			continue
		}
		payload := rustScalar(field)
		if field.Message != "" {
			payload = rustFieldPath(f, field, cur, byLocal)
			if rustBoxed(g, m.Name, field.Message) {
				payload = "Box<" + payload + ">"
			}
		}
		fmt.Fprintf(b, "        %s(%s),\n", rustCamel(field.Name), payload)
	}
	b.WriteString("    }\n")
}

func writeRustEnum(b *strings.Builder, e *model.Enum) {
	fmt.Fprintf(b, "/// %s\n#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]\n#[repr(i32)]\npub enum %s {\n",
		e.Full, rustEnumName(e.Name))
	// Aliased values (allow_alias: several names, one number) keep the
	// first name per number, like prost-build; from_str_name still
	// accepts every original name.
	var order []int32
	seen := map[int32]bool{}
	for _, v := range e.Values {
		if !seen[v.Number] {
			seen[v.Number] = true
			order = append(order, v.Number)
		}
	}
	byNumber := map[int32]model.EnumValue{}
	for _, v := range e.Values {
		if _, ok := byNumber[v.Number]; !ok {
			byNumber[v.Number] = v
		}
	}
	for _, n := range order {
		v := byNumber[n]
		fmt.Fprintf(b, "    %s = %d,\n", rustVariant(e.Name, v.Name), v.Number)
	}
	b.WriteString("}\n")
	name := rustEnumName(e.Name)
	fmt.Fprintf(b, "impl %s {\n    /// String value of the enum field names used in the ProtoBuf definition.\n", name)
	fmt.Fprintf(b, "    pub fn as_str_name(&self) -> &'static str {\n        match self {\n")
	for _, n := range order {
		v := byNumber[n]
		fmt.Fprintf(b, "            Self::%s => %q,\n", rustVariant(e.Name, v.Name), v.Name)
	}
	b.WriteString("        }\n    }\n")
	fmt.Fprintf(b, "    /// Creates an enum from field names used in the ProtoBuf definition.\n")
	fmt.Fprintf(b, "    pub fn from_str_name(value: &str) -> ::core::option::Option<Self> {\n        match value {\n")
	for _, v := range e.Values {
		kept := byNumber[v.Number]
		fmt.Fprintf(b, "            %q => Some(Self::%s),\n", v.Name, rustVariant(e.Name, kept.Name))
	}
	b.WriteString("            _ => None,\n        }\n    }\n}\n\n")
}

func rustEnumName(local string) string {
	segs := strings.Split(local, "_")
	return segs[len(segs)-1]
}

func writeRustImpl(b *strings.Builder, f *model.File, n *rustNode, cur []string, byLocal map[string]*rustNode, g map[string]map[string]bool, opt Options) {
	m := n.msg
	fmt.Fprintf(b, "impl %s {\n    pub const FULL_NAME: &'static str = %q;\n\n", n.self, m.Full)
	fmt.Fprintf(b, "    pub fn encode_to_vec(&self) -> Vec<u8> {\n")
	if opt.Register {
		fmt.Fprintf(b, "        meta::SCHEMA.with(|s| {\n")
	} else {
		fmt.Fprintf(b, "        meta::schema(|s| {\n")
	}
	fmt.Fprintf(b, "            let t = s.message(Self::FULL_NAME).expect(\"generated model: embedded type missing\");\n")
	fmt.Fprintf(b, "            let mut msg = t.new_message();\n")
	fmt.Fprintf(b, "            self.encode_into(s, &mut msg).expect(\"generated model: encode failed\");\n")
	fmt.Fprintf(b, "            msg.encode().expect(\"generated model: encode failed\")\n")
	fmt.Fprintf(b, "        })\n    }\n\n")
	fmt.Fprintf(b, "    pub fn decode(bytes: &[u8]) -> Result<Self, DecodeError> {\n")
	if opt.Register {
		fmt.Fprintf(b, "        meta::SCHEMA.with(|s| {\n")
	} else {
		fmt.Fprintf(b, "        meta::schema(|s| {\n")
	}
	fmt.Fprintf(b, "            let t = s.message(Self::FULL_NAME)?;\n")
	fmt.Fprintf(b, "            let msg = t.decode(bytes)?;\n")
	fmt.Fprintf(b, "            Self::from_message(s, &msg)\n")
	fmt.Fprintf(b, "        })\n    }\n\n")

	fmt.Fprintf(b, "    pub(crate) fn encode_into(&self, schema: &Schema, msg: &mut Message<'_>) -> Result<(), DecodeError> {\n")
	fmt.Fprintf(b, "        let t = schema.message(Self::FULL_NAME)?;\n")
	for _, field := range m.Fields {
		if field.Skip != "" || field.Oneof != "" {
			continue
		}
		writeRustEncode(b, *m, field, cur, byLocal, g)
	}
	for _, o := range rustOneofs(m) {
		fmt.Fprintf(b, "        if let Some(k) = &self.%s {\n            match k {\n", rustName(o))
		sub := append(append([]string{}, cur...), rustSnake(n.self))
		qkind := rustRel(append(append([]string{}, sub...), rustCamel(o)), cur)
		for _, field := range m.Fields {
			if field.Skip != "" || field.Oneof != o {
				continue
			}
			variant := rustCamel(field.Name)
			if field.Message != "" {
				fmt.Fprintf(b, "                %s::%s(m) => { m.encode_into(schema, &mut msg.set_message(t.field(%q)?))?; }\n",
					qkind, variant, field.Name)
			} else {
				fmt.Fprintf(b, "                %s::%s(v) => { msg.%s(t.field(%q)?, %s); }\n",
					qkind, variant,
					rustSetter(field), field.Name, rustSetArg(field, "v"))
			}
		}
		b.WriteString("            }\n        }\n")
	}
	b.WriteString("        Ok(())\n    }\n\n")

	fmt.Fprintf(b, "    pub(crate) fn from_message(schema: &Schema, msg: &Message<'_>) -> Result<Self, DecodeError> {\n")
	fmt.Fprintf(b, "        let t = schema.message(Self::FULL_NAME)?;\n        let mut o = Self::default();\n")
	for _, field := range m.Fields {
		if field.Skip != "" {
			continue
		}
		if field.Oneof != "" {
			writeRustLoadOne(b, f, n, field, byLocal, g)
			continue
		}
		writeRustLoad(b, f, *m, field, cur, byLocal, g)
	}
	b.WriteString("        Ok(o)\n    }\n}\n\n")
}

func writeRustEncode(b *strings.Builder, owner model.Message, field model.Field, cur []string, byLocal map[string]*rustNode, g map[string]map[string]bool) {
	name := "self." + rustName(field.Name)
	fmt.Fprintf(b, "        if %s {\n", rustEncodeCond(field, name))
	switch {
	case field.Repeated:
		fmt.Fprintf(b, "            for v in &%s {\n", name)
		if field.Message != "" {
			fmt.Fprintf(b, "                v.encode_into(schema, &mut msg.set_message(t.field(%q)?))?;\n", field.Name)
		} else {
			fmt.Fprintf(b, "                msg.%s(t.field(%q)?, %s);\n", rustSetter(field), field.Name, rustSetArg(field, "v"))
		}
		b.WriteString("            }\n")
	case field.Message != "":
		fmt.Fprintf(b, "            if let Some(v) = &%s { v.encode_into(schema, &mut msg.set_message(t.field(%q)?))?; }\n", name, field.Name)
	case field.NeedsHas:
		fmt.Fprintf(b, "            if let Some(v) = &%s { msg.%s(t.field(%q)?, %s); }\n", name, rustSetter(field), field.Name, rustSetArg(field, "v"))
	default:
		fmt.Fprintf(b, "            msg.%s(t.field(%q)?, %s);\n", rustSetter(field), field.Name, rustSetArg(field, name))
	}
	b.WriteString("        }\n")
}

func rustEncodeCond(field model.Field, name string) string {
	if field.Required {
		return "true"
	}
	switch {
	case field.Repeated:
		return "!" + name + ".is_empty()"
	case field.Message != "":
		return name + ".is_some()"
	case field.NeedsHas:
		return name + ".is_some()"
	case field.Kind == protoreflect.BoolKind:
		return name
	case field.Kind == protoreflect.StringKind, field.Kind == protoreflect.BytesKind:
		return "!" + name + ".is_empty()"
	case field.Kind == protoreflect.FloatKind, field.Kind == protoreflect.DoubleKind:
		return name + " != 0.0"
	default:
		return name + " != 0"
	}
}

func writeRustLoad(b *strings.Builder, f *model.File, owner model.Message, field model.Field, cur []string, byLocal map[string]*rustNode, g map[string]map[string]bool) {
	name := "o." + rustName(field.Name)
	if field.Message != "" && rustBoxed(g, owner.Name, field.Message) {
		writeRustLoadBoxed(b, f, field, cur, byLocal)
		return
	}
	if field.Repeated {
		fmt.Fprintf(b, "        if let Some(v) = msg.value(t.field(%q)?) {\n", field.Name)
		fmt.Fprintf(b, "            for i in 0..v.len() {\n                if let Some(item) = v.at(i) {\n")
		if field.Message != "" {
			fmt.Fprintf(b, "                    if let Some(m) = item.as_message() { %s.push(%s::from_message(schema, &m)?); }\n",
				name, rustFieldPath(f, field, cur, byLocal))
		} else {
			fmt.Fprintf(b, "                    %s.push(%s);\n", name, rustElem(field, "item"))
		}
		b.WriteString("                }\n            }\n        }\n")
		return
	}
	if field.Message != "" {
		fmt.Fprintf(b, "        if let Some(m) = msg.get_message(t.field(%q)?) {\n", field.Name)
		fmt.Fprintf(b, "            %s = Some(%s::from_message(schema, &m)?);\n        }\n", name, rustFieldPath(f, field, cur, byLocal))
		return
	}
	if field.NeedsHas {
		fmt.Fprintf(b, "        if msg.has(t.field(%q)?) { %s = Some(%s); }\n", field.Name, name, rustOptValue(field))
		return
	}
	fmt.Fprintf(b, "        %s = %s;\n", name, rustValue(field, fmt.Sprintf("msg.%s(t.field(%q)?)", rustGetter(field), field.Name)))
}

// writeRustLoadBoxed loads a recursive field whose struct type is boxed.
func writeRustLoadBoxed(b *strings.Builder, f *model.File, field model.Field, cur []string, byLocal map[string]*rustNode) {
	name := "o." + rustName(field.Name)
	path := rustFieldPath(f, field, cur, byLocal)
	if field.Repeated {
		fmt.Fprintf(b, "        if let Some(v) = msg.value(t.field(%q)?) {\n", field.Name)
		fmt.Fprintf(b, "            for i in 0..v.len() {\n                if let Some(item) = v.at(i) {\n")
		fmt.Fprintf(b, "                    if let Some(m) = item.as_message() { %s.push(Box::new(%s::from_message(schema, &m)?)); }\n", name, path)
		b.WriteString("                }\n            }\n        }\n")
		return
	}
	fmt.Fprintf(b, "        if let Some(m) = msg.get_message(t.field(%q)?) {\n", field.Name)
	fmt.Fprintf(b, "            %s = Some(Box::new(%s::from_message(schema, &m)?));\n        }\n", name, path)
}

func writeRustLoadOne(b *strings.Builder, f *model.File, n *rustNode, field model.Field, byLocal map[string]*rustNode, g map[string]map[string]bool) {
	cur := append(append([]string{}, n.segs...), rustSnake(n.self))
	kind := rustCamel(field.Oneof)
	qkind := rustRel(append(append([]string{}, cur...), kind), n.segs)
	variant := rustCamel(field.Name)
	payload := rustOptPayload(f, field, n.segs, byLocal, *n.msg, g)
	fmt.Fprintf(b, "        if msg.has(t.field(%q)?) { o.%s = Some(%s::%s(%s)); }\n",
		field.Name, rustName(field.Oneof), qkind, variant, payload)
}

func rustOptPayload(f *model.File, field model.Field, cur []string, byLocal map[string]*rustNode, owner model.Message, g map[string]map[string]bool) string {
	if field.Message != "" {
		ctor := fmt.Sprintf("%s::from_message(schema, &msg.get_message(t.field(%q)?).expect(\"generated model: oneof member\"))?",
			rustFieldPath(f, field, cur, byLocal), field.Name)
		if rustBoxed(g, owner.Name, field.Message) {
			return "Box::new(" + ctor + ")"
		}
		return ctor
	}
	return rustOptValue(field)
}

func rustOptValue(field model.Field) string {
	read := fmt.Sprintf("msg.%s(t.field(%q)?)", rustGetter(field), field.Name)
	switch field.Kind {
	case protoreflect.Int32Kind, protoreflect.Sint32Kind, protoreflect.Sfixed32Kind,
		protoreflect.EnumKind:
		return read + " as i32"
	case protoreflect.Uint32Kind, protoreflect.Fixed32Kind:
		return read + " as u32"
	case protoreflect.Uint64Kind, protoreflect.Fixed64Kind:
		return read + " as u64"
	case protoreflect.StringKind:
		return read + ".unwrap_or(\"\").to_string()"
	case protoreflect.BytesKind:
		return read + ".unwrap_or(&[]).to_vec()"
	default:
		return read
	}
}

func rustSetter(field model.Field) string {
	switch field.Kind {
	case protoreflect.FloatKind:
		return "set_f32"
	case protoreflect.DoubleKind:
		return "set_f64"
	case protoreflect.BoolKind:
		return "set_bool"
	case protoreflect.StringKind:
		return "set_str"
	case protoreflect.BytesKind:
		return "set_bytes"
	default:
		return "set_i64"
	}
}

func rustSetArg(field model.Field, expr string) string {
	deref := expr
	if expr == "v" {
		switch field.Kind {
		case protoreflect.Int32Kind, protoreflect.Sint32Kind, protoreflect.Sfixed32Kind,
			protoreflect.Uint32Kind, protoreflect.Fixed32Kind, protoreflect.EnumKind,
			protoreflect.Int64Kind, protoreflect.Sint64Kind, protoreflect.Sfixed64Kind,
			protoreflect.Uint64Kind, protoreflect.Fixed64Kind,
			protoreflect.FloatKind, protoreflect.DoubleKind, protoreflect.BoolKind:
			deref = "*v"
		}
	}
	switch field.Kind {
	case protoreflect.Int32Kind, protoreflect.Sint32Kind, protoreflect.Sfixed32Kind,
		protoreflect.Uint32Kind, protoreflect.Fixed32Kind, protoreflect.EnumKind,
		protoreflect.Uint64Kind, protoreflect.Fixed64Kind:
		/* set_i64 takes every integer kind: u32 zero-extends, u64 wraps
		 * with its bits intact (the core masks by field type). */
		return deref + " as i64"
	case protoreflect.StringKind:
		if expr == "v" {
			return "v.as_str()"
		}
		return expr + ".as_str()"
	case protoreflect.BytesKind:
		if expr == "v" {
			return "v.as_slice()"
		}
		return expr + ".as_slice()"
	default:
		return deref
	}
}

func rustGetter(field model.Field) string {
	switch field.Kind {
	case protoreflect.FloatKind:
		return "get_f32"
	case protoreflect.DoubleKind:
		return "get_f64"
	case protoreflect.BoolKind:
		return "get_bool"
	case protoreflect.StringKind:
		return "get_str"
	case protoreflect.BytesKind:
		return "get_bytes"
	default:
		return "get_i64"
	}
}

func rustValue(field model.Field, expr string) string {
	switch field.Kind {
	case protoreflect.Int32Kind, protoreflect.Sint32Kind, protoreflect.Sfixed32Kind,
		protoreflect.EnumKind:
		return expr + " as i32"
	case protoreflect.Uint32Kind, protoreflect.Fixed32Kind:
		return expr + " as u32"
	case protoreflect.Uint64Kind, protoreflect.Fixed64Kind:
		return expr + " as u64"
	case protoreflect.StringKind:
		return expr + ".unwrap_or(\"\").to_string()"
	case protoreflect.BytesKind:
		return expr + ".unwrap_or(&[]).to_vec()"
	default:
		return expr
	}
}

func rustElem(field model.Field, expr string) string {
	switch field.Kind {
	case protoreflect.Int32Kind, protoreflect.Sint32Kind, protoreflect.Sfixed32Kind,
		protoreflect.EnumKind:
		return expr + ".as_i64() as i32"
	case protoreflect.Uint32Kind, protoreflect.Fixed32Kind:
		return expr + ".as_i64() as u32"
	case protoreflect.Int64Kind, protoreflect.Sint64Kind, protoreflect.Sfixed64Kind:
		return expr + ".as_i64()"
	case protoreflect.Uint64Kind, protoreflect.Fixed64Kind:
		return expr + ".as_i64() as u64"
	case protoreflect.StringKind:
		return expr + ".as_str().unwrap_or(\"\").to_string()"
	case protoreflect.BytesKind:
		return expr + ".as_bytes().unwrap_or(&[]).to_vec()"
	case protoreflect.BoolKind:
		return expr + ".as_bool()"
	case protoreflect.FloatKind:
		return expr + ".as_f32()"
	case protoreflect.DoubleKind:
		return expr + ".as_f64()"
	default:
		return expr + ".as_i64()"
	}
}

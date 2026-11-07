package gen

// Dart emitter: protobuf-dart-shaped classes over the dynamic API.
//
// The surface mirrors what protoc-gen-dart would generate for the same .proto
// (create/writeToBuffer/fromBuffer, hasX/clearX/ensureX, PbList-style repeated
// fields, whichX for oneofs, ProtobufEnum enums, Int64 for 64-bit fields), but
// every codec call goes through the binding's existing dynamic API, so the wire
// format stays in the C core. The FileDescriptorSet rides inside the file
// (base64), so there is no .desc file at use time and no GeneratedMessage:
// only package:protobuf's PbList-free List surface (plain List fields) and
// ProtobufEnum base are reused, never mergeFromBuffer.

import (
	"fmt"
	"strings"

	"pbwire-codegen/internal/model"

	"google.golang.org/protobuf/reflect/protoreflect"
)

var dartKeywords = map[string]bool{
	"abstract": true, "as": true, "assert": true, "async": true, "await": true,
	"base": true, "break": true, "case": true, "catch": true, "class": true,
	"const": true, "continue": true, "covariant": true, "default": true,
	"deferred": true, "do": true, "dynamic": true, "else": true, "enum": true,
	"export": true, "extends": true, "extension": true, "external": true,
	"factory": true, "false": true, "final": true, "finally": true, "for": true,
	"function": true, "get": true, "hide": true, "if": true, "implements": true,
	"import": true, "in": true, "interface": true, "is": true, "late": true,
	"library": true, "mixin": true, "new": true, "null": true, "on": true,
	"operator": true, "part": true, "required": true, "rethrow": true,
	"return": true, "sealed": true, "set": true, "show": true, "static": true,
	"super": true, "switch": true, "sync": true, "this": true, "throw": true,
	"true": true, "try": true, "type": true, "typedef": true, "var": true,
	"void": true, "when": true, "while": true, "with": true, "yield": true,
}

func dartName(name string) string {
	if dartKeywords[name] {
		return name + "_"
	}
	return name
}

// dartOneofs returns the distinct oneof names of a message in field order.
func dartOneofs(m model.Message) []string {
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

// dartIs64 reports whether a field holds a 64-bit integer: the facade types
// those as fixnum Int64, exactly like protoc-gen-dart.
func dartIs64(field model.Field) bool {
	switch field.Kind {
	case protoreflect.Int64Kind, protoreflect.Sint64Kind, protoreflect.Sfixed64Kind,
		protoreflect.Uint64Kind, protoreflect.Fixed64Kind:
		return true
	default:
		return false
	}
}

// dartType is the facade field type: int/Int64/double/bool/String/Uint8List,
// the enum class, or the message class. Repeated fields wrap in List.
func dartType(field model.Field) string {
	var t string
	switch {
	case field.Message != "":
		t = field.Message
	case field.Enum != "":
		t = field.Enum
	default:
		switch field.Kind {
		case protoreflect.FloatKind, protoreflect.DoubleKind:
			t = "double"
		case protoreflect.BoolKind:
			t = "bool"
		case protoreflect.StringKind:
			t = "String"
		case protoreflect.BytesKind:
			t = "Uint8List"
		default:
			if dartIs64(field) {
				t = "Int64"
			} else {
				t = "int"
			}
		}
	}
	if field.Repeated {
		return "List<" + t + ">"
	}
	return t
}

// dartDefault is the implicit default literal: first enum value for enums,
// empty for bytes (protoc-gen-dart returns an empty list there).
// dartIsEnum reports whether a field uses a facade enum class: only enums
// defined in the same file get one (external enums stay plain ints, like
// the wire, so unknown values survive the round trip).
func dartIsEnum(field model.Field) bool { return field.Enum != "" }

func dartDefault(f *model.File, field model.Field) string {
	if dartIsEnum(field) {
		for _, e := range f.Enums {
			if e.Name == field.Enum && len(e.Values) > 0 {
				return field.Enum + "." + e.Values[0].Name
			}
		}
		return field.Enum + ".values.first"
	}
	switch field.Kind {
	case protoreflect.FloatKind, protoreflect.DoubleKind:
		return "0.0"
	case protoreflect.BoolKind:
		return "false"
	case protoreflect.StringKind:
		return "''"
	case protoreflect.BytesKind:
		return "Uint8List(0)"
	default:
		if dartIs64(field) {
			return "Int64.ZERO"
		}
		return "0"
	}
}

func dartNeedsFixnum(f *model.File) bool {
	for _, m := range f.Messages {
		for _, field := range m.Fields {
			if field.Skip == "" && dartIs64(field) {
				return true
			}
		}
	}
	return false
}

// dartChunks splits a long base64 literal into adjacent string literals so no
// generated line is huge (Dart concatenates adjacent literals, even in const).
func dartChunks(s string) string {
	var parts []string
	for len(s) > 0 {
		n := 76
		if n > len(s) {
			n = len(s)
		}
		parts = append(parts, fmt.Sprintf("%q", s[:n]))
		s = s[n:]
	}
	return strings.Join(parts, "\n    ")
}

// dartImpPrefix is the import prefix for one sibling model file: generated
// classes share the file namespace with the pbwire runtime, so a proto message
// named Value would make every reference to it ambiguous.
func dartImpPrefix(path string) string {
	var b strings.Builder
	b.WriteString("_imp_")
	for _, r := range stem(path) {
		if (r >= 'a' && r <= 'z') || (r >= 'A' && r <= 'Z') || (r >= '0' && r <= '9') {
			b.WriteRune(r)
		} else {
			b.WriteRune('_')
		}
	}
	return b.String()
}

func dartFiles(f *model.File, opt Options) ([]File, error) {
	// Models and schema part ways: facades in one library, the embedded
	// descriptor set plus its loader in another. model=false leaves the
	// schema library for dynamic-only users. Public members carry the file
	// stem: libraries share one namespace per package when imported together.
	fileStem := base(f)
	var files []File
	if opt.Model {
		// Sibling models are imported prefixed, like protoc-gen-dart: the local
		// name space already holds the pbwire runtime, so a foreign message
		// named Value would otherwise be ambiguous.
		qualifyFields(f, func(imp model.Import, local string) string {
			return dartImpPrefix(imp.Path) + "." + local
		})
		var b strings.Builder
		b.WriteString(header(f, "Generated facades over the pbwire dynamic API, protobuf-dart shape."))
		b.WriteString("import 'dart:typed_data';\n\n")
		if dartNeedsFixnum(f) {
			b.WriteString("import 'package:fixnum/fixnum.dart';\n")
		}
		if len(f.Enums) > 0 {
			b.WriteString("import 'package:protobuf/protobuf.dart' as $pb;\n")
		}
		b.WriteString("import 'package:pbwire/pbwire.dart';\n")
		for _, imp := range f.Imports {
			fmt.Fprintf(&b, "import '%s.dart' as %s;\n", stem(imp.Path), dartImpPrefix(imp.Path))
		}
		fmt.Fprintf(&b, "import '%s_meta.dart';\n\n", fileStem)
		for _, e := range f.Enums {
			writeDartEnum(&b, e)
		}
		for _, m := range f.Messages {
			for _, o := range dartOneofs(m) {
				writeDartOneofKind(&b, m, o)
			}
			writeDartMessage(&b, f, m, fileStem)
		}
		files = append(files, File{Name: fileStem + ".dart", Content: b.String()})
	}
	var meta strings.Builder
	writeDartMeta(&meta, f, opt, fileStem)
	files = append(files, File{Name: fileStem + "_meta.dart", Content: meta.String()})
	return files, nil
}

// writeDartMeta emits the schema library: the embedded descriptor set plus
// an explicit loader. register=true keeps the lazy per-isolate load (as
// before) with an override slot; otherwise initSchema (embedded bytes) or
// useSchema (external) must run first, loudly otherwise. registry adds the
// whole-file message list plus a preload that warms every type.
func writeDartMeta(b *strings.Builder, f *model.File, opt Options, stem string) {
	b.WriteString(header(f, "Embedded descriptor set plus its loader: the schema side of the generated model."))
	b.WriteString("import 'dart:convert';\n\n")
	b.WriteString("import 'package:pbwire/pbwire.dart';\n\n")
	fmt.Fprintf(b, "/// The FileDescriptorSet for %s, so messages encode with no .desc file.\n", f.Path)
	b.WriteString("const _descB64 =\n    " + dartChunks(opt.DescSetB64) + ";\n\n")
	b.WriteString("Schema? _schema;\n\nSchema? _schemaOverride;\n\n")
	fmt.Fprintf(b, "/// Loads the embedded set, or the given bytes for the descriptor mode.\n")
	fmt.Fprintf(b, "void %s_initSchema([List<int>? bytes]) {\n", stem)
	b.WriteString("  _schema = Schema.fromBytes(bytes ?? base64Decode(_descB64));\n")
	b.WriteString("}\n\n")
	fmt.Fprintf(b, "/// Registers an externally loaded schema: models encode/decode through it instead of the embedded set. A nil call clears the override.\n")
	fmt.Fprintf(b, "void %s_useSchema(Schema? s) {\n", stem)
	b.WriteString("  _schemaOverride = s;\n")
	b.WriteString("}\n\n")
	if opt.Register {
		fmt.Fprintf(b, "/// The embedded schema, loaded once per isolate.\n")
		fmt.Fprintf(b, "Schema get %s_schema => _schemaOverride ?? (_schema ??= Schema.fromBytes(base64Decode(_descB64)));\n", stem)
	} else {
		fmt.Fprintf(b, "/// The loaded schema, loudly unless initSchema ran.\n")
		fmt.Fprintf(b, "Schema get %s_schema {\n", stem)
		b.WriteString("  final o = _schemaOverride;\n")
		b.WriteString("  if (o != null) return o;\n")
		b.WriteString("  final s = _schema;\n")
		fmt.Fprintf(b, "  if (s == null) throw StateError('call %s_initSchema() first (generated with register=false)');\n", stem)
		b.WriteString("  return s;\n")
		b.WriteString("}\n")
	}
	if opt.Registry {
		fmt.Fprintf(b, "\n/// Every message in this file, for whole-schema pre-registration.\n")
		fmt.Fprintf(b, "const %s_messageNames = <String>[\n", stem)
		for _, m := range f.Messages {
			fmt.Fprintf(b, "  %q,\n", m.Full)
		}
		b.WriteString("];\n\n")
		fmt.Fprintf(b, "/// Warms every message type, so later calls pay no lookup.\n")
		fmt.Fprintf(b, "void %s_preload() {\n", stem)
		fmt.Fprintf(b, "  final s = %s_schema;\n", stem)
		fmt.Fprintf(b, "  for (final n in %s_messageNames) {\n", stem)
		b.WriteString("    s.message(n);\n")
		b.WriteString("  }\n")
		b.WriteString("}\n")
	}
}

// dartEnumMax is the highest value number: valueOf builds a by-value list.
func dartEnumMax(e model.Enum) int32 {
	var max int32
	for _, v := range e.Values {
		if v.Number > max {
			max = v.Number
		}
	}
	return max
}

func writeDartEnum(b *strings.Builder, e model.Enum) {
	fmt.Fprintf(b, "/// %s\nclass %s extends $pb.ProtobufEnum {\n", e.Full, e.Name)
	for _, v := range e.Values {
		fmt.Fprintf(b, "  static const %s %s = %s._(%d, %q);\n", e.Name, v.Name, e.Name, v.Number, v.Name)
	}
	b.WriteString("\n  static const List<" + e.Name + "> values = <" + e.Name + ">[\n")
	seen := map[int32]bool{}
	for _, v := range e.Values {
		if seen[v.Number] {
			continue // allow_alias: first name wins, like protoc-gen-dart
		}
		seen[v.Number] = true
		fmt.Fprintf(b, "    %s,\n", v.Name)
	}
	b.WriteString("  ];\n\n")
	fmt.Fprintf(b, "  static final List<%s?> _byValue = $pb.ProtobufEnum.$_initByValueList(values, %d);\n", e.Name, dartEnumMax(e))
	fmt.Fprintf(b, "  static %s? valueOf(int value) =>\n", e.Name)
	fmt.Fprintf(b, "      value < 0 || value >= _byValue.length ? null : _byValue[value];\n\n")
	fmt.Fprintf(b, "  const %s._(super.value, super.name);\n}\n\n", e.Name)
}

func writeDartOneofKind(b *strings.Builder, m model.Message, o string) {
	fmt.Fprintf(b, "/// Possible cases for the %q oneof of %s.\n", o, m.Full)
	fmt.Fprintf(b, "enum %s_%s {\n", m.Name, model.Pascal(o))
	for _, field := range m.Fields {
		if field.Skip != "" || field.Oneof != o {
			continue
		}
		fmt.Fprintf(b, "  %s,\n", dartName(field.Lower))
	}
	b.WriteString("  notSet,\n}\n\n")
}

func writeDartMessage(b *strings.Builder, f *model.File, m model.Message, stem string) {
	fmt.Fprintf(b, "/// %s\nclass %s {\n", m.Full, m.Name)
	writeDartCtor(b, f, m)
	for _, field := range m.Fields {
		if field.Skip != "" {
			fmt.Fprintf(b, "\n  // skipped %s: %s\n", field.Name, field.Skip)
			continue
		}
		writeDartField(b, f, m, field)
	}
	writeDartWhich(b, m)
	writeDartCodec(b, f, m, stem)
	b.WriteString("}\n\n")
}

// writeDartCtor emits the default constructor, create(), the named-parameter
// factory (protoc-gen-dart shape) and fromBuffer.
func writeDartCtor(b *strings.Builder, f *model.File, m model.Message) {
	// The generative constructor is private (protoc-gen-dart shape): the
	// same-named factory below would otherwise call itself.
	fmt.Fprintf(b, "  %s._();\n\n", m.Name)
	fmt.Fprintf(b, "  static %s create() => %s._();\n\n", m.Name, m.Name)
	// No named parameters at all (empty or all skipped): `factory M({})`
	// is a syntax error.
	nparams := 0
	for _, field := range m.Fields {
		if field.Skip == "" {
			nparams++
		}
	}
	if nparams == 0 {
		fmt.Fprintf(b, "  factory %s() => %s._();\n\n", m.Name, m.Name)
	} else {
		b.WriteString("  factory " + m.Name + "({")
		var params []string
		for _, field := range m.Fields {
			if field.Skip != "" {
				continue
			}
			// Every parameter is nullable: null means "leave the default".
			params = append(params, dartType(field)+"? "+dartName(field.Lower))
		}
		b.WriteString(strings.Join(params, ", "))
		b.WriteString("}) {\n    final o = " + m.Name + "._();\n")
		for _, field := range m.Fields {
			if field.Skip != "" {
				continue
			}
			name := dartName(field.Lower)
			if field.Repeated {
				fmt.Fprintf(b, "    if (%s != null) o.%s.addAll(%s);\n", name, name, name)
				continue
			}
			fmt.Fprintf(b, "    if (%s != null) o.%s = %s;\n", name, name, name)
		}
		b.WriteString("    return o;\n  }\n\n")
	}
	fmt.Fprintf(b, "  factory %s.fromBuffer(List<int> wire) => %s._()..mergeFromBuffer(wire);\n\n", m.Name, m.Name)
	fmt.Fprintf(b, "  static %s? _defaultInstance;\n", m.Name)
	fmt.Fprintf(b, "  static %s getDefault() => _defaultInstance ??= %s._();\n\n", m.Name, m.Name)
}

// dartBacking is the private backing field name.
func dartBacking(field model.Field) string { return "_" + dartName(field.Lower) }

// writeDartField emits one field plus its has/clear (and ensure for messages).
func writeDartField(b *strings.Builder, f *model.File, m model.Message, field model.Field) {
	_ = m
	name := dartName(field.Lower)
	pascal := field.Pascal
	t := dartType(field)
	def := dartDefault(f, field)
	switch {
	case field.Repeated:
		fmt.Fprintf(b, "\n  final %s %s = <%s>[]; // %s = %d\n", t, name, strings.TrimPrefix(strings.TrimSuffix(t, ">"), "List<"), field.Name, field.Number)
	case field.Message != "":
		back := dartBacking(field)
		fmt.Fprintf(b, "\n  %s? %s; // %s = %d\n", t, back, field.Name, field.Number)
		fmt.Fprintf(b, "  %s get %s => %s ?? %s.getDefault();\n", t, name, back, t)
		fmt.Fprintf(b, "  set %s(%s v) { %s }\n", name, t, dartOneofClear(field, back))
		fmt.Fprintf(b, "  bool has%s() => %s;\n", pascal, dartHas(m.Name, field, back))
		fmt.Fprintf(b, "  void clear%s() { %s }\n", pascal, dartClear(field, back))
		fmt.Fprintf(b, "  %s ensure%s() %s\n", t, pascal, dartEnsure(field, back, t))
	case field.Oneof != "":
		back := dartBacking(field)
		fmt.Fprintf(b, "\n  %s? %s; // %s = %d\n", t, back, field.Name, field.Number)
		fmt.Fprintf(b, "  %s get %s => %s ?? %s;\n", t, name, back, def)
		fmt.Fprintf(b, "  set %s(%s v) { %s }\n", name, t, dartOneofClear(field, back))
		fmt.Fprintf(b, "  bool has%s() => %s;\n", pascal, dartHas(m.Name, field, back))
		fmt.Fprintf(b, "  void clear%s() { %s }\n", pascal, dartClear(field, back))
		if field.Message != "" {
			fmt.Fprintf(b, "  %s ensure%s() %s\n", t, pascal, dartEnsure(field, back, t))
		}
	case field.NeedsHas:
		back := dartBacking(field)
		flag := "_has" + pascal
		fmt.Fprintf(b, "\n  %s %s = %s; // %s = %d\n", t, back, def, field.Name, field.Number)
		fmt.Fprintf(b, "  %s get %s => %s;\n", t, name, back)
		fmt.Fprintf(b, "  set %s(%s v) { %s = v; %s = true; }\n", name, t, back, flag)
		fmt.Fprintf(b, "  bool _%s = false;\n", flag[1:])
		fmt.Fprintf(b, "  bool has%s() => %s;\n", pascal, flag)
		fmt.Fprintf(b, "  void clear%s() { %s = %s; %s = false; }\n", pascal, back, def, flag)
	default:
		fmt.Fprintf(b, "\n  %s %s = %s; // %s = %d\n", t, name, def, field.Name, field.Number)
		fmt.Fprintf(b, "  bool has%s() => %s;\n", pascal, dartImplicitHas(field, name))
		fmt.Fprintf(b, "  void clear%s() { %s = %s; }\n", pascal, name, def)
	}
}

// dartOneofClear is the setter body: oneof members clear the group first.
func dartOneofClear(field model.Field, back string) string {
	if field.Oneof != "" {
		return fmt.Sprintf("clear%s(); %s = v;", model.Pascal(field.Oneof), back)
	}
	return back + " = v;"
}

// dartCtor is the empty-instance expression for a message field: the
// public create() when foreign (the ._() constructor is library-private),
// the private ._() otherwise.
func dartCtor(field model.Field) string {
	if field.Import != "" {
		return field.Message + ".create()"
	}
	return field.Message + "._()"
}

// dartEnsure is the ensureX implementation for message fields: a block body
// for oneof members (setters clear the group first), an arrow otherwise.
func dartEnsure(field model.Field, back, t string) string {
	if field.Oneof != "" {
		return fmt.Sprintf("{ clear%s(); return %s ??= %s; }", model.Pascal(field.Oneof), back, dartCtor(field))
	}
	return "=> " + back + " ??= " + dartCtor(field) + ";"
}

// dartHas is the hasX body for backed (message / oneof / optional) fields.
func dartHas(owner string, field model.Field, back string) string {
	if field.Oneof != "" {
		return fmt.Sprintf("which%s() == %s_%s.%s", model.Pascal(field.Oneof), owner, model.Pascal(field.Oneof), dartName(field.Lower))
	}
	return back + " != null"
}

// dartClear is the clearX body for backed fields.
func dartClear(field model.Field, back string) string {
	if field.Oneof != "" {
		return fmt.Sprintf("if (%s != null) { %s = null; }", back, back)
	}
	return back + " = null;"
}

// dartImplicitHas is the hasX body for proto3 implicit fields: presence is
// the wire presence, so has means non-default.
func dartImplicitHas(field model.Field, name string) string {
	switch field.Kind {
	case protoreflect.BoolKind:
		return name
	case protoreflect.StringKind, protoreflect.BytesKind:
		return name + ".isNotEmpty"
	case protoreflect.EnumKind:
		if dartIsEnum(field) {
			return name + ".value != 0"
		}
		return name + " != 0"
	default:
		if dartIs64(field) {
			return name + " != Int64.ZERO"
		}
		if field.Kind == protoreflect.FloatKind || field.Kind == protoreflect.DoubleKind {
			return name + " != 0.0"
		}
		return name + " != 0"
	}
}

// writeDartWhich emits whichX/clearX for every oneof of the message.
func writeDartWhich(b *strings.Builder, m model.Message) {
	for _, o := range dartOneofs(m) {
		pascal := model.Pascal(o)
		kind := m.Name + "_" + pascal
		fmt.Fprintf(b, "\n  %s which%s() {\n", kind, pascal)
		for _, field := range m.Fields {
			if field.Skip != "" || field.Oneof != o {
				continue
			}
			fmt.Fprintf(b, "    if (%s != null) return %s.%s;\n", dartBacking(field), kind, dartName(field.Lower))
		}
		fmt.Fprintf(b, "    return %s.notSet;\n  }\n", kind)
		fmt.Fprintf(b, "  void clear%s() {\n", pascal)
		for _, field := range m.Fields {
			if field.Skip != "" || field.Oneof != o {
				continue
			}
			fmt.Fprintf(b, "    %s = null;\n", dartBacking(field))
		}
		b.WriteString("  }\n")
	}
}

func writeDartCodec(b *strings.Builder, f *model.File, m model.Message, stem string) {
	fmt.Fprintf(b, "\n  Uint8List writeToBuffer() {\n")
	fmt.Fprintf(b, "    final msg = %s_schema.message(%q).newMessage();\n", stem, m.Full)
	b.WriteString("    try {\n      encodeInto(msg);\n      return msg.encode();\n    } finally {\n      msg.close();\n    }\n  }\n\n")
	b.WriteString("  void mergeFromBuffer(List<int> wire) {\n")
	fmt.Fprintf(b, "    final msg = %s_schema.message(%q).decode(wire);\n", stem, m.Full)
	b.WriteString("    try {\n      loadFrom(msg);\n    } finally {\n      msg.close();\n    }\n  }\n\n")
	b.WriteString("  void encodeInto(Message msg) {\n")
	for _, field := range m.Fields {
		if field.Skip != "" {
			continue
		}
		writeDartEncode(b, f, field)
	}
	b.WriteString("  }\n\n")
	b.WriteString("  void loadFrom(Message msg) {\n")
	for _, field := range m.Fields {
		if field.Skip != "" {
			continue
		}
		writeDartLoad(b, f, field)
	}
	b.WriteString("  }\n")
}

func writeDartEncode(b *strings.Builder, f *model.File, field model.Field) {
	_ = f
	name := dartName(field.Lower)
	key := fmt.Sprintf("%q", field.Name)
	enc := func(indent, dst, src string) {
		switch field.Kind {
		case protoreflect.FloatKind:
			fmt.Fprintf(b, "%s%s.setFloat(%s, %s);\n", indent, dst, key, src)
		case protoreflect.DoubleKind:
			fmt.Fprintf(b, "%s%s.setDouble(%s, %s);\n", indent, dst, key, src)
		case protoreflect.BoolKind:
			fmt.Fprintf(b, "%s%s.setBool(%s, %s);\n", indent, dst, key, src)
		case protoreflect.StringKind:
			fmt.Fprintf(b, "%s%s.setString(%s, %s);\n", indent, dst, key, src)
		case protoreflect.BytesKind:
			fmt.Fprintf(b, "%s%s.setBytes(%s, %s);\n", indent, dst, key, src)
		default:
			arg := src
			if dartIs64(field) {
				arg = src + ".toInt()"
			} else if dartIsEnum(field) {
				arg = src + ".value"
			}
			fmt.Fprintf(b, "%s%s.setInt(%s, %s);\n", indent, dst, key, arg)
		}
	}
	switch {
	case field.Repeated:
		fmt.Fprintf(b, "    if (%s.isNotEmpty) {\n      for (final v in %s) {\n", name, name)
		if field.Message != "" {
			fmt.Fprintf(b, "        v.encodeInto(msg.setMessage(%s));\n", key)
		} else {
			enc("        ", "msg", "v")
		}
		b.WriteString("      }\n    }\n")
	case field.Message != "":
		back := dartBacking(field)
		fmt.Fprintf(b, "    if (%s != null) %s!.encodeInto(msg.setMessage(%s));\n", back, back, key)
	case field.Oneof != "":
		back := dartBacking(field)
		b.WriteString("    if (" + back + " != null) {\n")
		if field.Message != "" {
			fmt.Fprintf(b, "      %s!.encodeInto(msg.setMessage(%s));\n", back, key)
		} else {
			enc("      ", "msg", back+"!")
		}
		b.WriteString("    }\n")
	case field.NeedsHas:
		flag := "_has" + field.Pascal
		if field.Message != "" {
			back := dartBacking(field)
			fmt.Fprintf(b, "    if (%s != null) %s!.encodeInto(msg.setMessage(%s));\n", back, back, key)
		} else {
			fmt.Fprintf(b, "    if (%s) {\n", flag)
			enc("      ", "msg", dartBacking(field))
			b.WriteString("    }\n")
		}
	default:
		fmt.Fprintf(b, "    if (%s) {\n", dartImplicitHas(field, name))
		enc("      ", "msg", name)
		b.WriteString("    }\n")
	}
}

func writeDartLoad(b *strings.Builder, f *model.File, field model.Field) {
	name := dartName(field.Lower)
	key := fmt.Sprintf("%q", field.Name)
	val := dartReadValue(f, field, key)
	switch {
	case field.Repeated:
		fmt.Fprintf(b, "    for (var i = 0; i < msg.count(%s); i++) {\n", key)
		b.WriteString("      final v = msg.at(" + key + ", i);\n      if (v == null) continue;\n")
		if field.Message != "" {
			fmt.Fprintf(b, "      final sub = v.asMessage();\n      if (sub != null) %s.add(%s..loadFrom(sub));\n", name, dartCtor(field))
		} else {
			fmt.Fprintf(b, "      %s.add(%s);\n", name, dartElem(f, field, "v"))
		}
		b.WriteString("    }\n")
	case field.Message != "":
		back := dartBacking(field)
		fmt.Fprintf(b, "    final sub%s = msg.getMessage(%s);\n", field.Pascal, key)
		if field.Oneof != "" {
			fmt.Fprintf(b, "    if (sub%s != null) %s = %s..loadFrom(sub%s);\n", field.Pascal, name, dartCtor(field), field.Pascal)
		} else {
			fmt.Fprintf(b, "    if (sub%s != null) (%s ??= %s).loadFrom(sub%s);\n", field.Pascal, back, dartCtor(field), field.Pascal)
		}
	case field.Oneof != "":
		fmt.Fprintf(b, "    if (msg.has(%s)) %s = %s;\n", key, name, val)
	case field.NeedsHas:
		fmt.Fprintf(b, "    if (msg.has(%s)) %s = %s;\n", key, name, val)
	default:
		fmt.Fprintf(b, "    %s = %s;\n", name, val)
	}
}

// dartReadValue is the decode expression for one singular scalar field.
func dartReadValue(f *model.File, field model.Field, key string) string {
	switch field.Kind {
	case protoreflect.FloatKind:
		return "msg.getFloat(" + key + ")"
	case protoreflect.DoubleKind:
		return "msg.getDouble(" + key + ")"
	case protoreflect.BoolKind:
		return "msg.getBool(" + key + ")"
	case protoreflect.StringKind:
		return "msg.getString(" + key + ") ?? ''"
	case protoreflect.BytesKind:
		return "msg.getBytes(" + key + ") ?? Uint8List(0)"
	default:
		raw := "msg.getInt(" + key + ")"
		if dartSigned32(field) {
			raw += ".toSigned(32)"
		}
		switch {
		case dartIs64(field):
			return "Int64(" + raw + ")"
		case dartIsEnum(field):
			return dartEnumRead(f, field, raw)
		default:
			return raw
		}
	}
}

func dartEnumRead(f *model.File, field model.Field, raw string) string {
	def := dartDefault(f, field)
	return field.Enum + ".valueOf(" + raw + ") ?? " + def
}

// dartElem converts one repeated Value to the facade element type.
func dartElem(f *model.File, field model.Field, expr string) string {
	switch field.Kind {
	case protoreflect.FloatKind:
		return expr + ".asFloat()"
	case protoreflect.DoubleKind:
		return expr + ".asDouble()"
	case protoreflect.BoolKind:
		return expr + ".asBool()"
	case protoreflect.StringKind:
		return expr + ".asString()!"
	case protoreflect.BytesKind:
		return expr + ".asBytes()!"
	default:
		raw := expr + ".asInt()"
		if dartSigned32(field) {
			raw += ".toSigned(32)"
		}
		if dartIs64(field) {
			return "Int64(" + raw + ")"
		}
		if dartIsEnum(field) {
			return "(" + field.Enum + ".valueOf(" + raw + ") ?? " + dartDefault(f, field) + ")"
		}
		return raw
	}
}

// dartSigned32 reports whether a decoded value arrives as an unsigned 64-bit
// int for a signed 32-bit field: Dart has no int32, so the facade clamps.
func dartSigned32(field model.Field) bool {
	switch field.Kind {
	case protoreflect.Int32Kind, protoreflect.Sint32Kind, protoreflect.Sfixed32Kind, protoreflect.EnumKind:
		return true
	default:
		return false
	}
}

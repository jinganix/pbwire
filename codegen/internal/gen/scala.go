package gen

import (
	"fmt"
	"strings"

	"pbwire-codegen/internal/model"

	"google.golang.org/protobuf/reflect/protoreflect"
)

// scalaFiles emits one Scala file: case classes over the Java runtime (the
// same runtime Kotlin reuses: Schema/Message in the default package).
// The descriptor set travels embedded as base64, so no .desc file is needed
// at use time. Oneof members are plain Options.
func scalaFiles(f *model.File, opt Options) ([]File, error) {
	// Models and schema part ways: case classes in one file, the embedded
	// descriptor set plus its loader in the other. model=false leaves the
	// schema file for dynamic-only users.
	var files []File
	if opt.Model {
		var b strings.Builder
		b.WriteString(header(f, "Case classes over the Java runtime (Schema/Message, no codegen at use time)."))
		b.WriteString("import java.util.Base64\n\n")
		b.WriteString("object " + base(f) + "Gen {\n")
		for _, e := range f.Enums {
			fmt.Fprintf(&b, "  // %s\n", e.Full)
			fmt.Fprintf(&b, "  object %s {\n", className(f, e.Name))
			for _, v := range e.Values {
				fmt.Fprintf(&b, "    val %s: Int = %d\n", v.Name, v.Number)
			}
			b.WriteString("  }\n\n")
		}
		for _, m := range f.Messages {
			writeScalaMessage(&b, f, m)
		}
		b.WriteString("}\n")
		files = append(files, File{Name: base(f) + ".scala", Content: b.String()})
	}
	var meta strings.Builder
	writeScalaMeta(&meta, f, opt)
	files = append(files, File{Name: base(f) + "_meta.scala", Content: meta.String()})
	return files, nil
}

// writeScalaMeta emits the schema object: the embedded descriptor set plus
// an explicit loader. register=true loads eagerly (as before); otherwise
// initSchema (embedded bytes by default, external bytes for the descriptor
// mode) must run first, loudly otherwise. registry adds the whole-file
// message list plus a preload that warms every type.
func writeScalaMeta(b *strings.Builder, f *model.File, opt Options) {
	meta := base(f) + "Meta"
	b.WriteString(header(f, "Embedded descriptor set plus its loader: the schema side of the generated model."))
	b.WriteString("import java.util.Base64\n\n")
	fmt.Fprintf(b, "object %s {\n", meta)
	// Triple-quoted chunks joined once: a single literal would exceed the
	// JVM's 64 KB string-constant limit, and a thousand-way `+` chain
	// overflows scalac (the conformance descriptor set is ~100 KB).
	b.WriteString("  private val desc: Array[Byte] = Base64.getDecoder.decode(\n")
	b.WriteString("    List(" + scalaChunks(opt.DescSetB64) + ").mkString.filterNot(_.isWhitespace)\n")
	b.WriteString("  )\n\n")
	b.WriteString("  private var _schema: Schema = _\n\n")
	b.WriteString("  def initSchema(bytes: Array[Byte] = desc): Unit = _schema = Schema.fromBytes(bytes)\n\n")
	if opt.Register {
		b.WriteString("  initSchema()\n\n")
	}
	b.WriteString("  def schema: Schema = {\n")
	b.WriteString("    if (_schema == null)\n")
	b.WriteString("      throw new IllegalStateException(\"call initSchema first (generated with register=false)\")\n")
	b.WriteString("    _schema\n")
	b.WriteString("  }\n")
	if opt.Registry {
		b.WriteString("\n  val messageNames: List[String] = List(\n")
		for _, m := range f.Messages {
			fmt.Fprintf(b, "    %q,\n", m.Full)
		}
		b.WriteString("  )\n\n")
		b.WriteString("  def preload(): Unit = messageNames.foreach(schema.message)\n")
	}
	b.WriteString("}\n")
}

// scalaMsg names the case class for a message field: the sibling file's Gen
// object member when foreign (same default package, no import line), this
// file's own class otherwise.
func scalaMsg(f *model.File, field model.Field) string {
	if field.Import != "" {
		if imp := findImport(f, field.Import); imp != nil {
			return stem(imp.Path) + "Gen." + imp.Prefix + "_" + field.Message
		}
	}
	return className(f, field.Message)
}

func scalaType(f *model.File, field model.Field) string {
	if field.Message != "" {
		return scalaMsg(f, field)
	}
	switch field.Kind {
	case protoreflect.Int32Kind, protoreflect.Sint32Kind, protoreflect.Sfixed32Kind:
		return "Int"
	case protoreflect.Int64Kind, protoreflect.Sint64Kind, protoreflect.Sfixed64Kind:
		return "Long"
	case protoreflect.Uint32Kind, protoreflect.Fixed32Kind:
		return "Long"
	case protoreflect.Uint64Kind, protoreflect.Fixed64Kind:
		return "Long"
	case protoreflect.FloatKind:
		return "Float"
	case protoreflect.DoubleKind:
		return "Double"
	case protoreflect.BoolKind:
		return "Boolean"
	case protoreflect.StringKind:
		return "String"
	case protoreflect.BytesKind:
		return "Array[Byte]"
	case protoreflect.EnumKind:
		return "Int"
	default:
		return "Long"
	}
}

// scalaChunks splits base64 into triple-quoted chunks under the JVM's
// 64 KB string-constant limit, joined at run time (a literal `+` chain
// would fold back into one over-long constant).
func scalaChunks(s string) string {
	const max = 60000
	var parts []string
	for i := 0; i < len(s); i += max {
		end := i + max
		if end > len(s) {
			end = len(s)
		}
		parts = append(parts, "\"\"\""+s[i:end]+"\"\"\"")
	}
	return strings.Join(parts, ", ")
}

// scalaProp is the Scala parameter name; keywords (`type`, `val`, ...) are
// backticked (the proto name in quotes is unchanged).
func scalaProp(field model.Field) string {
	switch field.Lower {
	case "type", "val", "var", "def", "object", "class", "trait", "match", "case", "import", "package", "extends", "with", "new", "this", "super", "if", "else", "for", "while", "do", "return", "throw", "try", "catch", "finally", "yield", "lazy", "override", "abstract", "final", "sealed", "implicit", "private", "protected", "true", "false", "null":
		return "`" + field.Lower + "`"
	default:
		return field.Lower
	}
}

func scalaFieldType(f *model.File, field model.Field) string {
	t := scalaType(f, field)
	if field.Repeated {
		return "List[" + t + "]"
	}
	if field.Message != "" || field.NeedsHas {
		return "Option[" + t + "]"
	}
	return t
}

func scalaDefault(f *model.File, field model.Field) string {
	if field.Repeated {
		return "Nil"
	}
	if field.Message != "" || field.NeedsHas {
		return "None"
	}
	switch scalaType(f, field) {
	case "Int":
		return "0"
	case "Long":
		return "0L"
	case "Float":
		return "0f"
	case "Double":
		return "0.0"
	case "Boolean":
		return "false"
	case "String":
		return "\"\""
	case "Array[Byte]":
		return "Array.emptyByteArray"
	default:
		return "0"
	}
}

func writeScalaMessage(b *strings.Builder, f *model.File, m model.Message) {
	cls := className(f, m.Name)
	fmt.Fprintf(b, "  // %s\n", m.Full)
	fmt.Fprintf(b, "  case class %s(\n", cls)
	for i, field := range m.Fields {
		if field.Skip != "" {
			fmt.Fprintf(b, "    // skipped %s: %s\n", field.Name, field.Skip)
			continue
		}
		comma := ","
		if i == len(m.Fields)-1 {
			comma = ""
		}
		fmt.Fprintf(b, "    %s: %s = %s%s\n", scalaProp(field), scalaFieldType(f, field), scalaDefault(f, field), comma)
	}
	fmt.Fprintf(b, "  ) {\n")
	fmt.Fprintf(b, "    def encode: Array[Byte] = %s.encode(this)\n", cls)
	b.WriteString("  }\n\n")
	fmt.Fprintf(b, "  object %s {\n", cls)
	fmt.Fprintf(b, "    private val ty: MessageType = %sMeta.schema.message(%q)\n\n", base(f), m.Full)
	fmt.Fprintf(b, "    def encode(v: %s): Array[Byte] = {\n", cls)
	b.WriteString("      val msg = ty.newMessage()\n")
	b.WriteString("      fill(msg, v)\n")
	b.WriteString("      msg.encode()\n")
	b.WriteString("    }\n\n")
	fmt.Fprintf(b, "    def decode(wire: Array[Byte]): %s = load(ty.decode(wire))\n\n", cls)
	fmt.Fprintf(b, "    def fill(msg: Message, v: %s): Unit = {\n", cls)
	for _, field := range m.Fields {
		if field.Skip != "" {
			continue
		}
		writeScalaFill(b, f, field)
	}
	b.WriteString("    }\n\n")
	fmt.Fprintf(b, "    def load(msg: Message): %s = %s(\n", cls, cls)
	for _, field := range m.Fields {
		if field.Skip != "" {
			continue
		}
		writeScalaLoad(b, f, field)
	}
	b.WriteString("    )\n")
	b.WriteString("  }\n\n")
}

func writeScalaFill(b *strings.Builder, f *model.File, field model.Field) {
	name := scalaProp(field)
	proto := field.Name
	switch {
	case field.Message != "" && !field.Repeated:
		sub := scalaMsg(f, field)
		fmt.Fprintf(b, "      v.%s.foreach(x => %s.fill(msg.newMessage(%q), x))\n", name, sub, proto)
	case field.Message != "" && field.Repeated:
		sub := scalaMsg(f, field)
		fmt.Fprintf(b, "      v.%s.foreach(x => %s.fill(msg.newMessage(%q), x))\n", name, sub, proto)
	case field.Repeated:
		fmt.Fprintf(b, "      v.%s.foreach(x => msg.set(%q, %s))\n", name, proto, scalaScalar(field, "x"))
	case field.NeedsHas:
		fmt.Fprintf(b, "      v.%s.foreach(x => msg.set(%q, %s))\n", name, proto, scalaScalar(field, "x"))
	case field.Required:
		fmt.Fprintf(b, "      msg.set(%q, %s)\n", proto, scalaScalar(field, "v."+name))
	default:
		// Proto3 implicit: zero stays off the wire.
		fmt.Fprintf(b, "      %s\n", scalaGuard(field, name, proto))
	}
}

// scalaGuard renders the write guard for an implicit-presence scalar.
func scalaGuard(field model.Field, name, proto string) string {
	set := fmt.Sprintf("msg.set(%q, %s)", proto, scalaScalar(field, "v."+name))
	switch field.Kind {
	case protoreflect.BoolKind:
		return fmt.Sprintf("if (v.%s) %s", name, set)
	case protoreflect.StringKind, protoreflect.BytesKind:
		return fmt.Sprintf("if (v.%s.nonEmpty) %s", name, set)
	default:
		return fmt.Sprintf("if (v.%s != 0) %s", name, set)
	}
}

func scalaScalar(field model.Field, src string) string {
	switch field.Kind {
	case protoreflect.BoolKind:
		return src
	case protoreflect.FloatKind:
		return src + ".toDouble"
	case protoreflect.DoubleKind:
		return src
	case protoreflect.StringKind:
		return src
	case protoreflect.BytesKind:
		return src
	default:
		return src + ".toLong"
	}
}

func writeScalaLoad(b *strings.Builder, f *model.File, field model.Field) {
	name := scalaProp(field)
	proto := field.Name
	t := scalaType(f, field)
	switch {
	case field.Message != "" && !field.Repeated:
		sub := scalaMsg(f, field)
		fmt.Fprintf(b, "      %s = Option(msg.getMessage(%q)).map(%s.load),\n", name, proto, sub)
	case field.Message != "" && field.Repeated:
		sub := scalaMsg(f, field)
		fmt.Fprintf(b, "      %s = msg.getList(%q).toArray.toList.collect { case m: Message => %s.load(m) },\n", name, proto, sub)
	case field.Repeated:
		fmt.Fprintf(b, "      %s = msg.getList(%q).toArray.toList.map(x => %s),\n", name, proto, scalaUnbox(t, "x"))
	case field.NeedsHas:
		fmt.Fprintf(b, "      %s = if (msg.has(%q)) Some(%s) else None,\n", name, proto, scalaGet(field))
	default:
		fmt.Fprintf(b, "      %s = %s,\n", name, scalaGet(field))
	}
}

// scalaUnbox converts one getList item (ints box as Long, floats as
// Float, everything else as itself) to the field's Scala type.
func scalaUnbox(t, src string) string {
	switch t {
	case "Int":
		return src + ".asInstanceOf[java.lang.Long].toInt"
	case "Long":
		return src + ".asInstanceOf[java.lang.Long].toLong"
	case "Float":
		return src + ".asInstanceOf[java.lang.Float].toFloat"
	case "Double":
		return src + ".asInstanceOf[java.lang.Double].toDouble"
	case "Boolean":
		return src + ".asInstanceOf[java.lang.Boolean].booleanValue"
	default:
		return src + ".asInstanceOf[" + t + "]"
	}
}

func scalaGet(field model.Field) string {
	proto := field.Name
	if field.Kind == protoreflect.EnumKind {
		return fmt.Sprintf("msg.getLong(%q).toInt", proto)
	}
	switch field.Kind {
	case protoreflect.BoolKind:
		return fmt.Sprintf("msg.getBool(%q)", proto)
	case protoreflect.FloatKind:
		return fmt.Sprintf("msg.getFloat(%q)", proto)
	case protoreflect.DoubleKind:
		return fmt.Sprintf("msg.getDouble(%q)", proto)
	case protoreflect.StringKind:
		return fmt.Sprintf("Option(msg.getString(%q)).getOrElse(\"\")", proto)
	case protoreflect.BytesKind:
		return fmt.Sprintf("Option(msg.getBytes(%q)).getOrElse(Array.emptyByteArray)", proto)
	case protoreflect.Int32Kind, protoreflect.Sint32Kind, protoreflect.Sfixed32Kind:
		return fmt.Sprintf("msg.getInt(%q)", proto)
	default:
		return fmt.Sprintf("msg.getLong(%q)", proto)
	}
}

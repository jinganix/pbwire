package gen

// PHP emitter: Google.Protobuf-shaped classes over the dynamic API.
//
// The surface mirrors protoc --php_out for the same .proto (getX/setX for
// singular fields, hasX/clearX for optional and message fields,
// RepeatedField for repeated, getKind/whichOneof for oneofs, enums as
// class constants with name/value helpers, serializeToString/
// mergeFromString), but every codec call goes through the binding's
// existing dynamic API, so the wire format stays in the C core. The
// FileDescriptorSet is embedded (no .desc file at use time).

import (
	"fmt"
	"strings"

	"pbwire-codegen/internal/model"

	"google.golang.org/protobuf/reflect/protoreflect"
)

var phpKeywords = map[string]bool{
	"abstract": true, "and": true, "array": true, "as": true, "break": true,
	"callable": true, "case": true, "catch": true, "class": true, "clone": true,
	"const": true, "continue": true, "declare": true, "default": true, "do": true,
	"echo": true, "else": true, "elseif": true, "empty": true, "enddeclare": true,
	"endfor": true, "endforeach": true, "endif": true, "endswitch": true,
	"endwhile": true, "enum": true, "eval": true, "exit": true, "extends": true,
	"final": true, "finally": true, "fn": true, "for": true, "foreach": true,
	"function": true, "global": true, "goto": true, "if": true, "implements": true,
	"include": true, "instanceof": true, "insteadof": true, "interface": true,
	"isset": true, "list": true, "match": true, "namespace": true, "new": true,
	"or": true, "print": true, "private": true, "protected": true, "public": true,
	"readonly": true, "require": true, "return": true, "static": true,
	"switch": true, "throw": true, "trait": true, "try": true, "unset": true,
	"use": true, "var": true, "while": true, "xor": true, "yield": true,
	"int": true, "float": true, "bool": true, "string": true, "void": true,
	"iterable": true, "object": true, "mixed": true, "never": true, "null": true,
	"false": true, "true": true, "self": true, "parent": true,
}

func phpName(name string) string {
	if phpKeywords[name] {
		return name + "_"
	}
	return name
}

// phpIdent sanitizes a file base name into a PHP class name suffix.
func phpIdent(s string) string {
	var b strings.Builder
	for _, r := range s {
		if (r >= 'a' && r <= 'z') || (r >= 'A' && r <= 'Z') || (r >= '0' && r <= '9') {
			b.WriteRune(r)
		} else {
			b.WriteRune('_')
		}
	}
	out := b.String()
	if out == "" {
		return "File"
	}
	return strings.ToUpper(out[:1]) + out[1:]
}

func phpFiles(f *model.File, opt Options) ([]File, error) {
	// Models and schema part ways: classes in one file, the embedded
	// descriptor set plus its loader in the holder file (already named per
	// file). model=false leaves the schema holder for dynamic-only users.
	holder := "PbwireFile_" + phpIdent(base(f))
	var files []File
	if opt.Model {
		var b strings.Builder
		b.WriteString("<?php\n\ndeclare(strict_types=1);\n\n")
		b.WriteString(headerP(f, "//", "Google.Protobuf-shaped classes over the pbwire runtime; the wire format stays in the C core."))
		b.WriteString("namespace Pbwire\\Gen;\n\nuse Pbwire\\Message;\nuse Pbwire\\RepeatedField;\nuse Pbwire\\Schema;\nuse Pbwire\\GeneratedUtil;\n\n")
		fmt.Fprintf(&b, "require_once __DIR__ . '/%s.php';\n\n", holder)
		for _, e := range f.Enums {
			writePhpEnum(&b, f, e)
		}
		for _, m := range f.Messages {
			writePhpMessage(&b, f, holder, m)
		}
		files = append(files, File{Name: base(f) + ".php", Content: b.String()})
	}
	var meta strings.Builder
	writePhpHolder(&meta, f, opt, holder)
	files = append(files, File{Name: holder + ".php", Content: meta.String()})
	return files, nil
}

// writePhpHolder emits the schema holder: the embedded descriptor set plus
// an explicit loader. register=true loads lazily on first use (as before)
// with an override slot; otherwise init (embedded bytes) or use_schema
// (external) must run first, loudly otherwise. registry adds the whole-file
// message list plus a preload that warms every type.
func writePhpHolder(b *strings.Builder, f *model.File, opt Options, holder string) {
	b.WriteString("<?php\n\ndeclare(strict_types=1);\n\n")
	b.WriteString(headerP(f, "//", "Embedded descriptor set plus its loader: the schema side of the generated model."))
	b.WriteString("namespace Pbwire\\Gen;\n\nuse Pbwire\\Schema;\n\n")
	fmt.Fprintf(b, "/** The embedded descriptor set: no .desc file at use time. */\nfinal class %s\n{\n", holder)
	fmt.Fprintf(b, "    private const DESC_B64 = '%s';\n\n", opt.DescSetB64)
	b.WriteString("    private static ?Schema $schema = null;\n\n")
	b.WriteString("    public static function init(): void\n")
	b.WriteString("    {\n")
	b.WriteString("        if (self::$schema === null) {\n")
	b.WriteString("            $raw = base64_decode(self::DESC_B64, true);\n")
	b.WriteString("            if ($raw === false) {\n")
	b.WriteString("                throw new \\RuntimeException('generated model: bad embedded descriptor set');\n")
	b.WriteString("            }\n")
	b.WriteString("            self::$schema = Schema::fromBytes($raw);\n")
	b.WriteString("        }\n")
	b.WriteString("    }\n\n")
	b.WriteString("    /// Registers an externally loaded schema: models encode/decode through it\n")
	b.WriteString("    /// instead of the embedded set. The caller owns it; null clears the override.\n")
	b.WriteString("    public static function use_schema(?Schema $s): void\n")
	b.WriteString("    {\n")
	b.WriteString("        self::$schema = $s;\n")
	b.WriteString("    }\n\n")
	if opt.Register {
		b.WriteString("    public static function schema(): Schema\n")
		b.WriteString("    {\n")
		b.WriteString("        if (self::$schema === null) {\n")
		b.WriteString("            self::init();\n")
		b.WriteString("        }\n")
		b.WriteString("        return self::$schema;\n")
		b.WriteString("    }\n")
	} else {
		b.WriteString("    public static function schema(): Schema\n")
		b.WriteString("    {\n")
		b.WriteString("        if (self::$schema === null) {\n")
		b.WriteString("            throw new \\RuntimeException('call init() first (generated with register=false)');\n")
		b.WriteString("        }\n")
		b.WriteString("        return self::$schema;\n")
		b.WriteString("    }\n")
	}
	if opt.Registry {
		b.WriteString("\n    /** Every message in this file, for whole-schema pre-registration. */\n")
		b.WriteString("    public const MESSAGE_NAMES = [\n")
		for _, m := range f.Messages {
			fmt.Fprintf(b, "        %q,\n", m.Full)
		}
		b.WriteString("    ];\n\n")
		b.WriteString("    /** Warms every message type, so later calls pay no lookup. */\n")
		b.WriteString("    public static function preload(): void\n")
		b.WriteString("    {\n")
		b.WriteString("        $s = self::schema();\n")
		b.WriteString("        foreach (self::MESSAGE_NAMES as $n) {\n")
		b.WriteString("            $s->message($n);\n")
		b.WriteString("        }\n")
		b.WriteString("    }\n")
	}
	b.WriteString("}\n")
}

// phpMsg names the class for a message field: the owning file's prefix when
// foreign (one shared Pbwire\Gen namespace, so no use line), this file's
// own otherwise.
func phpMsg(f *model.File, field model.Field) string {
	if field.Import != "" {
		if imp := findImport(f, field.Import); imp != nil {
			return imp.Prefix + "_" + field.Message
		}
	}
	return className(f, field.Message)
}

func writePhpEnum(b *strings.Builder, f *model.File, e model.Enum) {
	fmt.Fprintf(b, "/** %s */\nfinal class %s\n{\n", e.Full, className(f, e.Name))
	for _, v := range e.Values {
		fmt.Fprintf(b, "    public const %s = %d;\n", v.Name, v.Number)
	}
	b.WriteString("\n    public static function name(int $value): string\n    {\n")
	b.WriteString("        return match ($value) {\n")
	seen := map[int32]bool{}
	for _, v := range e.Values {
		if seen[v.Number] {
			continue // allow_alias: first name wins, like protoc
		}
		seen[v.Number] = true
		fmt.Fprintf(b, "            %d => %q,\n", v.Number, v.Name)
	}
	b.WriteString("            default => throw new \\UnexpectedValueException(\"enum has no name for value $value\"),\n")
	b.WriteString("        };\n    }\n\n")
	b.WriteString("    public static function value(string $name): int\n    {\n")
	b.WriteString("        return match ($name) {\n")
	for _, v := range e.Values {
		fmt.Fprintf(b, "            %q => %d,\n", v.Name, v.Number)
	}
	b.WriteString("            default => throw new \\UnexpectedValueException(\"enum has no value for name $name\"),\n")
	b.WriteString("        };\n    }\n}\n\n")
}

// phpOneofs returns the distinct oneof names of a message in field order.
func phpOneofs(m model.Message) []string {
	var out []string
	seen := map[string]bool{}
	for _, field := range m.Fields {
		if field.Skip == "" && field.Oneof != "" && !seen[field.Oneof] {
			seen[field.Oneof] = true
			out = append(out, field.Oneof)
		}
	}
	return out
}

func writePhpMessage(b *strings.Builder, f *model.File, holder string, m model.Message) {
	cls := className(f, m.Name)
	oneofs := phpOneofs(m)
	fmt.Fprintf(b, "/** %s */\nfinal class %s\n{\n", m.Full, cls)
	fmt.Fprintf(b, "    public const FULL_NAME = %q;\n\n", m.Full)
	for _, field := range m.Fields {
		if field.Skip != "" {
			fmt.Fprintf(b, "    // skipped %s: %s\n", field.Name, field.Skip)
			continue
		}
		writePhpProp(b, f, field)
	}
	for _, o := range oneofs {
		fmt.Fprintf(b, "    /** Active member of the %q oneof (proto field name, '' when unset). */\n", o)
		fmt.Fprintf(b, "    private string $%s = '';\n", phpOneofState(o))
	}
	b.WriteString("\n    public function __construct(?array $data = null)\n    {\n")
	for _, field := range m.Fields {
		if field.Skip != "" || !field.Repeated {
			continue
		}
		fmt.Fprintf(b, "        $this->%s = new RepeatedField(%s);\n", phpName(field.Lower), phpRepeatedArgs(f, field, ""))
	}
	b.WriteString("        if ($data !== null) {\n            $this->mergeFromArray($data);\n        }\n    }\n\n")
	writePhpArray(b, f, m)
	writePhpAccessors(b, f, m, oneofs)
	writePhpCodec(b, f, holder, m, oneofs)
	b.WriteString("}\n\n")
}

// phpOneofState is the property tracking which member of a oneof is set.
func phpOneofState(oneof string) string {
	return phpName(model.LowerCamel(oneof)) + "Kind"
}

// phpPropType is the PHP type of a backing property.
func phpPropType(f *model.File, field model.Field) string {
	if field.Repeated {
		return "RepeatedField"
	}
	if field.Message != "" {
		return "?" + phpMsg(f, field)
	}
	if field.NeedsHas || field.Required {
		return "?" + phpScalar(field)
	}
	return phpScalar(field)
}

func phpScalar(field model.Field) string {
	switch field.Kind {
	case protoreflect.FloatKind, protoreflect.DoubleKind:
		return "float"
	case protoreflect.BoolKind:
		return "bool"
	case protoreflect.StringKind, protoreflect.BytesKind:
		return "string"
	default:
		return "int"
	}
}

func phpPropDefault(f *model.File, field model.Field) string {
	if field.Repeated || field.Message != "" || field.NeedsHas || field.Required {
		return "null"
	}
	switch field.Kind {
	case protoreflect.BoolKind:
		return "false"
	case protoreflect.StringKind, protoreflect.BytesKind:
		return "''"
	case protoreflect.FloatKind, protoreflect.DoubleKind:
		return "0.0"
	default:
		return "0"
	}
}

func writePhpProp(b *strings.Builder, f *model.File, field model.Field) {
	if field.Repeated {
		fmt.Fprintf(b, "    private %s $%s; // %s = %d\n", phpPropType(f, field), phpName(field.Lower), field.Name, field.Number)
		return
	}
	fmt.Fprintf(b, "    private %s $%s = %s; // %s = %d\n",
		phpPropType(f, field), phpName(field.Lower), phpPropDefault(f, field), field.Name, field.Number)
}

// phpRepeatedArgs renders the RepeatedField constructor args for a field:
// (tag, items, class?, allowed?). Empty items default to [].
func phpRepeatedArgs(f *model.File, field model.Field, items string) string {
	if items == "" {
		items = "[]"
	}
	if field.Message != "" {
		return fmt.Sprintf("'message', %s, %s::class", items, phpMsg(f, field))
	}
	tag, allowed := phpElemTag(f, field)
	if allowed != "" {
		return fmt.Sprintf("'%s', %s, null, %s", tag, items, allowed)
	}
	return fmt.Sprintf("'%s', %s", tag, items)
}

// phpElemTag maps a field kind to the RepeatedField element tag.
func phpElemTag(f *model.File, field model.Field) (string, string) {
	switch field.Kind {
	case protoreflect.Int32Kind, protoreflect.Sint32Kind, protoreflect.Sfixed32Kind:
		return "int32", ""
	case protoreflect.Uint32Kind, protoreflect.Fixed32Kind:
		return "uint32", ""
	case protoreflect.Int64Kind, protoreflect.Sint64Kind, protoreflect.Sfixed64Kind:
		return "int64", ""
	case protoreflect.Uint64Kind, protoreflect.Fixed64Kind:
		return "uint64", ""
	case protoreflect.FloatKind:
		return "float", ""
	case protoreflect.DoubleKind:
		return "double", ""
	case protoreflect.BoolKind:
		return "bool", ""
	case protoreflect.StringKind:
		return "string", ""
	case protoreflect.BytesKind:
		return "bytes", ""
	case protoreflect.EnumKind:
		return "enum", phpAllowed(f, field)
	default:
		return "int64", ""
	}
}

// phpAllowed renders the closed-enum value list, or "" for open enums.
func phpAllowed(f *model.File, field model.Field) string {
	if !field.Closed {
		return ""
	}
	for _, e := range f.Enums {
		if e.Name != field.Enum {
			continue
		}
		var vals []string
		seen := map[int32]bool{}
		for _, v := range e.Values {
			if seen[v.Number] {
				continue
			}
			seen[v.Number] = true
			vals = append(vals, fmt.Sprintf("%d", v.Number))
		}
		return "[" + strings.Join(vals, ", ") + "]"
	}
	return ""
}

// phpCheck renders the GeneratedUtil validation call for a scalar value expression.
func phpCheck(f *model.File, field model.Field, expr string) string {
	switch field.Kind {
	case protoreflect.Int32Kind, protoreflect.Sint32Kind, protoreflect.Sfixed32Kind:
		return fmt.Sprintf("GeneratedUtil::checkInt32(%s)", expr)
	case protoreflect.Uint32Kind, protoreflect.Fixed32Kind:
		return fmt.Sprintf("GeneratedUtil::checkUint32(%s)", expr)
	case protoreflect.Int64Kind, protoreflect.Sint64Kind, protoreflect.Sfixed64Kind:
		return fmt.Sprintf("GeneratedUtil::checkInt64(%s)", expr)
	case protoreflect.Uint64Kind, protoreflect.Fixed64Kind:
		return fmt.Sprintf("GeneratedUtil::checkUint64(%s)", expr)
	case protoreflect.FloatKind:
		return fmt.Sprintf("GeneratedUtil::checkFloat(%s)", expr)
	case protoreflect.DoubleKind:
		return fmt.Sprintf("GeneratedUtil::checkDouble(%s)", expr)
	case protoreflect.BoolKind:
		return fmt.Sprintf("GeneratedUtil::checkBool(%s)", expr)
	case protoreflect.StringKind:
		return fmt.Sprintf("GeneratedUtil::checkString(%s)", expr)
	case protoreflect.BytesKind:
		return fmt.Sprintf("GeneratedUtil::checkBytes(%s)", expr)
	case protoreflect.EnumKind:
		return fmt.Sprintf("GeneratedUtil::checkEnum(%s, %s)", expr, phpAllowedOrNull(f, field))
	default:
		return expr
	}
}

func phpAllowedOrNull(f *model.File, field model.Field) string {
	if a := phpAllowed(f, field); a != "" {
		return a
	}
	return "null"
}

// phpParamType is the setter parameter type: 64-bit fields take int|string.
func phpParamType(field model.Field) string {
	switch field.Kind {
	case protoreflect.Int64Kind, protoreflect.Sint64Kind, protoreflect.Sfixed64Kind,
		protoreflect.Uint64Kind, protoreflect.Fixed64Kind:
		return "int|string"
	case protoreflect.FloatKind, protoreflect.DoubleKind:
		return "int|float"
	default:
		return phpScalar(field)
	}
}

// phpReturnType is the getter return type.
func phpReturnType(f *model.File, field model.Field) string {
	if field.Repeated {
		return "RepeatedField"
	}
	if field.Message != "" {
		return "?" + phpMsg(f, field)
	}
	return phpScalar(field)
}

func writePhpArray(b *strings.Builder, f *model.File, m model.Message) {
	b.WriteString("    /** Populate from proto-name keys, like the official constructor. */\n")
	b.WriteString("    public function mergeFromArray(array $data): void\n    {\n")
	b.WriteString("        foreach ($data as $k => $v) {\n")
	b.WriteString("            match ($k) {\n")
	for _, field := range m.Fields {
		if field.Skip != "" {
			continue
		}
		setter := "set" + field.Pascal
		if field.Repeated {
			fmt.Fprintf(b, "                %q => $this->%s($v),\n", field.Name, setter)
		} else if field.Message != "" {
			fmt.Fprintf(b, "                %q => $this->%s($v instanceof %s ? $v : new %s($v)),\n",
				field.Name, setter, phpMsg(f, field), phpMsg(f, field))
		} else {
			fmt.Fprintf(b, "                %q => $this->%s($v),\n", field.Name, setter)
		}
	}
	b.WriteString("                default => throw new \\InvalidArgumentException(\"unknown field \\\"$k\\\"\"),\n")
	b.WriteString("            };\n        }\n    }\n\n")
}

func writePhpAccessors(b *strings.Builder, f *model.File, m model.Message, oneofs []string) {
	for _, field := range m.Fields {
		if field.Skip != "" {
			continue
		}
		if field.Repeated {
			writePhpRepeated(b, f, field)
		} else if field.Message != "" {
			writePhpMessageField(b, f, field)
		} else {
			writePhpScalarField(b, f, field)
		}
	}
	for _, o := range oneofs {
		getter := "get" + model.Pascal(o)
		fmt.Fprintf(b, "    /** Which member of the %q oneof is set (proto field name, '' when unset). */\n", o)
		fmt.Fprintf(b, "    public function %s(): string\n    {\n", getter)
		fmt.Fprintf(b, "        return $this->%s;\n    }\n\n", phpOneofState(o))
		fmt.Fprintf(b, "    public function whichOneof(string $oneof): string\n    {\n")
		fmt.Fprintf(b, "        if ($oneof === %q) {\n", o)
		fmt.Fprintf(b, "            return $this->%s;\n", phpOneofState(o))
		b.WriteString("        }\n")
		b.WriteString("        throw new \\InvalidArgumentException(\"unknown oneof \\\"$oneof\\\"\");\n    }\n\n")
	}
}

func writePhpRepeated(b *strings.Builder, f *model.File, field model.Field) {
	prop, pascal := phpName(field.Lower), field.Pascal
	fmt.Fprintf(b, "    public function get%s(): RepeatedField\n    {\n", pascal)
	fmt.Fprintf(b, "        return $this->%s;\n    }\n\n", prop)
	fmt.Fprintf(b, "    /** @param array|RepeatedField $var */\n")
	fmt.Fprintf(b, "    public function set%s(array|RepeatedField $var): self\n    {\n", pascal)
	fmt.Fprintf(b, "        $items = $var instanceof RepeatedField ? iterator_to_array($var) : array_values($var);\n")
	if field.Message != "" {
		cls := phpMsg(f, field)
		b.WriteString("        $out = [];\n")
		fmt.Fprintf(b, "        foreach ($items as $v) { $out[] = $v instanceof %s ? $v : new %s($v); }\n", cls, cls)
		fmt.Fprintf(b, "        $this->%s = new RepeatedField(%s);\n", prop, phpRepeatedArgs(f, field, "$out"))
	} else {
		fmt.Fprintf(b, "        $this->%s = new RepeatedField(%s);\n", prop, phpRepeatedArgs(f, field, "$items"))
	}
	b.WriteString("        return $this;\n    }\n\n")
}

func writePhpMessageField(b *strings.Builder, f *model.File, field model.Field) {
	prop, pascal, cls := phpName(field.Lower), field.Pascal, phpMsg(f, field)
	fmt.Fprintf(b, "    public function get%s(): ?%s\n    {\n", pascal, cls)
	if field.Oneof != "" {
		fmt.Fprintf(b, "        return $this->%s === %q ? $this->%s : null;\n", phpOneofState(field.Oneof), field.Name, prop)
	} else {
		fmt.Fprintf(b, "        return $this->%s;\n", prop)
	}
	b.WriteString("    }\n\n")
	fmt.Fprintf(b, "    public function has%s(): bool\n    {\n", pascal)
	if field.Oneof != "" {
		fmt.Fprintf(b, "        return $this->%s === %q;\n", phpOneofState(field.Oneof), field.Name)
	} else {
		fmt.Fprintf(b, "        return $this->%s !== null;\n", prop)
	}
	b.WriteString("    }\n\n")
	if field.Oneof == "" {
		fmt.Fprintf(b, "    public function clear%s(): void\n    {\n", pascal)
		fmt.Fprintf(b, "        $this->%s = null;\n    }\n\n", prop)
	}
	fmt.Fprintf(b, "    public function set%s(?%s $var): self\n    {\n", pascal, cls)
	if field.Oneof != "" {
		fmt.Fprintf(b, "        $this->%s = $var;\n", prop)
		fmt.Fprintf(b, "        $this->%s = $var === null ? '' : %q;\n", phpOneofState(field.Oneof), field.Name)
	} else {
		fmt.Fprintf(b, "        $this->%s = $var;\n", prop)
	}
	b.WriteString("        return $this;\n    }\n\n")
}

func writePhpScalarField(b *strings.Builder, f *model.File, field model.Field) {
	prop, pascal := phpName(field.Lower), field.Pascal
	present := field.NeedsHas || field.Required
	if field.Oneof != "" {
		def := phpScalarDefault(field)
		fmt.Fprintf(b, "    public function get%s(): %s\n    {\n", pascal, phpScalar(field))
		fmt.Fprintf(b, "        return $this->%s === %q ? $this->%s : %s;\n", phpOneofState(field.Oneof), field.Name, prop, def)
		b.WriteString("    }\n\n")
		fmt.Fprintf(b, "    public function has%s(): bool\n    {\n", pascal)
		fmt.Fprintf(b, "        return $this->%s === %q;\n", phpOneofState(field.Oneof), field.Name)
		b.WriteString("    }\n\n")
		fmt.Fprintf(b, "    public function set%s(%s $var): self\n    {\n", pascal, phpParamType(field))
		fmt.Fprintf(b, "        $this->%s = %s;\n", prop, phpCheck(f, field, "$var"))
		fmt.Fprintf(b, "        $this->%s = %q;\n", phpOneofState(field.Oneof), field.Name)
		b.WriteString("        return $this;\n    }\n\n")
		return
	}
	fmt.Fprintf(b, "    public function get%s(): %s\n    {\n", pascal, phpScalar(field))
	if present {
		fmt.Fprintf(b, "        return $this->%s ?? %s;\n", prop, phpScalarDefault(field))
	} else {
		fmt.Fprintf(b, "        return $this->%s;\n", prop)
	}
	b.WriteString("    }\n\n")
	if present {
		fmt.Fprintf(b, "    public function has%s(): bool\n    {\n", pascal)
		fmt.Fprintf(b, "        return $this->%s !== null;\n", prop)
		b.WriteString("    }\n\n")
		fmt.Fprintf(b, "    public function clear%s(): void\n    {\n", pascal)
		fmt.Fprintf(b, "        $this->%s = null;\n", prop)
		b.WriteString("    }\n\n")
	}
	fmt.Fprintf(b, "    public function set%s(%s $var): self\n    {\n", pascal, phpParamType(field))
	fmt.Fprintf(b, "        $this->%s = %s;\n", prop, phpCheck(f, field, "$var"))
	b.WriteString("        return $this;\n    }\n\n")
}

func phpScalarDefault(field model.Field) string {
	switch field.Kind {
	case protoreflect.BoolKind:
		return "false"
	case protoreflect.StringKind, protoreflect.BytesKind:
		return "''"
	case protoreflect.FloatKind, protoreflect.DoubleKind:
		return "0.0"
	default:
		return "0"
	}
}

func writePhpCodec(b *strings.Builder, f *model.File, holder string, m model.Message, oneofs []string) {
	cls := className(f, m.Name)
	b.WriteString("    public function serializeToString(): string\n    {\n")
	fmt.Fprintf(b, "        $msg = %s::schema()->message(self::FULL_NAME)->newMessage();\n", holder)
	b.WriteString("        $this->encodeInto($msg);\n")
	b.WriteString("        return $msg->encode();\n    }\n\n")
	b.WriteString("    public function mergeFromString(string $wire): void\n    {\n")
	fmt.Fprintf(b, "        $this->mergeFromMessage(%s::schema()->message(self::FULL_NAME)->decode($wire));\n", holder)
	b.WriteString("    }\n\n")
	b.WriteString("    public static function fromString(string $wire): self\n    {\n")
	b.WriteString("        $o = new self();\n")
	b.WriteString("        $o->mergeFromString($wire);\n")
	b.WriteString("        return $o;\n    }\n\n")
	fmt.Fprintf(b, "    public function mergeFrom(%s $other): void\n    {\n", cls)
	b.WriteString("        $this->mergeFromString($other->serializeToString());\n    }\n\n")
	b.WriteString("    /** @internal_encode into an already-created dynamic message. */\n")
	b.WriteString("    public function encodeInto(Message $msg): void\n    {\n")
	for _, field := range m.Fields {
		if field.Skip != "" {
			continue
		}
		writePhpEncode(b, f, field)
	}
	b.WriteString("    }\n\n")
	b.WriteString("    /** @internal_merge a decoded dynamic message (singulars overwrite, repeated append). */\n")
	b.WriteString("    public function mergeFromMessage(Message $msg): void\n    {\n")
	for _, field := range m.Fields {
		if field.Skip != "" {
			continue
		}
		writePhpLoad(b, f, field)
	}
	b.WriteString("    }\n")
}

// phpEncodeCond is the write guard for implicit and explicit presence.
func phpEncodeCond(field model.Field, prop string) (string, bool) {
	if field.Required {
		return "", false // always written; missing throws below
	}
	state := "$this->" + prop
	switch {
	case field.Oneof != "":
		return "$this->" + phpOneofState(field.Oneof) + " === " + fmt.Sprintf("%q", field.Name), true
	case field.Repeated:
		return "count(" + state + ") !== 0", true
	case field.Message != "":
		return state + " !== null", true
	case field.NeedsHas:
		return state + " !== null", true
	case field.Kind == protoreflect.BoolKind:
		return state, true
	case field.Kind == protoreflect.StringKind || field.Kind == protoreflect.BytesKind:
		return state + " !== ''", true
	case field.Kind == protoreflect.FloatKind || field.Kind == protoreflect.DoubleKind:
		return state + " != 0.0", true
	default:
		return state + " != 0", true
	}
}

func writePhpEncode(b *strings.Builder, f *model.File, field model.Field) {
	prop := phpName(field.Lower)
	key := fmt.Sprintf("%q", field.Name)
	if field.Required {
		fmt.Fprintf(b, "        if ($this->%s === null) {\n", prop)
		fmt.Fprintf(b, "            throw new \\RuntimeException(%q);\n", "missing required field "+field.Name)
		b.WriteString("        }\n")
		writePhpEncodeOne(b, f, field, "$this->"+prop, key, "        ")
		return
	}
	cond, use := phpEncodeCond(field, prop)
	if !use {
		writePhpEncodeOne(b, f, field, "$this->"+prop, key, "        ")
		return
	}
	fmt.Fprintf(b, "        if (%s) {\n", cond)
	writePhpEncodeOne(b, f, field, "$this->"+prop, key, "            ")
	b.WriteString("        }\n")
}

func writePhpEncodeOne(b *strings.Builder, f *model.File, field model.Field, src, key, indent string) {
	switch {
	case field.Message != "" && field.Repeated:
		fmt.Fprintf(b, "%sforeach (%s as $v) {\n", indent, src)
		fmt.Fprintf(b, "%s    $v->encodeInto($msg->setMessage(%s));\n", indent, key)
		fmt.Fprintf(b, "%s}\n", indent)
	case field.Message != "":
		fmt.Fprintf(b, "%s%s->encodeInto($msg->setMessage(%s));\n", indent, src, key)
	case field.Repeated:
		fmt.Fprintf(b, "%sforeach (%s as $v) {\n", indent, src)
		fmt.Fprintf(b, "%s    $msg->%s(%s, $v);\n", indent, phpSetter(field), key)
		fmt.Fprintf(b, "%s}\n", indent)
	default:
		fmt.Fprintf(b, "%s$msg->%s(%s, %s);\n", indent, phpSetter(field), key, phpEncodeScalar(field))
	}
	_ = f
}

// phpEncodeScalar renders the dynamic setter value: setters take raw bits.
func phpEncodeScalar(field model.Field) string {
	if field.Repeated {
		return "$v"
	}
	return "$this->" + phpName(field.Lower)
}

func phpSetter(field model.Field) string {
	switch field.Kind {
	case protoreflect.FloatKind:
		return "setFloat"
	case protoreflect.DoubleKind:
		return "setDouble"
	case protoreflect.BoolKind:
		return "setBool"
	case protoreflect.StringKind:
		return "setString"
	case protoreflect.BytesKind:
		return "setBytes"
	default:
		return "setInt"
	}
}

func writePhpLoad(b *strings.Builder, f *model.File, field model.Field) {
	prop := phpName(field.Lower)
	key := fmt.Sprintf("%q", field.Name)
	if field.Repeated {
		fmt.Fprintf(b, "        for ($i = 0; $i < $msg->count(%s); $i++) {\n", key)
		fmt.Fprintf(b, "            $v = $msg->at(%s, $i);\n", key)
		fmt.Fprintf(b, "            if ($v === null) { continue; }\n")
		if field.Message != "" {
			fmt.Fprintf(b, "            $child = $v->asMessage();\n")
			fmt.Fprintf(b, "            if ($child !== null) { $m = new %s(); $m->mergeFromMessage($child); $this->%s[] = $m; }\n",
				phpMsg(f, field), prop)
		} else if phpSigned32(field) {
			fmt.Fprintf(b, "            $this->%s[] = GeneratedUtil::toSigned32($v->asInt());\n", prop)
		} else {
			fmt.Fprintf(b, "            $this->%s[] = %s;\n", prop, phpElem(field, "$v"))
		}
		b.WriteString("        }\n")
		return
	}
	if field.Message != "" {
		fmt.Fprintf(b, "        $child = $msg->getMessage(%s);\n", key)
		fmt.Fprintf(b, "        if ($child !== null) {\n")
		if field.Oneof != "" {
			fmt.Fprintf(b, "            $m = new %s(); $m->mergeFromMessage($child);\n", phpMsg(f, field))
			fmt.Fprintf(b, "            $this->%s = $m;\n", prop)
			fmt.Fprintf(b, "            $this->%s = %q;\n", phpOneofState(field.Oneof), field.Name)
		} else {
			fmt.Fprintf(b, "            if ($this->%s === null) { $this->%s = new %s(); }\n", prop, prop, phpMsg(f, field))
			fmt.Fprintf(b, "            $this->%s->mergeFromMessage($child);\n", prop)
		}
		b.WriteString("        }\n")
		return
	}
	read := fmt.Sprintf("$msg->%s(%s)", phpGetter(field), key)
	if phpSigned32(field) {
		read = "GeneratedUtil::toSigned32(" + read + ")"
	}
	if field.Oneof != "" {
		fmt.Fprintf(b, "        if ($msg->has(%s)) { $this->set%s(%s); }\n", key, field.Pascal, phpLoadValue(field, read))
		return
	}
	if field.NeedsHas || field.Required {
		fmt.Fprintf(b, "        if ($msg->has(%s)) { $this->set%s(%s); }\n", key, field.Pascal, phpLoadValue(field, read))
		return
	}
	fmt.Fprintf(b, "        $this->set%s(%s);\n", field.Pascal, phpLoadValue(field, read))
	_ = f
}

func phpSigned32(field model.Field) bool {
	switch field.Kind {
	case protoreflect.Int32Kind, protoreflect.Sint32Kind, protoreflect.Sfixed32Kind, protoreflect.EnumKind:
		return true
	default:
		return false
	}
}

func phpGetter(field model.Field) string {
	switch field.Kind {
	case protoreflect.FloatKind:
		return "getFloat"
	case protoreflect.DoubleKind:
		return "getDouble"
	case protoreflect.BoolKind:
		return "getBool"
	case protoreflect.StringKind:
		return "getString"
	case protoreflect.BytesKind:
		return "getBytes"
	default:
		return "getInt"
	}
}

func phpLoadValue(field model.Field, expr string) string {
	switch field.Kind {
	case protoreflect.StringKind, protoreflect.BytesKind:
		return expr + " ?? ''"
	default:
		return expr
	}
}

func phpElem(field model.Field, expr string) string {
	switch field.Kind {
	case protoreflect.StringKind:
		return expr + "->asString() ?? ''"
	case protoreflect.BytesKind:
		return expr + "->asBytes() ?? ''"
	case protoreflect.BoolKind:
		return expr + "->asBool()"
	case protoreflect.FloatKind:
		return expr + "->asFloat()"
	case protoreflect.DoubleKind:
		return expr + "->asDouble()"
	default:
		return expr + "->asInt()"
	}
}

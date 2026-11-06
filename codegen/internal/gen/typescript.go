package gen

import (
	"fmt"
	"strings"

	"pbwire-codegen/internal/model"

	"google.golang.org/protobuf/reflect/protoreflect"
)

// jsKind maps a model field to the generated runtime's kind tag.
func jsKind(field model.Field) string {
	if field.Message != "" {
		return "msg"
	}
	switch field.Kind {
	case protoreflect.Int32Kind:
		return "i32"
	case protoreflect.Sint32Kind:
		return "s32"
	case protoreflect.Sfixed32Kind:
		return "sfx32"
	case protoreflect.Uint32Kind:
		return "u32"
	case protoreflect.Fixed32Kind:
		return "fx32"
	case protoreflect.Int64Kind:
		return "i64"
	case protoreflect.Sint64Kind:
		return "s64"
	case protoreflect.Sfixed64Kind:
		return "sfx64"
	case protoreflect.Uint64Kind:
		return "u64"
	case protoreflect.Fixed64Kind:
		return "fx64"
	case protoreflect.FloatKind:
		return "f32"
	case protoreflect.DoubleKind:
		return "f64"
	case protoreflect.BoolKind:
		return "bool"
	case protoreflect.StringKind:
		return "str"
	case protoreflect.BytesKind:
		return "bytes"
	case protoreflect.EnumKind:
		return "enum"
	default:
		return "msg"
	}
}

// jsRuntime is emitted once per file: schema bootstrap plus the generic
// verify/fromObject/toObject engine every class delegates to. The wire
// itself always goes through the binding's dynamic API (the C core).
const jsRuntime = `'use strict';

// The verify/fromObject/toObject engine lives in lib/pbwire-compat.js, shared
// by every generated file. Generated files live two levels under the binding
// root (<binding>/{tests,examples}/gen); the compat runtime sits next to the
// dynamic runtime in <binding>/lib.
const { _makeClass } = require('../../lib/pbwire-compat.js');
`

func jsFieldName(field model.Field) string {
	name := field.Lower
	if name == "" {
		name = field.Name
	}
	// lowerCamel can collide with the proto name; the class reads both.
	return name
}

// javaScriptFiles emits a protobufjs-static-shaped module: one class per
// message (create/encode/decode/verify/fromObject/toObject), enum constants,
// and the embedded FileDescriptorSet so no .desc file is needed at use time.
func javaScriptFiles(f *model.File, opt Options) []File {
	// Models and schema part ways: the facade module requires its schema
	// module for the descriptor set and loader. model=false leaves the
	// schema module for dynamic-only users.
	var files []File
	if opt.Model {
		var b strings.Builder
		b.WriteString(header(f, "protobufjs-static-shaped facade over the pbwire dynamic API."))
		b.WriteString(jsRuntime)
		b.WriteString("\n")
		fmt.Fprintf(&b, "const { get_schema } = require('./%s_meta.js');\n\n", base(f))
		for _, e := range f.Enums {
			fmt.Fprintf(&b, "/** %s @enum {number} */\n", e.Full)
			fmt.Fprintf(&b, "const %s = Object.freeze({\n", className(f, e.Name))
			for _, v := range e.Values {
				fmt.Fprintf(&b, "  %s: %d,\n", v.Name, v.Number)
			}
			b.WriteString("});\n")
			fmt.Fprintf(&b, "const _REV_%s = Object.freeze({\n", className(f, e.Name))
			for _, v := range e.Values {
				fmt.Fprintf(&b, "  '%d': '%s',\n", v.Number, v.Name)
			}
			b.WriteString("});\n\n")
		}
		b.WriteString("const _ENUMS = {};\nconst _REVENUMS = {};\nconst _CLASSES = {};\n")
		// Sibling models register their classes here by bare name (the dep
		// file uses the same names); local classes below overwrite on
		// collision, so locals win.
		for _, imp := range f.Imports {
			if names := foreignMessageNames(f, imp.Path); len(names) > 0 {
				fmt.Fprintf(&b, "const _imp_%s = require('./%s.js');\n", stem(imp.Path), stem(imp.Path))
				for _, n := range names {
					fmt.Fprintf(&b, "_CLASSES['%s'] = _imp_%s.%s;\n", n, stem(imp.Path), n)
				}
			}
		}
		b.WriteString("\n")
		for _, e := range f.Enums {
			name := className(f, e.Name)
			fmt.Fprintf(&b, "_ENUMS['%s'] = %s;\n_REVENUMS['%s'] = _REV_%s;\n", name, name, name, name)
		}
		b.WriteString("\n")
		for _, m := range f.Messages {
			writeJSClass(&b, f, m)
		}
		// Nested types are also reachable through their parents (Oneof1.Inner),
		// the way protobufjs static modules nest them. Message variables are
		// bare, enum variables carry the package prefix (see className).
		varOf := map[string]string{}
		for _, m := range f.Messages {
			varOf[m.Name] = m.Name
		}
		for _, e := range f.Enums {
			varOf[e.Name] = className(f, e.Name)
		}
		alias := func(flat string) {
			name := flat
			for {
				idx := strings.LastIndex(name, "_")
				if idx < 0 {
					return
				}
				parent, child := name[:idx], name[idx+1:]
				if pvar, ok := varOf[parent]; ok {
					fmt.Fprintf(&b, "%s.%s = %s;\n", pvar, child, varOf[flat])
					return
				}
				name = parent
			}
		}
		for _, m := range f.Messages {
			alias(m.Name)
		}
		for _, e := range f.Enums {
			alias(e.Name)
		}
		b.WriteString("\n")
		b.WriteString("module.exports = {\n")
		for _, e := range f.Enums {
			fmt.Fprintf(&b, "  %s,\n", className(f, e.Name))
		}
		for _, m := range f.Messages {
			fmt.Fprintf(&b, "  %s,\n", m.Name)
		}
		b.WriteString("};\n")
		files = append(files, File{Name: base(f) + ".js", Content: b.String()})
	}
	var meta strings.Builder
	writeJSFaceMeta(&meta, f, opt)
	files = append(files, File{Name: base(f) + "_meta.js", Content: meta.String()})
	return files
}

// writeJSFaceMeta emits the schema module: the embedded descriptor set plus
// an explicit loader. register=true loads lazily on first use (as before)
// with an override slot; otherwise init_schema (embedded bytes) or
// use_schema (external) must run first, loudly otherwise. registry adds the
// whole-file message list plus a preload that warms every type.
func writeJSFaceMeta(b *strings.Builder, f *model.File, opt Options) {
	b.WriteString(header(f, "Embedded descriptor set plus its loader: the schema side of the generated facade."))
	b.WriteString("'use strict';\n\n")
	b.WriteString("const { Schema } = require('../../lib/pbwire.js');\n\n")
	fmt.Fprintf(b, "const _DESC_B64 = '%s';\n\n", opt.DescSetB64)
	b.WriteString("let _schema;\n")
	b.WriteString("let _override;\n\n")
	b.WriteString("function init_schema(bytes) {\n")
	b.WriteString("  _schema = Schema.fromBytes(bytes || Buffer.from(_DESC_B64, 'base64'));\n")
	b.WriteString("}\n\n")
	b.WriteString("function use_schema(s) {\n")
	b.WriteString("  _override = (s == null ? undefined : s);\n")
	b.WriteString("}\n\n")
	if opt.Register {
		b.WriteString("function get_schema() {\n")
		b.WriteString("  if (_override !== undefined) return _override;\n")
		b.WriteString("  if (_schema === undefined) _schema = Schema.fromBytes(Buffer.from(_DESC_B64, 'base64'));\n")
		b.WriteString("  return _schema;\n")
		b.WriteString("}\n")
	} else {
		b.WriteString("function get_schema() {\n")
		b.WriteString("  if (_override !== undefined) return _override;\n")
		b.WriteString("  if (_schema !== undefined) return _schema;\n")
		b.WriteString("  throw new Error('call init_schema() first (generated with register=false)');\n")
		b.WriteString("}\n")
	}
	if opt.Registry {
		b.WriteString("\nconst MESSAGE_NAMES = [\n")
		for _, m := range f.Messages {
			fmt.Fprintf(b, "  '%s',\n", m.Full)
		}
		b.WriteString("];\n\n")
		b.WriteString("function preload() {\n")
		b.WriteString("  const s = get_schema();\n")
		b.WriteString("  for (const n of MESSAGE_NAMES) s.message(n);\n")
		b.WriteString("}\n")
	}
	b.WriteString("\nmodule.exports = { get_schema, init_schema, use_schema")
	if opt.Registry {
		b.WriteString(", MESSAGE_NAMES, preload")
	}
	b.WriteString(" };\n")
}

func writeJSClass(b *strings.Builder, f *model.File, m model.Message) {
	fmt.Fprintf(b, "/** %s */\n", m.Full)
	fmt.Fprintf(b, "const %s_FIELDS = [\n", m.Name)
	for _, field := range m.Fields {
		if field.Skip != "" {
			fmt.Fprintf(b, "  // skipped %s: %s\n", field.Name, field.Skip)
			continue
		}
		en := "null"
		if field.Kind == protoreflect.EnumKind && field.Enum != "" {
			en = "'" + className(f, field.Enum) + "'"
		}
		msg := "null"
		if field.Message != "" {
			msg = "'" + field.Message + "'"
		}
		oneof := "null"
		if field.Oneof != "" {
			oneof = "'" + field.Oneof + "'"
		}
		req := "false"
		if field.Required {
			req = "true"
		}
		has := "false"
		if (field.NeedsHas || field.Message != "") && !field.Required {
			has = "true"
		}
		closed := "false"
		if field.Closed {
			closed = "true"
		}
		fmt.Fprintf(b, "  { n: '%s', js: '%s', k: '%s', rep: %t, msg: %s, en: %s, oneof: %s, req: %s, has: %s, closed: %s },\n",
			field.Name, jsFieldName(field), jsKind(field), field.Repeated, msg, en, oneof, req, has, closed)
	}
	fmt.Fprintf(b, "];\n")
	fmt.Fprintf(b, "const %s = _makeClass('%s', get_schema, %s_FIELDS);\n",
		m.Name, m.Full, m.Name)
	fmt.Fprintf(b, "_CLASSES['%s'] = %s;\n", m.Name, m.Name)
	fmt.Fprintf(b, "%s._ENUMS = _ENUMS;\n%s._REVENUMS = _REVENUMS;\n%s._CLASSES = _CLASSES;\n\n", m.Name, m.Name, m.Name)
}

func tsType(field model.Field) string {
	base := ""
	switch field.Kind {
	case protoreflect.Int32Kind, protoreflect.Sint32Kind, protoreflect.Sfixed32Kind,
		protoreflect.Uint32Kind, protoreflect.Fixed32Kind, protoreflect.EnumKind:
		base = "number"
	case protoreflect.Int64Kind, protoreflect.Sint64Kind, protoreflect.Sfixed64Kind,
		protoreflect.Uint64Kind, protoreflect.Fixed64Kind:
		base = "bigint"
	case protoreflect.FloatKind, protoreflect.DoubleKind:
		base = "number"
	case protoreflect.BoolKind:
		base = "boolean"
	case protoreflect.StringKind:
		base = "string"
	case protoreflect.BytesKind:
		base = "Uint8Array"
	default:
		base = field.Message
	}
	if field.Repeated {
		return base + "[]"
	}
	if field.Message != "" {
		return base + " | null"
	}
	return base
}

// writeTSNamespace re-exports nested types through their parent
// (Person.Inner), mirroring the runtime aliases.
func writeTSNamespace(b *strings.Builder, f *model.File, m model.Message) {
	declOf := map[string]string{}
	for _, mm := range f.Messages {
		declOf[mm.Name] = mm.Name
	}
	for _, e := range f.Enums {
		declOf[e.Name] = className(f, e.Name)
	}
	prefix := m.Name + "_"
	var kids [][2]string
	for _, mm := range f.Messages {
		if strings.HasPrefix(mm.Name, prefix) && !strings.Contains(mm.Name[len(prefix):], "_") {
			kids = append(kids, [2]string{mm.Name[len(prefix):], declOf[mm.Name]})
		}
	}
	for _, e := range f.Enums {
		if strings.HasPrefix(e.Name, prefix) && !strings.Contains(e.Name[len(prefix):], "_") {
			kids = append(kids, [2]string{e.Name[len(prefix):], declOf[e.Name]})
		}
	}
	if len(kids) == 0 {
		return
	}
	fmt.Fprintf(b, "export declare namespace %s {\n", m.Name)
	for _, k := range kids {
		fmt.Fprintf(b, "  export const %s: typeof %s;\n", k[0], k[1])
	}
	b.WriteString("}\n")
}

// typeScriptFiles emits the .d.ts for the facade: classes with static
// create/encode/decode/verify/fromObject/toObject plus the enum constants.
func typeScriptFiles(f *model.File, opt Options) []File {
	// Declarations describe the models; without them there is nothing to declare.
	if !opt.Model {
		return nil
	}
	var b strings.Builder
	b.WriteString(header(f, "TypeScript declarations for the protobufjs-static-shaped facade."))
	for _, imp := range f.Imports {
		if names := foreignMessageNames(f, imp.Path); len(names) > 0 {
			fmt.Fprintf(&b, "import type { %s } from \"./%s\";\n", strings.Join(names, ", "), stem(imp.Path))
		}
	}
	if len(f.Imports) > 0 {
		b.WriteString("\n")
	}
	for _, e := range f.Enums {
		fmt.Fprintf(&b, "/** %s */\nexport declare const %s: {\n", e.Full, className(f, e.Name))
		for _, v := range e.Values {
			fmt.Fprintf(&b, "  readonly %s: %d;\n", v.Name, v.Number)
		}
		b.WriteString("};\n\n")
	}
	for _, m := range f.Messages {
		fmt.Fprintf(&b, "/** %s */\nexport declare class %s {\n", m.Full, m.Name)
		for _, field := range m.Fields {
			if field.Skip != "" {
				fmt.Fprintf(&b, "  // skipped %s: %s\n", field.Name, field.Skip)
				continue
			}
			t := tsType(field)
			q := ""
			if !field.Required {
				q = "?"
			}
			fmt.Fprintf(&b, "  %s%s: %s;\n", jsFieldName(field), q, t)
		}
		b.WriteString("  constructor(props?: { [key: string]: any });\n")
		fmt.Fprintf(&b, "  static create(props?: { [key: string]: any }): %s;\n", m.Name)
		b.WriteString("  static encode(message: any, writer?: any): { finish(): Uint8Array };\n")
		b.WriteString("  static encodeDelimited(message: any): { finish(): Uint8Array };\n")
		fmt.Fprintf(&b, "  static decode(bytes: Uint8Array): %s;\n", m.Name)
		fmt.Fprintf(&b, "  static decodeDelimited(bytes: Uint8Array): %s;\n", m.Name)
		b.WriteString("  static verify(obj: any): string | null;\n")
		fmt.Fprintf(&b, "  static fromObject(obj: any): %s;\n", m.Name)
		fmt.Fprintf(&b, "  static toObject(message: any, opts?: { defaults?: boolean; enums?: string; longs?: string; bytes?: string }): { [key: string]: any };\n")
		b.WriteString("  toObject(opts?: any): { [key: string]: any };\n")
		b.WriteString("  toJSON(): { [key: string]: any };\n")
		b.WriteString("}\n")
		writeTSNamespace(&b, f, m)
		b.WriteString("\n")
	}
	return []File{{Name: base(f) + ".d.ts", Content: b.String()}}
}

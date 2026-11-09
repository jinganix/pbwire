package gen

import (
	"fmt"
	"strings"

	"pbwire-codegen/internal/model"

	"google.golang.org/protobuf/reflect/protoreflect"
)

// fsharpFiles emits one F# file: records over the C# P/Invoke runtime (the
// same runtime F# reuses: Schema/Message/MessageType). The descriptor set
// travels embedded as base64, so no .desc file is needed at use time.
// Oneof members are plain options.
func fsharpFiles(f *model.File, opt Options) ([]File, error) {
	// Models and schema part ways: records in one module, the embedded
	// descriptor set plus its loader in another. model=false leaves the
	// schema module for dynamic-only users.
	var files []File
	if opt.Model {
		var b strings.Builder
		b.WriteString(header(f, "Records over the C# P/Invoke runtime (Schema/Message, no codegen at use time)."))
		// `module rec`: message graphs can be mutually recursive (a nested
		// message can reference its parent), and F# needs declaration before
		// use otherwise. External paths are unchanged.
		b.WriteString("module rec " + fsharpModule(f) + "\n\n")
		b.WriteString("open System\nopen Pbwire\n")
		for _, imp := range f.Imports {
			fmt.Fprintf(&b, "open %s\n", fsharpModuleName(stem(imp.Path)))
		}
		b.WriteString("\n")
		for _, e := range f.Enums {
			fmt.Fprintf(&b, "/// %s\n", e.Full)
			fmt.Fprintf(&b, "module %s =\n", className(f, e.Name))
			for _, v := range e.Values {
				fmt.Fprintf(&b, "    let %s: int = %d\n", v.Name, v.Number)
			}
			b.WriteString("\n")
		}
		// F# needs declaration before use, so messages go out in dependency
		// order (nested types already come first; siblings are sorted here).
		for _, m := range fsharpOrder(f) {
			writeFSharpMessage(&b, f, m)
		}
		files = append(files, File{Name: base(f) + ".fs", Content: b.String()})
	}
	var meta strings.Builder
	writeFSharpMeta(&meta, f, opt)
	files = append(files, File{Name: base(f) + "_meta.fs", Content: meta.String()})
	return files, nil
}

// writeFSharpMeta emits the schema module: the embedded descriptor set plus
// an explicit loader. register=true loads eagerly (as before); otherwise
// initSchema (embedded bytes) or useSchema (an external schema) must run
// first, loudly otherwise. registry adds the whole-file message list plus
// a preload that warms every type.
func writeFSharpMeta(b *strings.Builder, f *model.File, opt Options) {
	meta := fsharpModule(f) + "Meta"
	b.WriteString(header(f, "Embedded descriptor set plus its loader: the schema side of the generated model."))
	fmt.Fprintf(b, "module %s\n\n", meta)
	b.WriteString("open System\nopen Pbwire\n\n")
	b.WriteString("let desc: byte[] = Convert.FromBase64String(\"\"\"\n")
	for i := 0; i < len(opt.DescSetB64); i += 76 {
		end := i + 76
		if end > len(opt.DescSetB64) {
			end = len(opt.DescSetB64)
		}
		b.WriteString(opt.DescSetB64[i:end] + "\n")
	}
	b.WriteString("\"\"\")\n\n")
	b.WriteString("let mutable private _schema: Schema = null\n")
	b.WriteString("let private _schemaLock = obj()\n\n")
	b.WriteString("/// Loads the embedded descriptor set and owns the resulting schema; a\n")
	b.WriteString("/// previous one loaded here is disposed.\n")
	b.WriteString("let initSchema (bytes: byte[]): unit =\n")
	b.WriteString("    let fresh = Schema.FromBytes(bytes)\n")
	b.WriteString("    lock _schemaLock (fun () ->\n")
	b.WriteString("        let old = _schema\n")
	b.WriteString("        _schema <- fresh\n")
	b.WriteString("        if not (isNull old) then old.Dispose())\n\n")
	b.WriteString("/// Registers an externally loaded schema: models encode/decode through it\n")
	b.WriteString("/// instead of the embedded set. The caller owns it; null restores the\n")
	b.WriteString("/// embedded set (call initSchema to reload).\n")
	b.WriteString("let useSchema (s: Schema): unit = _schema <- s\n\n")
	b.WriteString("let getSchema (): Schema =\n")
	b.WriteString("    match _schema with\n")
	b.WriteString("    | null -> failwith \"call initSchema or useSchema first (generated with register=false)\"\n")
	b.WriteString("    | s -> s\n")
	if opt.Register {
		b.WriteString("\ndo initSchema desc\n")
	}
	if opt.Registry {
		b.WriteString("\n/// Every message in this file, for whole-schema pre-registration.\n")
		b.WriteString("let messageNames: string list = [\n")
		for _, m := range f.Messages {
			fmt.Fprintf(b, "    %q\n", m.Full)
		}
		b.WriteString("]\n\n")
		b.WriteString("/// Warms every message type, so later calls pay no lookup.\n")
		b.WriteString("let preload (): unit = messageNames |> List.iter (fun full -> getSchema().Message(full) |> ignore)\n")
	}
}

func fsharpModule(f *model.File) string { return fsharpModuleName(base(f)) }

// fsharpModuleName maps a file stem to its module (xdep -> Xdep).
// fsMsg names the record module for a message field: the owning file's
// prefix when foreign (its module is opened above), this file's own
// otherwise.
func fsMsg(f *model.File, field model.Field) string {
	if field.Import != "" {
		if imp := findImport(f, field.Import); imp != nil {
			return imp.Prefix + "_" + field.Message
		}
	}
	return className(f, field.Message)
}

func fsharpModuleName(p string) string {
	var b strings.Builder
	upper := true
	for _, r := range p {
		if r == '_' || r == '-' || r == '.' {
			upper = true
			continue
		}
		if upper {
			b.WriteRune([]rune(strings.ToUpper(string(r)))[0])
			upper = false
			continue
		}
		b.WriteRune(r)
	}
	if b.Len() == 0 {
		return "PbwireGen"
	}
	return b.String()
}

func fsharpType(f *model.File, field model.Field) string {
	if field.Message != "" {
		return fsMsg(f, field)
	}
	switch field.Kind {
	case protoreflect.Int32Kind, protoreflect.Sint32Kind, protoreflect.Sfixed32Kind:
		return "int"
	case protoreflect.Int64Kind, protoreflect.Sint64Kind, protoreflect.Sfixed64Kind:
		return "int64"
	case protoreflect.Uint32Kind, protoreflect.Fixed32Kind:
		return "uint32"
	case protoreflect.Uint64Kind, protoreflect.Fixed64Kind:
		return "uint64"
	case protoreflect.FloatKind:
		return "float32"
	case protoreflect.DoubleKind:
		return "float"
	case protoreflect.BoolKind:
		return "bool"
	case protoreflect.StringKind:
		return "string"
	case protoreflect.BytesKind:
		return "byte[]"
	case protoreflect.EnumKind:
		return "int"
	default:
		return "int64"
	}
}

func fsharpFieldType(f *model.File, field model.Field) string {
	t := fsharpType(f, field)
	if field.Repeated {
		return t + " list"
	}
	if field.Message != "" || field.NeedsHas {
		return t + " option"
	}
	return t
}

func fsharpDefault(f *model.File, field model.Field) string {
	if field.Repeated {
		return "[]"
	}
	if field.Message != "" || field.NeedsHas {
		return "None"
	}
	switch fsharpType(f, field) {
	case "int":
		return "0"
	case "int64":
		return "0L"
	case "uint32":
		return "0u"
	case "uint64":
		return "0UL"
	case "float32":
		return "0.0f"
	case "float":
		return "0.0"
	case "bool":
		return "false"
	case "string":
		return "\"\""
	case "byte[]":
		return "[||]"
	default:
		return "0"
	}
}

func writeFSharpMessage(b *strings.Builder, f *model.File, m model.Message) {
	cls := className(f, m.Name)
	fmt.Fprintf(b, "/// %s\n", m.Full)
	fields := []model.Field{}
	for _, field := range m.Fields {
		if field.Skip == "" {
			fields = append(fields, field)
		} else {
			fmt.Fprintf(b, "(* skipped %s: %s *)\n", field.Name, field.Skip)
		}
	}
	if len(fields) == 0 {
		// F# has no empty records: an empty message is just unit. The
		// wire behavior is identical (encode always empty, decode drops).
		fmt.Fprintf(b, "type %s = unit\n\n", cls)
		fmt.Fprintf(b, "module %s =\n", cls)
		fmt.Fprintf(b, "    let private ty (): MessageType = %s.getSchema().Message(%q)\n\n", fsharpModule(f)+"Meta", m.Full)
		fmt.Fprintf(b, "    let defaultValue : %s = ()\n\n", cls)
		fmt.Fprintf(b, "    let fill (_msg: Message) (_v: %s) : unit = ()\n\n", cls)
		fmt.Fprintf(b, "    let encode (v: %s) : byte[] =\n", cls)
		b.WriteString("        use msg = ty().NewMessage()\n")
		b.WriteString("        fill msg v\n")
		b.WriteString("        msg.Encode()\n\n")
		fmt.Fprintf(b, "    let load (_msg: Message) : %s = ()\n\n", cls)
		fmt.Fprintf(b, "    let decode (wire: byte[]) : %s =\n", cls)
		b.WriteString("        use msg = ty().Decode(ReadOnlySpan wire)\n")
		b.WriteString("        load msg\n\n")
		return
	}
	fmt.Fprintf(b, "type %s =\n", cls)
	b.WriteString("    {")
	for i, field := range fields {
		sep := ";"
		if i == 0 {
			sep = " "
		}
		fmt.Fprintf(b, "\n    %s %s: %s", sep, fsharpProp(field), fsharpFieldType(f, field))
	}
	b.WriteString("\n    }\n\n")
	fmt.Fprintf(b, "module %s =\n", cls)
	fmt.Fprintf(b, "    let private ty (): MessageType = %s.getSchema().Message(%q)\n\n", fsharpModule(f)+"Meta", m.Full)
	fmt.Fprintf(b, "    let defaultValue : %s =\n", cls)
	b.WriteString("        {")
	for i, field := range fields {
		sep := ";"
		if i == 0 {
			sep = " "
		}
		fmt.Fprintf(b, "\n            %s %s = %s", sep, fsharpProp(field), fsharpDefault(f, field))
	}
	b.WriteString("\n        }\n\n")
	fmt.Fprintf(b, "    let rec fill (msg: Message) (v: %s) : unit =\n", cls)
	for _, field := range fields {
		writeFSharpFill(b, f, field)
	}
	fmt.Fprintf(b, "\n    let encode (v: %s) : byte[] =\n", cls)
	b.WriteString("        use msg = ty().NewMessage()\n")
	b.WriteString("        fill msg v\n")
	b.WriteString("        msg.Encode()\n\n")
	fmt.Fprintf(b, "    let rec load (msg: Message) : %s =\n", cls)
	b.WriteString("        {")
	for i, field := range fields {
		sep := ";"
		if i == 0 {
			sep = " "
		}
		fmt.Fprintf(b, "\n            %s %s = %s", sep, fsharpProp(field), fsharpLoad(f, field))
	}
	b.WriteString("\n        }\n\n")
	fmt.Fprintf(b, "    let decode (wire: byte[]) : %s =\n", cls)
	b.WriteString("        use msg = ty().Decode(ReadOnlySpan wire)\n")
	b.WriteString("        load msg\n\n")
}

// fsharpOrder returns the file's messages with dependencies first (stable:
// a message's module calls the fill/load of every message-typed field, so
// those modules must be defined above). Cycles keep protoc order.
func fsharpOrder(f *model.File) []model.Message {
	index := map[string]int{}
	for i, m := range f.Messages {
		index[m.Name] = i
	}
	deps := map[string][]string{}
	for _, m := range f.Messages {
		seen := map[string]bool{}
		for _, field := range m.Fields {
			if field.Skip != "" || field.Message == "" || seen[field.Message] {
				continue
			}
			seen[field.Message] = true
			if _, ok := index[field.Message]; ok {
				deps[m.Name] = append(deps[m.Name], field.Message)
			}
		}
	}
	var out []model.Message
	done := map[string]bool{}
	visiting := map[string]bool{}
	var visit func(name string)
	visit = func(name string) {
		if done[name] || visiting[name] {
			return
		}
		visiting[name] = true
		for _, d := range deps[name] {
			visit(d)
		}
		delete(visiting, name)
		done[name] = true
		out = append(out, f.Messages[index[name]])
	}
	for _, m := range f.Messages {
		visit(m.Name)
	}
	return out
}

// fsharpProp keeps the proto field name (lowercase, no clash with keywords).
func fsharpProp(field model.Field) string {
	if field.Name == "type" {
		return "``type``"
	}
	return field.Name
}

func writeFSharpFill(b *strings.Builder, f *model.File, field model.Field) {
	name := fsharpProp(field)
	proto := field.Name
	switch {
	case field.Message != "" && !field.Repeated:
		sub := fsMsg(f, field)
		fmt.Fprintf(b, "        v.%s |> Option.iter (fun x -> %s.fill (msg.SetMessage(%q)) x)\n", name, sub, proto)
	case field.Message != "" && field.Repeated:
		sub := fsMsg(f, field)
		fmt.Fprintf(b, "        v.%s |> List.iter (fun x -> %s.fill (msg.SetMessage(%q)) x)\n", name, sub, proto)
	case field.Repeated:
		fmt.Fprintf(b, "        v.%s |> List.iter (fun x -> msg.Set(%q, %s) |> ignore)\n", name, proto, fsharpScalar(field, "x"))
	case field.NeedsHas:
		fmt.Fprintf(b, "        v.%s |> Option.iter (fun x -> msg.Set(%q, %s) |> ignore)\n", name, proto, fsharpScalar(field, "x"))
	case field.Required:
		fmt.Fprintf(b, "        msg.Set(%q, %s) |> ignore\n", proto, fsharpScalar(field, "v."+name))
	default:
		// Proto3 implicit: zero stays off the wire.
		fmt.Fprintf(b, "        %s\n", fsharpGuard(field, name, proto))
	}
}

func fsharpScalar(field model.Field, src string) string {
	switch field.Kind {
	case protoreflect.BoolKind:
		return src
	case protoreflect.FloatKind:
		return src
	case protoreflect.DoubleKind:
		return src
	case protoreflect.StringKind:
		return src
	case protoreflect.BytesKind:
		return "ReadOnlySpan " + src
	default:
		return "int64 " + src
	}
}

// fsharpGuard renders the write guard for an implicit-presence scalar.
func fsharpGuard(field model.Field, name, proto string) string {
	set := fmt.Sprintf("msg.Set(%q, %s) |> ignore", proto, fsharpScalar(field, "v."+name))
	switch field.Kind {
	case protoreflect.BoolKind:
		return fmt.Sprintf("if v.%s then %s", name, set)
	case protoreflect.StringKind:
		return fmt.Sprintf("if v.%s <> \"\" then %s", name, set)
	case protoreflect.BytesKind:
		return fmt.Sprintf("if v.%s.Length <> 0 then %s", name, set)
	default:
		return fmt.Sprintf("if v.%s <> %s then %s", name, fsharpZero(field), set)
	}
}

// fsharpZero is the zero literal of a scalar field's F# type.
func fsharpZero(field model.Field) string {
	switch field.Kind {
	case protoreflect.Int32Kind, protoreflect.Sint32Kind, protoreflect.Sfixed32Kind, protoreflect.EnumKind:
		return "0"
	case protoreflect.Uint32Kind, protoreflect.Fixed32Kind:
		return "0u"
	case protoreflect.Int64Kind, protoreflect.Sint64Kind, protoreflect.Sfixed64Kind:
		return "0L"
	case protoreflect.Uint64Kind, protoreflect.Fixed64Kind:
		return "0UL"
	case protoreflect.FloatKind:
		return "0.0f"
	case protoreflect.DoubleKind:
		return "0.0"
	default:
		return "0"
	}
}

func fsharpLoad(f *model.File, field model.Field) string {
	proto := field.Name
	switch {
	case field.Message != "" && !field.Repeated:
		sub := fsMsg(f, field)
		return fmt.Sprintf("match msg.GetMessage(%q) with null -> None | n -> Some (%s.load n)", proto, sub)
	case field.Message != "" && field.Repeated:
		sub := fsMsg(f, field)
		return fmt.Sprintf("[ for i in 0 .. msg.Count(%q) - 1 -> match msg.At(%q, i) with null -> failwith %q | v -> match v.AsMessage() with null -> failwith %q | n -> %s.load n ]", proto, proto, proto, proto, sub)
	case field.Repeated:
		return fmt.Sprintf("[ for i in 0 .. msg.Count(%q) - 1 -> match msg.At(%q, i) with null -> failwith %q | v -> %s ]", proto, proto, proto, fsharpItem(field))
	case field.NeedsHas:
		return fmt.Sprintf("if msg.Has(%q) then Some (%s) else None", proto, fsharpGet(field))
	default:
		return fsharpGet(field)
	}
}

// fsharpItem renders one repeated-item load from a C# Value, converted
// to the record field type (Value accessors return int64/string).
func fsharpItem(field model.Field) string {
	switch field.Kind {
	case protoreflect.BoolKind:
		return "v.AsBool()"
	case protoreflect.FloatKind:
		return "v.AsFloat()"
	case protoreflect.DoubleKind:
		return "v.AsDouble()"
	case protoreflect.StringKind:
		return "v.AsString() |> Option.ofObj |> Option.defaultValue \"\""
	case protoreflect.BytesKind:
		return "v.AsBytes() |> Option.ofObj |> Option.defaultValue [||]"
	case protoreflect.Int32Kind, protoreflect.Sint32Kind, protoreflect.Sfixed32Kind, protoreflect.EnumKind:
		return "int (v.AsLong())"
	case protoreflect.Uint32Kind, protoreflect.Fixed32Kind:
		return "uint32 (v.AsLong())"
	case protoreflect.Uint64Kind, protoreflect.Fixed64Kind:
		return "uint64 (v.AsLong())"
	default:
		return "v.AsLong()"
	}
}

func fsharpGet(field model.Field) string {
	proto := field.Name
	switch field.Kind {
	case protoreflect.BoolKind:
		return fmt.Sprintf("msg.GetBool(%q)", proto)
	case protoreflect.FloatKind:
		return fmt.Sprintf("msg.GetFloat(%q)", proto)
	case protoreflect.DoubleKind:
		return fmt.Sprintf("msg.GetDouble(%q)", proto)
	case protoreflect.StringKind:
		return fmt.Sprintf("msg.GetString(%q) |> Option.ofObj |> Option.defaultValue \"\"", proto)
	case protoreflect.BytesKind:
		return fmt.Sprintf("msg.GetBytes(%q) |> Option.ofObj |> Option.defaultValue [||]", proto)
	case protoreflect.Int32Kind, protoreflect.Sint32Kind, protoreflect.Sfixed32Kind, protoreflect.EnumKind:
		return fmt.Sprintf("int (msg.GetLong(%q))", proto)
	case protoreflect.Uint32Kind, protoreflect.Fixed32Kind:
		return fmt.Sprintf("uint32 (msg.GetLong(%q))", proto)
	case protoreflect.Uint64Kind, protoreflect.Fixed64Kind:
		return fmt.Sprintf("uint64 (msg.GetLong(%q))", proto)
	default:
		return fmt.Sprintf("msg.GetLong(%q)", proto)
	}
}

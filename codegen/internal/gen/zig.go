package gen

import (
	"encoding/base64"
	"fmt"
	"strings"

	"pbwire-codegen/internal/model"

	"google.golang.org/protobuf/reflect/protoreflect"
)

// zigFiles emits one Zig file: generated structs over the Zig static-link runtime
// (runtime/pbwire.zig: Schema/Message/Field). The descriptor set travels
// embedded, so no .desc file is needed at use time. Oneof members are plain
// optionals. Slices decoded from the wire are dupe'd with the caller's
// allocator.
func zigFiles(f *model.File, opt Options) []File {
	var b strings.Builder
	b.WriteString(header(f, "Generated structs over the Zig static-link runtime (Schema/Message)."))
	b.WriteString("const std = @import(\"std\");\n")
	b.WriteString("const pbwire = @import(\"pbwire\");\n")
	for _, imp := range f.Imports {
		fmt.Fprintf(&b, "const %s = @import(\"%s.zig\");\n", stem(imp.Path), stem(imp.Path))
	}
	b.WriteString("\n")
	b.WriteString("const DESC: []const u8 = \"")
	raw, _ := base64.StdEncoding.DecodeString(opt.DescSetB64)
	for _, v := range raw {
		fmt.Fprintf(&b, "\\x%02x", v)
	}
	b.WriteString("\";\n\n")
	b.WriteString("pub fn loadSchema() pbwire.Error!pbwire.Schema {\n")
	b.WriteString("    return pbwire.Schema.fromBytes(DESC);\n}\n\n")
	for _, e := range f.Enums {
		fmt.Fprintf(&b, "// %s\n", e.Full)
		fmt.Fprintf(&b, "pub const %s = enum(i32) {\n", className(f, e.Name))
		for _, v := range e.Values {
			fmt.Fprintf(&b, "    %s = %d,\n", v.Name, v.Number)
		}
		b.WriteString("};\n\n")
	}
	for _, m := range f.Messages {
		writeZigMessage(&b, f, m)
	}
	return []File{{Name: base(f) + ".zig", Content: b.String()}}
}

// zigMsg names the struct for a message field: the sibling module's type
// when foreign (imported above), this file's own otherwise.
func zigMsg(f *model.File, field model.Field) string {
	if field.Import != "" {
		if imp := findImport(f, field.Import); imp != nil {
			return stem(imp.Path) + "." + imp.Prefix + "_" + field.Message
		}
	}
	return className(f, field.Message)
}

func zigType(f *model.File, field model.Field) string {
	if field.Message != "" {
		return zigMsg(f, field)
	}
	switch field.Kind {
	case protoreflect.Int32Kind, protoreflect.Sint32Kind, protoreflect.Sfixed32Kind:
		return "i32"
	case protoreflect.Int64Kind, protoreflect.Sint64Kind, protoreflect.Sfixed64Kind:
		return "i64"
	case protoreflect.Uint32Kind, protoreflect.Fixed32Kind:
		return "u32"
	case protoreflect.Uint64Kind, protoreflect.Fixed64Kind:
		return "u64"
	case protoreflect.FloatKind:
		return "f32"
	case protoreflect.DoubleKind:
		return "f64"
	case protoreflect.BoolKind:
		return "bool"
	case protoreflect.StringKind, protoreflect.BytesKind:
		return "[]const u8"
	case protoreflect.EnumKind:
		if field.Enum != "" {
			return className(f, field.Enum)
		}
		return "i32"
	default:
		return "i64"
	}
}

// zigProp is the Zig struct field name; `type` is a keyword, so it moves
// aside (the proto name in quotes is unchanged).
func zigProp(field model.Field) string {
	if field.Name == "type" {
		return "type_"
	}
	return field.Name
}

func zigFieldType(f *model.File, field model.Field) string {
	t := zigType(f, field)
	if field.Repeated {
		return "[]const " + t
	}
	if field.Message != "" || field.NeedsHas {
		return "?" + t
	}
	return t
}

func zigZero(t string) string {
	switch t {
	case "i32", "i64", "u32", "u64":
		return "0"
	case "f32", "f64":
		return "0"
	case "bool":
		return "false"
	case "[]const u8":
		return "\"\""
	default:
		if strings.HasPrefix(t, "[]const ") {
			return "&[_]u8{}"
		}
		if strings.HasPrefix(t, "?") {
			return "null"
		}
		return "0"
	}
}

func writeZigMessage(b *strings.Builder, f *model.File, m model.Message) {
	cls := className(f, m.Name)
	fmt.Fprintf(b, "// %s\n", m.Full)
	fmt.Fprintf(b, "pub const %s = struct {\n", cls)
	for _, field := range m.Fields {
		if field.Skip != "" {
			fmt.Fprintf(b, "    // skipped %s: %s\n", field.Name, field.Skip)
			continue
		}
		ft := zigFieldType(f, field)
		prop := zigProp(field)
		if field.Repeated {
			fmt.Fprintf(b, "    %s: %s = &[_]%s{},\n", prop, ft, zigType(f, field))
		} else if field.Message != "" || field.NeedsHas {
			fmt.Fprintf(b, "    %s: %s = null,\n", prop, ft)
		} else {
			fmt.Fprintf(b, "    %s: %s = %s,\n", prop, ft, zigDefault(f, field))
		}
	}
	b.WriteString("\n")
	// Zig 0.17 rejects `_ = x` for a parameter that is used, and rejects
	// unused named parameters: anonymous `_ : T` discards exactly the
	// parameters this message never touches.
	hasMsg, needsAlloc, hasField := false, false, false
	for _, field := range m.Fields {
		if field.Skip != "" {
			continue
		}
		hasField = true
		if field.Message != "" {
			hasMsg = true
		}
		if field.Repeated || field.Message != "" || field.Kind == protoreflect.StringKind || field.Kind == protoreflect.BytesKind {
			needsAlloc = true
		}
	}
	fillAlloc, fillSchema, fillMsg, fillTy, fillSelf := "_ : std.mem.Allocator", "_ : *const pbwire.Schema", "msg: *pbwire.Message", "ty: pbwire.MessageType", "self: *const "+cls
	if hasMsg {
		fillAlloc = "allocator: std.mem.Allocator"
		fillSchema = "schema: *const pbwire.Schema"
	}
	if !hasField {
		fillMsg = "_ : *pbwire.Message"
		fillTy = "_ : pbwire.MessageType"
		fillSelf = "_ : *const " + cls
	}
	loadAlloc, loadSchema, loadMsg, loadTy := "allocator: std.mem.Allocator", "schema: *const pbwire.Schema", "msg: *const pbwire.Message", "ty: pbwire.MessageType"
	if !needsAlloc {
		loadAlloc = "_ : std.mem.Allocator"
	}
	if !hasMsg {
		loadSchema = "_ : *const pbwire.Schema"
	}
	if !hasField {
		loadMsg = "_ : *const pbwire.Message"
		loadTy = "_ : pbwire.MessageType"
	}
	// encode fills a dynamic message and returns wire bytes.
	fmt.Fprintf(b, "    pub fn encode(self: *const %s, allocator: std.mem.Allocator, schema: *const pbwire.Schema) ![]u8 {\n", cls)
	fmt.Fprintf(b, "        const ty = try schema.messageType(%q);\n", m.Full)
	b.WriteString("        var msg = try ty.newMessage();\n")
	b.WriteString("        defer msg.deinit();\n")
	b.WriteString("        try self.fill(allocator, schema, &msg, ty);\n")
	b.WriteString("        return msg.encode(allocator);\n")
	b.WriteString("    }\n\n")
	fmt.Fprintf(b, "    pub fn fill(%s, %s, %s, %s, %s) !void {\n", fillSelf, fillAlloc, fillSchema, fillMsg, fillTy)
	for _, field := range m.Fields {
		if field.Skip != "" {
			continue
		}
		writeZigFill(b, f, field)
	}
	b.WriteString("    }\n\n")
	fmt.Fprintf(b, "    pub fn decode(allocator: std.mem.Allocator, schema: *const pbwire.Schema, wire: []const u8) !%s {\n", cls)
	fmt.Fprintf(b, "        const ty = try schema.messageType(%q);\n", m.Full)
	b.WriteString("        var msg = try ty.decode(wire);\n")
	b.WriteString("        defer msg.deinit();\n")
	b.WriteString("        return load(allocator, schema, &msg, ty);\n")
	b.WriteString("    }\n\n")
	fmt.Fprintf(b, "    pub fn load(%s, %s, %s, %s) !%s {\n", loadAlloc, loadSchema, loadMsg, loadTy, cls)
	if hasField {
		b.WriteString("        var out: " + cls + " = .{};\n")
	} else {
		b.WriteString("        const out: " + cls + " = .{};\n")
	}
	for _, field := range m.Fields {
		if field.Skip != "" {
			continue
		}
		writeZigLoad(b, f, field)
	}
	b.WriteString("        return out;\n")
	b.WriteString("    }\n};\n\n")
}

func writeZigFill(b *strings.Builder, f *model.File, field model.Field) {
	name := zigProp(field)
	proto := field.Name
	switch {
	case field.Message != "" && !field.Repeated:
		fmt.Fprintf(b, "        if (self.%s) |v| {\n", name)
		fmt.Fprintf(b, "            const f = try ty.field(%q);\n", proto)
		b.WriteString("            var sub = try msg.setMessage(f);\n")
		fmt.Fprintf(b, "            const sty = try schema.messageType(%q);\n", zigFullName(f, field))
		b.WriteString("            try v.fill(allocator, schema, &sub, sty);\n")
		b.WriteString("        }\n")
	case field.Message != "" && field.Repeated:
		fmt.Fprintf(b, "        for (self.%s) |v| {\n", name)
		fmt.Fprintf(b, "            const f = try ty.field(%q);\n", proto)
		b.WriteString("            var sub = try msg.setMessage(f);\n")
		fmt.Fprintf(b, "            const sty = try schema.messageType(%q);\n", zigFullName(f, field))
		b.WriteString("            try v.fill(allocator, schema, &sub, sty);\n")
		b.WriteString("        }\n")
	case field.Repeated:
		fmt.Fprintf(b, "        {\n")
		fmt.Fprintf(b, "            const f = try ty.field(%q);\n", proto)
		fmt.Fprintf(b, "            for (self.%s) |v| { try %s; }\n", name, zigSetScalar(field, "msg", "f", "v"))
		b.WriteString("        }\n")
	case field.NeedsHas:
		fmt.Fprintf(b, "        if (self.%s) |v| {\n", name)
		fmt.Fprintf(b, "            const f = try ty.field(%q);\n", proto)
		fmt.Fprintf(b, "            try %s;\n", zigSetScalar(field, "msg", "f", "v"))
		b.WriteString("        }\n")
	case field.Required:
		fmt.Fprintf(b, "        {\n")
		fmt.Fprintf(b, "            const f = try ty.field(%q);\n", proto)
		fmt.Fprintf(b, "            try %s;\n", zigSetScalar(field, "msg", "f", zigSelf(field)))
		b.WriteString("        }\n")
	default:
		// Proto3 implicit: zero stays off the wire.
		fmt.Fprintf(b, "        %s\n", zigGuard(field, proto))
	}
}

func zigSelf(field model.Field) string {
	return "self." + zigProp(field)
}

// zigDefault is the default value for a non-optional struct field.
// Enum fields default to their first enumerator (an integer zero may not
// be a valid value); everything else uses the type zero.
func zigDefault(f *model.File, field model.Field) string {
	if field.Kind == protoreflect.EnumKind && field.Enum != "" {
		for _, e := range f.Enums {
			if e.Name == field.Enum && len(e.Values) > 0 {
				return "." + e.Values[0].Name
			}
		}
	}
	return zigZero(zigFieldType(f, field))
}

// zigGuard renders the write guard for an implicit-presence scalar.
func zigGuard(field model.Field, proto string) string {
	cond := ""
	switch field.Kind {
	case protoreflect.BoolKind:
		cond = "self." + zigProp(field)
	case protoreflect.StringKind, protoreflect.BytesKind:
		cond = "self." + zigProp(field) + ".len > 0"
	case protoreflect.EnumKind:
		cond = "@intFromEnum(self." + zigProp(field) + ") != 0"
	default:
		cond = "self." + zigProp(field) + " != 0"
	}
	return "if (" + cond + ") {\n" +
		"            const f = try ty.field(\"" + proto + "\");\n" +
		"            try " + zigSetScalar(field, "msg", "f", zigSelf(field)) + ";\n" +
		"        }"
}

// zigSetScalar renders one setter call for a scalar value expression.
func zigSetScalar(field model.Field, msg, f, src string) string {
	switch field.Kind {
	case protoreflect.BoolKind:
		return fmt.Sprintf("%s.setBool(%s, %s)", msg, f, src)
	case protoreflect.FloatKind:
		return fmt.Sprintf("%s.setF32(%s, %s)", msg, f, src)
	case protoreflect.DoubleKind:
		return fmt.Sprintf("%s.setF64(%s, %s)", msg, f, src)
	case protoreflect.StringKind:
		return fmt.Sprintf("%s.setStr(%s, %s)", msg, f, src)
	case protoreflect.BytesKind:
		return fmt.Sprintf("%s.setBytes(%s, %s)", msg, f, src)
	case protoreflect.EnumKind:
		return fmt.Sprintf("%s.setI64(%s, @intCast(@intFromEnum(%s)))", msg, f, src)
	default:
		// Every integer kind: raw 64-bit bits. u64 wraps with a
		// bit-cast (values above maxInt(i64) cannot @intCast).
		if field.Kind == protoreflect.Uint64Kind || field.Kind == protoreflect.Fixed64Kind {
			return fmt.Sprintf("%s.setI64(%s, @bitCast(%s))", msg, f, src)
		}
		return fmt.Sprintf("%s.setI64(%s, @intCast(%s))", msg, f, src)
	}
}

func writeZigLoad(b *strings.Builder, f *model.File, field model.Field) {
	name := zigProp(field)
	proto := field.Name
	switch {
	case field.Message != "" && !field.Repeated:
		fmt.Fprintf(b, "        {\n")
		fmt.Fprintf(b, "            const f = try ty.field(%q);\n", proto)
		b.WriteString("            if (msg.getMessage(f)) |sub| {\n")
		fmt.Fprintf(b, "                const sty = try schema.messageType(%q);\n", zigFullName(f, field))
		fmt.Fprintf(b, "                out.%s = try %s.load(allocator, schema, &sub, sty);\n", name, zigType(f, field))
		b.WriteString("            }\n")
		b.WriteString("        }\n")
	case field.Message != "" && field.Repeated:
		fmt.Fprintf(b, "        {\n")
		fmt.Fprintf(b, "            const f = try ty.field(%q);\n", proto)
		b.WriteString("            if (msg.value(f)) |v| {\n")
		fmt.Fprintf(b, "                const sty = try schema.messageType(%q);\n", zigFullName(f, field))
		fmt.Fprintf(b, "                var xs = try allocator.alloc(%s, v.len());\n", zigType(f, field))
		b.WriteString("                for (0..v.len()) |i| {\n")
		b.WriteString("                    if (v.at(i)) |item| {\n")
		b.WriteString("                        if (item.asMessage()) |sub| {\n")
		fmt.Fprintf(b, "                            xs[i] = try %s.load(allocator, schema, &sub, sty);\n", zigType(f, field))
		b.WriteString("                        }\n")
		b.WriteString("                    }\n")
		b.WriteString("                }\n")
		fmt.Fprintf(b, "                out.%s = xs;\n", name)
		b.WriteString("            }\n")
		b.WriteString("        }\n")
	case field.Repeated:
		fmt.Fprintf(b, "        {\n")
		fmt.Fprintf(b, "            const f = try ty.field(%q);\n", proto)
		b.WriteString("            if (msg.value(f)) |v| {\n")
		fmt.Fprintf(b, "                var xs = try allocator.alloc(%s, v.len());\n", zigType(f, field))
		b.WriteString("                for (0..v.len()) |i| {\n")
		b.WriteString("                    if (v.at(i)) |item| {\n")
		fmt.Fprintf(b, "                        xs[i] = %s;\n", zigItemLoad(field, "item"))
		b.WriteString("                    }\n")
		b.WriteString("                }\n")
		fmt.Fprintf(b, "                out.%s = xs;\n", name)
		b.WriteString("            }\n")
		b.WriteString("        }\n")
	case field.NeedsHas:
		fmt.Fprintf(b, "        {\n")
		fmt.Fprintf(b, "            const f = try ty.field(%q);\n", proto)
		fmt.Fprintf(b, "            if (msg.has(f)) { out.%s = %s; }\n", name, zigGetOwned(field, "msg", "f"))
		b.WriteString("        }\n")
	default:
		fmt.Fprintf(b, "        {\n")
		fmt.Fprintf(b, "            const f = try ty.field(%q);\n", proto)
		fmt.Fprintf(b, "            out.%s = %s;\n", name, zigGetOwned(field, "msg", "f"))
		b.WriteString("        }\n")
	}
}

// zigGetOwned loads a singular field, dupe'ing strings/bytes into allocator
// memory (the wire view borrows the decoded tree, which the caller frees).
func zigGetOwned(field model.Field, msg, f string) string {
	switch field.Kind {
	case protoreflect.StringKind, protoreflect.BytesKind:
		return fmt.Sprintf("(if (%s.getStr(%s)) |s| try allocator.dupe(u8, s) else \"\")", msg, f)
	default:
		return zigGetScalar(field, msg, f)
	}
}

// zigI32 renders the i64 -> i32 narrowing. Wire values for 32-bit fields
// may arrive zero-extended (fixed32) or sign-extended (varint): truncate
// to 32 bits first, then reinterpret, so both survive.
func zigI32(src string) string {
	return fmt.Sprintf("@as(i32, @truncate(%s))", src)
}

// zigItemLoad renders one repeated-item load from a list Value.
func zigItemLoad(field model.Field, src string) string {
	switch field.Kind {
	case protoreflect.BoolKind:
		return fmt.Sprintf("%s.asBool()", src)
	case protoreflect.FloatKind:
		return fmt.Sprintf("%s.asF32()", src)
	case protoreflect.DoubleKind:
		return fmt.Sprintf("%s.asF64()", src)
	case protoreflect.StringKind, protoreflect.BytesKind:
		return fmt.Sprintf("(if (%s.asStr()) |s| try allocator.dupe(u8, s) else \"\")", src)
	case protoreflect.EnumKind:
		if field.Enum != "" {
			return fmt.Sprintf("@enumFromInt(%s.asI64())", src)
		}
		return fmt.Sprintf("@intCast(%s.asI64())", src)
	case protoreflect.Int32Kind, protoreflect.Sint32Kind, protoreflect.Sfixed32Kind:
		return zigI32(fmt.Sprintf("%s.asI64()", src))
	default:
		// Raw 64-bit bits; u64 wraps with a bit-cast.
		if field.Kind == protoreflect.Uint64Kind || field.Kind == protoreflect.Fixed64Kind {
			return fmt.Sprintf("@bitCast(%s.asI64())", src)
		}
		return fmt.Sprintf("@intCast(%s.asI64())", src)
	}
}

// zigFullName resolves the fully qualified proto name of a message field
// from the same file's message table (the neutral model only carries the
// local class name).
func zigFullName(f *model.File, field model.Field) string {
	for _, m := range f.Messages {
		if m.Name == field.Message {
			return m.Full
		}
	}
	// Foreign refs resolve by fully qualified proto name in the embedded
	// schema (the dep file carries the same shape).
	if field.Full != "" {
		return field.Full
	}
	return field.Message
}

func zigGetScalar(field model.Field, msg, f string) string {
	switch field.Kind {
	case protoreflect.BoolKind:
		return fmt.Sprintf("%s.getBool(%s)", msg, f)
	case protoreflect.FloatKind:
		return fmt.Sprintf("%s.getF32(%s)", msg, f)
	case protoreflect.DoubleKind:
		return fmt.Sprintf("%s.getF64(%s)", msg, f)
	case protoreflect.StringKind, protoreflect.BytesKind:
		return fmt.Sprintf("(%s.getStr(%s) orelse \"\")", msg, f)
	case protoreflect.EnumKind:
		if field.Enum != "" {
			return fmt.Sprintf("@enumFromInt(%s.getI64(%s))", msg, f)
		}
		return fmt.Sprintf("@intCast(%s.getI64(%s))", msg, f)
	case protoreflect.Int32Kind, protoreflect.Sint32Kind, protoreflect.Sfixed32Kind:
		return zigI32(fmt.Sprintf("%s.getI64(%s)", msg, f))
	default:
		// Raw 64-bit bits; u64 wraps with a bit-cast.
		if field.Kind == protoreflect.Uint64Kind || field.Kind == protoreflect.Fixed64Kind {
			return fmt.Sprintf("@bitCast(%s.getI64(%s))", msg, f)
		}
		return fmt.Sprintf("@intCast(%s.getI64(%s))", msg, f)
	}
}
